#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_compare.h"
#include "lizard/engine.h"
#include "test_util.h"

/*
 * Differential tests: every program runs on the reference interpreter and on
 * the Gecko Engine and must behave identically down to the error offset, the
 * fuel left, the instruction count and the heap statistics.
 */

typedef struct Case {
    const char *name;
    const char *text;
} Case;

/* Each program is valid, but may fail at run time: errors have to match too. */
static const Case programs[] = {
    /* ---- arithmetic ---- */
    {"literals",
     "println(1 + 2 * 3, 7 // 2, 7 % 3, -7 // 2, -7 % 3, 7 % -3, 7 / 2, 2 * 3.5)\n"},
    {"globals int",
     "let a = 10\nlet b = 3\nprintln(a + b, a - b, a * b, a / b, a // b, a % b)\n"},
    {"locals int",
     "{\n let a = 10\n let b = 3\n println(a + b, a - b, a * b, a / b, a // b, a % b)\n}\n"},
    {"negative operands",
     "{\n let a = -7\n let b = 2\n println(a // b, a % b, a // -b, a % -b, 7 // -2, 7 % -2, -7 // -2)\n}\n"},
    {"float arithmetic",
     "{\n let x = 5.5\n let y = 2\n println(x + y, x - y, x * y, x / y, x // y, x % y, y % x)\n}\n"},
    {"float modulo",
     "{\n let x = -7.0\n println(x % 3, x // 3, 7 % 2.0, 7.5 % 2, 6.0 % 4, 0.0 % 5, -0.0 % 5, 5.0 % -3)\n"
     " let big = 9007199254740993.0\n println(big % 7, 1e300 % 7, 5 % 1e300, 4503599627370496.0 % 3)\n}\n"},
    {"int overflow add", "{\n let a = 9223372036854775807\n println(a + 1)\n}\n"},
    {"int overflow add stored",
     "{\n let a = 9223372036854775807\n a = a + 1\n println(a)\n}\n"},
    {"int overflow multiply",
     "{\n let a = 9223372036854775807\n let b = 2\n println(a * b)\n}\n"},
    {"int overflow subtract",
     "{\n let a = -9223372036854775807\n println(a - 2)\n}\n"},
    {"int overflow negate",
     "{\n let m = -9223372036854775807 - 1\n println(-m)\n}\n"},
    {"int min arithmetic",
     "{\n let m = -9223372036854775807 - 1\n println(m // 2, m % 7, m / 2, m + 1, m * 1, m // -1)\n}\n"},
    {"division by zero", "{\n let a = 5\n println(a / 0)\n}\n"},
    {"floor division by zero", "{\n let a = 5\n println(a // 0)\n}\n"},
    {"modulo by zero", "{\n let a = 5\n println(a % 0)\n}\n"},
    {"float modulo by zero", "{\n let a = 5.0\n println(a % 0.0)\n}\n"},
    {"global division by zero", "let a = 1\nlet z = 0\nprintln(a / z)\n"},

    /* ---- comparison ---- */
    {"mixed int float order",
     "{\n let big = 9007199254740993\n let f = 9007199254740992.0\n"
     " println(big > f, big == f, big < f, big >= f, big <= f, f < big, f == big)\n}\n"},
    {"nan and infinity",
     "let inf = 1e308 * 10\nlet nan = inf - inf\n"
     "println(inf, nan, nan == nan, nan != nan, nan < 1, nan > 1, nan <= nan, nan >= 1)\n"
     "println(inf > 1e308, -inf < 0, inf == inf, nan == 1, 1 == nan, 1 < nan, 5 < inf)\n"},
    {"equality kinds",
     "let s = \"abc\"\nprintln(s == nil, nil == nil, nil == false, true == true, 1 == true, 1 == 1.0, 0 == -0.0)\n"
     "println(s == \"abc\", s != \"abd\", [1] == [1], [1] == [2], [] == [], nil != nil)\n"},
    {"order on strings is an error", "{\n let s = \"x\"\n println(s < 1)\n}\n"},
    {"order on nil is an error", "{\n let n = nil\n println(n >= 2)\n}\n"},

    /* ---- strings ---- */
    {"string concat",
     "let s = \"a\"\nlet t = \"b\"\nprintln(s + t, s == t, s == \"a\", s != t, s + \"c\" + \"d\")\n"},
    {"string loop",
     "{\n let s = \"x\"\n let i = 0\n while i < 5 {\n  s = s + \"y\"\n  i = i + 1\n }\n println(s)\n}\n"},
    {"string plus number", "{\n let s = \"x\"\n println(s + 1)\n}\n"},
    {"number plus string", "{\n let s = \"x\"\n println(1 + s)\n}\n"},
    {"string times number", "{\n let s = \"x\"\n println(s * 2)\n}\n"},

    /* ---- control flow ---- */
    {"while with locals",
     "{\n let i = 0\n let s = 0\n while i < 100 {\n  if i % 3 == 0 {\n   s = s + i\n  } else if i % 5 == 0 {\n   s = s - 1\n  }\n  i = i + 1\n }\n println(s)\n}\n"},
    {"while with globals",
     "let i = 0\nlet s = 0\nwhile i < 100 {\n  if i % 3 == 0 {\n   s = s + i\n  } else if i % 5 == 0 {\n   s = s - 1\n  }\n  i = i + 1\n}\nprintln(s)\n"},
    {"collatz small, globals",
     "let i = 1\nlet total = 0\nwhile i <= 300 {\n  let n = i\n  while n > 1 {\n    if n % 2 == 0 {\n      n = n / 2\n    } else {\n      n = n * 3 + 1\n    }\n    total = total + 1\n  }\n  i = i + 1\n}\nprintln(total)\n"},
    {"collatz small, locals and //",
     "{\n let i = 1\n let total = 0\n while i <= 300 {\n  let n = i\n  while n != 1 {\n   if n % 2 == 0 {\n    n = n // 2\n   } else {\n    n = n * 3 + 1\n   }\n   total = total + 1\n  }\n  i = i + 1\n }\n println(total)\n}\n"},
    {"fizzbuzz",
     "let i = 1\nwhile i <= 30 {\n  if i % 15 == 0 { println(\"FizzBuzz\") } else if i % 3 == 0 { println(\"Fizz\") } else if i % 5 == 0 { println(\"Buzz\") } else { println(i) }\n  i = i + 1\n}\n"},
    {"nested loops",
     "{\n let i = 0\n let s = 0\n while i < 12 {\n  let j = 0\n  while j < 12 {\n   s = s + i * j - j % 4\n   j = j + 1\n  }\n  i = i + 1\n }\n println(s)\n}\n"},
    {"float loop",
     "{\n let f = 0.5\n let n = 0\n while f < 1000 {\n  f = f * 1.5\n  n = n + 1\n }\n println(f, n)\n}\n"},
    {"short circuit values",
     "{\n let a = 1\n let b = 2\n println((a and b) + 1, (nil or b) * 10, a > 0 and b > 1, a > 5 or b > 1, a > 5 and b > 1, !a, !nil)\n}\n"},
    {"short circuit conditions",
     "{\n let a = 3\n let c = nil\n if a > 1 and a < 10 { println(\"in\") }\n if c or a > 2 { println(\"or\") }\n println(a and 0, 0 or a, c and a, c or c)\n}\n"},
    {"jump into expression",
     "{\n let a = 1\n let b = 2\n let c = 3\n println((a and b and c) + 1, (a and nil or c) * 2, (c or a) - (b and 1))\n}\n"},
    {"not and truthiness",
     "{\n let z = 0\n let e = \"\"\n let n = nil\n let f = false\n println(!z, !e, !n, !f, !!z, !(1 < 2), !(1 > 2))\n}\n"},

    /* ---- arrays ---- */
    {"array basics",
     "let xs = [1, 2, 3]\nprintln(xs[0], xs[-1], xs[1:3], xs + [4], len(xs))\nxs[0] = 10\nprintln(xs)\n"},
    {"array sum loop, locals",
     "{\n let xs = [1, 2, 3, 4, 5]\n let i = 0\n let s = 0\n while i < len(xs) {\n  s = s + xs[i]\n  i = i + 1\n }\n println(s)\n}\n"},
    {"array sum loop, globals",
     "let xs = range(0, 50)\nlet i = 0\nlet s = 0\nwhile i < len(xs) {\n  s = s + xs[i] * 2\n  xs[i] = s % 7\n  i = i + 1\n}\nprintln(xs, s)\n"},
    {"array push loop",
     "let xs = []\nlet i = 0\nwhile i < 40 {\n  push(xs, i * i)\n  i = i + 1\n}\nprintln(sum(xs), len(xs), xs[39], xs[-1])\n"},
    {"index out of range", "let xs = [1, 2, 3]\nprintln(xs[3])\n"},
    {"negative index out of range", "let xs = [1, 2, 3]\nprintln(xs[-4])\n"},
    {"index must be int", "let xs = [1, 2, 3]\nlet k: any = 1.5\nprintln(xs[k])\n"},
    {"index nil", "let xs = [1, 2, 3]\nlet k: any = nil\nprintln(xs[k])\n"},
    {"cannot index a number", "let n = 5\nprintln(n[0])\n"},
    {"index error inside a loop",
     "{\n let xs = [1, 2, 3]\n let i = 0\n let s = 0\n while i < 6 {\n  s = s + xs[i]\n  i = i + 1\n }\n println(s)\n}\n"},
    {"set index in range and out of range",
     "let xs = [1, 2, 3]\nxs[2] = 9\nxs[-1] = 8\nprintln(xs)\nxs[3] = 1\n"},
    {"typed array rejects wrong type", "let xs: int[] = [1, 2]\nlet v: any = 1.5\nxs[0] = v\n"},
    {"typed array accepts", "let xs: int[] = [1, 2]\nxs[1] = 7\nprintln(xs)\n"},
    {"float array int store",
     "let xs: float[] = [1.5, 2.5]\nlet v: any = 1\nxs[0] = v\nprintln(xs)\n"},
    {"array in itself", "let xs = [1, 2]\nxs[0] = xs\n"},
    {"nested arrays",
     "let xs = [[1, 2], [3, 4]]\nxs[0][1] = 9\nxs[1] = [7]\nprintln(xs, xs[0][1], xs[1][0])\n"},
    {"typed nested arrays",
     "let g: int[][] = [[1, 2], [3, 4]]\ng[1][0] = 30\nprintln(g)\nlet v: any = 2.5\ng[0][1] = v\n"},
    {"string items",
     "let xs = [\"a\", \"b\"]\nxs[0] = \"c\"\nxs[1] = 5\nprintln(xs, xs[0] + xs[0])\n"},
    {"aliasing",
     "let xs = [1, 2, 3]\nlet ys = xs\nys[0] = 99\nprintln(xs, xs == ys, xs == [99, 2, 3], copy(xs) == xs)\n"},
    {"temporary array", "println([1, 2, 3][1], [[1, 2], [3, 4]][1][0], [5][-1])\n"},
    {"bubble sort",
     "let a = [5, 2, 9, 1, 7, 3, 8, 6, 4, 0]\nlet n = len(a)\nlet i = 0\nwhile i < n {\n  let j = 0\n  while j < n - 1 - i {\n    if a[j] > a[j + 1] {\n      let t = a[j]\n      a[j] = a[j + 1]\n      a[j + 1] = t\n    }\n    j = j + 1\n  }\n  i = i + 1\n}\nprintln(a)\n"},
    {"sieve",
     "let n = 200\nlet flags = []\nlet i = 0\nwhile i <= n {\n  push(flags, true)\n  i = i + 1\n}\ni = 2\nwhile i * i <= n {\n  if flags[i] {\n    let j = i * i\n    while j <= n {\n      flags[j] = false\n      j = j + i\n    }\n  }\n  i = i + 1\n}\nlet count = 0\ni = 2\nwhile i <= n {\n  if flags[i] { count = count + 1 }\n  i = i + 1\n}\nprintln(count)\n"},
    {"matrix",
     "{\n let n = 6\n let m = []\n let i = 0\n while i < n {\n  let row = []\n  let j = 0\n  while j < n {\n   push(row, i * j)\n   j = j + 1\n  }\n  push(m, row)\n  i = i + 1\n }\n let t = 0\n i = 0\n while i < n {\n  let j = 0\n  while j < n {\n   t = t + m[i][j] * m[j][i]\n   m[i][j] = t % 10\n   j = j + 1\n  }\n  i = i + 1\n }\n println(t, m)\n}\n"},
    {"examples/arrays",
     "let xs = [1, 2, 3]\nlet ys: int[] = [4, 5]\nlet empty: string[] = []\nprintln(xs[0], xs[-1])\nxs[0] = 10\nprintln(xs[1:3], xs + ys)\npush(xs, 7)\nprintln(pop(xs), len(xs))\ninsert(xs, 1, 99)\nprintln(xs, remove(xs, 0))\npush(ys, 6)\npush(empty, \"a\")\nlet squares = []\nlet i = 0\nlet numbers = range(1, 6)\nwhile i < len(numbers) {\n  push(squares, numbers[i] * numbers[i])\n  i = i + 1\n}\nprintln(squares, sum(squares), min(squares), max(squares))\nreverse(squares)\nsort(squares)\nprintln(join(squares, \", \"), contains(squares, 16), index_of(squares, 99))\nlet grid: int[][] = [[1, 2], [3, 4]]\ngrid[1][0] = 30\nprintln(grid, grid[1][-1])\n"},

    /* ---- types and builtins ---- */
    {"typed variables",
     "let n: int = 5\nn = n + 1\nlet m: int? = nil\nm = 3\nm = m + 1\nprintln(n, m)\n"},
    {"type error on assignment", "let n: int = 5\nlet m: any = \"s\"\nn = m\n"},
    {"union types",
     "type Id = int | string\nlet id: Id = 7\nid = \"seven\"\nlet n: int = int(\"12\") + 1\nlet half: float = n / 2\nprintln(id, n, half)\n"},
    {"calling a number", "let x = 1\nx(2)\n"},
    {"wrong arity", "println(len(1, 2))\n"},
    {"undefined at run time", "{\n let a = 1\n println(a)\n}\nprintln(1 + 1)\n"},
    {"print kinds", "println(1, 1.5, true, nil, \"s\", [1, \"a\", [2]], len(\"abc\"))\nprint(1, 2)\nprint(\"\\n\")\n"},
    {"const folded", "const K = 2 * 3 + 1\nlet i = 0\nwhile i < K { i = i + 1 }\nprintln(i, K * K)\n"},
    {"shadowing",
     "let x = 1\n{\n let x = 2\n {\n  let x = x + 1\n  println(x)\n }\n println(x)\n}\nprintln(x)\n"},
    {"assign to global from block",
     "let g = 0\n{\n let i = 0\n while i < 10 {\n  g = g + i\n  i = i + 1\n }\n}\nprintln(g)\n"},
    {"block locals popped",
     "let k = 0\nwhile k < 4 {\n let a = k * 2\n let b = a + 1\n k = k + 1\n println(a, b)\n}\n"},
};

