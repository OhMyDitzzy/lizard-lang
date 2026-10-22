#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine_compare.h"
#include "lizard/engine.h"
#include "test_util.h"

/*
 * Differential fuzzing of the Gecko Engine against the reference interpreter.
 *
 *  1. Random Lizard programs: valid syntax, bounded loops, every operator, mixed
 *     ints and floats, globals and locals, arrays. Exercises what the compiler emits.
 *  2. Random bytecode: well-formed instruction sequences with idioms GE fuses, plus
 *     stack underflows, missing locals, jumps into the middle of fused runs, odd
 *     operands and the occasional broken jump. Exercises what the compiler never emits.
 *
 * Every run uses its own fuel budget, so metering has to stop at the same
 * instruction on both engines too. Usage: fuzz_engine [programs [chunks [seed]]]
 */

static uint64_t rng_state = 1;

static uint64_t next_random(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(2685821657736338717);
}

static unsigned below(unsigned limit) {
    return (unsigned)(next_random() % limit);
}

/* ---------------------------------------------------------------- programs */

typedef struct Gen {
    char text[32768];
    size_t length;
    char names[40][8]; /* scalar variables in scope */
    int name_count;
    int arrays;        /* a0..a{arrays-1}, each created non-empty, numbers only */
    int objs;          /* b0..b{objs-1}: arrays holding strings and arrays, to exercise reference counting */
    int serial;        /* unique suffix for loop counters and temporaries */
    bool safe;         /* keep values small and divisors non-zero, so programs run deep before failing */
} Gen;

static void emit(Gen *g, const char *format, ...) {
    va_list args;
    va_start(args, format);
    size_t room = sizeof g->text - g->length;
    int written = vsnprintf(g->text + g->length, room, format, args);
    va_end(args);
    if (written >= 0 && (size_t)written < room) {
        g->length += (size_t)written;
    } else {
        g->text[sizeof g->text - 1] = '\0'; /* out of space: the caller stops generating */
        g->length = sizeof g->text - 1;
    }
}

static const char *pick_name(Gen *g) {
    return g->names[below((unsigned)g->name_count)];
}

static void gen_expr(Gen *g, int depth);

static void gen_literal(Gen *g) {
    static const char *const floats[] = {"0.5", "2.0", "1.5", "3.25", "1e3", "0.1", "100.0", "7.0"};
    unsigned r = below(24);
    if (g->safe && r >= 16 && r < 20) {
        r = 0; /* no huge or negative literals in safe mode */
    }
    if (r < 13) {
        emit(g, "%u", below(10));
    } else if (r < 16) {
        emit(g, "%u", 10 + below(90));
    } else if (r < 17) {
        emit(g, "9223372036854775807");
    } else if (r < 18) {
        emit(g, "4611686018427387904");
    } else if (r < 19) {
        emit(g, "(-%u)", 1 + below(9));
    } else if (r < 20) {
        emit(g, "1000000007");
    } else {
        emit(g, "%s", floats[below(sizeof floats / sizeof floats[0])]);
    }
}

static void gen_index(Gen *g, int array, int depth) {
    /* int(...) keeps the compiler from proving the index is a float and refusing the program;
     * the run-time conversion may still fail (nan, infinity, out of range), which is the point. */
    emit(g, "a%d[", array);
    unsigned r = below(10);
    if (r < 7) {
        emit(g, "int(");
        gen_expr(g, depth - 1);
        emit(g, ") %% len(a%d)", array);
    } else if (r < 9) {
        emit(g, "%d", below(2) == 0 ? (int)below(4) : -(int)(1 + below(3)));
    } else {
        emit(g, "int(");
        gen_expr(g, depth - 1);
        emit(g, ")");
    }
    emit(g, "]");
}

static void gen_expr(Gen *g, int depth) {
    static const char *const ops[] = {"+", "-", "*", "+", "-", "*", "/", "//", "%"};
    unsigned r = below(depth <= 0 ? 4 : 12);
    switch (r) {
    case 0:
    case 1:
        emit(g, "%s", pick_name(g));
        break;
    case 2:
        gen_literal(g);
        break;
    case 3:
        if (below(2) == 0) {
            gen_literal(g);
        } else {
            emit(g, "%s", pick_name(g));
        }
        break;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8: {
        const char *op = ops[below(sizeof ops / sizeof ops[0])];
        emit(g, "(");
        gen_expr(g, depth - 1);
        emit(g, " %s ", op);
        if (g->safe && (op[0] == '/' || op[0] == '%')) {
            emit(g, "(");
            gen_expr(g, depth - 1);
            emit(g, " %% 7 + 1)"); /* in [1, 7] for any non-negative operand, never zero */
        } else {
            gen_expr(g, depth - 1);
        }
        emit(g, ")");
        break;
    }
    case 9:
        emit(g, "(-");
        gen_expr(g, depth - 1);
        emit(g, ")");
        break;
    case 10:
    case 11:
        if (g->arrays > 0) {
            gen_index(g, (int)below((unsigned)g->arrays), depth);
        } else {
            gen_literal(g);
        }
        break;
    default:
        gen_literal(g);
        break;
    }
}

