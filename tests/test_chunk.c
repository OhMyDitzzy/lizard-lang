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
    LZ_CHECK(chunk.lines[0] == 3 && chunk.lines[2] == 3 && chunk.lines[3] == 4);

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

static void test_opcode_names(void) {
    LZ_CHECK(strcmp(lz_opcode_name(LZ_OP_CALL), "OP_CALL") == 0);
    LZ_CHECK(strcmp(lz_opcode_name(200), "OP_UNKNOWN") == 0);
}

int main(void) {
    test_write_and_operands();
    test_constants_hold_references();
    test_constant_limit();
    test_opcode_names();
    return LZ_TEST_RESULT();
}
