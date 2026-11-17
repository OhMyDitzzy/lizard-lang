#include "lzapi.h"

#include "interface_build.h"
#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/error.h"
#include "lizard/function.h"
#include "lizard/link.h"
#include "lizard/lexer.h"
#include "lizard/module.h"
#include "lizard/native.h"
#include "lizard/native_module.h"
#include "lizard/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LznLiteralReader lz_native_literals;

#define HANDLES_MAX 65536
#define PARAMS_MAX 32
#define ARGS_MAX 255
#define DATA_ALIGN 16

struct LznValue {
    LzValue v;
    uint64_t gen;
    bool owned;
};

struct LznRef {
    LzValue v;
    LzVM *vm;
    struct LznRef *prev;
    struct LznRef *next;
};

typedef struct InstanceHdr {
    struct InstanceHdr *prev;
    struct InstanceHdr *next;
    LzInstance *instance;
    uint64_t live;
} InstanceHdr;

typedef struct ClassInfo ClassInfo;
typedef struct ModuleRec ModuleRec;

typedef struct FnInfo {
    struct FnInfo *next;
    LznFn fn;
    char *shown;        /* "mod.fn" or "Class.method" for messages */
    ModuleRec *module;
    ClassInfo *cls;     /* the class for a method, a getter and an init, else NULL */
    int min_args;
    int max_args;
    bool is_method;
    char *names[PARAMS_MAX];
    int fixed_count;
    LzValue defaults[PARAMS_MAX];
} FnInfo;

struct ClassInfo {
    struct ClassInfo *next;
    LznClassDef def; /* methods and fields point into copies the module owns */
    LzClass *cls;
    FnInfo *init;
    ModuleRec *module;
};

struct ModuleRec {
    struct ModuleRec *next;
    char *name;
    LzModule *module;
    bool linked;
    bool built;
    FnInfo *fns;
    ClassInfo *classes;
    char **names; /* declared type names: classes and errors */
    size_t name_count;
    size_t name_capacity;
};

typedef struct LznState {
    struct LznValue *handles;
    size_t top;
    uint64_t gen;
    LznRef *refs;
    InstanceHdr *instances;
    ModuleRec *modules;
} LznState;

struct LznCtx {
    LzVM *vm;
    LznState *st;
    uint64_t gen;
    size_t base;
    int argc;
    const LzValue *argv;
    const FnInfo *fn;
    void *self_data;
    LzValue result;
    bool has_result;
    bool error_set;
    LzStatus status;
};

struct LznBuilder {
    LzVM *vm;
    ModuleRec *rec;
    LzModule *module;
    LzInterface *iface;
    LznSigCheck check;
    char error[256];
    bool failed;
    /* signatures to check once every name is declared */
    char **pending;
    size_t pending_count;
    size_t pending_capacity;
    char **pending_label;
};


static LznState *state_get(LzVM *vm) {
    if (vm->lzn == NULL) {
        vm->lzn = calloc(1, sizeof(LznState));
    }
    return vm->lzn;
}

static char *dup_text(const char *text) {
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, text, n);
    }
    return copy;
}


static LznStatus ctx_fail(LznCtx *ctx, LzStatus status) {
    ctx->error_set = true;
    ctx->status = status == LZ_OK ? LZ_ERR_RUNTIME : status;
    return LZN_FAIL;
}

static LznStatus ctx_error(LznCtx *ctx, LzStatus status, uint8_t kind, const char *format, ...)
    LZN_PRINTF(4, 5);
static LznStatus ctx_error(LznCtx *ctx, LzStatus status, uint8_t kind, const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof text, format, args);
    va_end(args);
    (void)lz_vm_set_error(ctx->vm, status, "%s", text);
    ctx->vm->error_kind = kind;
    return ctx_fail(ctx, status);
}

static bool handle_ok(LznCtx *ctx, const LznValue *h) {
    LznState *st = ctx->st;
    if (h == NULL) {
        return false;
    }
    uintptr_t at = (uintptr_t)h;
    uintptr_t lo = (uintptr_t)(st->handles + ctx->base);
    uintptr_t hi = (uintptr_t)(st->handles + st->top);
    if (at < lo || at >= hi || (at - (uintptr_t)st->handles) % sizeof(LznValue) != 0 || h->gen != ctx->gen) {
        return false;
    }
    return true;
}

static bool handle_bad(LznCtx *ctx, const LznValue *h) {
    if (handle_ok(ctx, h)) {
        return false;
    }
    (void)ctx_error(ctx, LZ_ERR_RUNTIME, LZ_ERROR_RUNTIME, "%s: stale or invalid native handle", ctx->fn->shown);
    return true;
}

static LznValue *handle_new(LznCtx *ctx, LzValue value, bool owned) {
    LznState *st = ctx->st;
    if (st->top >= HANDLES_MAX) {
        if (owned) {
            lz_value_release(&ctx->vm->heap, &value);
        }
        (void)ctx_error(ctx, LZ_ERR_RUNTIME, LZ_ERROR_RUNTIME, "%s: too many native handles in one call (limit %d)",
                        ctx->fn->shown, HANDLES_MAX);
        return NULL;
    }
    LznValue *h = &st->handles[st->top++];
    h->v = value;
    h->gen = ctx->gen;
    h->owned = owned;
    return h;
}

static void handles_release(LznCtx *ctx) {
    LznState *st = ctx->st;
    while (st->top > ctx->base) {
        LznValue *h = &st->handles[--st->top];
        if (h->owned) {
            lz_value_release(&ctx->vm->heap, &h->v);
        }
        h->gen = 0;
        h->owned = false;
        h->v = lz_value_nil();
    }
}


static LzStatus run_fn(LzVM *vm, const FnInfo *fn, int argc, const LzValue *argv, void *self_data, LzValue *result) {
    LzValue filled[ARGS_MAX];
    if (argc < fn->fixed_count) {
        for (int i = 0; i < fn->fixed_count; i++) filled[i] = i < argc ? argv[i] : fn->defaults[i];
        argv = filled;
        argc = fn->fixed_count;
    }
    LznState *st = state_get(vm);
    if (st == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    if (st->handles == NULL) {
        st->handles = calloc(HANDLES_MAX, sizeof(LznValue));
        if (st->handles == NULL) {
            return lz_vm_out_of_memory(vm);
        }
    }
    if (argc > ARGS_MAX || st->top + (size_t)argc + 1 > HANDLES_MAX) {
        return lz_vm_set_error(vm, LZ_ERR_STACK_OVERFLOW, "%s(): too many arguments or nested native calls", fn->shown);
    }
    LznCtx ctx = {.vm = vm, .st = st, .gen = ++st->gen, .base = st->top, .argc = argc, .argv = argv, .fn = fn,
                  .self_data = self_data, .result = lz_value_nil(), .has_result = false, .error_set = false,
                  .status = LZ_OK};
    for (int i = 0; i < argc; i++) {
        LznValue *h = &st->handles[st->top++];
        h->v = argv[i];
        h->gen = ctx.gen;
        h->owned = false;
    }
    LznStatus status = fn->fn(&ctx);
    LzStatus out = LZ_OK;
    if (status != LZN_OK) {
        out = ctx.error_set ? ctx.status : lz_vm_set_error(vm, LZ_ERR_RUNTIME, "%s(): failed without an error", fn->shown);
        if (ctx.has_result) {
            lz_value_release(&vm->heap, &ctx.result);
        }
    } else {
        *result = ctx.has_result ? ctx.result : lz_value_nil();
    }
    handles_release(&ctx);
    return out;
}

static LzStatus native_arity_error(LzVM *vm, const FnInfo *fn, int given) {
    if (fn->max_args < 0) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected at least %d argument%s, got %d",
                               fn->shown, fn->min_args, fn->min_args == 1 ? "" : "s", given);
    }
    if (fn->min_args == fn->max_args) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %d argument%s, got %d",
                               fn->shown, fn->min_args, fn->min_args == 1 ? "" : "s", given);
    }
    return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected %d to %d arguments, got %d",
                           fn->shown, fn->min_args, fn->max_args, given);
}

