/*
 * The Gecko JIT, x86-64 backend (System V ABI). See docs/ENGINE.md.
 *
 * Model
 * -----
 * Native code runs on the VM's own value stack and frame stack. Values stay in
 * memory (vm->stack); the JIT knows statically how many values each instruction
 * has above the frame base ("depth"), so locals and temporaries are plain
 * [r14 + 16 * slot] operands. Registers:
 *
 *   rbx  the LzVM            r12  the GeJitExit of this entry
 *   r13  base index of the frame     r14  &vm->stack[base]
 *   r15  instructions executed since entry (flushed to vm->instructions on exit)
 *
 * Instructions are compiled the way GE fuses them (see ge_translate.c): `i = i + 1`
 * is one block of machine code that reads its operands in place, checks their
 * types, computes and stores. Whenever a check fails (a string, an overflow, an
 * out of range index, ...) the code "bails out": it leaves the state exactly as it
 * was before the instruction and returns to the interpreter, which runs that
 * instruction itself (and produces every error message). Nothing is changed before
 * every check of an instruction has passed, so a bail-out is always exact.
 *
 * Instructions the JIT does not inline are executed by calling the reference
 * semantics (lz_vm_exec_one); calls to user functions push the frame inline and
 * use a native `call`, so recursion stays in machine code.
 *
 * Fuel: native code is only ever entered while fuel is unlimited (the interpreter
 * checks), so there is nothing to meter; vm->instructions is kept exact.
 */

#define _DEFAULT_SOURCE

#include "ge_jit.h"

#if GE_JIT_X64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_jit_common.h"
#include "ge_jit_rt.h"
#include "ge_x64.h"


/* ---------------------------------------------------------------------------------
 * The entry trampoline
 * ------------------------------------------------------------------------------- */

typedef uint32_t (*TrampolineFn)(LzVM *vm, size_t base_index, LzValue *base_ptr, void *target,
                                 GeJitExit *exit);

static TrampolineFn g_tramp;
static void *g_exit_thunk;
static bool g_tramp_failed;

/*
 * enter(vm, base_index, base_ptr, target, exit): saves the callee-saved registers,
 * sets up rbx/r12-r15, and jumps to `target` laid out as the body of a native
 * function (a pad word and a return address on the stack). Native code leaves by
 * `ret` (a function returned) or by the exit thunk (everything else).
 */
static bool tramp_init(void) {
    if (g_tramp != NULL) {
        return true;
    }
    if (g_tramp_failed) {
        return false;
    }
    X64 x;
    x64_init(&x);
    int after = x_label(&x);
    int thunk = x_label(&x);
    int skip = x_label(&x);

    push_r(&x, RBP);
    push_r(&x, RBX);
    push_r(&x, R12);
    push_r(&x, R13);
    push_r(&x, R14);
    push_r(&x, R15);
    alu_ri(&x, 1, ALU_SUB, RSP, 8);
    mov_rr(&x, RBX, RDI);
    mov_rr(&x, R13, RSI);
    mov_rr(&x, R14, RDX);
    mov_rr(&x, R12, R8);
    alu_rr(&x, ALU_XOR, R15, R15);
    mov_mr(&x, R12, OFFS(GeJitExit, saved_rsp), RSP);
    mov_mi32(&x, R12, X_REASON, GE_JX_RETURNED);
    lea_label(&x, RAX, after);
    push_r(&x, RAX);
    alu_ri(&x, 1, ALU_SUB, RSP, 8);
    jmp_r(&x, RCX);

    x_bind(&x, thunk);
    mov_rm(&x, RSP, R12, OFFS(GeJitExit, saved_rsp));
    x_bind(&x, after);
    alu_mr(&x, ALU_ADD, RBX, O_INSTR, R15);
    mov_rm32(&x, RAX, R12, X_REASON);
    alu_ri(&x, 0, ALU_CMP, RAX, GE_JX_RETURNED);
    jcc_l(&x, CC_NE, skip);
    mov_mr(&x, RBX, O_SC, R13);
    x_bind(&x, skip);
    alu_ri(&x, 1, ALU_ADD, RSP, 8);
    pop_r(&x, R15);
    pop_r(&x, R14);
    pop_r(&x, R13);
    pop_r(&x, R12);
    pop_r(&x, RBX);
    pop_r(&x, RBP);
    ret(&x);

    size_t size = x64_size(&x);
    uint8_t *image = malloc(size);
    bool ok = image != NULL && x64_link(&x, image);
    size_t mapped = 0;
    uint8_t *memory = ok ? jit_exec_alloc(image, size, &mapped) : NULL;
    size_t thunk_at = x64_label_address(&x, thunk);
    free(image);
    x64_free(&x);
    if (memory == NULL) {
        g_tramp_failed = true;
        return false;
    }
    g_tramp = (TrampolineFn)(uintptr_t)memory;
    g_exit_thunk = memory + thunk_at;
    return true;
}


bool ge_jit_supported(void) {
    return tramp_init();
}

bool ge_jit_enter(LzVM *vm, const struct GeJit *jit, size_t pc, size_t base, size_t sp,
                  GeJitExit *exit) {
    if (pc >= jit->count || jit->entry[pc] == NULL || sp < base ||
        (int64_t)(sp - base) != jit->depth[pc] || base + jit->max_depth > LZ_STACK_MAX) {
        return false;
    }
    exit->reason = 0;
    exit->pc = 0;
    exit->status = 0;
    (void)g_tramp(vm, base, &vm->stack[base], jit->entry[pc], exit);
    return true;
}

/* ---------------------------------------------------------------------------------
 * Compilation
 * ------------------------------------------------------------------------------- */

typedef struct Cx {
    X64 x;
    JitIR ir;
    int *lab;            /* instruction -> label */
    /* emission state */
    size_t i;
    int d;
    int pend;
    int bail_lbl;
    int fail_lbl;
} Cx;

#define X (&c->x)

/* ---- emission helpers ---- */

static void emit_flush(Cx *c) {
    if (c->pend != 0) {
        lea(X, R15, R15, c->pend);
        c->pend = 0;
    }
}

static void emit_call_abs(Cx *c, uintptr_t fn) {
    mov_ri(X, RAX, (uint64_t)fn);
    call_r(X, RAX);
}

/* The exit sequence shared by every stub: reason and pc into the exit block, then the thunk. */
static void emit_leave(Cx *c, uint32_t reason, uint32_t pc) {
    mov_mi32(X, R12, X_PC, (int32_t)pc);
    mov_mi32(X, R12, X_REASON, (int32_t)reason);
    mov_ri(X, RAX, (uint64_t)(uintptr_t)g_exit_thunk);
    jmp_r(X, RAX);
}

/* A cold stub that returns to the interpreter at instruction pc, with `depth` values on the stack. */
static void emit_bail_stub(Cx *c, uint32_t pc, int depth, int pend) {
    if (pend != 0) {
        lea(X, R15, R15, pend);
    }
    lea(X, RAX, R13, depth);
    mov_mr(X, RBX, O_SC, RAX);
    emit_leave(c, GE_JX_BAIL, pc);
}

static int bail_label(Cx *c) {
    if (c->bail_lbl >= 0) {
        return c->bail_lbl;
    }
    int l = x_label(X);
    c->bail_lbl = l;
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, l);
    emit_bail_stub(c, (uint32_t)c->i, c->d, c->pend);
    x_section(X, saved);
    return l;
}

/* A stub for a failed call into the shared machinery: the status is in eax. */
static int fail_label(Cx *c) {
    if (c->fail_lbl >= 0) {
        return c->fail_lbl;
    }
    int l = x_label(X);
    c->fail_lbl = l;
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, l);
    mov_mr32(X, R12, X_STATUS, RAX);
    emit_leave(c, GE_JX_FAIL, (uint32_t)c->i);
    x_section(X, saved);
    return l;
}

static void emit_sync_sc(Cx *c, int depth) {
    lea(X, RAX, R13, depth);
    mov_mr(X, RBX, O_SC, RAX);
}

