#include "interface_build.h"

#include "lizard/chunk.h"

#include <stdlib.h>
#include <string.h>

typedef struct IfaceParam {
    char *name;
    char *type;
    bool has_default;
    bool is_rest;
} IfaceParam;

struct LzIfaceSig {
    IfaceParam *params;
    size_t param_count;
    size_t param_capacity;
    char *result;
    bool result_declared;
    bool variadic;
    char **type_params; /* `T`, `U` of a generic function, in order */
    size_t type_param_count;
};

struct LzIfaceMember {
    LzIfaceMemberKind kind;
    char *name;
    char *type;
    LzIfaceSig *sig;
    bool read_only;
    bool has_default;
    bool required;
    bool is_override;
    bool explicit_init;
};

struct LzIfaceEntry {
    LzIfaceKind kind;
    char *name;
    char *type;
    LzIfaceSig *sig;
    char *parent;
    char **traits;
    size_t trait_count;
    LzIfaceMember *members;
    size_t member_count;
    size_t member_capacity;
    uint64_t class_id;
};

struct LzInterface {
    LzIfaceEntry *entries;
    size_t count;
    size_t capacity;
};

static char *copy_text(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

/* Replaces *slot by a copy of text. False, with slot unchanged, when out of memory. */
static bool assign_text(char **slot, const char *text) {
    char *copy = copy_text(text);
    if (copy == NULL) {
        return false;
    }
    free(*slot);
    *slot = copy;
    return true;
}

LzInterface *lz_iface_new(void) {
    return calloc(1, sizeof(LzInterface));
}

void lz_iface_sig_free(LzIfaceSig *sig) {
    if (sig == NULL) {
        return;
    }
    for (size_t i = 0; i < sig->param_count; i++) {
        free(sig->params[i].name);
        free(sig->params[i].type);
    }
    free(sig->params);
    for (size_t i = 0; i < sig->type_param_count; i++) {
        free(sig->type_params[i]);
    }
    free(sig->type_params);
    free(sig->result);
    free(sig);
}

static void member_release(LzIfaceMember *member) {
    free(member->name);
    free(member->type);
    lz_iface_sig_free(member->sig);
}

static void entry_release(LzIfaceEntry *entry) {
    free(entry->name);
    free(entry->type);
    lz_iface_sig_free(entry->sig);
    free(entry->parent);
    for (size_t i = 0; i < entry->trait_count; i++) {
        free(entry->traits[i]);
    }
    free(entry->traits);
    for (size_t i = 0; i < entry->member_count; i++) {
        member_release(&entry->members[i]);
    }
    free(entry->members);
}

void lz_interface_free(LzInterface *iface) {
    if (iface == NULL) {
        return;
    }
    for (size_t i = 0; i < iface->count; i++) {
        entry_release(&iface->entries[i]);
    }
    free(iface->entries);
    free(iface);
}

LzIfaceEntry *lz_iface_add_entry(LzInterface *iface, LzIfaceKind kind, const char *name) {
    if (iface->count == iface->capacity) {
        size_t capacity = iface->capacity == 0 ? 16 : iface->capacity * 2;
        LzIfaceEntry *grown = realloc(iface->entries, capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        iface->entries = grown;
        iface->capacity = capacity;
    }
    LzIfaceEntry *entry = &iface->entries[iface->count];
    memset(entry, 0, sizeof *entry);
    entry->kind = kind;
    entry->name = copy_text(name);
    if (entry->name == NULL) {
        return NULL;
    }
    iface->count++;
    return entry;
}

bool lz_iface_entry_set_type(LzIfaceEntry *entry, const char *type) {
    return assign_text(&entry->type, type);
}

void lz_iface_entry_set_sig(LzIfaceEntry *entry, LzIfaceSig *sig) {
    lz_iface_sig_free(entry->sig);
    entry->sig = sig;
}

bool lz_iface_entry_set_parent(LzIfaceEntry *entry, const char *parent) {
    return assign_text(&entry->parent, parent);
}

bool lz_iface_entry_add_trait(LzIfaceEntry *entry, const char *trait) {
    char **grown = realloc(entry->traits, (entry->trait_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    entry->traits = grown;
    char *copy = copy_text(trait);
    if (copy == NULL) {
        return false;
    }
    entry->traits[entry->trait_count++] = copy;
    return true;
}

LzIfaceMember *lz_iface_add_member(LzIfaceEntry *entry, LzIfaceMemberKind kind, const char *name) {
    if (entry->member_count == entry->member_capacity) {
        size_t capacity = entry->member_capacity == 0 ? 8 : entry->member_capacity * 2;
        LzIfaceMember *grown = realloc(entry->members, capacity * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        entry->members = grown;
        entry->member_capacity = capacity;
    }
    LzIfaceMember *member = &entry->members[entry->member_count];
    memset(member, 0, sizeof *member);
    member->kind = kind;
    member->name = copy_text(name);
    if (member->name == NULL) {
        return NULL;
    }
    entry->member_count++;
    return member;
}

LzIfaceMember *lz_iface_member_at(LzIfaceEntry *entry, size_t index) {
    return index < entry->member_count ? &entry->members[index] : NULL;
}

bool lz_iface_member_set_type(LzIfaceMember *member, const char *type) {
    return assign_text(&member->type, type);
}

void lz_iface_member_set_sig(LzIfaceMember *member, LzIfaceSig *sig) {
    lz_iface_sig_free(member->sig);
    member->sig = sig;
}

void lz_iface_member_set_flags(LzIfaceMember *member, bool read_only, bool has_default, bool required,
                               bool is_override, bool explicit_init) {
    member->read_only = read_only;
    member->has_default = has_default;
    member->required = required;
    member->is_override = is_override;
    member->explicit_init = explicit_init;
}

LzIfaceSig *lz_iface_sig_new(void) {
    LzIfaceSig *sig = calloc(1, sizeof *sig);
    if (sig == NULL) {
        return NULL;
    }
    sig->result = copy_text("any");
    if (sig->result == NULL) {
        free(sig);
        return NULL;
    }
    return sig;
}

LzIfaceSig *lz_iface_sig_clone(const LzIfaceSig *sig) {
    LzIfaceSig *copy = lz_iface_sig_new();
    if (copy == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sig->param_count; i++) {
        const IfaceParam *param = &sig->params[i];
        if (!lz_iface_sig_add_param(copy, param->name, param->type, param->has_default, param->is_rest)) {
            lz_iface_sig_free(copy);
            return NULL;
        }
    }
    if (!lz_iface_sig_set_result(copy, sig->result, sig->result_declared)) {
        lz_iface_sig_free(copy);
        return NULL;
    }
    for (size_t i = 0; i < sig->type_param_count; i++) {
        if (!lz_iface_sig_add_type_param(copy, sig->type_params[i])) {
            lz_iface_sig_free(copy);
            return NULL;
        }
    }
    return copy;
}

bool lz_iface_sig_add_type_param(LzIfaceSig *sig, const char *name) {
    char **grown = realloc(sig->type_params, (sig->type_param_count + 1) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    sig->type_params = grown;
    char *copy = copy_text(name);
    if (copy == NULL) {
        return false;
    }
    sig->type_params[sig->type_param_count++] = copy;
    return true;
}

bool lz_iface_sig_add_param(LzIfaceSig *sig, const char *name, const char *type, bool has_default, bool is_rest) {
    if (sig->param_count == sig->param_capacity) {
        size_t capacity = sig->param_capacity == 0 ? 4 : sig->param_capacity * 2;
        IfaceParam *grown = realloc(sig->params, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        sig->params = grown;
        sig->param_capacity = capacity;
    }
    IfaceParam *param = &sig->params[sig->param_count];
    param->name = copy_text(name);
    param->type = copy_text(type);
    param->has_default = has_default;
    param->is_rest = is_rest;
    if (param->name == NULL || param->type == NULL) {
        free(param->name);
        free(param->type);
        return false;
    }
    sig->param_count++;
    if (is_rest) {
        sig->variadic = true;
    }
    return true;
}

bool lz_iface_sig_set_result(LzIfaceSig *sig, const char *type, bool declared) {
    if (!assign_text(&sig->result, type)) {
        return false;
    }
    sig->result_declared = declared;
    return true;
}

const LzInterface *lz_interface_of(const struct LzChunk *chunk) {
    return chunk != NULL ? chunk->iface : NULL;
}

size_t lz_interface_count(const LzInterface *iface) {
    return iface != NULL ? iface->count : 0;
}

const LzIfaceEntry *lz_interface_entry(const LzInterface *iface, size_t index) {
    return iface != NULL && index < iface->count ? &iface->entries[index] : NULL;
}

LzIfaceKind lz_interface_entry_kind(const LzIfaceEntry *entry) {
    return entry->kind;
}

const char *lz_interface_kind_name(LzIfaceKind kind) {
    switch (kind) {
    case LZ_IFACE_FN:
        return "fn";
    case LZ_IFACE_CONST:
        return "const";
    case LZ_IFACE_CLASS:
        return "class";
    case LZ_IFACE_TRAIT:
        return "trait";
    case LZ_IFACE_ERROR:
        return "error";
    case LZ_IFACE_TYPE:
        return "type";
    }
    return "?";
}

const char *lz_interface_entry_name(const LzIfaceEntry *entry) {
    return entry->name;
}

const char *lz_interface_entry_type(const LzIfaceEntry *entry) {
    return entry->type;
}

const LzIfaceSig *lz_interface_entry_sig(const LzIfaceEntry *entry) {
    return entry->sig;
}

uint64_t lz_interface_entry_class_id(const LzIfaceEntry *entry) {
    return entry->class_id;
}

void lz_iface_entry_set_class_id(LzIfaceEntry *entry, uint64_t id) {
    entry->class_id = id;
}

const char *lz_interface_entry_parent(const LzIfaceEntry *entry) {
    return entry->parent;
}

size_t lz_interface_entry_trait_count(const LzIfaceEntry *entry) {
    return entry->trait_count;
}

const char *lz_interface_entry_trait(const LzIfaceEntry *entry, size_t index) {
    return index < entry->trait_count ? entry->traits[index] : NULL;
}

size_t lz_interface_entry_member_count(const LzIfaceEntry *entry) {
    return entry->member_count;
}

const LzIfaceMember *lz_interface_entry_member(const LzIfaceEntry *entry, size_t index) {
    return index < entry->member_count ? &entry->members[index] : NULL;
}

LzIfaceMemberKind lz_interface_member_kind(const LzIfaceMember *member) {
    return member->kind;
}

const char *lz_interface_member_kind_name(LzIfaceMemberKind kind) {
    switch (kind) {
    case LZ_IFACE_MEMBER_INIT:
        return "init";
    case LZ_IFACE_MEMBER_FIELD:
        return "field";
    case LZ_IFACE_MEMBER_METHOD:
        return "method";
    case LZ_IFACE_MEMBER_STATIC:
        return "static";
    case LZ_IFACE_MEMBER_GETTER:
        return "get";
    case LZ_IFACE_MEMBER_CONST:
        return "const";
    case LZ_IFACE_MEMBER_OPERATOR:
        return "operator";
    }
    return "?";
}

const char *lz_interface_member_name(const LzIfaceMember *member) {
    return member->name;
}

const char *lz_interface_member_type(const LzIfaceMember *member) {
    return member->type;
}

const LzIfaceSig *lz_interface_member_sig(const LzIfaceMember *member) {
    return member->sig;
}

bool lz_interface_member_is_read_only(const LzIfaceMember *member) {
    return member->read_only;
}

bool lz_interface_member_has_default(const LzIfaceMember *member) {
    return member->has_default;
}

bool lz_interface_member_is_required(const LzIfaceMember *member) {
    return member->required;
}

bool lz_interface_member_is_override(const LzIfaceMember *member) {
    return member->is_override;
}

bool lz_interface_member_has_explicit_init(const LzIfaceMember *member) {
    return member->explicit_init;
}

size_t lz_interface_sig_param_count(const LzIfaceSig *sig) {
    return sig->param_count;
}

const char *lz_interface_sig_param_name(const LzIfaceSig *sig, size_t index) {
    return index < sig->param_count ? sig->params[index].name : NULL;
}

const char *lz_interface_sig_param_type(const LzIfaceSig *sig, size_t index) {
    return index < sig->param_count ? sig->params[index].type : NULL;
}

bool lz_interface_sig_param_has_default(const LzIfaceSig *sig, size_t index) {
    return index < sig->param_count && sig->params[index].has_default;
}

bool lz_interface_sig_param_is_rest(const LzIfaceSig *sig, size_t index) {
    return index < sig->param_count && sig->params[index].is_rest;
}

bool lz_interface_sig_is_variadic(const LzIfaceSig *sig) {
    return sig->variadic;
}

size_t lz_interface_sig_type_param_count(const LzIfaceSig *sig) {
    return sig->type_param_count;
}

const char *lz_interface_sig_type_param_name(const LzIfaceSig *sig, size_t index) {
    return index < sig->type_param_count ? sig->type_params[index] : "";
}

const char *lz_interface_sig_result(const LzIfaceSig *sig) {
    return sig->result;
}

bool lz_interface_sig_result_declared(const LzIfaceSig *sig) {
    return sig->result_declared;
}