/* Loops whose behaviour is swept over every fuel value, to prove metering stops at the same instruction. */
static const Case sweeps[] = {
    {"sweep locals",
     "{\n let i = 0\n let s = 0\n while i < 12 {\n  s = s + i * 2 % 7\n  i = i + 1\n }\n println(s)\n}\n"},
    {"sweep globals",
     "let i = 0\nlet s = 0\nwhile i < 12 {\n  if i % 3 == 0 { s = s + i } else { s = s - 1 }\n  i = i + 1\n}\nprintln(s)\n"},
    {"sweep floats",
     "{\n let n = 27\n while n != 1 {\n  if n % 2 == 0 { n = n / 2 } else { n = n * 3 + 1 }\n }\n println(n)\n}\n"},
    {"sweep arrays",
     "let xs = range(0, 6)\nlet i = 0\nlet s = 0\nwhile i < len(xs) {\n  s = s + xs[i]\n  xs[i] = s\n  i = i + 1\n}\nprintln(xs, s)\n"},
    {"sweep error",
     "{\n let a = 3\n let i = 0\n while i < 6 {\n  a = a * 100000000000\n  i = i + 1\n }\n}\n"},
    {"sweep calls",
     "fn f(a, b = 2, *r) { return a + b + len(r) }\nlet i = 0\nlet s = 0\nwhile i < 6 {\n  s = s + f(i) + f(i, 1, 2, 3)\n  i = i + 1\n}\nprintln(s)\n"},
    {"sweep recursion",
     "fn fib(n) { if n < 2 { return n }\n return fib(n - 1) + fib(n - 2) }\nprintln(fib(8))\n"},
    {"sweep closure",
     "fn mk() {\n let c = 0\n fn inc() { c = c + 1\n return c }\n return inc\n}\nlet f = mk()\nlet i = 0\nwhile i < 5 { f()\n i = i + 1 }\nprintln(f())\n"},
    {"sweep short circuit",
     "{\n let a = 1\n let i = 0\n while i < 8 {\n  if a < 3 and i % 2 == 0 or i > 6 { a = a + 1 }\n  i = i + 1\n }\n println(a)\n}\n"},
    /* ---- user-defined functions ---- */
    {"fn basic",
     "fn add(a, b) { return a + b }\nprintln(add(1, 2), add(1.5, 2))\n"},
    {"fn no return value",
     "fn hi() { println(\"hi\") }\nhi()\nprintln(hi())\n"},
    {"fn recursion",
     "fn fib(n) { if n < 2 { return n }\n return fib(n - 1) + fib(n - 2) }\nprintln(fib(15))\n"},
    {"fn mutual recursion",
     "fn even(n) { if n == 0 { return true }\n return odd(n - 1) }\nfn odd(n) { if n == 0 { return false }\n return even(n - 1) }\nprintln(even(10), odd(7))\n"},
    {"fn typed",
     "fn f(a: int, b: float = 1.5) -> float { return a + b }\nprintln(f(1), f(2, 3.0))\n"},
    {"fn typed arg error",
     "fn f(a: int) -> int { return a }\nlet x = \"s\"\nprintln(f(x))\n"},
    {"fn typed return error",
     "fn f(a) -> int { return a }\nprintln(f(1))\nprintln(f(\"s\"))\n"},
    {"fn defaults",
     "fn k(a, b = a * 2, c = b + 1) { return [a, b, c] }\nprintln(k(1), k(1, 5), k(1, 5, 9))\n"},
    {"fn named args",
     "fn k(a, b = 2) { return [a, b] }\nprintln(k(b = 9, a = 1), k(a = 4))\n"},
    {"fn rest",
     "fn s(*xs) {\n let t = 0\n let i = 0\n while i < len(xs) { t = t + xs[i]\n i = i + 1 }\n return t\n}\nprintln(s(), s(1), s(1, 2, 3))\n"},
    {"fn spread",
     "fn s(a, *xs) { return [a, xs] }\nlet v = [7, 8]\nprintln(s(1, *v), s(*v))\n"},
    {"fn closure counter",
     "fn mk() {\n let c = 0\n fn inc() { c = c + 1\n return c }\n return inc\n}\nlet f = mk()\nlet g = mk()\nf()\nf()\nprintln(f(), g())\n"},
    {"fn anonymous",
     "let sq = fn(x) { return x * x }\nprintln(sq(7))\n"},
    {"fn higher order",
     "fn apply(f, x) { return f(f(x)) }\nprintln(apply(fn(n) { return n + 3 }, 1))\n"},
    {"fn deep recursion",
     "fn d(n) { if n == 0 { return 0 }\n return d(n - 1) + 1 }\nprintln(d(1500))\n"},
    {"fn stack overflow",
     "fn g(n) { return g(n + 1) }\ng(0)\n"},
    {"fn runtime error inside",
     "fn h(a, b) { return a / b }\nprintln(h(4, 2))\nprintln(h(1, 0))\n"},
    {"fn arity error",
     "fn f(a, b) { return a }\nlet g = f\nprintln(g(1))\n"},
    {"fn locals in frame",
     "fn f(n) {\n let a = n * 2\n let b = a + 1\n {\n  let c = a + b\n  return c\n }\n}\nprintln(f(3), f(4))\n"},
    {"fn loop inside",
     "fn sum(n) {\n let i = 0\n let t = 0\n while i < n { t = t + i\n i = i + 1 }\n return t\n}\nprintln(sum(100), sum(10))\n"},
    {"fn arrays through calls",
     "fn fill(xs, n) {\n let i = 0\n while i < n { push(xs, i * i)\n i = i + 1 }\n return xs\n}\nlet a = []\nfill(a, 6)\nprintln(a, len(fill([], 3)))\n"},
    {"fn global access",
     "let total = 0\nfn bump(n) { total = total + n }\nbump(2)\nbump(5)\nprintln(total)\n"},
};

