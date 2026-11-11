#ifndef LIBDB_WAL_H
#define LIBDB_WAL_H
#include "base.h"

#define WAL_SEGMENT_BYTES (4U * 1024U * 1024U)
#define WAL_BATCH_BYTES (64U * 1024U)
#define WAL_HEADER_BYTES 64U
#define WAL_RECORD_HEADER_BYTES 32U
#define WAL_MIN_RECORD_BYTES (WAL_RECORD_HEADER_BYTES + LIBDB_VALID_VALUE_PREFIX_SIZE)
#define WAL_FORMAT_VERSION 2U
#define WAL_MAX_RECORDS ((WAL_BATCH_BYTES - WAL_HEADER_BYTES) / WAL_MIN_RECORD_BYTES)
#define WAL_DEFAULT_WAIT_US 10U
#define WAL_DEFAULT_MB 8192U
#define WAL_DEFAULT_MAX_INFLIGHT 1U
#define WAL_MAX_INFLIGHT 16U
_Static_assert(WAL_SEGMENT_BYTES == (uint64_t)alloc_grain * BLOCK_SIZE,
               "WAL and allocator extents must share their alignment");

struct thread;
struct write_buffer_index_node;
struct wal_segment;
struct wal_dependency {
    struct wal_segment *segment;
    uint64_t generation, count;
    key_type key;
    struct write_buffer_index_node *owner;
    struct wal_dependency *node_next, *seg_next, *seg_prev;
};
struct wal_segment {
    uint64_t generation, pending;
    uint32_t used, batch_refs;
    bool occupied, sealed;
    struct wal_dependency *dependencies;
};
struct wal_item {
    struct request *request;
    uint64_t lsn, version, start;
    struct wal_dependency *dependency;
};
enum wal_batch_state { WAL_BATCH_FREE, WAL_BATCH_BUILDING, WAL_BATCH_RETRY,
    WAL_BATCH_INFLIGHT, WAL_BATCH_COMPLETE, WAL_BATCH_APPLYING, WAL_BATCH_FAILED };
struct wal_context;
struct wal_batch {
    struct wal_context *context;
    unsigned char *buffer;
    struct wal_item items[WAL_MAX_RECORDS];
    struct wal_segment *segment;
    uint64_t generation, started, sequence, block;
    uint32_t count, valid_bytes, bytes;
    bool capacity_ready;
    enum wal_batch_state state;
};
struct wal_context {
    struct thread *worker;
    struct wal_segment *segments;
    uint32_t segment_count, head, tail, occupied;
    uint64_t start_block, next_lsn, next_batch, batch_delay_ticks;
    struct wal_batch *pool, *preparing;
    struct wal_batch *fifo[WAL_MAX_INFLIGHT];
    unsigned max_inflight, fifo_head, outstanding, inflight;
    bool failed, applying;
    uint64_t wait_started;
    uint64_t records, batches, bytes, reclaimed, wait_ticks, pressure_flushes;
    unsigned peak_inflight, peak_outstanding, peak_completed_waiting;
    bool credit_waiting;
    uint64_t credit_wait_started, credit_wait_ticks, submit_retries;
};




struct wal_recovery_batch {
    uint64_t generation, sequence, max_lsn, max_version;
    uint32_t count, valid_bytes, bytes;
};
struct wal_recovery_record {
    uint64_t key, version, lsn;
    uint32_t value_size;
    const unsigned char *value;
};
int wal_recovery_header(const unsigned char *, size_t, unsigned worker,
                        unsigned segment, struct wal_recovery_batch *);
int wal_recovery_decode(const unsigned char *, size_t, unsigned worker,
                        unsigned segment, struct wal_recovery_batch *);
void wal_recovery_record(const unsigned char *, struct wal_recovery_record *);

int wal_recovery_reset(struct thread *, const uint64_t *generations,
                       uint32_t tail, uint64_t max_lsn, uint64_t max_sequence);

int wal_recovery_finish(struct thread *, const uint64_t *generations,
                       uint32_t tail, uint64_t max_lsn, uint64_t max_sequence);


int wal_layout(uint64_t blocks, unsigned workers, unsigned mb_per_worker,
               uint64_t *data_blocks, uint64_t *worker_blocks);
int wal_init(struct thread *, uint64_t start_block, uint64_t block_count);

bool wal_enqueue_put(struct thread *, struct request *, uint64_t start);
void wal_poll(struct thread *, bool force);
bool wal_busy(const struct thread *);
void wal_check_error(const struct thread *);
void wal_apply_ready(struct thread *);
void wal_attach_dependencies(struct write_buffer_index_node *, struct wal_dependency *);
void wal_release_dependencies(struct write_buffer_index_node *);
void wal_pressure_poll(struct thread *);
void wal_drain(struct thread *);
void wal_destroy(struct thread *);
void wal_print_stats(const struct thread *, FILE *);
#endif
