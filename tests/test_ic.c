/*
 * The inline caches of the Gecko Engine on the member instructions (GET_FIELD, SET_FIELD,
 * METHOD_LOOKUP): sites whose class changes, fields holding weak links, sites that
 * start to miss after hits, and the claim they rest on, that the members of a class do not change
 * once it has instances. Every program runs on the reference interpreter and on GE, which must
 * agree on everything observable, and the output is checked against what the language says, so a
 * cache that is wrong in both engines or a program that does nothing cannot pass.
 */
#include <stdio.h>
#include <string.h>

#include "engine_compare.h"
#include "lizard/class.h"
#include "lizard/array.h"
#include "lizard/dict.h"
#include "lizard/link.h"
#include "test_util.h"

typedef struct Case {
    const char *name;
    const char *text;
    const char *expected; /* stdout of the run, which for a failing one is what came before the error */
    const char *failure;  /* part of the error message of a run that fails, NULL for one that does not */
} Case;

static const char poly_text[] =
    "class A {\n"
    "  pub v: int = 1\n"
    "  pub fn f(self) => self.v + 1\n"
    "}\n"
    "class B {\n"
    "  pub w: int = 0\n"
    "  pub v: int = 20\n"
    "  pub fn f(self) => self.v * 2\n"
    "}\n"
    "class C {\n"
    "  pub u: int = 0\n"
    "  pub z: int = 0\n"
    "  pub v: int = 300\n"
    "  pub fn f(self) => self.v - 1\n"
    "}\n"
    "fn get(o) => o.v\n"
    "fn call(o) => o.f()\n"
    "fn put(o, n) {\n"
    "  o.v = n\n"
    "  return o.v\n"
    "}\n"
    "let items = [A(), B(), C()]\n"
    "let total = 0\n"
    "let i = 0\n"
    "while i < 60 {\n"
    "  let o = items[i % 3]\n"
    "  total += get(o) + call(o) + put(o, i)\n"
    "  i += 1\n"
    "}\n"
    "println(total)\n";

static const char link_text[] =
    "class Node {\n"
    "  pub name: string\n"
    "  pub link parent: Node = nil\n"
    "  pub depth: int = 0\n"
    "}\n"
    "let root = Node(name: \"r\")\n"
    "let a = Node(name: \"a\", parent: root, depth: 1)\n"
    "let b = Node(name: \"b\", parent: a, depth: 2)\n"
    "fn up(n) => n.parent\n"
    "fn depth(x) => x.depth\n"
    "let steps = 0\n"
    "let n = b\n"
    "while n.parent != nil {\n"
    "  n = n.parent\n"
    "  steps += 1\n"
    "}\n"
    "let k = 0\n"
    "let i = 0\n"
    "while i < 40 {\n"
    "  k += up(b).depth + depth(a)\n"
    "  i += 1\n"
    "}\n"
    "println(steps, k, b.parent.name, n.name)\n"
    "let l = link(root)\n"
    "fn show(v) {\n"
    "  try {\n"
    "    println(depth(v))\n"
    "  } catch Error as e {\n"
    "    println(\"error:\", e.message)\n"
    "  }\n"
    "}\n"
    "for v in [b, a, root, l, nil, 5] {\n"
    "  show(v)\n"
    "}\n"
    "println(depth(b), depth(a))\n";

static const char hitfail_text[] =
    "class A {\n"
    "  pub v: int = 1\n"
    "}\n"
    "class B {\n"
    "  pub v: int = 20\n"
    "}\n"
    "class D {\n"
    "  pub w: int = 3\n"
    "}\n"
    "class P {\n"
    "  secret: int = 5\n"
    "}\n"
    "fn get(o) => o.v\n"
    "let sum = 0\n"
    "let i = 0\n"
    "while i < 30 {\n"
    "  sum += get(A())\n"
    "  i += 1\n"
    "}\n"
    "println(sum)\n"
    "fn show(o) {\n"
    "  try {\n"
    "    println(get(o))\n"
    "  } catch Error as e {\n"
    "    println(\"error:\", e.message)\n"
    "  }\n"
    "}\n"
    "for o in [B(), D(), P(), A(), B(), D(), A()] {\n"
    "  show(o)\n"
    "}\n"
    "let j = 0\n"
    "while j < 30 {\n"
    "  sum += get(A()) + get(B())\n"
    "  j += 1\n"
    "}\n"
    "println(sum)\n";

