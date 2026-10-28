#ifndef LIBDB_SPACE_STATS_H
#define LIBDB_SPACE_STATS_H
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
struct db;
struct user_rq;
struct space_snapshot { uint64_t used_blocks, used_bytes; };
/* Requires all workers to have stopped. Does not force a flush. */
const char *space_stats_snapshot(const struct db *, struct space_snapshot *);
void space_stats_print(const struct db *, FILE *);
/* Main freezes record_bytes after all load results arrive, before cache warmup. */
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
enum nvm_flush_phase { NVM_FLUSH_FIXED, NVM_FLUSH_WORKLOAD, NVM_FLUSH_STOPPED };
struct nvm_flush_control {
    atomic_int phase;
    atomic_bool pressure; /* 0: defer, 1: flush eligible groups. */
    bool sampled;
    uint64_t last_sample_ns, sampled_bytes, peak_bytes, pressure_changes;
};
void space_stats_nvm_init(struct db *);
void space_stats_nvm_start_at(struct db *, uint64_t now_ns);
void space_stats_nvm_start(struct db *);
void space_stats_nvm_stop(struct db *);
void space_stats_nvm_poll_at(struct db *, uint64_t now_ns);
void space_stats_nvm_poll(struct db *);
bool space_stats_nvm_flush_allowed(const struct db *);
void space_stats_nvm_print(const struct db *, FILE *);
/* Main-thread control, worker-readable threshold. now_ns supports fake clocks. */
void space_stats_gc_poll_at(struct db *, uint64_t now_ns);
void space_stats_gc_poll(struct db *);
void space_stats_gc_wait(struct db *, unsigned seconds);
double space_stats_gc_fraction(const struct db *);
const char *space_stats_load_bytes(const struct user_rq *, int, uint64_t *);
#endif
