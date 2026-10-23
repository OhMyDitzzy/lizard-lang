#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_compare.h"
#include "lizard/engine.h"
#include "test_util.h"

/*
 * The Gecko JIT. Every program runs on the reference interpreter and on GE with the
 * JIT compiling everything on first sight (eager) and with the normal hotness
 * threshold; all must agree on output, errors, error offsets, instruction counts and
 * heap statistics. Programs that must really run as machine code are checked for it.
 */

typedef struct Case {
    const char *name;
    const char *text;
    bool native; /* must enter machine code */
} Case;

static const Case programs[] = {
    /* ---- loops on ints, floats, mixed ---- */
    {"int loop", "{ let i = 0\n let s = 0\n while i < 1000 { s = s + i * 2 - 1\n i = i + 1 }\n println(s) }\n", true},
    {"global loop", "let i = 0\nlet s = 0\nwhile i < 1000 { s = s + i\n i = i + 1 }\nprintln(s)\n", true},
    {"float loop", "{ let x = 0.5\n let i = 0\n while i < 500 { x = x * 1.01 + 0.25 / 3\n i = i + 1 }\n println(x) }\n", true},
    {"mixed loop", "{ let x = 1\n let i = 0\n while i < 100 { x = x + 0.5\n i = i + 1 }\n println(x, x == 51, x > 50, x <= 51.0) }\n", true},
    {"int then float", "{ let x = 1\n let i = 0\n while i < 200 { x = x * 2\n i = i + 1\n if i == 62 { x = x / 3 } }\n println(x) }\n", true},
    {"overflow mid loop", "{ let x = 1\n let i = 0\n while i < 100 { x = x * 3\n i = i + 1 }\n println(x) }\n", true},
    {"overflow add global", "let x = 9223372036854775800\nlet i = 0\nwhile i < 20 { x = x + 1\n i = i + 1 }\nprintln(x)\n", true},
    {"modulo and floor", "{ let i = -20\n let s = 0\n while i < 20 { s = s + i % 7 + i // 3 + i % 8 + i // 4 + i % -3\n i = i + 1 }\n println(s) }\n", true},
    {"modulo power of two", "{ let i = -40\n while i < 40 { println(i % 2, i % 16, i // 2, i // 16)\n i = i + 7 } }\n", true},
    {"float modulo loop", "{ let i = 0.0\n let s = 0.0\n while i < 40 { s = s + i % 7 + (i - 20.0) % 3\n i = i + 1 }\n println(s) }\n", true},
    {"compare chain", "{ let i = 0\n let a = 0\n let b = 0\n let c = 0\n while i < 300 { if i < 100 { a = a + 1 } else if i >= 200 { b = b + 1 } else if i == 150 { c = c + 1 }\n i = i + 1 }\n println(a, b, c) }\n", true},
    {"compare floats nan", "{ let n = 0.0\n let i = 0\n n = (1e308 * 10) - (1e308 * 10)\n while i < 5 { println(n < 1, n == n, n != n, n >= 1, 1.5 < 2, 2 <= 2.0)\n i = i + 1 } }\n", true},
    {"not and negate", "{ let i = 0\n let t = 0\n while i < 100 { if !(i % 3 == 0) { t = t - i }\n t = -t\n i = i + 1 }\n println(t, -(2.5), !nil, !0) }\n", true},
    {"negate min", "{ let m = -9223372036854775807 - 1\n let i = 0\n while i < 3 { println(-m)\n i = i + 1 } }\n", true},
    {"division by zero in loop", "{ let i = 5\n while i > -1 { println(10 / i)\n i = i - 1 } }\n", true},
    {"string in loop", "{ let i = 0\n let s = \"a\"\n while i < 5 { s = s + \"b\"\n i = i + 1 }\n println(s) }\n", true},
    {"string arithmetic error", "{ let i = 0\n while i < 5 { println(i + \"x\")\n i = i + 1 } }\n", true},

    /* ---- arrays ---- */
    {"array read write", "{ let a = [1, 2, 3, 4, 5]\n let i = 0\n let s = 0\n while i < 100 { a[i % 5] = a[(i + 1) % 5] + 1\n s = s + a[-1] + a[i % 5]\n i = i + 1 }\n println(a, s) }\n", true},
    {"array index error", "{ let a = [1, 2, 3]\n let i = 0\n while i < 10 { println(a[i])\n i = i + 1 } }\n", true},
    {"array negative index", "{ let a = [1, 2, 3]\n let i = -1\n while i > -6 { println(a[i])\n i = i - 1 } }\n", true},
    {"nested arrays", "{ let g = [[1, 2], [3, 4]]\n let i = 0\n let s = 0\n while i < 100 { g[i % 2][1] = g[(i + 1) % 2][0] + i\n s = s + g[1][1]\n i = i + 1 }\n println(g, s) }\n", true},
    {"array of strings and objects", "{ let a = [\"x\", [1], \"y\"]\n let i = 0\n while i < 60 { let t = a[i % 3]\n a[(i + 1) % 3] = t\n i = i + 1 }\n println(a) }\n", true},
    {"typed array set", "{ let a: int[] = [1, 2, 3]\n let i = 0\n while i < 60 { a[i % 3] = i\n i = i + 1 }\n println(a)\n let s = join([\"s\"], \"\")\n a[0] = s }\n", true},
    {"push pop loop", "{ let a = []\n let i = 0\n while i < 200 { push(a, i * i)\n i = i + 1 }\n let s = 0\n while len(a) > 0 { s = s + pop(a) }\n println(s) }\n", true},
    {"array literal in loop", "{ let i = 0\n let s = 0\n while i < 100 { let a = [i, i + 1, i + 2]\n s = s + a[2]\n i = i + 1 }\n println(s) }\n", true},
    {"slice and concat", "{ let a = [1, 2, 3, 4]\n let i = 0\n while i < 60 { let b = a[1:3] + a\n i = i + len(b) }\n println(i) }\n", true},
    {"string index", "{ let s = \"hello\"\n let i = 0\n while i < 5 { println(s[i])\n i = i + 1 } }\n", true},

    /* ---- functions and calls ---- */
    {"fib", "fn fib(n) { if n < 2 { return n }\n return fib(n - 1) + fib(n - 2) }\nprintln(fib(22))\n", true},
    {"recursion depth", "fn down(n) { if n == 0 { return 0 }\n return 1 + down(n - 1) }\nprintln(down(1500))\n", true},
    {"stack overflow", "fn down(n) { return 1 + down(n + 1) }\nprintln(down(0))\n", true},
    {"error in callee", "fn f(n) { return 10 / n }\nlet i = 3\nwhile i > -1 { println(f(i))\n i = i - 1 }\n", true},
    {"error deep in recursion", "fn f(n) { if n == 0 { return 1 / n }\n return f(n - 1) }\nprintln(f(30))\n", true},
    {"mutual recursion", "fn ev(n) { if n == 0 { return true }\n return od(n - 1) }\nfn od(n) { if n == 0 { return false }\n return ev(n - 1) }\nprintln(ev(200), od(77))\n", true},
    {"defaults and named", "fn f(a, b = 2, c = 3) { return a * 100 + b * 10 + c }\n{ let i = 0\n let s = 0\n while i < 100 { s = s + f(i) + f(i, 5) + f(i, c = 1) + f(c = 4, a = i)\n i = i + 1 }\n println(s) }\n", true},
    {"rest and spread", "fn sum(*xs) { let t = 0\n let i = 0\n while i < len(xs) { t = t + xs[i]\n i = i + 1 }\n return t }\n{ let a = [1, 2, 3]\n let i = 0\n let s = 0\n while i < 60 { s = s + sum() + sum(i, 2) + sum(*a, i)\n i = i + 1 }\n println(s) }\n", true},
    {"typed parameters", "fn add(a: int, b: int = 1) -> int { return a + b }\n{ let i = 0\n let s = 0\n while i < 100 { s = add(s, 2) + add(i) - add(i)\n i = i + 1 }\n println(s)\n let f = 1.5 + len(\"\")\n println(add(f)) }\n", true},
    {"wrong return type", "fn f(x) -> int { let t = [1, \"s\"]\n if x > 3 { return t[1] }\n return x }\n{ let i = 0\n while i < 6 { println(f(i))\n i = i + 1 } }\n", true},
    {"arity error", "fn f(a, b) { return a }\n{ let i = 0\n while i < 5 { println(f(i, i))\n i = i + 1 }\n let g = f\n println(g(1)) }\n", true},
    {"not callable", "{ let i = 0\n let x = 3\n while i < 5 { println(i)\n i = i + 1 }\n x(1) }\n", true},
    {"native errors", "{ let i = 3\n while i > -3 { println(len(\"abc\"), int(\"7\") + i)\n println(int(i))\n let bad = [i]\n if i < 0 { println(int(bad)) }\n i = i - 1 } }\n", true},
    {"function returns array", "fn make(n) { let a = []\n let i = 0\n while i < n { push(a, i)\n i = i + 1 }\n return a }\n{ let i = 0\n let t = 0\n while i < 80 { t = t + len(make(i % 5))\n i = i + 1 }\n println(t) }\n", true},
    {"locals dropped on return", "fn f(n) { let a = [n]\n let b = [a, a]\n let c = \"s\" + \"t\"\n return b }\n{ let i = 0\n while i < 100 { f(i)\n i = i + 1 }\n println(f(1)) }\n", true},
    {"many locals", "fn f(n) { let a = n\n let b = a + 1\n let c = b + 1\n let d = c + 1\n let e = d + 1\n let g = e + 1\n let h = g + 1\n let k = h + 1\n let l = k + 1\n let m = l + 1\n return a + b + c + d + e + g + h + k + l + m }\n{ let i = 0\n let s = 0\n while i < 100 { s = s + f(i)\n i = i + 1 }\n println(s) }\n", true},

    /* ---- closures ---- */
    {"counter closure", "fn make() { let c = 0\n fn tick() { c = c + 1\n return c }\n return tick }\n{ let t = make()\n let u = make()\n let i = 0\n while i < 100 { t()\n i = i + 1 }\n println(t(), u()) }\n", true},
    {"closure over loop", "{ let fs = []\n let i = 0\n while i < 5 { let k = i * 10\n push(fs, fn() { return k + 1 })\n i = i + 1 }\n let j = 0\n while j < 5 { println(fs[j]())\n j = j + 1 } }\n", true},
    {"higher order", "fn twice(f, x) { return f(f(x)) }\n{ let i = 0\n let s = 0\n while i < 80 { s = s + twice(fn(n) { return n * 3 }, i)\n i = i + 1 }\n println(s) }\n", true},
    {"captured parameter returned", "fn adder(a) { return fn(b) { return a + b } }\n{ let i = 0\n let s = 0\n while i < 80 { s = s + adder(i)(1)\n i = i + 1 }\n println(s) }\n", true},

    /* ---- globals ---- */
    {"global function calls", "let calls = 0\nfn bump() { calls = calls + 1 }\nlet i = 0\nwhile i < 300 { bump()\n i = i + 1 }\nprintln(calls)\n", true},
    {"global defined in loop", "let i = 0\nwhile i < 100 { let n = i * 2\n i = i + 1 }\nprintln(i)\n", true},
    {"global shadows builtin", "let i = 0\nwhile i < 60 { i = i + 1 }\nfn len(x) { return 42 }\nprintln(len([1]), i)\n", true},
    {"global typed", "let x: int = 0\nlet i = 0\nwhile i < 70 { x = x + 1\n i = i + 1 }\nprintln(x)\nlet s = join([\"s\"], \"\")\nx = s\n", true},

    /* ---- other control flow ---- */
    {"nested loops", "{ let i = 0\n let s = 0\n while i < 30 { let j = 0\n while j < 30 { if (i + j) % 3 == 0 { s = s + i * j }\n j = j + 1 }\n i = i + 1 }\n println(s) }\n", true},
    {"early return in loop", "fn find(n) { let i = 0\n while i < 1000 { if i * i > n { return i }\n i = i + 1 }\n return -1 }\nprintln(find(10), find(500), find(999999999))\n", true},
    {"and or", "{ let i = 0\n let c = 0\n while i < 100 { if i % 2 == 0 and i % 3 == 0 or i == 7 { c = c + 1 }\n i = i + 1 }\n println(c) }\n", true},
    {"truthiness", "{ let vals = [nil, false, true, 0, \"\", [], 0.0]\n let i = 0\n while i < 7 { let v = vals[i]\n if v { println(i, \"t\") } else { println(i, \"f\") }\n i = i + 1 } }\n", true},
};

