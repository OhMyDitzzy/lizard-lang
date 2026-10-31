#ifndef LIZARD_GE_A64_H
#define LIZARD_GE_A64_H

/*
 * A small AArch64 assembler: just the instructions the Gecko JIT emits. Like ge_x64.h it
 * writes into a hot and a cold section and resolves forward references at link time.
 * Every instruction is one 32-bit word; branch offsets are range checked when linking
 * (conditional branches reach +-1 MiB), and the compiler gives up on a program whose
 * code would not fit, which then runs on the interpreter.
 *
 * Register conventions (see ge_jit_a64.c): x0-x15 scratch, x16 call/immediate scratch,
 * x17 address scratch used by the load/store helpers below (never holds live data),
 * x18 untouched (reserved by the platform).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { XZR = 31, SP = 31, LR = 30 };

enum { CD_EQ = 0, CD_NE = 1, CD_HS = 2, CD_LO = 3, CD_MI = 4, CD_PL = 5, CD_VS = 6, CD_VC = 7,
       CD_HI = 8, CD_LS = 9, CD_GE = 10, CD_LT = 11, CD_GT = 12, CD_LE = 13 };

enum { SEC_HOT = 0, SEC_COLD = 1 };

enum { FIX_B26 = 0, FIX_B19 = 1, FIX_ADR21 = 2 };

typedef struct A64Pos {
    int sec; /* -1: not bound yet */
    size_t off;
} A64Pos;

typedef struct A64Fixup {
    int sec;
    size_t at; /* offset of the instruction word */
    int label;
    int kind;
} A64Fixup;

typedef struct A64 {
    uint32_t *buf[2];
    size_t len[2]; /* in words */
    size_t cap[2];
    int sec;
    bool oom;
    A64Pos *pos;
    size_t label_count;
    size_t label_cap;
    A64Fixup *fix;
    size_t fix_count;
    size_t fix_cap;
} A64;

static inline void a64_init(A64 *x) {
    memset(x, 0, sizeof *x);
}

static inline void a64_free(A64 *x) {
    free(x->buf[0]);
    free(x->buf[1]);
    free(x->pos);
    free(x->fix);
    memset(x, 0, sizeof *x);
}

static inline void a_w(A64 *x, uint32_t word) {
    int s = x->sec;
    if (x->len[s] == x->cap[s]) {
        size_t cap = x->cap[s] == 0 ? 512 : x->cap[s] * 2;
        uint32_t *grown = realloc(x->buf[s], cap * sizeof *grown);
        if (grown == NULL) {
            x->oom = true;
            return;
        }
        x->buf[s] = grown;
        x->cap[s] = cap;
    }
    x->buf[s][x->len[s]++] = word;
}

static inline void a_section(A64 *x, int sec) {
    x->sec = sec;
}

static inline int a_label(A64 *x) {
    if (x->label_count == x->label_cap) {
        size_t cap = x->label_cap == 0 ? 256 : x->label_cap * 2;
        A64Pos *grown = realloc(x->pos, cap * sizeof *grown);
        if (grown == NULL) {
            x->oom = true;
            return 0;
        }
        x->pos = grown;
        x->label_cap = cap;
    }
    x->pos[x->label_count].sec = -1;
    x->pos[x->label_count].off = 0;
    return (int)x->label_count++;
}

static inline void a_bind(A64 *x, int label) {
    x->pos[label].sec = x->sec;
    x->pos[label].off = x->len[x->sec] * 4;
}

/* Emits `word` and records that its offset field refers to `label`. */
static inline void a_ref(A64 *x, uint32_t word, int label, int kind) {
    if (x->fix_count == x->fix_cap) {
        size_t cap = x->fix_cap == 0 ? 256 : x->fix_cap * 2;
        A64Fixup *grown = realloc(x->fix, cap * sizeof *grown);
        if (grown == NULL) {
            x->oom = true;
            return;
        }
        x->fix = grown;
        x->fix_cap = cap;
    }
    x->fix[x->fix_count].sec = x->sec;
    x->fix[x->fix_count].at = x->len[x->sec] * 4;
    x->fix[x->fix_count].label = label;
    x->fix[x->fix_count].kind = kind;
    x->fix_count++;
    a_w(x, word);
}