static const Case heap_programs[] = {
    {"heap arrays",
     "let xs = []\nlet i = 0\nwhile i < 30 {\n  push(xs, [i, i + 1])\n  i = i + 1\n}\nprintln(len(xs))\n"},
    {"heap closures",
     "fn mk(n) {\n let xs = [n]\n fn get() { return xs }\n return get\n}\nlet i = 0\nlet f = mk(0)\nwhile i < 20 {\n  f = mk(i)\n  i = i + 1\n}\nprintln(f())\n"},
    {"heap rest",
     "fn r(*xs) { return len(xs) }\nlet i = 0\nwhile i < 20 {\n  r(i, i, i)\n  i = i + 1\n}\nprintln(r(1, 2))\n"},
    {"heap strings",
     "let s = \"ab\"\nlet i = 0\nwhile i < 6 {\n  s = s + s\n  i = i + 1\n}\nprintln(len(s))\n"},
};

static void check_corpus(void) {
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        LZ_CHECK(compare_source(programs[i].name, programs[i].text, 0, 0, true));
    }
}

static void check_fuel_sweeps(void) {
    for (size_t i = 0; i < sizeof sweeps / sizeof sweeps[0]; i++) {
        for (uint64_t fuel = 1; fuel <= 700; fuel++) {
            LZ_CHECK(compare_source(sweeps[i].name, sweeps[i].text, fuel, 0, true));
        }
        for (uint64_t fuel = 700; fuel <= 4000; fuel += 37) {
            LZ_CHECK(compare_source(sweeps[i].name, sweeps[i].text, fuel, 0, true));
        }
    }
    /* The corpus under a few budgets too: errors and exhaustion in the middle of any program. */
    static const uint64_t budgets[] = {1, 2, 3, 5, 8, 13, 21, 55, 144, 377, 1000, 5000};
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        for (size_t b = 0; b < sizeof budgets / sizeof budgets[0]; b++) {
            LZ_CHECK(compare_source(programs[i].name, programs[i].text, budgets[b], 0, true));
        }
    }
}