static const Case heap_programs[] = {
    {"push until heap limit", "{ let a = []\n let i = 0\n while i < 100000 { push(a, i)\n i = i + 1 }\n println(len(a)) }\n", true},
    {"strings until heap limit", "{ let s = \"x\"\n let i = 0\n while i < 40 { s = s + s\n i = i + 1 }\n println(len(s)) }\n", true},
    {"arrays in function until heap limit", "fn mk(n) { return [n, [n], \"s\" + \"t\"] }\n{ let keep = []\n let i = 0\n while i < 100000 { push(keep, mk(i))\n i = i + 1 }\n println(len(keep)) }\n", true},
};

static void check_mode(LzJitMode mode, const char *label) {
    lz_engine_set_jit(mode);
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        char name[160];
        snprintf(name, sizeof name, "%s: %s", label, programs[i].name);
        LZ_CHECK(compare_source(name, programs[i].text, 0, 0, true));
    }
    for (size_t i = 0; i < sizeof heap_programs / sizeof heap_programs[0]; i++) {
        char name[160];
        snprintf(name, sizeof name, "%s: %s", label, heap_programs[i].name);
        for (size_t limit = 4096; limit <= (size_t)1 << 22; limit *= 4) {
            LZ_CHECK(compare_source(name, heap_programs[i].text, 0, limit, true));
        }
    }
}

