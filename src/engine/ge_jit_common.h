#ifndef LIZARD_GE_JIT_COMMON_H
#define LIZARD_GE_JIT_COMMON_H

/*
 * What the JIT backends share: the layout of the VM structures they poke at, and the
 * architecture-independent half of the compiler: decoding the program, the stack-height
 * analysis, and the discovery of the places control can enter (leaders).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ge_jit.h"
#include "lizard/array.h"
#include "lizard/native.h"

#define OFFS(type, field) ((int32_t)offsetof(type, field))

#define O_STACK OFFS(LzVM, stack)
#define O_SC OFFS(LzVM, stack_count)
#define O_INSTR OFFS(LzVM, instructions)
#define O_FC OFFS(LzVM, frame_count)
#define O_FRAMES OFFS(LzVM, frames)
#define O_OPENUPV OFFS(LzVM, open_upvalues)
#define O_GENT (OFFS(LzVM, globals) + OFFS(LzTable, entries))
#define O_GCNT (OFFS(LzVM, globals) + OFFS(LzTable, count))
#define O_GCAP (OFFS(LzVM, globals) + OFFS(LzTable, capacity))
#define O_RC OFFS(LzObject, refcount)
#define O_OTYPE OFFS(LzObject, type)
#define O_ARR_ITEMS OFFS(LzArray, items)
#define O_ARR_COUNT OFFS(LzArray, count)
#define O_ARR_ELEM OFFS(LzArray, element)
#define O_CL_FN OFFS(LzClosure, function)
#define O_FN_PARAMS OFFS(LzFunction, param_count)
#define O_FN_REST OFFS(LzFunction, has_rest)
#define O_FN_CACHE OFFS(LzFunction, engine_cache)
#define O_FN_CHUNK OFFS(LzFunction, chunk)
#define O_PR_JIT OFFS(GeProgram, jit)
#define O_JIT_ENTRY OFFS(struct GeJit, entry_call)
#define O_UPV_LOC OFFS(LzUpvalue, location)
#define O_CL_UPVC OFFS(LzClosure, upvalue_count)
#define O_CL_UPV OFFS(LzClosure, upvalues)
#define FS ((int32_t)sizeof(LzCallFrame))
#define FR_CLOSURE OFFS(LzCallFrame, closure)
#define FR_CHUNK OFFS(LzCallFrame, chunk)
#define FR_IP OFFS(LzCallFrame, ip)
#define FR_BASE OFFS(LzCallFrame, base)
#define FR_SUPPLIED OFFS(LzCallFrame, supplied)
#define FR_CALLOFF OFFS(LzCallFrame, call_offset)
#define X_REASON OFFS(GeJitExit, reason)
#define X_PC OFFS(GeJitExit, pc)
#define X_STATUS OFFS(GeJitExit, status)

_Static_assert(sizeof(LzValue) == 16, "the JIT assumes 16-byte values");
_Static_assert(offsetof(LzValue, as) == 8, "the JIT assumes the payload at +8");
_Static_assert(sizeof(LzValueType) == 4, "the JIT assumes a 4-byte value tag");
_Static_assert(sizeof(LzObjType) == 4, "the JIT assumes a 4-byte object type");
_Static_assert(sizeof(((LzObject *)0)->refcount) == 4, "the JIT assumes a 32-bit refcount");
_Static_assert(sizeof(((LzFunction *)0)->param_count) == 2, "the JIT assumes a 16-bit param_count");
_Static_assert(sizeof(((LzFunction *)0)->has_rest) == 1, "the JIT assumes a one-byte has_rest");
_Static_assert(sizeof(LzTableEntry) == 24, "the JIT assumes 24-byte table entries");
_Static_assert(sizeof(((GeIns *)0)->offset) == 4, "offset");

#define GE_TWO_53_I INT64_C(9007199254740992)

typedef struct JitIR {
    const LzChunk *chunk;
    const GeProgram *prog;
    size_t n;            /* instructions, not counting the sentinel */
    uint16_t *oa;        /* decoded operands of the plain instruction at each index */
    uint16_t *ob;
    int32_t *depth;      /* -1: not reached */
    uint8_t *reach;
    uint8_t *leader;     /* control can enter here from outside straight-line code */
    uint8_t *fused;      /* the fused form is used at this index */
    uint32_t *nexte;     /* next reachable index after i */
    JitSite *sites;      /* three inline caches per instruction */
    uint32_t max_depth;
} JitIR;

