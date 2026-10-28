#ifndef LIBDB_SSD_WRITE_STATS_H
#define LIBDB_SSD_WRITE_STATS_H
#include <stdatomic.h>
#include <stdint.h>
/* Successful SSD write completions, never reset by phase reporting. Each
 * worker owns its counters; the benchmark thread takes atomic snapshots.
 * Components are sampled sequentially, not with a global I/O barrier. */
enum ssd_write_kind { SSD_WRITE_FLUSH, SSD_WRITE_GC, SSD_WRITE_SPLIT,
                      SSD_WRITE_MERGE, SSD_WRITE_KINDS };
struct ssd_write_stats { _Atomic uint64_t bytes[SSD_WRITE_KINDS]; };
static inline void ssd_write_stats_init(struct ssd_write_stats *stats)
{
    for (int i = 0; i < SSD_WRITE_KINDS; i++) atomic_init(&stats->bytes[i], 0);
}
static inline void ssd_write_completed(struct ssd_write_stats *stats,
                                       enum ssd_write_kind kind, uint64_t bytes)
{
    atomic_fetch_add_explicit(&stats->bytes[kind], bytes, memory_order_relaxed);
}
#endif
