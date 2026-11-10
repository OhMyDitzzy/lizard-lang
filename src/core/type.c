#include "lizard/type.h"
#include "lizard/error.h"

#include "lizard/array.h"
#include "lizard/class.h"
#include "lizard/dict.h"
#include "lizard/function.h"
#include "lizard/link.h"
#include "lizard/native.h"

#include <stdio.h>
#include <string.h>

#define DESCRIBE_STRING_LIMIT 24

uint16_t lz_kind_of(const LzValue *value) {
    switch (value->type) {
    case LZ_VAL_NIL:
        return LZ_KIND_NIL;
    case LZ_VAL_BOOL:
        return LZ_KIND_BOOL;
    case LZ_VAL_INT:
        return LZ_KIND_INT;
    case LZ_VAL_FLOAT:
        return LZ_KIND_FLOAT;
    case LZ_VAL_OBJECT:
        switch (value->as.object->type) {
        case LZ_OBJ_STRING:
            return LZ_KIND_STRING;
        case LZ_OBJ_ARRAY:
            return LZ_KIND_ARRAY;
        case LZ_OBJ_DICT:
            return LZ_KIND_DICT;
        case LZ_OBJ_ERROR:
            return LZ_KIND_ERROR;
        case LZ_OBJ_NATIVE:
        case LZ_OBJ_CLOSURE:
        case LZ_OBJ_CLASS:
        case LZ_OBJ_BOUND:
            return LZ_KIND_FUNCTION;
        case LZ_OBJ_INSTANCE:
            return LZ_KIND_INSTANCE;
        case LZ_OBJ_LINK:
        case LZ_OBJ_FUNCTION:
        case LZ_OBJ_UPVALUE:
            return 0; /* no kind of its own, because a link is tested by its target and the rest are internal */
        }
        return 0;
    }
    return 0;
}

const char *lz_kind_name(uint16_t kind) {
    switch (kind) {
    case LZ_KIND_NIL:
        return "nil";
    case LZ_KIND_BOOL:
        return "bool";
    case LZ_KIND_INT:
        return "int";
    case LZ_KIND_FLOAT:
        return "float";
    case LZ_KIND_STRING:
        return "string";
    case LZ_KIND_FUNCTION:
        return "function";
    case LZ_KIND_ARRAY:
        return "array";
    case LZ_KIND_DICT:
        return "dict";
    case LZ_KIND_ERROR:
        return "error";
    case LZ_KIND_INSTANCE:
        return "object";
    default:
        return "value";
    }
}

const char *lz_value_kind_name(const LzValue *value) {
    if (lz_value_is_link(value)) {
        return "link";
    }
    if (lz_value_is_instance(value)) {
        return lz_value_as_instance(value)->cls->name->data;
    }
    return lz_kind_name(lz_kind_of(value));
}

LzType lz_type_of_kinds(uint16_t kinds) {
    LzType type;
    memset(&type, 0, sizeof type);
    type.kinds = kinds;
    return type;
}

static uint16_t literal_kind_bit(const LzLiteral *literal) {
    switch (literal->kind) {
    case LZ_VAL_INT:
        return LZ_KIND_INT;
    case LZ_VAL_BOOL:
        return LZ_KIND_BOOL;
    default:
        return LZ_KIND_STRING;
    }
}

static bool strings_equal(const LzString *a, const LzString *b) {
    return a == b || (a->length == b->length && memcmp(a->data, b->data, a->length) == 0);
}

static bool literals_equal(const LzLiteral *a, const LzLiteral *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case LZ_VAL_INT:
        return a->as.integer == b->as.integer;
    case LZ_VAL_BOOL:
        return a->as.boolean == b->as.boolean;
    default:
        return strings_equal(a->as.string, b->as.string);
    }
}

static bool literal_matches(const LzLiteral *literal, const LzValue *value) {
    if (literal->kind != value->type) {
        return false;
    }
    switch (literal->kind) {
    case LZ_VAL_INT:
        return literal->as.integer == value->as.integer;
    case LZ_VAL_BOOL:
        return literal->as.boolean == value->as.boolean;
    default:
        return value->as.object->type == LZ_OBJ_STRING &&
               strings_equal(literal->as.string, (const LzString *)value->as.object);
    }
}