static inline uint16_t rd16(const uint8_t *code, size_t offset) {
    return (uint16_t)((code[offset] << 8) | code[offset + 1]);
}

static inline bool is_fused_op(uint8_t op) {
    return op >= GE_OP_BIN && op <= GE_OP_INDEX1;
}

static inline bool const_is_number(const LzChunk *chunk, uint16_t index, bool int_only) {
    if (index >= chunk->constant_count) {
        return false;
    }
    const LzValue *v = &chunk->constants[index];
    return v->type == LZ_VAL_INT || (!int_only && v->type == LZ_VAL_FLOAT);
}

/* May this operand be read in place by the fused form? (Mirrors load_operand in ge_exec.c.) */
static inline bool operand_ok(const JitIR *c, unsigned kind, uint16_t index, int d, bool int_only) {
    if (kind == GE_K_LOCAL) {
        return index < d;
    }
    if (kind == GE_K_CONST) {
        return const_is_number(c->chunk, index, int_only);
    }
    return true;
}

static inline bool reach_edge(JitIR *c, uint32_t *work, size_t *top, uint32_t to, int32_t depth, bool jump) {
    if (to > c->n || depth < 0 || depth > 4000) {
        return false;
    }
    if (jump) {
        c->leader[to] = 1;
    }
    if (c->depth[to] < 0) {
        c->depth[to] = depth;
        work[(*top)++] = to;
        if ((uint32_t)depth > c->max_depth) {
            c->max_depth = (uint32_t)depth;
        }
        return true;
    }
    return c->depth[to] == depth;
}

static inline void note_peak(JitIR *c, int32_t depth) {
    if (depth > 0 && (uint32_t)depth > c->max_depth) {
        c->max_depth = (uint32_t)depth;
    }
}

/*
 * Stack-height analysis. Every instruction that can run must see the same number of
 * values above the frame base on every path, and may not pop more than there are. The
 * compiler always produces such code; anything else (hand-made bytecode that underflows
 * the stack, say) is left to the interpreter. Also decides, per fused instruction,
 * whether its fused form is valid here (otherwise it is compiled as the plain
 * instructions it covers, exactly like the interpreter falls back).
 */
