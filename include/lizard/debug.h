#ifndef LIZARD_DEBUG_H
#define LIZARD_DEBUG_H

#include "lizard/chunk.h"
#include "lizard/writer.h"

void lz_disassemble_chunk(const LzChunk *chunk, const char *name, const LzWriter *out);

/* Writes one instruction and returns the offset of the next one. */
size_t lz_disassemble_instruction(const LzChunk *chunk, size_t offset, const LzWriter *out);

#endif