static void check_heap_limits(void) {
    for (size_t i = 0; i < sizeof heap_programs / sizeof heap_programs[0]; i++) {
        for (size_t limit = 64; limit <= 6000; limit += 53) {
            LZ_CHECK(compare_source(heap_programs[i].name, heap_programs[i].text, 0, limit, true));
        }
    }
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        for (size_t limit = 100; limit <= 2000; limit += 450) {
            LZ_CHECK(compare_source(programs[i].name, programs[i].text, 0, limit, true));
        }
    }
}

/* Source text built at run time: long straight-line code. */
static void check_generated(void) {
    char *at;
    size_t flat_size = 3000 * 64 + 256;
    char *flat = malloc(flat_size);
    LZ_CHECK(flat != NULL);
    if (flat == NULL) {
        return;
    }
    at = flat;
    at += sprintf(at, "let a = 1\nlet b = 2\n");
    for (int i = 0; i < 3000; i++) {
        at += sprintf(at, "a = a + b * %d %% 7\nb = b + a // 3\n", i % 13 + 1);
    }
    at += sprintf(at, "println(a, b)\n");
    LZ_CHECK(compare_source("long straight line", flat, 0, 0, true));
    LZ_CHECK(compare_source("long straight line, fuel", flat, 9000, 0, true));
    free(flat);
}