/* Release the object in rcx (refcount already known to be <= 1): the real release. */
static void emit_drop_slow(Cx *c, int skip, int slow, bool keep_pair) {
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    if (keep_pair) {
        push_r(X, RAX);
        push_r(X, RDX);
    }
    mov_rr(X, RSI, RCX);
    mov_rr(X, RDI, RBX);
    emit_call_abs(c, (uintptr_t)jit_drop);
    if (keep_pair) {
        pop_r(X, RDX);
        pop_r(X, RAX);
    }
    jmp_l(X, skip);
    x_section(X, saved);
}

/* Drop the value at [base+off] when it is an object. keep_pair preserves rax and rdx. */
static void emit_drop_mem(Cx *c, int base, int32_t off, bool keep_pair) {
    int skip = x_label(X);
    int slow = x_label(X);
    alu_mi(X, 0, ALU_CMP, base, off, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, skip);
    mov_rm(X, RCX, base, off + 8);
    alu_mi(X, 0, ALU_CMP, RCX, O_RC, 1);
    jcc_l(X, CC_BE, slow);
    dec_m32(X, RCX, O_RC);
    x_bind(X, skip);
    emit_drop_slow(c, skip, slow, keep_pair);
}

/* Drop the object whose pointer is in `reg`, a reference nobody will read again. */
static void emit_drop_obj(Cx *c, int reg) {
    int skip = x_label(X);
    int slow = x_label(X);
    alu_mi(X, 0, ALU_CMP, reg, O_RC, 1);
    jcc_l(X, CC_BE, slow);
    dec_m32(X, reg, O_RC);
    x_bind(X, skip);
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    mov_rr(X, RSI, reg);
    mov_rr(X, RDI, RBX);
    emit_call_abs(c, (uintptr_t)jit_drop);
    jmp_l(X, skip);
    x_section(X, saved);
}

/* Stores rdx (tag) and rax (payload) at [base+off]; an object that was there is released. */
static void emit_store_drop(Cx *c, int base, int32_t off) {
    int done = x_label(X);
    int slow = x_label(X);
    alu_mi(X, 0, ALU_CMP, base, off, LZ_VAL_OBJECT);
    jcc_l(X, CC_E, slow);
    mov_mr(X, base, off, RDX);
    mov_mr(X, base, off + 8, RAX);
    x_bind(X, done);
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    mov_rm(X, RSI, base, off + 8);
    mov_mr(X, base, off, RDX);
    mov_mr(X, base, off + 8, RAX);
    mov_rr(X, RDI, RBX);
    emit_call_abs(c, (uintptr_t)jit_drop);
    jmp_l(X, done);
    x_section(X, saved);
}

/* Copies a value (rax = tag word, rdx = payload) and takes a reference when it is an object. */
static void emit_grab_pair(Cx *c) {
    int skip = x_label(X);
    alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, skip);
    inc_m32(X, RDX, O_RC);
    x_bind(X, skip);
}

static void emit_copy_grab(Cx *c, int sb, int32_t so, int db, int32_t dof) {
    mov_rm(X, RAX, sb, so);
    mov_rm(X, RDX, sb, so + 8);
    mov_mr(X, db, dof, RAX);
    mov_mr(X, db, dof + 8, RDX);
    emit_grab_pair(c);
}

/* ---- operands ---- */

enum { OPD_MEM, OPD_INT, OPD_FLT };

typedef struct Opd {
    int kind;
    int base;
    int32_t disp;
    int64_t iv;
    double fv;
} Opd;

static void emit_global(Cx *c, int reg, uint16_t name_index, JitSite *site, bool builtin_ok, int bail) {
    const LzObject *name = c->ir.chunk->constants[name_index].as.object;
    int slow = x_label(X);
    int done = x_label(X);
    mov_ri(X, RCX, (uint64_t)(uintptr_t)site);
    mov_rm32(X, RAX, RCX, 0);
    alu_rm(X, ALU_CMP, RAX, RBX, O_GCAP);
    jcc_l(X, CC_AE, slow);
    imul_rri(X, RAX, RAX, (int32_t)sizeof(LzTableEntry));
    alu_rm(X, ALU_ADD, RAX, RBX, O_GENT);
    mov_ri(X, RDX, (uint64_t)(uintptr_t)name);
    alu_rm(X, ALU_CMP, RDX, RAX, 0);
    jcc_l(X, CC_NE, slow);
    lea(X, reg, RAX, OFFS(LzTableEntry, value));
    x_bind(X, done);

    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    push_r(X, R8);
    push_r(X, R9);
    push_r(X, R10);
    push_r(X, R11);
    mov_rr(X, RDI, RBX);
    mov_ri(X, RSI, (uint64_t)(uintptr_t)name);
    mov_ri(X, RDX, (uint64_t)(uintptr_t)site);
    emit_call_abs(c, builtin_ok ? (uintptr_t)jit_global_ptr : (uintptr_t)jit_global_cell);
    pop_r(X, R11);
    pop_r(X, R10);
    pop_r(X, R9);
    pop_r(X, R8);
    test_rr(X, RAX, RAX);
    jcc_l(X, CC_E, bail);
    if (reg != RAX) {
        mov_rr(X, reg, RAX);
    }
    jmp_l(X, done);
    x_section(X, saved);
}

/* Where an assignment to an existing global goes (ge_exec.c: global_store). */
static void emit_global_store_cell(Cx *c, int reg, uint16_t name_index, JitSite *site, int bail) {
    mov_rm(X, RAX, RBX, O_GCAP);
    imul_rri(X, RAX, RAX, 3);
    shift_ri(X, 5, RAX, 2);
    mov_rm(X, RCX, RBX, O_GCNT);
    inc_r(X, RCX);
    alu_rr(X, ALU_CMP, RCX, RAX);
    jcc_l(X, CC_A, bail);
    emit_global(c, reg, name_index, site, false, bail);
}

static void prep_opd(Cx *c, Opd *o, unsigned kind, uint16_t index, int site_slot, int ptr_reg, int bail) {
    memset(o, 0, sizeof *o);
    if (kind == GE_K_LOCAL) {
        o->kind = OPD_MEM;
        o->base = R14;
        o->disp = 16 * (int32_t)index;
    } else if (kind == GE_K_CONST) {
        const LzValue *v = &c->ir.chunk->constants[index];
        if (v->type == LZ_VAL_INT) {
            o->kind = OPD_INT;
            o->iv = v->as.integer;
        } else {
            o->kind = OPD_FLT;
            o->fv = v->as.number;
        }
    } else {
        emit_global(c, ptr_reg, index, &c->ir.sites[3 * c->i + (size_t)site_slot], false, bail);
        o->kind = OPD_MEM;
        o->base = ptr_reg;
        o->disp = 0;
    }
}

static Opd stack_opd(int slot) {
    Opd o;
    memset(&o, 0, sizeof o);
    o.kind = OPD_MEM;
    o.base = R14;
    o.disp = 16 * slot;
    return o;
}

static uint64_t double_bits(double v) {
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

/* xmm = the operand as a double; jumps to bail when it is not a number. */
static void load_double(Cx *c, const Opd *o, int xmm, int bail, bool exact) {
    if (o->kind == OPD_INT) {
        mov_ri(X, RAX, (uint64_t)o->iv);
        cvtsi2sd_xr(X, xmm, RAX);
    } else if (o->kind == OPD_FLT) {
        mov_ri(X, RAX, double_bits(o->fv));
        movq_xr(X, xmm, RAX);
    } else {
        int not_float = x_label(X);
        int done = x_label(X);
        mov_rm32(X, RAX, o->base, o->disp);
        alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_FLOAT);
        jcc_l(X, CC_NE, not_float);
        movsd_xm(X, xmm, o->base, o->disp + 8);
        jmp_l(X, done);
        x_bind(X, not_float);
        alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_INT);
        jcc_l(X, CC_NE, bail);
        if (exact) {
            /* an int must convert without rounding: -2^53 <= v <= 2^53 */
            mov_rm(X, RAX, o->base, o->disp + 8);
            mov_ri(X, RCX, (uint64_t)GE_TWO_53_I);
            alu_rr(X, ALU_ADD, RAX, RCX);
            mov_ri(X, RCX, (uint64_t)GE_TWO_53_I * 2u);
            alu_rr(X, ALU_CMP, RAX, RCX);
            jcc_l(X, CC_A, bail);
        }
        cvtsi2sd_xm(X, xmm, o->base, o->disp + 8);
        x_bind(X, done);
    }
}

