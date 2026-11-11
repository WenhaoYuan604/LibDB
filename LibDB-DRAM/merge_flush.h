#ifndef LIBDB_MERGE_FLUSH_H
#define LIBDB_MERGE_FLUSH_H
#include "subspace.h"
#include "task.h"
struct thread;


void merge_check_after_delete(struct thread *thread, key_type key);
void maintenance_poll(struct thread *thread);
bool merge_same_partition(struct thread *thread, key_type left, key_type right);

void reclaim_blocks(struct thread *thread, const uint32_t *blocks, size_t count);
#endif