static LzStatus trampoline(LzVM *vm, int argc, const LzValue *argv, LzValue *result) {
    const FnInfo *fn = (const FnInfo *)vm->current_native->data;
    if (!fn->is_method) {
        return run_fn(vm, fn, argc, argv, NULL, result);
    }
    /* A method or getter takes self first, so the arity is checked here against what the caller wrote. */
    int written = argc - 1;
    if (written < fn->min_args || (fn->max_args >= 0 && written > fn->max_args)) {
        return native_arity_error(vm, fn, written);
    }
    const LzValue *self = &argv[0];
    if (!lz_value_is_instance(self) || lz_value_as_instance(self)->cls != fn->cls->cls) {
        const char *got = lz_value_is_instance(self) ? lz_class_display_name(lz_value_as_instance(self)->cls)
                                                       : lz_value_kind_name(self);
        (void)lz_vm_set_error(vm, LZ_ERR_TYPE, "%s(): expected %s, got %s", fn->shown, fn->cls->cls->name->data, got);
        vm->error_kind = LZ_ERROR_TYPE;
        return LZ_ERR_TYPE;
    }
    void *data = lz_native_instance_data(lz_value_as_instance(self));
    return run_fn(vm, fn, written, argv + 1, data, result);
}

bool lz_native_module_arity(LzVM *vm, const LzNative *native, int argc, LzStatus *status) {
    if (native->fn != trampoline) return false;
    const FnInfo *fn = native->data;
    *status = native_arity_error(vm, fn, argc - (fn->is_method ? 1 : 0));
    return true;
}


static InstanceHdr *hdr_of(const LzInstance *instance) {
    uintptr_t at = (uintptr_t)&instance->fields[instance->cls->field_count];
    at = (at + DATA_ALIGN - 1) & ~(uintptr_t)(DATA_ALIGN - 1);
    return (InstanceHdr *)at;
}

void *lz_native_instance_data(const LzInstance *instance) {
    return (char *)hdr_of(instance) + 2 * DATA_ALIGN;
}

bool lz_native_class_is(const LzClass *cls) {
    return cls->native != NULL;
}

size_t lz_native_instance_extra(const LzClass *cls) {
    const ClassInfo *info = cls->native;
    return info == NULL ? 0 : DATA_ALIGN + 2 * DATA_ALIGN + info->def.size;
}

void lz_native_instance_setup(LzInstance *instance) {
    const ClassInfo *info = instance->cls->native;
    memset(hdr_of(instance), 0, 2 * DATA_ALIGN + info->def.size);
    hdr_of(instance)->instance = instance;
}

static void instance_link(LzVM *vm, InstanceHdr *hdr) {
    LznState *st = vm->lzn;
    hdr->prev = NULL;
    hdr->next = st->instances;
    if (st->instances != NULL) {
        st->instances->prev = hdr;
    }
    st->instances = hdr;
    hdr->live = 1;
}

void lz_native_instance_finalize(LzInstance *instance) {
    InstanceHdr *hdr = hdr_of(instance);
    const ClassInfo *info = instance->cls->native;
    if (hdr->live == 0) {
        return;
    }
    LznState *st = instance->cls->vm->lzn;
    hdr->live = 0;
    if (hdr->prev != NULL) {
        hdr->prev->next = hdr->next;
    } else {
        st->instances = hdr->next;
    }
    if (hdr->next != NULL) {
        hdr->next->prev = hdr->prev;
    }
    hdr->prev = hdr->next = NULL;
    if (info->def.finalize != NULL) {
        info->def.finalize(lz_native_instance_data(instance));
    }
}

LzStatus lz_native_class_construct(LzVM *vm, LzClass *cls, size_t positional, const LzValue *argv,
                                   size_t named_count, LzValue *result) {
    ClassInfo *info = cls->native;
    if (named_count > 0) {
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): native classes do not take named arguments", cls->name->data);
    }
    int min = info->def.init != NULL ? info->init->min_args : 0;
    int max = info->def.init != NULL ? info->init->max_args : 0;
    if ((int)positional < min || (max >= 0 && (int)positional > max)) {
        if (info->init != NULL) return native_arity_error(vm, info->init, (int)positional);
        return lz_vm_set_error(vm, LZ_ERR_ARITY, "%s(): expected 0 arguments, got %zu", cls->name->data, positional);
    }
    if (state_get(vm) == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    LzInstance *instance = lz_instance_new_raw(vm, cls);
    if (instance == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    instance_link(vm, hdr_of(instance)); /* from here the finalizer runs, even when init fails */
    if (info->init != NULL) {
        LzValue ignored = lz_value_nil();
        LzStatus status = run_fn(vm, info->init, (int)positional, argv, lz_native_instance_data(instance), &ignored);
        if (status != LZ_OK) {
            lz_object_release(&vm->heap, &instance->base);
            return status;
        }
        lz_value_release(&vm->heap, &ignored);
    }
    *result = lz_value_object(&instance->base);
    return LZ_OK;
}


int lzn_arg_count(LznCtx *ctx) {
    return ctx->argc;
}

LznValue *lzn_arg(LznCtx *ctx, int index) {
    if (index < 0 || index >= ctx->argc) {
        return NULL;
    }
    return &ctx->st->handles[ctx->base + (size_t)index];
}

LznValue *lzn_nil(LznCtx *ctx) {
    return handle_new(ctx, lz_value_nil(), false);
}

LznValue *lzn_bool(LznCtx *ctx, bool value) {
    return handle_new(ctx, lz_value_bool(value), false);
}

LznValue *lzn_int(LznCtx *ctx, int64_t value) {
    return handle_new(ctx, lz_value_int(value), false);
}

LznValue *lzn_float(LznCtx *ctx, double value) {
    return handle_new(ctx, lz_value_float(value), false);
}

LznValue *lzn_string(LznCtx *ctx, const char *data, size_t length) {
    LzString *s = lz_string_new(&ctx->vm->heap, data, length);
    if (s == NULL) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(ctx->vm));
        return NULL;
    }
    return handle_new(ctx, lz_value_object(&s->base), true);
}

LznValue *lzn_array(LznCtx *ctx) {
    LzArray *a = lz_array_new(&ctx->vm->heap, 0);
    if (a == NULL) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(ctx->vm));
        return NULL;
    }
    return handle_new(ctx, lz_value_object(&a->base), true);
}

LznValue *lzn_dict(LznCtx *ctx) {
    LzDict *d = lz_dict_new(&ctx->vm->heap, 0);
    if (d == NULL) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(ctx->vm));
        return NULL;
    }
    return handle_new(ctx, lz_value_object(&d->base), true);
}

LznStatus lzn_return(LznCtx *ctx, LznValue *value) {
    if (handle_bad(ctx, value)) {
        return LZN_FAIL;
    }
    if (ctx->has_result) {
        lz_value_release(&ctx->vm->heap, &ctx->result);
    }
    ctx->result = value->v;
    lz_value_retain(&ctx->result);
    ctx->has_result = true;
    return LZN_OK;
}

static LznStatus return_value(LznCtx *ctx, LzValue value) {
    if (ctx->has_result) {
        lz_value_release(&ctx->vm->heap, &ctx->result);
    }
    ctx->result = value;
    ctx->has_result = true;
    return LZN_OK;
}

LznStatus lzn_return_nil(LznCtx *ctx) {
    return return_value(ctx, lz_value_nil());
}

LznStatus lzn_return_bool(LznCtx *ctx, bool value) {
    return return_value(ctx, lz_value_bool(value));
}

LznStatus lzn_return_int(LznCtx *ctx, int64_t value) {
    return return_value(ctx, lz_value_int(value));
}

LznStatus lzn_return_float(LznCtx *ctx, double value) {
    return return_value(ctx, lz_value_float(value));
}

LznStatus lzn_return_string(LznCtx *ctx, const char *data, size_t length) {
    LznValue *s = lzn_string(ctx, data, length);
    return s == NULL ? LZN_FAIL : lzn_return(ctx, s);
}

static LznKind kind_of(const LzValue *v) {
    switch (v->type) {
    case LZ_VAL_NIL: return LZN_NIL;
    case LZ_VAL_BOOL: return LZN_BOOL;
    case LZ_VAL_INT: return LZN_INT;
    case LZ_VAL_FLOAT: return LZN_FLOAT;
    case LZ_VAL_OBJECT: break;
    }
    switch (v->as.object->type) {
    case LZ_OBJ_STRING: return LZN_STRING;
    case LZ_OBJ_ARRAY: return LZN_ARRAY;
    case LZ_OBJ_DICT: return LZN_DICT;
    case LZ_OBJ_INSTANCE: return LZN_INSTANCE;
    case LZ_OBJ_LINK: return LZN_LINK;
    case LZ_OBJ_ERROR: return LZN_ERROR;
    case LZ_OBJ_COROUTINE: return LZN_COROUTINE;
    default: return LZN_FUNCTION;
    }
}

