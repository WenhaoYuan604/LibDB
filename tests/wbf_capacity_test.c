/* Real PMDK files, small quota. No device access or production data. */
#include "kvbuffer.h"
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
void access_nvm(void) {}
void access_nvm_release(void) {}
static struct write_buffer *wb;
static void *expand(void *unused){
    pthread_mutex_lock(&g_mutex_lock_wbs);
    write_buffer_file_new(wb);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
    return NULL;
}
static void close_wb(struct write_buffer *p){
    for(unsigned i = 0; i < p->file_num; i++) pmemobj_close(p->pool[i]);
    free(p);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
}
int main(void){
    char dir[] = "/tmp/libdb-wbf-cap-XXXXXX";
    assert(mkdtemp(dir) && !libdb_config_set_pmem_dir(dir));
    wb = write_buffer_initialize(); assert(wb);
    assert(wb->file_bytes == LIBDB_WBF_FILE_SIZE);
    assert(!atomic_load(&wb->pressure));
    pthread_t threads[4];
    for(unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, expand, NULL));
    for(unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(wb->file_num == 2 && wb->file_bytes == LIBDB_WBF_CAPACITY_LIMIT_BYTES);
    assert(wb->file_peak_bytes == wb->file_bytes && wb->limit_rejections == 3);
    /* A real file expansion raises pressure without a main sample, also when
     * dynamic scheduling is disabled; scheduling policy does not own the cap. */
    assert(atomic_load(&wb->pressure));
    char *extra = write_buffer_file_str(2);
    assert(access(extra, F_OK) == -1);
    uint64_t total = 0;
    for(unsigned i = 0; i < 2; i++){
        char *path = write_buffer_file_str(i);
        struct stat st; assert(!stat(path, &st)); total += st.st_size; free(path);
    }
    assert(total == LIBDB_WBF_CAPACITY_LIMIT_BYTES);
    struct write_buffer_free_node *head = NULL;
    unsigned segments = 0;
    while(write_buffer_segement_new(wb, 64, &head)) segments++;
    assert(segments && wb->limit_rejections == 4 && access(extra, F_OK) == -1);
    assert(atomic_load(&wb->reusable_slot_bytes) == (uint64_t)segments * WSB_SIZE);
    uint64_t reusable = atomic_load(&wb->reusable_slot_bytes);
    char value[44]; memset(value, 'x', sizeof(value));
    uint16_t seq;
    PMEMoid id = write_buffer_insert(1, 1, value, sizeof(value), wb, &seq, &head, 0);
    assert(atomic_load(&wb->reusable_slot_bytes) == reusable - 64);
    assert(write_buffer_free(wb, id, seq, 64, &head));
    assert(atomic_load(&wb->reusable_slot_bytes) == reusable);
    assert(wb->file_num == 2 && wb->limit_rejections == 4);
    while(head){ struct write_buffer_free_node *next = head->next; free(head); head = next; }
    close_wb(wb);
    /* Over-limit inventory is rejected before opening/mutating any pool. */
    int fd = open(extra, O_CREAT | O_EXCL | O_WRONLY, 0600); assert(fd >= 0);
    assert(!ftruncate(fd, 1)); close(fd);
    assert(!write_buffer_open_existing());
    struct stat st; assert(!stat(extra, &st) && st.st_size == 1);
    unlink(extra); free(extra);
    wb = write_buffer_open_existing(); assert(wb);
    assert(wb->file_bytes == total && wb->file_num == 2);
    assert(atomic_load(&wb->reusable_slot_bytes) == 0); /* no free list restored */
    assert(atomic_load(&wb->used_slot_bytes) == 0 && atomic_load(&wb->pressure));
    close_wb(wb);
    for(unsigned i = 0; i < 2; i++){
        char *path = write_buffer_file_str(i); assert(!unlink(path)); free(path);
    }
    assert(!rmdir(dir));
    puts("PASS concurrent hard quota, no excess file, slot reuse, recovery preflight");
    return 0;
}