/* ---- bytecode written by hand: shapes the compiler never emits ---- */

/* The stack grows by one per round until it overflows, with a fused instruction at the boundary. */
static void build_stack_boundary_bin(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    size_t loop = chunk->count;
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_ADD);
    bc_u16(chunk, LZ_OP_LOOP, (uint16_t)(chunk->count + 3 - loop));
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_stack_boundary_bin1(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    size_t loop = chunk->count;
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_ADD); /* the left operand is already on the stack */
    bc_u16(chunk, LZ_OP_LOOP, (uint16_t)(chunk->count + 3 - loop));
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_stack_boundary_cmpj(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    size_t loop = chunk->count;
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_LESS);
    size_t jump = chunk->count;
    bc_u16(chunk, LZ_OP_JUMP_IF_FALSE, 0); /* patched below to land on the POP at the exit */
    bc_op(chunk, LZ_OP_POP);
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_LOOP, (uint16_t)(chunk->count + 3 - loop));
    size_t exit_at = chunk->count;
    bc_op(chunk, LZ_OP_POP);
    bc_op(chunk, LZ_OP_RETURN);
    chunk->code[jump + 1] = (uint8_t)((exit_at - (jump + 3)) >> 8);
    chunk->code[jump + 2] = (uint8_t)((exit_at - (jump + 3)) & 0xFFu);
}

