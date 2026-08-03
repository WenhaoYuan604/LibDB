#ifndef LIBDB_KV_RECORD_H_
#define LIBDB_KV_RECORD_H_

#include "base.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t kv_version_type;

#define KV_RECORD_KEY_SIZE_BYTES   ((size_t)sizeof(uint16_t))
#define KV_RECORD_VERSION_BYTES    ((size_t)sizeof(kv_version_type))
#define KV_RECORD_VALUE_SIZE_BYTES ((size_t)sizeof(uint16_t))
#define KV_RECORD_HEADER_SIZE      \
    (KV_RECORD_KEY_SIZE_BYTES + sizeof(key_type) + KV_RECORD_VERSION_BYTES + \
     KV_RECORD_VALUE_SIZE_BYTES)
#define KV_RECORD_VALID_VALUE_SIZE ((size_t)LIBDB_PMEM_VALUE_PREFIX_SIZE)
#define KV_RECORD_PERSISTED_SIZE \
    (KV_RECORD_HEADER_SIZE + KV_RECORD_VALID_VALUE_SIZE)

struct __attribute__((packed, may_alias)) kv_record_value_prefix {
    unsigned char bytes[LIBDB_PMEM_VALUE_PREFIX_SIZE];
};

/*
 * Runtime-sized persistent layout. The allocation continues after value_prefix
 * up to key/value size selected by --kvsize, but that invalid tail is never
 * assigned or persisted by the PMEM write path.
 */
struct __attribute__((packed)) kv_pmem_record {
    uint16_t key_size;
    key_type key;
    kv_version_type version;
    uint16_t value_size;
    struct kv_record_value_prefix value_prefix;
    unsigned char invalid_value_tail[];
};

_Static_assert(sizeof(struct kv_pmem_record) == KV_RECORD_PERSISTED_SIZE,
               "fixed PMEM KV fields must occupy header plus value prefix");

static inline size_t kv_record_size(size_t value_size){
    return KV_RECORD_HEADER_SIZE + value_size;
}

struct kv_record_view {
    uint16_t key_size;
    key_type key;
    kv_version_type version;
    uint16_t value_size;
    const char *value;
    size_t record_size;
};

/*
 * Assign only the persistent fields and the valid 32-byte value prefix.
 * Deliberately do not touch the remainder of the logical value.
 */
static inline int kv_record_encode(char *dest, size_t dest_size,
                                   key_type key, kv_version_type version,
                                   const char *value, size_t value_size){
    if(dest == NULL || value == NULL ||
       value_size < KV_RECORD_VALID_VALUE_SIZE || value_size > UINT16_MAX ||
       dest_size != kv_record_size(value_size)){
        return -1;
    }

    struct kv_pmem_record *stored = (struct kv_pmem_record *)(void *)dest;
    const struct kv_record_value_prefix *prefix =
        (const struct kv_record_value_prefix *)(const void *)value;
    stored->key_size = (uint16_t)sizeof(key_type);
    stored->key = key;
    stored->version = version;
    stored->value_size = (uint16_t)value_size;
    stored->value_prefix = *prefix;
    return 0;
}

static inline int kv_record_copy_valid_value(
    char *dest, size_t dest_size, const struct kv_record_view *view){
    if(dest == NULL || view == NULL || view->value == NULL ||
       dest_size < view->value_size ||
       view->value_size < KV_RECORD_VALID_VALUE_SIZE){
        return -1;
    }

    struct kv_record_value_prefix *dest_prefix =
        (struct kv_record_value_prefix *)(void *)dest;
    const struct kv_record_value_prefix *source_prefix =
        (const struct kv_record_value_prefix *)(const void *)view->value;
    *dest_prefix = *source_prefix;
    return 0;
}

static inline int kv_record_decode(const char *record, size_t available,
                                   struct kv_record_view *view){
    if(record == NULL || view == NULL ||
       available < KV_RECORD_PERSISTED_SIZE){
        return -1;
    }

    const struct kv_pmem_record *stored =
        (const struct kv_pmem_record *)(const void *)record;
    view->key_size = stored->key_size;
    if(view->key_size != sizeof(key_type)){
        return -1;
    }
    view->key = stored->key;
    view->version = stored->version;
    view->value_size = stored->value_size;
    view->record_size = kv_record_size(view->value_size);
    if(view->value_size < KV_RECORD_VALID_VALUE_SIZE ||
       view->record_size > available){
        return -1;
    }
    view->value = (const char *)stored->value_prefix.bytes;
    return 0;
}

static inline int kv_record_decode_for_key(const char *record, size_t available,
                                           key_type expected_key,
                                           struct kv_record_view *view){
    if(kv_record_decode(record, available, view) != 0 ||
       view->key != expected_key){
        return -1;
    }
    return 0;
}

/* next_version and version_limit are private to one server worker. */
kv_version_type kv_record_next_thread_version(
    kv_version_type *next_version, kv_version_type *version_limit);

#endif
