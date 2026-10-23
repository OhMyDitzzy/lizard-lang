/*
 * The Gecko JIT, AArch64 backend (Linux, Android/Termux, macOS). Same design as the x86-64
 * backend (ge_jit_x64.c, docs/ENGINE.md): native code works on the VM's own value stack and
 * frame stack, every fast path checks what it assumes before changing anything, and a check
 * that fails "bails out" to the interpreter at the same instruction.
 *
 * Registers:
 *
 *   x19  the LzVM                     x20  the GeJitExit of this entry
 *   x21  base index of the frame      x22  &vm->stack[base]
 *   x23  instructions executed since entry (added to vm->instructions on exit)
 *   x24  &vm->stack_count             x25  &vm->frame_count
 *        (anchors: the fields after the big stack array are out of reach of a
 *         scaled load offset from x19, but not from these)
 *   x0-x15 scratch, x16 call target / immediates, x17 address scratch (ge_a64.h),
 *   x18 untouched (reserved by the platform), x30 saved in every native frame.
 *
 * A native function is entered by `blr` and keeps 16 bytes at sp: [sp] the return address,
 * [sp + 8] a scratch slot. Entering in the middle of a body (from the trampoline) builds the
 * same frame by hand, so `ret` works the same way everywhere. Exits other than a function's
 * return restore sp from the exit block and leave through the trampoline's thunk.
 */

#define _DEFAULT_SOURCE

#include "ge_jit.h"

#if GE_JIT_A64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ge_a64.h"
#include "ge_jit_common.h"
#include "ge_jit_rt.h"

#define R_VM 19
#define R_EXIT 20
#define R_BIDX 21
#define R_BPTR 22
#define R_EXEC 23
#define R_SC 24
#define R_FC 25

/* Field offsets relative to the anchors. */
#define A_SC 0
#define A_FRAMES (O_FRAMES - O_SC)
#define A_FC 0
#define A_OPENUPV (O_OPENUPV - O_FC)
#define A_INSTR (O_INSTR - O_FC)

_Static_assert(O_FRAMES - O_SC >= 0 && O_FRAMES - O_SC < 4096, "frames must be near stack_count");
_Static_assert(O_OPENUPV - O_FC >= 0 && O_OPENUPV - O_FC < 4096, "open_upvalues must be near frame_count");
_Static_assert(O_INSTR - O_FC >= 0 && O_INSTR - O_FC < 4096, "instructions must be near frame_count");

/* ---------------------------------------------------------------------------------
 * The entry trampoline
 * ------------------------------------------------------------------------------- */

typedef uint32_t (*TrampolineFn)(LzVM *vm, size_t base_index, LzValue *base_ptr, void *target,
                                 GeJitExit *exit);

static TrampolineFn g_tramp;
static void *g_exit_thunk;
static bool g_tramp_failed;

#define TRAMP_FRAME 96

/*
 * enter(vm, base_index, base_ptr, target, exit): saves the callee-saved registers, sets up
 * x19-x25, and branches to `target` laid out as the body of a native function. Native code
 * leaves by `ret` (a function returned) or by the exit thunk (everything else).
 */
static bool tramp_init(void) {
    if (g_tramp != NULL) {
        return true;
    }
    if (g_tramp_failed) {
        return false;
    }
    A64 x;
    a64_init(&x);
    int after = a_label(&x);
    int thunk = a_label(&x);
    int skip = a_label(&x);

    push_pair(&x, 29, 30, TRAMP_FRAME); /* stp x29, x30, [sp, #-96]! */
    stp_x(&x, 19, 20, SP, 16);
    stp_x(&x, 21, 22, SP, 32);
    stp_x(&x, 23, 24, SP, 48);
    str_x(&x, 25, SP, 64);
    mov_rr(&x, R_VM, 0);
    mov_rr(&x, R_BIDX, 1);
    mov_rr(&x, R_BPTR, 2);
    mov_rr(&x, R_EXIT, 4);
    mov_imm(&x, R_EXEC, 0);
    mov_imm(&x, 9, (uint64_t)O_SC);
    addsub_r(&x, A_ADD, R_SC, R_VM, 9);
    mov_imm(&x, 9, (uint64_t)O_FC);
    addsub_r(&x, A_ADD, R_FC, R_VM, 9);
    mov_rr(&x, 9, SP);
    str_x(&x, 9, R_EXIT, OFFS(GeJitExit, saved_rsp));
    mov_imm(&x, 9, GE_JX_RETURNED);
    str_w(&x, 9, R_EXIT, X_REASON);
    adr_l(&x, 9, after);
    push_x(&x, 9); /* the "return address" of the body: where a plain return lands */
    br_r(&x, 3);

    a_bind(&x, thunk);
    ldr_x(&x, 9, R_EXIT, OFFS(GeJitExit, saved_rsp));
    mov_rr(&x, SP, 9);
    a_bind(&x, after);
    ldr_x(&x, 9, R_FC, A_INSTR);
    addsub_r(&x, A_ADD, 9, 9, R_EXEC);
    str_x(&x, 9, R_FC, A_INSTR);
    ldr_w(&x, 0, R_EXIT, X_REASON);
    cmp_iw(&x, 0, GE_JX_RETURNED);
    bcond_l(&x, CD_NE, skip);
    str_x(&x, R_BIDX, R_SC, A_SC);
    a_bind(&x, skip);
    ldr_x(&x, 25, SP, 64);
    ldp_x(&x, 23, 24, SP, 48);
    ldp_x(&x, 21, 22, SP, 32);
    ldp_x(&x, 19, 20, SP, 16);
    pop_pair(&x, 29, 30, TRAMP_FRAME);
    ret_(&x);

    size_t size = a64_size(&x);
    uint8_t *image = malloc(size);
    bool ok = image != NULL && a64_link(&x, image);
    size_t mapped = 0;
    uint8_t *memory = ok ? jit_exec_alloc(image, size, &mapped) : NULL;
    size_t thunk_at = a64_label_address(&x, thunk);
    free(image);
    a64_free(&x);
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
    A64 x;
    JitIR ir;
    int *lab; /* instruction -> label */
    /* emission state */
    size_t i;
    int d;
    int pend;
    int bail_lbl;
    int fail_lbl;
} Cx;

#define X (&c->x)
#define INS(c) (&(c)->ir.prog->ins[(c)->i])

/* operand pointer registers (preserved across the slow path of a global lookup) */
#define P_A 10
#define P_B 11
#define P_S 12

static inline uint64_t u64(int64_t v) {
    return (uint64_t)v;
}

/* ---- emission helpers ---- */

static void emit_flush(Cx *c) {
    if (c->pend != 0) {
        add_imm(X, R_EXEC, R_EXEC, c->pend);
        c->pend = 0;
    }
}

static void emit_call_abs(Cx *c, uintptr_t fn) {
    mov_imm(X, 16, (uint64_t)fn);
    blr_r(X, 16);
}

