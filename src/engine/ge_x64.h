#ifndef LIZARD_GE_X64_H
#define LIZARD_GE_X64_H

/*
 * A small x86-64 assembler: just the instructions the Gecko JIT emits. Code goes
 * into two sections (hot and cold) so rarely taken paths (type fallbacks, bail-out
 * stubs, slow calls) stay out of the straight-line code. Labels may be used before
 * they are bound; x64_link resolves every rel32 reference once the sections have
 * been laid out one after the other.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};

enum { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_S = 8, CC_NS = 9,
       CC_P = 10, CC_NP = 11, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15, CC_O = 0 };

enum { ALU_ADD = 0, ALU_OR = 1, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7 };

enum { SEC_HOT = 0, SEC_COLD = 1 };

typedef struct X64Pos {
    int sec; /* -1: not bound yet */
    size_t off;
} X64Pos;

typedef struct X64Fixup {
    int sec;
    size_t at; /* offset of the rel32 field */
    int label;
} X64Fixup;

typedef struct X64 {
    uint8_t *buf[2];
    size_t len[2];
    size_t cap[2];
    int sec;
    bool oom;
    X64Pos *pos;
    size_t label_count;
    size_t label_cap;
    X64Fixup *fix;
    size_t fix_count;
    size_t fix_cap;
} X64;

static inline void x64_init(X64 *x) {
    memset(x, 0, sizeof *x);
}

static inline void x64_free(X64 *x) {
    free(x->buf[0]);
    free(x->buf[1]);
    free(x->pos);
    free(x->fix);
    memset(x, 0, sizeof *x);
}

static inline void x_b(X64 *x, unsigned v) {
    int s = x->sec;
    if (x->len[s] == x->cap[s]) {
        size_t cap = x->cap[s] == 0 ? 1024 : x->cap[s] * 2;
        uint8_t *grown = realloc(x->buf[s], cap);
        if (grown == NULL) {
            x->oom = true;
            return;
        }
        x->buf[s] = grown;
        x->cap[s] = cap;
    }
    x->buf[s][x->len[s]++] = (uint8_t)v;
}

static inline void x_d(X64 *x, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        x_b(x, (v >> (8 * i)) & 0xFF);
    }
}

static inline void x_q(X64 *x, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        x_b(x, (unsigned)((v >> (8 * i)) & 0xFF));
    }
}

static inline void x_section(X64 *x, int sec) {
    x->sec = sec;
}

/* ---- labels ---- */

