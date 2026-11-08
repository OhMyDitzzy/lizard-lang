#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/debug.h"
#include "lizard/link.h"
#include "test_util.h"

typedef struct Program {
    LzVM vm;
    LzBuffer out;
    LzBuffer err;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    LzSource source;
    LzStatus compile_status;
    LzStatus run_status;
} Program;

static void run_limited(Program *program, const char *text, uint64_t fuel, size_t heap_limit) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);
    if (heap_limit != 0) {
        lz_vm_set_heap_limit(&program->vm, heap_limit);
    }

    lz_source_from_string(&program->source, "t.lz", text);
    program->vm.trace_source = &program->source;
    lz_chunk_init(&program->chunk);
    program->compile_status =
        lz_compile(&program->vm, &program->source, &program->chunk, &program->diagnostics);

    if (fuel != 0) {
        lz_vm_set_fuel(&program->vm, fuel);
    }
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

static void run(Program *program, const char *text, uint64_t fuel) {
    run_limited(program, text, fuel, 0);
}

static void finish(Program *program) {
    LZ_CHECK(program->vm.stack_count == 0);
    LZ_CHECK(lz_link_check(&program->vm)); /* the table agrees with the heap */
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    LZ_CHECK(lz_link_check(&program->vm));
    /* No cell is left in the link table either: every link is gone with the objects. */
    LZ_CHECK(program->vm.link_count == 0);
    LZ_CHECK(program->vm.heap.live_objects == 0);
    LZ_CHECK(program->vm.heap.live_bytes == 0);
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
}

static void check_output(const char *text, const char *expected) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    LZ_CHECK(program.run_status == LZ_OK);
    const char *actual = program.out.data != NULL ? program.out.data : "";
    bool same = strcmp(actual, expected) == 0;
    if (!same) {
        fprintf(stderr, "program:\n%s\nexpected: [%s]\ngot: [%s]\n", text, expected, actual);
        for (size_t i = 0; i < program.diagnostics.count; i++) {
            fprintf(stderr, "diagnostic: %s\n", program.diagnostics.items[i].message);
        }
        if (program.run_status != LZ_OK) {
            fprintf(stderr, "runtime: %s\n", program.vm.error);
        }
    }
    LZ_CHECK(same);
    finish(&program);
}

static void check_compile_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    bool ok = program.compile_status == LZ_ERR_COMPILE && program.diagnostics.count > 0 &&
              strstr(program.diagnostics.items[0].message, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a compile error with [%s]; got status %d [%s]\n", text,
                message, (int)program.compile_status,
                program.diagnostics.count > 0 ? program.diagnostics.items[0].message : "no diagnostic");
    }
    LZ_CHECK(ok);
    LZ_CHECK(program.diagnostics.count == 1);
    finish(&program);
}

static void check_run_error(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status != LZ_OK && strstr(program.vm.error, message) != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a run-time error with [%s]; got %d [%s]\n", text,
                message, (int)program.run_status, program.vm.error);
    }
    LZ_CHECK(ok);
    finish(&program);
}

/* ---- 1. the parser: the `link` modifier on a field ---- */

static void test_parser(void) {
    check_output("class Node { pub name: string\n  pub link parent: Node = nil }\n"
                 "let n = Node(name: \"n\")\nlet c = Node(name: \"c\", parent: n)\nprintln(c.parent == n)\n",
                 "true\n");
    /* `link` without a name after it is just a field name. */
    check_output("class C { pub link: int = 4 }\nprintln(C().link)\n", "4\n");
    /* `pub read link x` is a read-only link field. */
    check_output("class N { pub name: string\n  pub read link owner: N = nil }\n"
                 "let n = N(name: \"n\")\nlet c = N(name: \"c\", owner: n)\nprintln(c.owner == n)\n",
                 "true\n");
    check_compile_error("class C { link fn f(self) => 1 }", "a link can only be a field");
    check_compile_error("class C { pub link const X = 1 }", "a link can only be a field");
}

/* ---- 2 and 3. the object, the table, the builtin and its properties ---- */