static void gen_cond(Gen *g, int depth) {
    static const char *const compares[] = {"<", "<=", ">", ">=", "==", "!="};
    unsigned r = below(12);
    if (r < 7) {
        gen_expr(g, 1);
        emit(g, " %s ", compares[below(sizeof compares / sizeof compares[0])]);
        gen_expr(g, 1);
    } else if (r < 8) {
        emit(g, "!(");
        gen_cond(g, depth - 1);
        emit(g, ")");
    } else if (r < 11 && depth > 0) {
        gen_cond(g, depth - 1);
        emit(g, below(2) == 0 ? " and " : " or ");
        gen_cond(g, depth - 1);
    } else {
        gen_expr(g, 1);
    }
}


static void gen_object_index(Gen *g, int array) {
    if (below(3) == 0) {
        emit(g, "b%d[%d]", array, (int)below(3));
    } else {
        emit(g, "b%d[int(", array);
        gen_expr(g, 1);
        emit(g, ") %% len(b%d)]", array);
    }
}

/* Statements on the arrays of objects: scalars over objects, objects over objects, arrays into arrays. */
static void gen_object_stmt(Gen *g) {
    int k = (int)below((unsigned)g->objs);
    int j = (int)below((unsigned)g->objs);
    switch (below(10)) {
    case 0:
        gen_object_index(g, k);
        emit(g, " = ");
        gen_expr(g, 2);
        emit(g, "\n");
        break;
    case 1:
        gen_object_index(g, k);
        emit(g, " = \"s%u\"\n", below(100));
        break;
    case 2:
        gen_object_index(g, k);
        emit(g, " = [");
        gen_expr(g, 1);
        emit(g, ", %u]\n", below(10));
        break;
    case 3:
        gen_object_index(g, k);
        emit(g, " = ");
        gen_object_index(g, j);
        emit(g, "\n");
        break;
    case 4:
        gen_object_index(g, k);
        emit(g, " = b%d\n", j); /* an array into an array: a cycle when j == k */
        break;
    case 5:
        emit(g, "println(");
        gen_object_index(g, k);
        emit(g, ")\n");
        break;
    case 6:
        emit(g, "push(b%d, \"p%u\")\n", k, below(100));
        break;
    case 7:
        emit(g, "push(b%d, [%u, %u])\n", k, below(10), below(10));
        break;
    case 8:
        emit(g, "println(b%d, len(b%d))\n", k, k);
        break;
    default:
        emit(g, "b%d = b%d\n", k, j); /* rebinding drops the last reference to an array */
        break;
    }
}

static void gen_stmt(Gen *g, int depth);

static void gen_body(Gen *g, int depth, unsigned count) {
    int saved = g->name_count;
    for (unsigned i = 0; i < count && g->length < sizeof g->text - 2048; i++) {
        gen_stmt(g, depth);
    }
    g->name_count = saved;
}

