#include "space_stats.h"
#include "insert.h"
#include "kv_record.h"
#include <inttypes.h>
#include <time.h>

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

    uint64_t dram = 0, slots = 0;
    for(int i = 0; i < db->thread; i++) {
        uint64_t bytes = write_buffer_live_bytes(&db->workers[i].kvbuffer);
        if(dram > UINT64_MAX - bytes) return "dram_bytes_overflow";
        dram += bytes;
        uint64_t charged = write_buffer_slot_bytes(&db->workers[i].kvbuffer);
        if(slots > UINT64_MAX - charged) return "dram_slot_bytes_overflow";
        slots += charged;
    }
    out->used_blocks = used;
    out->used_bytes = used * BLOCK_SIZE;
    out->dram_bytes = dram;
    out->dram_slot_bytes = slots;
    return NULL;
}

void space_stats_print(const struct db *db, FILE *fp) {
    if(!fp) return;
    struct space_snapshot snapshot;
    const char *snapshot_error = space_stats_snapshot(db, &snapshot);
    const char *error = db->initial_load.error;
    if(!error && !db->initial_load.complete) error = "load_not_completed";
    if(!error && !db->initial_load.record_bytes) error = "zero_initial_load_bytes";
    if(!error) error = snapshot_error;
    fprintf(fp, "SSD space: exclude_wal=1 force_flush=0 denominator=initial_load_input occupancy=gmap_nonzero\n");
    if(db->initial_load.error || !db->initial_load.complete)
        fprintf(fp, "initial_load_record_bytes=N/A\n");
    else
        fprintf(fp, "initial_load_record_bytes=%" PRIu64 "\n", db->initial_load.record_bytes);
    if(snapshot_error)
        fprintf(fp, "ssd_data_used_blocks=N/A ssd_data_used_bytes=N/A snapshot_reason=%s\n", snapshot_error);
    else
        fprintf(fp, "ssd_data_used_blocks=%" PRIu64 " ssd_data_used_bytes=%" PRIu64
                    "\n", snapshot.used_blocks, snapshot.used_bytes);
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
    db->nvm_flush = (struct nvm_flush_control){0};
    wb_snapshot_store(&db->nvm_flush.published_control, (uint64_t)NVM_FLUSH_FIXED << 1);
}

bool space_stats_nvm_flush_allowed_for_size(const struct db *db, uint64_t buffered_size) {
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED || !db) return true;
    uint64_t control = wb_snapshot_load(&db->nvm_flush.published_control);
    enum nvm_flush_phase phase = (enum nvm_flush_phase)(control >> 1);
    return phase == NVM_FLUSH_FIXED || (phase == NVM_FLUSH_ACTIVE &&
        (buffered_size >= LIBDB_FLUSH_HARD_THRESHOLD_BYTES || (control & 1)));
}

bool space_stats_nvm_flush_allowed(const struct db *db) {
    return space_stats_nvm_flush_allowed_for_size(db, 0);
}



static uint64_t nvm_capacity_sample(const struct db *db) {
    if(!db || !db->workers) return 0;
    uint64_t per_worker[MAX_THREAD], bytes = 0;
    if(db->thread < 0 || db->thread > MAX_THREAD) abort();
    for(int i = 0; i < db->thread; i++) {
        per_worker[i] = write_buffer_slot_bytes(&db->workers[i].kvbuffer);
        bytes = per_worker[i] > UINT64_MAX - bytes ? UINT64_MAX : bytes + per_worker[i];
    }
    if(bytes >= LIBDB_WBF_CAPACITY_LIMIT_BYTES) {
        FILE *streams[] = {stderr, db->output_fp};
        for(int j = 0; j < 2; j++) {
            FILE *fp = streams[j];
            if(!fp || (j && fp == stderr)) continue;
            fprintf(fp, "write-buffer capacity limit exceeded: phase=%s\n",
                db->phase_name ? db->phase_name : "startup");
            fflush(fp);
        }
        exit(EXIT_FAILURE);
    }
    return bytes;
}

void space_stats_nvm_check_capacity(struct db *db) {
    (void)nvm_capacity_sample(db);
}

static void nvm_sample(struct db *db, uint64_t now_ns) {
    struct nvm_flush_control *c = &db->nvm_flush;
    uint64_t bytes = nvm_capacity_sample(db);
    c->capacity_last_sample_ns = now_ns;
    c->capacity_sampled = true;
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED || nvm_flush_phase_get(c) != NVM_FLUSH_ACTIVE) return;
    bool old = nvm_flush_pressure_get(c), pressure = old;
    if(bytes >= LIBDB_NVM_FLUSH_HIGH_BYTES) pressure = true;
    else if(bytes <= LIBDB_NVM_FLUSH_LOW_BYTES) pressure = false;
    if(old != pressure) c->pressure_changes++;
    nvm_flush_pressure_set(c, pressure);
    c->sampled_bytes = bytes;
    if(bytes > c->peak_bytes) c->peak_bytes = bytes;
    if(bytes > c->pressure_peak_bytes) c->pressure_peak_bytes = bytes;
    c->last_sample_ns = now_ns;
    c->sampled = true;
}

static void nvm_reset_stats(struct db *db, uint64_t now_ns) {
    struct nvm_flush_control *c = &db->nvm_flush;
    c->sampled = false;
    c->peak_bytes = c->pressure_changes = c->pressure_peak_bytes = 0;

    nvm_sample(db, now_ns);
}

void space_stats_nvm_start_at(struct db *db, uint64_t now_ns) {
    if(!db) return;
    if(LIBDB_DYNAMIC_FLUSH_ENABLED) nvm_flush_phase_set(&db->nvm_flush, NVM_FLUSH_ACTIVE);
    nvm_reset_stats(db, now_ns);
}

void space_stats_nvm_reset_stats_at(struct db *db, uint64_t now_ns) {
    if(!db) return;
    if(LIBDB_DYNAMIC_FLUSH_ENABLED && nvm_flush_phase_get(&db->nvm_flush) == NVM_FLUSH_ACTIVE)
        nvm_reset_stats(db, now_ns);
    else nvm_sample(db, now_ns);
}

void space_stats_nvm_poll_at(struct db *db, uint64_t now_ns) {
    if(!db || !db->workers) return;
    if(db->nvm_flush.capacity_sampled &&
       now_ns - db->nvm_flush.capacity_last_sample_ns < LIBDB_NVM_FLUSH_POLL_NS) return;
    nvm_sample(db, now_ns);
}

void space_stats_nvm_start(struct db *db) { space_stats_nvm_start_at(db, monotonic_ns()); }
void space_stats_nvm_reset_stats(struct db *db) { space_stats_nvm_reset_stats_at(db, monotonic_ns()); }
void space_stats_nvm_poll(struct db *db) {
    if(db) space_stats_nvm_poll_at(db, monotonic_ns());
}
void space_stats_nvm_stop(struct db *db) {
    if(!db) return;
    space_stats_nvm_check_capacity(db);
    if(LIBDB_DYNAMIC_FLUSH_ENABLED && nvm_flush_phase_get(&db->nvm_flush) == NVM_FLUSH_ACTIVE)
        nvm_flush_phase_set(&db->nvm_flush, NVM_FLUSH_STOPPED);
}
