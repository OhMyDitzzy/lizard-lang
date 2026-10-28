#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lizard/array.h"
#include "lizard/builtin.h"
#include "test_util.h"

static uint64_t state = 0x9E3779B97F4A7C15ull;

static uint64_t next_random(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint32_t below(uint32_t limit) {
    return (uint32_t)(next_random() % limit);
}

static bool discard(void *ctx, const char *data, size_t length) {
    (void)ctx;
    (void)data;
    (void)length;
    return true;
}

static void add_random_constant(LzVM *vm, LzChunk *chunk) {
    static const int64_t integers[] = {0, 1, -1, 2, 1000, INT64_MAX, INT64_MIN};
    static const double floats[] = {0.0, -0.0, 1.5, -2.5, NAN, INFINITY, 1e300};
    static const char *const names[] = {"println", "print", "x", "y", "missing"};
    static const char *const natives[] = {"push", "pop", "insert", "remove", "sort", "join", "sum", "range", "len", "contains"};

    LzValue value = lz_value_nil();
    LzString *created = NULL;

    switch (below(8)) {
    case 0:
        value = lz_value_int(integers[below(7)]);
        break;
    case 1:
        value = lz_value_float(floats[below(7)]);
        break;
    case 2:
        value = below(2) == 0 ? lz_value_nil() : lz_value_bool(below(2) == 0);
        break;
    case 3: {
        const char *name = names[below(5)];
        value = lz_value_object(&lz_vm_intern(vm, name, strlen(name))->base);
        break;
    }
    case 4:
        created = lz_string_new(&vm->heap, "abc", 3);
        value = lz_value_object(&created->base);
        break;
    case 5:
        created = lz_string_new(&vm->heap, "", 0);
        value = lz_value_object(&created->base);
        break;
    case 6: {
        const char *native = below(2) == 0 ? "println" : natives[below(10)];
        LzString *println = lz_vm_intern(vm, native, strlen(native));
        const LzValue *builtin = lz_vm_lookup(vm, println);
        value = builtin != NULL ? *builtin : lz_value_nil();
        break;
    }
    default:
        value = lz_value_int((int64_t)below(100));
        break;
    }

    uint16_t index;
    (void)lz_chunk_add_constant(chunk, value, &index);
    if (created != NULL) {
        lz_object_release(&vm->heap, &created->base);
    }
}

static void run_one(void) {
    LzWriter sink = {discard, NULL, NULL};
    LzVM vm;
    lz_vm_init(&vm, sink, sink);
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    lz_vm_set_heap_limit(&vm, 1u << 20);

    LzChunk chunk;
    lz_chunk_init(&chunk);

    uint32_t constants = below(8);
    for (uint32_t i = 0; i < constants; i++) {
        add_random_constant(&vm, &chunk);
    }

    uint32_t type_count = below(4);
    for (uint32_t i = 0; i < type_count; i++) {
        LzType type = lz_type_of_kinds((uint16_t)below(128));
        if (below(2) == 0) {
            LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = (int64_t)below(4)};
            (void)lz_type_add_literal(&type, &literal);
        }
        if (below(3) == 0) {
            LzLiteral literal = {.kind = LZ_VAL_OBJECT,
                                 .as.string = lz_vm_intern(&vm, "abc", 3)};
            (void)lz_type_add_literal(&type, &literal);
        }
        uint16_t index;
        (void)lz_chunk_add_type(&chunk, &type, &index);
    }

    uint32_t length = below(96);
    for (uint32_t i = 0; i < length; i++) {
        uint8_t byte = below(2) == 0 ? (uint8_t)below(LZ_OP_RETURN + 1) : (uint8_t)below(256);
        (void)lz_chunk_write(&chunk, byte, i / 4 + 1);
    }

    lz_vm_set_fuel(&vm, 5000);
    LzStatus status = lz_vm_run(&vm, &chunk);
    LZ_CHECK(status == LZ_OK || status == LZ_ERR_RUNTIME || status == LZ_ERR_ARITY ||
             status == LZ_ERR_MEMORY || status == LZ_ERR_IO || status == LZ_ERR_STACK_OVERFLOW ||
             status == LZ_ERR_FUEL || status == LZ_ERR_HEAP_LIMIT || status == LZ_ERR_TYPE);
    LZ_CHECK(vm.stack_count == 0);

    lz_chunk_free(&vm.heap, &chunk);
    LZ_CHECK(lz_array_check_flags(&vm.heap) == 0); /* whatever the random program did */
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    LZ_CHECK(vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
}

int main(int argc, char **argv) {
    unsigned long iterations = argc > 1 ? strtoul(argv[1], NULL, 10) : 20000;
    for (unsigned long i = 0; i < iterations && lz_test_failures == 0; i++) {
        run_one();
    }
    return LZ_TEST_RESULT();
}
