#include <stdlib.h>
#include <string.h>

#include "ge_internal.h"

/*
 * Bytecode -> GE IR. Two things happen here:
 *
 *  1. Every instruction is decoded once (operands, jump targets), so the
 *     executor never re-reads bytes or re-checks operand ranges.
 *  2. Runs of instructions that almost every program contains are marked as one
 *     fused instruction ("superinstruction"). The IR stays 1:1 with the bytecode:
 *     the fused instruction replaces only the first slot of the run, and the
 *     slots it covers keep their plain form, so the executor can always fall
 *     back to plain execution at the same index. Fusion never changes behaviour,
 *     it only removes dispatches and stack traffic.
 *
 * A jump into the middle of a fused run is harmless for the same reason: it
 * lands on the plain instruction in that slot and runs the rest of the run plain.
 */

static bool is_load_op(uint8_t op) {
    return op == LZ_OP_CONSTANT || op == LZ_OP_GET_LOCAL || op == LZ_OP_GET_GLOBAL;
}

static bool is_arith_op(uint8_t op) {
    return op == LZ_OP_ADD || op == LZ_OP_SUBTRACT || op == LZ_OP_MULTIPLY || op == LZ_OP_DIVIDE ||
           op == LZ_OP_FLOOR_DIVIDE || op == LZ_OP_MODULO;
}

static bool is_compare_op(uint8_t op) {
    return op == LZ_OP_LESS || op == LZ_OP_GREATER || op == LZ_OP_LESS_EQUAL ||
           op == LZ_OP_GREATER_EQUAL || op == LZ_OP_EQUAL;
}

static uint16_t read_u16(const uint8_t *code, size_t offset) {
    return (uint16_t)((code[offset] << 8) | code[offset + 1]);
}

static bool is_name_constant(const LzChunk *chunk, uint16_t index) {
    if (index >= chunk->constant_count) {
        return false;
    }
    const LzValue *constant = &chunk->constants[index];
    return constant->type == LZ_VAL_OBJECT && constant->as.object->type == LZ_OBJ_STRING;
}

/* "Still plain": not replaced by GE_OP_DELEGATE (bad operands) or by a fused instruction. */
static bool plain_is(const GeIns *in, uint8_t op) {
    return in->op == op && in->plain == op;
}

static bool is_load(const GeIns *in) {
    return in->op == in->plain && is_load_op(in->plain);
}

static bool is_arith(const GeIns *in) {
    return in->op == in->plain && is_arith_op(in->plain);
}

static bool is_compare(const GeIns *in) {
    return in->op == in->plain && is_compare_op(in->plain);
}

static bool is_store(const GeIns *in) {
    return in->op == in->plain && (in->plain == LZ_OP_SET_LOCAL || in->plain == LZ_OP_SET_GLOBAL);
}

static unsigned load_kind(uint8_t plain) {
    if (plain == LZ_OP_GET_LOCAL) {
        return GE_K_LOCAL;
    }
    return plain == LZ_OP_GET_GLOBAL ? GE_K_GLOBAL : GE_K_CONST;
}

static unsigned store_kind(uint8_t plain) {
    return plain == LZ_OP_SET_LOCAL ? GE_ST_LOCAL : GE_ST_GLOBAL;
}

static void fuse(GeIns *in, uint8_t op, uint8_t sub, unsigned kind_a, unsigned kind_b,
                 unsigned kind_store, uint16_t a, uint16_t b, uint16_t c, size_t span,
                 uint32_t target) {
    in->op = op;
    in->sub = sub;
    in->kinds = (uint8_t)(kind_a | (kind_b << 2) | (kind_store << 4));
    in->a = a;
    in->b = b;
    in->c = c;
    in->span = (uint16_t)span;
    in->target = target;
}

