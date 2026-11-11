#include "insert.h"
#include "phase_io.h"
#include <inttypes.h>

void phase_io_snapshot(const struct db *db, struct phase_io_snapshot *out) {
    *out = (struct phase_io_snapshot){0};
    for(int w = 0; w < db->thread; w++)
        for(int i = 0; i < PHASE_IO_COUNT; i++)
            out->value[i] += atomic_load_explicit(&db->workers[w].phase_io.value[i], memory_order_relaxed);
}
void phase_io_finish(const struct db *db, const struct phase_io_snapshot *start,
                     int requests, uint64_t logical_bytes) {
    struct phase_io_snapshot end;

    phase_io_snapshot(db, &end);
    for(int i = 0; i < PHASE_IO_COUNT; i++) end.value[i] -= start->value[i];
    uint64_t data_bytes = (end.value[PHASE_FLUSH] + end.value[PHASE_SPLIT] +
                           end.value[PHASE_GC] + end.value[PHASE_MERGE]) * BLOCK_SIZE;
    uint64_t wal_bytes = end.value[PHASE_WAL_BYTES];
    uint64_t total_bytes = data_bytes + wal_bytes;
    FILE *streams[] = {stdout, db->output_fp};
    for(int i = 0; i < 2; i++) {
        FILE *fp = streams[i];
        if(!fp || (i && fp == stdout)) continue;
        fprintf(fp, "PHASE_IO version=1 phase=%s requests=%d logical_bytes=%" PRIu64
                " flush_blocks=%" PRIu64 " split_blocks=%" PRIu64 " gc_blocks=%" PRIu64
                " merge_blocks=%" PRIu64 " wal_bytes=%" PRIu64 " scan_ssd_keys=%" PRIu64
                " scan_read_blocks=%" PRIu64 " block_bytes=%u boundary=requests_complete drain=0 force_flush=0\n",
                db->phase_name ? db->phase_name : "test", requests, logical_bytes,
                end.value[PHASE_FLUSH], end.value[PHASE_SPLIT], end.value[PHASE_GC], end.value[PHASE_MERGE],
                end.value[PHASE_WAL_BYTES], end.value[PHASE_SCAN_KEYS], end.value[PHASE_SCAN_BLOCKS], BLOCK_SIZE);
        fprintf(fp, "PHASE_WA version=1 phase=%s requests=%d logical_bytes=%" PRIu64
                " data_write_bytes=%" PRIu64 " wal_bytes=%" PRIu64
                " total_write_bytes=%" PRIu64,
                db->phase_name ? db->phase_name : "test", requests, logical_bytes,
                data_bytes, wal_bytes, total_bytes);
        if(logical_bytes)
            fprintf(fp, " wa_data=%.9f wa_wal=%.9f wa_total=%.9f",
                    (double)data_bytes / logical_bytes, (double)wal_bytes / logical_bytes,
                    (double)total_bytes / logical_bytes);
        else
            fprintf(fp, " wa_data=N/A wa_wal=N/A wa_total=N/A");
        fprintf(fp, " boundary=requests_complete drain=0 force_flush=0\n");
        fflush(fp);
    }
}