static void test_builtin(void) {
    check_output("class Node { pub name: string }\nlet n = Node(name: \"root\")\nlet l = link(n)\n"
                 "println(l.alive)\nprintln(l.target)\nprintln(l == link(n))\nprintln(l)\n",
                 "true\nNode(name: \"root\")\ntrue\n<link Node>\n");
    /* link(link(x)) is a link to the same x. */
    check_output("class Node { pub name: string }\nlet n = Node(name: \"n\")\n"
                 "let a = link(n)\nlet b = link(a)\nprintln(a.target == b.target, b.alive)\n",
                 "true true\n");
    /* arrays, dicts, closures and functions can be linked; strings and scalars cannot. */
    check_output("let a = [1, 2]\nlet l = link(a)\nprintln(l.alive, l.target == a)\n"
                 "let d = {\"k\": 1}\nprintln(link(d).target == d)\n"
                 "let f = fn(x) => x + 1\nprintln(link(f).alive)\nlet c = 3\nlet g = fn(x) => x + c\n"
                 "println(link(g).alive)\n",
                 "true true\ntrue\ntrue\ntrue\n");
    check_run_error("let x = link(5)\n",
                    "link() needs an object that can be shared (instance, array, dict or function), got int");
    check_run_error("let x = link(\"s\")\n",
                    "link() needs an object that can be shared (instance, array, dict or function), got string");
    /* A link fits in an array and a dict: it is a leaf and makes no cycle. */
    check_output("class Node { pub name: string }\nlet n = Node(name: \"n\")\n"
                 "let l = link(n)\nlet xs = [l]\nprintln(xs[0].target == n, {\"l\": l}[\"l\"].alive)\n",
                 "true true\n");
}

/* ---- 2. a link is emptied when its target is freed ---- */

static void test_dead_target(void) {
    check_output("class Node { pub name: string }\nlet n = Node(name: \"n\")\nlet l = link(n)\n"
                 "println(l.alive, l.target == n)\nn = nil\nprintln(l.alive)\nprintln(l.target)\n"
                 "println(l)\n",
                 "true true\nfalse\nnil\nnil\n");
    /* The target of a link field may die before the object: it reads as nil, no crash. */
    check_output("class Node { pub name: string\n  pub link child: Node = nil }\n"
                 "let root = Node(name: \"root\")\nlet kid = Node(name: \"kid\", child: root)\n"
                 "root = nil\nprintln(kid.child)\n",
                 "nil\n");
}

/* ---- 4. link fields ---- */

static void test_fields(void) {
    /* Arguments, defaults, write and read. */
    check_output("class Node {\n  pub name: string\n  pub link parent: Node = nil\n  pub children: Node[] = []\n}\n"
                 "let root = Node(name: \"root\")\nlet kid = Node(name: \"kid\", parent: root)\n"
                 "root.children.push(kid)\nprintln(kid.parent == root, kid.parent.name)\n"
                 "println(kid.parent.parent)\n",
                 "true root\nnil\n");
    /* Writing accepts an object, nil or a link. */
    check_output("class Node { pub name: string\n  pub link parent: Node = nil }\n"
                 "let a = Node(name: \"a\")\nlet b = Node(name: \"b\")\nb.parent = a\n"
                 "b.parent = link(a)\nb.parent = nil\nprintln(b.parent)\n",
                 "nil\n");
    /* The type applies to the target. */
    check_run_error("class A { pub name: string }\nclass B { pub name: string }\n"
                    "class Box { pub link x: A = nil }\nlet box = Box()\nbox.x = B(name: \"b\")\n",
                    "expects A");
    /* copy() and with() make a fresh cell pointing at the same target. */
    check_output("class Node { pub name: string\n  pub link parent: Node = nil }\n"
                 "let a = Node(name: \"a\")\nlet b = Node(name: \"b\", parent: a)\n"
                 "let c = b.copy()\nlet d = b.with(name: \"d\")\n"
                 "println(c.parent == a, d.parent == a)\nb.parent = nil\nprintln(c.parent == a)\n",
                 "true true\ntrue\n");
    /* == compares targets, printing shows the target. */
    check_output("class Node { pub name: string }\nlet a = Node(name: \"a\")\nlet b = Node(name: \"b\")\n"
                 "class Box { pub link x: Node = nil }\nlet p = Box(x: a)\nlet q = Box(x: a)\n"
                 "let r = Box(x: b)\nprintln(p.x == q.x, p.x == r.x, p == q)\nprintln(p)\n",
                 "true false true\nBox(x: <link Node>)\n");
    /* A method can be called through a link field. */
    check_output("class Node { pub name: string\n  pub fn hello(self) => $\"hi {self.name}\"\n"
                 "  pub link parent: Node = nil }\n"
                 "let a = Node(name: \"a\")\nlet b = Node(name: \"b\", parent: a)\nprintln(b.parent.hello())\n",
                 "hi a\n");
    /* A method called through a standalone link: it runs on the target and leaves one reference only. */
    check_output("class Node { pub name: string\n  pub fn hello(self) => $\"hi {self.name}\" }\n"
                 "let n = Node(name: \"n\")\nlet l = link(n)\nprintln(l.hello())\nn = nil\n"
                 "println(l.alive)\n",
                 "hi n\nfalse\n");
    check_run_error("class Node { pub name: string\n  pub fn hello(self) => $\"hi {self.name}\" }\n"
                    "let n = Node(name: \"n\")\nlet l = link(n)\nn = nil\nl.hello()\n",
                    "points at nothing");
}

