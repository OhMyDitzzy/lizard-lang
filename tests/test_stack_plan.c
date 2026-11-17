#include <stdlib.h>
#include <string.h>

#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/stack_plan.h"
#include "lizard/vm.h"
#include "test_util.h"

static void check_bytes(const uint8_t *bytes, size_t count, size_t parameters,
                        LzStatus expected, size_t values, size_t handlers) {
    LzChunk chunk;
    lz_chunk_init(&chunk);
    for (size_t i = 0; i < count; i++) LZ_CHECK(lz_chunk_write(&chunk, bytes[i], 0));
    LzStackPlan plan = {123, 456};
    LZ_CHECK(lz_chunk_stack_plan(&chunk, parameters, &plan) == expected);
    if (expected == LZ_OK) {
        LZ_CHECK(plan.values == values);
        LZ_CHECK(plan.handlers == handlers);
    } else {
        LZ_CHECK(plan.values == 123 && plan.handlers == 456);
    }
    lz_chunk_discard(&chunk);
}

static void test_control_flow(void) {
    const uint8_t conditional[] = {
        LZ_OP_TRUE, LZ_OP_JUMP_IF_FALSE, 0, 5,
        LZ_OP_POP, LZ_OP_NIL, LZ_OP_JUMP, 0, 2,
        LZ_OP_POP, LZ_OP_FALSE, LZ_OP_RETURN
    };
    check_bytes(conditional, sizeof conditional, 0, LZ_OK, 1, 0);
    const uint8_t loop[] = {LZ_OP_NIL, LZ_OP_POP, LZ_OP_LOOP, 0, 5};
    check_bytes(loop, sizeof loop, 0, LZ_OK, 1, 0);
    const uint8_t bad_loop[] = {LZ_OP_NIL, LZ_OP_LOOP, 0, 4};
    check_bytes(bad_loop, sizeof bad_loop, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t catch_entry[] = {
        LZ_OP_TRY_BEGIN, 0, 4,
        LZ_OP_TRY_END, LZ_OP_JUMP, 0, 2,
        LZ_OP_DUP, LZ_OP_POP, LZ_OP_RETURN
    };
    /* A catch can need more slots than any successful path. The two return paths do not merge here. */
    const uint8_t catching[] = {
        LZ_OP_TRY_BEGIN, 0, 2, LZ_OP_TRY_END, LZ_OP_RETURN,
        LZ_OP_DUP, LZ_OP_POP, LZ_OP_RETURN
    };
    check_bytes(catching, sizeof catching, 3, LZ_OK, 5, 1);
    check_bytes(catch_entry, sizeof catch_entry, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t nested[] = {
        LZ_OP_TRY_BEGIN, 0, 7, LZ_OP_TRY_BEGIN, 0, 2,
        LZ_OP_TRY_END, LZ_OP_RETURN, LZ_OP_POP, LZ_OP_RETURN,
        LZ_OP_RETURN
    };
    check_bytes(nested, sizeof nested, 0, LZ_OK, 1, 2);
    const uint8_t operand_target[] = {LZ_OP_JUMP, 0, 1, LZ_OP_CONSTANT, 0, 0, LZ_OP_RETURN};
    check_bytes(operand_target, sizeof operand_target, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t truncated[] = {LZ_OP_CONSTANT, 0};
    check_bytes(truncated, sizeof truncated, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t underflow[] = {LZ_OP_ADD, LZ_OP_RETURN};
    check_bytes(underflow, sizeof underflow, 1, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t fallthrough[] = {LZ_OP_NIL};
    check_bytes(fallthrough, sizeof fallthrough, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t unknown[] = {255};
    check_bytes(unknown, sizeof unknown, 0, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t overflow[] = {LZ_OP_NIL, LZ_OP_RETURN};
    check_bytes(overflow, sizeof overflow, LZ_STACK_MAX, LZ_ERR_STACK_OVERFLOW, 0, 0);
    const uint8_t finish[] = {LZ_OP_RETURN};
    check_bytes(finish, sizeof finish, LZ_STACK_MAX, LZ_OK, LZ_STACK_MAX, 0);
}

static void test_temporary_peaks(void) {
    const uint8_t pair[] = {LZ_OP_ITER_PAIR, LZ_OP_POP, LZ_OP_POP, LZ_OP_RETURN};
    check_bytes(pair, sizeof pair, 3, LZ_OK, 4, 0);
    const uint8_t keep[] = {LZ_OP_GET_FIELD_KEEP, 0, 0, LZ_OP_POP, LZ_OP_RETURN};
    check_bytes(keep, sizeof keep, 1, LZ_OK, 2, 0);
    const uint8_t expression[] = {
        LZ_OP_NIL, LZ_OP_NIL, LZ_OP_NIL, LZ_OP_CALL, 1,
        LZ_OP_ADD, LZ_OP_RETURN
    };
    check_bytes(expression, sizeof expression, 2, LZ_OK, 5, 0);
    const uint8_t spread[] = {
        LZ_OP_NIL, LZ_OP_ARRAY_NEW, LZ_OP_NIL, LZ_OP_CALL_EX, 1, 0, 0,
        LZ_OP_DUP, LZ_OP_RETURN
    };
    check_bytes(spread, sizeof spread, 2, LZ_OK, 5, 0);
    const uint8_t defaults[] = {
        LZ_OP_JUMP_IF_SUPPLIED, 0, 0, 0, 5,
        LZ_OP_NIL, LZ_OP_DUP, LZ_OP_POP, LZ_OP_POP, LZ_OP_NIL,
        LZ_OP_RETURN
    };
    check_bytes(defaults, sizeof defaults, 1, LZ_ERR_RUNTIME, 0, 0);
    const uint8_t valid_defaults[] = {
        LZ_OP_JUMP_IF_SUPPLIED, 0, 0, 0, 4,
        LZ_OP_NIL, LZ_OP_SET_LOCAL, 0, 0, LZ_OP_RETURN
    };
    check_bytes(valid_defaults, sizeof valid_defaults, 1, LZ_OK, 2, 0);
}

static void test_opcode_coverage(void) {
    for (unsigned op = 0; op <= LZ_OP_IMPORT; op++) {
        if (op == LZ_OP_JUMP || op == LZ_OP_LOOP || op == LZ_OP_JUMP_IF_FALSE ||
            op == LZ_OP_JUMP_IF_TRUE || op == LZ_OP_JUMP_IF_SUPPLIED ||
            op == LZ_OP_TRY_BEGIN || op == LZ_OP_TRY_END) continue;
        uint8_t code[9] = {0};
        code[0] = (uint8_t)op;
        size_t length = lz_opcode_length((uint8_t)op);
        LZ_CHECK(length > 0 && length < sizeof code);
        code[length] = LZ_OP_RETURN;
        LzChunk chunk;
        lz_chunk_init(&chunk);
        for (size_t i = 0; i <= length; i++) LZ_CHECK(lz_chunk_write(&chunk, code[i], 0));
        LzStackPlan plan;
        LzStatus status = lz_chunk_stack_plan(&chunk, 8, &plan);
        if (status != LZ_OK) fprintf(stderr, "unhandled stack effect: %s\n", lz_opcode_name((uint8_t)op));
        LZ_CHECK(status == LZ_OK);
        lz_chunk_discard(&chunk);
    }
}

static void test_compiled(const char *text, bool execute) {
    LzVM vm;
    LzBuffer output = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&output), lz_writer_from_buffer(&output));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LzSource source;
    lz_source_from_string(&source, "stack-plan.lz", text);
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
    if (status != LZ_OK) {
        for (size_t i = 0; i < diagnostics.count; i++) fprintf(stderr, "%s\n", diagnostics.items[i].message);
    }
    LZ_CHECK(status == LZ_OK);
    if (status == LZ_OK) {
        LzStackPlan plan;
        LZ_CHECK(lz_chunk_stack_plan(&chunk, 0, &plan) == LZ_OK);
        for (LzObject *object = vm.heap.head; object != NULL; object = object->next) {
            if (object->type != LZ_OBJ_FUNCTION) continue;
            LzFunction *function = (LzFunction *)object;
            status = lz_chunk_stack_plan(&function->chunk, lz_function_slots(function), &plan);
            if (status != LZ_OK) fprintf(stderr, "stack plan failed for %s: %d\n", lz_function_display_name(function), status);
            LZ_CHECK(status == LZ_OK);
        }
        if (execute) LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    }
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0 && vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&output);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            LzSource source;
            char error[256];
            if (!lz_source_read_file(&source, argv[i], error, sizeof error)) {
                fprintf(stderr, "%s\n", error);
                return 1;
            }
            fprintf(stderr, "checking %s\n", argv[i]);
            test_compiled(source.text, false);
            lz_source_free(&source);
        }
        return LZ_TEST_RESULT();
    }
    test_control_flow();
    test_temporary_peaks();
    test_opcode_coverage();
    test_compiled("fn f(n = 3, *rest) { if n == 0 { return rest } return 1 + len(f(n - 1, *rest)) }\n"
                  "f(0, 1, 2)\n", true);
    test_compiled("fn f(n) { try { if n { throw Error(\"x\") } return 1 } catch e { return 2 } }\n"
                  "f(true)\n", true);
    test_compiled("fn f() { let result = 0\n for x in [1, 2, 3] { result += x }\n"
                  "while result < 10 { result += 1 }\n return fn() => result }\n f()()\n", true);
    test_compiled("class Box { pub value: int = 3\n pub fn read(self) { return self.value } }\n"
                  "let b = Box()\n b.value += 2\n b.read()\n", true);
    return LZ_TEST_RESULT();
}