/* The point of the JIT: these really did run as machine code. */
static void check_native_used(void) {
    if (!lz_engine_jit_available()) {
        return;
    }
    lz_engine_set_jit(LZ_JIT_EAGER);
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (!programs[i].native) {
            continue;
        }
        Outcome ge;
        run_source(LZ_ENGINE_GE, programs[i].text, 0, 0, &ge);
        if (ge.compile_status == LZ_OK && (ge.stats.jit_programs == 0 || ge.stats.jit_enters == 0)) {
            fprintf(stderr, "NOT NATIVE: %s (programs %zu, enters %llu)\n", programs[i].name,
                    ge.stats.jit_programs, (unsigned long long)ge.stats.jit_enters);
            lz_test_failures++;
        }
        outcome_free(&ge);
    }
    /* With the JIT off nothing is compiled; with a fuel limit nothing is either. */
    lz_engine_set_jit(LZ_JIT_OFF);
    Outcome off;
    run_source(LZ_ENGINE_GE, programs[0].text, 0, 0, &off);
    LZ_CHECK(off.stats.jit_programs == 0 && off.stats.jit_enters == 0);
    outcome_free(&off);
    lz_engine_set_jit(LZ_JIT_EAGER);
    Outcome fueled;
    run_source(LZ_ENGINE_GE, programs[0].text, 1000000, 0, &fueled);
    LZ_CHECK(fueled.stats.jit_enters == 0);
    outcome_free(&fueled);
}