/* ---- 4. an error class may carry a link field ---- */

static void test_error_links(void) {
    check_output("class Node { pub name: string }\nerror E : RuntimeError { pub link origin: Node = nil }\n"
                 "let n = Node(name: \"n\")\nlet e = E(\"boom\", origin: n)\nprintln(e.origin == n)\nn = nil\n"
                 "println(e.origin)\ntry { throw e } catch caught { println(caught.message, caught.origin) }\n",
                 "true\nnil\nboom nil\n");
}

/* ---- 5. the cycle message points at link ---- */

/* The refusal, and the help line that tells the user about `link`. */
static void check_cycle_with_link_help(const char *text, const char *message) {
    Program program;
    run(&program, text, 0);
    LZ_CHECK(program.compile_status == LZ_OK);
    bool ok = program.run_status != LZ_OK && strstr(program.vm.error, message) != NULL &&
              strstr(program.vm.error_help, "link") != NULL;
    if (!ok) {
        fprintf(stderr, "program:\n%s\nexpected a cycle error [%s] with a link hint; got [%s] [%s]\n",
                text, message, program.vm.error, program.vm.error_help);
    }
    LZ_CHECK(ok);
    finish(&program);
}

static void test_cycle_message(void) {
    check_cycle_with_link_help("class Box { pub inner: any = nil }\nlet a = Box()\na.inner = a\n",
                               "cannot store");
    check_run_error("let a = []\nlet b = [a]\na.push(b)\n", "reference cycle");
    check_run_error("let a = {}\nlet b = {\"a\": a}\na[\"b\"] = b\n", "reference cycle");
    /* A parent/child tree with a link back to the parent is fine. */
    check_output("class Node { pub name: string\n  pub link parent: Node = nil\n  pub children: Node[] = [] }\n"
                 "let root = Node(name: \"root\")\nlet kid = Node(name: \"kid\", parent: root)\n"
                 "root.children.push(kid)\nprintln(len(root.children), kid.parent.name)\n",
                 "1 root\n");
}

/* ---- 6 and 7. freeing: trees with links, long chains, no skink needed ---- */

static void test_freeing(void) {
    /* A deep tree with a link back to its parent is freed whole. */
    check_output("class Node { pub link parent: Node = nil\n  pub children: Node[] = [] }\n"
                 "let root = Node()\nlet i = 0\nwhile i < 50000 { let child = Node(parent: root)\n"
                 " root.children.push(child)\n i += 1 }\nprintln(len(root.children))\nroot = nil\n",
                 "50000\n");
    /* A long chain of owning `next` fields is released without a deep C stack. */
    check_output("class Node { pub next: any = nil }\nlet head = Node()\nlet i = 0\n"
                 "let tail = head\nwhile i < 100000 { let n = Node(next: head)\n head = n\n i += 1 }\n"
                 "println(i)\nhead = nil\ntail = nil\n",
                 "100000\n");
    /* A link does not keep its target alive: dropping the only owner frees it. */
    check_output("class Node { pub name: string }\nlet l = nil\n{ let n = Node(name: \"n\")\n"
                 "  l = link(n)\n}\nprintln(l.alive)\n", "false\n");
}


/* ---- item 1: a link to an object that dies as a child (queued release) is emptied ---- */

static const char *NODE_CLASS =
    "class Node { pub name: string\n  pub children: Node[] = []\n  pub link up: Node = nil\n"
    "  pub fn hello(self) => $\"hi {self.name}\" }\n";

static void check_with_node(const char *body, const char *expected) {
    char text[2048];
    snprintf(text, sizeof text, "%s%s", NODE_CLASS, body);
    check_output(text, expected);
}

