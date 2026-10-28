/* Reuse the memory-NVMe harness, but link the production WBF allocator and
 * recovery scanner. Every case owns ordinary temporary PMDK files. */
#define LIBDB_TEST_REAL_WBF 1
#define main mock_suite_main
#include "merge_test.c"
#undef main
#include <sys/stat.h>

static void gc_real_restart_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 1);
    gc_stage_test(a); drain();
    CHECK(a->key_num == 4 && atomic_load(&test_wbf.used_slot_bytes) == 2048);
    uint64_t file_bytes = test_wbf.file_bytes;
    for(unsigned i = 0; i < test_wbf.file_num; i++) pmemobj_close(test_wbf.pool[i]);
    pmemobj_close(pool);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
    pool = pmemobj_open(pool_path, "merge-test"); CHECK(pool);
    persistent.pool[0] = pool;
    struct write_buffer *restored = write_buffer_open_existing(); CHECK(restored);
    test_wbf = *restored; free(restored);
    CHECK(test_wbf.file_bytes == file_bytes && atomic_load(&test_wbf.used_slot_bytes) == 2048);
    hybrid.dram_index = dram_index_initialize(NULL, 0);
    CHECK(recovery_rebuild_subspaces(&database));
    CHECK(recovery_rebuild_write_buffer_indices(&database));
    CHECK(recovery_verify_post(&database));
    a = dram_index_search(hybrid.dram_index, 5, NULL, NULL);
    CHECK(a && a->boundary == 0 && a->key_num == 4 && a->buffered_size == 2048);
    for(int k = 1; k <= 6; k++) check_get(k, 'a');
    struct write_buffer_index_node *n = write_buffer_index_search(a->buffer_index, 5);
    struct kv_record_view view;
    CHECK(n && !kv_record_decode(pmemobj_direct(n->id), n->size, &view) && view.version == 5);
    gc_check_layout(a);
}
static void gc_real_fill_restart_test(void){
    gc_tail_grow_slots_test();
    for(unsigned i = 0; i < test_wbf.file_num; i++) pmemobj_close(test_wbf.pool[i]);
    pmemobj_close(pool); pthread_mutex_destroy(&g_mutex_lock_wbs);
    pool = pmemobj_open(pool_path, "merge-test"); CHECK(pool);
    persistent.pool[0] = pool;
    struct write_buffer *restored = write_buffer_open_existing(); CHECK(restored);
    test_wbf = *restored; free(restored);
    CHECK(!atomic_load(&test_wbf.used_slot_bytes));
    hybrid.dram_index = dram_index_initialize(NULL, 0);
    CHECK(recovery_rebuild_subspaces(&database));
    CHECK(recovery_rebuild_write_buffer_indices(&database));
    CHECK(recovery_verify_post(&database));
    struct subspace_info *a = dram_index_search(hybrid.dram_index, 100, NULL, NULL);
    CHECK(a && a->key_num == SLOT_MAX_ELEMENT + 1 && !a->buffered_size);
    for(int k = 1; k <= SLOT_MAX_ELEMENT; k++) check_get(k, 'a');
    check_get(100, 'b'); gc_check_layout(a);
}
static void gc_quota_exit_check(void){
    CHECK(test_wbf.file_bytes == LIBDB_WBF_CAPACITY_LIMIT_BYTES);
    CHECK(test_wbf.file_num == 2 && test_wbf.limit_rejections >= 2);
    char *path = write_buffer_file_str(2); CHECK(access(path, F_OK) == -1); free(path);
    gc_failure_exit_check();
}
static void gc_real_quota_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    int pipes[2]; CHECK(!pipe(pipes));
    pid_t pid = fork(); CHECK(pid >= 0);
    if(!pid){
        close(pipes[0]); failure_exit_fd = pipes[1]; atexit(gc_quota_exit_check);
        /* Exhaust all segment capacity in a different size class. Those free
         * slots cannot serve a 1KiB return, and a third file is forbidden. */
        while(write_buffer_segement_new(&test_wbf, 4096, &worker.buffer_free_head));
        a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0); drain(); _exit(42);
    }
    close(pipes[1]); char ok = 0; CHECK(read(pipes[0], &ok, 1) == 1 && ok); close(pipes[0]);
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
    CHECK(!commits && !reclaimed);
}
int main(void){
    struct test_case cases[] = {
        {"real-wbf-return", gc_tail_return_test}, {"real-wbf-fill", gc_tail_fill_test},
        {"real-wbf-restart-return", gc_real_restart_test},
        {"real-wbf-restart-fill", gc_real_fill_restart_test},
        {"real-wbf-hard-limit", gc_real_quota_test}
    };
    char directory[] = "/tmp/libdb-gc-wbf-XXXXXX"; CHECK(mkdtemp(directory));
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++){
        char dir[512], path[560]; snprintf(dir, sizeof(dir), "%s/%zu", directory, i);
        CHECK(!mkdir(dir, 0700)); snprintf(path, sizeof(path), "%s/leaf.pool", dir);
        CHECK(!libdb_config_set_pmem_dir(dir));
        pid_t pid = fork(); CHECK(pid >= 0);
        if(!pid){
            setup(path); cases[i].fn();
            for(unsigned j = 0; j < test_wbf.file_num; j++) pmemobj_close(test_wbf.pool[j]);
            pmemobj_close(pool); _exit(0);
        }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        for(unsigned j = 0; j < 3; j++) { char *file = write_buffer_file_str(j); unlink(file); free(file); }
        unlink(path); rmdir(dir);
        if(!WIFEXITED(status) || WEXITSTATUS(status)){
            fprintf(stderr, "FAIL %s (status=%d)\n", cases[i].name, status); rmdir(directory); return 1;
        }
        printf("PASS %s\n", cases[i].name); fflush(stdout);
    }
    CHECK(!rmdir(directory));
    return 0;
}