/* The exit sequence shared by every stub: reason and pc into the exit block, then the thunk. */
static void emit_leave(Cx *c, uint32_t reason, uint32_t pc) {
    mov_imm(X, 9, pc);
    str_w(X, 9, R_EXIT, X_PC);
    mov_imm(X, 9, reason);
    str_w(X, 9, R_EXIT, X_REASON);
    mov_imm(X, 16, (uint64_t)(uintptr_t)g_exit_thunk);
    br_r(X, 16);
}

static void emit_sync_sc(Cx *c, int depth) {
    add_imm(X, 9, R_BIDX, depth);
    str_x(X, 9, R_SC, A_SC);
}

/* A cold stub that returns to the interpreter at instruction pc, with `depth` values on the stack. */
static void emit_bail_stub(Cx *c, uint32_t pc, int depth, int pend) {
    if (pend != 0) {
        add_imm(X, R_EXEC, R_EXEC, pend);
    }
    emit_sync_sc(c, depth);
    emit_leave(c, GE_JX_BAIL, pc);
}

static int bail_label(Cx *c) {
    if (c->bail_lbl >= 0) {
        return c->bail_lbl;
    }
    int l = a_label(X);
    c->bail_lbl = l;
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, l);
    emit_bail_stub(c, (uint32_t)c->i, c->d, c->pend);
    a_section(X, saved);
    return l;
}

/* A stub for a failed call into the shared machinery: the status is in w0. */
static int fail_label(Cx *c) {
    if (c->fail_lbl >= 0) {
        return c->fail_lbl;
    }
    int l = a_label(X);
    c->fail_lbl = l;
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, l);
    str_w(X, 0, R_EXIT, X_STATUS);
    emit_leave(c, GE_JX_FAIL, (uint32_t)c->i);
    a_section(X, saved);
    return l;
}

static void emit_rc_inc(Cx *c, int obj) { /* ++object->refcount (x9 is clobbered) */
    ldr_w(X, 9, obj, O_RC);
    addsub_iw(X, A_ADD, 9, 9, 1);
    str_w(X, 9, obj, O_RC);
}

/* The real release of the object in x1 (refcount known to be <= 1), reached from the slow path. */
static void emit_drop_slow(Cx *c, int skip, int slow, bool keep_pair) {
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    if (keep_pair) {
        push_pair(X, 13, 14, 16);
    }
    mov_rr(X, 0, R_VM);
    emit_call_abs(c, (uintptr_t)jit_drop); /* x1 = the object */
    if (keep_pair) {
        pop_pair(X, 13, 14, 16);
    }
    b_l(X, skip);
    a_section(X, saved);
}

/* Drop the value at [base+off] when it is an object. keep_pair preserves x13 and x14. */
static void emit_drop_mem(Cx *c, int base, int32_t off, bool keep_pair) {
    int skip = a_label(X);
    int slow = a_label(X);
    ldr_w(X, 9, base, off);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, skip);
    ldr_x(X, 1, base, off + 8);
    ldr_w(X, 2, 1, O_RC);
    cmp_iw(X, 2, 1);
    bcond_l(X, CD_LS, slow);
    addsub_iw(X, A_SUB, 2, 2, 1);
    str_w(X, 2, 1, O_RC);
    a_bind(X, skip);
    emit_drop_slow(c, skip, slow, keep_pair);
}

/* Drop the object whose pointer is in `reg`, a reference nobody will read again. */
static void emit_drop_obj(Cx *c, int reg) {
    int skip = a_label(X);
    int slow = a_label(X);
    ldr_w(X, 9, reg, O_RC);
    cmp_iw(X, 9, 1);
    bcond_l(X, CD_LS, slow);
    addsub_iw(X, A_SUB, 9, 9, 1);
    str_w(X, 9, reg, O_RC);
    a_bind(X, skip);
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    mov_rr(X, 1, reg);
    mov_rr(X, 0, R_VM);
    emit_call_abs(c, (uintptr_t)jit_drop);
    b_l(X, skip);
    a_section(X, saved);
}

/* Stores x2 (tag) and x0 (payload) at [base+off]; an object that was there is released. */
static void emit_store_drop(Cx *c, int base, int32_t off) {
    int done = a_label(X);
    int slow = a_label(X);
    ldr_w(X, 9, base, off);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_EQ, slow);
    stp_x(X, 2, 0, base, off);
    a_bind(X, done);
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    ldr_x(X, 1, base, off + 8);
    stp_x(X, 2, 0, base, off);
    mov_rr(X, 0, R_VM);
    emit_call_abs(c, (uintptr_t)jit_drop);
    b_l(X, done);
    a_section(X, saved);
}

/* A value just copied (x0 = tag word, x2 = payload) gets a reference when it is an object. */
static void emit_grab_pair(Cx *c) {
    int skip = a_label(X);
    cmp_iw(X, 0, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, skip);
    emit_rc_inc(c, 2);
    a_bind(X, skip);
}

static void emit_copy_grab(Cx *c, int sb, int32_t so, int db, int32_t dof) {
    ldp_x(X, 0, 2, sb, so);
    stp_x(X, 0, 2, db, dof);
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
    int slow = a_label(X);
    int done = a_label(X);
    mov_imm(X, 1, (uint64_t)(uintptr_t)site);
    ldr_w(X, 0, 1, 0);
    ldr_x(X, 2, R_VM, O_GCAP);
    cmp_r(X, 0, 2);
    bcond_l(X, CD_HS, slow);
    mov_imm(X, 2, sizeof(LzTableEntry));
    mul_r(X, 0, 0, 2);
    ldr_x(X, 2, R_VM, O_GENT);
    addsub_r(X, A_ADD, 0, 0, 2);
    mov_imm(X, 2, (uint64_t)(uintptr_t)name);
    ldr_x(X, 3, 0, 0);
    cmp_r(X, 2, 3);
    bcond_l(X, CD_NE, slow);
    add_imm(X, reg, 0, OFFS(LzTableEntry, value));
    a_bind(X, done);

    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    push_pair(X, P_A, P_B, 32);
    stp_x(X, P_S, 13, SP, 16);
    mov_rr(X, 0, R_VM);
    mov_imm(X, 1, (uint64_t)(uintptr_t)name);
    mov_imm(X, 2, (uint64_t)(uintptr_t)site);
    emit_call_abs(c, builtin_ok ? (uintptr_t)jit_global_ptr : (uintptr_t)jit_global_cell);
    ldp_x(X, P_S, 13, SP, 16);
    pop_pair(X, P_A, P_B, 32);
    cbz_l(X, 0, bail);
    mov_rr(X, reg, 0);
    b_l(X, done);
    a_section(X, saved);
}