static void test_queued_release(void) {
    /* a child of an array field */
    check_with_node("let root = Node(name: \"root\")\nlet kid = Node(name: \"kid\")\n"
                    "root.children.push(kid)\nlet l = link(kid)\nkid = nil\nprintln(l.alive)\n"
                    "root = nil\nprintln(l.alive)\nprintln(l.target)\n",
                    "true\nfalse\nnil\n");
    /* an item of an array */
    check_with_node("let xs = [Node(name: \"a\"), Node(name: \"b\")]\nlet l = link(xs[1])\n"
                    "println(l.alive)\nxs = nil\nprintln(l.alive, l.target)\n",
                    "true\nfalse nil\n");
    /* an instance field (a plain, owning one) */
    check_with_node("class Holder { pub item: any = nil }\nlet n = Node(name: \"n\")\n"
                    "let h = Holder(item: n)\nlet l = link(n)\nn = nil\nprintln(l.alive)\nh = nil\n"
                    "println(l.alive, l.target)\n",
                    "true\nfalse nil\n");
    /* a dict value */
    check_with_node("let d = {\"k\": Node(name: \"v\")}\nlet l = link(d[\"k\"])\nprintln(l.alive)\n"
                    "d = nil\nprintln(l.alive, l.target)\n",
                    "true\nfalse nil\n");
    /* an array held by a dict held by an array: a dict value that is itself linked */
    check_with_node("let d = {\"k\": [1, 2]}\nlet l = link(d[\"k\"])\nd = nil\nprintln(l.alive)\n",
                    "false\n");
    /* an object captured by a closure (an upvalue) */
    check_with_node("fn keep(o) { return fn() { return o.name } }\n"
                    "let n = Node(name: \"cap\")\nlet l = link(n)\nlet f = keep(n)\n"
                    "n = nil\nprintln(l.alive, f())\nf = nil\nprintln(l.alive, l.target)\n",
                    "true cap\nfalse nil\n");
    /* three levels: root -> a -> b -> c; links to each */
    check_with_node("let root = Node(name: \"r\")\nlet a = Node(name: \"a\")\nlet b = Node(name: \"b\")\n"
                    "let c = Node(name: \"c\")\nb.children.push(c)\na.children.push(b)\n"
                    "root.children.push(a)\nlet la = link(a)\nlet lb = link(b)\nlet lc = link(c)\n"
                    "a = nil\nb = nil\nc = nil\nprintln(la.alive, lb.alive, lc.alive)\nroot = nil\n"
                    "println(la.alive, lb.alive, lc.alive)\nprintln(la.target, lb.target, lc.target)\n",
                    "true true true\nfalse false false\nnil nil nil\n");
    /* a link field that points at a child dies with the tree */
    check_with_node("let root = Node(name: \"r\")\nlet kid = Node(name: \"k\", up: root)\n"
                    "root.children.push(kid)\nlet l = link(root)\nlet k2 = link(kid)\nkid = nil\n"
                    "println(kid)\nprintln(k2.alive)\nroot = nil\nprintln(l.alive, k2.alive)\n",
                    "nil\ntrue\nfalse false\n");
    /* the target of several cells, one of them inside the dying tree */
    check_with_node("let root = Node(name: \"r\")\nlet kid = Node(name: \"k\")\nroot.children.push(kid)\n"
                    "let l1 = link(kid)\nlet l2 = link(kid)\nlet holder = [l1, l2]\nkid = nil\nroot = nil\n"
                    "println(holder[0].alive, holder[1].alive)\n",
                    "false false\n");
}

/* A closure cycle that Skink frees: the objects it frees go through the same release path. */
static void test_skink_cycle(void) {
    check_with_node("fn tick() {\n  let k = 0\n  let g = fn() { return k }\n  return 0\n}\n"
                    "fn make(o) {\n  let f = nil\n  f = fn() { return [o, f] }\n  return f\n}\n"
                    "let n = Node(name: \"n\")\nlet l = link(n)\nlet c = make(n)\nn = nil\nc = nil\n"
                    "tick()\nprintln(l.alive, l.target)\n",
                    "false nil\n");
    /* the same, with the cycle still held: the target lives; dropping it later frees both */
    check_with_node("fn make(o) {\n  let f = nil\n  f = fn() { return [o, f] }\n  return f\n}\n"
                    "let n = Node(name: \"n\")\nlet l = link(n)\nlet c = make(n)\nn = nil\n"
                    "println(l.alive)\n",
                    "true\n");
}

/* ---- item 2: a failed registration leaves no cell behind ---- */