/* SET_LOCAL to slots that do not exist: fused and plain must both reach the same error. */
static void build_bad_local_store(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    uint16_t slot = (uint16_t)(uintptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_ADD);
    bc_u16(chunk, LZ_OP_SET_LOCAL, slot);
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_bad_local_store_left(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    uint16_t slot = (uint16_t)(uintptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_CONSTANT, two); /* left operand on the stack, then [load] ADD [store] */
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_ADD);
    bc_u16(chunk, LZ_OP_SET_LOCAL, slot);
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_bad_local_load(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    uint16_t slot = (uint16_t)(uintptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    bc_u16(chunk, LZ_OP_GET_LOCAL, slot);
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_op(chunk, LZ_OP_ADD);
    bc_op(chunk, LZ_OP_RETURN);
}

/* A jump that lands in the middle of a run GE fuses: it must run the rest of the run plain. */
static void build_jump_into_run(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    int variant = (int)(intptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t two = bc_constant(chunk, lz_value_int(2));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    if (variant == 1) {
        bc_op(chunk, LZ_OP_NIL); /* a falsy condition for the jump below */
        size_t jump = chunk->count;
        bc_u16(chunk, LZ_OP_JUMP_IF_FALSE, 4); /* skips the POP and the first load, lands on the second */
        (void)jump;
        bc_op(chunk, LZ_OP_POP);
    } else {
        bc_u16(chunk, LZ_OP_JUMP, 3);
    }
    bc_u16(chunk, LZ_OP_CONSTANT, one); /* [load] [load] ADD: skipped by the jump */
    bc_u16(chunk, LZ_OP_CONSTANT, two);
    bc_op(chunk, LZ_OP_ADD);
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_empty(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)chunk;
    (void)context;
}

static void build_no_return(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_op(chunk, LZ_OP_POP);
}

static void build_truncated(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    bc_byte(chunk, (uint8_t)LZ_OP_NIL);
    bc_byte(chunk, (uint8_t)LZ_OP_CONSTANT);
    bc_byte(chunk, 0); /* the second operand byte is missing */
}

static void build_unknown_opcode(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    (void)context;
    bc_op(chunk, LZ_OP_NIL);
    bc_byte(chunk, 0xEE);
    bc_op(chunk, LZ_OP_RETURN);
}

/* Jumps that GE cannot place on an instruction boundary: the reference decides what happens. */
static void build_jump(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    uint16_t distance = (uint16_t)(uintptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    bc_u16(chunk, LZ_OP_JUMP, distance);
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_loop(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    uint16_t distance = (uint16_t)(uintptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u16(chunk, LZ_OP_LOOP, distance);
    bc_op(chunk, LZ_OP_RETURN);
}

static void build_bad_operands(LzVM *vm, LzChunk *chunk, void *context) {
    int variant = (int)(intptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    if (variant == 0) {
        bc_u16(chunk, LZ_OP_CONSTANT, 40); /* no such constant */
    } else if (variant == 1) {
        bc_u16(chunk, LZ_OP_GET_GLOBAL, one); /* the constant is not a name */
    } else if (variant == 2) {
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "nope")); /* undefined */
    } else if (variant == 3) {
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u16(chunk, LZ_OP_SET_GLOBAL, bc_name(vm, chunk, "nope")); /* assigning to nothing */
    } else if (variant == 4) {
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "println")); /* a builtin */
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u8(chunk, LZ_OP_CALL, 1);
        bc_op(chunk, LZ_OP_POP);
    } else {
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "x"));
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "y"));
        bc_op(chunk, LZ_OP_ADD);
        bc_u16(chunk, LZ_OP_SET_GLOBAL, bc_name(vm, chunk, "z"));
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "z"));
        bc_u16(chunk, LZ_OP_GET_GLOBAL, bc_name(vm, chunk, "x"));
        bc_op(chunk, LZ_OP_GREATER);
        bc_op(chunk, LZ_OP_POP);
    }
    bc_op(chunk, LZ_OP_RETURN);
}


