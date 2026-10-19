#include <string.h>

#include "chunk_helpers.h"
#include "lizard/builtin.h"
#include "lizard/debug.h"
#include "test_util.h"

int main(void) {
    LzBuffer out = {0};
    LzVM vm;
    lz_vm_init(&vm, lz_writer_from_buffer(&out), lz_writer_from_buffer(&out));

    LzChunk chunk;
    lz_chunk_init(&chunk);
    emit_u16(&chunk, LZ_OP_GET_GLOBAL, constant_name(&vm, &chunk, "println"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_text(&vm, &chunk, "halo"), 1);
    emit_u16(&chunk, LZ_OP_CONSTANT, constant_value(&chunk, lz_value_int(42)), 2);
    emit_u8(&chunk, LZ_OP_CALL, 2, 2);
    emit(&chunk, LZ_OP_RETURN, 3);

    LzWriter writer = lz_writer_from_buffer(&out);
    lz_disassemble_chunk(&chunk, "main", NULL, &writer);

    LZ_CHECK(out.data != NULL);
    if (out.data != NULL) {
        LZ_CHECK(strstr(out.data, "== main ==") != NULL);
        LZ_CHECK(strstr(out.data, "0000    1 OP_GET_GLOBAL") != NULL);
        LZ_CHECK(strstr(out.data, "'println'") != NULL);
        LZ_CHECK(strstr(out.data, "0003    | OP_CONSTANT") != NULL);
        LZ_CHECK(strstr(out.data, "'halo'") != NULL);
        LZ_CHECK(strstr(out.data, "'42'") != NULL);
        LZ_CHECK(strstr(out.data, "OP_CALL") != NULL);
        LZ_CHECK(strstr(out.data, "0011    3 OP_RETURN") != NULL);
    }

    LzChunk math;
    lz_chunk_init(&math);
    emit(&math, LZ_OP_NEGATE, 1);
    emit(&math, LZ_OP_ADD, 1);
    emit(&math, LZ_OP_SUBTRACT, 1);
    emit(&math, LZ_OP_MULTIPLY, 1);
    emit(&math, LZ_OP_DIVIDE, 1);
    lz_disassemble_chunk(&math, "math", NULL, &writer);
    LZ_CHECK(strstr(out.data, "OP_NEGATE") != NULL);
    LZ_CHECK(strstr(out.data, "0001    | OP_ADD") != NULL);
    LZ_CHECK(strstr(out.data, "OP_SUBTRACT") != NULL);
    LZ_CHECK(strstr(out.data, "OP_MULTIPLY") != NULL);
    LZ_CHECK(strstr(out.data, "0004    | OP_DIVIDE") != NULL);
    emit(&math, LZ_OP_EQUAL, 2);
    emit(&math, LZ_OP_LESS, 2);
    emit(&math, LZ_OP_GREATER, 2);
    emit(&math, LZ_OP_NOT, 2);
    lz_disassemble_chunk(&math, "logic", NULL, &writer);
    LZ_CHECK(strstr(out.data, "0005    2 OP_EQUAL") != NULL);
    LZ_CHECK(strstr(out.data, "OP_LESS") != NULL);
    LZ_CHECK(strstr(out.data, "OP_GREATER") != NULL);
    LZ_CHECK(strstr(out.data, "0008    | OP_NOT") != NULL);
    emit(&math, LZ_OP_FLOOR_DIVIDE, 3);
    emit(&math, LZ_OP_MODULO, 3);
    lz_disassemble_chunk(&math, "more", NULL, &writer);
    LZ_CHECK(strstr(out.data, "0009    3 OP_FLOOR_DIVIDE") != NULL);
    LZ_CHECK(strstr(out.data, "0010    | OP_MODULO") != NULL);
    lz_chunk_free(&vm.heap, &math);

    LzSource lines;
    lz_source_from_string(&lines, "t.lz", "a\nb\n");
    LzChunk located;
    lz_chunk_init(&located);
    emit(&located, LZ_OP_NIL, 0);
    emit(&located, LZ_OP_POP, 2);
    emit(&located, LZ_OP_RETURN, 3);
    lz_disassemble_chunk(&located, "located", &lines, &writer);
    LZ_CHECK(strstr(out.data, "0000    1 OP_NIL") != NULL);
    LZ_CHECK(strstr(out.data, "0001    2 OP_POP") != NULL);
    LZ_CHECK(strstr(out.data, "0002    | OP_RETURN") != NULL);
    lz_chunk_free(&vm.heap, &located);

    LzChunk flow;
    lz_chunk_init(&flow);
    emit(&flow, LZ_OP_NIL, 1);
    emit_u16(&flow, LZ_OP_JUMP_IF_FALSE, 4, 1);
    emit(&flow, LZ_OP_POP, 1);
    emit_u16(&flow, LZ_OP_GET_LOCAL, 7, 1);
    emit_u16(&flow, LZ_OP_SET_LOCAL, 9, 1);
    emit_u16(&flow, LZ_OP_JUMP, 0, 1);
    emit_u16(&flow, LZ_OP_LOOP, 9, 1);
    emit(&flow, LZ_OP_LESS_EQUAL, 1);
    emit(&flow, LZ_OP_GREATER_EQUAL, 1);
    emit(&flow, LZ_OP_RETURN, 1);
    lz_disassemble_chunk(&flow, "flow", NULL, &writer);
    LZ_CHECK(strstr(out.data, "OP_JUMP_IF_FALSE") != NULL);
    LZ_CHECK(strstr(out.data, "4 -> 0008") != NULL);
    LZ_CHECK(strstr(out.data, "OP_GET_LOCAL") != NULL);
    LZ_CHECK(strstr(out.data, "OP_SET_LOCAL") != NULL);
    LZ_CHECK(strstr(out.data, "OP_LOOP") != NULL);
    LZ_CHECK(strstr(out.data, "9 -> 0008") != NULL);
    LZ_CHECK(strstr(out.data, "OP_LESS_EQUAL") != NULL);
    LZ_CHECK(strstr(out.data, "OP_GREATER_EQUAL") != NULL);
    lz_chunk_free(&vm.heap, &flow);

    LzChunk typed;
    lz_chunk_init(&typed);
    LzType nullable_string = lz_type_of_kinds(LZ_KIND_STRING | LZ_KIND_NIL);
    uint16_t type_index = 0;
    LZ_CHECK(lz_chunk_add_type(&typed, &nullable_string, &type_index));
    emit_u16(&typed, LZ_OP_CHECK_TYPE, type_index, 1);
    emit_u16(&typed, LZ_OP_CHECK_TYPE, 9, 1);
    lz_disassemble_chunk(&typed, "typed", NULL, &writer);
    LZ_CHECK(strstr(out.data, "OP_CHECK_TYPE") != NULL);
    LZ_CHECK(strstr(out.data, "'string | nil'") != NULL);
    LZ_CHECK(strstr(out.data, "'<invalid>'") != NULL);
    lz_chunk_free(&vm.heap, &typed);

    size_t next = lz_disassemble_instruction(&chunk, 0, NULL, &writer);
    LZ_CHECK(next == 3);

    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_free(&vm);
    lz_buffer_free(&out);
    return LZ_TEST_RESULT();
}