/* Where an assignment to an existing global goes (ge_exec.c: global_store). */
static void emit_global_store_cell(Cx *c, int reg, uint16_t name_index, JitSite *site, int bail) {
    ldr_x(X, 0, R_VM, O_GCAP);
    mov_imm(X, 1, 3);
    mul_r(X, 0, 0, 1);
    lsr_i(X, 0, 0, 2);
    ldr_x(X, 1, R_VM, O_GCNT);
    addsub_i(X, A_ADD, 1, 1, 1);
    cmp_r(X, 1, 0);
    bcond_l(X, CD_HI, bail);
    emit_global(c, reg, name_index, site, false, bail);
}

static void prep_opd(Cx *c, Opd *o, unsigned kind, uint16_t index, int site_slot, int ptr_reg, int bail) {
    memset(o, 0, sizeof *o);
    if (kind == GE_K_LOCAL) {
        o->kind = OPD_MEM;
        o->base = R_BPTR;
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
    o.base = R_BPTR;
    o.disp = 16 * slot;
    return o;
}

static uint64_t double_bits(double v) {
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}

static void load_tag(Cx *c, const Opd *o, int reg) {
    ldr_w(X, reg, o->base, o->disp);
}

/* d<dreg> = the operand as a double; jumps to bail when it is not a number. Clobbers x0, x1, x3. */
static void load_double(Cx *c, const Opd *o, int dreg, int bail, bool exact) {
    if (o->kind == OPD_INT) {
        mov_imm(X, 0, u64(o->iv));
        scvtf_dx(X, dreg, 0);
    } else if (o->kind == OPD_FLT) {
        mov_imm(X, 0, double_bits(o->fv));
        fmov_dx(X, dreg, 0);
    } else {
        int not_float = a_label(X);
        int done = a_label(X);
        load_tag(c, o, 0);
        cmp_iw(X, 0, LZ_VAL_FLOAT);
        bcond_l(X, CD_NE, not_float);
        ldr_x(X, 0, o->base, o->disp + 8);
        fmov_dx(X, dreg, 0);
        b_l(X, done);
        a_bind(X, not_float);
        cmp_iw(X, 0, LZ_VAL_INT);
        bcond_l(X, CD_NE, bail);
        ldr_x(X, 0, o->base, o->disp + 8);
        if (exact) {
            /* an int must convert without rounding: -2^53 <= v <= 2^53 */
            mov_imm(X, 1, u64(GE_TWO_53_I));
            addsub_r(X, A_ADD, 1, 0, 1);
            mov_imm(X, 3, u64(GE_TWO_53_I) * 2u);
            cmp_r(X, 1, 3);
            bcond_l(X, CD_HI, bail);
        }
        scvtf_dx(X, dreg, 0);
        a_bind(X, done);
    }
}

static void load_int(Cx *c, const Opd *o, int reg) {
    if (o->kind == OPD_INT) {
        mov_imm(X, reg, u64(o->iv));
    } else {
        ldr_x(X, reg, o->base, o->disp + 8);
    }
}

static bool fits_imm12(int64_t v) {
    return v >= 0 && v < 4096;
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

/* x0 = x op y on ints (y in x1 unless a constant); the result tag goes to x2. */
static void emit_int_op(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    bool bconst = b->kind == OPD_INT;
    load_int(c, a, 0);
    switch (sub) {
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT: {
        int op = sub == LZ_OP_ADD ? A_ADDS : A_SUBS;
        if (bconst && fits_imm12(b->iv)) {
            addsub_i(X, op, 0, 0, (uint32_t)b->iv);
        } else if (bconst && b->iv < 0 && b->iv > -4096) {
            addsub_i(X, op == A_ADDS ? A_SUBS : A_ADDS, 0, 0, (uint32_t)(-b->iv));
        } else {
            load_int(c, b, 1);
            addsub_r(X, op, 0, 0, 1);
        }
        bcond_l(X, CD_VS, bail);
        break;
    }
    case LZ_OP_MULTIPLY:
        load_int(c, b, 1);
        mul_r(X, 3, 0, 1);
        smulh_r(X, 4, 0, 1);
        cmp_r_asr63(X, 4, 3);
        bcond_l(X, CD_NE, bail);
        mov_rr(X, 0, 3);
        break;
    case LZ_OP_DIVIDE:
        load_int(c, b, 1);
        if (!bconst || b->iv == 0) {
            cbz_l(X, 1, bail);
        }
        scvtf_dx(X, 0, 0);
        scvtf_dx(X, 1, 1);
        fop(X, F_DIV, 0, 0, 1);
        fmov_xd(X, 0, 0);
        mov_imm(X, 2, LZ_VAL_FLOAT);
        return;
    case LZ_OP_MODULO:
    case LZ_OP_FLOOR_DIVIDE: {
        bool modulo = sub == LZ_OP_MODULO;
        if (bconst) {
            if (b->iv <= 0) {
                b_l(X, bail); /* zero, -1 and negative divisors are the reference's */
                break;
            }
            int k = log2_exact(b->iv);
            if (modulo && k >= 0) {
                mov_imm(X, 1, u64(b->iv - 1));
                logic_r(X, L_AND, 0, 0, 1);
                break;
            }
            if (!modulo && k >= 0) {
                asr_i(X, 0, 0, k);
                break;
            }
            load_int(c, b, 1);
        } else {
            load_int(c, b, 1);
            cmp_i(X, 1, 0);
            bcond_l(X, CD_LE, bail);
        }
        int ok = a_label(X);
        sdiv_r(X, 3, 0, 1);
        msub_r(X, 4, 3, 1, 0); /* x4 = x - q * y */
        if (modulo) {
            mov_rr(X, 0, 4);
            cmp_i(X, 0, 0);
            bcond_l(X, CD_GE, ok);
            addsub_r(X, A_ADD, 0, 0, 1);
        } else {
            mov_rr(X, 0, 3);
            cmp_i(X, 4, 0);
            bcond_l(X, CD_GE, ok);
            addsub_i(X, A_SUB, 0, 0, 1);
        }
        a_bind(X, ok);
        break;
    }
    default:
        b_l(X, bail);
        break;
    }
    mov_imm(X, 2, LZ_VAL_INT);
}

/* x0 = bits of x op y on doubles, x2 = the float tag. */
static void emit_float_op(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    if (sub == LZ_OP_FLOOR_DIVIDE) {
        b_l(X, bail);
        return;
    }
    load_double(c, a, 0, bail, false);
    load_double(c, b, 1, bail, false);
    switch (sub) {
    case LZ_OP_ADD:
        fop(X, F_ADD, 0, 0, 1);
        break;
    case LZ_OP_SUBTRACT:
        fop(X, F_SUB, 0, 0, 1);
        break;
    case LZ_OP_MULTIPLY:
        fop(X, F_MUL, 0, 0, 1);
        break;
    case LZ_OP_DIVIDE:
        fcmp_d0(X, 1);
        bcond_l(X, CD_EQ, bail); /* a zero divisor is the reference's error */
        fop(X, F_DIV, 0, 0, 1);
        break;
    case LZ_OP_MODULO: {
        /* Whole-number operands only (ge_exec.c: arith): y in (0, 2^53], x in [-2^53, 2^53]. */
        int ok = a_label(X);
        mov_imm(X, 0, double_bits((double)GE_TWO_53_I));
        fmov_dx(X, 3, 0);
        mov_imm(X, 0, double_bits(-(double)GE_TWO_53_I));
        fmov_dx(X, 4, 0);
        fcmp_d0(X, 1);
        bcond_l(X, CD_LE, bail); /* y <= 0 or NaN */
        fcmp_dd(X, 1, 3);
        bcond_l(X, CD_HI, bail); /* y > 2^53 */
        fcmp_dd(X, 0, 3);
        bcond_l(X, CD_HI, bail); /* x > 2^53 or NaN */
        fcmp_dd(X, 0, 4);
        bcond_l(X, CD_LT, bail); /* x < -2^53 */
        fcvtzs_xd(X, 0, 0);
        fcvtzs_xd(X, 1, 1);
        scvtf_dx(X, 5, 0);
        fcmp_dd(X, 5, 0);
        bcond_l(X, CD_NE, bail);
        scvtf_dx(X, 5, 1);
        fcmp_dd(X, 5, 1);
        bcond_l(X, CD_NE, bail);
        sdiv_r(X, 3, 0, 1);
        msub_r(X, 4, 3, 1, 0);
        mov_rr(X, 0, 4);
        cmp_i(X, 0, 0);
        bcond_l(X, CD_GE, ok);
        addsub_r(X, A_ADD, 0, 0, 1);
        a_bind(X, ok);
        scvtf_dx(X, 0, 0);
        break;
    }
    default:
        b_l(X, bail);
        return;
    }
    fmov_xd(X, 0, 0);
    mov_imm(X, 2, LZ_VAL_FLOAT);
}

/* The arithmetic of a binary operation; leaves x0 = payload, x2 = tag. */
static void emit_arith(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int bail) {
    bool a_int = a->kind == OPD_INT;
    bool b_int = b->kind == OPD_INT;
    if (a->kind == OPD_FLT || b->kind == OPD_FLT) {
        emit_float_op(c, sub, a, b, bail);
        return;
    }
    int lfloat = a_label(X);
    if (!a_int) {
        load_tag(c, a, 9);
        cmp_iw(X, 9, LZ_VAL_INT);
        bcond_l(X, CD_NE, lfloat);
    }
    if (!b_int) {
        load_tag(c, b, 9);
        cmp_iw(X, 9, LZ_VAL_INT);
        bcond_l(X, CD_NE, lfloat);
    }
    emit_int_op(c, sub, a, b, bail);
    if (!(a_int && b_int)) {
        int lres = a_label(X);
        a_bind(X, lres);
        int saved = X->sec;
        a_section(X, SEC_COLD);
        a_bind(X, lfloat);
        emit_float_op(c, sub, a, b, bail);
        b_l(X, lres);
        a_section(X, saved);
    }
}

static int not_cc_int(uint8_t sub) {
    switch (sub) {
    case LZ_OP_LESS:
        return CD_GE;
    case LZ_OP_GREATER:
        return CD_LE;
    case LZ_OP_LESS_EQUAL:
        return CD_GT;
    case LZ_OP_GREATER_EQUAL:
        return CD_LT;
    default:
        return CD_NE; /* EQUAL */
    }
}

/* The float comparison, ending in a branch to lfalse; falls through when true. */
static void emit_float_compare(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int lfalse, int commit,
                               int bail) {
    bool a_exact = a->kind != OPD_INT || (a->iv >= -GE_TWO_53_I && a->iv <= GE_TWO_53_I);
    bool b_exact = b->kind != OPD_INT || (b->iv >= -GE_TWO_53_I && b->iv <= GE_TWO_53_I);
    if (!a_exact || !b_exact) {
        b_l(X, bail);
        return;
    }
    load_double(c, a, 0, bail, true);
    load_double(c, b, 1, bail, true);
    if (commit != 0) {
        add_imm(X, R_EXEC, R_EXEC, commit);
    }
    fcmp_dd(X, 0, 1);
    /* The flags of an unordered comparison make every one of these branches go to lfalse. */
    switch (sub) {
    case LZ_OP_LESS:
        bcond_l(X, CD_PL, lfalse);
        break;
    case LZ_OP_LESS_EQUAL:
        bcond_l(X, CD_HI, lfalse);
        break;
    case LZ_OP_GREATER:
        bcond_l(X, CD_LE, lfalse);
        break;
    case LZ_OP_GREATER_EQUAL:
        bcond_l(X, CD_LT, lfalse);
        break;
    default:
        bcond_l(X, CD_NE, lfalse);
        break;
    }
}

/*
 * Compares two numbers. Falls through when the comparison is true, branches to lfalse when
 * it is false, bails when an operand is not a number. `commit` instructions are added to
 * the counter on both paths (before the flags are set).
 */
static void emit_compare(Cx *c, uint8_t sub, const Opd *a, const Opd *b, int lfalse, int commit, int bail) {
    bool a_int = a->kind == OPD_INT;
    bool b_int = b->kind == OPD_INT;
    if (a->kind == OPD_FLT || b->kind == OPD_FLT) {
        emit_float_compare(c, sub, a, b, lfalse, commit, bail);
        return;
    }
    int lfloat = a_label(X);
    if (!a_int) {
        load_tag(c, a, 9);
        cmp_iw(X, 9, LZ_VAL_INT);
        bcond_l(X, CD_NE, lfloat);
    }
    if (!b_int) {
        load_tag(c, b, 9);
        cmp_iw(X, 9, LZ_VAL_INT);
        bcond_l(X, CD_NE, lfloat);
    }
    load_int(c, a, 0);
    if (commit != 0) {
        add_imm(X, R_EXEC, R_EXEC, commit);
    }
    if (b_int && fits_imm12(b->iv)) {
        cmp_i(X, 0, (uint32_t)b->iv);
    } else if (b_int && b->iv < 0 && b->iv > -4096) {
        cmn_i(X, 0, (uint32_t)(-b->iv));
    } else {
        load_int(c, b, 1);
        cmp_r(X, 0, 1);
    }
    bcond_l(X, not_cc_int(sub), lfalse);
    if (!(a_int && b_int)) {
        int ltrue = a_label(X);
        a_bind(X, ltrue);
        int saved = X->sec;
        a_section(X, SEC_COLD);
        a_bind(X, lfloat);
        emit_float_compare(c, sub, a, b, lfalse, commit, bail);
        b_l(X, ltrue);
        a_section(X, saved);
    }
}

/* Continue at instruction `next` after the code just emitted (a branch unless it follows). */
static void finish_group(Cx *c, uint32_t next) {
    if (c->ir.nexte[c->i] != next) {
        emit_flush(c);
        b_l(X, c->lab[next]);
    }
}

static void end_plain(Cx *c, int span) {
    c->pend += span;
    finish_group(c, (uint32_t)(c->i + (size_t)span));
}

/* ---- the instructions ---- */

static void emit_unconditional_bail(Cx *c) {
    b_l(X, bail_label(c));
}

static void emit_constant(Cx *c, uint16_t index) {
    const LzValue *v = &c->ir.chunk->constants[index];
    int32_t at = 16 * c->d;
    switch (v->type) {
    case LZ_VAL_OBJECT:
        mov_imm(X, 3, (uint64_t)(uintptr_t)v->as.object);
        emit_rc_inc(c, 3);
        mov_imm(X, 0, LZ_VAL_OBJECT);
        stp_x(X, 0, 3, R_BPTR, at);
        break;
    case LZ_VAL_INT:
        mov_imm(X, 0, LZ_VAL_INT);
        mov_imm(X, 2, u64(v->as.integer));
        stp_x(X, 0, 2, R_BPTR, at);
        break;
    case LZ_VAL_FLOAT:
        mov_imm(X, 0, LZ_VAL_FLOAT);
        mov_imm(X, 2, double_bits(v->as.number));
        stp_x(X, 0, 2, R_BPTR, at);
        break;
    case LZ_VAL_BOOL:
        mov_imm(X, 0, LZ_VAL_BOOL);
        mov_imm(X, 2, v->as.boolean ? 1 : 0);
        stp_x(X, 0, 2, R_BPTR, at);
        break;
    default:
        mov_imm(X, 0, LZ_VAL_NIL);
        stp_x(X, 0, XZR, R_BPTR, at);
        break;
    }
}

static void emit_truthiness_jump(Cx *c, bool jump_if_true, uint32_t target) {
    int32_t at = 16 * (c->d - 1);
    int l = c->lab[target];
    int next = a_label(X);
    emit_flush(c);
    ldr_w(X, 0, R_BPTR, at);
    if (jump_if_true) {
        cbz_wl(X, 0, next); /* nil: falsy */
        cmp_iw(X, 0, LZ_VAL_BOOL);
        bcond_l(X, CD_NE, l);
        ldr_b(X, 1, R_BPTR, at + 8);
        cbnz_wl(X, 1, l);
    } else {
        cbz_wl(X, 0, l);
        cmp_iw(X, 0, LZ_VAL_BOOL);
        bcond_l(X, CD_NE, next);
        ldr_b(X, 1, R_BPTR, at + 8);
        cbz_wl(X, 1, l);
    }
    a_bind(X, next);
}

static void emit_not(Cx *c) {
    int bail = bail_label(c);
    int32_t at = 16 * (c->d - 1);
    int store = a_label(X);
    int truthy = a_label(X);
    ldr_w(X, 0, R_BPTR, at);
    cmp_iw(X, 0, LZ_VAL_OBJECT);
    bcond_l(X, CD_EQ, bail);
    mov_imm(X, 2, 1);
    cbz_wl(X, 0, store);
    cmp_iw(X, 0, LZ_VAL_BOOL);
    bcond_l(X, CD_NE, truthy);
    ldr_b(X, 1, R_BPTR, at + 8);
    cbz_wl(X, 1, store);
    a_bind(X, truthy);
    mov_imm(X, 2, 0);
    a_bind(X, store);
    mov_imm(X, 1, LZ_VAL_BOOL);
    stp_x(X, 1, 2, R_BPTR, at);
    end_plain(c, 1);
}

static void emit_negate(Cx *c) {
    int bail = bail_label(c);
    int32_t at = 16 * (c->d - 1);
    int not_int = a_label(X);
    int done = a_label(X);
    ldr_w(X, 0, R_BPTR, at);
    cmp_iw(X, 0, LZ_VAL_INT);
    bcond_l(X, CD_NE, not_int);
    ldr_x(X, 1, R_BPTR, at + 8);
    neg_s(X, 1, 1);
    bcond_l(X, CD_VS, bail);
    str_x(X, 1, R_BPTR, at + 8);
    a_bind(X, done);
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, not_int);
    cmp_iw(X, 0, LZ_VAL_FLOAT);
    bcond_l(X, CD_NE, bail);
    ldr_x(X, 1, R_BPTR, at + 8);
    mov_imm(X, 0, UINT64_C(0x8000000000000000));
    logic_r(X, L_EOR, 1, 1, 0);
    str_x(X, 1, R_BPTR, at + 8);
    b_l(X, done);
    a_section(X, saved);
    end_plain(c, 1);
}

/* A plain arithmetic instruction on the two values on top of the stack. */
static void emit_plain_arith(Cx *c, uint8_t sub) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    emit_arith(c, sub, &a, &b, bail);
    stp_x(X, 2, 0, R_BPTR, 16 * (c->d - 2));
    end_plain(c, 1);
}