static const char extend1_text[] =
    "class Shape {\n"
    "  pub w: int = 2\n"
    "  pub h: int = 3\n"
    "  pub fn area(self) => self.w * self.h\n"
    "}\n"
    "let s = Shape()\n"
    "fn area_of(x) => x.area()\n"
    "fn per(x) => x.perimeter()\n"
    "fn width(x) => x.w\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 20 {\n"
    "  t += area_of(s) + width(s)\n"
    "  i += 1\n"
    "}\n"
    "println(t)\n"
    "fn try_per(x) {\n"
    "  try {\n"
    "    println(per(x))\n"
    "  } catch Error as e {\n"
    "    println(\"before:\", e.message)\n"
    "  }\n"
    "}\n"
    "try_per(s)\n"
    "extend Shape {\n"
    "  pub fn perimeter(self) => 2 * (self.w + self.h)\n"
    "}\n"
    "println(per(s), per(Shape()))\n"
    "let j = 0\n"
    "while j < 20 {\n"
    "  t += area_of(s) + per(s) + width(s)\n"
    "  j += 1\n"
    "}\n"
    "println(t)\n";

static const char pubread_text[] =
    "class Car {\n"
    "  pub read speed: int = 0\n"
    "  pub fn set(self, n: int) {\n"
    "    self.speed = n\n"
    "  }\n"
    "}\n"
    "class Bike {\n"
    "  pub speed: int = 0\n"
    "}\n"
    "fn poke(o, n) {\n"
    "  o.speed = n\n"
    "  return o.speed\n"
    "}\n"
    "let c = Car()\n"
    "let b = Bike()\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 20 {\n"
    "  c.set(i)\n"
    "  t += poke(b, i)\n"
    "  i += 1\n"
    "}\n"
    "println(t, c.speed, b.speed)\n"
    "fn try_poke(o) {\n"
    "  try {\n"
    "    println(poke(o, 7))\n"
    "  } catch Error as e {\n"
    "    println(\"error:\", e.message)\n"
    "  }\n"
    "}\n"
    "for o in [b, c, b, c, b] {\n"
    "  try_poke(o)\n"
    "}\n"
    "println(c.speed, b.speed)\n";

static const char fast_text[] =
    "class P {\n"
    "  pub n: int = 4\n"
    "  pub fn to_string(self) => \"P\" + string(self.n)\n"
    "}\n"
    "fn len_of(x) => len(x)\n"
    "fn safe(f, a) {\n"
    "  try {\n"
    "    return f(a)\n"
    "  } catch Error as e {\n"
    "    return \"error: \" + e.message\n"
    "  }\n"
    "}\n"
    "let s = \"héllo wörld\"\n"
    "let arr = [1, 2, 3]\n"
    "let total = 0\n"
    "let i = 0\n"
    "while i < 50 {\n"
    "  total += len(s) + len(arr) + len_of(\"abc\") + len_of(arr)\n"
    "  push(arr, i)\n"
    "  if len(arr) > 6 {\n"
    "    pop(arr)\n"
    "  }\n"
    "  i += 1\n"
    "}\n"
    "println(total, len(arr))\n"
    "println(safe(len_of, 5))\n"
    "println(safe(len_of, {\"a\": 1}))\n"
    "println(safe(len_of, nil))\n"
    "println(safe(len_of, link(arr)))\n"
    "println(P(), string(P()), string(7), string(2.5), string(nil), string(\"x\"))\n"
    "println(typeof(1), typeof(P()), int(\"12\") + int(3.9), float(2), bool(0), identical(arr, arr))\n"
    "println(safe(int, \"zz\"))\n"
    "println(safe(string, link(arr)) != nil)\n"
    "println(safe(pop, []))\n"
    "println(safe(len, 1))\n"
    "fn bare(g) {\n"
    "  try {\n"
    "    return g()\n"
    "  } catch Error as e {\n"
    "    return \"error: \" + e.message\n"
    "  }\n"
    "}\n"
    "println(bare(len), bare(push), bare(println) == nil)\n"
    "let long = join(map(range(200), fn(i) => \"é\"), \"\")\n"
    "println(len(long), len(long + \"abc\"))\n";

static const char priv2_text[] =
    "class A {\n"
    "  pub v: int = 1\n"
    "}\n"
    "class P {\n"
    "  v: int = 5\n"
    "}\n"
    "fn get(o) => o.v\n"
    "let sum = 0\n"
    "let i = 0\n"
    "while i < 30 {\n"
    "  sum += get(A())\n"
    "  i += 1\n"
    "}\n"
    "println(sum)\n"
    "println(get(P()))\n";