LznKind lzn_kind(LznCtx *ctx, LznValue *value) {
    return handle_bad(ctx, value) ? LZN_NIL : kind_of(&value->v);
}

const char *lzn_kind_name(LznKind kind) {
    static const char *const names[] = {"nil", "bool", "int", "float", "string", "array", "dict",
                                        "function", "instance", "link", "error", "coroutine"};
    return (unsigned)kind < sizeof names / sizeof names[0] ? names[kind] : "?";
}

LznStatus lzn_to_int(LznCtx *ctx, LznValue *value, int64_t *out) {
    if (!handle_ok(ctx, value) || value->v.type != LZ_VAL_INT) {
        return LZN_FAIL;
    }
    *out = value->v.as.integer;
    return LZN_OK;
}

LznStatus lzn_to_float(LznCtx *ctx, LznValue *value, double *out) {
    if (!handle_ok(ctx, value) || value->v.type != LZ_VAL_FLOAT) {
        return LZN_FAIL;
    }
    *out = value->v.as.number;
    return LZN_OK;
}

LznStatus lzn_to_bool(LznCtx *ctx, LznValue *value, bool *out) {
    if (!handle_ok(ctx, value) || value->v.type != LZ_VAL_BOOL) {
        return LZN_FAIL;
    }
    *out = value->v.as.boolean;
    return LZN_OK;
}

const char *lzn_string_data(LznCtx *ctx, LznValue *value, size_t *length) {
    if (!handle_ok(ctx, value) || kind_of(&value->v) != LZN_STRING) {
        return NULL;
    }
    const LzString *s = (const LzString *)value->v.as.object;
    if (length != NULL) {
        *length = s->length;
    }
    return s->data;
}


static LzArray *array_of(LznCtx *ctx, LznValue *h) {
    return handle_ok(ctx, h) && lz_value_is_array(&h->v) ? lz_value_as_array(&h->v) : NULL;
}

static LzDict *dict_of(LznCtx *ctx, LznValue *h) {
    return handle_ok(ctx, h) && lz_value_is_dict(&h->v) ? lz_value_as_dict(&h->v) : NULL;
}

size_t lzn_array_len(LznCtx *ctx, LznValue *array) {
    LzArray *a = array_of(ctx, array);
    return a == NULL ? 0 : a->count;
}

LznValue *lzn_array_get(LznCtx *ctx, LznValue *array, size_t index) {
    LzArray *a = array_of(ctx, array);
    if (a == NULL || index >= a->count) {
        return NULL;
    }
    LzValue v = a->items[index];
    lz_value_retain(&v);
    return handle_new(ctx, v, true);
}

static LznStatus from_status(LznCtx *ctx, LzStatus status) {
    return status == LZ_OK ? LZN_OK : ctx_fail(ctx, status);
}

LznStatus lzn_array_push(LznCtx *ctx, LznValue *array, LznValue *value) {
    LzArray *a = array_of(ctx, array);
    if (a == NULL || !handle_ok(ctx, value)) {
        return ctx_error(ctx, LZ_ERR_TYPE, LZ_ERROR_TYPE, "%s: expected an array handle", ctx->fn->shown);
    }
    return from_status(ctx, lz_array_push(ctx->vm, a, &value->v));
}

LznStatus lzn_array_set(LznCtx *ctx, LznValue *array, size_t index, LznValue *value) {
    LzArray *a = array_of(ctx, array);
    if (a == NULL || !handle_ok(ctx, value)) {
        return ctx_error(ctx, LZ_ERR_TYPE, LZ_ERROR_TYPE, "%s: expected an array handle", ctx->fn->shown);
    }
    if (index >= a->count) {
        return ctx_error(ctx, LZ_ERR_RUNTIME, LZ_ERROR_RANGE, "%s: array index %zu out of range", ctx->fn->shown, index);
    }
    LzValue at = lz_value_int((int64_t)index);
    return from_status(ctx, lz_array_set(ctx->vm, a, &at, &value->v));
}

size_t lzn_dict_len(LznCtx *ctx, LznValue *dict) {
    LzDict *d = dict_of(ctx, dict);
    return d == NULL ? 0 : d->count;
}

static LznValue *dict_lookup(LznCtx *ctx, LznValue *dict, const LzValue *key) {
    LzDict *d = dict_of(ctx, dict);
    if (d == NULL) {
        return NULL;
    }
    size_t at = lz_dict_index_of(d, key);
    if (at >= d->count) {
        return NULL;
    }
    LzValue v = d->entries[at].value;
    lz_value_retain(&v);
    return handle_new(ctx, v, true);
}

LznValue *lzn_dict_get(LznCtx *ctx, LznValue *dict, const char *key) {
    LzString *s = lz_vm_intern(ctx->vm, key, strlen(key));
    if (s == NULL) {
        return NULL;
    }
    LzValue k = lz_value_object(&s->base);
    return dict_lookup(ctx, dict, &k);
}

LznValue *lzn_dict_get_int(LznCtx *ctx, LznValue *dict, int64_t key) {
    LzValue k = lz_value_int(key);
    return dict_lookup(ctx, dict, &k);
}

static bool dict_has(LznCtx *ctx, LznValue *dict, const LzValue *key) {
    LzDict *d = dict_of(ctx, dict);
    return d != NULL && lz_dict_index_of(d, key) < d->count;
}

bool lzn_dict_has(LznCtx *ctx, LznValue *dict, const char *key) {
    LzString *s = lz_vm_intern(ctx->vm, key, strlen(key));
    if (s == NULL) {
        return false;
    }
    LzValue k = lz_value_object(&s->base);
    return dict_has(ctx, dict, &k);
}

bool lzn_dict_has_int(LznCtx *ctx, LznValue *dict, int64_t key) {
    LzValue k = lz_value_int(key);
    return dict_has(ctx, dict, &k);
}

static LznStatus dict_store(LznCtx *ctx, LznValue *dict, const LzValue *key, LznValue *value) {
    LzDict *d = dict_of(ctx, dict);
    if (d == NULL || !handle_ok(ctx, value)) {
        return ctx_error(ctx, LZ_ERR_TYPE, LZ_ERROR_TYPE, "%s: expected a dict handle", ctx->fn->shown);
    }
    return from_status(ctx, lz_dict_set(ctx->vm, d, key, &value->v));
}

LznStatus lzn_dict_set(LznCtx *ctx, LznValue *dict, const char *key, LznValue *value) {
    LzString *s = lz_vm_intern(ctx->vm, key, strlen(key));
    if (s == NULL) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(ctx->vm));
        return ctx_fail(ctx, LZ_ERR_MEMORY);
    }
    LzValue k = lz_value_object(&s->base);
    return dict_store(ctx, dict, &k, value);
}

LznStatus lzn_dict_set_int(LznCtx *ctx, LznValue *dict, int64_t key, LznValue *value) {
    LzValue k = lz_value_int(key);
    return dict_store(ctx, dict, &k, value);
}


LznRef *lzn_ref(LznCtx *ctx, LznValue *value) {
    if (handle_bad(ctx, value)) {
        return NULL;
    }
    LznRef *ref = malloc(sizeof *ref);
    if (ref == NULL) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(ctx->vm));
        return NULL;
    }
    ref->v = value->v;
    lz_value_retain(&ref->v);
    ref->vm = ctx->vm;
    ref->prev = NULL;
    ref->next = ctx->st->refs;
    if (ref->next != NULL) {
        ref->next->prev = ref;
    }
    ctx->st->refs = ref;
    return ref;
}

LznValue *lzn_deref(LznCtx *ctx, LznRef *ref) {
    if (ref == NULL) {
        return NULL;
    }
    LzValue v = ref->v;
    lz_value_retain(&v);
    return handle_new(ctx, v, true);
}