/* Fuses the run starting at k if it matches a pattern. Returns true when it did. */
static bool try_fuse(GeIns *ins, size_t count, size_t k) {
    GeIns *in = &ins[k];
    if (!is_load(in)) {
        return false;
    }
    const unsigned kind_b0 = load_kind(in->plain);

    /* [load a] [load b] CMP JUMP_IF_FALSE POP: `while i < n`, `if x > 0` */
    if (k + 5 <= count && is_load(&ins[k + 1]) && is_compare(&ins[k + 2]) &&
        plain_is(&ins[k + 3], LZ_OP_JUMP_IF_FALSE) && plain_is(&ins[k + 4], LZ_OP_POP)) {
        uint32_t miss = ins[k + 3].target;
        /* Both outcomes pop the condition, so the false path may skip the POP it jumps to. */
        if (miss < count && ins[miss].plain == LZ_OP_POP) {
            fuse(in, GE_OP_CMPJ, ins[k + 2].plain, load_kind(in->plain), load_kind(ins[k + 1].plain),
                 GE_ST_NONE, in->a, ins[k + 1].a, 0, 5, miss + 1);
            return true;
        }
    }

    /* [load b] CMP JUMP_IF_FALSE POP, the left operand already on the stack: `n % 2 == 0` */
    if (k + 4 <= count && is_compare(&ins[k + 1]) && plain_is(&ins[k + 2], LZ_OP_JUMP_IF_FALSE) &&
        plain_is(&ins[k + 3], LZ_OP_POP)) {
        uint32_t miss = ins[k + 2].target;
        if (miss < count && ins[miss].plain == LZ_OP_POP) {
            fuse(in, GE_OP_CMPJ1, ins[k + 1].plain, GE_K_LOCAL, kind_b0, GE_ST_NONE, 0, in->a, 0, 4,
                 miss + 1);
            return true;
        }
    }

    /* [load a] [load b] ARITH [SET_LOCAL | SET_GLOBAL]: `i + 1`, `i = i + 1`, `a * b` */
    if (k + 3 <= count && is_load(&ins[k + 1]) && is_arith(&ins[k + 2])) {
        size_t span = 3;
        unsigned kind_s = GE_ST_NONE;
        uint16_t target = 0;
        if (k + 4 <= count && is_store(&ins[k + 3])) {
            span = 4;
            kind_s = store_kind(ins[k + 3].plain);
            target = ins[k + 3].a;
        }
        fuse(in, GE_OP_BIN, ins[k + 2].plain, load_kind(in->plain), load_kind(ins[k + 1].plain),
             kind_s, in->a, ins[k + 1].a, target, span, 0);
        return true;
    }

    /* [load b] ARITH [store], the left operand already on the stack: `x * 2 + 1` */
    if (k + 2 <= count && is_arith(&ins[k + 1])) {
        size_t span = 2;
        unsigned kind_s = GE_ST_NONE;
        uint16_t target = 0;
        if (k + 3 <= count && is_store(&ins[k + 2])) {
            span = 3;
            kind_s = store_kind(ins[k + 2].plain);
            target = ins[k + 2].a;
        }
        fuse(in, GE_OP_BIN1, ins[k + 1].plain, GE_K_LOCAL, kind_b0, kind_s, 0, in->a, target, span, 0);
        return true;
    }

    /* [load a] [load b] INDEX: `xs[i]` */
    if (k + 3 <= count && is_load(&ins[k + 1]) && plain_is(&ins[k + 2], LZ_OP_INDEX)) {
        fuse(in, GE_OP_INDEX, 0, load_kind(in->plain), load_kind(ins[k + 1].plain), GE_ST_NONE, in->a,
             ins[k + 1].a, 0, 3, 0);
        return true;
    }

    /* [load b] INDEX, the array already on the stack: `grid[i][j]` */
    if (k + 2 <= count && plain_is(&ins[k + 1], LZ_OP_INDEX)) {
        fuse(in, GE_OP_INDEX1, 0, GE_K_LOCAL, kind_b0, GE_ST_NONE, 0, in->a, 0, 2, 0);
        return true;
    }

    return false;
}

void ge_cache_free(void *cache) {
    GeProgram *program = cache;
    ge_program_free(program);
    free(program);
}

void ge_program_free(GeProgram *program) {
    free(program->ins);
    program->ins = NULL;
    program->count = 0;
    program->fused_sites = 0;
}