/* A plain comparison: leaves a bool where the left operand was. */
static void emit_plain_compare(Cx *c, uint8_t sub) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    int lfalse = a_label(X);
    int done = a_label(X);
    int32_t at = 16 * (c->d - 2);
    emit_compare(c, sub, &a, &b, lfalse, 0, bail);
    mov_imm(X, 0, LZ_VAL_BOOL);
    mov_imm(X, 2, 1);
    stp_x(X, 0, 2, R_BPTR, at);
    a_bind(X, done);
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, lfalse);
    mov_imm(X, 0, LZ_VAL_BOOL);
    stp_x(X, 0, XZR, R_BPTR, at);
    b_l(X, done);
    a_section(X, saved);
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
        prep_opd(c, &a, ka, in->a, 0, P_A, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, P_B, bail);
    if (ks == GE_ST_GLOBAL) {
        emit_global_store_cell(c, P_S, in->c, &c->ir.sites[3 * c->i + 2], bail);
    }
    emit_arith(c, in->sub, &a, &b, bail);
    if (ks == GE_ST_NONE) {
        int slot = left_on_stack ? c->d - 1 : c->d;
        stp_x(X, 2, 0, R_BPTR, 16 * slot);
    } else if (ks == GE_ST_LOCAL) {
        emit_store_drop(c, R_BPTR, 16 * (int32_t)in->c);
    } else {
        emit_store_drop(c, P_S, 0);
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
        prep_opd(c, &a, ka, in->a, 0, P_A, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, P_B, bail);
    int lfalse = a_label(X);
    emit_compare(c, in->sub, &a, &b, lfalse, c->pend + in->span, bail);
    c->pend = 0;
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, lfalse);
    b_l(X, c->lab[in->target]);
    a_section(X, saved);
    finish_group(c, (uint32_t)(c->i + in->span));
}