static inline bool analyse(JitIR *c, int32_t entry_depth) {
    const GeIns *ins = c->prog->ins;
    size_t n = c->n;
    uint32_t *work = malloc((n + 2) * sizeof *work);
    if (work == NULL) {
        return false;
    }
    size_t top = 0;
    bool ok = true;

    c->depth[0] = entry_depth;
    work[top++] = 0;
    c->leader[0] = 1;
    note_peak(c, entry_depth);

    while (ok && top > 0) {
        uint32_t i = work[--top];
        int32_t d = c->depth[i];
        c->reach[i] = 1;
        if (i == n) {
            continue; /* ran off the end of the chunk: the interpreter reports it */
        }
        const GeIns *in = &ins[i];
        uint8_t op = in->op;
        uint8_t plain = in->plain;

        if (is_fused_op(op)) {
            unsigned ka = (unsigned)(in->kinds & 3u);
            unsigned kb = (unsigned)((in->kinds >> 2) & 3u);
            unsigned ks = (unsigned)((in->kinds >> 4) & 3u);
            bool use = true;
            int32_t exit_depth = d;
            int peak = 2;
            switch (op) {
            case GE_OP_BIN:
                use = operand_ok(c, ka, in->a, d, false) && operand_ok(c, kb, in->b, d, false) &&
                      (ks != GE_ST_LOCAL || in->c < d);
                exit_depth = ks == GE_ST_NONE ? d + 1 : d;
                break;
            case GE_OP_BIN1:
                use = d >= 1 && operand_ok(c, kb, in->b, d, false) &&
                      (ks != GE_ST_LOCAL || in->c + 1 < d);
                exit_depth = ks == GE_ST_NONE ? d : d - 1;
                peak = 1;
                break;
            case GE_OP_CMPJ:
                use = operand_ok(c, ka, in->a, d, false) && operand_ok(c, kb, in->b, d, false);
                break;
            case GE_OP_CMPJ1:
                use = d >= 1 && operand_ok(c, kb, in->b, d, false);
                exit_depth = d - 1;
                peak = 1;
                break;
            case GE_OP_INDEX:
                use = ka != GE_K_CONST && operand_ok(c, ka, in->a, d, false) &&
                      operand_ok(c, kb, in->b, d, true);
                exit_depth = d + 1;
                break;
            default: /* GE_OP_INDEX1 */
                use = d >= 1 && operand_ok(c, kb, in->b, d, true);
                peak = 1;
                break;
            }
            if (use) {
                c->fused[i] = 1;
                note_peak(c, d + peak);
                if (op == GE_OP_CMPJ || op == GE_OP_CMPJ1) {
                    ok = reach_edge(c, work, &top, in->target, exit_depth, true) &&
                         reach_edge(c, work, &top, i + in->span, exit_depth, false);
                } else {
                    ok = reach_edge(c, work, &top, i + in->span, exit_depth, false);
                }
                continue;
            }
            /* not usable here: fall through to the plain instruction */
        }

        const uint16_t a = c->oa[i];
        int32_t nd = d;
        bool falls = true;
        bool jumps = false;
        uint32_t target = in->target;

        if (op == GE_OP_DELEGATE &&
            (plain == LZ_OP_CONSTANT || plain == LZ_OP_GET_GLOBAL || plain == LZ_OP_SET_GLOBAL)) {
            continue; /* bad operands: always the interpreter's (an error) */
        }

        switch (plain) {
        case LZ_OP_CONSTANT:
        case LZ_OP_NIL:
        case LZ_OP_TRUE:
        case LZ_OP_FALSE:
        case LZ_OP_GET_GLOBAL:
        case LZ_OP_ARRAY_NEW:
        case LZ_OP_CLOSURE:
        case LZ_OP_GET_UPVALUE:
        case LZ_OP_GET_SELF:
            nd = d + 1;
            break;
        case LZ_OP_POP:
        case LZ_OP_DEFINE_GLOBAL:
        case LZ_OP_SET_GLOBAL:
        case LZ_OP_SET_UPVALUE:
        case LZ_OP_CLOSE_UPVALUE:
            ok = d >= 1;
            nd = d - 1;
            break;
        case LZ_OP_CALL:
            ok = d >= (int32_t)a + 1;
            nd = d - (int32_t)a;
            break;
        case LZ_OP_CALL_EX: {
            int32_t named = (int32_t)(a);
            ok = d >= named + 2;
            nd = d - (named + 1);
            break;
        }
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
        case LZ_OP_INDEX:
        case LZ_OP_ARRAY_PUSH:
        case LZ_OP_ARRAY_EXTEND:
            ok = d >= 2;
            nd = d - 1;
            break;
        case LZ_OP_NEGATE:
        case LZ_OP_NOT:
        case LZ_OP_CHECK_TYPE:
        case LZ_OP_CHECK_RETURN:
            ok = d >= 1;
            break;
        case LZ_OP_GET_LOCAL:
            ok = a < d;
            nd = d + 1;
            break;
        case LZ_OP_SET_LOCAL:
            ok = d >= 1 && a < d - 1;
            nd = d - 1;
            break;
        case LZ_OP_CHECK_PARAM:
            ok = a < d;
            break;
        case LZ_OP_SET_INDEX:
            ok = d >= 3;
            nd = d - 3;
            break;
        case LZ_OP_SLICE:
            ok = d >= 3;
            nd = d - 2;
            break;
        case LZ_OP_JUMP:
        case LZ_OP_LOOP:
            falls = false;
            jumps = true;
            break;
        case LZ_OP_JUMP_IF_FALSE:
        case LZ_OP_JUMP_IF_TRUE:
            ok = d >= 1;
            jumps = true;
            break;
        case LZ_OP_JUMP_IF_SUPPLIED:
            jumps = true;
            break;
        case LZ_OP_RETURN:
            falls = false;
            break;
        default:
            ok = false; /* an opcode this analysis does not know */
            break;
        }
        if (!ok) {
            break;
        }
        note_peak(c, nd);
        if (plain == LZ_OP_CALL || plain == LZ_OP_CALL_EX) {
            c->leader[i + 1] = 1; /* a frame resumes here */
        }
        if (jumps) {
            ok = reach_edge(c, work, &top, target, nd, true);
        }
        if (ok && falls) {
            ok = reach_edge(c, work, &top, i + 1, nd, false);
        }
    }
    free(work);
    return ok;
}

