#ifndef LIZARD_INTERFACE_BUILD_H
#define LIZARD_INTERFACE_BUILD_H

/*
 * Building an iface, used by the compiler only and not installed. The reading side is
 * lizard/iface.h. Every function that allocates returns NULL or false when out of memory.
 */

#include "lizard/interface.h"

LzInterface *lz_iface_new(void);
/* Frees the iface and everything it holds. NULL is fine. Called when its chunk is released. */
void lz_interface_free(LzInterface *iface);

LzIfaceEntry *lz_iface_add_entry(LzInterface *iface, LzIfaceKind kind, const char *name);
bool lz_iface_entry_set_type(LzIfaceEntry *entry, const char *type);
/* Takes the signature, which is freed with the entry. */
void lz_iface_entry_set_sig(LzIfaceEntry *entry, LzIfaceSig *sig);
bool lz_iface_entry_set_parent(LzIfaceEntry *entry, const char *parent);
bool lz_iface_entry_add_trait(LzIfaceEntry *entry, const char *trait);
LzIfaceMember *lz_iface_add_member(LzIfaceEntry *entry, LzIfaceMemberKind kind, const char *name);
/* The member at `index`, writable. The pointer is good until the next member is added. */
LzIfaceMember *lz_iface_member_at(LzIfaceEntry *entry, size_t index);

bool lz_iface_member_set_type(LzIfaceMember *member, const char *type);
void lz_iface_member_set_sig(LzIfaceMember *member, LzIfaceSig *sig);
void lz_iface_member_set_flags(LzIfaceMember *member, bool read_only, bool has_default, bool required,
                               bool is_override, bool explicit_init);

LzIfaceSig *lz_iface_sig_new(void);
void lz_iface_sig_free(LzIfaceSig *sig);
LzIfaceSig *lz_iface_sig_clone(const LzIfaceSig *sig);
bool lz_iface_sig_add_param(LzIfaceSig *sig, const char *name, const char *type, bool has_default, bool is_rest);
bool lz_iface_sig_set_result(LzIfaceSig *sig, const char *type, bool declared);
bool lz_iface_sig_add_type_param(LzIfaceSig *sig, const char *name);

#endif