static const char pubread2_text[] =
    "class Car {\n"
    "  pub read speed: int = 0\n"
    "}\n"
    "class Bike {\n"
    "  pub speed: int = 0\n"
    "}\n"
    "fn poke(o, n) {\n"
    "  o.speed = n\n"
    "  return o.speed\n"
    "}\n"
    "let b = Bike()\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 30 {\n"
    "  t += poke(b, i)\n"
    "  i += 1\n"
    "}\n"
    "println(t)\n"
    "println(poke(Car(), 7))\n";

static const char link2_text[] =
    "class Node {\n"
    "  pub link parent: Node = nil\n"
    "  pub depth: int = 0\n"
    "}\n"
    "let root = Node()\n"
    "let a = Node(parent: root, depth: 1)\n"
    "fn depth(x) => x.depth\n"
    "fn up(x) => x.parent\n"
    "let l = link(a)\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 30 {\n"
    "  t += depth(a) + depth(l) + depth(up(a)) + depth(root)\n"
    "  i += 1\n"
    "}\n"
    "println(t)\n"
    "println(depth(up(root)))\n";

static const char hitfail2_text[] =
    "class A {\n"
    "  pub v: int = 1\n"
    "}\n"
    "class B {\n"
    "  pub v: int = 20\n"
    "}\n"
    "class D {\n"
    "  pub w: int = 3\n"
    "}\n"
    "fn get(o) => o.v\n"
    "let sum = 0\n"
    "let i = 0\n"
    "while i < 30 {\n"
    "  sum += get(A()) + get(B())\n"
    "  i += 1\n"
    "}\n"
    "println(sum)\n"
    "println(get(D()))\n";

static const char extend2_text[] =
    "class Shape {\n"
    "  pub w: int = 2\n"
    "  pub fn area(self) => self.w * 3\n"
    "}\n"
    "let s = Shape()\n"
    "fn area_of(x) => x.area()\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 10 {\n"
    "  t += area_of(s) + s.w\n"
    "  i += 1\n"
    "}\n"
    "println(t)\n"
    "extend Shape {\n"
    "  pub fn area(self) => 99\n"
    "}\n"
    "println(area_of(s))\n";

static const char extend3_text[] =
    "class Shape {\n"
    "  pub w: int = 2\n"
    "  pub fn area(self) => self.w * 3\n"
    "}\n"
    "let s = Shape()\n"
    "fn area_of(x) => x.area()\n"
    "fn width(x) => x.w\n"
    "let t = 0\n"
    "let i = 0\n"
    "while i < 10 {\n"
    "  t += area_of(s) + width(s)\n"
    "  i += 1\n"
    "}\n"
    "println(t)\n"
    "extend Shape {\n"
    "  pub fn w(self) => 99\n"
    "}\n"
    "println(width(s), area_of(s))\n";

static const char poly_expected[] =
    "6156\n";

static const char link_expected[] =
    "2 80 a r\n"
    "2\n"
    "1\n"
    "0\n"
    "0\n"
    "error: nil has no field 'depth'\n"
    "error: int has no field 'depth'\n"
    "2 1\n";

static const char hitfail_expected[] =
    "30\n"
    "20\n"
    "error: D has no field 'v'\n"
    "error: P has no field 'v'\n"
    "1\n"
    "20\n"
    "error: D has no field 'v'\n"
    "1\n"
    "660\n";

static const char extend1_expected[] =
    "160\n"
    "before: Shape has no method or field 'perimeter'\n"
    "10 10\n"
    "520\n";

static const char pubread_expected[] =
    "190 19 19\n"
    "7\n"
    "error: 'speed' is read-only outside Car\n"
    "7\n"
    "error: 'speed' is read-only outside Car\n"
    "7\n"
    "19 7\n";

static const char fast_expected[] =
    "1288 6\n"
    "error: len(): expected an array or string, got int\n"
    "1\n"
    "error: len(): expected an array or string, got nil\n"
    "error: a link has to be read with .target first, got link\n"
    "P4 P4 7 2.5 nil x\n"
    "int P 15 2.0 false true\n"
    "error: int(): invalid integer \"zz\"\n"
    "true\n"
    "error: pop(): array is empty\n"
    "error: len(): expected an array or string, got int\n"
    "\n"
    "error: len(): wrong number of arguments (0 given) error: push(): wrong number of arguments (0 given) true\n"
    "200 203\n";

static const char priv2_expected[] =
    "30\n";

