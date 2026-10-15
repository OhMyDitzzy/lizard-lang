#ifndef LIZARD_DEBUG_H
#define LIZARD_DEBUG_H

#include "lizard/chunk.h"
#include "lizard/source.h"
#include "lizard/writer.h"

/*
 * source may be NULL. With it, the position column shows line numbers; without
 * it, the raw source offset stored in the chunk.
 */
void lz_disassemble_chunk(const LzChunk *chunk, const char *name, const LzSource *source,
                          const LzWriter *out);

/* Writes one instruction and returns the byte offset of the next one. */
size_t lz_disassemble_instruction(const LzChunk *chunk, size_t offset, const LzSource *source,
                                  const LzWriter *out);

#endif
