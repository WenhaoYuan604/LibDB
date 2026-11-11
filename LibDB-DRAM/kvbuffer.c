#include "kvbuffer.h"

static void allocation_failed(void) {
    fprintf(stderr, "fatal: DRAM write buffer allocation failed\n");
    exit(EXIT_FAILURE);
}

static void publish_usage(struct write_buffer *buffer) {
    wb_snapshot_store(&buffer->published_live_bytes, buffer->live_bytes);
    wb_snapshot_store(&buffer->published_slot_bytes, buffer->slot_bytes);
}

void write_buffer_initialize(struct write_buffer *buffer) {
    buffer->live_bytes = buffer->slot_bytes = 0;
    publish_usage(buffer);
}

size_t write_buffer_live_bytes(const struct write_buffer *buffer) {
    return wb_snapshot_load(&buffer->published_live_bytes);
}

size_t write_buffer_slot_bytes(const struct write_buffer *buffer) {
    return wb_snapshot_load(&buffer->published_slot_bytes);
}

size_t write_buffer_allocation_size(int size) {
    if(size < (int)KV_RECORD_ENCODED_SIZE || size > LIBDB_WBF_MAX_RECORD_SIZE) return 0;
    size_t slot = 32;
    while(slot < (size_t)size) slot *= 2;
    return slot;
}

char *write_buffer_allocate(int size, struct write_buffer *buffer) {
    size_t slot = write_buffer_allocation_size(size);
    if(!buffer || !slot) return NULL;
    if(buffer->live_bytes > UINT64_MAX - (uint64_t)size ||
       buffer->slot_bytes > UINT64_MAX - slot) allocation_failed();
    char *record = malloc(slot);
    if(!record) allocation_failed();
    buffer->live_bytes += (uint64_t)size;
    buffer->slot_bytes += slot;
    publish_usage(buffer);
    return record;
}


char *write_buffer_insert(key_type key, kv_version_type version,
                          const char *value, int value_size,
                          struct write_buffer *buffer) {
    if(!value || value_size < (int)KV_RECORD_VALID_VALUE_SIZE ||
       kv_record_size((size_t)value_size) > LIBDB_WBF_MAX_RECORD_SIZE) {
        fprintf(stderr, "fatal: invalid DRAM write buffer value size %d\n", value_size);
        exit(EXIT_FAILURE);
    }
    size_t size = kv_record_size((size_t)value_size);
    char *record = write_buffer_allocate((int)size, buffer);
    if(!record) allocation_failed();
    if(kv_record_encode(record, size, key, version, value, (size_t)value_size)) {
        fprintf(stderr, "fatal: failed to encode DRAM write buffer record\n");
        exit(EXIT_FAILURE);
    }
    return record;
}

int write_buffer_free(char *record, int size, struct write_buffer *buffer) {
    if(!record || !buffer || size < (int)KV_RECORD_ENCODED_SIZE ||
       size > LIBDB_WBF_MAX_RECORD_SIZE) return 0;
    struct kv_record_view view;
    size_t slot = write_buffer_allocation_size(size);

    if(kv_record_decode(record, (size_t)size, &view) ||
       view.record_size != (size_t)size || buffer->live_bytes < (size_t)size ||
       buffer->slot_bytes < slot) {
        fprintf(stderr, "fatal: invalid DRAM write buffer record or accounting\n");
        exit(EXIT_FAILURE);
    }
    free(record);
    buffer->live_bytes -= (size_t)size;
    buffer->slot_bytes -= slot;
    publish_usage(buffer);
    return 1;
}

void write_buffer_update_in_place(char *dest, size_t record_size, key_type key,
    kv_version_type version, const char *value, int value_size){
    if(dest == NULL || value == NULL ||
       value_size < (int)KV_RECORD_VALID_VALUE_SIZE ||
       record_size > LIBDB_WBF_MAX_RECORD_SIZE ||
       record_size != kv_record_size((size_t)value_size)){
        fprintf(stderr, "write_buffer_update_in_place: invalid arguments\n");
        exit(EXIT_FAILURE);
    }
    struct kv_record_view old;
    if(kv_record_decode_for_key(dest, record_size, key, &old) != 0 ||
       old.record_size != record_size || old.value_size != value_size){
        fprintf(stderr, "write_buffer_update_in_place: invalid original record or size mismatch\n");
        exit(EXIT_FAILURE);
    }
    if(kv_record_encode(dest, record_size, key, version, value, (size_t)value_size) != 0){
        fprintf(stderr, "write_buffer_update_in_place: failed to assign PMEM record\n");
        exit(EXIT_FAILURE);
    }
}
