/* Real orchestration and NVM control, fake injection/reads: never opens hardware. */
#include "client.h"
#include "hist.h"
#include "space_stats.h"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

int test_get_num, test_insert_num, test_update_num, test_scan_num;
struct db *client_monitor_db;
static struct db *expected_db;
static int scenario, injections, reads, histogram_resets, gc_waits;

void __wrap_space_stats_gc_wait(struct db *db, unsigned seconds)
{
    assert(db == expected_db && seconds == 10);
    gc_waits++;
}
void __wrap_space_stats_gc_poll(struct db *db) { assert(db == expected_db); }
void histogram_reset_all(int threads) { assert(threads == 8); histogram_resets++; }

struct user_rq *client_read_production_request(char *path, int *count)
{
    (void)path; (void)count;
    assert(!"unexpected production reader");
    return NULL;
}
struct user_rq *client_read_request(char *path, int *count, int size)
{
    assert(client_monitor_db == expected_db && size == 1004);
    const bool warm = !strcmp(path, "warm");
    const bool pre = !strcmp(path, "uniform_A");
    if (!warm) {
        assert(!test_get_num && !test_update_num && !test_insert_num && !test_scan_num);
        assert(gc_waits == 2); /* No extra warmup/wait inserted between A and E. */
    }
    reads++;
    if (pre && scenario == 2) { *count = 0; return NULL; }
    *count = warm ? 2000 : 1000;
    struct user_rq *requests = calloc(*count, sizeof(*requests));
    assert(requests);
    for (int i = 0; i < *count; i++) {
        requests[i].key = i + 1;
        requests[i].size = size;
        requests[i].type = warm || pre ? 1 : 2;
    }
    if (warm) test_insert_num = *count;
    else if (pre) test_get_num = test_update_num = *count / 2;
    else test_scan_num = *count;
    if (pre && scenario == 4) test_scan_num = 1;
    return requests;
}

bool client_initialize_load_injectors(struct db *db, struct user_rq *requests, int count)
{
    assert(db == expected_db && requests);
    injections++;
    const char *phase = injections == 1 ? "load" : injections == 2 ? "warmup" :
        (scenario != 0 && injections == 3 ? "precondition" : "measured");
    assert(db->benchmark_phase && !strcmp(db->benchmark_phase, phase));
    printf("inject stage=%d\n", injections);
    fflush(stdout);
    struct write_buffer *wb = db->write_buffer;
    if (injections <= 2) {
        assert(atomic_load(&db->nvm_flush.phase) == NVM_FLUSH_FIXED);
        assert(histogram_resets == 0);
        if (injections == 1) {
            assert(count == 2000 && requests[0].type == 1);
            atomic_store(&wb->used_slot_bytes, 777);
            /* Pressure set by the preceding database work must survive. */
            atomic_store(&wb->pressure, true);
        } else assert(count == 1000 && requests[0].type == 0);
    } else {
        assert(count == 1000);
        assert(atomic_load(&db->nvm_flush.phase) == NVM_FLUSH_WORKLOAD);
        assert(wb->pressure_tracking && atomic_load(&wb->pressure));
        assert(db->nvm_flush.sampled && db->nvm_flush.pressure_changes == 0);
        assert(db->nvm_flush.peak_bytes == 777);
        assert(histogram_resets == injections - 2);
        if (scenario != 0 && injections == 3) {
            assert(test_get_num == 500 && test_update_num == 500 && !test_scan_num);
            if (scenario == 3) return false;
            /* Detect failure to reset per-phase statistics before measured E. */
            db->nvm_flush.peak_bytes = 9999;
            db->nvm_flush.pressure_changes = wb->pressure_changes = 9;
        } else {
            assert(test_scan_num == 1000 && !test_get_num && !test_update_num);
            assert(atomic_load(&wb->used_slot_bytes) == 777);
        }
        space_stats_nvm_stop(db); /* Same completion transition as real injector. */
    }
    return true;
}

static void exercise(int mode)
{
    scenario = mode;
    struct write_buffer wb = {.file_bytes = LIBDB_WBF_CAPACITY_LIMIT_BYTES};
    struct db db = {.write_buffer = &wb, .load_injectors = 8, .size_value = 1004};
    expected_db = &db;
    db.output_fp = tmpfile();
    assert(db.output_fp);
    assert(!pthread_mutex_init(&g_mutex_lock_wbs, NULL));
    atomic_store(&wb.reusable_slot_bytes, wb.file_bytes -
                 (LIBDB_NVM_FLUSH_LOW_BYTES + LIBDB_NVM_FLUSH_HIGH_BYTES) / 2);
    space_stats_nvm_init(&db);
    assert(!libdb_config_set_warm_path("warm"));
    assert(!libdb_config_set_test_path("E"));
    assert(!libdb_config_set_precondition_path(mode ? "uniform_A" : ""));
    client_executor(&db);
    assert(mode <= 1); /* Failures must terminate before returning. */
    assert(injections == (mode ? 4 : 3) && reads == (mode ? 3 : 2));
    assert(benchmark_completed && client_monitor_db == NULL);
    assert(atomic_load(&db.nvm_flush.phase) == NVM_FLUSH_STOPPED);
    assert(!wb.pressure_tracking && atomic_load(&wb.pressure));
    rewind(db.output_fp);
    char text[8192] = {0};
    assert(fread(text, 1, sizeof(text) - 1, db.output_fp) > 0);
    char *load = strstr(text, "begin load"), *warm = strstr(text, "begin warmup");
    char *pre = strstr(text, "begin precondition"), *test = strstr(text, "begin measured");
    assert(load && warm && test && load < warm && warm < test);
    if (mode) assert(pre && warm < pre && pre < test);
    else assert(!pre);
    fclose(db.output_fp);
}

int main(void)
{
    for (int mode = 0; mode <= 4; mode++) {
        FILE *capture = tmpfile();
        assert(capture);
        fflush(NULL);
        pid_t pid = fork();
        assert(pid >= 0);
        if (!pid) {
            assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
            exercise(mode);
            exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
        assert(WEXITSTATUS(status) == (mode <= 1 ? 0 : EXIT_FAILURE));
        rewind(capture);
        char text[8192] = {0};
        assert(fread(text, 1, sizeof(text) - 1, capture) > 0);
        if (mode >= 2) assert(!strstr(text, "begin measured") && !strstr(text, "inject stage=4"));
        fclose(capture);
    }
    puts("client_workload: all scenarios passed");
    return 0;
}
