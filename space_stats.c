#include "space_stats.h"
#include "insert.h"
#include "kv_record.h"
#include <time.h>
#include <inttypes.h>

const char *space_stats_load_bytes(const struct user_rq *requests, int count,
                                  uint64_t *bytes) {
    *bytes = 0;
    if(!requests || count <= 0) return "load_input_unavailable";
    uint64_t total = 0;
    for(int i = 0; i < count; i++) {
        if(requests[i].type != 1) continue;
        if(requests[i].size < 0) return "invalid_load_record_size";
        uint64_t size = (uint64_t)requests[i].size + KV_RECORD_HEADER_SIZE;
        if(total > UINT64_MAX - size) return "load_bytes_overflow";
        total += size;
    }
    *bytes = total;
    return NULL;
}

const char *space_stats_snapshot(const struct db *db, struct space_snapshot *out) {
    *out = (struct space_snapshot){0};
    if(!db || !db->workers || db->thread <= 0) return "workers_unavailable";
    if(!g_map) return "gmap_unavailable";
    if(g_block_num != db->data_blocks) return "gmap_size_mismatch";
    if(g_block_num > UINT64_MAX / BLOCK_SIZE) return "capacity_overflow";

    /* Workers must be stopped: g_map is not atomic. Zero-reference blocks
     * do not count, even if not returned to the allocator. The map covers
     * only the data area; WAL blocks are outside its range. */
    uint64_t used = 0;
    for(uint64_t i = 0; i < g_block_num; i++)
        if(g_map[i] >= 1) used++;

    out->used_blocks = used;
    out->used_bytes = used * BLOCK_SIZE;
    return NULL;
}

void space_stats_print(const struct db *db, FILE *fp) {
    if(!fp) return;
    struct space_snapshot snapshot;
    const char *snapshot_error = space_stats_snapshot(db, &snapshot);
    const char *error = db ? db->initial_load.error : "database_unavailable";
    if(!error && !db->initial_load.complete) error = "load_not_completed";
    if(!error && !db->initial_load.record_bytes) error = "zero_initial_load_bytes";
    if(!error) error = snapshot_error;
    fprintf(fp, "SSD space: exclude_wal=1 force_flush=0 denominator=initial_load_input occupancy=gmap_nonzero\n");
    if(!db || db->initial_load.error || !db->initial_load.complete)
        fprintf(fp, "initial_load_record_bytes=N/A\n");
    else
        fprintf(fp, "initial_load_record_bytes=%" PRIu64 "\n", db->initial_load.record_bytes);
    if(snapshot_error)
        fprintf(fp, "ssd_data_used_blocks=N/A ssd_data_used_bytes=N/A snapshot_reason=%s\n", snapshot_error);
    else
        fprintf(fp, "ssd_data_used_blocks=%" PRIu64 " ssd_data_used_bytes=%" PRIu64 "\n",
                    snapshot.used_blocks, snapshot.used_bytes);
    if(error) fprintf(fp, "ssd_space_amplification=N/A reason=%s\n", error);
    else fprintf(fp, "ssd_space_amplification=%.9f\n",
                 (double)snapshot.used_bytes / (double)db->initial_load.record_bytes);
    if(!snapshot_error) {
        uint64_t online = atomic_load_explicit(&g_used_data_blocks, memory_order_relaxed);
        fprintf(fp, "ssd_online_used_blocks=%" PRIu64 " online_counter_matches_snapshot=%d\n",
                online, online == snapshot.used_blocks);
    }
    fflush(fp);
}

/* Online control reads only immutable geometry and atomic occupancy. */
double space_stats_gc_fraction(const struct db *db) {
    double threshold = db && atomic_load_explicit(&db->gc_space.relaxed, memory_order_relaxed)
        ? LIBDB_GC_RELAXED_THRESHOLD : LIBDB_SUBSPACE_REDUNDANT_THRESHOLD;
    return threshold / (1.0 + threshold);
}

