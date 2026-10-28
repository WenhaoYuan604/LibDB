#ifndef LIBDB_MERGE_FLUSH_H
#define LIBDB_MERGE_FLUSH_H
#include "subspace.h"
#include "task.h"
struct thread;

/* Queue by key: structural callbacks may replace a group before retry. */
void merge_check_after_delete(struct thread *thread, key_type key);
void maintenance_request_flush(struct thread *thread, key_type key);
void maintenance_poll(struct thread *thread);
void maintenance_defer_flush(struct thread *thread, key_type key);
void maintenance_clear_deferred(struct thread *thread);
bool merge_same_partition(struct thread *thread, key_type left, key_type right);

void read_pin_blocks(struct thread *thread, const uint32_t *blocks, size_t count);
void read_unpin_blocks(struct thread *thread, const uint32_t *blocks, size_t count);
void reclaim_blocks(struct thread *thread, const uint32_t *blocks, size_t count);
bool read_cache_mapping_current(struct thread *thread, key_type key, struct address_type addr);
#endif