/* A hot loop is compiled by the default policy, even though it started in the interpreter. */
static void check_hot_policy(void) {
    if (!lz_engine_jit_available()) {
        return;
    }
    lz_engine_set_jit(LZ_JIT_ON);
    Outcome hot;
    run_source(LZ_ENGINE_GE, "let i = 0\nwhile i < 100000 { i = i + 1 }\nprintln(i)\n", 0, 0, &hot);
    LZ_CHECK(hot.stats.jit_programs >= 1);
    LZ_CHECK(hot.stats.jit_enters >= 1);
    outcome_free(&hot);
    Outcome cold;
    run_source(LZ_ENGINE_GE, "let i = 0\nwhile i < 5 { i = i + 1 }\nprintln(i)\n", 0, 0, &cold);
    LZ_CHECK(cold.stats.jit_programs == 0);
    outcome_free(&cold);
}

/* Fuel sweeps: the JIT never runs with a limit, so the interpreter's exact accounting stays. */
static void check_fuel(void) {
    lz_engine_set_jit(LZ_JIT_EAGER);
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i += 3) {
        for (uint64_t fuel = 1; fuel <= 400; fuel += 13) {
            LZ_CHECK(compare_source(programs[i].name, programs[i].text, fuel, 0, true));
        }
    }
}

int main(void) {
    check_mode(LZ_JIT_EAGER, "eager");
    check_mode(LZ_JIT_ON, "on");
    check_native_used();
    check_hot_policy();
    check_fuel();
    return LZ_TEST_RESULT();
}
