#include <stdlib.h>

#include "lizard/stack_plan.h"
#include "lizard/vm.h"

typedef struct Position {
    size_t values;
    size_t handlers;
    bool boundary;
    bool seen;
} Position;

typedef struct Walk {
    const LzChunk *chunk;
    Position *positions;
    size_t *work;
    size_t pending;
    LzStackPlan peak;
} Walk;

static size_t read16(const uint8_t *code) {
    return ((size_t)code[0] << 8) | code[1];
}

static LzStatus edge(Walk *walk, size_t target, size_t values, size_t handlers) {
    if (target >= walk->chunk->count || !walk->positions[target].boundary) {
        return LZ_ERR_RUNTIME;
    }
    if (values > LZ_STACK_MAX || handlers > LZ_HANDLERS_MAX) {
        return LZ_ERR_STACK_OVERFLOW;
    }
    Position *position = &walk->positions[target];
    if (position->seen) {
        return position->values == values && position->handlers == handlers ? LZ_OK : LZ_ERR_RUNTIME;
    }
    position->seen = true;
    position->values = values;
    position->handlers = handlers;
    walk->work[walk->pending++] = target;
    if (values > walk->peak.values) walk->peak.values = values;
    if (handlers > walk->peak.handlers) walk->peak.handlers = handlers;
    return LZ_OK;
}

static LzStatus visit(Walk *walk, size_t at) {
    const uint8_t *code = walk->chunk->code;
    uint8_t op = code[at];
    size_t length = lz_opcode_length(op);
    size_t next = at + length;
    size_t operand = length >= 3 ? read16(code + at + 1) : 0;
    size_t values = walk->positions[at].values;
    size_t handlers = walk->positions[at].handlers;
    size_t need = 0, remove = 0, add = 0;
    bool falls = true;
    LzStatus status;

    switch (op) {
    case LZ_OP_STRING_BEGIN:
    case LZ_OP_STRING_FALLBACK:
    case LZ_OP_STRING_CHECK:
    case LZ_OP_BUILD_STRING: {
        const LzStringBuild *plan = lz_chunk_string_build(walk->chunk, (uint16_t)operand);
        if (plan == NULL) return LZ_ERR_RUNTIME;
        if (op == LZ_OP_STRING_BEGIN) {
            status = edge(walk, plan->fast, values + 1, handlers);
            if (status != LZ_OK) return status;
            add = 1;
        } else if (op == LZ_OP_STRING_FALLBACK) {
            if (values < 2) return LZ_ERR_RUNTIME;
            return edge(walk, plan->end, values - 1, handlers);
        } else if (op == LZ_OP_STRING_CHECK) {
            if (read16(code + at + 3) >= plan->count) return LZ_ERR_RUNTIME;
            need = 1;
            add = 1;
        } else {
            need = remove = plan->slots;
            add = 1;
        }
        break;
    }

    case LZ_OP_CONSTANT:
    case LZ_OP_NIL:
    case LZ_OP_TRUE:
    case LZ_OP_FALSE:
    case LZ_OP_GET_GLOBAL:
    case LZ_OP_ARRAY_NEW:
    case LZ_OP_CLOSURE:
    case LZ_OP_GET_UPVALUE:
    case LZ_OP_GET_SELF:
        add = 1;
        break;
    case LZ_OP_POP:
    case LZ_OP_DEFINE_GLOBAL:
    case LZ_OP_SET_GLOBAL:
    case LZ_OP_SET_UPVALUE:
    case LZ_OP_CLOSE_UPVALUE:
    case LZ_OP_CLASS_LINK:
        need = remove = 1;
        break;
    case LZ_OP_GET_LOCAL:
        need = operand + 1;
        add = 1;
        break;
    case LZ_OP_SET_LOCAL:
        need = operand + 2;
        remove = 1;
        break;
    case LZ_OP_CHECK_PARAM:
    case LZ_OP_CLOSE_UPVALUE_KEEP:
        need = operand + 1;
        break;
    case LZ_OP_CALL:
        need = (size_t)code[at + 1] + 1;
        remove = need - 1;
        break;
    case LZ_OP_CALL_EX:
        need = (size_t)code[at + 1] + 2;
        remove = need - 1;
        break;
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
    case LZ_OP_EQUAL:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
    case LZ_OP_ARRAY_PUSH:
    case LZ_OP_ARRAY_EXTEND:
    case LZ_OP_INDEX:
    case LZ_OP_RANGE_NEXT:
    case LZ_OP_IN:
        need = 2;
        remove = 1;
        break;
    case LZ_OP_SET_INDEX:
        need = remove = 3;
        break;
    case LZ_OP_SLICE:
    case LZ_OP_RANGE_TEST:
    case LZ_OP_RANGE_LAST:
    case LZ_OP_IN_RANGE:
        need = 3;
        remove = 2;
        break;
    case LZ_OP_SET_FIELD:
    case LZ_OP_ITER_CHECK:
        need = remove = 2;
        break;
    case LZ_OP_NEGATE:
    case LZ_OP_NOT:
    case LZ_OP_BIT_NOT:
    case LZ_OP_CHECK_TYPE:
    case LZ_OP_CHECK_RETURN:
    case LZ_OP_ITER_LENGTH:
    case LZ_OP_ERROR_IS:
    case LZ_OP_GET_FIELD:
    case LZ_OP_GET_FIELD_W:
    case LZ_OP_IS:
        need = 1;
        break;
    case LZ_OP_SWAP:
    case LZ_OP_METHOD_LOOKUP:
        need = 2;
        break;
    case LZ_OP_SUPER:
        need = code[at + 3] == 0 ? 2 : 1;
        break;
    case LZ_OP_INDEX_KEEP:
    case LZ_OP_ITER_NEXT:
        need = 2;
        add = 1;
        break;
    case LZ_OP_ITER_PAIR:
        need = 3;
        add = 1;
        break;
    case LZ_OP_NIL_TEST:
    case LZ_OP_DUP:
    case LZ_OP_GET_FIELD_KEEP:
        need = 1;
        add = 1;
        break;
    case LZ_OP_DICT:
        need = remove = operand * 2;
        add = 1;
        break;
    case LZ_OP_TRY_BEGIN:
        /* Catch starts at the saved height with one error value. The handler has already been removed by unwinding. */
        status = edge(walk, next + operand, values + 1, handlers);
        if (status != LZ_OK) return status;
        handlers++;
        break;
    case LZ_OP_TRY_END:
        if (handlers == 0) return LZ_ERR_RUNTIME;
        handlers--;
        break;
    case LZ_OP_JUMP:
        return edge(walk, next + operand, values, handlers);
    case LZ_OP_LOOP:
        if (operand > next) return LZ_ERR_RUNTIME;
        return edge(walk, next - operand, values, handlers);
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE:
        need = 1;
        status = edge(walk, next + operand, values, handlers);
        if (status != LZ_OK) return status;
        break;
    case LZ_OP_JUMP_IF_SUPPLIED:
        status = edge(walk, next + read16(code + at + 3), values, handlers);
        if (status != LZ_OK) return status;
        break;
    case LZ_OP_THROW:
        need = 1;
        falls = false;
        break;
    case LZ_OP_RETURN:
        falls = false;
        break;
    case LZ_OP_CLASS_INIT:
    case LZ_OP_ERROR_CLASS:
    case LZ_OP_EXTEND:
    case LZ_OP_IMPORT:
        break;
    default:
        return LZ_ERR_RUNTIME;
    }
    if (values < need) return LZ_ERR_RUNTIME;
    return falls ? edge(walk, next, values - remove + add, handlers) : LZ_OK;
}

