#ifndef KVBUFFER_H_
#define KVBUFFER_H_

#include "kv_record.h"

#include "wb_snapshot.h"



struct write_buffer {
    _Alignas(64) uint64_t live_bytes;
    uint64_t slot_bytes;
    _Alignas(64) uint64_t published_live_bytes;
    uint64_t published_slot_bytes;
};
_Static_assert(sizeof(struct write_buffer) == 128, "write-buffer cache-line layout");
_Static_assert(offsetof(struct write_buffer, published_live_bytes) == 64,
               "snapshot must not share the writer cache line");


size_t write_buffer_allocation_size(int size);
size_t write_buffer_slot_bytes(const struct write_buffer *buffer);
void write_buffer_initialize(struct write_buffer *buffer);
size_t write_buffer_live_bytes(const struct write_buffer *buffer);
void write_buffer_update_in_place(char *, size_t, key_type, kv_version_type, const char *, int);
char *write_buffer_allocate(int size, struct write_buffer *buffer);
char *write_buffer_insert(key_type key, kv_version_type version,
                          const char *value, int value_size,
                          struct write_buffer *buffer);




int write_buffer_free(char *record, int size, struct write_buffer *buffer);

#endif
