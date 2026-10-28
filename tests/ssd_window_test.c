/* Exercise the actual snapshot/report code without opening an SSD. Unused
 * client functions are discarded at link time, as in other mock tests. */
#include "../client.c"
#include <assert.h>

static void *complete_writes(void *arg)
{
    struct ssd_write_stats *stats = arg;
    for (int i = 0; i < 10000; i++)
        for (int k = 0; k < SSD_WRITE_KINDS; k++)
            ssd_write_completed(stats, k, BLOCK_SIZE);
    return NULL;
}

int main(void)
{
    struct thread workers[2] = {0};
    struct db db = {.workers = workers, .thread = 2, .size_kv = 1024,
                    .benchmark_phase = "measured"};
    for (int i = 0; i < 2; i++) ssd_write_stats_init(&workers[i].ssd_writes);
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, complete_writes, &workers[0].ssd_writes));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    uint64_t before[SSD_WRITE_KINDS], after[SSD_WRITE_KINDS];
    snapshot_ssd_writes(&db, before);
    for (int k = 0; k < SSD_WRITE_KINDS; k++) {
        assert(before[k] == UINT64_C(40000) * BLOCK_SIZE);
        ssd_write_completed(&workers[1].ssd_writes, k, (k + 1) * BLOCK_SIZE);
    }
    snapshot_ssd_writes(&db, after);
    /* Completion after the end snapshot must not affect the report. */
    ssd_write_completed(&workers[1].ssd_writes, SSD_WRITE_GC, 999 * BLOCK_SIZE);
    test_update_num = 400; test_get_num = 600;
    db.output_fp = tmpfile(); assert(db.output_fp);
    report_ssd_window(&db, 1000, 2.5, before, after);
    rewind(db.output_fp);
    char line[1024]; assert(fgets(line, sizeof(line), db.output_fp));
    assert(strstr(line, "\"phase\":\"measured\""));
    assert(strstr(line, "\"updates\":400,\"reads\":600"));
    assert(strstr(line, "\"seconds\":2.500000000"));
    assert(strstr(line, "\"flush_bytes\":4096,\"gc_bytes\":8192,\"split_bytes\":12288,\"merge_bytes\":16384"));
    assert(!fgets(line, sizeof(line), db.output_fp));
    fclose(db.output_fp);
    return 0;
}
