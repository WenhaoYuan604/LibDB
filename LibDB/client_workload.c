#define _GNU_SOURCE


#include "client.h"
#include "hist.h"
#include "space_stats.h"

static void phase_marker(struct db *db, const char *event, const char *phase)
{
    db->benchmark_phase = strcmp(event, "begin") == 0 ? phase : NULL;
    fprintf(stdout, "Benchmark phase: %s %s\n", event, phase);
    fflush(stdout);
    if (db->output_fp && db->output_fp != stdout) {
        fprintf(db->output_fp, "Benchmark phase: %s %s\n", event, phase);
        fflush(db->output_fp);
    }
}

static void reset_operations(void)
{
    test_get_num = test_insert_num = test_update_num = test_scan_num = 0;
}

static void execute_trace(struct db *db, char *path, const char *phase, bool production)
{
    int count = 0;
    phase_marker(db, "begin", phase);
    reset_operations();
    struct user_rq *requests = production ? client_read_production_request(path, &count) :
        client_read_request(path, &count, db->size_value);
    if (!requests || count <= 0) {
        fprintf(stderr, "failed to load %s workload: %s\n", phase, path);
        free(requests);
        exit(EXIT_FAILURE);
    }
    if (strcmp(phase, "ycsbf") == 0) {

        int parsed = test_get_num + test_update_num + test_insert_num + test_scan_num;
        if (test_insert_num || test_scan_num || parsed < count || parsed - count >= 1000)
            { fprintf(stderr, "invalid YCSB F operation counts\n"); exit(EXIT_FAILURE); }
        for (int i = count; i < parsed; i++) {
            if (requests[i].type == 0) test_get_num--;
            else if (requests[i].type == 1) test_update_num--;
            else { fprintf(stderr, "invalid YCSB F tail\n"); exit(EXIT_FAILURE); }
        }
    }
    if ((strcmp(phase, "precondition") == 0 || strcmp(phase, "ycsbf") == 0) &&
        (test_insert_num || test_scan_num ||
         (uint64_t)test_get_num + (uint64_t)test_update_num != (uint64_t)count)) {
        fprintf(stderr, "%s must contain READ/UPDATE requests matching the executed count\n", phase);
        free(requests);
        exit(EXIT_FAILURE);
    }
    int scan_length = !strcmp(phase, "measured_e10") ? 10 :
        !strcmp(phase, "measured_e50") ? 50 : !strcmp(phase, "measured_e100") ? 100 : 0;
    if (scan_length) {
        if (test_get_num || test_update_num || !test_scan_num ||
            (uint64_t)test_insert_num + test_scan_num != (uint64_t)count) {
            fprintf(stderr, "invalid scan suite operation counts\n"); exit(EXIT_FAILURE);
        }
        for (int i = 0; i < count; i++) if (requests[i].type == 2 && requests[i].size != scan_length) {
            fprintf(stderr, "%s requires scan length %d\n", phase, scan_length); exit(EXIT_FAILURE);
        }
    }
    histogram_reset_all(db->load_injectors);


    if (atomic_load_explicit(&db->nvm_flush.phase, memory_order_relaxed) == NVM_FLUSH_FIXED)
        space_stats_nvm_start(db);
    else
        space_stats_nvm_reset_stats(db);
    if (!client_initialize_load_injectors(db, requests, count)) {
        fprintf(stderr, "%s workload failed\n", phase);
        exit(EXIT_FAILURE);
    }
    if (client_phase_is_final(db)) space_stats_nvm_stop(db);
    free(requests);
    phase_marker(db, "end", phase);
}

void client_executor(struct db *db)
{
    client_monitor_db = db;
    int warm_num = 0;
    const bool suite = g_libdb_config.post_load_path[0] != 0;
    bool production = strstr(g_libdb_config.test_path, "Production") != NULL;
    struct user_rq *warm = production ?
        client_read_production_request(g_libdb_config.warm_path, &warm_num) :
        client_read_request(g_libdb_config.warm_path, &warm_num, db->size_value);
    if (!warm || warm_num <= 0) {
        fprintf(stderr, "failed to load warm workload: %s\n", g_libdb_config.warm_path);
        free(warm);
        exit(EXIT_FAILURE);
    }
    int cache_key_num = production ? warm_num / 10 : warm_num / 2;
    if (!db->recovery_mode) {
        phase_marker(db, "begin", "load");
        printf("start to load\n");
        fflush(stdout);
        db->initial_load.complete = client_initialize_load_injectors(db, warm, warm_num);
        if (!db->initial_load.complete) {
            fprintf(stderr, "initial load failed; stopping benchmark\n");
            exit(EXIT_FAILURE);
        }
        db->initial_load.error = space_stats_load_bytes(warm, warm_num, &db->initial_load.record_bytes);
        space_stats_gc_poll(db);
        space_stats_gc_wait(db, 10);
        phase_marker(db, "end", "load");
    } else {
        printf("recovery mode: skip load, keep cache warmup\n");
        fflush(stdout);
    }
    if (suite) {
        free(warm);
        execute_trace(db, g_libdb_config.post_load_path, "ycsbf", false);
        phase_marker(db, "begin", "warmup");
        reset_operations();
        int reloaded = 0;
        warm = client_read_request(g_libdb_config.warm_path, &reloaded, db->size_value);
        if (!warm || reloaded != warm_num) {
            fprintf(stderr, "warm trace changed or could not be reloaded\n"); exit(EXIT_FAILURE);
        }
    }
    if (!suite) phase_marker(db, "begin", "warmup");
    if (suite) {
        reset_operations();
        test_get_num = cache_key_num;
        histogram_reset_all(db->load_injectors);
        space_stats_nvm_reset_stats(db);
    }
    for (int i = warm_num - cache_key_num; i < warm_num; i++)
        warm[i].type = 0;
    if (cache_key_num > 0 &&
        !client_initialize_load_injectors(db, &warm[warm_num - cache_key_num], cache_key_num))
        exit(EXIT_FAILURE);
    phase_marker(db, "end", "warmup");
    free(warm);
    space_stats_gc_wait(db, 10);

    if (g_libdb_config.precondition_path[0])
        execute_trace(db, g_libdb_config.precondition_path, "precondition", false);
    if (suite) {
        execute_trace(db, g_libdb_config.test_path, "measured_e10", false);
        execute_trace(db, g_libdb_config.test_e50_path, "measured_e50", false);
        execute_trace(db, g_libdb_config.test_e100_path, "measured_e100", false);
    } else {
        execute_trace(db, g_libdb_config.test_path, "measured", production);
    }
    fflush(stdout);
    if (db->output_fp) fflush(db->output_fp);
    benchmark_completed = 1;
    client_monitor_db = NULL;
}
