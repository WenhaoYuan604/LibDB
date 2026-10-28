#include "insert.h"
#include "kv_record.h"
#include <assert.h>
static void dynamic_gc_test(void) {
    struct db db = {0};
    uint_fast8_t map[8] = {0};
    g_map = map;
    g_block_num = db.data_blocks = 8;
    atomic_store(&g_used_data_blocks, 0);
    db.initial_load.record_bytes = 4 * BLOCK_SIZE;
    space_stats_gc_poll_at(&db, 0);
    assert(!db.gc_space.sampled);
    assert(space_stats_gc_fraction(&db) == LIBDB_SUBSPACE_REDUNDANT_FRACTION);
    db.initial_load.complete = true;
    for(int i = 0; i < 5; i++) block_ref_add(i);
    space_stats_gc_poll_at(&db, 0); /* Amplification 1.25 is below the current limit. */
    assert(space_stats_gc_fraction(&db) == LIBDB_GC_RELAXED_THRESHOLD / (1.0 + LIBDB_GC_RELAXED_THRESHOLD));
    block_ref_add(5);
    space_stats_gc_poll_at(&db, LIBDB_GC_POLL_NS - 1);
    assert(atomic_load(&db.gc_space.relaxed));
    space_stats_gc_poll_at(&db, LIBDB_GC_POLL_NS);
    assert(space_stats_gc_fraction(&db) == LIBDB_SUBSPACE_REDUNDANT_FRACTION);
    block_ref_remove(5);
    block_ref_remove(4);
    space_stats_gc_poll_at(&db, 2 * LIBDB_GC_POLL_NS);
    assert(atomic_load(&db.gc_space.relaxed));
    db.initial_load.record_bytes = 0;
    space_stats_gc_poll_at(&db, 3 * LIBDB_GC_POLL_NS);
    assert(!atomic_load(&db.gc_space.relaxed));
    db.initial_load.record_bytes = 4 * BLOCK_SIZE;
    space_stats_gc_poll_at(&db, 4 * LIBDB_GC_POLL_NS);
    assert(atomic_load(&db.gc_space.relaxed));
    g_block_num--;
    space_stats_gc_poll_at(&db, 5 * LIBDB_GC_POLL_NS);
    assert(!atomic_load(&db.gc_space.relaxed));
    g_block_num++;
    db.initial_load.error = "load_failed";
    space_stats_gc_poll_at(&db, 6 * LIBDB_GC_POLL_NS);
    assert(!atomic_load(&db.gc_space.relaxed));
    db.initial_load.error = NULL;
    db.initial_load.complete = false;
    db.recovery_mode = 1;
    space_stats_gc_poll_at(&db, 7 * LIBDB_GC_POLL_NS);
    assert(space_stats_gc_fraction(&db) == LIBDB_SUBSPACE_REDUNDANT_FRACTION);
    g_map = NULL;
    atomic_store(&g_used_data_blocks, 0);
}
static void printed(struct db *db, char *text, size_t size) {
    FILE *fp = tmpfile(); assert(fp);
    space_stats_print(db, fp); rewind(fp);
    size_t n = fread(text, 1, size - 1, fp); text[n] = 0;
    fclose(fp);
}
static void snapshot_test(void) {
    struct db db = {0}; struct thread worker = {0};
    uint_fast8_t map[8] = {0, 3, 0, 1};
    db.thread = 1; db.workers = &worker;
    g_map = map; g_block_num = db.data_blocks = 8;
    db.initial_load.complete = true;
    db.initial_load.record_bytes = 4 * BLOCK_SIZE;
    block_ref_recount();
    struct space_snapshot snap;
    assert(!space_stats_snapshot(&db, &snap));
    assert(snap.used_blocks == 2 && snap.used_bytes == 2 * BLOCK_SIZE);
    char text[2048], again[2048];
    printed(&db, text, sizeof(text)); printed(&db, again, sizeof(again));
    assert(!strcmp(text, again));
    assert(strstr(text, "ssd_space_amplification=0.500000000"));
    assert(strstr(text, "online_counter_matches_snapshot=1"));
    assert(strstr(text, "force_flush=0"));
    assert(!strstr(text, "dram_write_buffer"));
    atomic_store(&g_used_data_blocks, 1);
    printed(&db, text, sizeof(text));
    assert(strstr(text, "online_counter_matches_snapshot=0"));
    db.initial_load.complete = false;
    printed(&db, text, sizeof(text)); assert(strstr(text, "reason=load_not_completed"));
    db.initial_load.complete = true; db.initial_load.record_bytes = 0;
    printed(&db, text, sizeof(text)); assert(strstr(text, "reason=zero_initial_load_bytes"));
    db.initial_load.record_bytes = BLOCK_SIZE; g_block_num--;
    printed(&db, text, sizeof(text)); assert(strstr(text, "reason=gmap_size_mismatch"));
    g_map = NULL;
    printed(&db, text, sizeof(text)); assert(strstr(text, "reason=gmap_unavailable"));
    printed(NULL, text, sizeof(text)); assert(strstr(text, "reason=database_unavailable"));
    atomic_store(&g_used_data_blocks, 0);
}
int main(void) {
    dynamic_gc_test();
    snapshot_test();
    uint64_t bytes = 99;
    assert(space_stats_load_bytes(NULL, 0, &bytes) && !bytes);
    struct user_rq load[] = {{.type=1,.size=100}, {.type=1,.size=200}, {.type=0,.size=100}};
    assert(!space_stats_load_bytes(load, 3, &bytes));
    assert(bytes == 300 + 2 * KV_RECORD_HEADER_SIZE);
    load[0].size = -1;
    assert(space_stats_load_bytes(load, 3, &bytes));
    uint_fast8_t map[8] = {0}; g_map = map; g_block_num = 8;
    block_ref_add(1); block_ref_add(1);
    assert(atomic_load(&g_used_data_blocks) == 1);
    block_ref_remove(1);
    assert(atomic_load(&g_used_data_blocks) == 1);
    block_ref_remove(1); block_ref_clear(1);
    assert(atomic_load(&g_used_data_blocks) == 0);
    map[2] = 4; map[5] = 1; block_ref_recount();
    assert(atomic_load(&g_used_data_blocks) == 2);
    block_ref_clear(2); block_ref_clear(2);
    assert(atomic_load(&g_used_data_blocks) == 1);
    return 0;
}