static inline void jit_ir_free(JitIR *ir) {
    free(ir->oa);
    free(ir->ob);
    free(ir->depth);
    free(ir->reach);
    free(ir->leader);
    free(ir->fused);
    free(ir->nexte);
    free(ir->sites);
    memset(ir, 0, sizeof *ir);
}

/* Decodes and analyses a program. False when it is not of a shape the JIT handles. */
static inline bool jit_ir_build(JitIR *c, const LzChunk *chunk, const GeProgram *program,
                                size_t entry_depth) {
    const size_t n = program->count - 1;
    memset(c, 0, sizeof *c);
    c->chunk = chunk;
    c->prog = program;
    c->n = n;
    c->oa = calloc(n + 1, sizeof *c->oa);
    c->ob = calloc(n + 1, sizeof *c->ob);
    c->depth = malloc((n + 1) * sizeof *c->depth);
    c->reach = calloc(n + 1, 1);
    c->leader = calloc(n + 1, 1);
    c->fused = calloc(n + 1, 1);
    c->nexte = calloc(n + 1, sizeof *c->nexte);
    c->sites = calloc(3 * (n + 1), sizeof *c->sites);
    if (c->oa == NULL || c->ob == NULL || c->depth == NULL || c->reach == NULL || c->leader == NULL ||
        c->fused == NULL || c->nexte == NULL || c->sites == NULL) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        c->depth[i] = -1;
    }
    for (size_t i = 0; i < 3 * (n + 1); i++) {
        c->sites[i].hint = GE_NO_HINT;
        c->sites[i].bhint = GE_NO_HINT;
        c->sites[i].count = GE_NO_HINT;
    }
    for (size_t i = 0; i < n; i++) {
        const GeIns *in = &program->ins[i];
        size_t length = lz_opcode_length(in->plain);
        if (length == 0 || in->offset + length > chunk->count) {
            return false;
        }
        if (length == 3 || length == 5) {
            c->oa[i] = rd16(chunk->code, in->offset + 1);
        } else if (length == 2 || length == 4) {
            c->oa[i] = chunk->code[in->offset + 1];
        }
        if (length == 5) {
            c->ob[i] = rd16(chunk->code, in->offset + 3);
        } else if (length == 4) {
            c->ob[i] = rd16(chunk->code, in->offset + 2);
        }
    }
    if (!analyse(c, (int32_t)entry_depth)) {
        return false;
    }
    /* A fused instruction whose covered slots can also be reached must leave the counter
     * in order at the slot after the group. */
    for (size_t i = 0; i < n; i++) {
        if (c->reach[i] && c->fused[i]) {
            const GeIns *in = &program->ins[i];
            for (uint32_t k = 1; k < in->span; k++) {
                if (c->reach[i + k]) {
                    c->leader[i + in->span] = 1;
                    break;
                }
            }
        }
    }
    uint32_t later = (uint32_t)n + 1;
    for (size_t i = n + 1; i-- > 0;) {
        c->nexte[i] = later;
        if (c->reach[i]) {
            later = (uint32_t)i;
        }
    }
    return true;
}

#endif