static void load_int(Cx *c, const Opd *o, int reg) {
    if (o->kind == OPD_INT) {
        mov_ri(X, reg, (uint64_t)o->iv);
    } else {
        mov_rm(X, reg, o->base, o->disp + 8);
    }
}

static bool fits32(int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX;
}

static int log2_exact(int64_t v) {
    if (v <= 0 || (v & (v - 1)) != 0) {
        return -1;
    }
    int k = 0;
    while (((int64_t)1 << k) != v) {
        k++;
    }
    return k;
}

/* rax = x op y on ints (y in rcx unless a constant); the result tag goes to rdx. */
static void emit_int_op(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    bool bconst = b->kind == OPD_INT;
    load_int(c, a, RAX);
    switch (sub) {
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT: {
        int op = sub == LZ_OP_ADD ? ALU_ADD : ALU_SUB;
        if (bconst && fits32(b->iv)) {
            alu_ri(X, 1, op, RAX, (int32_t)b->iv);
        } else {
            load_int(c, b, RCX);
            alu_rr(X, op, RAX, RCX);
        }
        jcc_l(X, CC_O, bail);
        break;
    }
    case LZ_OP_MULTIPLY:
        load_int(c, b, RCX);
        imul_rr(X, RAX, RCX);
        jcc_l(X, CC_O, bail);
        break;
    case LZ_OP_DIVIDE:
        load_int(c, b, RCX);
        if (!bconst || b->iv == 0) {
            test_rr(X, RCX, RCX);
            jcc_l(X, CC_E, bail);
        }
        cvtsi2sd_xr(X, 0, RAX);
        cvtsi2sd_xr(X, 1, RCX);
        sse_arith(X, SSE_DIV, 0, 1);
        movq_rx(X, RAX, 0);
        mov_ri(X, RDX, LZ_VAL_FLOAT);
        return;
    case LZ_OP_MODULO:
    case LZ_OP_FLOOR_DIVIDE: {
        bool modulo = sub == LZ_OP_MODULO;
        if (bconst) {
            if (b->iv <= 0) {
                jmp_l(X, bail); /* zero, -1 and negative divisors are the reference's */
                break;
            }
            int k = log2_exact(b->iv);
            if (modulo && k >= 0 && fits32(b->iv - 1)) {
                alu_ri(X, 1, ALU_AND, RAX, (int32_t)(b->iv - 1));
                break;
            }
            if (!modulo && k >= 0) {
                shift_ri(X, 7, RAX, k);
                break;
            }
            load_int(c, b, RCX);
        } else {
            load_int(c, b, RCX);
            test_rr(X, RCX, RCX);
            jcc_l(X, CC_LE, bail);
        }
        int ok = x_label(X);
        cqo(X);
        idiv_r(X, RCX);
        if (modulo) {
            mov_rr(X, RAX, RDX);
            test_rr(X, RAX, RAX);
            jcc_l(X, CC_NS, ok);
            alu_rr(X, ALU_ADD, RAX, RCX);
        } else {
            test_rr(X, RDX, RDX);
            jcc_l(X, CC_NS, ok);
            dec_r(X, RAX);
        }
        x_bind(X, ok);
        break;
    }
    default:
        jmp_l(X, bail);
        break;
    }
    mov_ri(X, RDX, LZ_VAL_INT);
}

/* rax = bits of x op y on doubles, rdx = the float tag. */
static void emit_float_op(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    if (sub == LZ_OP_FLOOR_DIVIDE) {
        jmp_l(X, bail);
        return;
    }
    load_double(c, a, 0, bail, false);
    load_double(c, b, 1, bail, false);
    switch (sub) {
    case LZ_OP_ADD:
        sse_arith(X, SSE_ADD, 0, 1);
        break;
    case LZ_OP_SUBTRACT:
        sse_arith(X, SSE_SUB, 0, 1);
        break;
    case LZ_OP_MULTIPLY:
        sse_arith(X, SSE_MUL, 0, 1);
        break;
    case LZ_OP_DIVIDE: {
        int ok = x_label(X);
        xorpd_xx(X, 2, 2);
        ucomisd_xx(X, 1, 2);
        jcc_l(X, CC_P, ok);
        jcc_l(X, CC_E, bail); /* a zero divisor is the reference's error */
        x_bind(X, ok);
        sse_arith(X, SSE_DIV, 0, 1);
        break;
    }
    case LZ_OP_MODULO: {
        /* Whole-number operands only (ge_exec.c: arith): y in (0, 2^53], x in [-2^53, 2^53]. */
        int ok = x_label(X);
        mov_ri(X, RAX, double_bits((double)GE_TWO_53_I));
        movq_xr(X, 3, RAX);
        mov_ri(X, RAX, double_bits(-(double)GE_TWO_53_I));
        movq_xr(X, 4, RAX);
        xorpd_xx(X, 2, 2);
        ucomisd_xx(X, 1, 2);
        jcc_l(X, CC_BE, bail); /* y <= 0 or NaN */
        ucomisd_xx(X, 3, 1);
        jcc_l(X, CC_B, bail); /* y > 2^53 or NaN */
        ucomisd_xx(X, 3, 0);
        jcc_l(X, CC_B, bail); /* x > 2^53 or NaN */
        ucomisd_xx(X, 0, 4);
        jcc_l(X, CC_B, bail); /* x < -2^53 */
        cvttsd2si_rx(X, RAX, 0);
        cvttsd2si_rx(X, RCX, 1);
        cvtsi2sd_xr(X, 5, RAX);
        ucomisd_xx(X, 5, 0);
        jcc_l(X, CC_NE, bail);
        jcc_l(X, CC_P, bail);
        cvtsi2sd_xr(X, 5, RCX);
        ucomisd_xx(X, 5, 1);
        jcc_l(X, CC_NE, bail);
        jcc_l(X, CC_P, bail);
        cqo(X);
        idiv_r(X, RCX);
        mov_rr(X, RAX, RDX);
        test_rr(X, RAX, RAX);
        jcc_l(X, CC_NS, ok);
        alu_rr(X, ALU_ADD, RAX, RCX);
        x_bind(X, ok);
        cvtsi2sd_xr(X, 0, RAX);
        break;
    }
    default:
        jmp_l(X, bail);
        return;
    }
    movq_rx(X, RAX, 0);
    mov_ri(X, RDX, LZ_VAL_FLOAT);
}

/* The arithmetic of a binary operation; leaves rax = payload, rdx = tag. */
static void emit_arith(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    bool a_int = a->kind == OPD_INT;
    bool b_int = b->kind == OPD_INT;
    if (a->kind == OPD_FLT || b->kind == OPD_FLT) {
        emit_float_op(c, sub, a, b, bail);
        return;
    }
    int lfloat = x_label(X);
    if (!a_int) {
        alu_mi(X, 0, ALU_CMP, a->base, a->disp, LZ_VAL_INT);
        jcc_l(X, CC_NE, lfloat);
    }
    if (!b_int) {
        alu_mi(X, 0, ALU_CMP, b->base, b->disp, LZ_VAL_INT);
        jcc_l(X, CC_NE, lfloat);
    }
    emit_int_op(c, sub, a, b, bail);
    if (!(a_int && b_int)) {
        int lres = x_label(X);
        x_bind(X, lres);
        int saved = X->sec;
        x_section(X, SEC_COLD);
        x_bind(X, lfloat);
        emit_float_op(c, sub, a, b, bail);
        jmp_l(X, lres);
        x_section(X, saved);
    }
}