static const char pubread2_expected[] =
    "435\n";

static const char link2_expected[] =
    "60\n";

static const char hitfail2_expected[] =
    "630\n";

static const char extend2_expected[] =
    "80\n";

static const char extend3_expected[] =
    "80\n";

static const Case cases[] = {
    {"poly", poly_text, poly_expected, NULL},
    {"link", link_text, link_expected, NULL},
    {"hitfail", hitfail_text, hitfail_expected, NULL},
    {"extend1", extend1_text, extend1_expected, NULL},
    {"pubread", pubread_text, pubread_expected, NULL},
    {"fast", fast_text, fast_expected, NULL},
    {"priv2", priv2_text, priv2_expected, "'v' is private to P"},
    {"pubread2", pubread2_text, pubread2_expected, "'speed' is read-only outside Car"},
    {"link2", link2_text, link2_expected, "nil has no field 'depth'"},
    {"hitfail2", hitfail2_text, hitfail2_expected, "D has no field 'v'"},
    {"extend2", extend2_text, extend2_expected, "'area' is already defined for Shape"},
    {"extend3", extend3_text, extend3_expected, "'w' is already defined for Shape"},
};

static void check_case(const Case *c) {
    Outcome ref;
    Outcome ge;
    run_source(LZ_ENGINE_REFERENCE, c->text, 0, 0, &ref);
    run_source(LZ_ENGINE_GE, c->text, 0, 0, &ge);
    LZ_CHECK(ref.compile_status == LZ_OK);
    LZ_CHECK(ge.stats.translated);
    LZ_CHECK(outcomes_match(c->name, c->text, 0, &ref, &ge));
    if (strcmp(text_or_empty(&ref.out), c->expected) != 0) {
        fprintf(stderr, "WRONG OUTPUT %s\n  expected: [%s]\n  got     : [%s]\n", c->name, c->expected,
                text_or_empty(&ref.out));
        LZ_CHECK(false);
    }
    if (c->failure == NULL) {
        LZ_CHECK(ref.run_status == LZ_OK);
    } else {
        LZ_CHECK(ref.run_status != LZ_OK);
        LZ_CHECK(strstr(ref.error, c->failure) != NULL);
    }
    outcome_free(&ref);
    outcome_free(&ge);
}

/* The fast native call must run out of fuel exactly where the reference does, len of a string included. */
static void check_fuel(void) {
    for (uint64_t fuel = 1; fuel < 400; fuel += 3) {
        LZ_CHECK(compare_source("fast, fuel", fast_text, fuel, 0, true));
    }
    for (uint64_t fuel = 400; fuel < 3200; fuel += 97) {
        LZ_CHECK(compare_source("fast, fuel", fast_text, fuel, 0, true));
    }
}

/*
 * len of a string costs one fuel unit per 64 bytes on top of its instruction. Both engines use the
 * same short way, so they can't check each other here: the cost is measured against the bytes.
 */
static void check_len_cost(void) {
    static const char base[] = "let t = join(map(range(192), fn(i) => \"\\u{e9}\"), \"\")\nlet a = 1\n";
    static const char with_len[] = "let t = join(map(range(192), fn(i) => \"\\u{e9}\"), \"\")\nlet a = len(t)\n";
    for (int engine = 0; engine < 2; engine++) {
        LzEngine which = engine == 0 ? LZ_ENGINE_REFERENCE : LZ_ENGINE_GE;
        Outcome plain;
        Outcome counted;
        run_source(which, base, 1000000, 0, &plain);
        run_source(which, with_len, 1000000, 0, &counted);
        LZ_CHECK(plain.run_status == LZ_OK && counted.run_status == LZ_OK);
        uint64_t spent = plain.fuel - counted.fuel;
        uint64_t instructions = counted.instructions - plain.instructions;
        /* 192 characters of two bytes each: 384 bytes, six units. */
        LZ_CHECK(spent == instructions + 6);
        outcome_free(&plain);
        outcome_free(&counted);
    }
}