/* array[index] with the array operand a and the index operand b; leaves x0 = array, x3:x4 = item. */
static void emit_array_load(Cx *c, const Opd *a, const Opd *b, int bail) {
    load_tag(c, a, 9);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, bail);
    ldr_x(X, 0, a->base, a->disp + 8);
    ldr_w(X, 9, 0, O_OTYPE);
    cmp_iw(X, 9, LZ_OBJ_ARRAY);
    bcond_l(X, CD_NE, bail);
    if (b->kind == OPD_INT) {
        mov_imm(X, 1, u64(b->iv));
    } else {
        load_tag(c, b, 9);
        cmp_iw(X, 9, LZ_VAL_INT);
        bcond_l(X, CD_NE, bail);
        ldr_x(X, 1, b->base, b->disp + 8);
    }
    int positive = a_label(X);
    ldr_x(X, 2, 0, O_ARR_COUNT);
    cmp_i(X, 1, 0);
    bcond_l(X, CD_GE, positive);
    addsub_r(X, A_ADD, 1, 1, 2);
    a_bind(X, positive);
    cmp_r(X, 1, 2);
    bcond_l(X, CD_HS, bail);
    lsl_i(X, 1, 1, 4);
    ldr_x(X, 9, 0, O_ARR_ITEMS);
    addsub_r(X, A_ADD, 1, 1, 9);
    ldp_x(X, 3, 4, 1, 0);
}