static int not_cc_int(uint8_t sub) {
    switch (sub) {
    case LZ_OP_LESS:
        return CC_GE;
    case LZ_OP_GREATER:
        return CC_LE;
    case LZ_OP_LESS_EQUAL:
        return CC_G;
    case LZ_OP_GREATER_EQUAL:
        return CC_L;
    default:
        return CC_NE; /* EQUAL */
    }
}

/* The float comparison, ending in a jump to lfalse; falls through when true. */
static void emit_float_compare(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int lfalse, int commit,
                               int bail) {
    bool a_exact = a->kind != OPD_INT || (a->iv >= -GE_TWO_53_I && a->iv <= GE_TWO_53_I);
    bool b_exact = b->kind != OPD_INT || (b->iv >= -GE_TWO_53_I && b->iv <= GE_TWO_53_I);
    if (!a_exact || !b_exact) {
        jmp_l(X, bail);
        return;
    }
    load_double(c, a, 0, bail, true);
    load_double(c, b, 1, bail, true);
    if (commit != 0) {
        lea(X, R15, R15, commit);
    }
    switch (sub) {
    case LZ_OP_LESS: /* x < y: y above x */
        ucomisd_xx(X, 1, 0);
        jcc_l(X, CC_BE, lfalse);
        break;
    case LZ_OP_LESS_EQUAL:
        ucomisd_xx(X, 1, 0);
        jcc_l(X, CC_B, lfalse);
        break;
    case LZ_OP_GREATER:
        ucomisd_xx(X, 0, 1);
        jcc_l(X, CC_BE, lfalse);
        break;
    case LZ_OP_GREATER_EQUAL:
        ucomisd_xx(X, 0, 1);
        jcc_l(X, CC_B, lfalse);
        break;
    default: /* EQUAL: equal and ordered */
        ucomisd_xx(X, 0, 1);
        jcc_l(X, CC_NE, lfalse);
        jcc_l(X, CC_P, lfalse);
        break;
    }
}

/*
 * Compares two numbers. Falls through when the comparison is true, jumps to lfalse
 * when it is false, bails when an operand is not a number. `commit` instructions are
 * added to the counter on both paths (before the flags are set).
 */
static void emit_compare(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int lfalse, int commit, int bail) {
    bool a_int = a->kind == OPD_INT;
    bool b_int = b->kind == OPD_INT;
    if (a->kind == OPD_FLT || b->kind == OPD_FLT) {
        emit_float_compare(c, sub, a, b, lfalse, commit, bail);
        return;
    }
    int lfloat = x_label(X);
    if (!a_int) {
        alu_mi(X, 0, ALU_CMP, a->base, a->disp, LZ_VAL_INT);
        jcc_l(X, CC_NE, lfloat);
    }
    if (!b_int) {
        alu_mi(X, 0, ALU_CMP, b->base, b->disp, LZ_VAL_INT);
        jcc_l(X, CC_NE, lfloat);
    }
    load_int(c, a, RAX);
    if (b_int && fits32(b->iv)) {
        if (commit != 0) {
            lea(X, R15, R15, commit);
        }
        alu_ri(X, 1, ALU_CMP, RAX, (int32_t)b->iv);
    } else {
        load_int(c, b, RCX);
        if (commit != 0) {
            lea(X, R15, R15, commit);
        }
        alu_rr(X, ALU_CMP, RAX, RCX);
    }
    jcc_l(X, not_cc_int(sub), lfalse);
    if (!(a_int && b_int)) {
        int ltrue = x_label(X);
        x_bind(X, ltrue);
        int saved = X->sec;
        x_section(X, SEC_COLD);
        x_bind(X, lfloat);
        emit_float_compare(c, sub, a, b, lfalse, commit, bail);
        jmp_l(X, ltrue);
        x_section(X, saved);
    }
}

/* Continue at instruction `next` after the code just emitted (a jump unless it follows). */
static void finish_group(Cx *c, uint32_t next) {
    if (c->ir.nexte[c->i] != next) {
        emit_flush(c);
        jmp_l(X, c->lab[next]);
    }
}

static void end_plain(Cx *c, int span) {
    c->pend += span;
    finish_group(c, (uint32_t)(c->i + (size_t)span));
}

/* ---- the instructions ---- */

static void emit_unconditional_bail(Cx *c) {
    jmp_l(X, bail_label(c));
}

static void emit_constant(Cx *c, uint16_t index) {
    const LzValue *v = &c->ir.chunk->constants[index];
    int32_t at = 16 * c->d;
    switch (v->type) {
    case LZ_VAL_OBJECT:
        mov_ri(X, RAX, (uint64_t)(uintptr_t)v->as.object);
        inc_m32(X, RAX, O_RC);
        mov_mi64(X, R14, at, LZ_VAL_OBJECT);
        mov_mr(X, R14, at + 8, RAX);
        break;
    case LZ_VAL_INT:
        mov_mi64(X, R14, at, LZ_VAL_INT);
        if (fits32(v->as.integer)) {
            mov_mi64(X, R14, at + 8, (int32_t)v->as.integer);
        } else {
            mov_ri(X, RAX, (uint64_t)v->as.integer);
            mov_mr(X, R14, at + 8, RAX);
        }
        break;
    case LZ_VAL_FLOAT:
        mov_mi64(X, R14, at, LZ_VAL_FLOAT);
        mov_ri(X, RAX, double_bits(v->as.number));
        mov_mr(X, R14, at + 8, RAX);
        break;
    case LZ_VAL_BOOL:
        mov_mi64(X, R14, at, LZ_VAL_BOOL);
        mov_mi64(X, R14, at + 8, v->as.boolean ? 1 : 0);
        break;
    default:
        mov_mi64(X, R14, at, LZ_VAL_NIL);
        mov_mi64(X, R14, at + 8, 0);
        break;
    }
}

static void emit_truthiness_jump(Cx *c, bool jump_if_true, uint32_t target) {
    int32_t at = 16 * (c->d - 1);
    int l = c->lab[target];
    int next = x_label(X);
    emit_flush(c);
    mov_rm32(X, RAX, R14, at);
    test_rr(X, RAX, RAX);
    if (jump_if_true) {
        jcc_l(X, CC_E, next); /* nil: falsy */
        alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_BOOL);
        jcc_l(X, CC_NE, l);
        cmp_mi8(X, R14, at + 8, 0);
        jcc_l(X, CC_NE, l);
    } else {
        jcc_l(X, CC_E, l);
        alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_BOOL);
        jcc_l(X, CC_NE, next);
        cmp_mi8(X, R14, at + 8, 0);
        jcc_l(X, CC_E, l);
    }
    x_bind(X, next);
}

static void emit_not(Cx *c) {
    int bail = bail_label(c);
    int32_t at = 16 * (c->d - 1);
    int store = x_label(X);
    int truthy = x_label(X);
    mov_rm32(X, RAX, R14, at);
    alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_OBJECT);
    jcc_l(X, CC_E, bail);
    mov_ri(X, RDX, 1);
    test_rr(X, RAX, RAX);
    jcc_l(X, CC_E, store);
    alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_BOOL);
    jcc_l(X, CC_NE, truthy);
    cmp_mi8(X, R14, at + 8, 0);
    jcc_l(X, CC_E, store);
    x_bind(X, truthy);
    mov_ri(X, RDX, 0);
    x_bind(X, store);
    mov_mi64(X, R14, at, LZ_VAL_BOOL);
    mov_mr(X, R14, at + 8, RDX);
    end_plain(c, 1);
}

static void emit_negate(Cx *c) {
    int bail = bail_label(c);
    int32_t at = 16 * (c->d - 1);
    int not_int = x_label(X);
    int done = x_label(X);
    mov_rm32(X, RAX, R14, at);
    alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_INT);
    jcc_l(X, CC_NE, not_int);
    mov_rm(X, RCX, R14, at + 8);
    neg_r(X, RCX);
    jcc_l(X, CC_O, bail);
    mov_mr(X, R14, at + 8, RCX);
    x_bind(X, done);
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, not_int);
    alu_ri(X, 0, ALU_CMP, RAX, LZ_VAL_FLOAT);
    jcc_l(X, CC_NE, bail);
    mov_rm(X, RCX, R14, at + 8);
    mov_ri(X, RAX, UINT64_C(0x8000000000000000));
    alu_rr(X, ALU_XOR, RCX, RAX);
    mov_mr(X, R14, at + 8, RCX);
    jmp_l(X, done);
    x_section(X, saved);
    end_plain(c, 1);
}