static void check_ref_cache(void) {
    LzVM vm;
    Outcome result;
    outcome_begin(&vm, &result);
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzSource source;
    lz_source_from_string(&source, "t.lz",
        "class P {pub n:int=4; pub fn f(self)=>self.n}\n"
        "fn get(p)=>p.n\nfn put(p,n){p.n=n}\nfn call(p)=>p.f()\n"
        "let p=P()\nfor i in 0..30 {get(p);put(p,i);call(p)}\n");
    LzDiagnostics diagnostics;
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    size_t hits = 0;
    const char *names[] = {"get", "put", "call"};
    for (size_t i = 0; i < 3; i++) {
        LzString *name = lz_vm_intern(&vm, names[i], strlen(names[i]));
        const LzValue *value = lz_table_get(&vm.globals, name);
        LZ_CHECK(value != NULL && lz_value_is_closure(value));
        if (value == NULL || !lz_value_is_closure(value)) continue;
        LzFunction *function = lz_value_as_closure(value)->function;
        for (size_t at = 0; at < function->chunk.count;) {
            uint8_t op = function->chunk.code[at];
            if (op == LZ_OP_GET_FIELD || op == LZ_OP_SET_FIELD || op == LZ_OP_METHOD_LOOKUP) {
                LzMemberCache *site = lz_chunk_member_site(&function->chunk, at);
                LZ_CHECK(site != NULL && site->ic_class != 0 && site->ic_run >= 30);
                if (site != NULL && site->ic_class != 0) {
                    hits++;
                    size_t saved = vm.ext_count;
                    vm.ext_count++;
                    LZ_CHECK(site->ic_extensions != vm.ext_count);
                    /* Any new extension makes a previous shape miss, even when
                     * the cached member itself belongs to the immutable chain. */
                    LzInstance instance = {0};
                    instance.cls = (LzClass *)lz_class_by_id(&vm, site->ic_class);
                    LZ_CHECK(!lz_member_cache_matches(&vm, site, &instance));
                    vm.ext_count = (size_t)UINT16_MAX + 1;
                    LZ_CHECK(!lz_member_cache_matches(&vm, site, &instance));
                    vm.ext_count = saved;
                    LZ_CHECK(lz_member_cache_matches(&vm, site, &instance));
                    if (hits == 1) {
                        LzMemberCache missing = {0};
                        LzString *absent = lz_vm_intern(&vm, "absent_cache_test", 17);
                        vm.frame_count = 1;
                        for (size_t j = 0; j < LZ_MEMBER_CACHE_BAD_MAX; j++) {
                            LZ_CHECK(!lz_member_cache_miss(&vm, &missing, &instance, absent, 0));
                        }
                        LZ_CHECK(missing.ic_stuck != 0 && missing.ic_bad == LZ_MEMBER_CACHE_BAD_MAX);
                        vm.frame_count = 0;
                    }
                }
            }
            size_t length = lz_opcode_length(op);
            LZ_CHECK(length != 0);
            if (length == 0) break;
            at += length;
        }
    }
    LZ_CHECK(hits == 3);
    LzChunk sites;
    lz_chunk_init(&sites);
    for (size_t i = 0; i < LZ_MEMBER_SITES_MAX + 1; i++) {
        size_t offset = sites.count;
        LZ_CHECK(lz_chunk_write(&sites, LZ_OP_GET_FIELD, 0));
        LZ_CHECK(lz_chunk_register_member_site(&sites, offset));
        LZ_CHECK(lz_chunk_write_u16(&sites, 0, 0));
    }
    LzMemberCache *site = lz_chunk_member_site(&sites, 0);
    LZ_CHECK(site != NULL);
    site->ic_class = 42;
    LZ_CHECK(lz_chunk_member_site(&sites, 3) != site);
    LZ_CHECK(lz_chunk_member_site(&sites, 3)->ic_class == 0);
    LZ_CHECK(lz_chunk_member_site(&sites, LZ_MEMBER_SITES_MAX * 3) == NULL);
    LZ_CHECK(site->ic_class == 42);
    lz_chunk_truncate(&vm.heap, &sites, sites.count, 0);
    LZ_CHECK(site->ic_class == 0);
    lz_chunk_free(&vm.heap, &sites);
    outcome_end(&vm, &chunk, &result);
    LZ_CHECK(result.live_objects == 0 && result.live_bytes == 0);
    outcome_free(&result);
}

/* These inputs are already owned by the VM stack. Each chunk exercises one
 * non-numeric semantic path without unrelated startup opcodes in its counts. */