static void gen_stmt(Gen *g, int depth) {
    if (g->objs > 0 && below(100) < 12) {
        gen_object_stmt(g);
        return;
    }
    unsigned r = below(100);
    if (r < 32) {
        emit(g, "%s = ", pick_name(g));
        if (g->safe) {
            emit(g, "(");
            gen_expr(g, 3);
            emit(g, ") %% 1000\n"); /* bounded: no overflow however long the loops run */
        } else {
            gen_expr(g, 3);
            emit(g, "\n");
        }
    } else if (r < 42 && g->arrays > 0) {
        int array = (int)below((unsigned)g->arrays);
        gen_index(g, array, 2);
        emit(g, " = ");
        gen_expr(g, 2);
        emit(g, "\n");
    } else if (r < 50 && g->arrays > 0) {
        emit(g, "push(a%d, ", (int)below((unsigned)g->arrays));
        gen_expr(g, 2);
        emit(g, ")\n");
    } else if (r < 54 && !g->safe) {
        /* a bool kept in a variable: later arithmetic on it is a run-time type error the compiler cannot see */
        emit(g, "%s = ", pick_name(g));
        gen_cond(g, 1);
        emit(g, "\n");
    } else if (r < 60) {
        emit(g, "println(");
        gen_expr(g, 3);
        if (below(3) == 0) {
            emit(g, ", ");
            gen_expr(g, 2);
        }
        emit(g, ")\n");
    } else if (r < 73 && depth < 4) {
        emit(g, "if ");
        gen_cond(g, 2);
        emit(g, " {\n");
        gen_body(g, depth + 1, 1 + below(3));
        if (below(2) == 0) {
            emit(g, "} else {\n");
            gen_body(g, depth + 1, 1 + below(2));
        }
        emit(g, "}\n");
    } else if (r < 85 && depth < 3) {
        int id = g->serial++;
        emit(g, "let c%d = 0\nwhile c%d < %u {\nc%d = c%d + 1\n", id, id, 1 + below(6), id, id);
        gen_body(g, depth + 1, 1 + below(4));
        emit(g, "}\n");
    } else if (r < 92 && g->name_count < 38) {
        int id = g->serial++;
        emit(g, "let t%d = ", id);
        gen_expr(g, 2);
        emit(g, "\n");
        (void)snprintf(g->names[g->name_count++], sizeof g->names[0], "t%d", id);
    } else if (r < 96 && depth < 4) {
        emit(g, "{\n");
        gen_body(g, depth + 1, 1 + below(3));
        emit(g, "}\n");
    } else {
        emit(g, "println(%s, %s)\n", pick_name(g), pick_name(g));
    }
}

static void gen_program(Gen *g, bool locals, bool safe) {
    memset(g, 0, sizeof *g);
    g->safe = safe;
    int vars = 2 + (int)below(3);
    g->arrays = (int)below(3);
    g->objs = below(3) == 0 ? 0 : 1 + (int)below(2);
    if (locals) {
        emit(g, "{\n");
    }
    for (int i = 0; i < vars; i++) {
        (void)snprintf(g->names[g->name_count++], sizeof g->names[0], "v%d", i);
        emit(g, "let v%d = ", i);
        unsigned r = below(10);
        if (r == 0 && !safe) {
            emit(g, "1e308 * 10");
        } else if (r == 1 && i > 0 && !safe) {
            emit(g, "v%d - v%d", i - 1, i - 1);
        } else {
            gen_literal(g);
        }
        emit(g, "\n");
    }
    for (int i = 0; i < g->arrays; i++) {
        emit(g, "let a%d = [", i);
        unsigned items = 1 + below(5);
        for (unsigned j = 0; j < items; j++) {
            emit(g, j == 0 ? "" : ", ");
            gen_literal(g);
        }
        emit(g, "]\n");
    }
    for (int i = 0; i < g->objs; i++) {
        static const char *const items[] = {"\"a\"", "[1, 2]", "7", "\"xyz\"", "[3]", "true"};
        emit(g, "let b%d = [", i);
        for (unsigned j = 0; j < 3 + below(3); j++) {
            emit(g, j == 0 ? "%s" : ", %s", items[below(sizeof items / sizeof items[0])]);
        }
        emit(g, "]\n");
    }
    gen_body(g, 0, 4 + below(8));
    emit(g, "println(");
    for (int i = 0; i < vars; i++) {
        emit(g, i == 0 ? "v%d" : ", v%d", i);
    }
    emit(g, ")\n");
    if (locals) {
        emit(g, "}\n");
    }
}

static uint64_t pick_fuel(void) {
    switch (below(5)) {
    case 0:
        return 1 + below(150);
    case 1:
        return 100 + below(2000);
    case 2:
        return 1000 + below(40000);
    default:
        return 0; /* unlimited */
    }
}