void lzn_unref(LznRef *ref) {
    if (ref == NULL) {
        return;
    }
    LznState *st = ref->vm->lzn;
    if (ref->prev != NULL) {
        ref->prev->next = ref->next;
    } else {
        st->refs = ref->next;
    }
    if (ref->next != NULL) {
        ref->next->prev = ref->prev;
    }
    LzValue v = ref->v;
    LzVM *vm = ref->vm;
    free(ref);
    lz_value_release(&vm->heap, &v);
}


LznStatus lzn_throw(LznCtx *ctx, LznErrId kind, const char *fmt, ...) {
    char text[256];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    uint8_t real = LZ_ERROR_RUNTIME;
    if (kind >= LZN_ERR_FIRST_MODULE && kind < LZN_ERR_FIRST_MODULE + LZ_ERROR_TYPES_MAX) {
        unsigned k = (unsigned)(kind - LZN_ERR_FIRST_MODULE);
        if (k < lz_vm_error_kind_count(ctx->vm) && lz_vm_error_kind_scope(ctx->vm, k) == ctx->fn->module->module) {
            real = (uint8_t)k;
        } else {
            (void)snprintf(text, sizeof text, "%s: lzn_throw with an error kind that this module did not declare",
                           ctx->fn->shown);
        }
    } else if (kind >= 0 && kind < LZ_ERROR_KIND_COUNT) {
        real = (uint8_t)kind;
    }
    LzStatus status = real == LZ_ERROR_ARITY ? LZ_ERR_ARITY : real == LZ_ERROR_IO ? LZ_ERR_IO : LZ_ERR_RUNTIME;
    return ctx_error(ctx, status, real, "%s", text);
}

static LznStatus expected(LznCtx *ctx, int index, const char *want) {
    const LzValue *value = index >= 0 && index < ctx->argc ? &ctx->st->handles[ctx->base + (size_t)index].v : NULL;
    const char *got = value != NULL ? lz_value_kind_name(value) : "nothing";
    char shown[128];
    if (value != NULL && (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_STRING)) {
        LzString *string = (LzString *)value->as.object;
        (void)snprintf(shown, sizeof shown, "\"%.*s\"", (int)string->length, string->data);
        got = shown;
    }
    int slot = index < ctx->fn->fixed_count ? index : ctx->fn->fixed_count;
    const char *name = slot >= 0 && slot < PARAMS_MAX ? ctx->fn->names[slot] : NULL;
    return ctx_error(ctx, LZ_ERR_TYPE, LZ_ERROR_TYPE, "argument '%s' of %s() expects %s, got %s",
                     name != NULL ? name : "argument", ctx->fn->shown, want, got);
}

static const LzValue *arg_value(LznCtx *ctx, int index) {
    return index >= 0 && index < ctx->argc ? &ctx->st->handles[ctx->base + (size_t)index].v : NULL;
}

LznStatus lzn_check_int(LznCtx *ctx, int index, int64_t *out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || v->type != LZ_VAL_INT) {
        return expected(ctx, index, "int");
    }
    *out = v->as.integer;
    return LZN_OK;
}

LznStatus lzn_check_float(LznCtx *ctx, int index, double *out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || (v->type != LZ_VAL_FLOAT && v->type != LZ_VAL_INT)) {
        return expected(ctx, index, "float");
    }
    *out = v->type == LZ_VAL_INT ? (double)v->as.integer : v->as.number;
    return LZN_OK;
}

LznStatus lzn_check_bool(LznCtx *ctx, int index, bool *out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || v->type != LZ_VAL_BOOL) {
        return expected(ctx, index, "bool");
    }
    *out = v->as.boolean;
    return LZN_OK;
}

LznStatus lzn_check_string(LznCtx *ctx, int index, const char **data, size_t *length) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || kind_of(v) != LZN_STRING) {
        return expected(ctx, index, "string");
    }
    const LzString *s = (const LzString *)v->as.object;
    *data = s->data;
    if (length != NULL) {
        *length = s->length;
    }
    return LZN_OK;
}

LznStatus lzn_check_array(LznCtx *ctx, int index, LznValue **out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || !lz_value_is_array(v)) {
        return expected(ctx, index, "array");
    }
    *out = lzn_arg(ctx, index);
    return LZN_OK;
}

LznStatus lzn_check_dict(LznCtx *ctx, int index, LznValue **out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || !lz_value_is_dict(v)) {
        return expected(ctx, index, "dict");
    }
    *out = lzn_arg(ctx, index);
    return LZN_OK;
}

LznStatus lzn_check_function(LznCtx *ctx, int index, LznValue **out) {
    const LzValue *v = arg_value(ctx, index);
    if (v == NULL || kind_of(v) != LZN_FUNCTION) {
        return expected(ctx, index, "function");
    }
    *out = lzn_arg(ctx, index);
    return LZN_OK;
}


LznStatus lzn_call(LznCtx *ctx, LznValue *fn, int nargs, LznValue *const *args, LznValue **result) {
    LzValue values[32];
    if (handle_bad(ctx, fn)) {
        return LZN_FAIL;
    }
    if (nargs < 0 || nargs > 32) {
        return ctx_error(ctx, LZ_ERR_RUNTIME, LZ_ERROR_RUNTIME, "%s: lzn_call takes at most 32 arguments", ctx->fn->shown);
    }
    for (int i = 0; i < nargs; i++) {
        if (handle_bad(ctx, args[i])) {
            return LZN_FAIL;
        }
        values[i] = args[i]->v;
    }
    LzValue out = lz_value_nil();
    LzStatus status = lz_vm_call_value(ctx->vm, &fn->v, (size_t)nargs, values, &out);
    if (status != LZ_OK) {
        return ctx_fail(ctx, status);
    }
    LznValue *h = handle_new(ctx, out, true);
    if (h == NULL) {
        return LZN_FAIL;
    }
    if (result != NULL) {
        *result = h;
    }
    return LZN_OK;
}

typedef struct BlockHdr {
    LzVM *vm;
    size_t size;
    uint64_t pad;
} BlockHdr;

void *lzn_alloc(LznCtx *ctx, size_t size) {
    return lzn_realloc(ctx, NULL, 0, size);
}

void *lzn_realloc(LznCtx *ctx, void *block, size_t old_size, size_t new_size) {
    LzVM *vm = ctx->vm;
    BlockHdr *old = block == NULL ? NULL : (BlockHdr *)block - 1;
    if (old != NULL && old->size != old_size) {
        (void)ctx_error(ctx, LZ_ERR_RUNTIME, LZ_ERROR_RUNTIME, "%s: lzn_realloc with a wrong old size", ctx->fn->shown);
        return NULL;
    }
    if (new_size > SIZE_MAX - sizeof(BlockHdr) || !lz_heap_charge(&vm->heap, new_size)) {
        (void)ctx_fail(ctx, lz_vm_out_of_memory(vm));
        return NULL;
    }
    BlockHdr *grown = realloc(old, sizeof(BlockHdr) + new_size);
    if (grown == NULL) {
        lz_heap_uncharge(&vm->heap, new_size);
        (void)ctx_fail(ctx, lz_vm_out_of_memory(vm));
        return NULL;
    }
    if (old != NULL) {
        lz_heap_uncharge(&vm->heap, old_size);
    }
    grown->vm = vm;
    grown->size = new_size;
    return grown + 1;
}

/* ctx may be NULL, so a finalizer can free its buffers. */
void lzn_free(LznCtx *ctx, void *block, size_t size) {
    (void)ctx;
    if (block == NULL) {
        return;
    }
    BlockHdr *hdr = (BlockHdr *)block - 1;
    if (hdr->size != size) {
        return;
    }
    lz_heap_uncharge(&hdr->vm->heap, hdr->size);
    free(hdr);
}

LznStatus lzn_spend(LznCtx *ctx, uint64_t units) {
    return from_status(ctx, lz_vm_charge_fuel(ctx->vm, units));
}

void *lzn_self_data(LznCtx *ctx) {
    return ctx->self_data;
}


LznStatus lzn_builder_fail(LznBuilder *b, const char *fmt, ...) {
    if (!b->failed) {
        va_list args;
        va_start(args, fmt);
        (void)vsnprintf(b->error, sizeof b->error, fmt, args);
        va_end(args);
        b->failed = true;
    }
    return LZN_FAIL;
}

const char *lzn_module_name(LznBuilder *b) {
    return b->rec->name;
}

