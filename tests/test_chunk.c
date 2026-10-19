#include <string.h>

#include "lizard/chunk.h"
#include "test_util.h"

static void test_write_and_operands(void) {
    LzChunk chunk;
    lz_chunk_init(&chunk);

    LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_CONSTANT, 3));
    LZ_CHECK(lz_chunk_write_u16(&chunk, 0x0102, 3));
    LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_RETURN, 4));

    LZ_CHECK(chunk.count == 4);
    LZ_CHECK(chunk.code[1] == 0x01 && chunk.code[2] == 0x02);
    LZ_CHECK(chunk.offsets[0] == 3 && chunk.offsets[2] == 3 && chunk.offsets[3] == 4);

    for (int i = 0; i < 1000; i++) {
        LZ_CHECK(lz_chunk_write(&chunk, LZ_OP_NIL, 5));
    }
    LZ_CHECK(chunk.count == 1004);
    LZ_CHECK(chunk.code[3] == LZ_OP_RETURN);

    LzHeap heap;
    lz_heap_init(&heap);
    lz_chunk_free(&heap, &chunk);
    LZ_CHECK(chunk.code == NULL && chunk.count == 0);
    lz_heap_destroy(&heap);
}

static void test_constants_hold_references(void) {
    LzHeap heap;
    lz_heap_init(&heap);
    LzChunk chunk;
    lz_chunk_init(&chunk);

    LzString *text = lz_string_new(&heap, "hi", 2);
    uint16_t first = 99;
    uint16_t second = 99;
    LZ_CHECK(lz_chunk_add_constant(&chunk, lz_value_object(&text->base), &first));
    LZ_CHECK(lz_chunk_add_constant(&chunk, lz_value_int(5), &second));
    LZ_CHECK(first == 0 && second == 1);
    LZ_CHECK(text->base.refcount == 2);

    lz_object_release(&heap, &text->base);
    LZ_CHECK(heap.live_objects == 1);

    lz_chunk_free(&heap, &chunk);
    LZ_CHECK(heap.live_objects == 0);
    lz_heap_destroy(&heap);
}

static void test_constant_limit(void) {
    LzHeap heap;
    lz_heap_init(&heap);
    LzChunk chunk;
    lz_chunk_init(&chunk);

    uint16_t index = 0;
    for (int i = 0; i < LZ_CONSTANTS_MAX; i++) {
        LZ_CHECK(lz_chunk_add_constant(&chunk, lz_value_int(i), &index));
    }
    LZ_CHECK(index == LZ_CONSTANTS_MAX - 1);
    LZ_CHECK(!lz_chunk_add_constant(&chunk, lz_value_int(0), &index));

    lz_chunk_free(&heap, &chunk);
    lz_heap_destroy(&heap);
}

static void test_type_table(void) {
    LzChunk chunk;
    lz_chunk_init(&chunk);
    LzHeap heap;
    lz_heap_init(&heap);

    LzType integer = lz_type_of_kinds(LZ_KIND_INT);
    LzType string = lz_type_of_kinds(LZ_KIND_STRING);
    uint16_t first = 99;
    uint16_t second = 99;
    uint16_t again = 99;
    LZ_CHECK(lz_chunk_add_type(&chunk, &integer, &first));
    LZ_CHECK(lz_chunk_add_type(&chunk, &string, &second));
    LZ_CHECK(lz_chunk_add_type(&chunk, &integer, &again));
    LZ_CHECK(first == 0 && second == 1 && again == 0);
    LZ_CHECK(chunk.type_count == 2);

    LzType many = lz_type_of_kinds(0);
    for (int i = 0; i < 9; i++) {
        LzType single = lz_type_of_kinds(0);
        LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = i};
        LZ_CHECK(lz_type_add_literal(&single, &literal));
        uint16_t index;
        LZ_CHECK(lz_chunk_add_type(&chunk, &single, &index));
    }
    LZ_CHECK(chunk.type_count == 11);
    (void)many;

    /* Tens of thousands of distinct types must stay fast, and identical ones must still merge. */
    for (int i = 0; i < 30000; i++) {
        LzType single = lz_type_of_kinds(0);
        LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = 1000 + i};
        LZ_CHECK(lz_type_add_literal(&single, &literal));
        uint16_t index;
        LZ_CHECK(lz_chunk_add_type(&chunk, &single, &index));
        LZ_CHECK(index == 11 + i);
    }
    LZ_CHECK(chunk.type_count == 30011);
    for (int i = 0; i < 30000; i += 997) {
        LzType single = lz_type_of_kinds(0);
        LzLiteral literal = {.kind = LZ_VAL_INT, .as.integer = 1000 + i};
        LZ_CHECK(lz_type_add_literal(&single, &literal));
        uint16_t index;
        LZ_CHECK(lz_chunk_add_type(&chunk, &single, &index));
        LZ_CHECK(index == 11 + i);
    }
    LZ_CHECK(chunk.type_count == 30011);

    /* A union whose literals come in another order is the same type. */
    LzType forward = lz_type_of_kinds(0);
    LzType backward = lz_type_of_kinds(0);
    LzLiteral one = {.kind = LZ_VAL_INT, .as.integer = -1};
    LzLiteral two = {.kind = LZ_VAL_INT, .as.integer = -2};
    LZ_CHECK(lz_type_add_literal(&forward, &one) && lz_type_add_literal(&forward, &two));
    LZ_CHECK(lz_type_add_literal(&backward, &two) && lz_type_add_literal(&backward, &one));
    LZ_CHECK(lz_type_hash(&forward) == lz_type_hash(&backward));
    uint16_t first_index;
    uint16_t second_index;
    LZ_CHECK(lz_chunk_add_type(&chunk, &forward, &first_index));
    LZ_CHECK(lz_chunk_add_type(&chunk, &backward, &second_index));
    LZ_CHECK(first_index == second_index);

    lz_chunk_free(&heap, &chunk);
    LZ_CHECK(chunk.types == NULL && chunk.type_count == 0);
    lz_heap_destroy(&heap);
}

static void test_opcode_names(void) {
    LZ_CHECK(lz_opcode_length(LZ_OP_CHECK_TYPE) == 3);
    LZ_CHECK(strcmp(lz_opcode_name(LZ_OP_CHECK_TYPE), "OP_CHECK_TYPE") == 0);
    LZ_CHECK(strcmp(lz_opcode_name(LZ_OP_CALL), "OP_CALL") == 0);
    LZ_CHECK(strcmp(lz_opcode_name(200), "OP_UNKNOWN") == 0);
}

int main(void) {
    test_write_and_operands();
    test_constants_hold_references();
    test_constant_limit();
    test_type_table();
    test_opcode_names();
    return LZ_TEST_RESULT();
}