static void emit_grab_item(Cx *c) { /* the item in x3:x4 gets a reference when it is an object */
    int skip = a_label(X);
    cmp_iw(X, 3, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, skip);
    emit_rc_inc(c, 4);
    a_bind(X, skip);
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
        prep_opd(c, &a, ka, in->a, 0, P_A, bail);
    }
    prep_opd(c, &b, kb, in->b, 1, P_B, bail);
    emit_array_load(c, &a, &b, bail);
    emit_grab_item(c);
    int slot = array_on_stack ? c->d - 1 : c->d;
    stp_x(X, 3, 4, R_BPTR, 16 * slot);
    if (array_on_stack) {
        emit_drop_obj(c, 0); /* the array reference the stack held */
    }
    end_plain(c, in->span);
}

static void emit_index_plain(Cx *c) {
    int bail = bail_label(c);
    Opd a = stack_opd(c->d - 2);
    Opd b = stack_opd(c->d - 1);
    emit_array_load(c, &a, &b, bail);
    emit_grab_item(c);
    stp_x(X, 3, 4, R_BPTR, 16 * (c->d - 2));
    emit_drop_obj(c, 0);
    end_plain(c, 1);
}

static void emit_set_index(Cx *c) {
    int bail = bail_label(c);
    int32_t arr = 16 * (c->d - 3);
    int32_t idx = 16 * (c->d - 2);
    int32_t val = 16 * (c->d - 1);
    ldr_w(X, 9, R_BPTR, arr);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, bail);
    ldr_x(X, 0, R_BPTR, arr + 8);
    ldr_w(X, 9, 0, O_OTYPE);
    cmp_iw(X, 9, LZ_OBJ_ARRAY);
    bcond_l(X, CD_NE, bail);
    ldr_w(X, 9, R_BPTR, idx);
    cmp_iw(X, 9, LZ_VAL_INT);
    bcond_l(X, CD_NE, bail);
    ldr_x(X, 1, R_BPTR, idx + 8);
    int positive = a_label(X);
    ldr_x(X, 2, 0, O_ARR_COUNT);
    cmp_i(X, 1, 0);
    bcond_l(X, CD_GE, positive);
    addsub_r(X, A_ADD, 1, 1, 2);
    a_bind(X, positive);
    cmp_r(X, 1, 2);
    bcond_l(X, CD_HS, bail);
    ldr_x(X, 9, 0, O_ARR_ELEM); /* typed arrays check the value: the reference's */
    cbnz_l(X, 9, bail);
    ldr_w(X, 9, R_BPTR, val);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_EQ, bail);
    lsl_i(X, 1, 1, 4);
    ldr_x(X, 9, 0, O_ARR_ITEMS);
    addsub_r(X, A_ADD, 1, 1, 9);
    ldr_w(X, 9, 1, 0);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_EQ, bail);
    ldp_x(X, 3, 4, R_BPTR, val);
    stp_x(X, 3, 4, 1, 0);
    emit_drop_obj(c, 0);
    end_plain(c, 1);
}

static void emit_get_global(Cx *c, uint16_t name_index) {
    int bail = bail_label(c);
    emit_global(c, P_A, name_index, &c->ir.sites[3 * c->i], true, bail);
    emit_copy_grab(c, P_A, 0, R_BPTR, 16 * c->d);
    end_plain(c, 1);
}

static void emit_set_global(Cx *c, uint16_t name_index) {
    int bail = bail_label(c);
    emit_global_store_cell(c, P_S, name_index, &c->ir.sites[3 * c->i], bail);
    ldp_x(X, 2, 0, R_BPTR, 16 * (c->d - 1));
    emit_store_drop(c, P_S, 0);
    end_plain(c, 1);
}

/* Runs the instruction through the reference semantics (lz_vm_exec_one). */
static void emit_delegate(Cx *c) {
    const GeIns *in = INS(c);
    emit_sync_sc(c, c->d);
    c->pend += 1;
    emit_flush(c);
    int fail = fail_label(c);
    mov_rr(X, 0, R_VM);
    mov_imm(X, 1, (uint64_t)(uintptr_t)c->ir.chunk);
    mov_imm(X, 2, in->offset);
    emit_call_abs(c, (uintptr_t)jit_delegate);
    cbnz_wl(X, 0, fail);
    finish_group(c, (uint32_t)(c->i + 1));
}

/*
 * The cold path of an instruction with an inline fast path: the reference semantics through
 * lz_vm_exec_one, then back to `back`. The hot path counts this instruction lazily (in
 * c->pend), so the cold path adds it for the call and takes it off again afterwards.
 */
static void emit_delegate_cold(Cx *c, int slow, int back) {
    const GeIns *in = INS(c);
    int fail = fail_label(c);
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    emit_sync_sc(c, c->d);
    add_imm(X, R_EXEC, R_EXEC, c->pend + 1);
    mov_rr(X, 0, R_VM);
    mov_imm(X, 1, (uint64_t)(uintptr_t)c->ir.chunk);
    mov_imm(X, 2, in->offset);
    emit_call_abs(c, (uintptr_t)jit_delegate);
    cbnz_wl(X, 0, fail);
    add_imm(X, R_EXEC, R_EXEC, -(c->pend + 1));
    b_l(X, back);
    a_section(X, saved);
}

