#ifndef LIBDB_SLOT_INDEX_H
#define LIBDB_SLOT_INDEX_H
#include "base.h"
#include "subspace.h"
#include "write_buffer_index.h"
#include <stdatomic.h>


struct slot_allocator { atomic_size_t live_slots; };
struct slot_update { struct index_slot *slot; struct index_slot image; };
struct thread;
void slot_allocator_initialize(struct slot_allocator *allocator);
size_t slot_allocator_live_slots(const struct slot_allocator *allocator);

struct index_slot *slot_index_allocate(struct slot_allocator *allocator);
void slot_index_free(struct slot_allocator *allocator, struct index_slot *slot);

int slot_index_apply_updates(const struct slot_update *updates, uint32_t count);
struct write_buffer_return_type slot_index_insert(struct subspace_info *, char *, key_type, int, int);
struct address_type slot_index_search(struct subspace_info *, key_type);


int slot_index_insert_after_flush(struct subspace_info *, key_type *, struct address_type *,
                                 int, struct slot_allocator *);
bool slot_index_delete(struct subspace_info *, key_type, struct thread *);
#endif
