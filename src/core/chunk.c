#include "interface_build.h"
#include "lizard/chunk.h"

#include <stdlib.h>
#include <string.h>

typedef struct MemberSite {
    size_t offset_plus_one;
    LzMemberCache cache;
} MemberSite;

typedef struct ChunkCache {
    LzStringBuild *builds;
    size_t build_count;
    size_t build_capacity;
    MemberSite *members;
    LzGlobalSite sites[];
} ChunkCache;

static ChunkCache *chunk_cache(const LzChunk *chunk) {
    return chunk->global_sites == NULL ? NULL :
        (ChunkCache *)((unsigned char *)chunk->global_sites - offsetof(ChunkCache, sites));
}

static void free_cache(LzChunk *chunk) {
    ChunkCache *cache = chunk_cache(chunk);
    if (cache != NULL) {
        for (size_t i = 0; i < cache->build_count; i++) free(cache->builds[i].pieces);
        free(cache->builds);
        free(cache->members);
        free(cache);
    }
}

void lz_chunk_init(LzChunk *chunk) {
    chunk->code = NULL;
    chunk->offsets = NULL;
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->constants = NULL;
    chunk->constant_count = 0;
    chunk->constant_capacity = 0;
    chunk->types = NULL;
    chunk->type_count = 0;
    chunk->type_capacity = 0;
    chunk->type_slots = NULL;
    chunk->type_slot_capacity = 0;
    chunk->module = NULL;
    chunk->iface = NULL;
    chunk->global_sites = NULL;
    chunk->global_site_count = 0;
}

void lz_chunk_discard(LzChunk *chunk) {
    free(chunk->code);
    free(chunk->offsets);
    free(chunk->constants);
    free(chunk->types);
    free(chunk->type_slots);
    free_cache(chunk);
    lz_interface_free(chunk->iface);
    lz_chunk_init(chunk);
}

void lz_chunk_free(LzHeap *heap, LzChunk *chunk) {
    for (size_t i = 0; i < chunk->constant_count; i++) {
        lz_value_release(heap, &chunk->constants[i]);
    }
    lz_chunk_discard(chunk);
}

static bool reserve_code(LzChunk *chunk, size_t extra) {
    size_t needed = chunk->count + extra;
    if (needed <= chunk->capacity) {
        return true;
    }

    size_t capacity = chunk->capacity != 0 ? chunk->capacity : 64;
    while (capacity < needed) {
        capacity *= 2;
    }

    uint8_t *code = realloc(chunk->code, capacity);
    if (code == NULL) {
        return false;
    }
    chunk->code = code;

    uint32_t *offsets = realloc(chunk->offsets, capacity * sizeof *offsets);
    if (offsets == NULL) {
        return false;
    }
    chunk->offsets = offsets;
    chunk->capacity = capacity;
    return true;
}

bool lz_chunk_write(LzChunk *chunk, uint8_t byte, uint32_t source_offset) {
    if (!reserve_code(chunk, 1)) {
        return false;
    }
    chunk->code[chunk->count] = byte;
    chunk->offsets[chunk->count] = source_offset;
    chunk->count++;
    return true;
}

bool lz_chunk_write_u16(LzChunk *chunk, uint16_t operand, uint32_t source_offset) {
    if (!reserve_code(chunk, 2)) {
        return false;
    }
    return lz_chunk_write(chunk, (uint8_t)(operand >> 8), source_offset) &&
           lz_chunk_write(chunk, (uint8_t)(operand & 0xFF), source_offset);
}