static void fuzz_programs(unsigned count, uint64_t seed) {
    unsigned compiled = 0;
    unsigned failed_run = 0;
    unsigned deep = 0; /* programs that executed at least 200 instructions */
    uint64_t instructions = 0;
    uint64_t dispatches = 0;
    Gen *g = malloc(sizeof *g);
    LZ_CHECK(g != NULL);
    if (g == NULL) {
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        rng_state = seed + (uint64_t)i * UINT64_C(0x9E3779B97F4A7C15) + 1;
        gen_program(g, below(2) == 0, below(100) < 70);
        uint64_t fuel = pick_fuel();

        Outcome ref;
        Outcome ge;
        run_source(LZ_ENGINE_REFERENCE, g->text, fuel, 0, &ref);
        run_source(LZ_ENGINE_GE, g->text, fuel, 0, &ge);
        if (ref.compile_status != LZ_OK && getenv("FUZZ_SHOW") != NULL) {
            fprintf(stderr, "compile error: %s\n", ref.diagnostic);
        }
        if (ref.compile_status == LZ_OK) {
            compiled++;
            if (ref.run_status != LZ_OK) {
                failed_run++;
            }
            if (ref.instructions >= 200) {
                deep++;
            }
            instructions += ge.stats.instructions;
            dispatches += ge.stats.dispatches;
            if (!ge.stats.translated) {
                fprintf(stderr, "NOT TRANSLATED (program %u)\n%s\n", i, g->text);
                LZ_CHECK(false);
            }
        }
        if (!outcomes_match("random program", g->text, fuel, &ref, &ge)) {
            fprintf(stderr, "  (program %u, seed %llu)\n", i, (unsigned long long)seed);
            LZ_CHECK(false);
        }
        outcome_free(&ref);
        outcome_free(&ge);
    }
    free(g);
    fprintf(stderr,
            "fuzz_engine: %u programs, %u compiled, %u ended in a runtime error or fuel, %u ran 200+ instructions, "
            "%llu instructions in %llu GE dispatches\n",
            count, compiled, failed_run, deep, (unsigned long long)instructions, (unsigned long long)dispatches);
    LZ_CHECK(compiled * 10 >= count * 8); /* the generator must keep producing valid programs */
}

/* ---------------------------------------------------------------- bytecode */

typedef struct RandomIns {
    uint8_t op;
    uint16_t operand;
    int target; /* instruction index for jumps, -1 otherwise */
} RandomIns;

#define MAX_INS 96

typedef struct Plan {
    RandomIns ins[MAX_INS];
    int count;
    int names[8];      /* constant indexes of name strings */
    int name_count;
    int values[10];    /* constant indexes of values */
    int value_count;
    int types;
    int numeric[8];    /* constant indexes of small ints and floats */
    int numeric_count;
    int zero;
    int one;
    int limit;         /* loop bound */
    int index_consts[4];
} Plan;

static void push_ins(Plan *plan, uint8_t op, uint16_t operand) {
    if (plan->count < MAX_INS) {
        plan->ins[plan->count].op = op;
        plan->ins[plan->count].operand = operand;
        plan->ins[plan->count].target = -1;
        plan->count++;
    }
}

/* Operand for a "load" instruction: a constant, a local slot or a global. */
static void push_load(Plan *plan) {
    switch (below(5)) {
    case 0:
    case 1:
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->values[below((unsigned)plan->value_count)]);
        break;
    case 2:
        push_ins(plan, LZ_OP_GET_LOCAL, (uint16_t)below(4));
        break;
    default:
        push_ins(plan, LZ_OP_GET_GLOBAL, (uint16_t)plan->names[below((unsigned)plan->name_count > 4 ? 4 : (unsigned)plan->name_count)]);
        break;
    }
}

static uint8_t random_arith(void) {
    static const uint8_t ops[] = {LZ_OP_ADD, LZ_OP_SUBTRACT, LZ_OP_MULTIPLY, LZ_OP_DIVIDE, LZ_OP_FLOOR_DIVIDE, LZ_OP_MODULO};
    return ops[below(sizeof ops)];
}

static uint8_t random_compare(void) {
    static const uint8_t ops[] = {LZ_OP_EQUAL, LZ_OP_LESS, LZ_OP_GREATER, LZ_OP_LESS_EQUAL, LZ_OP_GREATER_EQUAL};
    return ops[below(sizeof ops)];
}

static void push_store(Plan *plan) {
    if (below(2) == 0) {
        push_ins(plan, LZ_OP_SET_LOCAL, (uint16_t)below(4));
    } else {
        push_ins(plan, LZ_OP_SET_GLOBAL, (uint16_t)plan->names[below(3)]);
    }
}


/*
 * Disciplined mode: the stack stays balanced and loops are bounded, so the
 * program runs deep and the fused instructions execute on their fast paths.
 * Locals: 0, 4, 5 are loop counters, 1 and 3 numbers, 2 an array.
 */
static const uint16_t counter_slots[3] = {0, 4, 5};

static void d_load(Plan *plan) {
    switch (below(7)) {
    case 0:
    case 1:
    case 2:
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->numeric[below((unsigned)plan->numeric_count)]);
        break;
    case 3:
    case 4:
        push_ins(plan, LZ_OP_GET_LOCAL, below(2) == 0 ? 1 : 3);
        break;
    default:
        push_ins(plan, LZ_OP_GET_GLOBAL, (uint16_t)plan->names[below(3)]);
        break;
    }
}