/* ---- moves and constants ---- */

static inline void mov_rr(A64 *x, int rd, int rm) {
    if (rd == SP || rm == SP) {
        a_w(x, 0x91000000u | ((uint32_t)rm << 5) | (uint32_t)rd); /* add rd, rm, #0 */
    } else {
        a_w(x, 0xAA0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
    }
}

/* rd = a 64-bit constant, in as few movz/movn/movk as it takes. */
static inline void mov_imm(A64 *x, int rd, uint64_t v) {
    int zeros = 0;
    int ones = 0;
    for (int h = 0; h < 4; h++) {
        uint32_t part = (uint32_t)((v >> (16 * h)) & 0xFFFFu);
        zeros += part == 0;
        ones += part == 0xFFFFu;
    }
    bool use_n = ones > zeros;
    uint64_t base = use_n ? ~v : v;
    bool first = true;
    for (int h = 0; h < 4; h++) {
        uint32_t part = (uint32_t)((base >> (16 * h)) & 0xFFFFu);
        if (part == 0) {
            continue;
        }
        if (first) {
            a_w(x, (use_n ? 0x92800000u : 0xD2800000u) | ((uint32_t)h << 21) | (part << 5) | (uint32_t)rd);
            first = false;
        } else {
            uint32_t real = use_n ? (uint32_t)((v >> (16 * h)) & 0xFFFFu) : part;
            a_w(x, 0xF2800000u | ((uint32_t)h << 21) | (real << 5) | (uint32_t)rd);
        }
    }
    if (first) { /* every halfword was the filler: 0 (movz) or all ones (movn) */
        a_w(x, (use_n ? 0x92800000u : 0xD2800000u) | (uint32_t)rd);
    }
}

/* ---- arithmetic ---- */

enum { A_ADD = 0, A_ADDS = 1, A_SUB = 2, A_SUBS = 3 };

static inline uint32_t addsub_imm_base(int op, bool w) {
    static const uint32_t x64[4] = {0x91000000u, 0xB1000000u, 0xD1000000u, 0xF1000000u};
    return x64[op] & (w ? ~0x80000000u : ~0u);
}

/* rd = rn op #imm, imm in 0..4095 */
static inline void addsub_i(A64 *x, int op, int rd, int rn, uint32_t imm) {
    a_w(x, addsub_imm_base(op, false) | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void addsub_iw(A64 *x, int op, int rd, int rn, uint32_t imm) {
    a_w(x, addsub_imm_base(op, true) | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void addsub_r(A64 *x, int op, int rd, int rn, int rm) {
    static const uint32_t base[4] = {0x8B000000u, 0xAB000000u, 0xCB000000u, 0xEB000000u};
    a_w(x, base[op] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* rd = rn +/- imm for any imm (x17 is used for the large ones; rn must not be SP then). */
static inline void add_imm(A64 *x, int rd, int rn, int64_t imm) {
    int op = A_ADD;
    uint64_t mag = (uint64_t)imm;
    if (imm < 0) {
        op = A_SUB;
        mag = (uint64_t)(-imm);
    }
    if (mag < 4096) {
        addsub_i(x, op, rd, rn, (uint32_t)mag);
    } else if ((mag & 0xFFFu) == 0 && (mag >> 12) < 4096) {
        a_w(x, addsub_imm_base(op, false) | (1u << 22) | ((uint32_t)(mag >> 12) << 10) |
                   ((uint32_t)rn << 5) | (uint32_t)rd);
    } else {
        mov_imm(x, 17, mag);
        addsub_r(x, op, rd, rn, 17);
    }
}

static inline void cmp_i(A64 *x, int rn, uint32_t imm) { addsub_i(x, A_SUBS, XZR, rn, imm); }
static inline void cmn_i(A64 *x, int rn, uint32_t imm) { addsub_i(x, A_ADDS, XZR, rn, imm); }
static inline void cmp_iw(A64 *x, int rn, uint32_t imm) { addsub_iw(x, A_SUBS, XZR, rn, imm); }
static inline void cmp_r(A64 *x, int rn, int rm) { addsub_r(x, A_SUBS, XZR, rn, rm); }

/* cmp rn, rm, asr #63 */
static inline void cmp_r_asr63(A64 *x, int rn, int rm) {
    a_w(x, 0xEB000000u | (2u << 22) | ((uint32_t)rm << 16) | (63u << 10) | ((uint32_t)rn << 5) | XZR);
}

static inline void neg_s(A64 *x, int rd, int rm) { /* negs: sets V for INT64_MIN */
    a_w(x, 0xEB0003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
}

enum { L_AND = 0, L_ORR = 1, L_EOR = 2, L_ANDS = 3 };

static inline void logic_r(A64 *x, int op, int rd, int rn, int rm) {
    static const uint32_t base[4] = {0x8A000000u, 0xAA000000u, 0xCA000000u, 0xEA000000u};
    a_w(x, base[op] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void tst_r(A64 *x, int rn, int rm) { logic_r(x, L_ANDS, XZR, rn, rm); }

static inline void mul_r(A64 *x, int rd, int rn, int rm) {
    a_w(x, 0x9B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void smulh_r(A64 *x, int rd, int rn, int rm) {
    a_w(x, 0x9B407C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void sdiv_r(A64 *x, int rd, int rn, int rm) {
    a_w(x, 0x9AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* rd = ra - rn * rm */
static inline void msub_r(A64 *x, int rd, int rn, int rm, int ra) {
    a_w(x, 0x9B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void lsrv_x(A64 *x, int rd, int rn, int rm) { /* rd = rn >> (rm & 63) */
    a_w(x, 0x9AC02400u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void lslv_x(A64 *x, int rd, int rn, int rm) { /* rd = rn << (rm & 63) */
    a_w(x, 0x9AC02000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void asrv_x(A64 *x, int rd, int rn, int rm) { /* rd = rn >> (rm & 63), sign-filling */
    a_w(x, 0x9AC02800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void mvn_x(A64 *x, int rd, int rm) { /* rd = ~rm (orn rd, xzr, rm) */
    a_w(x, 0xAA2003E0u | ((uint32_t)rm << 16) | (uint32_t)rd);
}

static inline void lsl_i(A64 *x, int rd, int rn, int s) {
    a_w(x, 0xD3400000u | ((uint32_t)((64 - s) & 63) << 16) | ((uint32_t)(63 - s) << 10) |
               ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void lsr_i(A64 *x, int rd, int rn, int s) {
    a_w(x, 0xD3400000u | ((uint32_t)s << 16) | (63u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static inline void asr_i(A64 *x, int rd, int rn, int s) {
    a_w(x, 0x93400000u | ((uint32_t)s << 16) | (63u << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/* ---- loads and stores ---- */

enum { SZ_B = 0, SZ_H = 1, SZ_W = 2, SZ_X = 3 };

/*
 * Load or store `size` bytes at [rn + off]. Uses the scaled form when it fits, the unscaled
 * form for small negative or odd offsets, and x17 as an offset register otherwise.
 */
static inline void a_ldst(A64 *x, int size, bool load, int rt, int rn, int32_t off) {
    static const uint32_t uimm_load[4] = {0x39400000u, 0x79400000u, 0xB9400000u, 0xF9400000u};
    static const uint32_t uimm_store[4] = {0x39000000u, 0x79000000u, 0xB9000000u, 0xF9000000u};
    static const uint32_t unsc_load[4] = {0x38400000u, 0x78400000u, 0xB8400000u, 0xF8400000u};
    static const uint32_t unsc_store[4] = {0x38000000u, 0x78000000u, 0xB8000000u, 0xF8000000u};
    static const uint32_t reg_load[4] = {0x38606800u, 0x78606800u, 0xB8606800u, 0xF8606800u};
    static const uint32_t reg_store[4] = {0x38206800u, 0x78206800u, 0xB8206800u, 0xF8206800u};
    int32_t scale = 1 << size;
    if (off >= 0 && off % scale == 0 && off / scale < 4096) {
        a_w(x, (load ? uimm_load[size] : uimm_store[size]) | ((uint32_t)(off / scale) << 10) |
                   ((uint32_t)rn << 5) | (uint32_t)rt);
    } else if (off >= -256 && off <= 255) {
        a_w(x, (load ? unsc_load[size] : unsc_store[size]) | (((uint32_t)off & 0x1FFu) << 12) |
                   ((uint32_t)rn << 5) | (uint32_t)rt);
    } else {
        mov_imm(x, 17, (uint64_t)(int64_t)off);
        a_w(x, (load ? reg_load[size] : reg_store[size]) | (17u << 16) | ((uint32_t)rn << 5) | (uint32_t)rt);
    }
}

static inline void ldr_x(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_X, true, rt, rn, off); }
static inline void str_x(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_X, false, rt, rn, off); }
static inline void ldr_w(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_W, true, rt, rn, off); }
static inline void str_w(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_W, false, rt, rn, off); }
static inline void ldr_h(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_H, true, rt, rn, off); }
static inline void ldr_b(A64 *x, int rt, int rn, int32_t off) { a_ldst(x, SZ_B, true, rt, rn, off); }

/* ldp/stp of two 64-bit registers at [rn + off] (off a multiple of 8). */
static inline void a_pair(A64 *x, bool load, int rt, int rt2, int rn, int32_t off) {
    if (off % 8 == 0 && off >= -512 && off <= 504) {
        a_w(x, (load ? 0xA9400000u : 0xA9000000u) | (((uint32_t)(off / 8) & 0x7Fu) << 15) |
                   ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
    } else {
        add_imm(x, 17, rn, off); /* rn is never SP here */
        a_w(x, (load ? 0xA9400000u : 0xA9000000u) | ((uint32_t)rt2 << 10) | (17u << 5) | (uint32_t)rt);
    }
}

static inline void ldp_x(A64 *x, int rt, int rt2, int rn, int32_t off) { a_pair(x, true, rt, rt2, rn, off); }
static inline void stp_x(A64 *x, int rt, int rt2, int rn, int32_t off) { a_pair(x, false, rt, rt2, rn, off); }

/* stp rt, rt2, [sp, #-size]! and ldp rt, rt2, [sp], #size (size a multiple of 16, at most 504). */
static inline void push_pair(A64 *x, int rt, int rt2, int size) {
    a_w(x, 0xA9800000u | (((uint32_t)(-size / 8) & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt);
}

static inline void pop_pair(A64 *x, int rt, int rt2, int size) {
    a_w(x, 0xA8C00000u | (((uint32_t)(size / 8) & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt);
}

static inline void push_x(A64 *x, int rt) { /* str rt, [sp, #-16]! */
    a_w(x, 0xF8000C00u | (((uint32_t)-16 & 0x1FFu) << 12) | (31u << 5) | (uint32_t)rt);
}

static inline void pop_x(A64 *x, int rt) { /* ldr rt, [sp], #16 */
    a_w(x, 0xF8400400u | (16u << 12) | (31u << 5) | (uint32_t)rt);
}

/* ---- control flow ---- */

static inline void b_l(A64 *x, int label) { a_ref(x, 0x14000000u, label, FIX_B26); }
static inline void bcond_l(A64 *x, int cond, int label) { a_ref(x, 0x54000000u | (uint32_t)cond, label, FIX_B19); }
static inline void cbz_l(A64 *x, int rt, int label) { a_ref(x, 0xB4000000u | (uint32_t)rt, label, FIX_B19); }
static inline void cbnz_l(A64 *x, int rt, int label) { a_ref(x, 0xB5000000u | (uint32_t)rt, label, FIX_B19); }
static inline void cbz_wl(A64 *x, int rt, int label) { a_ref(x, 0x34000000u | (uint32_t)rt, label, FIX_B19); }
static inline void cbnz_wl(A64 *x, int rt, int label) { a_ref(x, 0x35000000u | (uint32_t)rt, label, FIX_B19); }
static inline void adr_l(A64 *x, int rd, int label) { a_ref(x, 0x10000000u | (uint32_t)rd, label, FIX_ADR21); }
static inline void blr_r(A64 *x, int rn) { a_w(x, 0xD63F0000u | ((uint32_t)rn << 5)); }
static inline void br_r(A64 *x, int rn) { a_w(x, 0xD61F0000u | ((uint32_t)rn << 5)); }
static inline void ret_(A64 *x) { a_w(x, 0xD65F03C0u); }

/* ---- floating point (doubles in d0-d31) ---- */

enum { F_ADD = 0x2800, F_SUB = 0x3800, F_MUL = 0x0800, F_DIV = 0x1800 };

static inline void fop(A64 *x, int op, int dd, int dn, int dm) {
    a_w(x, 0x1E600000u | (uint32_t)op | ((uint32_t)dm << 16) | ((uint32_t)dn << 5) | (uint32_t)dd);
}

static inline void fmov_dx(A64 *x, int dd, int xn) { a_w(x, 0x9E670000u | ((uint32_t)xn << 5) | (uint32_t)dd); }
static inline void fmov_xd(A64 *x, int xd, int dn) { a_w(x, 0x9E660000u | ((uint32_t)dn << 5) | (uint32_t)xd); }
static inline void scvtf_dx(A64 *x, int dd, int xn) { a_w(x, 0x9E620000u | ((uint32_t)xn << 5) | (uint32_t)dd); }
static inline void fcvtzs_xd(A64 *x, int xd, int dn) { a_w(x, 0x9E780000u | ((uint32_t)dn << 5) | (uint32_t)xd); }
static inline void fcmp_dd(A64 *x, int dn, int dm) { a_w(x, 0x1E602000u | ((uint32_t)dm << 16) | ((uint32_t)dn << 5)); }
static inline void fcmp_d0(A64 *x, int dn) { a_w(x, 0x1E602008u | ((uint32_t)dn << 5)); }

/* ---- linking ---- */

static inline size_t a64_size(const A64 *x) { /* bytes; the cold section follows the hot one */
    return (x->len[0] + x->len[1]) * 4;
}

static inline size_t a64_label_address(const A64 *x, int label) {
    const A64Pos *p = &x->pos[label];
    return (p->sec == 1 ? x->len[0] * 4 : 0) + p->off;
}

/* Lays the sections out in out[0..a64_size) and resolves every reference. False on failure. */
static inline bool a64_link(const A64 *x, uint8_t *out) {
    if (x->oom) {
        return false;
    }
    size_t cold_at = x->len[0] * 4;
    if (x->len[0] > 0) {
        memcpy(out, x->buf[0], x->len[0] * 4);
    }
    if (x->len[1] > 0) {
        memcpy(out + cold_at, x->buf[1], x->len[1] * 4);
    }
    for (size_t i = 0; i < x->fix_count; i++) {
        const A64Fixup *f = &x->fix[i];
        const A64Pos *p = &x->pos[f->label];
        if (p->sec < 0) {
            return false;
        }
        int64_t target = (int64_t)((p->sec == 1 ? cold_at : 0) + p->off);
        int64_t at = (int64_t)((f->sec == 1 ? cold_at : 0) + f->at);
        int64_t delta = target - at;
        uint32_t word;
        memcpy(&word, out + at, 4);
        if (f->kind == FIX_B26) {
            if (delta < -((int64_t)1 << 27) || delta >= ((int64_t)1 << 27)) {
                return false;
            }
            word |= (uint32_t)((delta >> 2) & 0x3FFFFFF);
        } else if (f->kind == FIX_B19) {
            if (delta < -((int64_t)1 << 20) || delta >= ((int64_t)1 << 20)) {
                return false;
            }
            word |= ((uint32_t)((delta >> 2) & 0x7FFFF)) << 5;
        } else {
            if (delta < -((int64_t)1 << 20) || delta >= ((int64_t)1 << 20)) {
                return false;
            }
            word |= (((uint32_t)delta & 3u) << 29) | ((((uint32_t)(delta >> 2)) & 0x7FFFFu) << 5);
        }
        memcpy(out + at, &word, 4);
    }
    return true;
}

#endif
