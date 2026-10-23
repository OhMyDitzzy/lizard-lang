/* Runtime helpers called from machine code, and executable memory. See ge_jit_rt.h. */

#define _DEFAULT_SOURCE

#include "ge_jit_rt.h"

#include <stdlib.h>

#include "ge_jit_common.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define GE_JIT_HAVE_MMAP 1
#else
#define GE_JIT_HAVE_MMAP 0
#endif

/* ---------------------------------------------------------------------------------
 * Runtime helpers called from machine code
 * ------------------------------------------------------------------------------- */

void jit_drop(LzVM *vm, LzObject *object) {
    if (object->refcount > 1) {
        object->refcount--;
    } else {
        lz_object_release(&vm->heap, object);
    }
}

/* Drops count values from first[count - 1] down to first[0]. */
void jit_drop_range(LzVM *vm, LzValue *first, uint64_t count) {
    while (count > 0) {
        count--;
        if (first[count].type == LZ_VAL_OBJECT) {
            jit_drop(vm, first[count].as.object);
        }
    }
}

static const LzValue *cell_of(const LzTable *table, const LzValue *found, uint32_t *slot) {
    const LzTableEntry *entry = (const LzTableEntry *)((const char *)found - offsetof(LzTableEntry, value));
    *slot = (uint32_t)(entry - table->entries);
    return found;
}

/* The cell of a global variable, or NULL when the name is not one. */
const LzValue *jit_global_cell(LzVM *vm, const LzString *name, JitSite *site) {
    const LzTable *table = &vm->globals;
    if (site->hint < table->capacity && table->entries[site->hint].key == name) {
        return &table->entries[site->hint].value;
    }
    const LzValue *found = lz_table_get(table, name);
    return found == NULL ? NULL : cell_of(table, found, &site->hint);
}

/* A global or else a builtin (globals shadow builtins), or NULL when undefined. */
const LzValue *jit_global_ptr(LzVM *vm, const LzString *name, JitSite *site) {
    const LzValue *found = jit_global_cell(vm, name, site);
    if (found != NULL) {
        return found;
    }
    const LzTable *builtins = &vm->builtins;
    if (site->count == (uint32_t)vm->globals.count && site->bhint < builtins->capacity &&
        builtins->entries[site->bhint].key == name) {
        return &builtins->entries[site->bhint].value;
    }
    found = lz_table_get(builtins, name);
    if (found == NULL) {
        return NULL;
    }
    site->count = (uint32_t)vm->globals.count;
    return cell_of(builtins, found, &site->bhint);
}

/*
 * OP_CALL through the shared call machinery. Low 32 bits: the status. Bit 32: a frame
 * was pushed. *entry: the callee's native entry point when it has one.
 */
uint64_t jit_call(LzVM *vm, uint64_t argc, uint64_t resume, uint64_t call_offset, void **entry) {
    bool entered = false;
    *entry = NULL;
    LzStatus status = lz_vm_call(vm, (size_t)argc, (size_t)resume, (uint32_t)call_offset, &entered);
    if (status == LZ_OK && entered) {
        const LzFunction *function = vm->frames[vm->frame_count - 1].closure->function;
        const GeProgram *program = function->engine_cache;
        if (program != NULL && program->jit != NULL) {
            *entry = program->jit->entry_call;
        }
    }
    return (uint64_t)status | (entered ? (uint64_t)1 << 32 : 0);
}

uint32_t jit_delegate(LzVM *vm, const LzChunk *chunk, uint64_t byte_offset) {
    size_t ip = (size_t)byte_offset;
    bool halted = false;
    return (uint32_t)lz_vm_exec_one(vm, chunk, &ip, &halted);
}

#if GE_JIT_HAVE_MMAP

void *jit_exec_alloc(const uint8_t *code, size_t size, size_t *mapped) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return NULL;
    }
    size_t length = (size + (size_t)page - 1) / (size_t)page * (size_t)page;
    void *memory = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        return NULL;
    }
    memcpy(memory, code, size);
#if defined(__GNUC__) || defined(__clang__)
    /* Instruction and data caches are separate on ARM: make the new code visible. */
    __builtin___clear_cache((char *)memory, (char *)memory + size);
#endif
    if (mprotect(memory, length, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, length);
        return NULL;
    }
    *mapped = length;
    return memory;
}

static void jit_exec_free(void *memory, size_t length) {
    munmap(memory, length);
}

#else

void *jit_exec_alloc(const uint8_t *code, size_t size, size_t *mapped) {
    (void)code;
    (void)size;
    (void)mapped;
    return NULL;
}

static void jit_exec_free(void *memory, size_t length) {
    (void)memory;
    (void)length;
}

#endif

void ge_jit_free(struct GeJit *jit) {
    if (jit == NULL) {
        return;
    }
    if (jit->mem != NULL) {
        jit_exec_free(jit->mem, jit->mem_size);
    }
    free(jit->entry);
    free(jit->depth);
    free(jit->sites);
    free(jit);
}