static void d_store(Plan *plan) {
    if (below(3) == 0) {
        push_ins(plan, LZ_OP_SET_GLOBAL, (uint16_t)plan->names[below(3)]);
    } else {
        push_ins(plan, LZ_OP_SET_LOCAL, below(2) == 0 ? 1 : 3);
    }
}

static uint8_t d_arith(void) {
    static const uint8_t ops[] = {LZ_OP_ADD, LZ_OP_ADD, LZ_OP_SUBTRACT, LZ_OP_MULTIPLY, LZ_OP_MULTIPLY,
                                  LZ_OP_DIVIDE, LZ_OP_FLOOR_DIVIDE, LZ_OP_MODULO};
    return ops[below(sizeof ops)];
}

static void d_index(Plan *plan) {
    if (below(3) == 0) {
        push_ins(plan, LZ_OP_GET_LOCAL, 0);
    } else {
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->index_consts[below(4)]);
    }
}

static void d_stmts(Plan *plan, int depth, unsigned count);

static void d_stmt(Plan *plan, int depth) {
    unsigned r = below(100);
    if (plan->count > MAX_INS - 40) {
        return;
    }
    if (r < 28) { /* a = b op c [op d]; store */
        d_load(plan);
        d_load(plan);
        push_ins(plan, d_arith(), 0);
        if (below(3) == 0) {
            d_load(plan);
            push_ins(plan, d_arith(), 0);
        }
        d_store(plan);
    } else if (r < 34) {
        d_load(plan);
        d_load(plan);
        push_ins(plan, d_arith(), 0);
        push_ins(plan, LZ_OP_POP, 0);
    } else if (r < 52 && depth < 3) { /* if / else */
        if (below(3) == 0) {
            d_load(plan);
        }
        d_load(plan);
        d_load(plan);
        push_ins(plan, random_compare(), 0);
        int to_else = plan->count;
        push_ins(plan, LZ_OP_JUMP_IF_FALSE, 0);
        push_ins(plan, LZ_OP_POP, 0);
        d_stmts(plan, depth + 1, 1 + below(2));
        int to_end = plan->count;
        push_ins(plan, LZ_OP_JUMP, 0);
        if (to_else < MAX_INS) {
            plan->ins[to_else].target = plan->count;
        }
        push_ins(plan, LZ_OP_POP, 0);
        d_stmts(plan, depth + 1, below(2));
        if (to_end < MAX_INS) {
            plan->ins[to_end].target = plan->count;
        }
    } else if (r < 62) { /* v = array[i] */
        push_ins(plan, LZ_OP_GET_LOCAL, 2);
        d_index(plan);
        push_ins(plan, LZ_OP_INDEX, 0);
        if (below(3) == 0) {
            push_ins(plan, LZ_OP_POP, 0);
        } else {
            d_store(plan);
        }
    } else if (r < 70) { /* array[i] = v */
        push_ins(plan, LZ_OP_GET_LOCAL, 2);
        d_index(plan);
        d_load(plan);
        push_ins(plan, LZ_OP_SET_INDEX, 0);
    } else if (r < 74) { /* push(array, v) */
        push_ins(plan, LZ_OP_GET_LOCAL, 2);
        d_load(plan);
        push_ins(plan, LZ_OP_ARRAY_PUSH, 0);
        push_ins(plan, LZ_OP_POP, 0);
    } else if (r < 80) { /* println(v) */
        push_ins(plan, LZ_OP_GET_GLOBAL, (uint16_t)plan->names[4]);
        d_load(plan);
        push_ins(plan, LZ_OP_CALL, 1);
        push_ins(plan, LZ_OP_POP, 0);
    } else if (r < 92 && depth < 3) { /* bounded loop */
        uint16_t counter = counter_slots[depth];
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->zero);
        push_ins(plan, LZ_OP_SET_LOCAL, counter);
        int head = plan->count;
        push_ins(plan, LZ_OP_GET_LOCAL, counter);
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->limit);
        push_ins(plan, LZ_OP_LESS, 0);
        int to_exit = plan->count;
        push_ins(plan, LZ_OP_JUMP_IF_FALSE, 0);
        push_ins(plan, LZ_OP_POP, 0);
        d_stmts(plan, depth + 1, 1 + below(3));
        push_ins(plan, LZ_OP_GET_LOCAL, counter);
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->one);
        push_ins(plan, LZ_OP_ADD, 0);
        push_ins(plan, LZ_OP_SET_LOCAL, counter);
        int back = plan->count;
        push_ins(plan, LZ_OP_LOOP, 0);
        if (to_exit < MAX_INS) {
            plan->ins[to_exit].target = plan->count;
        }
        if (back < MAX_INS) {
            plan->ins[back].target = head;
        }
        push_ins(plan, LZ_OP_POP, 0);
    } else if (r < 96) {
        d_load(plan);
        push_ins(plan, below(2) == 0 ? LZ_OP_NOT : LZ_OP_NEGATE, 0);
        push_ins(plan, LZ_OP_POP, 0);
    } else {
        d_load(plan);
        push_ins(plan, LZ_OP_POP, 0);
    }
}