bool ge_translate(const LzChunk *chunk, GeProgram *program) {
    memset(program, 0, sizeof *program);
    const size_t bytes = chunk->count;
    if (bytes >= UINT32_MAX) {
        return false;
    }

    /* Pass 1: find the instruction boundaries. */
    size_t count = 0;
    for (size_t offset = 0; offset < bytes;) {
        size_t length = lz_opcode_length(chunk->code[offset]);
        if (length == 0 || offset + length > bytes) {
            return false;
        }
        count++;
        offset += length;
    }

    GeIns *ins = calloc(count + 1, sizeof *ins);
    uint32_t *index_of = malloc((bytes + 1) * sizeof *index_of);
    if (ins == NULL || index_of == NULL) {
        free(ins);
        free(index_of);
        return false;
    }
    for (size_t i = 0; i <= bytes; i++) {
        index_of[i] = GE_NO_HINT; /* not an instruction boundary */
    }

    /* Pass 2: decode. */
    size_t k = 0;
    for (size_t offset = 0; offset < bytes; k++) {
        uint8_t op = chunk->code[offset];
        size_t length = lz_opcode_length(op);
        GeIns *in = &ins[k];
        in->op = op;
        in->plain = op;
        in->span = 1;
        in->offset = (uint32_t)offset;
        in->hint_a = GE_NO_HINT;
        in->hint_b = GE_NO_HINT;
        in->hint_c = GE_NO_HINT;
        if (length == 3) {
            in->a = read_u16(chunk->code, offset + 1); /* every 3-byte instruction has a u16 */
        } else if (length == 2) {
            in->a = chunk->code[offset + 1]; /* OP_CALL's argument count */
        } else if (length == 4) {
            in->a = chunk->code[offset + 1]; /* OP_CALL_EX: named count, then the names constant */
            in->b = read_u16(chunk->code, offset + 2);
        } else if (length == 5) {
            in->a = read_u16(chunk->code, offset + 1); /* two u16 operands */
            in->b = read_u16(chunk->code, offset + 3);
        }
        in->target = GE_NO_HINT; /* GET_GLOBAL: no builtin cached yet (jumps overwrite this) */
        index_of[offset] = (uint32_t)k;
        offset += length;
    }
    index_of[bytes] = (uint32_t)count;
    ins[count].op = GE_OP_END;
    ins[count].plain = GE_OP_END;
    ins[count].span = 1;
    ins[count].offset = (uint32_t)bytes;

    /* Pass 3: resolve jumps. A jump that does not land on a boundary is garbage the
     * reference interpreter must define, so the whole chunk is left to it. */
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        GeIns *in = &ins[i];
        size_t after = (size_t)in->offset + lz_opcode_length(in->plain);
        size_t destination;
        if (in->plain == LZ_OP_JUMP || in->plain == LZ_OP_JUMP_IF_FALSE ||
            in->plain == LZ_OP_JUMP_IF_TRUE) {
            destination = after + in->a;
        } else if (in->plain == LZ_OP_JUMP_IF_SUPPLIED) {
            destination = after + in->b;
        } else if (in->plain == LZ_OP_LOOP) {
            if (in->a > after) {
                ok = false;
                break;
            }
            destination = after - in->a;
        } else {
            continue;
        }
        if (destination > bytes || index_of[destination] == GE_NO_HINT) {
            ok = false;
            break;
        }
        in->target = index_of[destination];
    }
    free(index_of);
    if (!ok) {
        free(ins);
        return false;
    }

    /* Pass 4: instructions that need the reference interpreter, or whose operands are
     * not what the fast path assumes, are delegated as they are. */
    for (size_t i = 0; i < count; i++) {
        GeIns *in = &ins[i];
        switch (in->plain) {
        case LZ_OP_CONSTANT:
            if (in->a >= chunk->constant_count) {
                in->op = GE_OP_DELEGATE;
            }
            break;
        case LZ_OP_GET_GLOBAL:
        case LZ_OP_SET_GLOBAL:
            if (!is_name_constant(chunk, in->a)) {
                in->op = GE_OP_DELEGATE;
            }
            break;
        case LZ_OP_DEFINE_GLOBAL:
        case LZ_OP_CHECK_TYPE:
        case LZ_OP_ARRAY_NEW:
        case LZ_OP_ARRAY_PUSH:
        case LZ_OP_SLICE:
        /* Functions: none of these changes the frame stack, so the reference runs them. */
        case LZ_OP_CLOSURE:
        case LZ_OP_GET_UPVALUE:
        case LZ_OP_SET_UPVALUE:
        case LZ_OP_CLOSE_UPVALUE:
        case LZ_OP_GET_SELF:
        case LZ_OP_CHECK_PARAM:
        case LZ_OP_CHECK_RETURN:
        case LZ_OP_ARRAY_EXTEND:
            in->op = GE_OP_DELEGATE;
            break;
        default:
            break;
        }
    }

    /* Pass 5: fuse. */
    size_t fused_sites = 0;
    for (size_t i = 0; i < count; i++) {
        if (try_fuse(ins, count, i)) {
            fused_sites++;
        }
    }

    program->ins = ins;
    program->count = count + 1;
    program->fused_sites = fused_sites;
    return true;
}
