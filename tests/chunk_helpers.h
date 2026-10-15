#ifndef LIZARD_TEST_CHUNK_HELPERS_H
#define LIZARD_TEST_CHUNK_HELPERS_H

#include <string.h>

#include "lizard/vm.h"

static void emit(LzChunk *chunk, LzOpCode op, uint32_t offset) {
    (void)lz_chunk_write(chunk, (uint8_t)op, offset);
}

static void emit_u8(LzChunk *chunk, LzOpCode op, uint8_t operand, uint32_t offset) {
    emit(chunk, op, offset);
    (void)lz_chunk_write(chunk, operand, offset);
}

static void emit_u16(LzChunk *chunk, LzOpCode op, uint16_t operand, uint32_t offset) {
    emit(chunk, op, offset);
    (void)lz_chunk_write_u16(chunk, operand, offset);
}

static uint16_t constant_value(LzChunk *chunk, LzValue value) {
    uint16_t index = 0;
    (void)lz_chunk_add_constant(chunk, value, &index);
    return index;
}

/* The chunk takes its own reference, so the temporary one is dropped here. */
static uint16_t constant_text(LzVM *vm, LzChunk *chunk, const char *text) {
    LzString *string = lz_string_new(&vm->heap, text, strlen(text));
    uint16_t index = constant_value(chunk, lz_value_object(&string->base));
    lz_object_release(&vm->heap, &string->base);
    return index;
}

static uint16_t constant_name(LzVM *vm, LzChunk *chunk, const char *name) {
    LzString *interned = lz_vm_intern(vm, name, strlen(name));
    return constant_value(chunk, lz_value_object(&interned->base));
}

#endif