static void d_stmts(Plan *plan, int depth, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        d_stmt(plan, depth);
    }
}

static void plan_disciplined(Plan *plan) {
    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->zero);                                        /* 0: counter */
    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->numeric[below((unsigned)plan->numeric_count)]); /* 1: number */
    push_ins(plan, LZ_OP_ARRAY_NEW, 0);                                                          /* 2: array */
    for (unsigned items = 3 + below(3); items > 0; items--) {
        push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->numeric[below((unsigned)plan->numeric_count)]);
        push_ins(plan, LZ_OP_ARRAY_PUSH, 0);
    }
    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->numeric[below((unsigned)plan->numeric_count)]); /* 3: number */
    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->zero);                                        /* 4: counter */
    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->zero);                                        /* 5: counter */
    d_stmts(plan, 0, 3 + below(6));
    push_ins(plan, LZ_OP_RETURN, 0);
}

static void plan_wild(Plan *plan) {
    int target_pop = -1;
    while (plan->count < 8 + (int)below(70)) {
        unsigned r = below(100);
        if (r < 14) { /* [load] [load] ARITH [store] */
            push_load(plan);
            push_load(plan);
            push_ins(plan, random_arith(), 0);
            if (below(2) == 0) {
                push_store(plan);
            }
        } else if (r < 22) { /* [load] ARITH: left operand from whatever is on the stack */
            push_load(plan);
            push_ins(plan, random_arith(), 0);
        } else if (r < 32) { /* [load] [load] CMP JUMP_IF_FALSE POP ... POP */
            if (below(2) == 0) {
                push_load(plan);
            }
            push_load(plan);
            push_ins(plan, random_compare(), 0);
            int jump = plan->count;
            push_ins(plan, LZ_OP_JUMP_IF_FALSE, 0);
            push_ins(plan, LZ_OP_POP, 0);
            for (unsigned filler = below(3); filler > 0; filler--) {
                push_load(plan);
                push_ins(plan, LZ_OP_POP, 0);
            }
            if (jump < MAX_INS && plan->count < MAX_INS) {
                plan->ins[jump].target = plan->count; /* the else path's POP */
                push_ins(plan, LZ_OP_POP, 0);
            }
        } else if (r < 38) { /* [load] [load] INDEX */
            if (below(3) == 0) {
                push_ins(plan, LZ_OP_ARRAY_NEW, 0);
                for (unsigned items = below(4); items > 0; items--) {
                    push_ins(plan, LZ_OP_CONSTANT, (uint16_t)plan->values[below((unsigned)plan->value_count)]);
                    push_ins(plan, LZ_OP_ARRAY_PUSH, 0);
                }
            } else {
                push_load(plan);
            }
            push_load(plan);
            push_ins(plan, LZ_OP_INDEX, 0);
        } else if (r < 42) {
            push_ins(plan, LZ_OP_ARRAY_NEW, 0);
            for (unsigned items = below(4); items > 0; items--) {
                push_load(plan);
                push_ins(plan, LZ_OP_ARRAY_PUSH, 0);
            }
            if (below(2) == 0) {
                push_load(plan);
                push_load(plan);
                push_ins(plan, LZ_OP_SET_INDEX, 0);
            }
        } else if (r < 46) {
            push_ins(plan, LZ_OP_ARRAY_NEW, 0);
            push_load(plan);
            push_load(plan);
            push_load(plan);
            push_ins(plan, LZ_OP_SLICE, 0);
        } else if (r < 54) {
            push_load(plan);
        } else if (r < 58) {
            push_ins(plan, LZ_OP_POP, 0);
        } else if (r < 62) {
            push_ins(plan, random_compare(), 0);
        } else if (r < 66) {
            push_ins(plan, random_arith(), 0);
        } else if (r < 68) {
            push_ins(plan, LZ_OP_NOT, 0);
        } else if (r < 70) {
            push_ins(plan, LZ_OP_NEGATE, 0);
        } else if (r < 72) {
            push_ins(plan, (uint8_t)(LZ_OP_NIL + below(3)), 0);
        } else if (r < 76) {
            push_store(plan);
        } else if (r < 80) { /* forward jump */
            int at = plan->count;
            push_ins(plan, below(3) == 0 ? LZ_OP_JUMP : (below(2) == 0 ? LZ_OP_JUMP_IF_FALSE : LZ_OP_JUMP_IF_TRUE), 0);
            if (at < MAX_INS) {
                plan->ins[at].target = -2; /* chosen once the length is known: forward */
            }
        } else if (r < 83) { /* backward jump */
            int at = plan->count;
            push_ins(plan, LZ_OP_LOOP, 0);
            if (at < MAX_INS) {
                plan->ins[at].target = -3; /* backward */
            }
        } else if (r < 86) {
            push_ins(plan, LZ_OP_CALL, (uint16_t)below(4));
        } else if (r < 88) {
            if (plan->types > 0) {
                push_ins(plan, LZ_OP_CHECK_TYPE, (uint16_t)below((unsigned)plan->types + 1));
            }
        } else if (r < 90) {
            push_ins(plan, LZ_OP_DEFINE_GLOBAL, (uint16_t)plan->names[below((unsigned)plan->name_count)]);
        } else if (r < 91) {
            push_ins(plan, LZ_OP_RETURN, 0);
        } else if (r < 93) {
            push_ins(plan, LZ_OP_GET_LOCAL, (uint16_t)below(600)); /* usually out of range */
        } else {
            push_ins(plan, LZ_OP_CONSTANT, (uint16_t)below(40)); /* sometimes out of range */
        }
    }
    (void)target_pop;
    if (below(10) != 0) {
        push_ins(plan, LZ_OP_RETURN, 0);
    }
}

