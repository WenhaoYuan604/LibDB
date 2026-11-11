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


double space_stats_gc_fraction(const struct db *db) {
    double threshold = db && atomic_load_explicit(&db->gc_space.relaxed, memory_order_relaxed)
        ? LIBDB_GC_RELAXED_THRESHOLD : LIBDB_SUBSPACE_REDUNDANT_THRESHOLD;
    return threshold / (1.0 + threshold);
}

void space_stats_gc_poll_at(struct db *db, uint64_t now_ns) {
    if(!db) return;
    struct gc_space_control *control = &db->gc_space;

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
        space_stats_nvm_poll_at(db, now);
        if(now >= end) break;
        uint64_t delay = end - now;
        if(delay > LIBDB_GC_POLL_NS) delay = LIBDB_GC_POLL_NS;
        if(LIBDB_DYNAMIC_FLUSH_ENABLED && delay > LIBDB_NVM_FLUSH_POLL_NS)
            delay = LIBDB_NVM_FLUSH_POLL_NS;
        struct timespec duration = {.tv_sec = delay / UINT64_C(1000000000),
                                   .tv_nsec = delay % UINT64_C(1000000000)};
        nanosleep(&duration, NULL);
    }
}


void space_stats_nvm_init(struct db *db) {
    atomic_init(&db->nvm_flush.phase, NVM_FLUSH_FIXED);
}

bool space_stats_nvm_flush_allowed_for_size(const struct db *db, uint64_t buffered_size) {
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED || !db) return true;
    int phase = atomic_load_explicit(&db->nvm_flush.phase, memory_order_acquire);
    return phase == NVM_FLUSH_FIXED || (phase == NVM_FLUSH_ACTIVE &&
        (buffered_size >= LIBDB_FLUSH_HARD_THRESHOLD_BYTES || !db->write_buffer ||
         atomic_load_explicit(&db->write_buffer->pressure, memory_order_relaxed)));
}

bool space_stats_nvm_flush_allowed(const struct db *db) {

    return space_stats_nvm_flush_allowed_for_size(db, 0);
}

static void nvm_sample_locked(struct db *db, uint64_t now_ns) {
    struct nvm_flush_control *c = &db->nvm_flush;
    struct write_buffer *wb = db->write_buffer;
    struct wbf_snapshot snapshot = wb ? write_buffer_snapshot_locked(wb) : (struct wbf_snapshot){0};
    uint64_t bytes = snapshot.used_slot_bytes;
    uint64_t pressure_bytes = snapshot.pressure_bytes;
    if(wb){
        if(pressure_bytes >= LIBDB_NVM_FLUSH_HIGH_BYTES) write_buffer_set_pressure_locked(wb, true);
        else if(pressure_bytes <= LIBDB_NVM_FLUSH_LOW_BYTES) write_buffer_set_pressure_locked(wb, false);
        c->pressure_changes = wb->pressure_changes;
    }
    c->sampled_bytes = bytes;
    if(bytes > c->peak_bytes) c->peak_bytes = bytes;
    if(pressure_bytes > c->pressure_peak_bytes) c->pressure_peak_bytes = pressure_bytes;
    c->last_sample_ns = now_ns;
    c->sampled = true;
}
static void nvm_sample(struct db *db, uint64_t now_ns) {
    pthread_mutex_lock(&g_mutex_lock_wbs);
    nvm_sample_locked(db, now_ns);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
}

static void nvm_reset_stats_locked(struct db *db, uint64_t now_ns) {
    struct nvm_flush_control *c = &db->nvm_flush;
    c->sampled = false;
    c->peak_bytes = c->pressure_changes = c->pressure_peak_bytes = 0;
    if(db->write_buffer) db->write_buffer->pressure_changes = 0;


    nvm_sample_locked(db, now_ns);
}

void space_stats_nvm_start_at(struct db *db, uint64_t now_ns) {
    if(!db || !LIBDB_DYNAMIC_FLUSH_ENABLED) return;
    pthread_mutex_lock(&g_mutex_lock_wbs);
    if(db->write_buffer) db->write_buffer->pressure_tracking = true;
    nvm_reset_stats_locked(db, now_ns);
    atomic_store_explicit(&db->nvm_flush.phase, NVM_FLUSH_ACTIVE, memory_order_release);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
}

void space_stats_nvm_reset_stats_at(struct db *db, uint64_t now_ns) {
    if(!db || !LIBDB_DYNAMIC_FLUSH_ENABLED ||
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) != NVM_FLUSH_ACTIVE) return;
    pthread_mutex_lock(&g_mutex_lock_wbs);
    nvm_reset_stats_locked(db, now_ns);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
}

void space_stats_nvm_poll_at(struct db *db, uint64_t now_ns) {
    if(!db || !LIBDB_DYNAMIC_FLUSH_ENABLED ||
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) != NVM_FLUSH_ACTIVE) return;
    if(db->nvm_flush.sampled && now_ns - db->nvm_flush.last_sample_ns < LIBDB_NVM_FLUSH_POLL_NS) return;
    nvm_sample(db, now_ns);
}

void space_stats_nvm_start(struct db *db) { space_stats_nvm_start_at(db, monotonic_ns()); }
void space_stats_nvm_reset_stats(struct db *db) { space_stats_nvm_reset_stats_at(db, monotonic_ns()); }
void space_stats_nvm_poll(struct db *db) {
    if(db && LIBDB_DYNAMIC_FLUSH_ENABLED &&
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) == NVM_FLUSH_ACTIVE)
        space_stats_nvm_poll_at(db, monotonic_ns());
}
void space_stats_nvm_stop(struct db *db) {
    if(db && LIBDB_DYNAMIC_FLUSH_ENABLED &&
       atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) == NVM_FLUSH_ACTIVE){
        pthread_mutex_lock(&g_mutex_lock_wbs);
        atomic_store_explicit(&db->nvm_flush.phase, NVM_FLUSH_STOPPED, memory_order_release);
        if(db->write_buffer){
            db->write_buffer->pressure_tracking = false;
            db->nvm_flush.pressure_changes = db->write_buffer->pressure_changes;
        }
        pthread_mutex_unlock(&g_mutex_lock_wbs);
    }
}