static bool literal_from_value(const LzValue *value, LzLiteral *out) {
    switch (value->type) {
    case LZ_VAL_INT:
        out->kind = LZ_VAL_INT;
        out->as.integer = value->as.integer;
        return true;
    case LZ_VAL_BOOL:
        out->kind = LZ_VAL_BOOL;
        out->as.boolean = value->as.boolean;
        return true;
    case LZ_VAL_OBJECT:
        if (value->as.object->type == LZ_OBJ_STRING) {
            out->kind = LZ_VAL_OBJECT;
            out->as.string = (const LzString *)value->as.object;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LzType lz_type_of_value(const LzValue *value) {
    if (value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_LINK) {
        /* A link is described as what it points at, or as nil when its target is gone. */
        LzValue target = lz_link_deref((const LzLink *)value->as.object);
        return lz_type_of_value(&target);
    }
    LzType type = lz_type_of_kinds(0);
    LzLiteral literal;
    if (literal_from_value(value, &literal)) {
        type.literals[0] = literal;
        type.literal_count = 1;
    } else {
        type.kinds = lz_kind_of(value);
        if (lz_value_is_array(value)) {
            type.element = lz_value_as_array(value)->element;
        } else if (lz_value_is_dict(value)) {
            type.element = lz_value_as_dict(value)->key_type;
            type.value_element = lz_value_as_dict(value)->value_type;
        } else if (lz_value_is_instance(value)) {
            type.class_name = lz_value_as_instance(value)->cls->name;
            type.class_id = lz_value_as_instance(value)->cls->id;
        }
    }
    return type;
}

uint16_t lz_type_possible_kinds(const LzType *type) {
    uint16_t kinds = type->kinds;
    for (size_t i = 0; i < type->literal_count; i++) {
        kinds = (uint16_t)(kinds | literal_kind_bit(&type->literals[i]));
    }
    return kinds;
}

LzType lz_type_widen(const LzType *type) {
    LzType widened = lz_type_of_kinds(lz_type_possible_kinds(type));
    widened.element = type->element;
    widened.value_element = type->value_element;
    widened.class_name = type->class_name;
    widened.class_id = type->class_id;
    widened.fn = type->fn;
    widened.tparam = type->tparam;
    widened.tparam_nullable = type->tparam_nullable;
    return widened;
}

static bool literal_accepted(const LzType *type, const LzLiteral *literal) {
    if ((literal_kind_bit(literal) & type->kinds) != 0) {
        return true;
    }
    for (size_t i = 0; i < type->literal_count; i++) {
        if (literals_equal(&type->literals[i], literal)) {
            return true;
        }
    }
    return false;
}

bool lz_type_add_literal(LzType *type, const LzLiteral *literal) {
    if (literal_accepted(type, literal)) {
        return true;
    }
    if (type->literal_count >= LZ_TYPE_MAX_LITERALS) {
        return false;
    }
    type->literals[type->literal_count++] = *literal;
    return true;
}

/* A type parameter and nil, like `T?`. Any other union with one is `any` anyway. */
static bool type_param_or_nil(LzType *out, const LzType *a, const LzType *b) {
    if (a->tparam != NULL && a->kinds == LZ_KINDS_ANY && b->kinds == LZ_KIND_NIL && b->literal_count == 0) {
        *out = *a;
        out->tparam_nullable = 1;
        return true;
    }
    return false;
}

bool lz_type_union(LzType *out, const LzType *a, const LzType *b) {
    if (type_param_or_nil(out, a, b) || type_param_or_nil(out, b, a)) {
        return true;
    }
    LzType result = lz_type_of_kinds((uint16_t)(a->kinds | b->kinds));
    if ((a->kinds & LZ_KIND_ARRAY) != 0 && (b->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = a->element == b->element ? a->element : NULL;
    } else if ((a->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = a->element;
    } else if ((b->kinds & LZ_KIND_ARRAY) != 0) {
        result.element = b->element;
    }
    /* Two different dict types in a union merge into a dict of any, like arrays do. */
    if ((a->kinds & LZ_KIND_DICT) != 0 && (b->kinds & LZ_KIND_DICT) != 0) {
        if (a->element == b->element && a->value_element == b->value_element) {
            result.element = a->element;
            result.value_element = a->value_element;
        }
    } else if ((a->kinds & LZ_KIND_DICT) != 0) {
        result.element = a->element;
        result.value_element = a->value_element;
    } else if ((b->kinds & LZ_KIND_DICT) != 0) {
        result.element = b->element;
        result.value_element = b->value_element;
    }
    if ((a->kinds & LZ_KIND_INSTANCE) != 0 && (b->kinds & LZ_KIND_INSTANCE) != 0) {
        bool same = a->class_id == b->class_id;
        result.class_name = same ? a->class_name : NULL; /* any object */
        result.class_id = same ? a->class_id : 0;
    } else if ((a->kinds & LZ_KIND_INSTANCE) != 0) {
        result.class_name = a->class_name;
        result.class_id = a->class_id;
    } else if ((b->kinds & LZ_KIND_INSTANCE) != 0) {
        result.class_name = b->class_name;
        result.class_id = b->class_id;
    }
    /* Two different function signatures in a union merge into any function, like arrays do. */
    if ((a->kinds & LZ_KIND_FUNCTION) != 0 && (b->kinds & LZ_KIND_FUNCTION) != 0) {
        result.fn = a->fn == b->fn ? a->fn : NULL;
    } else if ((a->kinds & LZ_KIND_FUNCTION) != 0) {
        result.fn = a->fn;
    } else if ((b->kinds & LZ_KIND_FUNCTION) != 0) {
        result.fn = b->fn;
    }
    if ((result.kinds & LZ_KIND_INSTANCE) == 0 && (result.kinds & LZ_KIND_ERROR) != 0) {
        /*
         * an error kind (class_id is kind + 1, and 0 means any error), and two different ones
         * make any error
         */
        if ((a->kinds & LZ_KIND_ERROR) != 0 && (b->kinds & LZ_KIND_ERROR) != 0) {
            result.class_id = a->class_id == b->class_id ? a->class_id : 0;
        } else {
            result.class_id = (a->kinds & LZ_KIND_ERROR) != 0 ? a->class_id : b->class_id;
        }
    }
    for (size_t i = 0; i < a->literal_count; i++) {
        if (!lz_type_add_literal(&result, &a->literals[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < b->literal_count; i++) {
        if (!lz_type_add_literal(&result, &b->literals[i])) {
            return false;
        }
    }
    *out = result;
    return true;
}

bool lz_type_is_any(const LzType *type) {
    return type->kinds == LZ_KINDS_ANY && type->literal_count == 0 && type->element == NULL &&
           type->tparam == NULL;
}

bool lz_type_equal(const LzType *a, const LzType *b) {
    if (a->kinds != b->kinds || a->literal_count != b->literal_count || a->tparam != b->tparam ||
        a->tparam_nullable != b->tparam_nullable) {
        return false;
    }
    if ((a->kinds & LZ_KIND_FUNCTION) != 0 && a->fn != b->fn) {
        return false;
    }
    if ((a->kinds & LZ_KIND_ARRAY) != 0 && a->element != b->element) {
        return false;
    }
    if ((a->kinds & LZ_KIND_DICT) != 0 &&
        (a->element != b->element || a->value_element != b->value_element)) {
        return false;
    }
    if ((a->kinds & LZ_KIND_INSTANCE) != 0 && a->class_id != b->class_id) {
        return false;
    }
    if ((a->kinds & LZ_KIND_INSTANCE) == 0 && (a->kinds & LZ_KIND_ERROR) != 0 && a->class_id != b->class_id) {
        return false;
    }
    for (size_t i = 0; i < a->literal_count; i++) {
        if (!literal_accepted(b, &a->literals[i])) {
            return false;
        }
    }
    return true;
}

static uint32_t mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDull;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ull;
    value ^= value >> 33;
    return (uint32_t)value;
}

uint32_t lz_type_hash(const LzType *type) {
    uint32_t hash = mix(type->kinds + 1u);
    if ((type->kinds & LZ_KIND_FUNCTION) != 0 && type->fn != NULL) {
        hash ^= mix((uint64_t)(uintptr_t)type->fn ^ 0xAAAAu);
    }
    if (type->tparam != NULL) {
        hash ^= mix((uint64_t)(uintptr_t)type->tparam ^ 0xBBBBu ^ type->tparam_nullable);
    }
    if ((type->kinds & LZ_KIND_ARRAY) != 0) {
        hash ^= mix((uint64_t)(uintptr_t)type->element);
    }
    if ((type->kinds & LZ_KIND_DICT) != 0) {
        hash ^= mix((uint64_t)(uintptr_t)type->element ^ 0x7777u);
        hash ^= mix((uint64_t)(uintptr_t)type->value_element ^ 0x8888u);
    }
    if ((type->kinds & LZ_KIND_INSTANCE) != 0) {
        hash ^= mix(type->class_id ^ 0x9999u);
    }
    for (size_t i = 0; i < type->literal_count; i++) {
        const LzLiteral *literal = &type->literals[i];
        uint32_t part;
        switch (literal->kind) {
        case LZ_VAL_INT:
            part = mix((uint64_t)literal->as.integer);
            break;
        case LZ_VAL_BOOL:
            part = mix(literal->as.boolean ? 0x1111u : 0x2222u);
            break;
        default:
            part = literal->as.string->hash ^ 0x5BD1E995u;
            break;
        }
        hash += part; /* a sum, so the order of the literals does not matter */
    }
    return hash;
}

#if defined(__GNUC__) || defined(__clang__)
#define INSTANCE_KIND_UNLIKELY(kind) __builtin_expect((kind) == LZ_KIND_INSTANCE, 0)
#else
#define INSTANCE_KIND_UNLIKELY(kind) ((kind) == LZ_KIND_INSTANCE)
#endif

static bool takes_arguments(const LzValue *value, size_t argc, int depth) {
    if (value->type != LZ_VAL_OBJECT) {
        return true;
    }
    switch (value->as.object->type) {
    case LZ_OBJ_CLOSURE: {
        const LzFunction *function = ((const LzClosure *)value->as.object)->function;
        return argc >= function->required && (function->has_rest || argc <= function->param_count);
    }
    case LZ_OBJ_NATIVE: {
        const LzNative *native = (const LzNative *)value->as.object;
        return argc >= (size_t)native->min_args &&
               (native->max_args == LZ_VARIADIC || argc <= (size_t)native->max_args);
    }
    case LZ_OBJ_BOUND: {
        const LzBound *bound = (const LzBound *)value->as.object;
        if (bound->skip_first || depth > 4) {
            return true; /* a call of a function held in a field, so the shape is the callee's own */
        }
        return takes_arguments(&bound->callee, argc + 1, depth + 1); /* the receiver comes first */
    }
    default:
        return true; /* a class, and its constructor checks the call itself */
    }
}

bool lz_function_value_takes(const LzValue *value, size_t argc) {
    return takes_arguments(value, argc, 0);
}

/* A function value against the signature a type asks for. Only the argument count is known. */
static bool function_fits(const LzFnType *fn, const LzValue *value) {
    /* A caller of this type passes any number from `required` to the most it names. */
    if (!lz_function_value_takes(value, fn->required)) {
        return false;
    }
    return fn->has_rest || lz_function_value_takes(value, fn->param_count);
}

/*
 * The `is` operator tests a link by its target, so no target means no match. Anything else works
 * as usual.
 */
bool lz_type_matches_through_link(const LzType *type, const LzValue *value) {
    if (lz_value_is_link(value)) {
        const LzLink *link = lz_value_as_link(value);
        if (link->target == NULL) {
            return false;
        }
        LzValue target = lz_value_object(link->target);
        return lz_type_matches(type, &target);
    }
    return lz_type_matches(type, value);
}

bool lz_type_matches(const LzType *type, const LzValue *value) {
    if (lz_value_is_array(value)) {
        if ((type->kinds & LZ_KIND_ARRAY) == 0) {
            return false;
        }
        const LzArray *array = lz_value_as_array(value);
        const LzType *wanted = type->element;
        if (wanted == NULL || array->element == wanted) {
            return true;
        }
        if (array->element != NULL && lz_type_assignable(array->element, wanted) == LZ_ASSIGN_ALWAYS) {
            return true; /* every item already fits the tag, and the tag fits the target */
        }
        for (size_t i = 0; i < array->count; i++) {
            if (!lz_type_matches(wanted, &array->items[i])) {
                return false;
            }
        }
        return true;
    }
    if (lz_value_is_dict(value)) {
        if ((type->kinds & LZ_KIND_DICT) == 0) {
            return false;
        }
        const LzDict *dict = lz_value_as_dict(value);
        if (type->element == NULL && type->value_element == NULL) {
            return true;
        }
        return lz_dict_conform((LzDict *)dict, type->element, type->value_element);
    }
    uint16_t value_kind = lz_kind_of(value);
    if (value_kind == LZ_KIND_ERROR && (type->kinds & LZ_KIND_ERROR) != 0 && (type->kinds & LZ_KIND_INSTANCE) == 0 &&
        type->class_id != 0) {
        const LzError *error = (const LzError *)value->as.object;
        size_t bit = (size_t)(type->class_id - 1);
        return bit < 256 && ((error->is_kind[bit / 64] >> (bit % 64)) & 1u) != 0;
    }
    if (value_kind == LZ_KIND_ERROR && (type->kinds & LZ_KIND_INSTANCE) != 0 && type->class_id != 0) {
        /* an error of a class that has this trait, or this class as a parent */
        return lz_error_is_class((const LzError *)value->as.object, type->class_id);
    }
    if (value_kind == 0 && value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_LINK) {
        /*
         * A link cell is not the object it points at. It fits no class, array, dict, function or
         * scalar type, because a typed parameter, variable, array item or plain field would
         * otherwise be handed a cell where code expects an instance. Only `any` takes it,
         * because `any` makes no promise about the value. A link FIELD checks its target instead
         * (class.c), and `link(x) is Node` asks lz_type_matches_through_link.
         */
        return (type->kinds & LZ_KINDS_ANY) == LZ_KINDS_ANY && type->class_id == 0;
    }
    if (INSTANCE_KIND_UNLIKELY(value_kind)) {
        return (type->kinds & LZ_KIND_INSTANCE) != 0 &&
               (type->class_id == 0 || lz_class_is_a(lz_value_as_instance(value)->cls, type->class_id));
    }
    if ((value_kind & type->kinds) != 0) {
        if (value_kind == LZ_KIND_FUNCTION && type->fn != NULL) {
            return function_fits(type->fn, value); /* the count of arguments, and types are not tested */
        }
        return true;
    }
    for (size_t i = 0; i < type->literal_count; i++) {
        if (literal_matches(&type->literals[i], value)) {
            return true;
        }
    }
    return false;
}

/* How an array of items `from` can be stored as an array of items `to`, where NULL means any. */
static LzAssign element_assignable(const LzType *from, const LzType *to) {
    if (to == NULL) {
        return LZ_ASSIGN_ALWAYS;
    }
    if (from == NULL) {
        LzType any = lz_type_of_kinds(LZ_KINDS_ANY);
        return lz_type_assignable(&any, to);
    }
    return lz_type_assignable(from, to);
}

/*
 * The type of parameter `index` of a signature, or `any` when it names none. False when there's
 * no such parameter.
 */
static bool fn_param_type(const LzFnType *fn, size_t index, LzType *out) {
    const LzType *found;
    if (index < fn->param_count) {
        found = fn->params[index];
    } else if (fn->has_rest) {
        found = fn->rest;
    } else {
        return false;
    }
    *out = found != NULL ? *found : lz_type_of_kinds(LZ_KINDS_ANY);
    return true;
}

static LzAssign worst(LzAssign a, LzAssign b) {
    return a == LZ_ASSIGN_NEVER || b == LZ_ASSIGN_NEVER ? LZ_ASSIGN_NEVER
           : a == LZ_ASSIGN_MAYBE || b == LZ_ASSIGN_MAYBE ? LZ_ASSIGN_MAYBE
                                                           : LZ_ASSIGN_ALWAYS;
}

/*
 * Can a function of signature `from` stand where signature `to` is asked for? The callers of
 * `to` pass between `to->required` and `to->param_count` arguments, or any number with a rest
 * parameter, so `from` must take all of those. Parameters are contravariant, meaning what the
 * callers pass must fit what the function takes, and the result is covariant. A NULL `from` is a
 * function of unknown shape, which might fit.
 */
static LzAssign fn_assignable(const LzFnType *from, const LzFnType *to) {
    if (to == NULL) {
        return LZ_ASSIGN_ALWAYS;
    }
    if (from == NULL) {
        return LZ_ASSIGN_MAYBE;
    }
    size_t from_max = from->has_rest ? SIZE_MAX : from->param_count;
    size_t to_max = to->has_rest ? SIZE_MAX : to->param_count;
    size_t low = from->required > to->required ? from->required : to->required;
    size_t high = from_max < to_max ? from_max : to_max;
    if (low > high) {
        return LZ_ASSIGN_NEVER; /* no call of the one is a call the other takes */
    }
    LzAssign verdict = from->required <= to->required && to_max <= from_max ? LZ_ASSIGN_ALWAYS
                                                                           : LZ_ASSIGN_MAYBE;
    size_t shown = from->param_count > to->param_count ? from->param_count : to->param_count;
    if ((from->has_rest || to->has_rest) && shown < SIZE_MAX) {
        shown += 1;
    }
    for (size_t i = 0; i < shown; i++) {
        LzType given;
        LzType taken;
        if (!fn_param_type(to, i, &given) || !fn_param_type(from, i, &taken)) {
            continue;
        }
        verdict = worst(verdict, lz_type_assignable(&given, &taken));
        if (verdict == LZ_ASSIGN_NEVER) {
            return verdict;
        }
    }
    if (to->result != NULL) {
        LzType produced = from->result != NULL ? *from->result : lz_type_of_kinds(LZ_KINDS_ANY);
        verdict = worst(verdict, lz_type_assignable(&produced, to->result));
    }
    return verdict;
}

LzAssign lz_type_assignable(const LzType *from, const LzType *to) {
    uint16_t scalar = (uint16_t)~(LZ_KIND_ARRAY | LZ_KIND_DICT | LZ_KIND_INSTANCE | LZ_KIND_FUNCTION);
    bool always = (from->kinds & ~to->kinds & LZ_KINDS_ANY & scalar) == 0;
    bool overlap = (from->kinds & to->kinds & scalar) != 0;

    if ((from->kinds & LZ_KIND_INSTANCE) != 0) {
        if ((to->kinds & LZ_KIND_INSTANCE) == 0) {
            always = false;
        } else if (to->class_id == 0 || to->class_id == from->class_id) {
            overlap = true;
        } else if (from->class_id == 0) {
            always = false; /* some object, maybe of that class */
            overlap = true;
        } else {
            always = false; /* stage 2 would need a subclass of to->class_name to overlap here */
        }
    }

    if ((from->kinds & LZ_KIND_ERROR) != 0 && (to->kinds & LZ_KIND_ERROR) != 0 &&
        (to->kinds & LZ_KIND_INSTANCE) == 0 && to->class_id != 0 && from->class_id != to->class_id) {
        always = false; /* some error, maybe of that kind: checked when it runs */
        overlap = true;
    }

    if ((from->kinds & LZ_KIND_FUNCTION) != 0) {
        if ((to->kinds & LZ_KIND_FUNCTION) == 0) {
            always = false;
        } else {
            LzAssign shape = fn_assignable(from->fn, to->fn);
            if (shape != LZ_ASSIGN_ALWAYS) {
                always = false;
            }
            if (shape != LZ_ASSIGN_NEVER) {
                overlap = true;
            }
        }
    }

    if ((from->kinds & LZ_KIND_ARRAY) != 0) {
        if ((to->kinds & LZ_KIND_ARRAY) == 0) {
            always = false;
        } else {
            LzAssign items = element_assignable(from->element, to->element);
            if (items != LZ_ASSIGN_ALWAYS) {
                always = false;
            }
            if (items != LZ_ASSIGN_NEVER) {
                overlap = true;
            }
        }
    }

    if ((from->kinds & LZ_KIND_DICT) != 0) {
        if ((to->kinds & LZ_KIND_DICT) == 0) {
            always = false;
        } else {
            LzType from_keys = lz_type_of_kinds(0);
            LzType to_keys = lz_type_of_kinds(0);
            LzType from_values = lz_type_of_kinds(0);
            LzType to_values = lz_type_of_kinds(0);
            if (from->element != NULL) {
                from_keys = *from->element;
            }
            if (to->element != NULL) {
                to_keys = *to->element;
            }
            if (from->value_element != NULL) {
                from_values = *from->value_element;
            }
            if (to->value_element != NULL) {
                to_values = *to->value_element;
            }
            LzType any = lz_type_of_kinds(LZ_KINDS_ANY);
            if (from->element == NULL) {
                from_keys = any;
            }
            if (to->element == NULL) {
                to_keys = any;
            }
            if (from->value_element == NULL) {
                from_values = any;
            }
            if (to->value_element == NULL) {
                to_values = any;
            }
            LzAssign keys = to->element == NULL ? LZ_ASSIGN_ALWAYS
                                                : lz_type_assignable(&from_keys, &to_keys);
            LzAssign values = to->value_element == NULL
                                  ? LZ_ASSIGN_ALWAYS
                                  : lz_type_assignable(&from_values, &to_values);
            if (keys != LZ_ASSIGN_ALWAYS || values != LZ_ASSIGN_ALWAYS) {
                always = false;
            }
            if (keys != LZ_ASSIGN_NEVER && values != LZ_ASSIGN_NEVER) {
                overlap = true;
            }
        }
    }

    for (size_t i = 0; i < from->literal_count; i++) {
        if (literal_accepted(to, &from->literals[i])) {
            overlap = true;
        } else {
            always = false;
        }
    }
    if (always) {
        return LZ_ASSIGN_ALWAYS;
    }

    for (size_t i = 0; i < to->literal_count; i++) {
        if ((literal_kind_bit(&to->literals[i]) & from->kinds) != 0) {
            overlap = true;
        }
    }
    return overlap ? LZ_ASSIGN_MAYBE : LZ_ASSIGN_NEVER;
}

static void write_text(const LzWriter *out, const char *text) {
    (void)lz_writer_write_cstr(out, text);
}

static void write_string(const LzWriter *out, const LzString *string) {
    size_t shown = string->length < DESCRIBE_STRING_LIMIT ? string->length : DESCRIBE_STRING_LIMIT;
    write_text(out, "\"");
    (void)lz_writer_write(out, string->data, shown);
    if (shown < string->length) {
        write_text(out, "...");
    }
    write_text(out, "\"");
}

static void write_literal(const LzWriter *out, const LzLiteral *literal) {
    char text[32];
    switch (literal->kind) {
    case LZ_VAL_INT:
        (void)snprintf(text, sizeof text, "%lld", (long long)literal->as.integer);
        write_text(out, text);
        break;
    case LZ_VAL_BOOL:
        write_text(out, literal->as.boolean ? "true" : "false");
        break;
    default:
        write_string(out, literal->as.string);
        break;
    }
}

/*
 * Does the text of a type end in a `-> result` that would swallow a `[]`, `?` or `|` written
 * after it?
 */
static bool has_top_arrow(const char *text) {
    int depth = 0;
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (text[i] == '(' || text[i] == '[') {
            depth++;
        } else if (text[i] == ')' || text[i] == ']') {
            depth--;
        } else if (depth == 0 && text[i] == '-' && text[i + 1] == '>') {
            return true;
        }
    }
    return false;
}

/*
 * The text of a type that is about to get `[]` or `?`. A union or a `fn(..) -> r` needs
 * parentheses.
 */
static bool needs_parens(const char *text) {
    return strstr(text, " | ") != NULL || has_top_arrow(text);
}

static void write_fn(const LzFnType *fn, const LzWriter *out) {
    write_text(out, "fn(");
    for (size_t i = 0; i < fn->param_count; i++) {
        if (i > 0) {
            write_text(out, ", ");
        }
        if (fn->params[i] != NULL) {
            lz_type_describe(fn->params[i], out);
        } else {
            write_text(out, "any");
        }
        if (i >= fn->required) {
            write_text(out, " = ..."); /* a parameter with a default, which is not something a type can say */
        }
    }
    if (fn->has_rest) {
        write_text(out, fn->param_count > 0 ? ", ..." : "...");
        if (fn->rest != NULL) {
            lz_type_describe(fn->rest, out);
        } else {
            write_text(out, "any");
        }
    }
    write_text(out, ")");
    if (fn->result != NULL) {
        write_text(out, " -> ");
        lz_type_describe(fn->result, out);
    }
}

/* A nested type in brackets, parenthesised when it's a union, like (int | string)[]. */
static void write_nested(const LzType *type, const LzWriter *out) {
    if (type == NULL) {
        write_text(out, "any");
        return;
    }
    LzBuffer item = {0};
    LzWriter item_out = lz_writer_from_buffer(&item);
    lz_type_describe(type, &item_out);
    bool compound = item.data != NULL && needs_parens(item.data);
    write_text(out, compound ? "(" : "");
    write_text(out, item.data != NULL ? item.data : "?");
    write_text(out, compound ? ")" : "");
    lz_buffer_free(&item);
}

void lz_type_describe(const LzType *type, const LzWriter *out) {
    if (type->tparam != NULL && type->kinds == LZ_KINDS_ANY) {
        write_text(out, type->tparam->data);
        write_text(out, type->tparam_nullable ? "?" : "");
        return;
    }
    if (type->kinds == LZ_KINDS_ANY && type->element == NULL) {
        write_text(out, "any");
        return;
    }
    if (type->kinds == 0 && type->literal_count == 0) {
        write_text(out, "never");
        return;
    }

    bool first = true;
    uint16_t kinds = type->kinds;
#define PART(text)                                                                                 \
    do {                                                                                           \
        if (!first) {                                                                              \
            write_text(out, " | ");                                                                \
        }                                                                                          \
        first = false;                                                                             \
        write_text(out, text);                                                                     \
    } while (0)

    if ((kinds & LZ_KINDS_NUMBER) == LZ_KINDS_NUMBER) {
        PART("number");
        kinds = (uint16_t)(kinds & ~LZ_KINDS_NUMBER);
    }
    if ((kinds & LZ_KIND_INT) != 0) {
        PART("int");
    }
    if ((kinds & LZ_KIND_FLOAT) != 0) {
        PART("float");
    }
    if ((kinds & LZ_KIND_STRING) != 0) {
        PART("string");
    }
    if ((kinds & LZ_KIND_BOOL) != 0) {
        PART("bool");
    }
    if ((kinds & LZ_KIND_NIL) != 0) {
        PART("nil");
    }
    if ((kinds & LZ_KIND_FUNCTION) != 0) {
        if (type->fn == NULL) {
            PART("function");
        } else {
            /* In a union the signature is parenthesised when its result would swallow the rest. */
            bool union_member = kinds != LZ_KIND_FUNCTION || type->literal_count != 0;
            LzBuffer signature = {0};
            LzWriter signature_out = lz_writer_from_buffer(&signature);
            write_fn(type->fn, &signature_out);
            bool wrap = union_member && signature.data != NULL && has_top_arrow(signature.data);
            if (!first) {
                write_text(out, " | ");
            }
            first = false;
            write_text(out, wrap ? "(" : "");
            write_text(out, signature.data != NULL ? signature.data : "fn");
            write_text(out, wrap ? ")" : "");
            lz_buffer_free(&signature);
        }
    }
    if ((kinds & LZ_KIND_ARRAY) != 0) {
        if (!first) {
            write_text(out, " | ");
        }
        first = false;
        if (type->element == NULL) {
            write_text(out, "any[]");
        } else {
            /* A union item type needs parentheses: (int | string)[]. */
            LzBuffer item = {0};
            LzWriter item_out = lz_writer_from_buffer(&item);
            lz_type_describe(type->element, &item_out);
            bool compound = item.data != NULL && needs_parens(item.data);
            write_text(out, compound ? "(" : "");
            write_text(out, item.data != NULL ? item.data : "?");
            write_text(out, compound ? ")[]" : "[]");
            lz_buffer_free(&item);
        }
    }
    if ((kinds & LZ_KIND_INSTANCE) != 0 && type->class_name != NULL) {
        PART(type->class_name->data); /* "any object" stays implied, like errors */
    }
    if ((kinds & LZ_KIND_ERROR) != 0 && (kinds & LZ_KIND_INSTANCE) == 0 && type->class_name != NULL) {
        PART(type->class_name->data);
    }
    if ((kinds & LZ_KIND_DICT) != 0) {
        if (!first) {
            write_text(out, " | ");
        }
        first = false;
        write_text(out, "dict");
        if (type->element != NULL || type->value_element != NULL) {
            write_text(out, "[");
            write_nested(type->element, out);
            write_text(out, ", ");
            write_nested(type->value_element, out);
            write_text(out, "]");
        }
    }
#undef PART

    for (size_t i = 0; i < type->literal_count; i++) {
        if (!first) {
            write_text(out, " | ");
        }
        first = false;
        write_literal(out, &type->literals[i]);
    }
}

void lz_value_describe(const LzValue *value, const LzWriter *out) {
    LzLiteral literal;
    if (literal_from_value(value, &literal)) {
        write_literal(out, &literal);
    } else if (value->type == LZ_VAL_OBJECT) {
        if (lz_value_is_array(value)) {
            write_text(out, "array");
        } else if (lz_value_is_dict(value)) {
            write_text(out, "dict");
        } else if (lz_value_is_instance(value)) {
            write_text(out, lz_value_as_instance(value)->cls->name->data);
        } else {
            write_text(out, "function");
        }
    } else {
        (void)lz_value_write(value, out);
    }
}
