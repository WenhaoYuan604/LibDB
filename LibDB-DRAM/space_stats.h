#ifndef LIBDB_SPACE_STATS_H
#define LIBDB_SPACE_STATS_H
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "wb_snapshot.h"
struct db;
struct user_rq;
struct space_snapshot {
    uint64_t used_blocks, used_bytes, dram_bytes, dram_slot_bytes;
};

struct initial_load_stats {
    uint64_t record_bytes;
    bool complete;
    const char *error;
};
struct gc_space_control {
    atomic_bool relaxed;
    bool sampled;
    uint64_t last_sample_ns;
};
enum nvm_flush_phase { NVM_FLUSH_FIXED, NVM_FLUSH_ACTIVE, NVM_FLUSH_STOPPED };
struct nvm_flush_control {

    _Alignas(8) uint64_t published_control;
    bool capacity_sampled;
    uint64_t capacity_last_sample_ns;
    bool sampled;
    uint64_t last_sample_ns, sampled_bytes, peak_bytes, pressure_changes;
    uint64_t pressure_peak_bytes;
};
static inline enum nvm_flush_phase nvm_flush_phase_get(const struct nvm_flush_control *c) {
    return (enum nvm_flush_phase)(wb_snapshot_load(&c->published_control) >> 1);
}
static inline bool nvm_flush_pressure_get(const struct nvm_flush_control *c) {
    return (wb_snapshot_load(&c->published_control) & 1) != 0;
}

static inline void nvm_flush_phase_set(struct nvm_flush_control *c, enum nvm_flush_phase phase) {
    wb_snapshot_store(&c->published_control, ((uint64_t)phase << 1) |
                      (wb_snapshot_load(&c->published_control) & 1));
}
static inline void nvm_flush_pressure_set(struct nvm_flush_control *c, bool pressure) {
    wb_snapshot_store(&c->published_control,
                      (wb_snapshot_load(&c->published_control) & ~UINT64_C(1)) | pressure);
}

void space_stats_nvm_check_capacity(struct db *);
void space_stats_nvm_init(struct db *);
void space_stats_nvm_start_at(struct db *, uint64_t now_ns);
void space_stats_nvm_start(struct db *);

void space_stats_nvm_reset_stats_at(struct db *, uint64_t now_ns);
void space_stats_nvm_reset_stats(struct db *);
void space_stats_nvm_stop(struct db *);
void space_stats_nvm_poll_at(struct db *, uint64_t now_ns);
void space_stats_nvm_poll(struct db *);
bool space_stats_nvm_flush_allowed(const struct db *);

bool space_stats_nvm_flush_allowed_for_size(const struct db *, uint64_t buffered_size);

void space_stats_gc_poll_at(struct db *, uint64_t now_ns);
void space_stats_gc_poll(struct db *);
void space_stats_gc_wait(struct db *, unsigned seconds);
double space_stats_gc_fraction(const struct db *);
const char *space_stats_load_bytes(const struct user_rq *, int, uint64_t *);


const char *space_stats_snapshot(const struct db *, struct space_snapshot *);
void space_stats_print(const struct db *, FILE *);
#endif
