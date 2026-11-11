#ifndef LIBDB_PHASE_IO_H
#define LIBDB_PHASE_IO_H
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
enum phase_io_metric {
    PHASE_FLUSH, PHASE_SPLIT, PHASE_GC, PHASE_MERGE,
    PHASE_WAL_BYTES, PHASE_SCAN_KEYS, PHASE_SCAN_BLOCKS, PHASE_IO_COUNT
};

struct phase_io_counters {
    bool enabled;
    _Atomic uint64_t value[PHASE_IO_COUNT];
};
struct phase_io_snapshot { uint64_t value[PHASE_IO_COUNT]; };
static inline void phase_io_init(struct phase_io_counters *stats, bool enabled) {
    stats->enabled = enabled;
    for(int i = 0; i < PHASE_IO_COUNT; i++) atomic_init(&stats->value[i], 0);
}
static inline void phase_io_add(struct phase_io_counters *stats, enum phase_io_metric kind, uint64_t amount) {
    if(stats->enabled) atomic_fetch_add_explicit(&stats->value[kind], amount, memory_order_relaxed);
}
struct db;
void phase_io_snapshot(const struct db *, struct phase_io_snapshot *);
void phase_io_finish(const struct db *, const struct phase_io_snapshot *, int requests, uint64_t logical_bytes);
#endif