/* A plain arithmetic instruction on the two values on top of the stack. */
static void emit_plain_arith(Cx *c, uint8_t sub) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    emit_arith(c, sub, &a, &b, bail);
    mov_mr(X, R14, 16 * (c->d - 2), RDX);
    mov_mr(X, R14, 16 * (c->d - 2) + 8, RAX);
    end_plain(c, 1);
}

/* A plain comparison: leaves a bool where the left operand was. */
static void emit_plain_compare(Cx *c, uint8_t sub) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    int lfalse = x_label(X);
    int done = x_label(X);
    int32_t at = 16 * (c->d - 2);
    emit_compare(c, sub, &a, &b, lfalse, 0, bail);
    mov_mi64(X, R14, at, LZ_VAL_BOOL);
    mov_mi64(X, R14, at + 8, 1);
    x_bind(X, done);
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, lfalse);
    mov_mi64(X, R14, at, LZ_VAL_BOOL);
    mov_mi64(X, R14, at + 8, 0);
    jmp_l(X, done);
    x_section(X, saved);
    end_plain(c, 1);
}

/* [load a] [load b] ARITH [store] and the variant with the left operand on the stack. */
static void emit_bin(Cx *c, const GeIns *in, bool left_on_stack) {
    int bail = bail_label(c);
    unsigned ka = (unsigned)(in->kinds & 3u);
    unsigned kb = (unsigned)((in->kinds >> 2) & 3u);
    unsigned ks = (unsigned)((in->kinds >> 4) & 3u);
    Opd a;
    Opd b;
    if (left_on_stack) {
        a = stack_opd(c->d - 1);
    } else {
        prep_opd(c, &a, ka, in->a, 0, R8, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, R9, bail);
    if (ks == GE_ST_GLOBAL) {
        emit_global_store_cell(c, R10, in->c, &c->ir.sites[3 * c->i + 2], bail);
    }
    emit_arith(c, in->sub, &a, &b, bail);
    if (ks == GE_ST_NONE) {
        int slot = left_on_stack ? c->d - 1 : c->d;
        mov_mr(X, R14, 16 * slot, RDX);
        mov_mr(X, R14, 16 * slot + 8, RAX);
    } else if (ks == GE_ST_LOCAL) {
        emit_store_drop(c, R14, 16 * (int32_t)in->c);
    } else {
        emit_store_drop(c, R10, 0);
    }
    end_plain(c, in->span);
}

/* [load a] [load b] CMP JUMP_IF_FALSE POP and the variant with the left operand on the stack. */
static void emit_cmpj(Cx *c, const GeIns *in, bool left_on_stack) {
    int bail = bail_label(c);
    unsigned ka = (unsigned)(in->kinds & 3u);
    unsigned kb = (unsigned)((in->kinds >> 2) & 3u);
    Opd a;
    Opd b;
    if (left_on_stack) {
        a = stack_opd(c->d - 1);
    } else {
        prep_opd(c, &a, ka, in->a, 0, R8, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, R9, bail);
    int lfalse = x_label(X);
    emit_compare(c, in->sub, &a, &b, lfalse, c->pend + in->span, bail);
    c->pend = 0;
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, lfalse);
    jmp_l(X, c->lab[in->target]);
    x_section(X, saved);
    finish_group(c, (uint32_t)(c->i + in->span));
}

/* array[index] with the array operand a and the index operand b; leaves rax = array, rsi:rdi = item. */
static void emit_array_load(Cx *c, const Opd *a, const Opd *b, int bail) {
    alu_mi(X, 0, ALU_CMP, a->base, a->disp, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, bail);
    mov_rm(X, RAX, a->base, a->disp + 8);
    alu_mi(X, 0, ALU_CMP, RAX, O_OTYPE, LZ_OBJ_ARRAY);
    jcc_l(X, CC_NE, bail);
    if (b->kind == OPD_INT) {
        mov_ri(X, RCX, (uint64_t)b->iv);
    } else {
        alu_mi(X, 0, ALU_CMP, b->base, b->disp, LZ_VAL_INT);
        jcc_l(X, CC_NE, bail);
        mov_rm(X, RCX, b->base, b->disp + 8);
    }
    int positive = x_label(X);
    mov_rm(X, RDX, RAX, O_ARR_COUNT);
    test_rr(X, RCX, RCX);
    jcc_l(X, CC_NS, positive);
    alu_rr(X, ALU_ADD, RCX, RDX);
    x_bind(X, positive);
    alu_rr(X, ALU_CMP, RCX, RDX);
    jcc_l(X, CC_AE, bail);
    shift_ri(X, 4, RCX, 4);
    alu_rm(X, ALU_ADD, RCX, RAX, O_ARR_ITEMS);
    mov_rm(X, RSI, RCX, 0);
    mov_rm(X, RDI, RCX, 8);
}

static void emit_grab_item(Cx *c) { /* the item in rsi:rdi gets a reference when it is an object */
    int skip = x_label(X);
    alu_ri(X, 0, ALU_CMP, RSI, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, skip);
    inc_m32(X, RDI, O_RC);
    x_bind(X, skip);
}

/* [load a] [load b] INDEX and [load b] INDEX (array on the stack). */
static void emit_index_fused(Cx *c, const GeIns *in, bool array_on_stack) {
    int bail = bail_label(c);
    unsigned ka = (unsigned)(in->kinds & 3u);
    unsigned kb = (unsigned)((in->kinds >> 2) & 3u);
    Opd a;
    Opd b;
    if (array_on_stack) {
        a = stack_opd(c->d - 1);
    } else {
        prep_opd(c, &a, ka, in->a, 0, R8, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, R9, bail);
    emit_array_load(c, &a, &b, bail);
    emit_grab_item(c);
    int slot = array_on_stack ? c->d - 1 : c->d;
    mov_mr(X, R14, 16 * slot, RSI);
    mov_mr(X, R14, 16 * slot + 8, RDI);
    if (array_on_stack) {
        emit_drop_obj(c, RAX); /* the array reference the stack held */
    }
    end_plain(c, in->span);
}

static void emit_index_plain(Cx *c) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    emit_array_load(c, &a, &b, bail);
    emit_grab_item(c);
    mov_mr(X, R14, 16 * (c->d - 2), RSI);
    mov_mr(X, R14, 16 * (c->d - 2) + 8, RDI);
    emit_drop_obj(c, RAX);
    end_plain(c, 1);
}

static void emit_set_index(Cx *c) {
    int bail = bail_label(c);
    int32_t arr = 16 * (c->d - 3);
    int32_t idx = 16 * (c->d - 2);
    int32_t val = 16 * (c->d - 1);
    alu_mi(X, 0, ALU_CMP, R14, arr, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, bail);
    mov_rm(X, RAX, R14, arr + 8);
    alu_mi(X, 0, ALU_CMP, RAX, O_OTYPE, LZ_OBJ_ARRAY);
    jcc_l(X, CC_NE, bail);
    alu_mi(X, 0, ALU_CMP, R14, idx, LZ_VAL_INT);
    jcc_l(X, CC_NE, bail);
    mov_rm(X, RCX, R14, idx + 8);
    int positive = x_label(X);
    mov_rm(X, RDX, RAX, O_ARR_COUNT);
    test_rr(X, RCX, RCX);
    jcc_l(X, CC_NS, positive);
    alu_rr(X, ALU_ADD, RCX, RDX);
    x_bind(X, positive);
    alu_rr(X, ALU_CMP, RCX, RDX);
    jcc_l(X, CC_AE, bail);
    alu_mi(X, 1, ALU_CMP, RAX, O_ARR_ELEM, 0); /* typed arrays check the value: the reference's */
    jcc_l(X, CC_NE, bail);
    alu_mi(X, 0, ALU_CMP, R14, val, LZ_VAL_OBJECT);
    jcc_l(X, CC_E, bail);
    shift_ri(X, 4, RCX, 4);
    alu_rm(X, ALU_ADD, RCX, RAX, O_ARR_ITEMS);
    alu_mi(X, 0, ALU_CMP, RCX, 0, LZ_VAL_OBJECT);
    jcc_l(X, CC_E, bail);
    mov_rm(X, RSI, R14, val);
    mov_rm(X, RDI, R14, val + 8);
    mov_mr(X, RCX, 0, RSI);
    mov_mr(X, RCX, 8, RDI);
    emit_drop_obj(c, RAX);
    end_plain(c, 1);
}

static void emit_get_global(Cx *c, uint16_t name_index) {
    int bail = bail_label(c);
    emit_global(c, R8, name_index, &c->ir.sites[3 * c->i], true, bail);
    emit_copy_grab(c, R8, 0, R14, 16 * c->d);
    end_plain(c, 1);
}

static void emit_set_global(Cx *c, uint16_t name_index) {
    int bail = bail_label(c);
    emit_global_store_cell(c, R10, name_index, &c->ir.sites[3 * c->i], bail);
    mov_rm(X, RDX, R14, 16 * (c->d - 1));
    mov_rm(X, RAX, R14, 16 * (c->d - 1) + 8);
    emit_store_drop(c, R10, 0);
    end_plain(c, 1);
}

/* Runs the instruction through the reference semantics (lz_vm_exec_one). */
static void emit_delegate(Cx *c) {
    const GeIns *in = &c->ir.prog->ins[c->i];
    emit_sync_sc(c, c->d);
    c->pend += 1;
    emit_flush(c);
    mov_rr(X, RDI, RBX);
    mov_ri(X, RSI, (uint64_t)(uintptr_t)c->ir.chunk);
    mov_ri(X, RDX, in->offset);
    emit_call_abs(c, (uintptr_t)jit_delegate);
    int fail = fail_label(c);
    test_rr32(X, RAX, RAX); /* the status is a 32-bit value: the upper half is undefined */
    jcc_l(X, CC_NE, fail);
    finish_group(c, (uint32_t)(c->i + 1));
}

/*
 * The cold path of an instruction with an inline fast path: the reference semantics through
 * lz_vm_exec_one, then back to `back`. The hot path counts this instruction lazily (in
 * c->pend), so the cold path adds it for the call and takes it off again afterwards.
 */
static void emit_delegate_cold(Cx *c, int slow, int back) {
    const GeIns *in = &c->ir.prog->ins[c->i];
    int fail = fail_label(c);
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    emit_sync_sc(c, c->d);
    lea(X, R15, R15, c->pend + 1);
    mov_rr(X, RDI, RBX);
    mov_ri(X, RSI, (uint64_t)(uintptr_t)c->ir.chunk);
    mov_ri(X, RDX, in->offset);
    emit_call_abs(c, (uintptr_t)jit_delegate);
    test_rr32(X, RAX, RAX);
    jcc_l(X, CC_NE, fail);
    lea(X, R15, R15, -(c->pend + 1));
    jmp_l(X, back);
    x_section(X, saved);
}

/* CHECK_TYPE / CHECK_RETURN / CHECK_PARAM: a nil, bool, int or float the type accepts passes inline. */
static void emit_check(Cx *c, int32_t disp, uint16_t type_index) {
    if (type_index >= c->ir.chunk->type_count || (c->ir.chunk->types[type_index].kinds & 0xFu) == 0) {
        emit_delegate(c);
        return;
    }
    int slow = x_label(X);
    int done = x_label(X);
    mov_rm32(X, RAX, R14, disp);
    alu_ri(X, 0, ALU_CMP, RAX, 3);
    jcc_l(X, CC_A, slow); /* an object: arrays are checked element by element */
    mov_ri(X, RCX, c->ir.chunk->types[type_index].kinds & 0xFu);
    bt_rr32(X, RCX, RAX);
    jcc_l(X, CC_AE, slow); /* the kind is not in the type (a literal type, or a mismatch) */
    x_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

/* The closure of the running frame in rax, or the slow path when there is none. */
static void emit_frame_closure(Cx *c, int slow) {
    mov_rm(X, RAX, RBX, O_FC);
    imul_rri(X, RAX, RAX, FS);
    alu_rr(X, ALU_ADD, RAX, RBX);
    mov_rm(X, RAX, RAX, O_FRAMES - FS + FR_CLOSURE);
    test_rr(X, RAX, RAX);
    jcc_l(X, CC_E, slow);
}

/* rax = the closure's upvalue `index`'s location in r8 (slow path when out of range). */
static void emit_upvalue_location(Cx *c, uint16_t index, int slow) {
    emit_frame_closure(c, slow);
    cmp_mi16(X, RAX, O_CL_UPVC, (int32_t)index);
    jcc_l(X, CC_BE, slow);
    mov_rm(X, RAX, RAX, O_CL_UPV + 8 * (int32_t)index);
    mov_rm(X, R8, RAX, O_UPV_LOC);
}

static void emit_get_upvalue(Cx *c, uint16_t index) {
    int slow = x_label(X);
    int done = x_label(X);
    emit_upvalue_location(c, index, slow);
    emit_copy_grab(c, R8, 0, R14, 16 * c->d);
    x_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_set_upvalue(Cx *c, uint16_t index) {
    int slow = x_label(X);
    int done = x_label(X);
    int inl = x_label(X);
    emit_upvalue_location(c, index, slow); /* RAX = the upvalue, R8 = its location */
    mov_rr(X, R10, R8);
    mov_rm(X, RDX, R14, 16 * (c->d - 1));
    /*
     * An object stored into a closed upvalue may close a reference cycle, and the reference
     * implementation tells the cycle collector (lz_skink_note_store). Everything that cannot
     * need that is stored inline: a number, bool or nil, a string, any store into an upvalue
     * that is already watched, and any store into one that is still open (its frame's close
     * reports it).
     */
    alu_ri(X, 0, ALU_CMP, RDX, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, inl);
    mov_rm(X, RCX, R14, 16 * (c->d - 1) + 8);
    alu_mi(X, 0, ALU_CMP, RCX, O_OTYPE, LZ_OBJ_STRING);
    jcc_l(X, CC_E, inl);
    alu_mi(X, 1, ALU_CMP, RAX, O_UPV_WATCHED, 0);
    jcc_l(X, CC_NE, inl);
    lea(X, RCX, RAX, O_UPV_CLOSED);
    alu_rr(X, ALU_CMP, R8, RCX); /* the location is the upvalue's own slot: it is closed */
    jcc_l(X, CC_E, slow);
    x_bind(X, inl);
    mov_rm(X, RAX, R14, 16 * (c->d - 1) + 8);
    emit_store_drop(c, R10, 0);
    x_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_get_self(Cx *c) {
    int slow = x_label(X);
    int done = x_label(X);
    emit_frame_closure(c, slow);
    inc_m32(X, RAX, O_RC);
    mov_mi64(X, R14, 16 * c->d, LZ_VAL_OBJECT);
    mov_mr(X, R14, 16 * c->d + 8, RAX);
    x_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_call(Cx *c, uint16_t argc) {
    const GeIns *in = &c->ir.prog->ins[c->i];
    int d = c->d;
    int delta = d - (int)argc;
    int32_t callee = 16 * (d - 1 - (int)argc);
    int slow = x_label(X);
    int after = x_label(X);
    int enter_exit = x_label(X);

    c->pend += 1;
    emit_flush(c);

    alu_mi(X, 0, ALU_CMP, R14, callee, LZ_VAL_OBJECT);
    jcc_l(X, CC_NE, slow);
    mov_rm(X, RAX, R14, callee + 8);
    alu_mi(X, 0, ALU_CMP, RAX, O_OTYPE, LZ_OBJ_CLOSURE);
    jcc_l(X, CC_NE, slow);
    mov_rm(X, RDX, RAX, O_CL_FN);
    cmp_mi16(X, RDX, O_FN_PARAMS, (int32_t)argc);
    jcc_l(X, CC_NE, slow);
    cmp_mi8(X, RDX, O_FN_REST, 0);
    jcc_l(X, CC_NE, slow);
    mov_rm(X, RSI, RDX, O_FN_CACHE);
    test_rr(X, RSI, RSI);
    jcc_l(X, CC_E, slow);
    mov_rm(X, RSI, RSI, O_PR_JIT);
    test_rr(X, RSI, RSI);
    jcc_l(X, CC_E, slow);
    mov_rm(X, RDI, RBX, O_FC);
    alu_ri(X, 1, ALU_CMP, RDI, LZ_FRAMES_MAX);
    jcc_l(X, CC_AE, slow);
    mov_rm(X, R8, RSI, O_JIT_ENTRY);

    /* push the frame */
    imul_rri(X, R9, RDI, FS);
    alu_rr(X, ALU_ADD, R9, RBX);
    mov_mi64(X, R9, O_FRAMES - FS + FR_IP, (int32_t)(c->i + 1));
    mov_mr(X, R9, O_FRAMES + FR_CLOSURE, RAX);
    lea(X, RCX, RDX, O_FN_CHUNK);
    mov_mr(X, R9, O_FRAMES + FR_CHUNK, RCX);
    mov_mi64(X, R9, O_FRAMES + FR_IP, 0);
    lea(X, RCX, R13, delta);
    mov_mr(X, R9, O_FRAMES + FR_BASE, RCX);
    mov_mi64(X, R9, O_FRAMES + FR_SUPPLIED, -1);
    mov_mi32(X, R9, O_FRAMES + FR_CALLOFF, (int32_t)in->offset);
    inc_r(X, RDI);
    mov_mr(X, RBX, O_FC, RDI);

    push_r(X, R13);
    push_r(X, R14);
    mov_rr(X, R13, RCX);
    lea(X, R14, R14, 16 * delta);
    call_r(X, R8);
    pop_r(X, R14);
    pop_r(X, R13);
    x_bind(X, after);

    /* Everything else: natives, defaults, a rest parameter, errors, functions without native code. */
    int fail = fail_label(c); /* made before switching sections: it emits its own cold code */
    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, slow);
    emit_sync_sc(c, d);
    mov_rr(X, RDI, RBX);
    mov_ri(X, RSI, argc);
    mov_ri(X, RDX, c->i + 1);
    mov_ri(X, RCX, in->offset);
    mov_rr(X, R8, RSP);
    emit_call_abs(c, (uintptr_t)jit_call);
    mov_rr(X, RDX, RAX);
    shift_ri(X, 5, RDX, 32); /* bit 32 of the result: a frame was pushed */
    test_rr32(X, RAX, RAX);  /* the status */
    jcc_l(X, CC_NE, fail);
    test_rr(X, RDX, RDX);
    jcc_l(X, CC_E, after);
    mov_rm(X, RCX, RSP, 0);
    test_rr(X, RCX, RCX);
    jcc_l(X, CC_E, enter_exit);
    mov_rm(X, RAX, RBX, O_FC);
    imul_rri(X, RAX, RAX, FS);
    alu_rr(X, ALU_ADD, RAX, RBX);
    mov_rm(X, RDX, RAX, O_FRAMES - FS + FR_BASE);
    push_r(X, R13);
    push_r(X, R14);
    mov_rr(X, R13, RDX);
    shift_ri(X, 4, RDX, 4);
    lea(X, R14, RDX, O_STACK);
    alu_rr(X, ALU_ADD, R14, RBX);
    call_r(X, RCX);
    pop_r(X, R14);
    pop_r(X, R13);
    jmp_l(X, after);
    x_bind(X, enter_exit);
    emit_leave(c, GE_JX_ENTER, 0);
    x_section(X, saved);
    c->pend = 0;
    finish_group(c, (uint32_t)(c->i + 1));
}

static void emit_return(Cx *c) {
    int d = c->d;
    if (d < 1) {
        emit_unconditional_bail(c);
        return;
    }
    int bail = bail_label(c);
    int halt = x_label(X);
    int ok = x_label(X);
    alu_mi(X, 1, ALU_CMP, RBX, O_FC, 1);
    jcc_l(X, CC_BE, halt);
    mov_rm(X, RAX, RBX, O_OPENUPV);
    test_rr(X, RAX, RAX);
    jcc_l(X, CC_E, ok);
    mov_rm(X, RCX, RAX, O_UPV_LOC);
    alu_rr(X, ALU_CMP, RCX, R14);
    jcc_l(X, CC_AE, bail); /* a variable of this frame was captured: the reference closes it */
    x_bind(X, ok);
    mov_rm(X, RDX, R14, 16 * (d - 1));
    mov_rm(X, RAX, R14, 16 * (d - 1) + 8);
    if (d <= 8) {
        for (int k = d - 2; k >= -1; k--) {
            emit_drop_mem(c, R14, 16 * k, true);
        }
    } else {
        push_r(X, RAX);
        push_r(X, RDX);
        mov_rr(X, RDI, RBX);
        lea(X, RSI, R14, -16);
        mov_ri(X, RDX, (uint64_t)d);
        emit_call_abs(c, (uintptr_t)jit_drop_range);
        pop_r(X, RDX);
        pop_r(X, RAX);
    }
    mov_mr(X, R14, -16, RDX);
    mov_mr(X, R14, -8, RAX);
    dec_m64(X, RBX, O_FC);
    lea(X, R15, R15, c->pend + 1);
    alu_ri(X, 1, ALU_ADD, RSP, 8);
    ret(X);

    int saved = X->sec;
    x_section(X, SEC_COLD);
    x_bind(X, halt);
    lea(X, R15, R15, c->pend + 1);
    lea(X, RAX, R13, d);
    mov_mr(X, RBX, O_SC, RAX);
    emit_leave(c, GE_JX_HALT, (uint32_t)c->i);
    x_section(X, saved);
    c->pend = 0;
}

static void emit_jump_if_supplied(Cx *c) {
    const GeIns *in = &c->ir.prog->ins[c->i];
    uint16_t param = c->ir.oa[c->i];
    int l = c->lab[in->target];
    emit_flush(c);
    if (param >= 64) {
        jmp_l(X, l);
        return;
    }
    mov_rm(X, RAX, RBX, O_FC);
    imul_rri(X, RAX, RAX, FS);
    alu_rr(X, ALU_ADD, RAX, RBX);
    mov_rm(X, RAX, RAX, O_FRAMES - FS + FR_SUPPLIED);
    mov_ri(X, RCX, (uint64_t)1 << param);
    test_rr(X, RAX, RCX);
    jcc_l(X, CC_NE, l);
}

static void emit_instruction(Cx *c) {
    const GeIns *in = &c->ir.prog->ins[c->i];
    uint8_t plain = in->plain;
    int d = c->d;

    if (c->i == c->ir.n) {
        emit_unconditional_bail(c); /* the end of the chunk without a RETURN */
        return;
    }
    if (c->ir.fused[c->i]) {
        switch (in->op) {
        case GE_OP_BIN:
            emit_bin(c, in, false);
            return;
        case GE_OP_BIN1:
            emit_bin(c, in, true);
            return;
        case GE_OP_CMPJ:
            emit_cmpj(c, in, false);
            return;
        case GE_OP_CMPJ1:
            emit_cmpj(c, in, true);
            return;
        case GE_OP_INDEX:
            emit_index_fused(c, in, false);
            return;
        default:
            emit_index_fused(c, in, true);
            return;
        }
    }
    if (in->op == GE_OP_DELEGATE) {
        if (plain == LZ_OP_CONSTANT || plain == LZ_OP_GET_GLOBAL || plain == LZ_OP_SET_GLOBAL) {
            emit_unconditional_bail(c);
        } else if (plain == LZ_OP_CHECK_TYPE || plain == LZ_OP_CHECK_RETURN) {
            emit_check(c, 16 * (d - 1), c->ir.oa[c->i]);
        } else if (plain == LZ_OP_CHECK_PARAM) {
            emit_check(c, 16 * (int32_t)c->ir.oa[c->i], c->ir.ob[c->i]);
        } else if (plain == LZ_OP_GET_UPVALUE) {
            emit_get_upvalue(c, c->ir.oa[c->i]);
        } else if (plain == LZ_OP_SET_UPVALUE) {
            emit_set_upvalue(c, c->ir.oa[c->i]);
        } else if (plain == LZ_OP_GET_SELF) {
            emit_get_self(c);
        } else {
            emit_delegate(c);
        }
        return;
    }
    if (plain == LZ_OP_CALL_EX) {
        emit_unconditional_bail(c); /* spread and named arguments: the interpreter does the call */
        return;
    }

    uint16_t a = c->ir.oa[c->i];
    switch (plain) {
    case LZ_OP_CONSTANT:
        emit_constant(c, a);
        end_plain(c, 1);
        break;
    case LZ_OP_NIL:
        mov_mi64(X, R14, 16 * d, LZ_VAL_NIL);
        mov_mi64(X, R14, 16 * d + 8, 0);
        end_plain(c, 1);
        break;
    case LZ_OP_TRUE:
    case LZ_OP_FALSE:
        mov_mi64(X, R14, 16 * d, LZ_VAL_BOOL);
        mov_mi64(X, R14, 16 * d + 8, plain == LZ_OP_TRUE ? 1 : 0);
        end_plain(c, 1);
        break;
    case LZ_OP_POP:
        emit_drop_mem(c, R14, 16 * (d - 1), false);
        end_plain(c, 1);
        break;
    case LZ_OP_GET_LOCAL:
        emit_copy_grab(c, R14, 16 * (int32_t)a, R14, 16 * d);
        end_plain(c, 1);
        break;
    case LZ_OP_SET_LOCAL:
        mov_rm(X, RDX, R14, 16 * (d - 1));
        mov_rm(X, RAX, R14, 16 * (d - 1) + 8);
        emit_store_drop(c, R14, 16 * (int32_t)a);
        end_plain(c, 1);
        break;
    case LZ_OP_GET_GLOBAL:
        emit_get_global(c, a);
        break;
    case LZ_OP_SET_GLOBAL:
        emit_set_global(c, a);
        break;
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
        emit_plain_arith(c, plain);
        break;
    case LZ_OP_EQUAL:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
        emit_plain_compare(c, plain);
        break;
    case LZ_OP_NOT:
        emit_not(c);
        break;
    case LZ_OP_NEGATE:
        emit_negate(c);
        break;
    case LZ_OP_JUMP:
    case LZ_OP_LOOP:
        c->pend += 1;
        emit_flush(c);
        jmp_l(X, c->lab[in->target]);
        break;
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE:
        c->pend += 1;
        emit_truthiness_jump(c, plain == LZ_OP_JUMP_IF_TRUE, in->target);
        finish_group(c, (uint32_t)(c->i + 1));
        break;
    case LZ_OP_JUMP_IF_SUPPLIED:
        c->pend += 1;
        emit_jump_if_supplied(c);
        finish_group(c, (uint32_t)(c->i + 1));
        break;
    case LZ_OP_INDEX:
        emit_index_plain(c);
        break;
    case LZ_OP_SET_INDEX:
        emit_set_index(c);
        break;
    case LZ_OP_CALL:
        emit_call(c, a);
        break;
    case LZ_OP_RETURN:
        emit_return(c);
        break;
    default:
        emit_delegate(c);
        break;
    }
}


struct GeJit *ge_jit_compile(const LzChunk *chunk, const GeProgram *program, size_t entry_depth) {
    if (program == NULL || program->ins == NULL || program->count < 1 || !tramp_init() ||
        entry_depth > 4000) {
        return NULL;
    }
    const size_t n = program->count - 1;
    if (n == 0 || n > 60000) {
        return NULL;
    }

    Cx c;
    memset(&c, 0, sizeof c);
    x64_init(&c.x);
    struct GeJit *jit = NULL;
    uint8_t *image = NULL;
    c.lab = malloc((n + 1) * sizeof *c.lab);
    if (c.lab == NULL || !jit_ir_build(&c.ir, chunk, program, entry_depth)) {
        goto done;
    }

    for (size_t i = 0; i <= n; i++) {
        c.lab[i] = x_label(&c.x);
    }
    c.pend = 0;

    /* The entry used by native calls: a prologue, then the stack check, then instruction 0. */
    int entry_call = x_label(&c.x);
    x_bind(&c.x, entry_call);
    alu_ri(&c.x, 1, ALU_SUB, RSP, 8);
    c.i = 0;
    c.d = (int)entry_depth;
    c.bail_lbl = -1;
    c.fail_lbl = -1;
    lea(&c.x, RAX, R13, (int32_t)c.ir.max_depth);
    alu_ri(&c.x, 1, ALU_CMP, RAX, LZ_STACK_MAX);
    int entry_bail = x_label(&c.x);
    jcc_l(&c.x, CC_A, entry_bail);
    {
        int saved = c.x.sec;
        x_section(&c.x, SEC_COLD);
        x_bind(&c.x, entry_bail);
        emit_bail_stub(&c, 0, (int)entry_depth, 0);
        x_section(&c.x, saved);
    }

    for (size_t i = 0; i <= n; i++) {
        if (!c.ir.reach[i]) {
            continue;
        }
        c.i = i;
        c.d = c.ir.depth[i];
        c.bail_lbl = -1;
        c.fail_lbl = -1;
        if (c.ir.leader[i]) {
            emit_flush(&c);
        }
        x_bind(&c.x, c.lab[i]);
        emit_instruction(&c);
    }

    if (c.x.oom) {
        goto done;
    }
    size_t size = x64_size(&c.x);
    if (size > ((size_t)8 << 20)) {
        goto done;
    }
    image = malloc(size);
    if (image == NULL || !x64_link(&c.x, image)) {
        goto done;
    }

    {
        /* A debugging aid: LZ_JIT_DUMP=prefix writes each compiled program to prefix.N.bin,
         * for objdump -D -b binary -mi386:x86-64 -M intel. */
        static unsigned dump_counter;
        const char *prefix = getenv("LZ_JIT_DUMP");
        if (prefix != NULL) {
            char name[512];
            snprintf(name, sizeof name, "%s.%u.bin", prefix, dump_counter++);
            FILE *f = fopen(name, "wb");
            if (f != NULL) {
                fwrite(image, 1, size, f);
                fclose(f);
            }
        }
    }
    jit = calloc(1, sizeof *jit);
    if (jit == NULL) {
        goto done;
    }
    jit->entry = calloc(n + 1, sizeof *jit->entry);
    jit->depth = malloc((n + 1) * sizeof *jit->depth);
    if (jit->entry == NULL || jit->depth == NULL) {
        ge_jit_free(jit);
        jit = NULL;
        goto done;
    }
    jit->mem = jit_exec_alloc(image, size, &jit->mem_size);
    if (jit->mem == NULL) {
        ge_jit_free(jit);
        jit = NULL;
        goto done;
    }
    jit->code_size = size;
    jit->count = n + 1;
    jit->max_depth = c.ir.max_depth;
    jit->sites = c.ir.sites;
    c.ir.sites = NULL;
    for (size_t i = 0; i <= n; i++) {
        jit->depth[i] = c.ir.depth[i];
        jit->entry[i] = (c.ir.reach[i] && c.ir.leader[i])
                            ? (uint8_t *)jit->mem + x64_label_address(&c.x, c.lab[i])
                            : NULL;
    }
    jit->entry_call = (uint8_t *)jit->mem + x64_label_address(&c.x, entry_call);

done:
    free(image);
    free(c.lab);
    jit_ir_free(&c.ir);
    x64_free(&c.x);
    return jit;
}

#endif /* x86-64 */
