/*
 * Model-based fuzzing of links. For each seed a random Lizard program is generated (at most
 * MAX_OPS operations on a few slots: instances, arrays, dicts, closures, closure cycles, links to
 * them, links stored in fields, arrays and dicts, copies, aliases, drops, a Skink pass). While the
 * program text is generated, a small C model tracks which objects own which and decides when each
 * object is dead; it prints the expected output (every `.alive` read). The program is run in the
 * VM and the output must be identical. After each program: lz_link_check, and nothing is left
 * alive. A difference is a bug in the VM (or in this model).
 *
 * usage: fuzz_links [seeds] [first-seed] [-v]      (default: 2000 seeds; LZ_FUZZ_SEEDS overrides)
 * The _ge build and LZ_JIT=eager run the same programs through the other engines.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/debug.h"
#include "lizard/link.h"
#include "test_util.h"

#define MAX_OPS 200
#define SLOTS 6
#define LINKS 6
#define KEYS 3
#define MAX_OBJ 512
#define TICKS 4
#define MAX_EDGES 64

/* ---- the VM side ---- */

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

static void run_program(Program *program, const char *text) {
    memset(program, 0, sizeof *program);
    lz_vm_init(&program->vm, lz_writer_from_buffer(&program->out),
               lz_writer_from_buffer(&program->err));
    LZ_CHECK(lz_builtin_install(&program->vm) == LZ_OK);
    lz_source_from_string(&program->source, "f.lz", text);
    program->vm.trace_source = &program->source;
    lz_chunk_init(&program->chunk);
    program->compile_status =
        lz_compile(&program->vm, &program->source, &program->chunk, &program->diagnostics);
    if (program->compile_status == LZ_OK) {
        program->run_status = lz_vm_run(&program->vm, &program->chunk);
    }
}

static bool finish_program(Program *program) {
    bool ok = program->vm.stack_count == 0 && lz_link_check(&program->vm);
    lz_chunk_free(&program->vm.heap, &program->chunk);
    lz_vm_release_roots(&program->vm);
    ok = ok && lz_link_check(&program->vm) && program->vm.link_count == 0 &&
         program->vm.heap.live_objects == 0 && program->vm.heap.live_bytes == 0;
    lz_vm_free(&program->vm);
    lz_buffer_free(&program->out);
    lz_buffer_free(&program->err);
    return ok;
}

/* ---- random numbers ---- */

static uint64_t rng_state;

static uint64_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static int below(int limit) {
    return (int)(next_random() % (uint64_t)limit);
}

/* ---- the model ---- */

enum { K_NODE, K_ARRAY, K_DICT, K_CLOSURE, K_CYCLE, K_CHILDREN };

typedef struct Edge {
    int target; /* object id */
    bool is_link;
    int key; /* dict key, array index, or -1 */
} Edge;

typedef struct Obj {
    int kind;
    bool dead;       /* certainly dead */
    bool unsure;     /* not held by a slot, but a closure cycle (Skink, on its own schedule) may still hold it */
    bool tainted;    /* is a closure cycle, or can be reached from one: its death time is Skink's */
    int children;    /* node: id of its children array (shared by copies) */
    int up;          /* node: target of its `up` link field, or -1 */
    int name;        /* node: number in its name */
    int count;
    Edge edges[MAX_EDGES];
} Obj;

typedef struct Model {
    Obj objs[MAX_OBJ];
    int count;
    int slot[SLOTS];  /* object id or -1 */
    int link[LINKS];  /* target object id; -2: the variable holds nil; -1 is not used */
    int names;
    char *expected;
    size_t expected_length;
    size_t expected_capacity;
} Model;