static inline int x_label(X64 *x) {
    if (x->label_count == x->label_cap) {
        size_t cap = x->label_cap == 0 ? 256 : x->label_cap * 2;
        X64Pos *grown = realloc(x->pos, cap * sizeof *grown);
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

static inline void x_bind(X64 *x, int label) {
    x->pos[label].sec = x->sec;
    x->pos[label].off = x->len[x->sec];
}

static inline void x_fixup(X64 *x, int label) {
    if (x->fix_count == x->fix_cap) {
        size_t cap = x->fix_cap == 0 ? 256 : x->fix_cap * 2;
        X64Fixup *grown = realloc(x->fix, cap * sizeof *grown);
        if (grown == NULL) {
            x->oom = true;
            return;
        }
        x->fix = grown;
        x->fix_cap = cap;
    }
    x->fix[x->fix_count].sec = x->sec;
    x->fix[x->fix_count].at = x->len[x->sec];
    x->fix[x->fix_count].label = label;
    x->fix_count++;
    x_d(x, 0);
}

/* ---- encoding helpers ---- */

static inline void x_rex(X64 *x, int w, int reg, int rm) {
    unsigned r = 0x40u | ((unsigned)w << 3) | (((unsigned)reg >> 3) & 1u) << 2 | (((unsigned)rm >> 3) & 1u);
    if (r != 0x40u) {
        x_b(x, r);
    }
}

static inline void x_mrr(X64 *x, int reg, int rm) {
    x_b(x, 0xC0u | (((unsigned)reg & 7u) << 3) | ((unsigned)rm & 7u));
}

static inline void x_mrm(X64 *x, int reg, int base, int32_t disp) {
    unsigned b = (unsigned)base & 7u;
    unsigned mod = (disp == 0 && b != 5) ? 0u : (disp >= -128 && disp <= 127 ? 1u : 2u);
    x_b(x, (mod << 6) | (((unsigned)reg & 7u) << 3) | (b == 4 ? 4u : b));
    if (b == 4) {
        x_b(x, 0x24);
    }
    if (mod == 1) {
        x_b(x, (unsigned)disp & 0xFFu);
    } else if (mod == 2) {
        x_d(x, (uint32_t)disp);
    }
}

/* ---- moves ---- */

static inline void mov_rr(X64 *x, int d, int s) {
    x_rex(x, 1, s, d);
    x_b(x, 0x89);
    x_mrr(x, s, d);
}

static inline void mov_rm(X64 *x, int d, int b, int32_t o) {
    x_rex(x, 1, d, b);
    x_b(x, 0x8B);
    x_mrm(x, d, b, o);
}

static inline void mov_mr(X64 *x, int b, int32_t o, int s) {
    x_rex(x, 1, s, b);
    x_b(x, 0x89);
    x_mrm(x, s, b, o);
}

static inline void mov_rm32(X64 *x, int d, int b, int32_t o) {
    x_rex(x, 0, d, b);
    x_b(x, 0x8B);
    x_mrm(x, d, b, o);
}

static inline void mov_mr32(X64 *x, int b, int32_t o, int s) {
    x_rex(x, 0, s, b);
    x_b(x, 0x89);
    x_mrm(x, s, b, o);
}

static inline void mov_mi32(X64 *x, int b, int32_t o, int32_t imm) {
    x_rex(x, 0, 0, b);
    x_b(x, 0xC7);
    x_mrm(x, 0, b, o);
    x_d(x, (uint32_t)imm);
}

/* qword store of a sign-extended imm32 */
static inline void mov_mi64(X64 *x, int b, int32_t o, int32_t imm) {
    x_rex(x, 1, 0, b);
    x_b(x, 0xC7);
    x_mrm(x, 0, b, o);
    x_d(x, (uint32_t)imm);
}

static inline void mov_ri(X64 *x, int d, uint64_t imm) {
    if (imm <= UINT32_MAX) {
        x_rex(x, 0, 0, d);
        x_b(x, 0xB8u | ((unsigned)d & 7u));
        x_d(x, (uint32_t)imm);
    } else if ((int64_t)imm == (int64_t)(int32_t)imm) {
        x_rex(x, 1, 0, d);
        x_b(x, 0xC7);
        x_mrr(x, 0, d);
        x_d(x, (uint32_t)imm);
    } else {
        x_rex(x, 1, 0, d);
        x_b(x, 0xB8u | ((unsigned)d & 7u));
        x_q(x, imm);
    }
}

static inline void movzx_rm8(X64 *x, int d, int b, int32_t o) {
    x_rex(x, 0, d, b);
    x_b(x, 0x0F);
    x_b(x, 0xB6);
    x_mrm(x, d, b, o);
}

static inline void lea(X64 *x, int d, int b, int32_t o) {
    x_rex(x, 1, d, b);
    x_b(x, 0x8D);
    x_mrm(x, d, b, o);
}

/* ---- arithmetic and logic ---- */

static inline void alu_rr(X64 *x, int op, int d, int s) { /* d = d op s */
    x_rex(x, 1, s, d);
    x_b(x, ((unsigned)op << 3) | 1u);
    x_mrr(x, s, d);
}

static inline void alu_rm(X64 *x, int op, int d, int b, int32_t o) { /* d = d op [b+o] */
    x_rex(x, 1, d, b);
    x_b(x, ((unsigned)op << 3) | 3u);
    x_mrm(x, d, b, o);
}

static inline void alu_mr(X64 *x, int op, int b, int32_t o, int s) { /* [b+o] = [b+o] op s */
    x_rex(x, 1, s, b);
    x_b(x, ((unsigned)op << 3) | 1u);
    x_mrm(x, s, b, o);
}

static inline void alu_ri(X64 *x, int w, int op, int d, int32_t imm) {
    x_rex(x, w, 0, d);
    if (imm >= -128 && imm <= 127) {
        x_b(x, 0x83);
        x_mrr(x, op, d);
        x_b(x, (unsigned)imm & 0xFFu);
    } else {
        x_b(x, 0x81);
        x_mrr(x, op, d);
        x_d(x, (uint32_t)imm);
    }
}

static inline void alu_mi(X64 *x, int w, int op, int b, int32_t o, int32_t imm) {
    x_rex(x, w, 0, b);
    if (imm >= -128 && imm <= 127) {
        x_b(x, 0x83);
        x_mrm(x, op, b, o);
        x_b(x, (unsigned)imm & 0xFFu);
    } else {
        x_b(x, 0x81);
        x_mrm(x, op, b, o);
        x_d(x, (uint32_t)imm);
    }
}

static inline void cmp_mi8(X64 *x, int b, int32_t o, int32_t imm) { /* cmp byte [b+o], imm8 */
    x_rex(x, 0, 0, b);
    x_b(x, 0x80);
    x_mrm(x, 7, b, o);
    x_b(x, (unsigned)imm & 0xFFu);
}

static inline void cmp_mi16(X64 *x, int b, int32_t o, int32_t imm) { /* cmp word [b+o], imm16 */
    x_b(x, 0x66);
    x_rex(x, 0, 0, b);
    x_b(x, 0x81);
    x_mrm(x, 7, b, o);
    x_b(x, (unsigned)imm & 0xFFu);
    x_b(x, ((unsigned)imm >> 8) & 0xFFu);
}

static inline void test_rr(X64 *x, int a, int b) {
    x_rex(x, 1, b, a);
    x_b(x, 0x85);
    x_mrr(x, b, a);
}

static inline void test_rr32(X64 *x, int a, int b) {
    x_rex(x, 0, b, a);
    x_b(x, 0x85);
    x_mrr(x, b, a);
}

static inline void bt_rr32(X64 *x, int base, int bit) { /* CF = bit `bit` of base (32-bit) */
    x_rex(x, 0, bit, base);
    x_b(x, 0x0F);
    x_b(x, 0xA3);
    x_mrr(x, bit, base);
}

static inline void imul_rr(X64 *x, int d, int s) {
    x_rex(x, 1, d, s);
    x_b(x, 0x0F);
    x_b(x, 0xAF);
    x_mrr(x, d, s);
}

static inline void imul_rri(X64 *x, int d, int s, int32_t imm) {
    x_rex(x, 1, d, s);
    x_b(x, 0x69);
    x_mrr(x, d, s);
    x_d(x, (uint32_t)imm);
}

static inline void neg_r(X64 *x, int r) {
    x_rex(x, 1, 0, r);
    x_b(x, 0xF7);
    x_mrr(x, 3, r);
}

static inline void not_r(X64 *x, int r) {
    x_rex(x, 1, 0, r);
    x_b(x, 0xF7);
    x_mrr(x, 2, r);
}

static inline void shift_cl(X64 *x, int ext, int r) { /* shift r by cl; ext: 4 shl, 5 shr, 7 sar */
    x_rex(x, 1, 0, r);
    x_b(x, 0xD3);
    x_mrr(x, ext, r);
}

static inline void idiv_r(X64 *x, int r) {
    x_rex(x, 1, 0, r);
    x_b(x, 0xF7);
    x_mrr(x, 7, r);
}

static inline void cqo(X64 *x) {
    x_b(x, 0x48);
    x_b(x, 0x99);
}

static inline void shift_ri(X64 *x, int ext, int r, int amount) { /* ext: 4 shl, 5 shr, 7 sar */
    x_rex(x, 1, 0, r);
    x_b(x, 0xC1);
    x_mrr(x, ext, r);
    x_b(x, (unsigned)amount);
}

static inline void inc_m32(X64 *x, int b, int32_t o) {
    x_rex(x, 0, 0, b);
    x_b(x, 0xFF);
    x_mrm(x, 0, b, o);
}

static inline void dec_m32(X64 *x, int b, int32_t o) {
    x_rex(x, 0, 0, b);
    x_b(x, 0xFF);
    x_mrm(x, 1, b, o);
}

static inline void dec_m64(X64 *x, int b, int32_t o) {
    x_rex(x, 1, 0, b);
    x_b(x, 0xFF);
    x_mrm(x, 1, b, o);
}

static inline void inc_r(X64 *x, int r) {
    x_rex(x, 1, 0, r);
    x_b(x, 0xFF);
    x_mrr(x, 0, r);
}

static inline void dec_r(X64 *x, int r) {
    x_rex(x, 1, 0, r);
    x_b(x, 0xFF);
    x_mrr(x, 1, r);
}

static inline void setcc_al(X64 *x, int cc) {
    x_b(x, 0x0F);
    x_b(x, 0x90u + (unsigned)cc);
    x_b(x, 0xC0); /* al */
}

/* ---- control flow ---- */

static inline void jmp_l(X64 *x, int label) {
    x_b(x, 0xE9);
    x_fixup(x, label);
}

static inline void jcc_l(X64 *x, int cc, int label) {
    x_b(x, 0x0F);
    x_b(x, 0x80u + (unsigned)cc);
    x_fixup(x, label);
}

static inline void call_r(X64 *x, int r) {
    x_rex(x, 0, 0, r);
    x_b(x, 0xFF);
    x_mrr(x, 2, r);
}

static inline void call_m(X64 *x, int b, int32_t o) {
    x_rex(x, 0, 0, b);
    x_b(x, 0xFF);
    x_mrm(x, 2, b, o);
}

static inline void jmp_r(X64 *x, int r) {
    x_rex(x, 0, 0, r);
    x_b(x, 0xFF);
    x_mrr(x, 4, r);
}

static inline void ret(X64 *x) {
    x_b(x, 0xC3);
}

static inline void push_r(X64 *x, int r) {
    x_rex(x, 0, 0, r);
    x_b(x, 0x50u | ((unsigned)r & 7u));
}

static inline void pop_r(X64 *x, int r) {
    x_rex(x, 0, 0, r);
    x_b(x, 0x58u | ((unsigned)r & 7u));
}

static inline void lea_label(X64 *x, int d, int label) { /* lea d, [rip + label] */
    x_rex(x, 1, d, 0);
    x_b(x, 0x8D);
    x_b(x, ((unsigned)d & 7u) << 3 | 5u);
    x_fixup(x, label);
}

/* ---- SSE2 ---- */

static inline void sse_rr(X64 *x, int prefix, int w, int op, int reg, int rm) {
    if (prefix != 0) {
        x_b(x, (unsigned)prefix);
    }
    x_rex(x, w, reg, rm);
    x_b(x, 0x0F);
    x_b(x, (unsigned)op);
    x_mrr(x, reg, rm);
}

static inline void sse_rm(X64 *x, int prefix, int w, int op, int reg, int b, int32_t o) {
    if (prefix != 0) {
        x_b(x, (unsigned)prefix);
    }
    x_rex(x, w, reg, b);
    x_b(x, 0x0F);
    x_b(x, (unsigned)op);
    x_mrm(x, reg, b, o);
}

enum { SSE_ADD = 0x58, SSE_MUL = 0x59, SSE_SUB = 0x5C, SSE_DIV = 0x5E };

static inline void movsd_xm(X64 *x, int xr, int b, int32_t o) { sse_rm(x, 0xF2, 0, 0x10, xr, b, o); }
static inline void movsd_mx(X64 *x, int b, int32_t o, int xr) { sse_rm(x, 0xF2, 0, 0x11, xr, b, o); }
static inline void movq_xr(X64 *x, int xr, int r) { sse_rr(x, 0x66, 1, 0x6E, xr, r); }
static inline void movq_rx(X64 *x, int r, int xr) { sse_rr(x, 0x66, 1, 0x7E, xr, r); }
static inline void cvtsi2sd_xr(X64 *x, int xr, int r) { sse_rr(x, 0xF2, 1, 0x2A, xr, r); }
static inline void cvtsi2sd_xm(X64 *x, int xr, int b, int32_t o) { sse_rm(x, 0xF2, 1, 0x2A, xr, b, o); }
static inline void cvttsd2si_rx(X64 *x, int r, int xr) { sse_rr(x, 0xF2, 1, 0x2C, r, xr); }
static inline void sse_arith(X64 *x, int op, int d, int s) { sse_rr(x, 0xF2, 0, op, d, s); }
static inline void ucomisd_xx(X64 *x, int a, int b) { sse_rr(x, 0x66, 0, 0x2E, a, b); }
static inline void xorpd_xx(X64 *x, int a, int b) { sse_rr(x, 0x66, 0, 0x57, a, b); }

/* ---- linking ---- */

/*
 * Lays the hot section then the cold one (16-byte aligned) into out[0..total) and
 * resolves every fixup. Returns total size, or 0 on failure (out of memory or an
 * unbound label). out may be NULL to only measure.
 */
static inline size_t x64_size(const X64 *x) {
    size_t cold_at = (x->len[0] + 15u) & ~(size_t)15u;
    return cold_at + x->len[1];
}

static inline bool x64_link(const X64 *x, uint8_t *out) {
    if (x->oom) {
        return false;
    }
    size_t base[2] = {0, (x->len[0] + 15u) & ~(size_t)15u};
    memset(out, 0xCC, x64_size(x));
    memcpy(out, x->buf[0], x->len[0]);
    if (x->len[1] > 0) {
        memcpy(out + base[1], x->buf[1], x->len[1]);
    }
    for (size_t i = 0; i < x->fix_count; i++) {
        const X64Fixup *f = &x->fix[i];
        const X64Pos *p = &x->pos[f->label];
        if (p->sec < 0) {
            return false;
        }
        int64_t target = (int64_t)(base[p->sec] + p->off);
        int64_t from = (int64_t)(base[f->sec] + f->at + 4);
        int32_t rel = (int32_t)(target - from);
        memcpy(out + base[f->sec] + f->at, &rel, 4);
    }
    return true;
}

static inline size_t x64_label_address(const X64 *x, int label) {
    size_t cold_at = (x->len[0] + 15u) & ~(size_t)15u;
    const X64Pos *p = &x->pos[label];
    return (p->sec == 1 ? cold_at : 0) + p->off;
}

#endif