static void plan_program(Plan *plan) {
    if (below(100) < 60) {
        plan_disciplined(plan);
    } else {
        plan_wild(plan);
    }
}

static void build_random_chunk(LzVM *vm, LzChunk *chunk, void *context) {
    uint64_t seed = *(const uint64_t *)context;
    rng_state = seed;
    Plan *plan = malloc(sizeof *plan);
    LZ_CHECK(plan != NULL);
    if (plan == NULL) {
        return;
    }
    memset(plan, 0, sizeof *plan);

    /* Constants: values first, then names (x, y, z first so most globals exist). */
    static const int64_t ints[] = {0, 1, -1, 2, 3, 7, 100, 9223372036854775807, -9223372036854775807 - 1, 4611686018427387904, 9007199254740993};
    static const double floats[] = {0.0, -0.0, 0.5, 1.5, 2.0, 1e308, 9007199254740992.0, -3.25};
    unsigned value_count = 5 + below(6);
    for (unsigned i = 0; i < value_count; i++) {
        LzValue value;
        switch (below(8)) {
        case 0:
            value = lz_value_float(floats[below(sizeof floats / sizeof floats[0])]);
            break;
        case 1:
            value = lz_value_nil();
            break;
        case 2:
            value = lz_value_bool(below(2) == 0);
            break;
        default:
            value = lz_value_int(ints[below(sizeof ints / sizeof ints[0])]);
            break;
        }
        plan->values[plan->value_count++] = bc_constant(chunk, value);
    }
    if (below(6) == 0) { /* a string constant to meet arithmetic and indexing */
        LzString *text = lz_string_new(&vm->heap, "str", 3);
        if (text != NULL) {
            plan->values[plan->value_count - 1] = bc_constant(chunk, lz_value_object(&text->base));
            lz_object_release(&vm->heap, &text->base);
        }
    }
    static const char *const names[] = {"x", "y", "z", "nope", "println", "len", "w", "x"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        plan->names[plan->name_count++] = bc_name(vm, chunk, names[i]);
    }
    static const int64_t small[] = {1, 2, 3, 5, 7, 11};
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) {
        plan->numeric[plan->numeric_count++] = bc_constant(chunk, lz_value_int(small[i]));
    }
    plan->numeric[plan->numeric_count++] = bc_constant(chunk, lz_value_float(1.5));
    plan->numeric[plan->numeric_count++] = bc_constant(chunk, lz_value_float(-0.5));
    plan->zero = bc_constant(chunk, lz_value_int(0));
    plan->one = bc_constant(chunk, lz_value_int(1));
    plan->limit = bc_constant(chunk, lz_value_int(2 + (int64_t)below(5)));
    static const int64_t indexes[] = {0, 1, 2, -1};
    for (size_t i = 0; i < 4; i++) {
        plan->index_consts[i] = bc_constant(chunk, lz_value_int(indexes[i]));
    }
    unsigned type_count = below(3);
    for (unsigned i = 0; i < type_count; i++) {
        LzType type = lz_type_of_kinds((uint16_t)below(128));
        uint16_t index = 0;
        if (lz_chunk_add_type(chunk, &type, &index)) {
            plan->types++;
        }
    }

    plan_program(plan);

    /* Byte offsets, then resolve jump targets to distances. */
    size_t offsets[MAX_INS + 1];
    size_t at = 0;
    for (int i = 0; i < plan->count; i++) {
        offsets[i] = at;
        at += lz_opcode_length(plan->ins[i].op);
    }
    offsets[plan->count] = at;

    for (int i = 0; i < plan->count; i++) {
        RandomIns *ins = &plan->ins[i];
        if (ins->op != LZ_OP_JUMP && ins->op != LZ_OP_JUMP_IF_FALSE && ins->op != LZ_OP_JUMP_IF_TRUE &&
            ins->op != LZ_OP_LOOP) {
            continue;
        }
        size_t after = offsets[i] + 3;
        if (ins->op == LZ_OP_LOOP) {
            int to = ins->target >= 0 ? ins->target : (int)below((unsigned)i + 1);
            ins->operand = (uint16_t)(after - offsets[to]);
        } else {
            int to = ins->target >= 0 ? ins->target : i + 1 + (int)below((unsigned)(plan->count - i));
            if (to > plan->count) {
                to = plan->count;
            }
            ins->operand = (uint16_t)(offsets[to] - after);
        }
        if (below(40) == 0) {
            ins->operand = (uint16_t)(ins->operand + 1 + below(2)); /* now lands inside an instruction */
        }
    }

    for (int i = 0; i < plan->count; i++) {
        size_t length = lz_opcode_length(plan->ins[i].op);
        if (length == 3) {
            bc_u16(chunk, (LzOpCode)plan->ins[i].op, plan->ins[i].operand);
        } else if (length == 2) {
            bc_u8(chunk, (LzOpCode)plan->ins[i].op, (uint8_t)plan->ins[i].operand);
        } else {
            bc_op(chunk, (LzOpCode)plan->ins[i].op);
        }
    }
    free(plan);
}