static void check_direct_semantics(void) {
    for (unsigned which = 0; which < 9; which++) {
        LzVM vm;
        Outcome outcome;
        outcome_begin(&vm, &outcome);
        LzChunk chunk;
        lz_chunk_init(&chunk);
        uint16_t index = 0;
        if (which == 0 || which == 3) {
            LzLink *link = lz_link_new(&vm.heap, NULL);
            LZ_CHECK(link != NULL);
            vm.stack[vm.stack_count++] = lz_value_object(&link->base);
            if (which == 0) {
                LzString *name = lz_vm_intern(&vm, "alive", 5);
                LzValue value = lz_value_object(&name->base);
                LZ_CHECK(lz_chunk_add_constant(&chunk, value, &index));
                LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_GET_FIELD, 0));
                LZ_CHECK(lz_chunk_write_u16(&chunk, index, 0));
            } else {
                vm.stack[vm.stack_count++] = lz_value_nil();
                LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_EQUAL, 0));
            }
        } else if (which == 1) {
            LzArray *array = lz_array_new(&vm.heap, 1);
            LZ_CHECK(array != NULL);
            array->items[array->count++] = lz_value_int(2);
            vm.stack[vm.stack_count++] = lz_value_object(&array->base);
            LzType type = lz_type_of_kinds(LZ_KIND_ARRAY);
            LZ_CHECK(lz_chunk_add_type(&chunk, &type, &index));
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_CHECK_PARAM, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, 0, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, index, 0));
        } else if (which >= 4 && which <= 6) {
            LzArray *array = lz_array_new(&vm.heap, 1);
            LZ_CHECK(array != NULL);
            array->items[array->count++] = lz_value_int(9);
            if (which == 6) vm.stack[vm.stack_count++] = lz_value_int(1);
            vm.stack[vm.stack_count++] = lz_value_object(&array->base);
            if (which == 5) vm.stack[vm.stack_count++] = lz_value_int(0);
            LZ_CHECK(lz_chunk_write(&chunk, which == 4 ? LZ_OP_ITER_LENGTH :
                which == 5 ? LZ_OP_ITER_NEXT : LZ_OP_ITER_CHECK, 0));
            if (which == 6) LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_NIL, 0));
        } else if (which == 7) {
            vm.stack[vm.stack_count++] = lz_value_int(7);
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_CLOSE_UPVALUE_KEEP, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, 0, 0));
        } else if (which == 8) {
            LzDict *dict = lz_dict_new(&vm.heap, 0);
            LZ_CHECK(dict != NULL);
            LzValue key = lz_value_int(0), value = lz_value_int(9);
            LZ_CHECK(lz_dict_set(&vm, dict, &key, &value) == LZ_OK);
            vm.stack[vm.stack_count++] = lz_value_object(&dict->base);
            LZ_CHECK(lz_chunk_add_constant(&chunk, key, &index));
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_GET_LOCAL, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, 0, 0));
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_CONSTANT, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, index, 0));
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_INDEX, 0));
        } else {
            LzString *text = lz_string_new(&vm.heap, "Ab", 2);
            vm.stack[vm.stack_count++] = lz_value_object(&text->base);
            vm.stack[vm.stack_count++] = lz_value_nil();
            LzString *name = lz_vm_intern(&vm, "to_upper", 8);
            LzValue value = lz_value_object(&name->base);
            LZ_CHECK(lz_chunk_add_constant(&chunk, value, &index));
            LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_METHOD_LOOKUP, 0));
            LZ_CHECK(lz_chunk_write_u16(&chunk, index, 0));
            LZ_CHECK(lz_chunk_write(&chunk, 1, 0));
        }
        LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_RETURN, 0));
        LzValue result = lz_value_nil();
        LzEngineStats stats;
        LZ_CHECK(lz_ge_eval_stats(&vm, &chunk, &result, &stats) == LZ_OK);
        LZ_CHECK(stats.translated && stats.delegated == 0);
        if (which == 0 || which == 3) LZ_CHECK(result.type == LZ_VAL_BOOL && !result.as.boolean);
        else if (which == 1) LZ_CHECK(lz_value_is_array(&result));
        else if (which == 2) LZ_CHECK(result.type == LZ_VAL_OBJECT && result.as.object->type == LZ_OBJ_NATIVE);
        else if (which == 6) LZ_CHECK(result.type == LZ_VAL_NIL);
        else LZ_CHECK(result.type == LZ_VAL_INT && result.as.integer ==
            (which == 7 ? 7 : which == 8 ? 9 : 1));
        lz_value_release(&vm.heap, &result);
        outcome_end(&vm, &chunk, &outcome);
        LZ_CHECK(outcome.live_objects == 0 && outcome.live_bytes == 0);
        outcome_free(&outcome);
    }
}

int main(void) {
    check_direct_semantics();
    check_ref_cache();
    check_len_cost();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        check_case(&cases[i]);
    }
    check_fuel();
    return LZ_TEST_RESULT();
}