/* CHECK_TYPE / CHECK_RETURN / CHECK_PARAM: a nil, bool, int or float the type accepts passes inline. */
static void emit_check(Cx *c, int32_t disp, uint16_t type_index) {
    if (type_index >= c->ir.chunk->type_count || (c->ir.chunk->types[type_index].kinds & 0xFu) == 0) {
        emit_delegate(c);
        return;
    }
    int slow = a_label(X);
    int done = a_label(X);
    ldr_w(X, 0, R_BPTR, disp);
    cmp_iw(X, 0, 3);
    bcond_l(X, CD_HI, slow); /* an object: arrays are checked element by element */
    mov_imm(X, 1, c->ir.chunk->types[type_index].kinds & 0xFu);
    lsrv_x(X, 1, 1, 0); /* x1 = kinds >> tag */
    mov_imm(X, 2, 1);
    logic_r(X, L_AND, 1, 1, 2);
    cbz_l(X, 1, slow); /* the kind is not in the type (a literal type, or a mismatch) */
    a_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

/* x0 = the closure of the running frame, or the slow path when there is none. */
static void emit_frame_closure(Cx *c, int slow) {
    ldr_x(X, 0, R_FC, A_FC);
    mov_imm(X, 15, (uint64_t)FS);
    mul_r(X, 0, 0, 15);
    addsub_r(X, A_ADD, 0, 0, R_SC);
    ldr_x(X, 0, 0, A_FRAMES - FS + FR_CLOSURE);
    cbz_l(X, 0, slow);
}

/* x5 = the location of the closure's upvalue `index` (slow path when out of range). */
static void emit_upvalue_location(Cx *c, uint16_t index, int slow) {
    emit_frame_closure(c, slow);
    ldr_h(X, 1, 0, O_CL_UPVC);
    mov_imm(X, 2, index);
    cmp_r(X, 1, 2);
    bcond_l(X, CD_LS, slow);
    ldr_x(X, 0, 0, O_CL_UPV + 8 * (int32_t)index);
    ldr_x(X, 5, 0, O_UPV_LOC);
}

static void emit_get_upvalue(Cx *c, uint16_t index) {
    int slow = a_label(X);
    int done = a_label(X);
    emit_upvalue_location(c, index, slow);
    emit_copy_grab(c, 5, 0, R_BPTR, 16 * c->d);
    a_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_set_upvalue(Cx *c, uint16_t index) {
    int slow = a_label(X);
    int done = a_label(X);
    emit_upvalue_location(c, index, slow);
    mov_rr(X, P_S, 5);
    ldp_x(X, 2, 0, R_BPTR, 16 * (c->d - 1));
    emit_store_drop(c, P_S, 0);
    a_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_get_self(Cx *c) {
    int slow = a_label(X);
    int done = a_label(X);
    emit_frame_closure(c, slow);
    emit_rc_inc(c, 0);
    mov_imm(X, 1, LZ_VAL_OBJECT);
    stp_x(X, 1, 0, R_BPTR, 16 * c->d);
    a_bind(X, done);
    emit_delegate_cold(c, slow, done);
    end_plain(c, 1);
}

static void emit_call(Cx *c, uint16_t argc) {
    const GeIns *in = INS(c);
    int d = c->d;
    int delta = d - (int)argc;
    int32_t callee = 16 * (d - 1 - (int)argc);
    int slow = a_label(X);
    int after = a_label(X);
    int enter_exit = a_label(X);
    int fail = fail_label(c);

    c->pend += 1;
    emit_flush(c);

    ldr_w(X, 9, R_BPTR, callee);
    cmp_iw(X, 9, LZ_VAL_OBJECT);
    bcond_l(X, CD_NE, slow);
    ldr_x(X, 0, R_BPTR, callee + 8);
    ldr_w(X, 9, 0, O_OTYPE);
    cmp_iw(X, 9, LZ_OBJ_CLOSURE);
    bcond_l(X, CD_NE, slow);
    ldr_x(X, 2, 0, O_CL_FN);
    ldr_h(X, 9, 2, O_FN_PARAMS);
    cmp_iw(X, 9, argc);
    bcond_l(X, CD_NE, slow);
    ldr_b(X, 9, 2, O_FN_REST);
    cbnz_wl(X, 9, slow);
    ldr_x(X, 3, 2, O_FN_CACHE);
    cbz_l(X, 3, slow);
    ldr_x(X, 3, 3, O_PR_JIT);
    cbz_l(X, 3, slow);
    ldr_x(X, 4, R_FC, A_FC);
    cmp_i(X, 4, LZ_FRAMES_MAX);
    bcond_l(X, CD_HS, slow);
    ldr_x(X, 5, 3, O_JIT_ENTRY);

    /* push the frame */
    mov_imm(X, 15, (uint64_t)FS);
    mul_r(X, 6, 4, 15);
    addsub_r(X, A_ADD, 6, 6, R_SC);
    mov_imm(X, 9, c->i + 1);
    str_x(X, 9, 6, A_FRAMES - FS + FR_IP);
    str_x(X, 0, 6, A_FRAMES + FR_CLOSURE);
    add_imm(X, 9, 2, O_FN_CHUNK);
    str_x(X, 9, 6, A_FRAMES + FR_CHUNK);
    str_x(X, XZR, 6, A_FRAMES + FR_IP);
    add_imm(X, 9, R_BIDX, delta);
    str_x(X, 9, 6, A_FRAMES + FR_BASE);
    mov_imm(X, 9, UINT64_MAX);
    str_x(X, 9, 6, A_FRAMES + FR_SUPPLIED);
    mov_imm(X, 9, in->offset);
    str_w(X, 9, 6, A_FRAMES + FR_CALLOFF);
    addsub_i(X, A_ADD, 4, 4, 1);
    str_x(X, 4, R_FC, A_FC);

    push_pair(X, R_BIDX, R_BPTR, 16);
    add_imm(X, R_BIDX, R_BIDX, delta);
    add_imm(X, R_BPTR, R_BPTR, 16 * delta);
    blr_r(X, 5);
    pop_pair(X, R_BIDX, R_BPTR, 16);
    a_bind(X, after);

    /* Everything else: natives, defaults, a rest parameter, errors, functions without native code. */
    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, slow);
    emit_sync_sc(c, d);
    mov_rr(X, 0, R_VM);
    mov_imm(X, 1, argc);
    mov_imm(X, 2, c->i + 1);
    mov_imm(X, 3, in->offset);
    add_imm(X, 4, SP, 8); /* the out slot for the callee's entry point */
    emit_call_abs(c, (uintptr_t)jit_call);
    lsr_i(X, 9, 0, 32); /* bit 32 of the result: a frame was pushed */
    cbnz_wl(X, 0, fail); /* the status */
    cbz_l(X, 9, after);
    ldr_x(X, 1, SP, 8);
    cbz_l(X, 1, enter_exit);
    ldr_x(X, 2, R_FC, A_FC);
    mov_imm(X, 15, (uint64_t)FS);
    mul_r(X, 2, 2, 15);
    addsub_r(X, A_ADD, 2, 2, R_SC);
    ldr_x(X, 3, 2, A_FRAMES - FS + FR_BASE);
    push_pair(X, R_BIDX, R_BPTR, 16);
    mov_rr(X, R_BIDX, 3);
    lsl_i(X, 3, 3, 4);
    addsub_r(X, A_ADD, R_BPTR, 3, R_VM);
    add_imm(X, R_BPTR, R_BPTR, O_STACK);
    blr_r(X, 1);
    pop_pair(X, R_BIDX, R_BPTR, 16);
    b_l(X, after);
    a_bind(X, enter_exit);
    emit_leave(c, GE_JX_ENTER, 0);
    a_section(X, saved);
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
    int halt = a_label(X);
    int ok = a_label(X);
    ldr_x(X, 0, R_FC, A_FC);
    cmp_i(X, 0, 1);
    bcond_l(X, CD_LS, halt);
    ldr_x(X, 0, R_FC, A_OPENUPV);
    cbz_l(X, 0, ok);
    ldr_x(X, 1, 0, O_UPV_LOC);
    cmp_r(X, 1, R_BPTR);
    bcond_l(X, CD_HS, bail); /* a variable of this frame was captured: the reference closes it */
    a_bind(X, ok);
    ldp_x(X, 13, 14, R_BPTR, 16 * (d - 1));
    if (d <= 8) {
        for (int k = d - 2; k >= -1; k--) {
            emit_drop_mem(c, R_BPTR, 16 * k, true);
        }
    } else {
        push_pair(X, 13, 14, 16);
        mov_rr(X, 0, R_VM);
        add_imm(X, 1, R_BPTR, -16);
        mov_imm(X, 2, (uint64_t)d);
        emit_call_abs(c, (uintptr_t)jit_drop_range);
        pop_pair(X, 13, 14, 16);
    }
    stp_x(X, 13, 14, R_BPTR, -16);
    ldr_x(X, 0, R_FC, A_FC);
    addsub_i(X, A_SUB, 0, 0, 1);
    str_x(X, 0, R_FC, A_FC);
    add_imm(X, R_EXEC, R_EXEC, c->pend + 1);
    pop_x(X, LR);
    ret_(X);

    int saved = X->sec;
    a_section(X, SEC_COLD);
    a_bind(X, halt);
    add_imm(X, R_EXEC, R_EXEC, c->pend + 1);
    emit_sync_sc(c, d);
    emit_leave(c, GE_JX_HALT, (uint32_t)c->i);
    a_section(X, saved);
    c->pend = 0;
}

static void emit_jump_if_supplied(Cx *c) {
    const GeIns *in = INS(c);
    uint16_t param = c->ir.oa[c->i];
    int l = c->lab[in->target];
    emit_flush(c);
    if (param >= 64) {
        b_l(X, l);
        return;
    }
    ldr_x(X, 0, R_FC, A_FC);
    mov_imm(X, 15, (uint64_t)FS);
    mul_r(X, 0, 0, 15);
    addsub_r(X, A_ADD, 0, 0, R_SC);
    ldr_x(X, 0, 0, A_FRAMES - FS + FR_SUPPLIED);
    mov_imm(X, 1, (uint64_t)1 << param);
    tst_r(X, 0, 1);
    bcond_l(X, CD_NE, l);
}

static void emit_instruction(Cx *c) {
    const GeIns *in = INS(c);
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
        mov_imm(X, 0, LZ_VAL_NIL);
        stp_x(X, 0, XZR, R_BPTR, 16 * d);
        end_plain(c, 1);
        break;
    case LZ_OP_TRUE:
    case LZ_OP_FALSE:
        mov_imm(X, 0, LZ_VAL_BOOL);
        mov_imm(X, 2, plain == LZ_OP_TRUE ? 1 : 0);
        stp_x(X, 0, 2, R_BPTR, 16 * d);
        end_plain(c, 1);
        break;
    case LZ_OP_POP:
        emit_drop_mem(c, R_BPTR, 16 * (d - 1), false);
        end_plain(c, 1);
        break;
    case LZ_OP_GET_LOCAL:
        emit_copy_grab(c, R_BPTR, 16 * (int32_t)a, R_BPTR, 16 * d);
        end_plain(c, 1);
        break;
    case LZ_OP_SET_LOCAL:
        ldp_x(X, 2, 0, R_BPTR, 16 * (d - 1));
        emit_store_drop(c, R_BPTR, 16 * (int32_t)a);
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
        b_l(X, c->lab[in->target]);
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

/* The reach of a conditional branch is +-1 MiB: bigger programs are left to the interpreter. */
#define A64_MAX_CODE ((size_t)1000000)

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
    a64_init(&c.x);
    struct GeJit *jit = NULL;
    uint8_t *image = NULL;
    c.lab = malloc((n + 1) * sizeof *c.lab);
    if (c.lab == NULL || !jit_ir_build(&c.ir, chunk, program, entry_depth)) {
        goto done;
    }

    for (size_t i = 0; i <= n; i++) {
        c.lab[i] = a_label(&c.x);
    }
    c.pend = 0;

    /* The entry used by native calls: a prologue, then the stack check, then instruction 0. */
    int entry_call = a_label(&c.x);
    a_bind(&c.x, entry_call);
    push_x(&c.x, LR);
    c.i = 0;
    c.d = (int)entry_depth;
    c.bail_lbl = -1;
    c.fail_lbl = -1;
    {
        A64 *x = &c.x;
        add_imm(x, 0, R_BIDX, (int64_t)c.ir.max_depth);
        mov_imm(x, 1, LZ_STACK_MAX);
        cmp_r(x, 0, 1);
        int entry_bail = a_label(x);
        bcond_l(x, CD_HI, entry_bail);
        int saved = x->sec;
        a_section(x, SEC_COLD);
        a_bind(x, entry_bail);
        emit_bail_stub(&c, 0, (int)entry_depth, 0);
        a_section(x, saved);
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
        a_bind(&c.x, c.lab[i]);
        emit_instruction(&c);
    }

    if (c.x.oom) {
        goto done;
    }
    size_t size = a64_size(&c.x);
    if (size > A64_MAX_CODE) {
        goto done;
    }
    image = malloc(size);
    if (image == NULL || !a64_link(&c.x, image)) {
        goto done;
    }

    {
        /* A debugging aid: LZ_JIT_DUMP=prefix writes each compiled program to prefix.N.bin,
         * for aarch64-linux-gnu-objdump -D -b binary -m aarch64. */
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
                            ? (uint8_t *)jit->mem + a64_label_address(&c.x, c.lab[i])
                            : NULL;
    }
    jit->entry_call = (uint8_t *)jit->mem + a64_label_address(&c.x, entry_call);

done:
    free(image);
    free(c.lab);
    jit_ir_free(&c.ir);
    a64_free(&c.x);
    return jit;
}

#endif /* GE_JIT_A64 */