/* println is called through the builtin cache, then a global of the same name is defined: it must win. */
static void build_shadow_builtin(LzVM *vm, LzChunk *chunk, void *context) {
    (void)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t five = bc_constant(chunk, lz_value_int(5));
    uint16_t name = bc_name(vm, chunk, "println");
    size_t loop = chunk->count;
    bc_u16(chunk, LZ_OP_GET_GLOBAL, name);
    bc_u16(chunk, LZ_OP_CONSTANT, one);
    bc_u8(chunk, LZ_OP_CALL, 1);
    bc_op(chunk, LZ_OP_POP);
    bc_u16(chunk, LZ_OP_CONSTANT, five);
    bc_u16(chunk, LZ_OP_DEFINE_GLOBAL, name);
    bc_u16(chunk, LZ_OP_LOOP, (uint16_t)(chunk->count + 3 - loop));
    bc_op(chunk, LZ_OP_RETURN);
}

/* Native calls in every shape: result discarded, nested, wrong arity, non-function callee. */
static void build_calls(LzVM *vm, LzChunk *chunk, void *context) {
    int variant = (int)(intptr_t)context;
    uint16_t one = bc_constant(chunk, lz_value_int(1));
    uint16_t len_name = bc_name(vm, chunk, "len");
    uint16_t println_name = bc_name(vm, chunk, "println");
    uint16_t text = 0;
    LzString *string = lz_string_new(&vm->heap, "hello", 5);
    if (string != NULL) {
        text = bc_constant(chunk, lz_value_object(&string->base));
        lz_object_release(&vm->heap, &string->base);
    }
    switch (variant) {
    case 0: /* println(len("hello")) */
        bc_u16(chunk, LZ_OP_GET_GLOBAL, println_name);
        bc_u16(chunk, LZ_OP_GET_GLOBAL, len_name);
        bc_u16(chunk, LZ_OP_CONSTANT, text);
        bc_u8(chunk, LZ_OP_CALL, 1);
        bc_u8(chunk, LZ_OP_CALL, 1);
        break;
    case 1: /* len(1, 1): wrong arity */
        bc_u16(chunk, LZ_OP_GET_GLOBAL, len_name);
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u8(chunk, LZ_OP_CALL, 2);
        break;
    case 2: /* 1(1): not a function */
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u16(chunk, LZ_OP_CONSTANT, one);
        bc_u8(chunk, LZ_OP_CALL, 1);
        break;
    case 3: /* call with more arguments than the stack holds */
        bc_u16(chunk, LZ_OP_GET_GLOBAL, println_name);
        bc_u8(chunk, LZ_OP_CALL, 3);
        break;
    default: /* println() with no arguments, then twice with the callee's own result */
        bc_u16(chunk, LZ_OP_GET_GLOBAL, println_name);
        bc_u8(chunk, LZ_OP_CALL, 0);
        bc_u16(chunk, LZ_OP_GET_GLOBAL, println_name);
        bc_u8(chunk, LZ_OP_CALL, 0);
        bc_op(chunk, LZ_OP_NEGATE); /* negating the nil it returned: an error */
        break;
    }
    bc_op(chunk, LZ_OP_RETURN);
}

/* Negation of every kind of value, including the one that overflows. */
static void build_negate(LzVM *vm, LzChunk *chunk, void *context) {
    (void)vm;
    int variant = (int)(intptr_t)context;
    LzValue values[] = {lz_value_int(5), lz_value_int(INT64_MIN), lz_value_float(2.5), lz_value_float(0.0),
                        lz_value_nil(), lz_value_bool(true)};
    uint16_t index = bc_constant(chunk, values[variant]);
    bc_u16(chunk, LZ_OP_CONSTANT, index);
    bc_op(chunk, LZ_OP_NEGATE);
    bc_op(chunk, LZ_OP_NEGATE);
    bc_op(chunk, LZ_OP_RETURN);
}

