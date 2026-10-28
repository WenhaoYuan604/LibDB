/* Exercise actual main-thread polling and reader loops with a fake clock.
 * Including client.c exposes its private main-thread reader context only here. */
#include "../client.c"
#include <assert.h>
#include <time.h>

static uint64_t fake_now, clock_step;
static bool polling, nvm_polling;
static uint64_t nvm_start;
static unsigned sleep_calls;
static struct db *observed;
uint64_t spdk_get_ticks_hz(void) { return 1000000; }
uint64_t spdk_get_ticks(void) { return fake_now / 1000; }
int __wrap_clock_gettime(clockid_t clock_id, struct timespec *out) {
    assert(clock_id == CLOCK_MONOTONIC);
    fake_now += clock_step;
    out->tv_sec = fake_now / UINT64_C(1000000000);
    out->tv_nsec = fake_now % UINT64_C(1000000000);
    if(nvm_polling && fake_now >= nvm_start + 3 * LIBDB_NVM_FLUSH_POLL_NS) {
        assert(observed->nvm_flush.last_sample_ns >= nvm_start + 2 * LIBDB_NVM_FLUSH_POLL_NS);
        load_injector_completed[0] = 1;
    }
    if(polling && fake_now >= 3 * LIBDB_GC_POLL_NS) {
        assert(observed->gc_space.last_sample_ns >= 2 * LIBDB_GC_POLL_NS);
        load_injector_completed[0] = 1;
    }
    return 0;
}
int __wrap_nanosleep(const struct timespec *duration, struct timespec *remaining) {
    (void)remaining;
    uint64_t delay = duration->tv_sec * UINT64_C(1000000000) + duration->tv_nsec;
    assert(delay > 0 && delay <= LIBDB_GC_POLL_NS);
    fake_now += delay;
    sleep_calls++;
    return 0;
}

int main(void) {
    struct db db = {0};
    uint_fast8_t map[16] = {0};
    g_map = map;
    g_block_num = db.data_blocks = 16;
    db.load_injectors = 1;
    db.initial_load.complete = true;
    db.initial_load.record_bytes = 4 * BLOCK_SIZE;
    db.output_fp = tmpfile(); assert(db.output_fp);
    observed = &db;
    uint64_t completed = 0;
    load_injector_completed = &completed;
    polling = true;
    clock_step = LIBDB_GC_POLL_NS / 2;
    client_polling_completed(&db, 1);
    assert(test_completed && db.gc_space.sampled);
    polling = false;
    clock_step = 0;
    uint64_t start = fake_now;
    space_stats_gc_wait(&db, 10);
    assert(fake_now - start == UINT64_C(10000000000));
    assert(sleep_calls == (UINT64_C(10000000000) + LIBDB_GC_POLL_NS - 1) / LIBDB_GC_POLL_NS);
    assert(fake_now - db.gc_space.last_sample_ns < LIBDB_GC_POLL_NS);

    char path[] = "/tmp/libdb-gc-reader-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *fp = fdopen(fd, "w"); assert(fp);
    for(int i = 0; i < 4096; i++) fprintf(fp, "I user%d value 100\n", i + 1);
    fclose(fp);
    client_monitor_db = &db;
    clock_step = LIBDB_GC_POLL_NS;
    start = db.gc_space.last_sample_ns;
    int count;
    struct user_rq *requests = client_read_production_request(path, &count);
    assert(requests && count == 4000);
    assert(db.gc_space.last_sample_ns >= start + 4 * LIBDB_GC_POLL_NS);
    free(requests);
    fp = fopen(path, "w"); assert(fp);
    for(int i = 0; i < 4096; i++) fprintf(fp, "I user%d value\n", i + 1);
    fclose(fp);
    start = db.gc_space.last_sample_ns;
    requests = client_read_request(path, &count, 100);
    assert(requests && count == 4000);
    assert(db.gc_space.last_sample_ns >= start + 4 * LIBDB_GC_POLL_NS);
    free(requests);
    /* Run the actual main completion loop with dynamic flush active while
     * GC is inactive (recovery skips initial load). */
    struct write_buffer wb = {0};
    db.write_buffer = &wb;
    wb.file_bytes = LIBDB_NVM_FLUSH_HIGH_BYTES;
    assert(pthread_mutex_init(&g_mutex_lock_wbs, NULL) == 0);
    db.initial_load.complete = false;
    db.recovery_mode = 1;
    atomic_store(&wb.used_slot_bytes, LIBDB_NVM_FLUSH_HIGH_BYTES);
    space_stats_nvm_init(&db);
    nvm_start = fake_now;
    space_stats_nvm_start_at(&db, nvm_start);
    assert(space_stats_nvm_flush_allowed(&db));
    completed = 0; test_completed = 0;
    clock_step = LIBDB_NVM_FLUSH_POLL_NS / 2;
    nvm_polling = true;
    client_polling_completed(&db, 1);
    nvm_polling = false;
    assert(test_completed && db.nvm_flush.sampled);
    assert(atomic_load(&db.nvm_flush.phase) == NVM_FLUSH_STOPPED);
    assert(!space_stats_nvm_flush_allowed(&db));
    client_monitor_db = NULL;
    unlink(path);
    fclose(db.output_fp);
    puts("PASS main completion polling, timed waits and both readers with fake time");
    return 0;
}