static void run_with_failing_row(const char *text, int fail_at, bool *ran_out_of_memory) {
    Program program;
    lz_link_fail_next_row = fail_at;
    run(&program, text, 0);
    lz_link_fail_next_row = 0;
    LZ_CHECK(program.compile_status == LZ_OK);
    *ran_out_of_memory = program.run_status != LZ_OK && strstr(program.vm.error, "out of memory") != NULL;
    finish(&program); /* lz_link_check: no unregistered cell; live_objects == 0 */
}

static void test_row_failure(void) {
    bool oom = false;
    /* link(x) itself */
    run_with_failing_row("class N { pub v: int = 1 }\nlet n = N()\nlet l = link(n)\nprintln(l)\n", 1, &oom);
    LZ_CHECK(oom);
    /* writing a link field */
    run_with_failing_row("class N { pub v: int = 1\n  pub link up: N = nil }\nlet a = N()\nlet b = N()\n"
                         "b.up = a\n",
                         1, &oom);
    LZ_CHECK(oom);
    /* the constructor argument of a link field */
    run_with_failing_row("class N { pub v: int = 1\n  pub link up: N = nil }\nlet a = N()\nlet b = N(up: a)\n",
                         1, &oom);
    LZ_CHECK(oom);
    /* copy(): the source's link has its row; the copy needs a fresh cell only (no new row), so the
       row must be made for a brand new target to fail: fail the second row of the program */
    run_with_failing_row("class N { pub v: int = 1\n  pub link up: N = nil }\nlet a = N()\nlet b = N(up: a)\n"
                         "let c = b.copy()\nprintln(c.up == a)\n",
                         2, &oom);
    LZ_CHECK(!oom); /* the copy shares a's row: nothing to fail, and no crash */
    /* a second target's row fails after the first succeeded */
    run_with_failing_row("class N { pub v: int = 1 }\nlet a = N()\nlet b = N()\nlet la = link(a)\n"
                         "let lb = link(b)\nprintln(la.alive)\n",
                         2, &oom);
    LZ_CHECK(oom);
}

/* A real failing allocation of the cell itself (heap limit), also through copy(). */
static void test_cell_failure(void) {
    size_t compiled = 0;
    for (size_t limit = 6000; limit < 15000; limit += 8) {
        Program program;
        run_limited(&program,
                    "class N { pub v: int = 1\n  pub link up: N = nil }\nlet a = N()\nlet b = N(up: a)\n"
                    "let c = b.copy()\nlet l = link(c)\nc.up = l\nprintln(1)\n",
                    0, limit);
        if (program.compile_status == LZ_OK) { /* a tiny limit may refuse the compiler itself */
            compiled++;
        }
        finish(&program);
    }
    LZ_CHECK(compiled > 50);
}

/* ---- item 4: a link cell is not the object it points at (matrix: operation x kind of link) ---- */

typedef struct { const char *name, *setup; } LinkKind;
typedef struct { const char *name, *code; bool refused; } LinkOp;

static const LinkKind LINK_KINDS[] = {
    {"instance", "let v = Node(name: \"n\")\nlet l = link(v)\n"},
    {"array", "let v = [1, 2, 3]\nlet l = link(v)\n"},
    {"dict", "let v = {\"a\": 1}\nlet l = link(v)\n"},
    {"closure", "let v = mk()\nlet l = link(v)\n"},
    {"dead", "let v = Node(name: \"n\")\nlet l = link(v)\nv = nil\n"},
    {"nil", "let l = link(nil)\n"},
};

