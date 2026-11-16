/*
 * A native module written against lzapi.h and nothing else of Lizard. tests/test_native.c includes
 * it before any internal header, and tests/native/check-standalone.sh compiles it alone with only
 * lzapi.h in sight, which is what keeps the public header self-contained.
 */
#include "lzapi.h"

#include <stdio.h>
#include <string.h>

static int g_err_nmod = -1;
static int g_err_child = -1;
static int g_inits;
static int g_created;
static int g_finalized;
static LznRef *g_callback;
static LznRef *g_kept;
static LznValue *g_stale;

typedef struct CounterData {
    int64_t value;
    char *buffer;
    int64_t step;
} CounterData;

/* ---- plain functions ------------------------------------------------------------------------ */

static LznStatus nm_add(LznCtx *ctx) {
    int64_t a, b;
    if (lzn_check_int(ctx, 0, &a) != LZN_OK || lzn_check_int(ctx, 1, &b) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return_int(ctx, a + b);
}

static LznStatus nm_scale(LznCtx *ctx) {
    double x;
    int64_t k;
    if (lzn_check_float(ctx, 0, &x) != LZN_OK || lzn_check_int(ctx, 1, &k) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return_float(ctx, x * (double)k);
}

static LznStatus nm_greet(LznCtx *ctx) {
    const char *name;
    size_t length;
    char buffer[128];
    if (lzn_check_string(ctx, 0, &name, &length) != LZN_OK) {
        return LZN_FAIL;
    }
    int n = snprintf(buffer, sizeof buffer, "hello, %.*s", (int)length, name);
    return lzn_return_string(ctx, buffer, (size_t)n);
}

static LznStatus nm_flip(LznCtx *ctx) {
    bool b;
    if (lzn_check_bool(ctx, 0, &b) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return_bool(ctx, !b);
}

static LznStatus nm_kind(LznCtx *ctx) {
    const char *name = lzn_kind_name(lzn_kind(ctx, lzn_arg(ctx, 0)));
    return lzn_return_string(ctx, name, strlen(name));
}

static LznStatus nm_nothing(LznCtx *ctx) {
    return lzn_return_nil(ctx);
}

static LznStatus nm_count_args(LznCtx *ctx) {
    return lzn_return_int(ctx, lzn_arg_count(ctx));
}

/* A wrong conversion fails and changes nothing: ints are not floats here, strings are not ints. */
static LznStatus nm_strict(LznCtx *ctx) {
    int64_t i = -1;
    double f = -1;
    bool b = false;
    LznValue *v = lzn_arg(ctx, 0);
    int score = 0;
    score += lzn_to_int(ctx, v, &i) == LZN_OK ? 1 : 0;
    score += lzn_to_float(ctx, v, &f) == LZN_OK ? 10 : 0;
    score += lzn_to_bool(ctx, v, &b) == LZN_OK ? 100 : 0;
    score += lzn_string_data(ctx, v, NULL) != NULL ? 1000 : 0;
    return lzn_return_int(ctx, score);
}

/* ---- arrays and dicts ----------------------------------------------------------------------- */

static LznStatus nm_sum(LznCtx *ctx) {
    LznValue *array;
    if (lzn_check_array(ctx, 0, &array) != LZN_OK) {
        return LZN_FAIL;
    }
    int64_t total = 0;
    for (size_t i = 0; i < lzn_array_len(ctx, array); i++) {
        int64_t item;
        if (lzn_to_int(ctx, lzn_array_get(ctx, array, i), &item) != LZN_OK) {
            return lzn_throw(ctx, LZN_ERR_TYPE, "sum: item %zu is not an int", i);
        }
        total += item;
    }
    return lzn_return_int(ctx, total);
}

static LznStatus nm_range(LznCtx *ctx) {
    int64_t n;
    if (lzn_check_int(ctx, 0, &n) != LZN_OK) {
        return LZN_FAIL;
    }
    LznValue *array = lzn_array(ctx);
    if (array == NULL) {
        return LZN_FAIL;
    }
    for (int64_t i = 0; i < n; i++) {
        if (lzn_array_push(ctx, array, lzn_int(ctx, i * i)) != LZN_OK) {
            return LZN_FAIL;
        }
    }
    return lzn_return(ctx, array);
}

static LznStatus nm_poke(LznCtx *ctx) {
    LznValue *array;
    int64_t at, value;
    if (lzn_check_array(ctx, 0, &array) != LZN_OK || lzn_check_int(ctx, 1, &at) != LZN_OK ||
        lzn_check_int(ctx, 2, &value) != LZN_OK) {
        return LZN_FAIL;
    }
    if (lzn_array_set(ctx, array, (size_t)at, lzn_int(ctx, value)) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return_nil(ctx);
}

static LznStatus nm_make_dict(LznCtx *ctx) {
    const char *key;
    size_t length;
    int64_t value;
    if (lzn_check_string(ctx, 0, &key, &length) != LZN_OK || lzn_check_int(ctx, 1, &value) != LZN_OK) {
        return LZN_FAIL;
    }
    LznValue *dict = lzn_dict(ctx);
    if (dict == NULL || lzn_dict_set(ctx, dict, key, lzn_int(ctx, value)) != LZN_OK ||
        lzn_dict_set_int(ctx, dict, 7, lzn_string(ctx, "seven", 5)) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return(ctx, dict);
}

static LznStatus nm_dict_info(LznCtx *ctx) {
    LznValue *dict;
    if (lzn_check_dict(ctx, 0, &dict) != LZN_OK) {
        return LZN_FAIL;
    }
    int64_t score = (int64_t)lzn_dict_len(ctx, dict);
    score += lzn_dict_has(ctx, dict, "a") ? 10 : 0;
    score += lzn_dict_has_int(ctx, dict, 7) ? 100 : 0;
    LznValue *a = lzn_dict_get(ctx, dict, "a");
    int64_t a_value;
    if (a != NULL && lzn_to_int(ctx, a, &a_value) == LZN_OK) {
        score += a_value * 1000;
    }
    score += lzn_dict_get(ctx, dict, "missing") == NULL ? 100000 : 0;
    return lzn_return_int(ctx, score);
}

/* ---- errors --------------------------------------------------------------------------------- */

static LznStatus nm_fail(LznCtx *ctx) {
    int64_t which;
    if (lzn_check_int(ctx, 0, &which) != LZN_OK) {
        return LZN_FAIL;
    }
    switch (which) {
    case 0: return lzn_throw(ctx, LZN_ERR_TYPE, "builtin type error %d", 1);
    case 1: return lzn_throw(ctx, LZN_ERR_RANGE, "builtin range error");
    case 2: return lzn_throw(ctx, g_err_nmod, "module error");
    case 3: return lzn_throw(ctx, g_err_child, "child of the module error");
    default: return LZN_FAIL; /* fails without an error: the VM must still report something */
    }
}

/* ---- calling back and lasting references ---------------------------------------------------- */

static LznStatus nm_apply(LznCtx *ctx) {
    LznValue *fn;
    int64_t x;
    if (lzn_check_function(ctx, 0, &fn) != LZN_OK || lzn_check_int(ctx, 1, &x) != LZN_OK) {
        return LZN_FAIL;
    }
    LznValue *args[1] = {lzn_int(ctx, x)};
    LznValue *result = NULL;
    if (lzn_call(ctx, fn, 1, args, &result) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return(ctx, result);
}

static LznStatus nm_store(LznCtx *ctx) {
    LznValue *fn;
    if (lzn_check_function(ctx, 0, &fn) != LZN_OK) {
        return LZN_FAIL;
    }
    LznRef *ref = lzn_ref(ctx, fn);
    if (ref == NULL) {
        return LZN_FAIL;
    }
    lzn_unref(g_callback);
    g_callback = ref;
    return lzn_return_nil(ctx);
}

static LznStatus nm_fire(LznCtx *ctx) {
    int64_t x;
    if (lzn_check_int(ctx, 0, &x) != LZN_OK) {
        return LZN_FAIL;
    }
    if (g_callback == NULL) {
        return lzn_throw(ctx, LZN_ERR_RUNTIME, "no callback stored");
    }
    LznValue *fn = lzn_deref(ctx, g_callback);
    LznValue *args[1] = {lzn_int(ctx, x)};
    LznValue *result = NULL;
    if (fn == NULL || lzn_call(ctx, fn, 1, args, &result) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return(ctx, result);
}

static LznStatus nm_clear(LznCtx *ctx) {
    lzn_unref(g_callback);
    g_callback = NULL;
    return lzn_return_nil(ctx);
}

/* Keeps any value alive past the program, so the VM has to let go of it at the end. */
static LznStatus nm_keep(LznCtx *ctx) {
    LznRef *ref = lzn_ref(ctx, lzn_arg(ctx, 0));
    if (ref == NULL) {
        return LZN_FAIL;
    }
    g_kept = ref;
    return lzn_return_nil(ctx);
}

static LznStatus nm_remember_handle(LznCtx *ctx) {
    g_stale = lzn_int(ctx, 5);
    return lzn_return_nil(ctx);
}

static LznStatus nm_use_handle(LznCtx *ctx) {
    return lzn_return(ctx, g_stale);
}

/* ---- memory and fuel ------------------------------------------------------------------------ */

static LznStatus nm_spin(LznCtx *ctx) {
    int64_t n;
    if (lzn_check_int(ctx, 0, &n) != LZN_OK) {
        return LZN_FAIL;
    }
    for (int64_t i = 0; i < n; i++) {
        if (lzn_spend(ctx, 1) != LZN_OK) {
            return LZN_FAIL;
        }
    }
    return lzn_return_int(ctx, n);
}

static LznStatus nm_alloc(LznCtx *ctx) {
    int64_t size;
    if (lzn_check_int(ctx, 0, &size) != LZN_OK) {
        return LZN_FAIL;
    }
    char *block = lzn_alloc(ctx, (size_t)size);
    if (block == NULL) {
        return LZN_FAIL;
    }
    memset(block, 1, (size_t)size);
    lzn_free(ctx, block, (size_t)size);
    return lzn_return_int(ctx, size);
}

static LznStatus nm_finalized(LznCtx *ctx) {
    return lzn_return_int(ctx, g_finalized);
}

static LznStatus nm_created(LznCtx *ctx) {
    return lzn_return_int(ctx, g_created);
}

/* ---- a class -------------------------------------------------------------------------------- */

static LznStatus counter_init(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    g_created++;
    d->value = 0;
    d->step = 1;
    d->buffer = lzn_alloc(ctx, 64);
    if (d->buffer == NULL) {
        return LZN_FAIL;
    }
    if (lzn_arg_count(ctx) > 0 && lzn_check_int(ctx, 0, &d->value) != LZN_OK) {
        return LZN_FAIL;
    }
    if (lzn_arg_count(ctx) > 1 && lzn_check_int(ctx, 1, &d->step) != LZN_OK) {
        return LZN_FAIL;
    }
    if (d->value < 0) {
        return lzn_throw(ctx, g_err_nmod, "Counter starts at a negative value");
    }
    return LZN_OK;
}

static void counter_finalize(void *data) {
    CounterData *d = data;
    g_finalized++;
    lzn_free(NULL, d->buffer, 64);
    d->buffer = NULL;
}

static LznStatus counter_add(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    int64_t n = d->step;
    if (lzn_arg_count(ctx) > 0 && lzn_check_int(ctx, 0, &n) != LZN_OK) {
        return LZN_FAIL;
    }
    d->value += n;
    return lzn_return_int(ctx, d->value);
}

static LznStatus counter_value(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    return lzn_return_int(ctx, d->value);
}

static LznStatus counter_merge(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    LznValue *other = lzn_arg(ctx, 0);
    if (lzn_kind(ctx, other) != LZN_INSTANCE) {
        return lzn_throw(ctx, LZN_ERR_TYPE, "Counter.merge(): expected a Counter");
    }
    d->value += 1000;
    return lzn_return(ctx, other);
}

static LznStatus counter_apply_to(LznCtx *ctx) {
    CounterData *d = lzn_self_data(ctx);
    LznValue *fn;
    if (lzn_check_function(ctx, 0, &fn) != LZN_OK) {
        return LZN_FAIL;
    }
    LznValue *args[1] = {lzn_int(ctx, d->value)};
    LznValue *result = NULL;
    if (lzn_call(ctx, fn, 1, args, &result) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_return(ctx, result);
}

static const LznMethodDef counter_methods[] = {
    {"add", counter_add, "fn(n: int) -> int", 0, 1},
    {"merge", counter_merge, "fn(other: Counter) -> Counter", 1, 1},
    {"apply_to", counter_apply_to, "fn(f: fn(int) -> int) -> int", 1, 1},
    {NULL, NULL, NULL, 0, 0},
};

static const LznFieldDef counter_fields[] = {
    {"value", "int", counter_value},
    {NULL, NULL, NULL},
};

/* ---- the module ----------------------------------------------------------------------------- */

LznStatus nmod_init(LznBuilder *b);
LznStatus nmod_init(LznBuilder *b) {
    g_inits++;
    g_err_nmod = lzn_module_error(b, "NmodError", LZN_ERR_RUNTIME);
    g_err_child = lzn_module_error(b, "NmodChild", g_err_nmod);
    if (g_err_nmod < 0 || g_err_child < 0) {
        return LZN_FAIL;
    }
    LznClassDef counter = {
        .size = sizeof(CounterData),
        .init = counter_init,
        .init_signature = "fn(value: int, step: int)",
        .init_min_args = 0,
        .init_max_args = 2,
        .finalize = counter_finalize,
        .methods = counter_methods,
        .fields = counter_fields,
    };
    if (lzn_module_class(b, "Counter", &counter) != LZN_OK) {
        return LZN_FAIL;
    }
    if (lzn_module_const_int(b, "VERSION", 3) != LZN_OK || lzn_module_const_float(b, "HALF", 0.5) != LZN_OK ||
        lzn_module_const_string(b, "NAME", "nmod") != LZN_OK || lzn_module_const_bool(b, "ON", true) != LZN_OK) {
        return LZN_FAIL;
    }
    struct {
        const char *name;
        LznFn fn;
        const char *sig;
        int min, max;
    } fns[] = {
        {"add", nm_add, "fn(int, int) -> int", 2, 2},
        {"scale", nm_scale, "fn(x: float, k: int) -> float", 2, 2},
        {"greet", nm_greet, "fn(string) -> string", 1, 1},
        {"flip", nm_flip, "fn(bool) -> bool", 1, 1},
        {"kind", nm_kind, "fn(any) -> string", 1, 1},
        {"nothing", nm_nothing, "fn()", 0, 0},
        {"count_args", nm_count_args, "fn(a: any, b: any, c: any) -> int", 0, 3},
        {"strict", nm_strict, "fn(any) -> int", 1, 1},
        {"sum", nm_sum, "fn(int[]) -> int", 1, 1},
        {"range", nm_range, "fn(int) -> int[]", 1, 1},
        {"poke", nm_poke, "fn(int[], int, int)", 3, 3},
        {"make_dict", nm_make_dict, "fn(string, int) -> any", 2, 2},
        {"dict_info", nm_dict_info, "fn(any) -> int", 1, 1},
        {"fail", nm_fail, "fn(int)", 1, 1},
        {"apply", nm_apply, "fn(fn(int) -> int, int) -> int", 2, 2},
        {"store", nm_store, "fn(fn(int) -> int)", 1, 1},
        {"fire", nm_fire, "fn(int) -> int", 1, 1},
        {"clear", nm_clear, "fn()", 0, 0},
        {"keep", nm_keep, "fn(any)", 1, 1},
        {"remember_handle", nm_remember_handle, "fn()", 0, 0},
        {"use_handle", nm_use_handle, "fn() -> int", 0, 0},
        {"spin", nm_spin, "fn(int) -> int", 1, 1},
        {"alloc", nm_alloc, "fn(int) -> int", 1, 1},
        {"finalized", nm_finalized, "fn() -> int", 0, 0},
        {"created", nm_created, "fn() -> int", 0, 0},
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        if (lzn_module_fn(b, fns[i].name, fns[i].fn, fns[i].sig, fns[i].min, fns[i].max) != LZN_OK) {
            return LZN_FAIL;
        }
    }
    return LZN_OK;
}

void nmod_reset(void);
void nmod_reset(void) {
    g_inits = 0;
    g_created = 0;
    g_finalized = 0;
    g_callback = NULL;
    g_kept = NULL;
    g_stale = NULL;
}

int nmod_inits(void);
int nmod_inits(void) {
    return g_inits;
}

int nmod_created(void);
int nmod_created(void) {
    return g_created;
}

int nmod_finalized(void);
int nmod_finalized(void) {
    return g_finalized;
}

/* ---- modules that must not register --------------------------------------------------------- */

LznStatus badsig_init(LznBuilder *b);
LznStatus badsig_init(LznBuilder *b) {
    return lzn_module_fn(b, "f", nm_nothing, "fn(int", 1, 1);
}

LznStatus badtype_init(LznBuilder *b);
LznStatus badtype_init(LznBuilder *b) {
    return lzn_module_fn(b, "f", nm_add, "fn(int[, int) -> int", 2, 2);
}

LznStatus badcount_init(LznBuilder *b);
LznStatus badcount_init(LznBuilder *b) {
    return lzn_module_fn(b, "f", nm_add, "fn(int, int) -> int", 1, 1);
}

LznStatus dupname_init(LznBuilder *b);
LznStatus dupname_init(LznBuilder *b) {
    if (lzn_module_fn(b, "f", nm_add, "fn(int, int) -> int", 2, 2) != LZN_OK) {
        return LZN_FAIL;
    }
    return lzn_module_const_int(b, "f", 1);
}

/* A second module with the same error name as the first, to show that kinds are told apart by module. */
static int g_err_other = -1;

static LznStatus other_fail(LznCtx *ctx) {
    return lzn_throw(ctx, g_err_other, "the other module failed");
}

LznStatus other_init(LznBuilder *b);
LznStatus other_init(LznBuilder *b) {
    g_err_other = lzn_module_error(b, "NmodError", LZN_ERR_RUNTIME);
    if (g_err_other < 0) {
        return LZN_FAIL;
    }
    return lzn_module_fn(b, "fail", other_fail, "fn()", 0, 0);
}