static void fuzz_chunks(unsigned count, uint64_t seed) {
    unsigned translated = 0;
    unsigned errors = 0;
    unsigned deep = 0;
    uint64_t instructions = 0;
    uint64_t dispatches = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t chunk_seed = seed + (uint64_t)i * UINT64_C(0xD1B54A32D192ED03) + 7;
        rng_state = chunk_seed ^ UINT64_C(0xABCDEF);
        uint64_t fuel = below(4) == 0 ? 60000 : 20 + below(2500); /* a loop must not run forever */

        Outcome ref;
        Outcome ge;
        run_chunk(LZ_ENGINE_REFERENCE, build_random_chunk, &chunk_seed, fuel, &ref);
        run_chunk(LZ_ENGINE_GE, build_random_chunk, &chunk_seed, fuel, &ge);
        if (ge.stats.translated) {
            translated++;
        }
        if (ref.run_status != LZ_OK) {
            errors++;
        }
        if (ref.instructions >= 50) {
            deep++;
        }
        instructions += ge.stats.instructions;
        dispatches += ge.stats.dispatches;
        if (!outcomes_match("random bytecode", "(bytecode)", fuel, &ref, &ge)) {
            fprintf(stderr, "  (chunk %u, seed %llu, fuel %llu)\n", i, (unsigned long long)chunk_seed,
                    (unsigned long long)fuel);
            LZ_CHECK(false);
        }
        outcome_free(&ref);
        outcome_free(&ge);
    }
    fprintf(stderr,
            "fuzz_engine: %u chunks, %u translated, %u ended in an error or fuel, %u ran 50+ instructions, "
            "%llu instructions in %llu GE dispatches\n",
            count, translated, errors, deep, (unsigned long long)instructions, (unsigned long long)dispatches);
    LZ_CHECK(translated * 2 >= count); /* most chunks must reach GE rather than fall back */
}

int main(int argc, char **argv) {
    unsigned programs = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 3000;
    unsigned chunks = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 30000;
    uint64_t seed = argc > 3 ? strtoull(argv[3], NULL, 10) : 20260101;
    fuzz_programs(programs, seed);
    fuzz_chunks(chunks, seed);
    return LZ_TEST_RESULT();
}