/* refused: must fail with an error that names the link; the others have defined behaviour. */
static const LinkOp LINK_OPS[] = {
    {"index", "println(l[0])", true},
    {"setindex", "l[0] = 1", true},
    {"index_str", "println(l[\"a\"])", true},
    {"slice", "println(l[0:1])", true},
    {"for", "for x in l { println(x) }", true},
    {"len", "println(len(l))", true},
    {"in", "println(1 in l)", true},
    {"call", "println(l())", true},
    {"add", "println(l + 1)", true},
    {"add_left", "println(1 + l)", true},
    {"concat", "println(\"a\" + l)", true},
    {"neg", "println(-l)", true},
    {"bits", "println(l & 1)", true},
    {"lt", "println(l < 1)", true},
    {"lt_link", "println(l < l)", true},
    {"int", "println(int(l))", true},
    {"float", "println(float(l))", true},
    {"copy", "println(l.copy())", true},
    {"with", "println(l.with(name: \"x\"))", true},
    {"push", "l.push(1)", true},
    {"push_fn", "push(l, 1)", true},
    {"sort", "println(sort(l))", true},
    {"join", "println(join(l, \",\"))", true},
    {"spread", "fn f(*a) { return len(a) }\nprintln(f(*l))", true},
    {"throw", "throw l", true},
    {"param_node", "fn f(x: Node) { return x.name }\nprintln(f(l))", true},
    {"param_array", "fn f(x: int[]) { return len(x) }\nprintln(f(l))", true},
    {"param_dict", "fn f(x: dict) { return len(x) }\nprintln(f(l))", true},
    {"typed_let", "let y: Node = l\nprintln(y.name)", true},
    {"typed_array", "let ys: Node[] = [l]\nprintln(len(ys))", true},
    {"return_type", "fn f() -> Node { return l }\nprintln(f().name)", true},
    {"field_arr", "println(l.name)", true}, /* for a non-instance; instance + nil/dead handled below */
    /* defined behaviour, never a crash: */
    {"alive", "println(l.alive)", false},
    {"target", "println(l.target)", false},
    {"is", "println(l is Node)", false},
    {"eq", "println(l == l)", false},
    {"identical", "println(identical(l, l))", false},
    {"print", "println(l)", false},
    {"string", "println(string(l))", false},
    {"interp", "println($\"x{l}\")", false},
    {"param_any", "fn f(x) { return x }\nprintln(f(l) == l)", false},
    {"store_array", "let xs = [l]\nprintln(len(xs))", false},
    {"store_dict", "let d = {\"k\": l}\nprintln(len(d))", false},
    {"truthy", "if l { println(1) }", false},
};

static void test_link_matrix(void) {
    size_t cells = 0, refused_cells = 0, named_by_link = 0;
    for (size_t k = 0; k < sizeof LINK_KINDS / sizeof LINK_KINDS[0]; k++) {
        for (size_t o = 0; o < sizeof LINK_OPS / sizeof LINK_OPS[0]; o++) {
            char text[2048];
            snprintf(text, sizeof text, "%sfn mk() { return fn() { return 1 } }\n%s%s\n", NODE_CLASS,
                     LINK_KINDS[k].setup, LINK_OPS[o].code);
            Program program;
            run(&program, text, 0);
            if (program.compile_status != LZ_OK) {
                fprintf(stderr, "matrix %s x %s: compile error\n", LINK_KINDS[k].name, LINK_OPS[o].name);
            }
            LZ_CHECK(program.compile_status == LZ_OK);
            bool is_instance_field = strcmp(LINK_OPS[o].name, "field_arr") == 0 &&
                                     strcmp(LINK_KINDS[k].name, "instance") == 0;
            if (LINK_OPS[o].refused && !is_instance_field) {
                /* An error, always. Most name the link; a few are other defined errors (a method that
                 * does not exist on the target, an array literal of `any` for a typed array). */
                if (program.run_status == LZ_OK) {
                    fprintf(stderr, "matrix %s x %s: expected an error\n", LINK_KINDS[k].name,
                            LINK_OPS[o].name);
                }
                LZ_CHECK(program.run_status != LZ_OK);
                if (strstr(program.vm.error, "link") != NULL) {
                    named_by_link++;
                }
                refused_cells++;
            } else if (!LINK_OPS[o].refused) {
                if (program.run_status != LZ_OK) {
                    fprintf(stderr, "matrix %s x %s: unexpected error [%s]\n", LINK_KINDS[k].name,
                            LINK_OPS[o].name, program.vm.error);
                }
                LZ_CHECK(program.run_status == LZ_OK);
            }
            finish(&program); /* lz_link_check, live_objects == 0 */
            cells++;
        }
    }
    LZ_CHECK(cells >= 30 * 6);
    LZ_CHECK(named_by_link * 10 >= refused_cells * 8); /* at least 80% say "link" */
}

/* A method called through a link runs on the object: `self` is the target, not the cell. */
static void test_method_through_link(void) {
    check_with_node("let n = Node(name: \"n\")\nlet l = link(n)\nprintln(l.hello())\nprintln(l.name)\n"
                    "n = nil\nprintln(l.alive)\n",
                    "hi n\nn\nfalse\n");
}

int main(void) {
    test_parser();
    test_builtin();
    test_dead_target();
    test_fields();
    test_error_links();
    test_cycle_message();
    test_freeing();
    test_queued_release();
    test_skink_cycle();
    test_row_failure();
    test_cell_failure();
    test_link_matrix();
    test_method_through_link();
    return LZ_TEST_RESULT();
}
