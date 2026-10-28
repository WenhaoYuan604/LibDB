/* Exercise the real allocator/free/recovery accounting in temporary PMDK files. */
#include "kvbuffer.h"
#include <assert.h>
#include <unistd.h>

void access_nvm(void) {}
void access_nvm_release(void) {}
static bool fail_malloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size) {
    if(fail_malloc) { fail_malloc = false; return NULL; }
    return __real_malloc(size);
}
static void discard_free_list(struct write_buffer *wb, struct write_buffer_free_node **head) {
    while(*head) {
        struct write_buffer_free_node *p = *head;
        *head = p->next;
        atomic_fetch_sub(&wb->reusable_slot_bytes, (uint64_t)p->num * p->size);
        free(p);
    }
}
static struct write_buffer *concurrent_wb;
static void *concurrent_worker(void *arg) {
    struct write_buffer_free_node *head = NULL;
    char value[64]; memset(value, 't', sizeof(value));
    unsigned thread = (unsigned)(uintptr_t)arg;
    for(unsigned i = 0; i < 100; i++) {
        int value_size = i % 2 ? 44 : 45;
        uint16_t seq;
        PMEMoid id = write_buffer_insert(100 + thread, i + 1, value, value_size,
                                         concurrent_wb, &seq, &head, thread);
        assert(!OID_IS_NULL(id));
        assert(write_buffer_free(concurrent_wb, id, seq, kv_record_size(value_size), &head));
    }
    discard_free_list(concurrent_wb, &head);
    return NULL;
}
int main(void) {
    char dir[] = "/tmp/libdb-wbf-test-XXXXXX";
    assert(mkdtemp(dir));
    assert(libdb_config_set_pmem_dir(dir) == 0);
    char *path = write_buffer_file_str(0); assert(path);
    PMEMobjpool *pool = pmemobj_create(path, "write_buffer", 64U * 1024 * 1024, 0600);
    assert(pool);
    assert(!OID_IS_NULL(pmemobj_root(pool, sizeof(struct write_buffer_file_root))));
    struct write_buffer wb = {.file_num = 1, .pool = {pool}, .file_bytes = 64U * 1024 * 1024};
    atomic_init(&wb.used_slot_bytes, 0);
    assert(pthread_mutex_init(&g_mutex_lock_wbs, NULL) == 0);
    struct write_buffer_free_node *head = NULL;
    char value[2048]; memset(value, 'x', sizeof(value));
    uint16_t seq;
    PMEMoid a = write_buffer_insert(1, 1, value, 44, &wb, &seq, &head, 0);
    assert(atomic_load(&wb.used_slot_bytes) == 64);
    assert(atomic_load(&wb.reusable_slot_bytes) == WSB_SIZE - 64);
    PMEMoid b = write_buffer_insert(1, 2, value, 45, &wb, &seq, &head, 0);
    assert(atomic_load(&wb.used_slot_bytes) == 64 + 128);
    assert(write_buffer_free(&wb, a, 0, 64, &head));
    assert(atomic_load(&wb.used_slot_bytes) == 128);
    assert(!write_buffer_free(&wb, a, 0, 64, &head));
    assert(atomic_load(&wb.used_slot_bytes) == 128);
    PMEMoid reused = write_buffer_insert(2, 1, value, 44, &wb, &seq, &head, 0);
    assert(OID_EQUALS(reused, a));
    assert(atomic_load(&wb.used_slot_bytes) == 192);
    assert(!write_buffer_free(&wb, reused, 0, 64, NULL));
    assert(atomic_load(&wb.used_slot_bytes) == 192);
    PMEMoid c = write_buffer_insert(3, 1, value, 1000, &wb, &seq, &head, 0);
    assert(atomic_load(&wb.used_slot_bytes) == 1216);
    /* Free-list allocation failure must not report reusable capacity. */
    uint64_t free_before_failure = atomic_load(&wb.reusable_slot_bytes);
    fail_malloc = true;
    assert(!write_buffer_free(&wb, c, 0, 1020, &head));
    assert(atomic_load(&wb.used_slot_bytes) == 1216);
    assert(atomic_load(&wb.reusable_slot_bytes) == free_before_failure);
    PMEMoid older = write_buffer_insert(1, 1, value, 45, &wb, &seq, &head, 0);
    concurrent_wb = &wb;
    pthread_t threads[4];
    uint64_t before = atomic_load(&wb.used_slot_bytes);
    for(unsigned i = 0; i < 4; i++)
        assert(pthread_create(&threads[i], NULL, concurrent_worker, (void *)(uintptr_t)i) == 0);
    for(unsigned i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&wb.used_slot_bytes) == before);
    discard_free_list(&wb, &head);
    pmemobj_close(pool);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
    /* Both versions of key 1 occupy slots; the failed free is durably invalid. */
    struct write_buffer *opened = write_buffer_open_existing(); assert(opened);
    assert(atomic_load(&opened->used_slot_bytes) == 64 + 128 + 128);
    assert(opened->file_bytes == 64U * 1024 * 1024);
    assert(atomic_load(&opened->reusable_slot_bytes) == 0);
    assert(write_buffer_rebuild_occupancy(opened));
    assert(atomic_load(&opened->used_slot_bytes) == 320); /* no double count */
    assert(write_buffer_free(opened, b, 0, 65, &head));
    assert(atomic_load(&opened->used_slot_bytes) == 192);
    assert(write_buffer_free(opened, reused, 0, 64, &head));
    assert(write_buffer_free(opened, older, 0, 65, &head));
    assert(atomic_load(&opened->used_slot_bytes) == 0);
    assert(atomic_load(&opened->reusable_slot_bytes) == 320);
    discard_free_list(opened, &head);
    pmemobj_close(opened->pool[0]); free(opened);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
    unlink(path); free(path); rmdir(dir);
    puts("PASS WBF slot allocation, replacement, reuse, failed free and recovery accounting");
    return 0;
}
