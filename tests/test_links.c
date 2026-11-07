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
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
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
              strstr(program.vm.error_help, "use a link field") != NULL;
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
    check_run_error("let a = []\nlet b = [a]\na.push(b)\n", "use a link field");
    check_run_error("let a = {}\nlet b = {\"a\": a}\na[\"b\"] = b\n", "use a link field");
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

int main(void) {
    test_parser();
    test_builtin();
    test_dead_target();
    test_fields();
    test_error_links();
    test_cycle_message();
    test_freeing();
    return LZ_TEST_RESULT();
}