static void say(Model *m, const char *format, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>

static void say(Model *m, const char *format, ...) {
    char line[128];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (m->expected_length + (size_t)n + 1 > m->expected_capacity) {
        m->expected_capacity = (m->expected_capacity + (size_t)n + 1) * 2;
        m->expected = realloc(m->expected, m->expected_capacity);
    }
    memcpy(m->expected + m->expected_length, line, (size_t)n + 1);
    m->expected_length += (size_t)n;
}

static void mark(const Model *m, int id, bool *seen) {
    if (id < 0 || seen[id]) {
        return;
    }
    seen[id] = true;
    const Obj *o = &m->objs[id];
    if (o->kind == K_NODE) {
        mark(m, o->children, seen);
    }
    for (int i = 0; i < o->count; i++) {
        if (!o->edges[i].is_link) {
            mark(m, o->edges[i].target, seen);
        }
    }
}

/*
 * Marks what is dead now. An object reachable from a slot is alive. Of the others, one that no
 * closure cycle ever held is dead for sure (reference counting frees it at once); one that a
 * cycle held (`tainted`) is "unsure": it dies when Skink collects the cycle, which the model does
 * not predict (the collector spreads its work over several passes). Its output lines are "?",
 * which match either answer.
 */
static void settle(Model *m) {
    bool seen[MAX_OBJ];
    memset(seen, 0, sizeof seen);
    for (int s = 0; s < SLOTS; s++) {
        mark(m, m->slot[s], seen);
    }
    for (int i = 0; i < m->count; i++) {
        if (seen[i]) {
            m->objs[i].unsure = false;
        } else if (m->objs[i].tainted) {
            m->objs[i].unsure = !m->objs[i].dead;
        } else {
            m->objs[i].dead = true;
        }
    }
}

static void taint(Model *m, int from) {
    bool seen[MAX_OBJ];
    memset(seen, 0, sizeof seen);
    mark(m, from, seen);
    for (int i = 0; i < m->count; i++) {
        if (seen[i]) {
            m->objs[i].tainted = true;
        }
    }
}

static bool reaches(const Model *m, int from, int to) {
    bool seen[MAX_OBJ];
    memset(seen, 0, sizeof seen);
    mark(m, from, seen);
    return seen[to];
}

static int new_obj(Model *m, int kind) {
    if (m->count >= MAX_OBJ - 4) {
        return -1;
    }
    int id = m->count++;
    memset(&m->objs[id], 0, sizeof m->objs[id]);
    m->objs[id].kind = kind;
    m->objs[id].up = -1;
    m->objs[id].children = -1;
    return id;
}

static void add_edge(Model *m, int owner, int target, bool is_link, int key) {
    Obj *o = &m->objs[owner];
    if (o->count < MAX_EDGES) {
        o->edges[o->count++] = (Edge){target, is_link, key};
    }
    if (!is_link && o->tainted) {
        taint(m, target);
    }
}

static void set_keyed_edge(Model *m, int owner, int key, int target, bool is_link) {
    Obj *o = &m->objs[owner];
    for (int i = 0; i < o->count; i++) {
        if (o->edges[i].key == key) {
            o->edges[i].target = target;
            o->edges[i].is_link = is_link;
            if (!is_link && o->tainted) {
                taint(m, target);
            }
            return;
        }
    }
    add_edge(m, owner, target, is_link, key);
}

/* ---- the generator ---- */

typedef struct Text {
    char *data;
    size_t length, capacity;
} Text;

static void put(Text *t, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void put(Text *t, const char *format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (t->length + (size_t)n + 1 > t->capacity) {
        t->capacity = (t->capacity + (size_t)n + 1) * 2;
        t->data = realloc(t->data, t->capacity);
    }
    memcpy(t->data + t->length, line, (size_t)n + 1);
    t->length += (size_t)n;
}

static const char *PRELUDE =
    "class Node { pub name: string\n"
    "  pub children: Node[] = []\n"
    "  pub link up: Node = nil }\n"
    "fn mk(o) { return fn() { return o } }\n"
    "fn cyc(o) {\n  let f = nil\n  f = fn() { return [o, f] }\n  return f\n}\n"
    "fn tick() {\n  let k = 0\n  let g = fn() { return k }\n  return 0\n}\n"
    "let s0 = nil\nlet s1 = nil\nlet s2 = nil\nlet s3 = nil\nlet s4 = nil\nlet s5 = nil\n"
    "let l0 = nil\nlet l1 = nil\nlet l2 = nil\nlet l3 = nil\nlet l4 = nil\nlet l5 = nil\n";

static bool is_live_kind(const Model *m, int slot, int kind) {
    int id = m->slot[slot];
    return id >= 0 && m->objs[id].kind == kind;
}

static int pick_slot(const Model *m) {
    int candidates[SLOTS], n = 0;
    for (int s = 0; s < SLOTS; s++) {
        if (m->slot[s] >= 0) {
            candidates[n++] = s;
        }
    }
    return n == 0 ? -1 : candidates[below(n)];
}

static int pick_slot_kind(const Model *m, int kind) {
    int candidates[SLOTS], n = 0;
    for (int s = 0; s < SLOTS; s++) {
        if (is_live_kind(m, s, kind)) {
            candidates[n++] = s;
        }
    }
    return n == 0 ? -1 : candidates[below(n)];
}

static int pick_link(const Model *m) {
    int candidates[LINKS], n = 0;
    for (int l = 0; l < LINKS; l++) {
        if (m->link[l] != -2) {
            candidates[n++] = l;
        }
    }
    return n == 0 ? -1 : candidates[below(n)];
}

static const char *alive_text(const Obj *o) {
    return o->unsure ? "?" : (o->dead ? "false" : "true");
}

static void print_alive(Model *m, Text *t, int l) {
    put(t, "println(l%d.alive)\n", l);
    say(m, "%s\n", alive_text(&m->objs[m->link[l]]));
}

/* One random operation. Returns without doing anything when its preconditions do not hold. */
static void step(Model *m, Text *t) {
    int op = below(22);
    int s = below(SLOTS), a, b, l;
    switch (op) {
    case 0: { /* a new node in slot s */
        int id = new_obj(m, K_NODE);
        int kids = new_obj(m, K_CHILDREN);
        if (id < 0 || kids < 0) {
            return;
        }
        m->objs[id].children = kids;
        m->objs[id].name = m->names++;
        put(t, "s%d = Node(name: \"n%d\")\n", s, m->objs[id].name);
        m->slot[s] = id;
        break;
    }
    case 1: { /* a new array */
        int id = new_obj(m, K_ARRAY);
        if (id < 0) {
            return;
        }
        put(t, "s%d = []\n", s);
        m->slot[s] = id;
        break;
    }
    case 2: { /* a new dict */
        int id = new_obj(m, K_DICT);
        if (id < 0) {
            return;
        }
        put(t, "s%d = {}\n", s);
        m->slot[s] = id;
        break;
    }
    case 3: { /* a closure that captures slot b */
        b = pick_slot(m);
        int id = b < 0 ? -1 : new_obj(m, K_CLOSURE);
        if (id < 0) {
            return;
        }
        add_edge(m, id, m->slot[b], false, -1);
        put(t, "s%d = mk(s%d)\n", s, b);
        m->slot[s] = id;
        break;
    }
    case 4: { /* a closure cycle that captures slot b */
        b = pick_slot(m);
        int id = b < 0 ? -1 : new_obj(m, K_CYCLE);
        if (id < 0) {
            return;
        }
        add_edge(m, id, m->slot[b], false, -1);
        m->objs[id].tainted = true;
        taint(m, m->slot[b]);
        put(t, "s%d = cyc(s%d)\n", s, b);
        m->slot[s] = id;
        break;
    }
    case 5: /* alias */
        b = pick_slot(m);
        if (b < 0 || b == s) {
            return;
        }
        put(t, "s%d = s%d\n", s, b);
        m->slot[s] = m->slot[b];
        break;
    case 6: /* drop */
        put(t, "s%d = nil\n", s);
        m->slot[s] = -1;
        break;
    case 7: /* a link to a slot's object */
        b = pick_slot(m);
        if (b < 0) {
            return;
        }
        l = below(LINKS);
        put(t, "l%d = link(s%d)\n", l, b);
        m->link[l] = m->slot[b];
        break;
    case 8: /* a link variable is dropped, or copied */
        l = below(LINKS);
        if (below(2) == 0) {
            put(t, "l%d = nil\n", l);
            m->link[l] = -2;
        } else {
            int from = pick_link(m);
            if (from < 0) {
                return;
            }
            put(t, "l%d = l%d\n", l, from);
            m->link[l] = m->link[from];
        }
        break;
    case 9: /* node.children.push(node) */
        a = pick_slot_kind(m, K_NODE);
        b = pick_slot_kind(m, K_NODE);
        if (a < 0 || b < 0) {
            return;
        }
        {
            int kids = m->objs[m->slot[a]].children;
            if (reaches(m, m->slot[b], kids) || m->objs[kids].count >= MAX_EDGES - 1) {
                return; /* would be a cycle (reference counting cannot free it) */
            }
            put(t, "s%d.children.push(s%d)\n", a, b);
            add_edge(m, kids, m->slot[b], false, -1);
        }
        break;
    case 10: /* array.push(object) */
        a = pick_slot_kind(m, K_ARRAY);
        b = pick_slot(m);
        if (a < 0 || b < 0 || reaches(m, m->slot[b], m->slot[a]) ||
            m->objs[m->slot[a]].count >= MAX_EDGES - 1) {
            return;
        }
        put(t, "s%d.push(s%d)\n", a, b);
        add_edge(m, m->slot[a], m->slot[b], false, -1);
        break;
    case 11: /* array.push(link) */
        a = pick_slot_kind(m, K_ARRAY);
        l = pick_link(m);
        if (a < 0 || l < 0 || m->objs[m->slot[a]].count >= MAX_EDGES - 1) {
            return;
        }
        put(t, "s%d.push(l%d)\n", a, l);
        add_edge(m, m->slot[a], m->link[l], true, m->objs[m->slot[a]].count);
        break;
    case 12: { /* dict["k"] = object */
        a = pick_slot_kind(m, K_DICT);
        b = pick_slot(m);
        int key = below(KEYS);
        if (a < 0 || b < 0 || reaches(m, m->slot[b], m->slot[a])) {
            return;
        }
        put(t, "s%d[\"k%d\"] = s%d\n", a, key, b);
        set_keyed_edge(m, m->slot[a], 1000 + key, m->slot[b], false);
        break;
    }
    case 13: { /* dict["k"] = link */
        a = pick_slot_kind(m, K_DICT);
        l = pick_link(m);
        int key = below(KEYS);
        if (a < 0 || l < 0) {
            return;
        }
        put(t, "s%d[\"k%d\"] = l%d\n", a, key, l);
        set_keyed_edge(m, m->slot[a], 1000 + key, m->link[l], true);
        break;
    }
    case 14: /* node.up = node (a link field) */
        a = pick_slot_kind(m, K_NODE);
        b = pick_slot_kind(m, K_NODE);
        if (a < 0 || b < 0) {
            return;
        }
        put(t, "s%d.up = s%d\n", a, b);
        m->objs[m->slot[a]].up = m->slot[b];
        break;
    case 15: /* node.up = nil */
        a = pick_slot_kind(m, K_NODE);
        if (a < 0) {
            return;
        }
        put(t, "s%d.up = nil\n", a);
        m->objs[m->slot[a]].up = -1;
        break;
    case 16: { /* copy a node: a new node that shares the children array and the link target */
        b = pick_slot_kind(m, K_NODE);
        int id = b < 0 ? -1 : new_obj(m, K_NODE);
        if (id < 0) {
            return;
        }
        const Obj *src = &m->objs[m->slot[b]];
        m->objs[id].children = src->children;
        m->objs[id].up = src->up;
        m->objs[id].name = src->name;
        put(t, "s%d = s%d.copy()\n", s, b);
        m->slot[s] = id;
        break;
    }
    case 17: /* read through a link */
        l = pick_link(m);
        if (l < 0) {
            return;
        }
        print_alive(m, t, l);
        break;
    case 18: /* a method or field read through a link to a live node */
        l = pick_link(m);
        if (l < 0 || m->objs[m->link[l]].dead || m->objs[m->link[l]].unsure ||
            m->objs[m->link[l]].kind != K_NODE) {
            return;
        }
        put(t, "println(l%d.name)\n", l);
        say(m, "n%d\n", m->objs[m->link[l]].name);
        break;
    case 19: /* .target */
        l = pick_link(m);
        if (l < 0) {
            return;
        }
        put(t, "println(l%d.target == nil)\n", l);
        say(m, "%s\n", m->objs[m->link[l]].unsure ? "?" : (m->objs[m->link[l]].dead ? "true" : "false"));
        break;
    default: /* a Skink pass */
        /* Skink works through its candidates over several passes; a few of them give it a
         * chance to run. The model does not rely on when it finishes (see settle). */
        for (int pass = 0; pass < TICKS; pass++) {
            put(t, "tick()\n");
        }
        break;
    }
    settle(m);
}

static void finale(Model *m, Text *t) {
    for (int l = 0; l < LINKS; l++) {
        if (m->link[l] != -2) {
            print_alive(m, t, l);
        }
    }
    for (int s = 0; s < SLOTS; s++) {
        int id = m->slot[s];
        if (id < 0) {
            continue;
        }
        const Obj *o = &m->objs[id];
        if (o->kind == K_NODE) {
            put(t, "println(s%d.up != nil)\n", s);
            say(m, "%s\n", o->up < 0 ? "false" : (m->objs[o->up].unsure ? "?" : (m->objs[o->up].dead ? "false" : "true")));
        } else if (o->kind == K_ARRAY) {
            for (int i = 0; i < o->count; i++) {
                if (o->edges[i].is_link) {
                    put(t, "println(s%d[%d].alive)\n", s, i);
                    say(m, "%s\n", alive_text(&m->objs[o->edges[i].target]));
                }
            }
        } else if (o->kind == K_DICT) {
            for (int i = 0; i < o->count; i++) {
                if (o->edges[i].is_link) {
                    put(t, "println(s%d[\"k%d\"].alive)\n", s, o->edges[i].key - 1000);
                    say(m, "%s\n", alive_text(&m->objs[o->edges[i].target]));
                }
            }
        }
    }
}

/* Array elements are indexed by position: an array's link edge key is its index, which the
 * model records at push time (edge count == position because arrays only grow). */

static int ops_limit = 0; /* debugging: stop the program after this many operations */

/* Equal line by line; an expected "?" matches "true" and "false". */
static bool same_output(const char *expected, const char *actual) {
    while (*expected != '\0' && *actual != '\0') {
        const char *e_end = strchr(expected, '\n');
        const char *a_end = strchr(actual, '\n');
        if (e_end == NULL || a_end == NULL) {
            return false;
        }
        size_t e_len = (size_t)(e_end - expected), a_len = (size_t)(a_end - actual);
        bool wildcard = e_len == 1 && expected[0] == '?';
        if (wildcard) {
            if (!((a_len == 4 && strncmp(actual, "true", 4) == 0) || (a_len == 5 && strncmp(actual, "false", 5) == 0))) {
                return false;
            }
        } else if (e_len != a_len || strncmp(expected, actual, e_len) != 0) {
            return false;
        }
        expected = e_end + 1;
        actual = a_end + 1;
    }
    return *expected == '\0' && *actual == '\0';
}

static int fuzz_one(uint64_t seed, bool verbose) {
    static Model model;
    memset(&model, 0, sizeof model);
    for (int s = 0; s < SLOTS; s++) {
        model.slot[s] = -1;
    }
    for (int l = 0; l < LINKS; l++) {
        model.link[l] = -2;
    }
    rng_state = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    for (int i = 0; i < 4; i++) {
        next_random();
    }

    Text text = {0};
    put(&text, "%s", PRELUDE);
    int ops = 20 + below(MAX_OPS - 20 + 1);
    if (ops_limit > 0 && ops_limit < ops) {
        ops = ops_limit;
    }
    for (int i = 0; i < ops; i++) {
        step(&model, &text);
    }
    finale(&model, &text);

    Program program;
    run_program(&program, text.data);
    const char *actual = program.out.data != NULL ? program.out.data : "";
    const char *expected = model.expected != NULL ? model.expected : "";
    int failed = 0;
    if (program.compile_status != LZ_OK || program.run_status != LZ_OK || !same_output(expected, actual)) {
        failed = 1;
        fprintf(stderr, "fuzz_links: seed %llu: %s\n", (unsigned long long)seed,
                program.compile_status != LZ_OK ? "compile error"
                : program.run_status != LZ_OK   ? program.vm.error
                                                : "output differs from the model");
        if (verbose) {
            for (int i = 0; i < model.count; i++) {
                const Obj *o = &model.objs[i];
                fprintf(stderr, "obj %d kind %d dead %d unsure %d children %d up %d edges:", i, o->kind,
                        o->dead, o->unsure, o->children, o->up);
                for (int e = 0; e < o->count; e++) {
                    fprintf(stderr, " %s%d", o->edges[e].is_link ? "L" : "", o->edges[e].target);
                }
                fputc('\n', stderr);
            }
            for (int k = 0; k < SLOTS; k++) {
                fprintf(stderr, "s%d=%d l%d=%d\n", k, model.slot[k], k, model.link[k]);
            }
        }
        {
            fprintf(stderr, "--- program ---\n%s--- expected ---\n%s--- got ---\n%s---\n", text.data,
                    expected, actual);
        }
    }
    if (!finish_program(&program)) {
        failed = 1;
        fprintf(stderr, "fuzz_links: seed %llu: invariant check or leak after the program\n",
                (unsigned long long)seed);
    }
    free(text.data);
    free(model.expected);
    model.expected = NULL;
    return failed;
}

int main(int argc, char **argv) {
    long seeds = 2000;
    long first = 1;
    const char *env = getenv("LZ_FUZZ_SEEDS");
    if (env != NULL && atol(env) > 0) {
        seeds = atol(env);
    }
    if (argc > 1 && atol(argv[1]) > 0) {
        seeds = atol(argv[1]);
    }
    if (argc > 2) {
        first = atol(argv[2]);
    }
    if (argc > 4) {
        ops_limit = atoi(argv[4]);
    }
    int failures = 0;
    for (long i = 0; i < seeds && failures < 5; i++) {
        failures += fuzz_one((uint64_t)(first + i), argc > 3);
    }
    LZ_CHECK(failures == 0);
    printf("fuzz_links: %ld seeds, %d failures\n", seeds, failures);
    return LZ_TEST_RESULT();
}