static void check_bytecode(void) {
    LZ_CHECK(compare_chunk("stack boundary: fused arithmetic", build_stack_boundary_bin, NULL, 0, 1));
    LZ_CHECK(compare_chunk("stack boundary: left operand on the stack", build_stack_boundary_bin1, NULL, 0, 1));
    LZ_CHECK(compare_chunk("stack boundary: fused compare and jump", build_stack_boundary_cmpj, NULL, 0, 1));
    for (uintptr_t slot = 0; slot < 5; slot++) {
        LZ_CHECK(compare_chunk("store to a missing local", build_bad_local_store, (void *)slot, 0, 1));
        LZ_CHECK(compare_chunk("store to a missing local, left operand", build_bad_local_store_left, (void *)slot, 0, 1));
        LZ_CHECK(compare_chunk("load of a missing local", build_bad_local_load, (void *)slot, 0, 1));
    }
    LZ_CHECK(compare_chunk("jump into a fused run", build_jump_into_run, (void *)(intptr_t)0, 0, 1));
    LZ_CHECK(compare_chunk("conditional jump into a fused run", build_jump_into_run, (void *)(intptr_t)1, 0, 1));
    LZ_CHECK(compare_chunk("empty chunk", build_empty, NULL, 0, 1));
    LZ_CHECK(compare_chunk("no OP_RETURN", build_no_return, NULL, 0, 1));
    LZ_CHECK(compare_chunk("truncated instruction", build_truncated, NULL, 0, 0));
    LZ_CHECK(compare_chunk("unknown opcode", build_unknown_opcode, NULL, 0, 0));
    LZ_CHECK(compare_chunk("jump to the end", build_jump, (void *)(uintptr_t)4, 0, 1));
    for (uintptr_t distance = 0; distance < 8; distance++) {
        LZ_CHECK(compare_chunk("jump", build_jump, (void *)distance, 0, -1));
        LZ_CHECK(compare_chunk("loop", build_loop, (void *)distance, 3000, -1)); /* may spin: fuel ends it */
    }
    LZ_CHECK(compare_chunk("jump past the end", build_jump, (void *)(uintptr_t)1000, 0, 0));
    LZ_CHECK(compare_chunk("loop before the start", build_loop, (void *)(uintptr_t)1000, 0, 0));
    for (intptr_t variant = 0; variant < 6; variant++) {
        LZ_CHECK(compare_chunk("odd operands", build_bad_operands, (void *)variant, 0, 1));
    }
    LZ_CHECK(compare_chunk("global defined after the builtin was cached", build_shadow_builtin, NULL, 5000, 1));
    for (intptr_t variant = 0; variant < 5; variant++) {
        LZ_CHECK(compare_chunk("native calls", build_calls, (void *)variant, 0, 1));
    }
    for (intptr_t variant = 0; variant < 6; variant++) {
        LZ_CHECK(compare_chunk("negate", build_negate, (void *)variant, 0, 1));
    }

    /* Fuel at every value, so metering must stop at the same instruction inside any of the above. */
    static const ChunkBuilder builders[] = {build_stack_boundary_bin, build_jump_into_run, build_no_return,
                                            build_empty, build_bad_operands};
    for (size_t b = 0; b < sizeof builders / sizeof builders[0]; b++) {
        for (uint64_t fuel = 1; fuel <= 60; fuel++) {
            LZ_CHECK(compare_chunk("bytecode under fuel", builders[b], (void *)(intptr_t)5, fuel, -1));
        }
    }
    for (uint64_t fuel = 1; fuel <= 5000; fuel += 7) {
        LZ_CHECK(compare_chunk("stack boundary under fuel", build_stack_boundary_bin, NULL, fuel, 1));
        LZ_CHECK(compare_chunk("stack boundary (compare) under fuel", build_stack_boundary_cmpj, NULL, fuel, 1));
    }
}

/* The engine API itself. */
static void check_api(void) {
    LzEngine engine;
    LZ_CHECK(lz_engine_from_name("ge", &engine) && engine == LZ_ENGINE_GE);
    LZ_CHECK(lz_engine_from_name("gecko", &engine) && engine == LZ_ENGINE_GE);
    LZ_CHECK(lz_engine_from_name("ref", &engine) && engine == LZ_ENGINE_REFERENCE);
    LZ_CHECK(lz_engine_from_name("reference", &engine) && engine == LZ_ENGINE_REFERENCE);
    LZ_CHECK(!lz_engine_from_name("jit", &engine));
    LZ_CHECK(strcmp(lz_engine_name(LZ_ENGINE_GE), "ge") == 0);
    LZ_CHECK(strcmp(lz_engine_name(LZ_ENGINE_REFERENCE), "ref") == 0);

    /* Statistics: the same instruction count on both, fewer dispatches on GE. */
    const char *text = "{\n let i = 0\n let s = 0\n while i < 1000 {\n  s = s + i * 2\n  i = i + 1\n }\n println(s)\n}\n";
    Outcome ref;
    Outcome ge;
    run_source(LZ_ENGINE_REFERENCE, text, 0, 0, &ref);
    run_source(LZ_ENGINE_GE, text, 0, 0, &ge);
    LZ_CHECK(ref.stats.instructions == ge.stats.instructions);
    LZ_CHECK(ref.stats.instructions == ref.stats.dispatches);
    LZ_CHECK(!ref.stats.translated);
    LZ_CHECK(ge.stats.translated);
    LZ_CHECK(ge.stats.dispatches * 2 < ge.stats.instructions);
    LZ_CHECK(ge.stats.fused_sites > 0);
    LZ_CHECK(ge.stats.program_size > 0);
    outcome_free(&ref);
    outcome_free(&ge);
}

int main(void) {
    check_api();
    check_corpus();
    check_generated();
    check_bytecode();
    check_fuel_sweeps();
    check_heap_limits();
    return LZ_TEST_RESULT();
}