static bool name_ok(const char *name) {
    if (name == NULL || !(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        bool ok = *p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool declared(const LznBuilder *b, const char *name) {
    for (size_t i = 0; i < b->rec->name_count; i++) {
        if (strcmp(b->rec->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Claims a name of the module for a function, constant, class or error. */
static bool claim(LznBuilder *b, const char *name) {
    if (b->failed) {
        return false;
    }
    if (!name_ok(name)) {
        (void)lzn_builder_fail(b, "'%s' is not a valid name in module '%s'", name != NULL ? name : "(null)", b->rec->name);
        return false;
    }
    if (declared(b, name)) {
        (void)lzn_builder_fail(b, "module '%s' declares '%s' twice", b->rec->name, name);
        return false;
    }
    ModuleRec *rec = b->rec;
    if (rec->name_count == rec->name_capacity) {
        size_t capacity = rec->name_capacity == 0 ? 16 : rec->name_capacity * 2;
        char **grown = realloc(rec->names, capacity * sizeof *grown);
        if (grown == NULL) {
            (void)lzn_builder_fail(b, "out of memory");
            return false;
        }
        rec->names = grown;
        rec->name_capacity = capacity;
    }
    char *copy = dup_text(name);
    if (copy == NULL) {
        (void)lzn_builder_fail(b, "out of memory");
        return false;
    }
    rec->names[rec->name_count++] = copy;
    return true;
}

static LznStatus set_global(LznBuilder *b, const char *name, LzValue value) {
    LzVM *vm = b->vm;
    LzString *key = lz_vm_intern(vm, name, strlen(name));
    if (key == NULL || !lz_table_set(&vm->heap, &b->module->globals, key, value)) {
        lz_value_release(&vm->heap, &value);
        return lzn_builder_fail(b, "out of memory");
    }
    lz_value_release(&vm->heap, &value); /* the table holds its own reference */
    return LZN_OK;
}

typedef struct Parsed {
    char *names[PARAMS_MAX];
    char *types[PARAMS_MAX];
    size_t count;
    char *defaults[PARAMS_MAX];
    bool rest[PARAMS_MAX];
    char *result;
} Parsed;

static void parsed_free(Parsed *p) {
    for (size_t i = 0; i < p->count; i++) {
        free(p->names[i]);
        free(p->types[i]);
        free(p->defaults[i]);
    }
    free(p->result);
    memset(p, 0, sizeof *p);
}

static char *trimmed(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    char *out = malloc((size_t)(end - start) + 1);
    if (out != NULL) {
        memcpy(out, start, (size_t)(end - start));
        out[end - start] = '\0';
    }
    return out;
}

static bool parsed_add(Parsed *p, const char *start, const char *end, char *err, size_t err_size) {
    if (p->count >= PARAMS_MAX) {
        (void)snprintf(err, err_size, "too many parameters (limit %d)", PARAMS_MAX);
        return false;
    }
    char *text = trimmed(start, end);
    if (text == NULL) {
        (void)snprintf(err, err_size, "out of memory");
        return false;
    }
    if (text[0] == '\0') {
        free(text);
        (void)snprintf(err, err_size, "empty parameter type");
        return false;
    }
    bool rest = text[0] == '*';
    if (p->count > 0 && p->rest[p->count - 1]) {
        free(text);
        (void)snprintf(err, err_size, rest ? "a signature can have only one rest parameter"
                                         : "the rest parameter must be the last one");
        return false;
    }
    if (rest) memmove(text, text + 1, strlen(text));
    char *equal = strchr(text, '=');
    char *fallback = NULL;
    if (equal != NULL) {
        *equal = '\0';
        fallback = trimmed(equal + 1, equal + 1 + strlen(equal + 1));
        if (fallback == NULL) {
            free(text);
            (void)snprintf(err, err_size, "out of memory");
            return false;
        }
        LzSource source;
        lz_source_from_string(&source, "<native signature>", fallback);
        LzLexer lexer;
        lz_native_literals.init(&lexer, &source);
        LzToken token = lz_native_literals.next(&lexer);
        bool signed_literal = token.type == LZ_TOKEN_MINUS || token.type == LZ_TOKEN_PLUS;
        if (signed_literal) token = lz_native_literals.next(&lexer);
        bool literal = token.type == LZ_TOKEN_INTEGER || token.type == LZ_TOKEN_FLOAT ||
                       token.type == LZ_TOKEN_NIL || token.type == LZ_TOKEN_TRUE || token.type == LZ_TOKEN_FALSE ||
                       (token.type == LZ_TOKEN_STRING && !token.value.string.is_template);
        if (signed_literal && token.type != LZ_TOKEN_INTEGER && token.type != LZ_TOKEN_FLOAT) literal = false;
        LzToken tail = lz_native_literals.next(&lexer);
        if (tail.type == LZ_TOKEN_NEWLINE) tail = lz_native_literals.next(&lexer);
        if (rest || !literal || tail.type != LZ_TOKEN_EOF) {
            free(fallback);
            free(text);
            (void)snprintf(err, err_size, rest ? "a rest parameter cannot have a default" : "a parameter default must be a literal");
            return false;
        }
    } else if (!rest && p->count > 0 && p->defaults[p->count - 1] != NULL) {
        free(text);
        (void)snprintf(err, err_size, "a parameter without a default cannot follow one with a default");
        return false;
    }
    const char *p0 = text;
    while (*p0 == '_' || (*p0 >= 'a' && *p0 <= 'z') || (*p0 >= 'A' && *p0 <= 'Z') || (*p0 >= '0' && *p0 <= '9')) {
        p0++;
    }
    char name[16];
    (void)snprintf(name, sizeof name, "arg%zu", p->count + 1);
    const char *type = text;
    char *owned_name = NULL;
    const char *name_end = p0;
    while (*p0 == ' ' || *p0 == '\t') p0++;
    if (name_end != text && *p0 == ':') {
        owned_name = malloc((size_t)(name_end - text) + 1);
        if (owned_name != NULL) {
            memcpy(owned_name, text, (size_t)(name_end - text));
            owned_name[name_end - text] = '\0';
        }
        type = p0 + 1;
        while (*type == ' ' || *type == '\t') {
            type++;
        }
    }
    p->names[p->count] = owned_name != NULL ? owned_name : dup_text(name);
    p->types[p->count] = dup_text(type);
    free(text);
    if (p->names[p->count] == NULL || p->types[p->count] == NULL || p->types[p->count][0] == '\0') {
        free(fallback);
        free(p->names[p->count]);
        free(p->types[p->count]);
        (void)snprintf(err, err_size, "empty parameter type");
        return false;
    }
    for (size_t i = 0; i < p->count; i++) {
        if (strcmp(p->names[i], p->names[p->count]) == 0) {
            (void)snprintf(err, err_size, "duplicate parameter name '%s'", p->names[p->count]);
            free(fallback);
            free(p->names[p->count]);
            free(p->types[p->count]);
            return false;
        }
    }
    p->defaults[p->count] = fallback;
    p->rest[p->count] = rest;
    p->count++;
    return true;
}

/* "fn(int, x: string) -> int?" into its parts. */
static bool sig_parse(const char *text, Parsed *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    if (text == NULL) {
        (void)snprintf(err, err_size, "missing signature");
        return false;
    }
    const char *p = text;
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "fn", 2) != 0) {
        (void)snprintf(err, err_size, "a signature starts with 'fn(', like \"fn(int) -> int\"");
        return false;
    }
    p += 2;
    while (*p == ' ') {
        p++;
    }
    if (*p != '(') {
        (void)snprintf(err, err_size, "expected '(' after 'fn'");
        return false;
    }
    p++;
    const char *start = p;
    int depth = 0;
    for (;; p++) {
        if (*p == '\'' || *p == '"' || (*p == 'r' && (p[1] == '\'' || p[1] == '"'))) {
            LzSource source;
            lz_source_from_string(&source, "<native signature>", p);
            LzLexer lexer;
            lz_native_literals.init(&lexer, &source);
            LzToken token = lz_native_literals.next(&lexer);
            if (token.type == LZ_TOKEN_STRING) {
                p += token.length - 1;
                continue;
            }
        }
        if (*p == '\0') {
            (void)snprintf(err, err_size, "missing ')'");
            parsed_free(out);
            return false;
        }
        if (*p == '(' || *p == '[' || *p == '{') {
            depth++;
        } else if ((*p == ')' || *p == ']' || *p == '}') && depth > 0) {
            depth--;
        } else if (*p == ',' && depth == 0) {
            if (!parsed_add(out, start, p, err, err_size)) {
                parsed_free(out);
                return false;
            }
            start = p + 1;
        } else if (*p == ')' && depth == 0) {
            break;
        }
    }
    char *last = trimmed(start, p);
    bool empty = last != NULL && last[0] == '\0';
    free(last);
    if (!empty || out->count > 0) {
        if (!parsed_add(out, start, p, err, err_size)) {
            parsed_free(out);
            return false;
        }
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return true;
    }
    if (strncmp(p, "->", 2) != 0) {
        (void)snprintf(err, err_size, "expected '->' after the parameters");
        parsed_free(out);
        return false;
    }
    p += 2;
    out->result = trimmed(p, p + strlen(p));
    if (out->result == NULL || out->result[0] == '\0') {
        (void)snprintf(err, err_size, "expected a result type after '->'");
        parsed_free(out);
        return false;
    }
    return true;
}

/*
 * Reads a signature and returns the interface record.
 * The type texts are checked later, once every name of the module is known.
 */
static LzIfaceSig *sig_make(LznBuilder *b, const char *label, const char *text, const char *override_result) {
    Parsed parsed;
    char err[160];
    if (!sig_parse(text, &parsed, err, sizeof err)) {
        (void)lzn_builder_fail(b, "invalid signature for '%s' in module '%s': %s", label, b->rec->name, err);
        return NULL;
    }
    LzIfaceSig *sig = lz_iface_sig_new();
    bool ok = sig != NULL;
    for (size_t i = 0; ok && i < parsed.count; i++) {
        ok = lz_iface_sig_add_param(sig, parsed.names[i], parsed.types[i], parsed.defaults[i] != NULL, parsed.rest[i]);
    }
    const char *result = override_result != NULL ? override_result : parsed.result != NULL ? parsed.result : "nil";
    ok = ok && lz_iface_sig_set_result(sig, result, true);
    if (ok) {
        /* queue every type text for the check */
        size_t need = b->pending_count + parsed.count + 1;
        if (need > b->pending_capacity) {
            size_t capacity = need * 2;
            char **grown = realloc(b->pending, capacity * sizeof *grown);
            char **labels = grown != NULL ? realloc(b->pending_label, capacity * sizeof *labels) : NULL;
            if (grown != NULL) {
                b->pending = grown;
            }
            if (labels != NULL) {
                b->pending_label = labels;
            }
            ok = grown != NULL && labels != NULL;
            if (ok) {
                b->pending_capacity = capacity;
            }
        }
        for (size_t i = 0; ok && i <= parsed.count; i++) {
            const char *type = i < parsed.count ? parsed.types[i] : result;
            b->pending[b->pending_count] = dup_text(type);
            b->pending_label[b->pending_count] = dup_text(label);
            ok = b->pending[b->pending_count] != NULL && b->pending_label[b->pending_count] != NULL;
            b->pending_count++;
        }
    }
    parsed_free(&parsed);
    if (!ok) {
        lz_iface_sig_free(sig);
        (void)lzn_builder_fail(b, "out of memory");
        return NULL;
    }
    return sig;
}

static FnInfo *fn_make(LznBuilder *b, const char *shown, LznFn fn, ClassInfo *cls, bool is_method, const char *signature) {
    FnInfo *info = calloc(1, sizeof *info);
    if (info == NULL) {
        (void)lzn_builder_fail(b, "out of memory");
        return NULL;
    }
    info->fn = fn;
    info->shown = dup_text(shown);
    info->module = b->rec;
    info->cls = cls;
    info->is_method = is_method;
    Parsed parsed;
    char err[160];
    if (!sig_parse(signature, &parsed, err, sizeof err)) {
        free(info->shown);
        free(info);
        (void)lzn_builder_fail(b, "%s", err);
        return NULL;
    }
    info->max_args = (int)parsed.count;
    info->fixed_count = (int)parsed.count;
    for (size_t i = 0; i < parsed.count; i++) {
        info->names[i] = dup_text(parsed.names[i]);
        if (parsed.rest[i]) {
            info->max_args = -1;
            info->fixed_count--;
        } else if (parsed.defaults[i] == NULL) {
            info->min_args++;
        } else {
            LzSource source;
            lz_source_from_string(&source, "<native signature>", parsed.defaults[i]);
            LzLexer lexer;
            lz_native_literals.init(&lexer, &source);
            LzToken token = lz_native_literals.next(&lexer);
            bool negative = token.type == LZ_TOKEN_MINUS;
            if (negative || token.type == LZ_TOKEN_PLUS) token = lz_native_literals.next(&lexer);
            LzValue value = lz_value_nil();
            if (token.type == LZ_TOKEN_INTEGER) value = lz_value_int(negative ? (int64_t)(0 - (uint64_t)token.value.integer) : token.value.integer);
            if (token.type == LZ_TOKEN_FLOAT) value = lz_value_float(negative ? -token.value.number : token.value.number);
            if (token.type == LZ_TOKEN_TRUE || token.type == LZ_TOKEN_FALSE) value = lz_value_bool(token.type == LZ_TOKEN_TRUE);
            if (token.type == LZ_TOKEN_STRING) {
                LzBuffer buffer = {0};
                bool decoded = lz_native_literals.string(&source, &token, &buffer);
                LzString *string = decoded ? lz_vm_intern(b->vm, buffer.data != NULL ? buffer.data : "", buffer.length) : NULL;
                lz_buffer_free(&buffer);
                if (string == NULL) {
                    parsed_free(&parsed);
                    for (size_t j = 0; j <= i; j++) {
                        free(info->names[j]);
                        lz_value_release(&b->vm->heap, &info->defaults[j]);
                    }
                    free(info->shown);
                    free(info);
                    (void)lzn_builder_fail(b, "out of memory");
                    return NULL;
                }
                value = lz_value_object(&string->base);
                lz_value_retain(&value);
            }
            info->defaults[i] = value;
        }
    }
    parsed_free(&parsed);
    info->next = b->rec->fns;
    b->rec->fns = info;
    if (info->shown == NULL) {
        (void)lzn_builder_fail(b, "out of memory");
        return NULL;
    }
    return info;
}

static LzNative *native_make(LznBuilder *b, FnInfo *info, int min_args, int max_args) {
    LzVM *vm = b->vm;
    LzString *name = lz_vm_intern(vm, info->shown, strlen(info->shown));
    LzNative *native = name == NULL ? NULL : lz_native_new(&vm->heap, name, trampoline, min_args, max_args);
    if (native == NULL) {
        (void)lzn_builder_fail(b, "out of memory");
        return NULL;
    }
    native->data = info;
    return native;
}

LznStatus lzn_module_fn(LznBuilder *b, const char *name, LznFn fn, const char *signature) {
    if (!claim(b, name)) {
        return LZN_FAIL;
    }
    if (fn == NULL) {
        return lzn_builder_fail(b, "function '%s' of module '%s' has no implementation", name, b->rec->name);
    }
    LzIfaceSig *sig = sig_make(b, name, signature, NULL);
    if (sig == NULL) {
        return LZN_FAIL;
    }
    char shown[160];
    (void)snprintf(shown, sizeof shown, "%s.%s", b->rec->name, name);
    FnInfo *info = fn_make(b, shown, fn, NULL, false, signature);
    LzNative *native = info != NULL ? native_make(b, info, info->min_args, info->max_args < 0 ? LZ_VARIADIC : info->max_args) : NULL;
    LzIfaceEntry *entry = native != NULL ? lz_iface_add_entry(b->iface, LZ_IFACE_FN, name) : NULL;
    if (entry == NULL) {
        if (native != NULL) {
            lz_object_release(&b->vm->heap, &native->base);
        }
        lz_iface_sig_free(sig);
        return lzn_builder_fail(b, "out of memory");
    }
    lz_iface_entry_set_sig(entry, sig);
    return set_global(b, name, lz_value_object(&native->base));
}

static LznStatus add_const(LznBuilder *b, const char *name, const char *type, LzValue value) {
    if (!claim(b, name)) {
        lz_value_release(&b->vm->heap, &value);
        return LZN_FAIL;
    }
    LzIfaceEntry *entry = lz_iface_add_entry(b->iface, LZ_IFACE_CONST, name);
    if (entry == NULL || !lz_iface_entry_set_type(entry, type)) {
        lz_value_release(&b->vm->heap, &value);
        return lzn_builder_fail(b, "out of memory");
    }
    return set_global(b, name, value);
}

LznStatus lzn_module_const_int(LznBuilder *b, const char *name, int64_t value) {
    return add_const(b, name, "int", lz_value_int(value));
}

LznStatus lzn_module_const_float(LznBuilder *b, const char *name, double value) {
    return add_const(b, name, "float", lz_value_float(value));
}

LznStatus lzn_module_const_bool(LznBuilder *b, const char *name, bool value) {
    return add_const(b, name, "bool", lz_value_bool(value));
}

LznStatus lzn_module_const_string(LznBuilder *b, const char *name, const char *value) {
    LzString *s = lz_string_new(&b->vm->heap, value, strlen(value));
    if (s == NULL) {
        return lzn_builder_fail(b, "out of memory");
    }
    return add_const(b, name, "string", lz_value_object(&s->base));
}

LznErrId lzn_module_error(LznBuilder *b, const char *name, LznErrId parent) {
    if (!claim(b, name)) {
        return -1;
    }
    uint8_t parent_kind;
    if (parent >= 0 && parent < LZ_ERROR_KIND_COUNT) {
        parent_kind = (uint8_t)parent;
    } else if (parent >= LZN_ERR_FIRST_MODULE && parent < LZN_ERR_FIRST_MODULE + LZ_ERROR_TYPES_MAX &&
               (unsigned)(parent - LZN_ERR_FIRST_MODULE) < lz_vm_error_kind_count(b->vm) &&
               lz_vm_error_kind_scope(b->vm, (unsigned)(parent - LZN_ERR_FIRST_MODULE)) == b->module) {
        parent_kind = (uint8_t)(parent - LZN_ERR_FIRST_MODULE);
    } else {
        (void)lzn_builder_fail(b, "error '%s' of module '%s' has a parent kind that is not a built-in one or its own",
                               name, b->rec->name);
        return -1;
    }
    LzString *interned = lz_vm_intern(b->vm, name, strlen(name));
    uint8_t kind = 0;
    if (interned == NULL || lz_vm_error_declare(b->vm, b->module, interned, parent_kind, &kind) != LZ_OK) {
        (void)lzn_builder_fail(b, "cannot declare error '%s' in module '%s': %s", name, b->rec->name,
                               interned == NULL ? "out of memory" : b->vm->error);
        return -1;
    }
    LzIfaceEntry *entry = lz_iface_add_entry(b->iface, LZ_IFACE_ERROR, name);
    const char *parent_name = lz_vm_error_kind_name(b->vm, parent_kind);
    if (entry == NULL || !lz_iface_entry_set_parent(entry, parent_name)) {
        (void)lzn_builder_fail(b, "out of memory");
        return -1;
    }
    lz_iface_entry_set_class_id(entry, (uint64_t)kind + 1);
    return LZN_ERR_FIRST_MODULE + kind;
}

LznStatus lzn_module_class(LznBuilder *b, const char *name, const LznClassDef *def) {
    if (!claim(b, name)) {
        return LZN_FAIL;
    }
    LzVM *vm = b->vm;
    ClassInfo *info = calloc(1, sizeof *info);
    if (info == NULL) {
        return lzn_builder_fail(b, "out of memory");
    }
    info->def = *def;
    info->module = b->rec;
    info->next = b->rec->classes;
    b->rec->classes = info;
    if (def->size > (size_t)1 << 24) {
        return lzn_builder_fail(b, "class '%s' of module '%s' asks for too much instance data", name, b->rec->name);
    }
    LzString *interned = lz_vm_intern(vm, name, strlen(name));
    LzClass *cls = interned == NULL ? NULL : lz_class_new(vm, interned, 0);
    if (cls == NULL) {
        return lzn_builder_fail(b, "out of memory");
    }
    info->cls = cls;
    cls->native = info;
    cls->module = b->module;
    vm->has_classes = true;

    LzIfaceEntry *entry = lz_iface_add_entry(b->iface, LZ_IFACE_CLASS, name);
    if (entry == NULL) {
        lz_object_release(&vm->heap, &cls->base);
        return lzn_builder_fail(b, "out of memory");
    }
    lz_iface_entry_set_class_id(entry, cls->id);

    char shown[160];
    /* the constructor */
    LzIfaceSig *init_sig = sig_make(b, name, def->init_signature != NULL ? def->init_signature : "fn()",
                                    "nil");
    if (init_sig == NULL) {
        lz_object_release(&vm->heap, &cls->base);
        return LZN_FAIL;
    }
    LzIfaceMember *init_member = lz_iface_add_member(entry, LZ_IFACE_MEMBER_INIT, "init");
    if (init_member == NULL) {
        lz_iface_sig_free(init_sig);
        lz_object_release(&vm->heap, &cls->base);
        return lzn_builder_fail(b, "out of memory");
    }
    lz_iface_member_set_sig(init_member, init_sig);
    if (def->init != NULL) {
        (void)snprintf(shown, sizeof shown, "%s.init", name);
        info->init = fn_make(b, shown, def->init, info, true, def->init_signature != NULL ? def->init_signature : "fn()");
        if (info->init == NULL) {
            lz_object_release(&vm->heap, &cls->base);
            return LZN_FAIL;
        }
    }
    for (const LznMethodDef *m = def->methods; m != NULL && m->name != NULL; m++) {
        LzString *mname = lz_vm_intern(vm, m->name, strlen(m->name));
        LzMember *member = NULL;
        bool clash = false;
        for (size_t i = 0; mname != NULL && i < cls->member_count; i++) {
            clash = clash || cls->members[i].name == mname;
        }
        if (!name_ok(m->name) || m->fn == NULL || clash) {
            lz_object_release(&vm->heap, &cls->base);
            return lzn_builder_fail(b, "class '%s' of module '%s' has a bad or repeated member '%s'", name, b->rec->name,
                                    m->name != NULL ? m->name : "(null)");
        }
        LzIfaceSig *sig = sig_make(b, m->name, m->signature, NULL);
        (void)snprintf(shown, sizeof shown, "%s.%s", name, m->name);
        FnInfo *minfo = sig != NULL ? fn_make(b, shown, m->fn, info, true, m->signature) : NULL;
        LzNative *native = minfo != NULL ? native_make(b, minfo, 1, LZ_VARIADIC) : NULL;
        LzIfaceMember *im = native != NULL ? lz_iface_add_member(entry, LZ_IFACE_MEMBER_METHOD, m->name) : NULL;
        if (im == NULL || mname == NULL || !lz_class_add_member(cls, mname, LZ_MEMBER_METHOD, LZ_VIS_PUB, 0, &member)) {
            if (native != NULL) {
                lz_object_release(&vm->heap, &native->base);
            }
            lz_iface_sig_free(sig);
            lz_object_release(&vm->heap, &cls->base);
            return b->failed ? LZN_FAIL : lzn_builder_fail(b, "out of memory");
        }
        lz_iface_member_set_sig(im, sig);
        member->value = lz_value_object(&native->base); /* the member owns the creation reference */
    }
    for (const LznFieldDef *f = def->fields; f != NULL && f->name != NULL; f++) {
        LzString *fname = lz_vm_intern(vm, f->name, strlen(f->name));
        LzMember *member = NULL;
        bool clash = false;
        for (size_t i = 0; fname != NULL && i < cls->member_count; i++) {
            clash = clash || cls->members[i].name == fname;
        }
        if (!name_ok(f->name) || f->get == NULL || f->type == NULL || clash) {
            lz_object_release(&vm->heap, &cls->base);
            return lzn_builder_fail(b, "class '%s' of module '%s' has a bad or repeated field '%s'", name,
                                    b->rec->name, f->name != NULL ? f->name : "(null)");
        }
        char signature[200];
        (void)snprintf(signature, sizeof signature, "fn() -> %s", f->type);
        LzIfaceSig *sig = sig_make(b, f->name, signature, NULL);
        (void)snprintf(shown, sizeof shown, "%s.%s", name, f->name);
        FnInfo *finfo = sig != NULL ? fn_make(b, shown, f->get, info, true, signature) : NULL;
        LzNative *native = finfo != NULL ? native_make(b, finfo, 1, LZ_VARIADIC) : NULL;
        LzIfaceMember *im = native != NULL ? lz_iface_add_member(entry, LZ_IFACE_MEMBER_GETTER, f->name) : NULL;
        if (im == NULL || fname == NULL || !lz_class_add_member(cls, fname, LZ_MEMBER_GETTER, LZ_VIS_PUB, 0, &member)) {
            if (native != NULL) {
                lz_object_release(&vm->heap, &native->base);
            }
            lz_iface_sig_free(sig);
            lz_object_release(&vm->heap, &cls->base);
            return b->failed ? LZN_FAIL : lzn_builder_fail(b, "out of memory");
        }
        lz_iface_member_set_sig(im, sig);
        member->value = lz_value_object(&native->base);
    }
    lz_class_seal(cls);
    cls->linked = true;
    return set_global(b, name, lz_value_object(&cls->base));
}


static ModuleRec *rec_find(LznState *st, const char *name) {
    for (ModuleRec *rec = st->modules; rec != NULL; rec = rec->next) {
        if (rec->module != NULL && strcmp(rec->name, name) == 0) {
            return rec;
        }
    }
    return NULL;
}

static void builder_cleanup(LznBuilder *b) {
    for (size_t i = 0; i < b->pending_count; i++) {
        free(b->pending[i]);
        free(b->pending_label[i]);
    }
    free(b->pending);
    free(b->pending_label);
}

static bool module_name_ok(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    bool start = true;
    for (const char *p = name; *p != '\0'; p++) {
        bool alpha = *p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z');
        bool digit = *p >= '0' && *p <= '9';
        if (*p == '.') {
            if (start) {
                return false;
            }
            start = true;
        } else if (alpha || (digit && !start)) {
            start = false;
        } else {
            return false;
        }
    }
    return !start;
}

static void module_link(LzVM *vm, ModuleRec *rec);

LzStatus lz_native_module_build(LzVM *vm, const char *name, LznInitFn init, LznSigCheck check) {
    if (!module_name_ok(name) || init == NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot register native module '%s': the name or the init function is not valid",
                               name != NULL ? name : "(null)");
    }
    LznState *st = state_get(vm);
    if (st == NULL) {
        return lz_vm_out_of_memory(vm);
    }
    if (rec_find(st, name) != NULL) {
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "native module '%s' is registered twice", name);
    }
    ModuleRec *rec = calloc(1, sizeof *rec);
    LzModule *module = rec != NULL ? calloc(1, sizeof *module) : NULL;
    LzChunk *chunk = module != NULL ? calloc(1, sizeof *chunk) : NULL;
    char display[200];
    (void)snprintf(display, sizeof display, "<native %s>", name);
    char id[220];
    (void)snprintf(id, sizeof id, "native:%s", name);
    if (chunk == NULL) {
        free(module);
        free(rec);
        return lz_vm_out_of_memory(vm);
    }
    rec->name = dup_text(name);
    module->id = dup_text(id);
    module->source.name = dup_text(display);
    module->source.text = dup_text("");
    module->source.length = 0;
    module->source.owned = true;
    if (rec->name == NULL || module->id == NULL || module->source.name == NULL || module->source.text == NULL) {
        free(rec->name);
        lz_module_discard(module);
        free(rec);
        return lz_vm_out_of_memory(vm);
    }
    lz_table_init(&module->globals);
    module->state = LZ_MODULE_COMPILED;
    module->is_native = true;
    module->chunk = chunk;
    lz_chunk_init(chunk);
    chunk->module = module;
    chunk->iface = lz_iface_new();
    rec->module = module;
    rec->next = st->modules;
    st->modules = rec;
    LznBuilder b = {.vm = vm, .rec = rec, .module = module, .iface = chunk->iface, .check = check};
    if (chunk->iface == NULL) {
        (void)lzn_builder_fail(&b, "out of memory");
    } else if (init(&b) != LZN_OK && !b.failed) {
        (void)lzn_builder_fail(&b, "init of module '%s' failed", name);
    }
    if (!b.failed && b.pending_count > 0 && check != NULL) {
        for (size_t i = 0; i < b.pending_count && !b.failed; i++) {
            char err[200];
            if (!check(vm, (const char *const *)rec->names, rec->name_count, (const char *const *)&b.pending[i], 1, err,
                       sizeof err)) {
                (void)lzn_builder_fail(&b, "invalid signature for '%s' in module '%s': %s", b.pending_label[i], name, err);
            }
        }
    }
    builder_cleanup(&b);
    rec->built = !b.failed;
    if (b.failed) {
        char message[256];
        (void)snprintf(message, sizeof message, "%s", b.error);
        /* what init made stays with the module record until the VM ends, so nothing dangles */
        st->modules = rec->next;
        rec->next = NULL;
        lz_vm_error_scope_drop(vm, module);
        lz_chunk_free(&vm->heap, chunk);
        lz_table_free(&vm->heap, &module->globals);
        for (ClassInfo *c = rec->classes; c != NULL; c = c->next) {
            c->cls = NULL;
        }
        /* keep the record alive for the FnInfo pointers held by natives that died with the heap */
        rec->module = NULL;
        rec->next = st->modules;
        st->modules = rec;
        lz_module_discard(module);
        return lz_vm_set_error(vm, LZ_ERR_RUNTIME, "cannot register native module '%s': %s", name, message);
    }
    module_link(vm, rec);
    return LZ_OK;
}

static void module_link(LzVM *vm, ModuleRec *rec) {
    LzModule **tail = &vm->modules;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    rec->module->next = NULL;
    *tail = rec->module;
    vm->module_count++;
    rec->linked = true;
}

LzModule *lz_native_module_find(LzVM *vm, const char *name, bool *known) {
    *known = false;
    LznState *st = vm->lzn;
    ModuleRec *rec = st != NULL ? rec_find(st, name) : NULL;
    if (rec == NULL || rec->module == NULL || !rec->built) {
        return NULL;
    }
    *known = true;
    return rec->module;
}


void lz_native_vm_shutdown(LzVM *vm) {
    LznState *st = vm->lzn;
    if (st == NULL) {
        return;
    }
    /* Finalizers first, once each, then the references the modules still hold. */
    while (st->instances != NULL) {
        lz_native_instance_finalize(st->instances->instance);
    }
    while (st->refs != NULL) {
        lzn_unref(st->refs);
        while (st->instances != NULL) {
            lz_native_instance_finalize(st->instances->instance);
        }
    }
    for (ModuleRec *rec = st->modules; rec != NULL; rec = rec->next) {
        for (FnInfo *fn = rec->fns; fn != NULL; fn = fn->next) {
            for (int i = 0; i < fn->fixed_count; i++) {
                lz_value_release(&vm->heap, &fn->defaults[i]);
                fn->defaults[i] = lz_value_nil();
            }
        }
        if (rec->module != NULL && !rec->linked) {
            if (rec->module->chunk != NULL) {
                lz_chunk_free(&vm->heap, rec->module->chunk);
                lz_chunk_init(rec->module->chunk);
            }
            lz_table_free(&vm->heap, &rec->module->globals);
        }
    }
}

void lz_native_vm_free(LzVM *vm) {
    LznState *st = vm->lzn;
    if (st == NULL) {
        return;
    }
    while (st->modules != NULL) {
        ModuleRec *rec = st->modules;
        st->modules = rec->next;
        if (rec->module != NULL && !rec->linked) {
            lz_module_discard(rec->module);
        }
        while (rec->fns != NULL) {
            FnInfo *fn = rec->fns;
            rec->fns = fn->next;
            for (int i = 0; i < PARAMS_MAX; i++) free(fn->names[i]);
            free(fn->shown);
            free(fn);
        }
        while (rec->classes != NULL) {
            ClassInfo *c = rec->classes;
            rec->classes = c->next;
            free(c);
        }
        for (size_t i = 0; i < rec->name_count; i++) {
            free(rec->names[i]);
        }
        free(rec->names);
        free(rec->name);
        free(rec);
    }
    free(st->handles);
    free(st);
    vm->lzn = NULL;
}
