#include "wal.h"
#include "insert.h"
#include "get.h"
#include "kv_record.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

static void fatal(const char *message) {
    fprintf(stderr, "fatal: WAL %s\n", message);
    exit(EXIT_FAILURE);
}

static void put32(unsigned char *p, uint32_t v) {
    for(unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v) {
    for(unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
int wal_layout(uint64_t blocks, unsigned workers, unsigned mb,
               uint64_t *data_blocks, uint64_t *worker_blocks) {
    const uint64_t grain = WAL_SEGMENT_BYTES / BLOCK_SIZE;
    if(!workers || workers > ctr_max_qp || mb < 8 || mb % 4) return -1;
    uint64_t each = (uint64_t)mb * 1024 * 1024 / BLOCK_SIZE;
    uint64_t aligned = blocks / grain * grain;
    if(each > aligned / workers || aligned - each * workers < (uint64_t)workers * alloc_grain)
        return -1;

    if(aligned - each * workers > INT_MAX) return -1;
    *data_blocks = aligned - each * workers;
    *worker_blocks = each;
    return 0;
}
static void free_storage(struct wal_context *w) {
    if(w->pool) {
        for(unsigned i = 0; i <= w->max_inflight; i++)
            if(w->pool[i].buffer) spdk_free(w->pool[i].buffer);
        free(w->pool);
    }
    free(w->segments);
    free(w);
}
static struct wal_batch *new_preparing(struct wal_context *w) {
    for(unsigned i = 0; i <= w->max_inflight; i++) {
        struct wal_batch *b = &w->pool[i];
        if(b->state == WAL_BATCH_FREE) {
            b->state = WAL_BATCH_BUILDING;
            b->count = b->bytes = 0;
            b->valid_bytes = WAL_HEADER_BYTES;
            b->capacity_ready = false;
            b->segment = NULL;
            return b;
        }
    }
    fatal("batch pool exhausted despite available credit");
    return NULL;
}
void wal_check_error(const struct thread *t) {
    if(t->wal && t->wal->failed) fatal("I/O failed; pending requests not acknowledged");
}
int wal_init(struct thread *t, uint64_t start, uint64_t blocks) {
    uint64_t count = blocks / (WAL_SEGMENT_BYTES / BLOCK_SIZE);
    unsigned max = t->db && t->db->wal_max_inflight ? t->db->wal_max_inflight : WAL_DEFAULT_MAX_INFLIGHT;
    if(t->wal || start % alloc_grain || blocks % (WAL_SEGMENT_BYTES / BLOCK_SIZE) ||
       count < 2 || count > UINT32_MAX || max > WAL_MAX_INFLIGHT) return -1;
    struct wal_context *w = calloc(1, sizeof(*w));
    if(!w) return -1;
    w->max_inflight = max;
    w->segments = calloc((size_t)count, sizeof(*w->segments));
    w->pool = calloc(max + 1, sizeof(*w->pool));
    if(!w->segments || !w->pool) { free_storage(w); return -1; }
    for(unsigned i = 0; i <= max; i++) {
        w->pool[i].context = w;
        w->pool[i].buffer = spdk_zmalloc(WAL_BATCH_BYTES, BLOCK_SIZE, NULL,
                                        SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        if(!w->pool[i].buffer) { free_storage(w); return -1; }
    }
    unsigned wait_us = t->db && t->db->wal_wait_us ? t->db->wal_wait_us : WAL_DEFAULT_WAIT_US;
    uint64_t hz = spdk_get_ticks_hz();
    w->batch_delay_ticks = (hz / 1000000) * wait_us +
        ((hz % 1000000) * wait_us + 999999) / 1000000;
    w->worker = t;
    w->start_block = start;
    w->segment_count = (uint32_t)count;
    w->occupied = 1;
    w->segments[0].occupied = true;
    w->segments[0].generation = 1;
    w->next_lsn = w->next_batch = 1;
    w->preparing = new_preparing(w);
    t->wal = w;
    return 0;
}
static void reclaim(struct wal_context *w) {
    while(w->occupied) {
        struct wal_segment *s = &w->segments[w->head];
        if(!s->sealed || s->pending || s->batch_refs) break;
        assert(!s->dependencies);
        s->occupied = false;
        w->occupied--;
        w->reclaimed++;
        w->head = (w->head + 1) % w->segment_count;
    }
}
static bool reserve_batch(struct wal_context *w) {
    reclaim(w);
    struct wal_segment *s = &w->segments[w->tail];
    if(s->occupied && !s->sealed && s->used + WAL_BATCH_BYTES <= WAL_SEGMENT_BYTES)
        return true;
    if(s->occupied) s->sealed = true;
    reclaim(w);
    uint32_t next = (w->tail + 1) % w->segment_count;
    s = &w->segments[next];
    if(s->occupied) return false;
    if(!w->occupied) w->head = next;
    if(s->generation == UINT64_MAX) fatal("segment generation exhausted");
    s->generation++;
    s->used = 0;
    s->sealed = false;
    s->occupied = true;
    w->tail = next;
    w->occupied++;
    return true;
}
static bool batch_expired(const struct wal_context *w) {
    return spdk_get_ticks() - w->preparing->started >= w->batch_delay_ticks;
}
static void finish_credit_wait(struct wal_context *w) {
    if(w->credit_waiting) {
        w->credit_wait_ticks += spdk_get_ticks() - w->credit_wait_started;
        w->credit_waiting = false;
    }
}
bool wal_enqueue_put(struct thread *t, struct request *r, uint64_t start) {
    struct wal_context *w = t->wal;
    assert(w);
    wal_check_error(t);
    struct wal_batch *b = w->preparing;
    if(r->type != 1 || !r->value || r->size < KV_RECORD_VALID_VALUE_SIZE ||
       kv_record_size((size_t)r->size) > BLOCK_SIZE) fatal("invalid PUT");
    uint32_t record_bytes = WAL_RECORD_HEADER_BYTES + (uint32_t)r->size;

    if(b->state == WAL_BATCH_BUILDING &&
       (b->count == WAL_MAX_RECORDS || record_bytes > WAL_BATCH_BYTES - b->valid_bytes))
        b->capacity_ready = true;
    if(b->capacity_ready && w->outstanding == w->max_inflight) {
        if(!w->credit_waiting) {
            w->credit_waiting = true;
            w->credit_wait_started = spdk_get_ticks();
        }
        return false;
    }
    finish_credit_wait(w);
    if(b->state == WAL_BATCH_RETRY || b->capacity_ready ||
       (b->count && w->outstanding < w->max_inflight && batch_expired(w))) return false;
    if(!b->count && !reserve_batch(w)) {
        if(!w->wait_started) w->wait_started = spdk_get_ticks();
        return false;
    }
    if(w->wait_started) {
        w->wait_ticks += spdk_get_ticks() - w->wait_started;
        w->wait_started = 0;
    }
    if(!b->count) {
        memset(b->buffer, 0, WAL_BATCH_BYTES);
        b->started = spdk_get_ticks();
        b->segment = &w->segments[w->tail];
        b->generation = b->segment->generation;
        b->segment->batch_refs++;
    }
    struct wal_dependency *d = calloc(1, sizeof(*d));
    if(!d) fatal("dependency allocation failed");
    struct wal_segment *s = b->segment;
    d->segment = s;
    d->generation = b->generation;
    d->count = 1;
    d->key = r->key;
    d->seg_next = s->dependencies;
    if(d->seg_next) d->seg_next->seg_prev = d;
    s->dependencies = d;
    s->pending++;
    if(w->next_lsn == UINT64_MAX) fatal("LSN exhausted");
    struct wal_item *item = &b->items[b->count];
    *item = (struct wal_item){r, w->next_lsn++,
        kv_record_next_thread_version(&t->kv_version_next, &t->kv_version_limit), start, d};
    unsigned char *p = b->buffer + b->valid_bytes;
    put64(p, item->lsn);
    put64(p + 8, r->key);
    put64(p + 16, item->version);
    put32(p + 24, (uint32_t)r->size);
    put32(p + 28, 1);

    memcpy(p + WAL_RECORD_HEADER_BYTES, r->value, KV_RECORD_VALID_VALUE_SIZE);
    b->valid_bytes += record_bytes;
    b->count++;
    if(b->count == WAL_MAX_RECORDS || WAL_BATCH_BYTES - b->valid_bytes < WAL_MIN_RECORD_BYTES)
        b->capacity_ready = true;
    w->records++;
    return true;
}
static void complete(void *arg, const struct spdk_nvme_cpl *cpl) {
    struct wal_batch *b = arg;
    struct wal_context *w = b->context;
    assert(b->state == WAL_BATCH_INFLIGHT && w->inflight);
    w->inflight--;
    if(spdk_nvme_cpl_is_error(cpl)) {
        b->state = WAL_BATCH_FAILED;
        w->failed = true;
    } else b->state = WAL_BATCH_COMPLETE;

    bool blocked = false;
    unsigned waiting = 0;
    for(unsigned i = 0; i < w->outstanding; i++) {
        struct wal_batch *next = w->fifo[(w->fifo_head + i) % w->max_inflight];
        if(next->state != WAL_BATCH_COMPLETE) blocked = true;
        else if(blocked) waiting++;
    }
    if(waiting > w->peak_completed_waiting) w->peak_completed_waiting = waiting;
}
void wal_poll(struct thread *t, bool force) {
    struct wal_context *w = t->wal;
    if(!w) return;
    wal_check_error(t);
    reclaim(w);
    struct wal_batch *b = w->preparing;
    if(!b->count || w->outstanding == w->max_inflight) return;
    if(!force && b->state != WAL_BATCH_RETRY && !b->capacity_ready && !batch_expired(w)) return;
    if(b->state == WAL_BATCH_BUILDING) {
        struct wal_segment *s = b->segment;
        assert(s->generation == b->generation && s->batch_refs);
        uint32_t valid = b->valid_bytes;
        b->bytes = (valid + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
        if(w->next_batch == UINT64_MAX) fatal("batch sequence exhausted");
        b->sequence = w->next_batch++;
        uint32_t segment_id = (uint32_t)(s - w->segments);
        b->block = w->start_block + (uint64_t)segment_id * (WAL_SEGMENT_BYTES / BLOCK_SIZE) + s->used / BLOCK_SIZE;
        unsigned char *p = b->buffer;
        memcpy(p, "LDBWAL02", 8);
        put32(p + 8, WAL_FORMAT_VERSION);
        put32(p + 12, (uint32_t)t->thread);
        put64(p + 16, b->generation);
        put64(p + 24, b->sequence);
        put32(p + 32, b->count);
        put32(p + 36, valid);
        put32(p + 40, segment_id);
        assert(s->used + b->bytes <= WAL_SEGMENT_BYTES);
        s->used += b->bytes;
        if(s->used + WAL_BATCH_BYTES > WAL_SEGMENT_BYTES) s->sealed = true;
        b->state = WAL_BATCH_RETRY;
    }
    int rc = spdk_nvme_ns_cmd_write(t->ns_entry->ns, t->ns_entry->qpair[t->thread],
        b->buffer, b->block, b->bytes / BLOCK_SIZE, complete, b, 0);
    if(rc == -ENOMEM || rc == -EAGAIN) { w->submit_retries++; return; }
    if(rc) { b->state = WAL_BATCH_FAILED; w->failed = true; wal_check_error(t); }
    b->state = WAL_BATCH_INFLIGHT;
    w->fifo[(w->fifo_head + w->outstanding) % w->max_inflight] = b;
    w->outstanding++;
    w->inflight++;
    if(w->inflight > w->peak_inflight) w->peak_inflight = w->inflight;
    if(w->outstanding > w->peak_outstanding) w->peak_outstanding = w->outstanding;
    w->batches++;
    w->bytes += b->bytes;
    phase_io_add(&t->phase_io, PHASE_WAL_BYTES, b->bytes);
    w->preparing = new_preparing(w);
}
static void unlink_dependency(struct wal_dependency *d) {
    if(d->seg_prev) d->seg_prev->seg_next = d->seg_next;
    else d->segment->dependencies = d->seg_next;
    if(d->seg_next) d->seg_next->seg_prev = d->seg_prev;
}
void wal_attach_dependencies(struct write_buffer_index_node *node, struct wal_dependency *d) {
    if(!d) return;
    for(struct wal_dependency *old = node->wal_dependencies; old; old = old->node_next) {
        if(old->segment == d->segment && old->generation == d->generation) {
            old->count += d->count;
            unlink_dependency(d);
            free(d);
            return;
        }
    }
    d->owner = node;
    d->node_next = node->wal_dependencies;
    node->wal_dependencies = d;
}
void wal_release_dependencies(struct write_buffer_index_node *node) {
    struct wal_dependency *d = node->wal_dependencies;
    node->wal_dependencies = NULL;
    while(d) {
        struct wal_dependency *next = d->node_next;
        assert(d->generation == d->segment->generation && d->segment->pending >= d->count);
        d->segment->pending -= d->count;
        unlink_dependency(d);
        free(d);
        d = next;
    }
}
void wal_apply_ready(struct thread *t) {
    struct wal_context *w = t->wal;
    if(!w) return;
    wal_check_error(t);
    assert(!w->applying);
    w->applying = true;
    while(w->outstanding) {
        struct wal_batch *b = w->fifo[w->fifo_head];
        if(b->state != WAL_BATCH_COMPLETE) break;
        b->state = WAL_BATCH_APPLYING;
        for(uint32_t i = 0; i < b->count; i++) {
            wal_check_error(t);
            struct wal_item *item = &b->items[i];
            struct request *r = item->request;
            insert_version(r->key, r->size, r->value, t, r->break_down, item->version, item->dependency);

            wal_check_error(t);
            server_submit_result(r->key, r->value, 0, 1, 0,
                t->spdk_result_ring[r->key % t->db->load_injectors], item->start, r->break_down);
            thread_completed[t->thread]++;
            free(r);
        }
        assert(b->segment->generation == b->generation && b->segment->batch_refs);
        b->segment->batch_refs--;
        b->segment = NULL;
        b->count = 0;
        b->state = WAL_BATCH_FREE;
        w->fifo[w->fifo_head] = NULL;
        w->fifo_head = (w->fifo_head + 1) % w->max_inflight;
        w->outstanding--;
        finish_credit_wait(w);
        reclaim(w);
    }
    w->applying = false;
}
bool wal_busy(const struct thread *t) {
    return t->wal && (t->wal->preparing->count || t->wal->outstanding);
}
void wal_pressure_poll(struct thread *t) {
    struct wal_context *w = t->wal;
    if(!w) return;
    wal_check_error(t);
    reclaim(w);
    if(!w->occupied || w->segment_count - w->occupied > 1) return;



    for(struct wal_dependency *d = w->segments[w->head].dependencies; d; d = d->seg_next) {
        if(!d->owner) continue;
        struct subspace_info *group = dram_index_search(t->index->dram_index, d->key, NULL, NULL);
        if(!group || subspace_busy(group) ||
           write_buffer_index_search(group->buffer_index, d->key) != d->owner) continue;
        if(subspace_force_flush(group, t)) w->pressure_flushes++;
        return;
    }
}
void wal_drain(struct thread *t) {
    while(wal_busy(t)) {
        wal_poll(t, true);
        if(spdk_nvme_qpair_process_completions(t->ns_entry->qpair[t->thread], 0) < 0)
            fatal("completion polling failed");
        wal_apply_ready(t);
    }
}
void wal_print_stats(const struct thread *t, FILE *out) {
    const struct wal_context *w = t->wal;
    if(!w) return;
    fprintf(out, "WAL worker %d: records=%" PRIu64 " batches=%" PRIu64
        " bytes=%" PRIu64 " avg_records=%.2f reclaimed=%" PRIu64
        " occupied=%u wait_us=%.2f pressure_flushes=%" PRIu64
        "\n",
        t->thread, w->records, w->batches, w->bytes,
        w->batches ? (double)w->records / w->batches : 0.0, w->reclaimed, w->occupied,
        (double)w->wait_ticks * 1000000.0 / spdk_get_ticks_hz(), w->pressure_flushes);
    uint64_t credit_ticks = w->credit_wait_ticks;
    if(w->credit_waiting) credit_ticks += spdk_get_ticks() - w->credit_wait_started;
    fprintf(out, "WAL concurrency worker %d: peak_inflight=%u"
        " peak_outstanding=%u peak_completed_waiting=%u credit_wait_us=%.2f"
        " submit_retries=%" PRIu64 " scope=process\n",
        t->thread, w->peak_inflight, w->peak_outstanding,
        w->peak_completed_waiting, (double)credit_ticks * 1000000.0 / spdk_get_ticks_hz(),
        w->submit_retries);
}
void wal_destroy(struct thread *t) {
    struct wal_context *w = t->wal;
    if(!w) return;
    assert(!wal_busy(t) && !w->inflight && !w->applying);
    for(unsigned i = 0; i <= w->max_inflight; i++)
        assert(!w->pool[i].count && (w->pool[i].state == WAL_BATCH_FREE || &w->pool[i] == w->preparing));

    for(uint32_t i = 0; i < w->segment_count; i++) {
        assert(!w->segments[i].batch_refs);
        struct wal_dependency *d = w->segments[i].dependencies;
        while(d) {
            struct wal_dependency *next = d->seg_next;
            if(d->owner) d->owner->wal_dependencies = NULL;
            free(d);
            d = next;
        }
    }
    free_storage(w);
    t->wal = NULL;
}

static uint32_t recovery_u32(const unsigned char *p) {
    uint32_t v = 0;
    for(unsigned i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint64_t recovery_u64(const unsigned char *p) {
    uint64_t v = 0;
    for(unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
int wal_recovery_header(const unsigned char *p, size_t available, unsigned worker,
                        unsigned segment, struct wal_recovery_batch *b) {
    if(!p || !b || available < WAL_HEADER_BYTES) return -1;
    if(memcmp(p, "LDBWAL", 6)) return -1;
    if(memcmp(p, "LDBWAL02", 8) || recovery_u32(p + 8) != WAL_FORMAT_VERSION ||
       recovery_u32(p + 12) != worker || recovery_u32(p + 40) != segment) return -2;
    *b = (struct wal_recovery_batch){.generation = recovery_u64(p + 16),
        .sequence = recovery_u64(p + 24), .count = recovery_u32(p + 32),
        .valid_bytes = recovery_u32(p + 36)};
    if(!b->generation || !b->sequence || !b->count || b->count > WAL_MAX_RECORDS ||
       b->valid_bytes < WAL_HEADER_BYTES || b->valid_bytes > WAL_BATCH_BYTES) return -1;
    for(unsigned i = 44; i < WAL_HEADER_BYTES; i++) if(p[i]) return -1;
    b->bytes = (b->valid_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
    return b->bytes <= available ? 0 : -1;
}
void wal_recovery_record(const unsigned char *p, struct wal_recovery_record *r) {
    *r = (struct wal_recovery_record){.lsn = recovery_u64(p), .key = recovery_u64(p + 8),
        .version = recovery_u64(p + 16), .value_size = recovery_u32(p + 24),
        .value = p + WAL_RECORD_HEADER_BYTES};
}
int wal_recovery_decode(const unsigned char *p, size_t available, unsigned worker,
                        unsigned segment, struct wal_recovery_batch *b) {
    int rc = wal_recovery_header(p, available, worker, segment, b);
    if(rc) return rc;
    size_t off = WAL_HEADER_BYTES;
    uint64_t previous = 0;
    for(uint32_t i = 0; i < b->count; i++) {
        if(b->valid_bytes - off < WAL_MIN_RECORD_BYTES) return -1;
        struct wal_recovery_record r;
        wal_recovery_record(p + off, &r);
        if(recovery_u32(p + off + 28) != 1 || !r.version || r.lsn <= previous ||
           r.value_size < LIBDB_VALID_VALUE_PREFIX_SIZE ||
           r.value_size > BLOCK_SIZE - KV_RECORD_HEADER_SIZE ||
           r.value_size > b->valid_bytes - off - WAL_RECORD_HEADER_BYTES) return -1;
        previous = r.lsn;
        if(r.version > b->max_version) b->max_version = r.version;
        off += WAL_RECORD_HEADER_BYTES + r.value_size;
    }
    b->max_lsn = previous;
    return off == b->valid_bytes ? 0 : -1;
}
static int restore_recovery_state(struct thread *t, const uint64_t *generations,
                       uint32_t tail, uint64_t max_lsn, uint64_t max_sequence, bool retain) {
    struct wal_context *w = t->wal;
    if(!w || !generations || tail >= w->segment_count || wal_busy(t) ||
       max_lsn == UINT64_MAX || max_sequence == UINT64_MAX) return -1;
    for(uint32_t i = 0; i < w->segment_count; i++)
        if(generations[i] == UINT64_MAX || (w->segments[i].dependencies &&
           (!retain || w->segments[i].generation != generations[i]))) return -1;
    for(uint32_t i = 0; i < w->segment_count; i++)
        w->segments[i] = (struct wal_segment){.generation = generations[i],
            .occupied = true, .sealed = true, .used = WAL_SEGMENT_BYTES,
            .dependencies = retain ? w->segments[i].dependencies : NULL,
            .pending = retain ? w->segments[i].pending : 0};
    w->tail = tail;
    w->head = (tail + 1) % w->segment_count;
    w->occupied = w->segment_count;
    w->next_lsn = max_lsn + 1;
    w->next_batch = max_sequence + 1;
    return 0;
}

int wal_recovery_reset(struct thread *t, const uint64_t *generations,
                       uint32_t tail, uint64_t max_lsn, uint64_t max_sequence) {
    return restore_recovery_state(t, generations, tail, max_lsn, max_sequence, false);
}
int wal_recovery_finish(struct thread *t, const uint64_t *generations,
                        uint32_t tail, uint64_t max_lsn, uint64_t max_sequence) {
    return restore_recovery_state(t, generations, tail, max_lsn, max_sequence, true);
}