LzStatus lz_chunk_stack_plan(const LzChunk *chunk, size_t parameters, LzStackPlan *plan) {
    if (chunk->count == 0 || chunk->code == NULL) return LZ_ERR_RUNTIME;
    if (chunk->count > SIZE_MAX / sizeof(Position) || chunk->count > SIZE_MAX / sizeof(size_t)) {
        return LZ_ERR_MEMORY;
    }
    Walk walk = {.chunk = chunk};
    walk.positions = calloc(chunk->count, sizeof *walk.positions);
    walk.work = malloc(chunk->count * sizeof *walk.work);
    if (walk.positions == NULL || walk.work == NULL) {
        free(walk.positions);
        free(walk.work);
        return LZ_ERR_MEMORY;
    }
    LzStatus status = LZ_OK;
    for (size_t at = 0; at < chunk->count;) {
        size_t length = lz_opcode_length(chunk->code[at]);
        if (length == 0 || length > chunk->count - at) {
            status = LZ_ERR_RUNTIME;
            break;
        }
        walk.positions[at].boundary = true;
        at += length;
    }
    if (status == LZ_OK) status = edge(&walk, 0, parameters, 0);
    while (status == LZ_OK && walk.pending != 0) {
        status = visit(&walk, walk.work[--walk.pending]);
    }
    if (status == LZ_OK) *plan = walk.peak;
    free(walk.positions);
    free(walk.work);
    return status;
}