bool lz_chunk_add_constant(LzChunk *chunk, LzValue value, uint16_t *index) {
    if (chunk->constant_count >= LZ_CONSTANTS_MAX) {
        return false;
    }

    if (chunk->constant_count == chunk->constant_capacity) {
        size_t capacity = chunk->constant_capacity != 0 ? chunk->constant_capacity * 2 : 8;
        LzValue *grown = realloc(chunk->constants, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        chunk->constants = grown;
        chunk->constant_capacity = capacity;
    }

    lz_value_retain(&value);
    chunk->constants[chunk->constant_count] = value;
    *index = (uint16_t)chunk->constant_count;
    chunk->constant_count++;
    return true;
}

static void insert_type_slot(uint32_t *slots, size_t capacity, uint32_t hash, size_t index) {
    size_t i = hash & (capacity - 1);
    while (slots[i] != 0) {
        i = (i + 1) & (capacity - 1);
    }
    slots[i] = (uint32_t)index + 1;
}

static bool rebuild_type_slots(LzChunk *chunk) {
    size_t capacity = 16;
    while (capacity < (chunk->type_count + 1) * 2) {
        capacity *= 2;
    }
    uint32_t *slots = calloc(capacity, sizeof *slots);
    if (slots == NULL) {
        return false;
    }
    for (size_t i = 0; i < chunk->type_count; i++) {
        insert_type_slot(slots, capacity, lz_type_hash(&chunk->types[i]), i);
    }
    free(chunk->type_slots);
    chunk->type_slots = slots;
    chunk->type_slot_capacity = capacity;
    return true;
}

bool lz_chunk_add_type(LzChunk *chunk, const LzType *type, uint16_t *index) {
    uint32_t hash = lz_type_hash(type);
    if (chunk->type_slots != NULL) {
        size_t i = hash & (chunk->type_slot_capacity - 1);
        while (chunk->type_slots[i] != 0) {
            size_t candidate = chunk->type_slots[i] - 1;
            if (lz_type_equal(&chunk->types[candidate], type)) {
                *index = (uint16_t)candidate;
                return true;
            }
            i = (i + 1) & (chunk->type_slot_capacity - 1);
        }
    }
    if (chunk->type_count >= LZ_TYPES_MAX) {
        return false;
    }

    if (chunk->type_count == chunk->type_capacity) {
        size_t capacity = chunk->type_capacity != 0 ? chunk->type_capacity * 2 : 4;
        LzType *grown = realloc(chunk->types, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        chunk->types = grown;
        chunk->type_capacity = capacity;
    }
    chunk->types[chunk->type_count] = *type;
    *index = (uint16_t)chunk->type_count;
    chunk->type_count++;

    if (chunk->type_slots == NULL || chunk->type_count * 2 > chunk->type_slot_capacity) {
        return rebuild_type_slots(chunk);
    }
    insert_type_slot(chunk->type_slots, chunk->type_slot_capacity, hash, chunk->type_count - 1);
    return true;
}

const char *lz_opcode_name(uint8_t opcode) {
    switch (opcode) {
    case LZ_OP_STRING_BEGIN: return "OP_STRING_BEGIN";
    case LZ_OP_STRING_FALLBACK: return "OP_STRING_FALLBACK";
    case LZ_OP_STRING_CHECK: return "OP_STRING_CHECK";
    case LZ_OP_BUILD_STRING: return "OP_BUILD_STRING";
    case LZ_OP_CONSTANT:
        return "OP_CONSTANT";
    case LZ_OP_NIL:
        return "OP_NIL";
    case LZ_OP_TRUE:
        return "OP_TRUE";
    case LZ_OP_FALSE:
        return "OP_FALSE";
    case LZ_OP_POP:
        return "OP_POP";
    case LZ_OP_GET_GLOBAL:
        return "OP_GET_GLOBAL";
    case LZ_OP_DEFINE_GLOBAL:
        return "OP_DEFINE_GLOBAL";
    case LZ_OP_CALL:
        return "OP_CALL";
    case LZ_OP_ADD:
        return "OP_ADD";
    case LZ_OP_SUBTRACT:
        return "OP_SUBTRACT";
    case LZ_OP_MULTIPLY:
        return "OP_MULTIPLY";
    case LZ_OP_DIVIDE:
        return "OP_DIVIDE";
    case LZ_OP_FLOOR_DIVIDE:
        return "OP_FLOOR_DIVIDE";
    case LZ_OP_MODULO:
        return "OP_MODULO";
    case LZ_OP_NEGATE:
        return "OP_NEGATE";
    case LZ_OP_EQUAL:
        return "OP_EQUAL";
    case LZ_OP_LESS:
        return "OP_LESS";
    case LZ_OP_GREATER:
        return "OP_GREATER";
    case LZ_OP_NOT:
        return "OP_NOT";
    case LZ_OP_LESS_EQUAL:
        return "OP_LESS_EQUAL";
    case LZ_OP_GREATER_EQUAL:
        return "OP_GREATER_EQUAL";
    case LZ_OP_GET_LOCAL:
        return "OP_GET_LOCAL";
    case LZ_OP_SET_LOCAL:
        return "OP_SET_LOCAL";
    case LZ_OP_SET_GLOBAL:
        return "OP_SET_GLOBAL";
    case LZ_OP_JUMP:
        return "OP_JUMP";
    case LZ_OP_JUMP_IF_FALSE:
        return "OP_JUMP_IF_FALSE";
    case LZ_OP_JUMP_IF_TRUE:
        return "OP_JUMP_IF_TRUE";
    case LZ_OP_LOOP:
        return "OP_LOOP";
    case LZ_OP_CHECK_TYPE:
        return "OP_CHECK_TYPE";
    case LZ_OP_ARRAY_NEW:
        return "OP_ARRAY_NEW";
    case LZ_OP_ARRAY_PUSH:
        return "OP_ARRAY_PUSH";
    case LZ_OP_INDEX:
        return "OP_INDEX";
    case LZ_OP_SET_INDEX:
        return "OP_SET_INDEX";
    case LZ_OP_SLICE:
        return "OP_SLICE";
    case LZ_OP_RETURN:
        return "OP_RETURN";
    case LZ_OP_CALL_EX:
        return "OP_CALL_EX";
    case LZ_OP_CLOSURE:
        return "OP_CLOSURE";
    case LZ_OP_GET_UPVALUE:
        return "OP_GET_UPVALUE";
    case LZ_OP_SET_UPVALUE:
        return "OP_SET_UPVALUE";
    case LZ_OP_CLOSE_UPVALUE:
        return "OP_CLOSE_UPVALUE";
    case LZ_OP_GET_SELF:
        return "OP_GET_SELF";
    case LZ_OP_CHECK_PARAM:
        return "OP_CHECK_PARAM";
    case LZ_OP_CHECK_RETURN:
        return "OP_CHECK_RETURN";
    case LZ_OP_JUMP_IF_SUPPLIED:
        return "OP_JUMP_IF_SUPPLIED";
    case LZ_OP_ARRAY_EXTEND:
        return "OP_ARRAY_EXTEND";
    case LZ_OP_DICT:
        return "OP_DICT";
    case LZ_OP_ITER_PAIR:
        return "OP_ITER_PAIR";
    case LZ_OP_ITER_CHECK:
        return "OP_ITER_CHECK";
    case LZ_OP_SWAP:
        return "OP_SWAP";
    case LZ_OP_BIT_AND:
        return "OP_BIT_AND";
    case LZ_OP_BIT_OR:
        return "OP_BIT_OR";
    case LZ_OP_BIT_XOR:
        return "OP_BIT_XOR";
    case LZ_OP_BIT_NOT:
        return "OP_BIT_NOT";
    case LZ_OP_SHL:
        return "OP_SHL";
    case LZ_OP_SHR:
        return "OP_SHR";
    case LZ_OP_INDEX_KEEP:
        return "OP_INDEX_KEEP";
    case LZ_OP_ITER_LENGTH:
        return "OP_ITER_LENGTH";
    case LZ_OP_ITER_NEXT:
        return "OP_ITER_NEXT";
    case LZ_OP_RANGE_TEST:
        return "OP_RANGE_TEST";
    case LZ_OP_RANGE_LAST:
        return "OP_RANGE_LAST";
    case LZ_OP_RANGE_NEXT:
        return "OP_RANGE_NEXT";
    case LZ_OP_CLOSE_UPVALUE_KEEP:
        return "OP_CLOSE_UPVALUE_KEEP";
    case LZ_OP_IN:
        return "OP_IN";
    case LZ_OP_IN_RANGE:
        return "OP_IN_RANGE";
    case LZ_OP_NIL_TEST:
        return "OP_NIL_TEST";
    case LZ_OP_DUP:
        return "OP_DUP";
    case LZ_OP_TRY_BEGIN:
        return "OP_TRY_BEGIN";
    case LZ_OP_TRY_END:
        return "OP_TRY_END";
    case LZ_OP_THROW:
        return "OP_THROW";
    case LZ_OP_ERROR_IS:
        return "OP_ERROR_IS";
    case LZ_OP_GET_FIELD:
        return "OP_GET_FIELD";
    case LZ_OP_GET_FIELD_KEEP:
        return "OP_GET_FIELD_KEEP";
    case LZ_OP_SET_FIELD:
        return "OP_SET_FIELD";
    case LZ_OP_METHOD_LOOKUP:
        return "OP_METHOD_LOOKUP";
    case LZ_OP_GET_FIELD_W:
        return "OP_GET_FIELD_W";
    case LZ_OP_CLASS_INIT:
        return "OP_CLASS_INIT";
    case LZ_OP_CLASS_LINK:
        return "OP_CLASS_LINK";
    case LZ_OP_EXTEND:
        return "OP_EXTEND";
    case LZ_OP_IMPORT:
        return "OP_IMPORT";
    case LZ_OP_ERROR_CLASS:
        return "OP_ERROR_CLASS";
    case LZ_OP_SUPER:
        return "OP_SUPER";
    case LZ_OP_IS:
        return "OP_IS";
    default:
        return "OP_UNKNOWN";
    }
}

size_t lz_opcode_length(uint8_t opcode) {
    switch (opcode) {
    case LZ_OP_STRING_BEGIN:
    case LZ_OP_STRING_FALLBACK:
    case LZ_OP_BUILD_STRING:
    case LZ_OP_CONSTANT:
    case LZ_OP_GET_GLOBAL:
    case LZ_OP_DEFINE_GLOBAL:
    case LZ_OP_SET_GLOBAL:
    case LZ_OP_GET_LOCAL:
    case LZ_OP_SET_LOCAL:
    case LZ_OP_JUMP:
    case LZ_OP_JUMP_IF_FALSE:
    case LZ_OP_JUMP_IF_TRUE:
    case LZ_OP_LOOP:
    case LZ_OP_CHECK_TYPE:
    case LZ_OP_CLOSURE:
    case LZ_OP_GET_UPVALUE:
    case LZ_OP_SET_UPVALUE:
    case LZ_OP_CHECK_RETURN:
        return 3;
    case LZ_OP_CALL:
        return 2;
    case LZ_OP_CALL_EX:
        return 4;
    case LZ_OP_STRING_CHECK:
    case LZ_OP_CHECK_PARAM:
    case LZ_OP_JUMP_IF_SUPPLIED:
        return 5;
    case LZ_OP_NIL:
    case LZ_OP_TRUE:
    case LZ_OP_FALSE:
    case LZ_OP_POP:
    case LZ_OP_ADD:
    case LZ_OP_SUBTRACT:
    case LZ_OP_MULTIPLY:
    case LZ_OP_DIVIDE:
    case LZ_OP_FLOOR_DIVIDE:
    case LZ_OP_MODULO:
    case LZ_OP_NEGATE:
    case LZ_OP_EQUAL:
    case LZ_OP_LESS:
    case LZ_OP_GREATER:
    case LZ_OP_LESS_EQUAL:
    case LZ_OP_GREATER_EQUAL:
    case LZ_OP_NOT:
    case LZ_OP_ARRAY_NEW:
    case LZ_OP_ARRAY_PUSH:
    case LZ_OP_INDEX:
    case LZ_OP_SET_INDEX:
    case LZ_OP_SLICE:
    case LZ_OP_RETURN:
    case LZ_OP_CLOSE_UPVALUE:
    case LZ_OP_GET_SELF:
    case LZ_OP_ARRAY_EXTEND:
    case LZ_OP_SWAP:
    case LZ_OP_BIT_AND:
    case LZ_OP_BIT_OR:
    case LZ_OP_BIT_XOR:
    case LZ_OP_BIT_NOT:
    case LZ_OP_SHL:
    case LZ_OP_SHR:
    case LZ_OP_INDEX_KEEP:
    case LZ_OP_ITER_LENGTH:
    case LZ_OP_ITER_NEXT:
    case LZ_OP_RANGE_NEXT:
    case LZ_OP_IN:
    case LZ_OP_NIL_TEST:
    case LZ_OP_DUP:
    case LZ_OP_TRY_END:
    case LZ_OP_THROW:
        return 1;
    case LZ_OP_ERROR_IS:
        return 2;
    case LZ_OP_TRY_BEGIN:
    case LZ_OP_GET_FIELD:
    case LZ_OP_GET_FIELD_KEEP:
    case LZ_OP_GET_FIELD_W:
    case LZ_OP_SET_FIELD:
    case LZ_OP_CLASS_INIT:
        return 3;
    case LZ_OP_METHOD_LOOKUP:
        return 4;
    case LZ_OP_CLASS_LINK:
    case LZ_OP_EXTEND:
    case LZ_OP_ERROR_CLASS:
        return 6;
    case LZ_OP_IMPORT:
        return 8;
    case LZ_OP_SUPER:
        return 4;
    case LZ_OP_IS:
        return 3;
    case LZ_OP_CLOSE_UPVALUE_KEEP:
        return 3; /* the slot whose upvalue is closed */
    case LZ_OP_DICT:
        return 3; /* the entry count */
    case LZ_OP_ITER_CHECK:
        return 1;
    case LZ_OP_ITER_PAIR:
        return 1;
    case LZ_OP_RANGE_TEST:
    case LZ_OP_RANGE_LAST:
    case LZ_OP_IN_RANGE:
        return 2; /* the inclusive flag */
    default:
        return 0;
    }
}

void lz_chunk_truncate(LzHeap *heap, LzChunk *chunk, size_t code_count, size_t constant_count) {
    if (code_count < chunk->count) {
        chunk->count = code_count;
    }
    while (chunk->constant_count > constant_count) {
        chunk->constant_count--;
        lz_value_release(heap, &chunk->constants[chunk->constant_count]);
    }
    /* Keep live build plans, but no cached cell may refer to a reused constant index. */
    ChunkCache *cache = chunk_cache(chunk);
    if (cache != NULL) {
        while (cache->build_count > 0 && cache->builds[cache->build_count - 1].end > code_count) {
            free(cache->builds[--cache->build_count].pieces);
        }
        memset(chunk->global_sites, 0, chunk->global_site_count * sizeof *chunk->global_sites);
        if (cache->members != NULL) memset(cache->members, 0, LZ_MEMBER_SITES_MAX * sizeof *cache->members);
    }
}

LzGlobalSite *lz_chunk_global_site(const LzChunk *chunk, uint16_t index) {
    LzChunk *cache = (LzChunk *)chunk;
    if (index >= cache->global_site_count) {
        size_t count = cache->constant_count > (size_t)index + 1 ? cache->constant_count : (size_t)index + 1;
        ChunkCache *old = chunk_cache(chunk);
        ChunkCache *grown = realloc(old, offsetof(ChunkCache, sites) + count * sizeof(LzGlobalSite));
        if (grown == NULL) return NULL;
        if (old == NULL) {
            grown->builds = NULL;
            grown->build_count = grown->build_capacity = 0;
            grown->members = NULL;
        }
        memset(grown->sites + cache->global_site_count, 0,
               (count - cache->global_site_count) * sizeof(LzGlobalSite));
        cache->global_sites = grown->sites;
        cache->global_site_count = count;
    }
    return &cache->global_sites[index];
}

LzMemberCache *lz_chunk_member_site(const LzChunk *chunk, size_t offset) {
    if (lz_chunk_global_site(chunk, 0) == NULL) return NULL;
    ChunkCache *cache = chunk_cache(chunk);
    if (cache->members == NULL) {
        cache->members = calloc(LZ_MEMBER_SITES_MAX, sizeof *cache->members);
        if (cache->members == NULL) return NULL;
    }
    MemberSite *site = &cache->members[offset % LZ_MEMBER_SITES_MAX];
    if (site->offset_plus_one != offset + 1) {
        memset(site, 0, sizeof *site);
        site->offset_plus_one = offset + 1;
    }
    return &site->cache;
}

bool lz_chunk_add_string_build(LzChunk *chunk, const LzStringBuild *build, uint16_t *index) {
    if (build->count == 0 || build->count > 64 || build->pieces == NULL) return false;
    size_t slots = build->count;
    for (size_t i = 0; i < build->count; i++) {
        if (build->pieces[i].mode > LZ_STRING_PIECE_FORMAT) return false;
        if (build->pieces[i].mode == LZ_STRING_PIECE_FORMAT) slots++;
    }
    if (build->slots != slots || build->peak < slots) return false;
    if (lz_chunk_global_site(chunk, 0) == NULL) return false;
    ChunkCache *cache = chunk_cache(chunk);
    if (cache->build_count >= UINT16_MAX) return false;
    if (cache->build_count == cache->build_capacity) {
        size_t capacity = cache->build_capacity == 0 ? 4 : cache->build_capacity * 2;
        LzStringBuild *grown = realloc(cache->builds, capacity * sizeof *grown);
        if (grown == NULL) return false;
        cache->builds = grown;
        cache->build_capacity = capacity;
    }
    *index = (uint16_t)cache->build_count;
    cache->builds[cache->build_count++] = *build;
    return true;
}

const LzStringBuild *lz_chunk_string_build(const LzChunk *chunk, uint16_t index) {
    const ChunkCache *cache = chunk_cache(chunk);
    return cache != NULL && index < cache->build_count ? &cache->builds[index] : NULL;
}