void space_stats_gc_poll_at(struct db *db, uint64_t now_ns) {
    if(!db) return;
    struct gc_space_control *control = &db->gc_space;
    /* Do not arm the timer during load or when recovery skipped load. */
    if(!db->initial_load.complete) {
        atomic_store_explicit(&control->relaxed, false, memory_order_relaxed);
        return;
    }
    if(control->sampled && now_ns - control->last_sample_ns < LIBDB_GC_POLL_NS) return;
    uint64_t blocks = atomic_load_explicit(&g_used_data_blocks, memory_order_relaxed);
    const char *error = db->initial_load.error;
    if(!error && !db->initial_load.record_bytes) error = "zero_initial_load_bytes";
    if(!error && !g_map) error = "gmap_unavailable";
    if(!error && g_block_num != db->data_blocks) error = "gmap_size_mismatch";
    if(!error && (blocks > db->data_blocks || blocks > UINT64_MAX / BLOCK_SIZE))
        error = "invalid_online_occupancy";
    double amplification = error ? 0 : (double)(blocks * BLOCK_SIZE) / db->initial_load.record_bytes;
    bool relaxed = !error && amplification <= LIBDB_GC_SPACE_LIMIT;
    atomic_store_explicit(&control->relaxed, relaxed, memory_order_relaxed);
    control->sampled = true;
    control->last_sample_ns = now_ns;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        abort();
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

void space_stats_gc_poll(struct db *db) {
    if(db && db->initial_load.complete) space_stats_gc_poll_at(db, monotonic_ns());
}

void space_stats_gc_wait(struct db *db, unsigned seconds) {
    uint64_t end = monotonic_ns() + (uint64_t)seconds * UINT64_C(1000000000);
    for(;;) {
        uint64_t now = monotonic_ns();
        space_stats_gc_poll_at(db, now);
        if(now >= end) break;
        uint64_t delay = end - now;
        if(delay > LIBDB_GC_POLL_NS) delay = LIBDB_GC_POLL_NS;
        struct timespec duration = {.tv_sec = delay / UINT64_C(1000000000),
                                   .tv_nsec = delay % UINT64_C(1000000000)};
        nanosleep(&duration, NULL);
    }
}

/* Phase and pressure are the only fields read by workers. Main owns sampling. */
void space_stats_nvm_init(struct db *db) {
    atomic_init(&db->nvm_flush.phase, NVM_FLUSH_FIXED);
    atomic_init(&db->nvm_flush.pressure, false);
}

bool space_stats_nvm_flush_allowed(const struct db *db) {
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED || !db) return true;
    int phase = atomic_load_explicit(&db->nvm_flush.phase, memory_order_acquire);
    return phase == NVM_FLUSH_FIXED || (phase == NVM_FLUSH_WORKLOAD &&
        atomic_load_explicit(&db->nvm_flush.pressure, memory_order_relaxed));
}

static void nvm_sample(struct db *db, uint64_t now_ns) {
    struct nvm_flush_control *c = &db->nvm_flush;
    /* Missing accounting must never silently enable unbounded deferral. */
    uint64_t bytes = db->write_buffer ?
        atomic_load_explicit(&db->write_buffer->used_slot_bytes, memory_order_relaxed) : UINT64_MAX;
    bool pressure = bytes >= LIBDB_NVM_FLUSH_LIMIT_BYTES;
    if(pressure != atomic_load_explicit(&c->pressure, memory_order_relaxed)) c->pressure_changes++;
    atomic_store_explicit(&c->pressure, pressure, memory_order_relaxed);
    c->sampled_bytes = bytes;
    if(bytes > c->peak_bytes) c->peak_bytes = bytes;
    c->last_sample_ns = now_ns;
    c->sampled = true;
}

void space_stats_nvm_start_at(struct db *db, uint64_t now_ns) {
    if(!db || !LIBDB_DYNAMIC_FLUSH_ENABLED) return;
    struct nvm_flush_control *c = &db->nvm_flush;
    c->sampled = false;
    c->peak_bytes = c->pressure_changes = 0;
    atomic_store_explicit(&c->pressure, false, memory_order_relaxed);
    nvm_sample(db, now_ns);
    atomic_store_explicit(&c->phase, NVM_FLUSH_WORKLOAD, memory_order_release);
}

void space_stats_nvm_poll_at(struct db *db, uint64_t now_ns) {
    if(!db || !LIBDB_DYNAMIC_FLUSH_ENABLED ||
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) != NVM_FLUSH_WORKLOAD) return;
    if(db->nvm_flush.sampled && now_ns - db->nvm_flush.last_sample_ns < LIBDB_NVM_FLUSH_POLL_NS) return;
    nvm_sample(db, now_ns);
}

void space_stats_nvm_start(struct db *db) { space_stats_nvm_start_at(db, monotonic_ns()); }
void space_stats_nvm_poll(struct db *db) {
    if(db && LIBDB_DYNAMIC_FLUSH_ENABLED &&
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) == NVM_FLUSH_WORKLOAD)
        space_stats_nvm_poll_at(db, monotonic_ns());
}
void space_stats_nvm_stop(struct db *db) {
    if(db && LIBDB_DYNAMIC_FLUSH_ENABLED &&
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) == NVM_FLUSH_WORKLOAD)
        atomic_store_explicit(&db->nvm_flush.phase, NVM_FLUSH_STOPPED, memory_order_release);
}
void space_stats_nvm_print(const struct db *db, FILE *fp) {
    if(!db || !fp) return;
    fprintf(fp, "NVM flush: enabled=%d limit_bytes=%" PRIu64 " poll_ns=%" PRIu64
        " occupancy=unreclaimed_slots phase=workload_only soft_limit=1\n",
        LIBDB_DYNAMIC_FLUSH_ENABLED, (uint64_t)LIBDB_NVM_FLUSH_LIMIT_BYTES,
        (uint64_t)LIBDB_NVM_FLUSH_POLL_NS);
    fprintf(fp, "nvm_used_slot_bytes=%" PRIu64 " nvm_sampled_peak_bytes=%" PRIu64
        " nvm_pressure_changes=%" PRIu64 " nvm_sampled=%d\n",
        db->write_buffer ? atomic_load_explicit(&db->write_buffer->used_slot_bytes, memory_order_relaxed) : 0,
        db->nvm_flush.peak_bytes, db->nvm_flush.pressure_changes, db->nvm_flush.sampled);
}
