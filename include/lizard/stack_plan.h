#ifndef LIZARD_STACK_PLAN_H
#define LIZARD_STACK_PLAN_H

#include "lizard/chunk.h"

typedef struct LzStackPlan {
    size_t values;
    size_t handlers;
} LzStackPlan;

/* Measures one frame, including its parameters and catch entries. Callee frames, spread arguments and native callback reserves must be admitted separately at their call sites. */
LzStatus lz_chunk_stack_plan(const LzChunk *chunk, size_t parameters, LzStackPlan *plan);

#endif
