/* In-memory NVMe with explicitly driven, reorderable completions; real PMDK
 * transactions use an ordinary temporary file. No NVMe device is opened. */
#include "insert.h"
#include "get.h"
#include "scan.h"
#include "kv_record.h"
#include "recovery.h"
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if(!(x)){ fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while(0)
#define DISK_BLOCKS 8192
#define TASKS 32
#define VALUE_SIZE 44
#define RECORD_SIZE (KV_RECORD_HEADER_SIZE + VALUE_SIZE)
static char disk[DISK_BLOCKS][BLOCK_SIZE];
static bool allocated[DISK_BLOCKS];
static uint32_t next_block = 1;
static uint32_t fail_space_at;
static PMEMobjpool *pool;
static PMEMoid slot_segment;
static size_t slot_cursor;
static const char *pool_path;
static struct thread worker;
static struct db database;
static struct write_buffer test_wbf;
static uint64_t slot_bytes(size_t size){
    uint64_t n = WBF_SLOT_MIN_SIZE; while(n < size) n *= 2; return n;
}
static struct hybrid_index hybrid;
static struct nvm_index persistent;
static struct ns_entry ns;
static struct spdk_nvme_qpair *qpairs[1];
static struct task task_store[TASKS], *free_head[1], *free_tail[1];
static struct result *last_result;
static int commits, reclaimed, writes_completed, queued_count, pending_writes;
static bool fail_read, fail_write, fail_commit, fail_submit, fail_alloc;
static bool fail_completion_poll;
static uint32_t completion_polls, poll_delay;
static void (*completion_hook)(void);
static bool migration_test;
static bool fail_wbf, fail_slot_alloc;
static uint32_t last_commit_count;
static PMEMoid last_commit_slots[2 * SUBSPACE_MAX_SLOT];
/* Arm only for the last GC read callback: its first malloc is the packed
 * output buffer. Poison it to expose unwritten bytes in submitted blocks. */
static bool capture_gc_allocation, fail_gc_allocation, track_gc_cache;
static size_t gc_allocation_bytes;
static unsigned gc_cache_deletes;
static key_type gc_deleted_keys[SUBSPACE_MAX_KEY];
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size){
    if(!capture_gc_allocation) return __real_malloc(size);
    capture_gc_allocation = false;
    gc_allocation_bytes = size;
    if(fail_gc_allocation) return NULL;
    void *p = __real_malloc(size);
    if(p) memset(p, 0xa5, size);
    return p;
}
void __real_kvcache_delete(kvcache *, lru_list *, key_type);
void __wrap_kvcache_delete(kvcache *cache, lru_list *list, key_type key){
    if(track_gc_cache){
        CHECK(gc_cache_deletes < SUBSPACE_MAX_KEY);
        gc_deleted_keys[gc_cache_deletes++] = key;
    }
    __real_kvcache_delete(cache, list, key);
}
static size_t stored_record_size = RECORD_SIZE;
static int failure_exit_fd = -1;
struct io {
    void *buffer;
    uint32_t block;
    bool write;
    spdk_nvme_cmd_cb cb;
    void *arg;
    uint32_t flags;
};
static struct io queued[4096];

struct address_type ADDR_NULL = {0};
void access_nvm(void) {}
void access_nvm_release(void) {}
int random_level(int max, unsigned int *seed){ return rand_r(seed) % (max + 1); }
uint64_t spdk_get_ticks(void){ static uint64_t tick; return ++tick; }
uint64_t spdk_get_ticks_hz(void){ return 1000000; }
uint32_t spdk_env_get_current_core(void){ return 0; }
int spdk_env_thread_launch_pinned(uint32_t core, thread_start_fn fn, void *arg){ return -1; }
void *spdk_zmalloc(size_t size, size_t align, uint64_t *phys, int socket, uint32_t flags){
    if(fail_alloc) return NULL;
    void *p = NULL;
    CHECK(posix_memalign(&p, align, size) == 0);
    memset(p, 0, size);
    return p;
}
void spdk_free(void *p){ free(p); }
size_t spdk_ring_enqueue(struct spdk_ring *ring, void **objects, size_t count, size_t *free_space){
    CHECK(count == 1 && last_result == NULL);
    last_result = objects[0];
    return count;
}
static int submit(void *buffer, uint64_t lba, uint32_t count, spdk_nvme_cmd_cb cb, void *arg, uint32_t flags, bool write){
    if(fail_submit) return -EIO;
    CHECK(lba < DISK_BLOCKS && count == 1 && queued_count < 4096);
    if(write && migration_test) CHECK(flags & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
    if(write) for(int i = 0; i < TASKS; i++)
        if(arg == &task_store[i] && task_store[i].gc_plan) CHECK(flags == 0);
    if(write) pending_writes++;
    queued[queued_count++] = (struct io){buffer, lba, write, cb, arg, flags};
    return 0;
}
int spdk_nvme_ns_cmd_read(struct spdk_nvme_ns *ns_, struct spdk_nvme_qpair *q, void *buffer,
                         uint64_t lba, uint32_t count, spdk_nvme_cmd_cb cb, void *arg, uint32_t flags){
    return submit(buffer, lba, count, cb, arg, flags, false);
}
int spdk_nvme_ns_cmd_write(struct spdk_nvme_ns *ns_, struct spdk_nvme_qpair *q, void *buffer,
                          uint64_t lba, uint32_t count, spdk_nvme_cmd_cb cb, void *arg, uint32_t flags){
    return submit(buffer, lba, count, cb, arg, flags, true);
}
extern void flush_cb(void *, const struct spdk_nvme_cpl *);
extern void split_write_cb(void *, const struct spdk_nvme_cpl *);
static uint64_t expected_ssd_bytes[SSD_WRITE_KINDS];
static void verify_ssd_completions(void){
    for (int kind = 0; kind < SSD_WRITE_KINDS; kind++)
        CHECK(atomic_load(&worker.ssd_writes.bytes[kind]) == expected_ssd_bytes[kind]);
}
static void complete_at(int at){
    CHECK(at < queued_count);
    struct io io = queued[at];
    memmove(queued + at, queued + at + 1, (--queued_count - at) * sizeof(*queued));
    if(io.write) pending_writes--;
    struct spdk_nvme_cpl cpl = {0};
    if((io.write && fail_write) || (!io.write && fail_read)) cpl.status.sc = 1;
    else if(io.write){ memcpy(disk[io.block], io.buffer, BLOCK_SIZE); writes_completed++; }
    else memcpy(io.buffer, disk[io.block], BLOCK_SIZE);
    if (io.write && !spdk_nvme_cpl_is_error(&cpl)) {
        enum ssd_write_kind kind = SSD_WRITE_MERGE;
        if (io.cb == flush_cb) kind = SSD_WRITE_FLUSH;
        else if (io.cb == split_write_cb) kind = SSD_WRITE_SPLIT;
        else for (int i = 0; i < TASKS; i++)
            if (io.arg == &task_store[i] && task_store[i].gc_plan) kind = SSD_WRITE_GC;
        expected_ssd_bytes[kind] += BLOCK_SIZE;
    }
    io.cb(io.arg, &cpl);
    verify_ssd_completions();
}
int32_t spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *q, uint32_t max){
    completion_polls++;
    if(completion_hook) completion_hook();
    if(fail_completion_poll) return -EIO;
    if(completion_polls <= poll_delay || !queued_count) return 0;
    complete_at(0);
    return 1;
}
static void drain(void){
    int limit = 10000;
    while(queued_count && --limit) complete_at(queued_count - 1);
    CHECK(limit);
}
int get_free_block(struct segment **segment, struct free_space *fs){
    if(fail_space_at && next_block >= fail_space_at) return -1;
    CHECK(next_block < DISK_BLOCKS);
    allocated[next_block] = true;
    return next_block++;
}
void free_block(uint32_t *blocks, int count, struct segment **segment, struct free_space *fs){
    for(int i = 0; i < count; i++){
        CHECK(allocated[blocks[i]]);
        if(migration_test) CHECK(commits > 0);
        allocated[blocks[i]] = false;
        block_ref_clear(blocks[i]);
        reclaimed++;
    }
}
PMEMoid __wrap_nvm_index_get_free_node(struct nvm_index *index, struct nvm_index_free_node **head, PMEMobjpool **out){
    if(fail_slot_alloc) return OID_NULL;
    CHECK(slot_cursor < LNS_SIZE / LNF_SLOT_MIN_SIZE);
    PMEMoid oid = slot_segment;
    oid.off += slot_cursor++ * LNF_SLOT_MIN_SIZE;
    *out = pool;
    return oid;
}
int __wrap_nvm_index_reserve_free_nodes(struct nvm_index *index, struct nvm_index_free_node **head, uint32_t count){ return 1; }
int __real_nvm_index_commit_slot_updates(struct nvm_index *, const struct nvm_slot_update *, uint32_t);
int __wrap_nvm_index_commit_slot_updates(struct nvm_index *index, const struct nvm_slot_update *updates, uint32_t count){
    CHECK(count <= 2 * SUBSPACE_MAX_SLOT);
    last_commit_count = count;
    for(uint32_t i = 0; i < count; i++) last_commit_slots[i] = updates[i].slot;
    if(migration_test){
        CHECK(pending_writes == 0 && writes_completed > 0);
        for(uint32_t i = 0; i < count; i++){
            for(uint32_t j = 0; j < SLOT_MAX_ELEMENT; j++){
                const struct slot_element *entry = &updates[i].image.entries[j];
                if(!entry->addr.size) continue;
                struct kv_record_view view;
                CHECK(kv_record_decode_for_key(disk[entry->addr.block] + entry->addr.off,
                      entry->addr.size, entry->key, &view) == 0);
            }
        }
    }
    if(fail_commit) return -1;
    int rc = __real_nvm_index_commit_slot_updates(index, updates, count);
    CHECK(rc == 0);
    commits++;
    return rc;
}
#ifndef LIBDB_TEST_REAL_WBF
/* PMEM record invalidation matches the production write-buffer contract. */
int write_buffer_free(struct write_buffer *buffer, PMEMoid id, uint16_t seq, int size, struct write_buffer_free_node **head){
    uint16_t *key_size = pmemobj_direct(id);
    CHECK(*key_size == sizeof(key_type));
    CHECK(atomic_load(&buffer->used_slot_bytes) >= slot_bytes(size));
    atomic_fetch_sub(&buffer->used_slot_bytes, slot_bytes(size));
    atomic_fetch_add(&buffer->reusable_slot_bytes, slot_bytes(size));
    *key_size = 0;
    pmemobj_persist(pool, key_size, sizeof(*key_size));
    return 1;
}

PMEMoid write_buffer_insert(key_type key, kv_version_type version, const char *value,
    int value_size, struct write_buffer *buffer, uint16_t *seq,
    struct write_buffer_free_node **head, int thread){
    if(fail_wbf) return OID_NULL;
    PMEMoid oid;
    CHECK(pmemobj_zalloc(pool, &oid, kv_record_size(value_size), 2) == 0);
    CHECK(kv_record_encode(pmemobj_direct(oid), kv_record_size(value_size), key, version, value, value_size) == 0);
    pmemobj_persist(pool, pmemobj_direct(oid), kv_record_size(value_size));
    atomic_fetch_add(&buffer->used_slot_bytes, slot_bytes(kv_record_size(value_size)));
    atomic_fetch_sub(&buffer->reusable_slot_bytes, slot_bytes(kv_record_size(value_size)));
    *seq = 0;
    return oid;
}
#endif
kv_version_type kv_record_next_thread_version(kv_version_type *next, kv_version_type *limit){ return ++*next; }

static void setup(const char *path){
    ssd_write_stats_init(&worker.ssd_writes);
    CHECK(!atexit(verify_ssd_completions));
    pool = pmemobj_create(path, "merge-test", 32 * 1024 * 1024, 0600);
    CHECK(pool);
    pool_path = path;
    CHECK(pmemobj_zalloc(pool, &slot_segment, LNS_SIZE, 1) == 0);
    PMEMoid root_oid = pmemobj_root(pool, sizeof(struct leaf_node_file_root));
    struct leaf_node_file_root *root = pmemobj_direct(root_oid);
    root->segment_num = 1;
    root->segment[0] = slot_segment;
    pmemobj_persist(pool, root, sizeof(*root));
    persistent.file_num = 1;
    persistent.pool[0] = pool;
#ifdef LIBDB_TEST_REAL_WBF
    struct write_buffer *real = write_buffer_initialize();
    CHECK(real);
    test_wbf = *real;
    free(real);
#else
    atomic_init(&test_wbf.used_slot_bytes, 0);
    test_wbf.file_bytes = test_wbf.file_peak_bytes = 32 * 1024 * 1024;
    atomic_init(&test_wbf.reusable_slot_bytes, test_wbf.file_bytes);
    CHECK(pthread_mutex_init(&g_mutex_lock_wbs, NULL) == 0);
#endif
    database.write_buffer = worker.kvbuffer = &test_wbf;
    space_stats_nvm_init(&database);
    database.thread = 1;
    database.boundary_size = 2;
    database.load_injectors = 1;
    database.workers = &worker;
    static key_type boundaries[] = {0, 1000000};
    static key_type *boundary_arrays[] = {boundaries};
    database.boundary = boundary_arrays;
    hybrid.dram_index = dram_index_initialize(NULL, 0);
    hybrid.nvm_index = &persistent;
    database.index = &hybrid;
    worker.db = &database;
    worker.index = &hybrid;
    worker.ns_entry = &ns;
    worker.lru_list = lru_list_init(1 << 20);
    static struct spdk_ring *rings[1];
    worker.spdk_result_ring = rings;
    ns.qpair = qpairs;
    ns.task_array_head = free_head;
    ns.task_array_tail = free_tail;
    for(int i = 0; i < TASKS; i++){
        task_store[i].ns_entry = &ns;
        task_store[i].buffer = calloc(LIBDB_TASK_BUFFER_BLOCK_COUNT, BLOCK_SIZE);
        task_store[i].thread_seq = 0;
        free_task(&task_store[i]);
    }
    static uint_fast8_t map[DISK_BLOCKS];
    g_map = map;
    g_block_num = DISK_BLOCKS;
    static uint64_t counters[16];
    thread_block_rw = counters;
    thread_flush_block_write = counters + 1;
    thread_split_block_rw = counters + 2;
    thread_split_block_write = counters + 3;
    thread_gc_block_rw = counters + 4;
    thread_gc_block_write = counters + 5;
    invalid_block_num = counters + 6;
    thread_kvcache_hit = counters + 7;
    thread_kvbuffer_hit = counters + 8;
    thread_scan_total = counters + 9;
    thread_scan_on_block = counters + 10;
    thread_scan_block = counters + 11;
    static unsigned int seed = 5;
    random_seed = &seed;
    static struct insert_time_breakdown breakdown;
    insert_breakdown = &breakdown;
}
static struct subspace_info *group(key_type fence){
    struct subspace_info *g = subspace_info_initialize();
    dram_index_insert(hybrid.dram_index, fence, g, NULL);
    thread_subspaces[0]++;
    return g;
}
static void buffer_put(struct subspace_info *g, key_type key, char byte, size_t size){
    char value[BLOCK_SIZE]; memset(value, byte, sizeof(value));
    uint16_t seq;
    PMEMoid oid = write_buffer_insert(key, key + byte, value, size - KV_RECORD_HEADER_SIZE,
        &test_wbf, &seq, &worker.buffer_free_head, 0);
    CHECK(!OID_IS_NULL(oid));
    struct write_buffer_return_type old = nvm_index_insert(g, oid, key, size, seq, 0);
    if(old.size) write_buffer_free(worker.kvbuffer, old.id, old.WBF_seq, old.size, &worker.buffer_free_head);
}
static void buffered(struct subspace_info *g, key_type first, int count){
    for(int i = 0; i < count; i++) buffer_put(g, first + i, 'a', RECORD_SIZE);
}
static void stored(struct subspace_info *g, key_type first, int count, int per_slot){
    for(int start = 0; start < count; start += per_slot){
        CHECK(g->slot_num < SUBSPACE_MAX_SLOT);
        uint32_t slot = g->slot_num++;
        g->slot[slot] = __wrap_nvm_index_get_free_node(&persistent, NULL, &g->pool[slot]);
        struct nvm_index_slot *image = pmemobj_direct(g->slot[slot]);
        image->boundary = g->boundary;
        int n = count - start < per_slot ? count - start : per_slot;
        g->slot_key_num[slot] = n;
        g->min_key[slot] = first + start;
        for(int i = 0; i < n; i++){
            uint32_t block = get_free_block(NULL, NULL);
            char value[BLOCK_SIZE]; memset(value, 'a', sizeof(value));
            key_type key = first + start + i;
            CHECK(kv_record_encode(disk[block], stored_record_size, key, key, value, stored_record_size - KV_RECORD_HEADER_SIZE) == 0);
            image->entries[i].key = key;
            image->entries[i].addr = (struct address_type){block, 0, stored_record_size, 0};
            block_ref_add(block);
            g->total_block_num++;
            g->key_num++;
        }
        pmemobj_persist(pool, image, sizeof(*image));
    }
}
static void check_get(key_type key, char expected){
    int size = -1;
    char *value = get(key, &worker, &size, 0, NULL);
    if(!value && size){
        drain();
        CHECK(last_result && last_result->type == 0);
        value = last_result->value;
        size = last_result->size;
        free(last_result); last_result = NULL;
    }
    if(!expected){ CHECK(!value && size == 0); return; }
    CHECK(value && size >= LIBDB_PMEM_VALUE_PREFIX_SIZE);
    for(int i = 0; i < LIBDB_PMEM_VALUE_PREFIX_SIZE; i++) CHECK(value[i] == expected);
    free(value);
}

static void threshold_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    buffered(a, 1, 64); buffered(b, 1001, 64);
    db_delete(999, &worker); CHECK(thread_subspaces[0] == 2); /* equality is not below */
    db_delete(1, &worker); CHECK(thread_subspaces[0] == 1 && subspace_live_keys(a) == 127);
    db_delete(1, &worker); CHECK(subspace_live_keys(a) == 127 && worker.merge_count == 1);
    check_get(1, 0); check_get(2, 'a'); check_get(1001, 'a');
}
static void capacity_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    buffered(a, 1, 63); buffered(b, 1001, 66);
    db_delete(999, &worker); CHECK(thread_subspaces[0] == 2); /* 129 */
    db_delete(1001, &worker); CHECK(thread_subspaces[0] == 1 && subspace_live_keys(a) == 128);
}
static void neighbor_test(void){
    struct subspace_info *a = group(0), *b = group(1001), *c = group(2001);
    buffered(a, 1, 20); buffered(b, 1001, 70); buffered(c, 2001, 10);
    db_delete(1500, &worker); /* only neighbors are below threshold; choose smaller right */
    CHECK(thread_subspaces[0] == 2 && subspace_live_keys(b) == 80);
    CHECK(dram_index_neighbor(hybrid.dram_index, 0, 1) == b);
    db_delete(1500, &worker); CHECK(thread_subspaces[0] == 1 && subspace_live_keys(a) == 100);
}
static void tie_and_empty_test(void){
    struct subspace_info *a = group(0), *b = group(1001), *c = group(2001);
    db_delete(1500, &worker); CHECK(thread_subspaces[0] == 2);
    CHECK(dram_index_search(hybrid.dram_index, 1500, NULL, NULL) == a);
    CHECK(dram_index_search(hybrid.dram_index, 2500, NULL, NULL) == c);
    db_delete(2500, &worker); CHECK(thread_subspaces[0] == 1 && a->slot_num == 0);
}
static void partition_test(void){
    group(0); group(1000000);
    db_delete(999999, &worker); CHECK(thread_subspaces[0] == 2);
    static key_type b0[] = {0, 2000}, b1[] = {1000, 3000};
    static key_type *bounds[] = {b0, b1};
    database.thread = 2; database.boundary = bounds;
    CHECK(!merge_same_partition(&worker, 0, 2000));
    CHECK(!merge_same_partition(&worker, 0, 1000));
    CHECK(merge_same_partition(&worker, 1, 999));
}
static void index_test(void){
    for(int i = 0; i < 17000; i++) group((key_type)i * 10);
    CHECK(!hybrid.dram_index->head->is_leaf && !hybrid.dram_index->head->children[0]->is_leaf);
    for(int i = 0; i < 16999; i++){
        struct subspace_info *next = dram_index_neighbor(hybrid.dram_index, (key_type)i * 10, 1);
        CHECK(next && next->boundary == (key_type)(i + 1) * 10);
        CHECK(dram_index_remove(hybrid.dram_index, (key_type)i * 10));
        CHECK(dram_index_search(hybrid.dram_index, (key_type)(i + 1) * 10, NULL, NULL) == next);
    }
    CHECK(hybrid.dram_index->head->is_leaf && hybrid.dram_index->head->nchild == 1);
    CHECK(dram_index_remove(hybrid.dram_index, 169990));
    CHECK(!dram_index_search(hybrid.dram_index, 0, NULL, NULL));
}
static void delete_test(void){
    struct subspace_info *a = group(0);
    stored(a, 1, 3, 2);
    buffer_put(a, 1, 'b', RECORD_SIZE);
    CHECK(subspace_live_keys(a) == 3);
    kvcache_insert(a->kvcache, worker.lru_list, 1, "cache", 5, 0);
    kvcache_insert(a->kvcache, worker.lru_list, 2, "cache", 5, 0);
    db_delete(1, &worker);
    CHECK(a->key_num == 2 && a->slot_key_num[0] == 1 && a->min_key[0] == 2);
    CHECK(a->buffered_size == 0 && worker.lru_list->size == 5);
    db_delete(2, &worker); CHECK(a->slot_num == 1 && a->min_key[0] == 3);
    db_delete(3, &worker); db_delete(3, &worker);
    CHECK(a->key_num == 0 && a->slot_num == 0 && reclaimed == 3);
    CHECK(!worker.lru_list->lru_head && !worker.lru_list->lru_tail && !worker.lru_list->size);
    check_get(1, 0);
}
static void migration_test_fn(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 20, 1); stored(b, 1001, 20, 1); /* combined slots exceed fixed capacity */
    buffer_put(a, 1, 'b', RECORD_SIZE); buffer_put(b, 1001, 'c', RECORD_SIZE);
    kvcache_insert(b->kvcache, worker.lru_list, 1002, "cache", 5, 0);
    PMEMoid old = b->slot[0];
    struct nvm_index_slot original = *(struct nvm_index_slot *)pmemobj_direct(old);
    migration_test = true;
    db_delete(999, &worker);
    CHECK(a->is_merging && a->slot_num == 4 && thread_subspaces[0] == 1);
    CHECK(subspace_live_keys(a) == 40 && a->key_num == 40);
    CHECK(!memcmp(pmemobj_direct(old), &original, sizeof(original)) && reclaimed == 0);
    buffer_put(a, 2, 'd', RECORD_SIZE); buffer_put(a, 3000, 'e', RECORD_SIZE);
    CHECK(!subspace_maybe_flush(a, &worker) || a->buffered_size <= BLOCK_SIZE * Flush_Blcok_Num);
    drain();
    CHECK(!a->is_merging && !a->migration_slot_shadow && commits == 1 && reclaimed == 40);
    CHECK(a->total_block_num == 1 && worker.merge_read_blocks == 40 && worker.merge_write_blocks == 1);
    for(uint32_t i = 0; i < a->slot_num; i++) CHECK(subspace_slot_boundary(a, i) == 0);
    check_get(1, 'b'); check_get(2, 'd'); check_get(1001, 'c'); check_get(3000, 'e');
    /* Normal restart: committed slots have one fence and remain readable. */
    CHECK(((struct nvm_index_slot *)pmemobj_direct(old))->entries[0].addr.size == 0);
    migration_test = false;
}
static void pending_read_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 3, 3); stored(b, 1001, 3, 3);
    struct address_type addr = nvm_index_search(b, 1001);
    int size;
    CHECK(get(1001, &worker, &size, 0, NULL) == NULL && queued_count == 1);
    migration_test = true;
    db_delete(999, &worker);
    /* Keep the old get pending while every migration I/O completes. */
    while(queued_count > 1) complete_at(queued_count - 1);
    CHECK(worker.merge_count == 1 && allocated[addr.block] && b->redirect == a);
    complete_at(0);
    CHECK(!allocated[addr.block] && last_result);
    free(last_result->value); free(last_result); last_result = NULL;
    CHECK(a->read_refs == 0 && worker.read_pins == NULL);
    migration_test = false;
}
static void delete_wait_and_cache_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 2, 2); stored(b, 1001, 2, 2);
    db_delete(999, &worker); CHECK(a->is_merging);
    db_delete(1001, &worker); CHECK(!a->is_merging && a->key_num == 3);
    int size;
    CHECK(!get(1002, &worker, &size, 0, NULL));
    db_delete(1002, &worker);
    drain();
    CHECK(last_result); free(last_result->value); free(last_result); last_result = NULL;
    check_get(1002, 0);
}
static void busy_retry_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    buffered(a, 1, 5); buffered(b, 1001, 5);
    b->is_gc = true;
    db_delete(999, &worker); CHECK(thread_subspaces[0] == 2 && worker.maintenance);
    maintenance_poll(&worker); CHECK(thread_subspaces[0] == 2);
    b->is_gc = false;
    maintenance_poll(&worker); CHECK(thread_subspaces[0] == 1 && worker.merge_count == 1);
    maintenance_poll(&worker); CHECK(worker.merge_count == 1);
}
static void allocation_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 2, 2); stored(b, 1001, 2, 2);
    fail_alloc = true;
    db_delete(999, &worker);
    CHECK(thread_subspaces[0] == 2 && !a->is_merging && queued_count == 0);
    fail_alloc = false; maintenance_poll(&worker); drain();
    CHECK(worker.merge_count == 1);
}
static void failure_exit_check(void){
    if(failure_exit_fd >= 0){
        char ok = reclaimed == 0 && worker.merge_count == 0 &&
                  dram_index_search(hybrid.dram_index, 0, NULL, NULL)->is_merging;
        CHECK(write(failure_exit_fd, &ok, 1) == 1);
    }
}
static void failure_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 2, 2); stored(b, 1001, 2, 2);
    for(int kind = 0; kind < 4; kind++){
        int pipes[2]; CHECK(pipe(pipes) == 0);
        pid_t pid = fork(); CHECK(pid >= 0);
        if(!pid){
            close(pipes[0]); failure_exit_fd = pipes[1]; atexit(failure_exit_check);
            fail_read = kind == 0; fail_write = kind == 1; fail_commit = kind == 2; fail_submit = kind == 3;
            db_delete(999, &worker); drain(); _exit(42);
        }
        close(pipes[1]); char ok = 0; CHECK(read(pipes[0], &ok, 1) == 1 && ok); close(pipes[0]);
        int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
    }
}
static void prefix_test(void){
    struct subspace_info *a = group(0);
    for(int i = 1; i <= 300; i++) buffer_put(a, i, 'a', BLOCK_SIZE);
    struct write_buffer_index *prefix = write_buffer_index_take_prefix(a->buffer_index, 1000, 256 * BLOCK_SIZE);
    CHECK(write_buffer_index_flush_size(prefix, 0) == 256 * BLOCK_SIZE);
    CHECK(write_buffer_index_search(prefix, 256) && !write_buffer_index_search(prefix, 257));
    CHECK(write_buffer_index_search(a->buffer_index, 257) && !write_buffer_index_search(a->buffer_index, 256));
    write_buffer_index_merge(prefix, a->buffer_index);
    write_buffer_index_destroy(a->buffer_index); a->buffer_index = prefix;
    CHECK(subspace_live_keys(a) == 300);
}
static void scan_during_merge_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 3, 3); stored(b, 1001, 3, 3);
    buffer_put(b, 1002, 'b', RECORD_SIZE);
    key_type keys[6]; char *values[6] = {0}; int sizes[6];
    struct scan_kv result = {6, keys, values, sizes};
    CHECK(scan(1, 6, (char *)&result, &worker, 0) == 0);
    int reads = queued_count;
    CHECK(reads == 5);
    db_delete(999, &worker);
    while(queued_count > reads) complete_at(queued_count - 1);
    CHECK(worker.merge_count == 1);
    drain();
    CHECK(last_result && last_result->type == 2 && last_result->size == 6);
    for(int i = 0; i < 6; i++){
        CHECK(keys[i] == (key_type)(i < 3 ? i + 1 : 1001 + i - 3));
        CHECK(values[i][0] == (i == 4 ? 'b' : 'a'));
        free(values[i]);
    }
    free(last_result); last_result = NULL;
    CHECK(!worker.read_pins && !a->read_refs);
    memset(values, 0, sizeof(values));
    CHECK(scan(1, 6, (char *)&result, &worker, 0) == 0); drain();
    CHECK(last_result && last_result->size == 6);
    for(int i = 0; i < 6; i++) free(values[i]);
    free(last_result); last_result = NULL;
}
static void backlog_and_split_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 10, 10); stored(b, 1001, 10, 10);
    db_delete(999, &worker);
    char value[BLOCK_SIZE]; memset(value, 'z', sizeof(value));
    for(int i = 0; i < 300; i++)
        insert(10000 + i, BLOCK_SIZE - KV_RECORD_HEADER_SIZE, value, &worker, insert_breakdown);
    CHECK(a->is_merging && a->buffered_size > LIBDB_TASK_BUFFER_BLOCK_COUNT * BLOCK_SIZE);
    int limit = 10000;
    while((queued_count || worker.maintenance) && --limit){
        drain(); maintenance_poll(&worker);
    }
    CHECK(limit && thread_subspaces[0] == 1 && a->buffered_size > LIBDB_TASK_BUFFER_BLOCK_COUNT * BLOCK_SIZE);
    CHECK(!queued_count && !worker.maintenance);
    for(int round = 0; round < 4; round++) for(int i = 0; i < 300; i++){
        insert(10000 + i, BLOCK_SIZE - KV_RECORD_HEADER_SIZE, value, &worker, insert_breakdown);
        drain(); maintenance_poll(&worker); CHECK(!queued_count);
    }
    CHECK(thread_subspaces[0] > 1);
    for(int i = 0; i < 300; i++) check_get(10000 + i, 'z');
    check_get(1, 'a'); check_get(1001, 'a');
    CHECK(!worker.read_pins);
}
static void gc_after_merge_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored_record_size = 1024;
    stored(a, 1, 5, 5); stored(b, 1001, 5, 5);
    db_delete(999, &worker); drain();
    CHECK(a->total_block_num == 3);
    db_delete(1, &worker); db_delete(2, &worker);
    db_delete(5, &worker); db_delete(1001, &worker);
    a->is_gc = true;
    garbadge_collection(a, &worker); drain();
    CHECK(!a->is_gc);
    for(int i = 0; i < a->slot_num; i++)
        for(int j = 0; j < a->slot_key_num[i]; j++)
            CHECK(allocated[subspace_slot_elements(a, i)[j].addr.block]);
    check_get(3, 'a'); check_get(4, 'a'); check_get(1002, 'a'); check_get(1005, 'a');
}

static void restart_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 20, 1); stored(b, 1001, 20, 1);
    db_delete(999, &worker); drain();
    db_delete(1, &worker); db_delete(1001, &worker);
    pmemobj_close(pool);
    pool = pmemobj_open(pool_path, "merge-test");
    CHECK(pool);
    persistent.pool[0] = pool;
    hybrid.dram_index = dram_index_initialize(NULL, 0);
    CHECK(recovery_rebuild_subspaces(&database));
    CHECK(recovery_verify_post(&database));
    struct subspace_info *restored = dram_index_search(hybrid.dram_index, 1002, NULL, NULL);
    CHECK(restored && restored->boundary == 0 && restored->key_num == 38);
    CHECK(dram_index_neighbor(hybrid.dram_index, 0, 1) == NULL);
    check_get(1, 0); check_get(1001, 0); check_get(2, 'a'); check_get(1020, 'a');
}

static void no_task_retry_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored(a, 1, 3, 3); stored(b, 1001, 3, 3);
    struct task *held[TASKS];
    for(int i = 0; i < TASKS; i++) { held[i] = get_free_task(&ns, 0); CHECK(held[i]); }
    db_delete(999, &worker);
    CHECK(!a->is_merging && thread_subspaces[0] == 2 && worker.maintenance);
    for(int i = 0; i < TASKS; i++) free_task(held[i]);
    maintenance_poll(&worker); drain();
    CHECK(worker.merge_count == 1 && thread_subspaces[0] == 1);
}
static void buffer_union_test(void){
    struct subspace_info *a = group(0);
    stored(a, 1, 2, 2);
    buffered(a, 1, 3);
    struct write_buffer_index *frozen = malloc(sizeof(*frozen));
    write_buffer_index_init(frozen);
    struct subspace_info fake = {0}; fake.buffer_index = frozen;
    buffered(&fake, 2, 3);
    a->buffer_index->next = frozen;
    frozen->prev = a->buffer_index;
    CHECK(subspace_live_keys(a) == 4);
}
static void variable_records_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored_record_size = 3000; stored(a, 1, 3, 3);
    stored_record_size = 1200; stored(b, 1001, 3, 3);
    db_delete(999, &worker); drain();
    CHECK(a->total_block_num == 4);
    for(int i = 1; i <= 3; i++) check_get(i, 'a');
    for(int i = 1001; i <= 1003; i++) check_get(i, 'a');
    for(uint32_t i = 0; i < a->slot_num; i++)
        for(uint32_t j = 0; j < a->slot_key_num[i]; j++){
            struct address_type addr = subspace_slot_elements(a, i)[j].addr;
            CHECK(addr.off + addr.size <= BLOCK_SIZE);
        }
}

static void space_rollback_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    stored_record_size = 3000; stored(a, 1, 2, 2); stored(b, 1001, 2, 2);
    uint32_t first_destination = next_block;
    fail_space_at = next_block + 1;
    db_delete(999, &worker);
    CHECK(!a->is_merging && thread_subspaces[0] == 2 && queued_count == 0);
    CHECK(!allocated[first_destination] && reclaimed == 1);
    CHECK(allocated[nvm_index_search(a, 1).block] && allocated[nvm_index_search(b, 1001).block]);
    fail_space_at = 0;
    maintenance_poll(&worker); drain(); CHECK(worker.merge_count == 1);
}

/* Force a small batch through the production flush callback. */
extern void flush_cb(void *, const struct spdk_nvme_cpl *);
extern int flush_write_buffer_to_ssd(struct subspace_info *, struct thread *,
    struct ns_entry *, uint32_t, void (*)(void *, const struct spdk_nvme_cpl *),
    key_type, struct insert_time_breakdown *);
static bool test_force_flush(struct subspace_info *g, struct thread *t) {
    return flush_write_buffer_to_ssd(g, t, t->ns_entry, 0, flush_cb, 0,
                                    &insert_breakdown[t->thread]) != 0;
}

static void gc_amplification_threshold_test(void) {
    struct subspace_info *a = group(0);
    for(int i = 1; i <= 17; i++) buffer_put(a, i, 'a', 1024);
    CHECK(test_force_flush(a, &worker));
    drain();
    CHECK(a->total_block_num == 5 && thread_gc_block_rw[0] == 0);
    for(int i = 1; i <= 7; i++) buffer_put(a, i, 'b', 1024);
    CHECK(test_force_flush(a, &worker));
    /* After publication: 7 KiB redundant / 28 KiB occupied = 25%.
     * New 0.3-over-valid semantics trigger GC; old 30%-of-occupied did not. */
    drain();
    CHECK(thread_gc_block_rw[0] > 0 && !a->is_gc);
    for(int i = 1; i <= 7; i++) check_get(i, 'b');
    for(int i = 8; i <= 17; i++) check_get(i, 'a');
}

static void gc_block_threshold_test(void) {
    struct subspace_info *a = group(0);
    stored_record_size = 3072;
    stored(a, 1, 1, 1);
    uint32_t block = nvm_index_search(a, 1).block;
    /* A block with 3 KiB valid + 1 KiB unused has amplification 4/3 > 1.3. */
    a->is_gc = true;
    garbadge_collection(a, &worker);
    CHECK(queued_count == 1 && thread_gc_block_rw[0] == 1);
    drain();
    CHECK(!allocated[block] && !a->is_gc);
    check_get(1, 'a');
}

static void gc_relaxed_group_test(void) {
    atomic_store(&database.gc_space.relaxed, true);
    struct subspace_info *a = group(0);
    for(int i = 1; i <= 17; i++) buffer_put(a, i, 'a', 1024);
    CHECK(test_force_flush(a, &worker)); drain();
    for(int i = 1; i <= 7; i++) buffer_put(a, i, 'b', 1024);
    CHECK(test_force_flush(a, &worker)); drain();
    /* 25% redundant is below 0.45/1.45, unlike the default policy. */
    CHECK(thread_gc_block_rw[0] == 0 && !a->is_gc);
    /* Further overwrites cross 0.45/1.45 and must still trigger GC. */
    for(int i = 1; i <= 7; i++) buffer_put(a, i, 'c', 1024);
    CHECK(test_force_flush(a, &worker)); drain();
    CHECK(thread_gc_block_rw[0] > 0 && !a->is_gc);
    for(int i = 1; i <= 7; i++) check_get(i, 'c');
}

static void gc_relaxed_block_test(void) {
    atomic_store(&database.gc_space.relaxed, true);
    struct subspace_info *a = group(0);
    stored_record_size = 3072;
    stored(a, 1, 1, 1);
    a->is_gc = true;
    garbadge_collection(a, &worker);
    CHECK(!queued_count && !a->is_gc && thread_gc_block_rw[0] == 0);
    /* A decision already made at the default threshold remains consistent
     * even if main has since published the relaxed threshold. */
    a->is_gc = true;
    garbadge_collection_with_fraction(a, &worker, 0.3 / 1.3);
    CHECK(queued_count == 1 && thread_gc_block_rw[0] == 1);
    drain();
    check_get(1, 'a');
}


/* Foreground writes exercise the production gate; buffer_put only builds
 * deliberately dormant groups without triggering policy during setup. */
static void foreground_put(key_type key, char byte, int record_size){
    char value[BLOCK_SIZE]; memset(value, byte, sizeof(value));
    insert(key, record_size - KV_RECORD_HEADER_SIZE, value, &worker, insert_breakdown);
}
static void allow_pressure(void){
    space_stats_nvm_start_at(&database, 0);
    atomic_store(&test_wbf.pressure, true);
}
/* The configured local threshold is strict, independently of KV size. */
static void flush_boundary_test(void){
    struct subspace_info *a = group(0);
    const unsigned records = BLOCK_SIZE * LIBDB_FLUSH_BLOCK_COUNT / 1024;
    CHECK(records >= 1 && records < SUBSPACE_MAX_KEY);
    for(unsigned i = 1; i < records; i++) foreground_put(i, 'a', 1024);
    CHECK(!queued_count && a->buffered_size == (records - 1) * 1024);
    foreground_put(records, 'b', 1024);
    CHECK(!queued_count && a->buffered_size == BLOCK_SIZE * LIBDB_FLUSH_BLOCK_COUNT);
    foreground_put(records + 1, 'c', 1024);
    CHECK(queued_count && a->flush_block_num == 1);
    drain();
    check_get(records, 'b'); check_get(records + 1, 'c');
}
static void flush_policy_boundary_test(void){
    struct subspace_info *a = group(0);
    const unsigned records = BLOCK_SIZE * LIBDB_FLUSH_BLOCK_COUNT / 1024;
    space_stats_nvm_start_at(&database, 0);
    CHECK(!space_stats_nvm_flush_allowed(&database));
    for(unsigned i = 1; i <= records + 1; i++) foreground_put(i, 'a', 1024);
    CHECK(!queued_count && a->buffered_size > BLOCK_SIZE * LIBDB_FLUSH_BLOCK_COUNT);
    allow_pressure();
    maintenance_poll(&worker); CHECK(!queued_count);
    foreground_put(1, 'b', 1024);
    CHECK(queued_count && a->flush_block_num == 1);
    drain(); check_get(1, 'b');
}
static void dynamic_cycle_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    space_stats_nvm_start_at(&database, 0);
    for(int i = 0; i < 48; i++) foreground_put(i + 1, 'a', 1024);
    for(int i = 0; i < 16; i++) foreground_put(i + 1001, 'b', 1024);
    uint64_t before = atomic_load(&test_wbf.used_slot_bytes);
    CHECK(before == LIBDB_NVM_FLUSH_HIGH_BYTES && !queued_count);
    space_stats_nvm_poll_at(&database, LIBDB_NVM_FLUSH_POLL_NS);
    CHECK(space_stats_nvm_flush_allowed(&database));
    for(int i = 0; i < 100; i++) maintenance_poll(&worker);
    check_get(1, 'a'); check_get(1001, 'b');
    CHECK(!queued_count && !worker.maintenance && a->buffered_size == 48 * 1024);
    foreground_put(1001, 'c', 1024); /* Equality stays buffered even under pressure. */
    CHECK(!queued_count && b->buffered_size == 16 * 1024);
    foreground_put(1, 'c', 1024); /* Only this new write triggers A. */
    CHECK(queued_count && a->flush_block_num == 1 && !b->flush_block_num);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == before);
    complete_at(0);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == before); /* Partial completion. */
    space_stats_nvm_stop(&database);
    drain(); maintenance_poll(&worker);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 16 * 1024);
    CHECK(!worker.maintenance && !queued_count);
    check_get(1, 'c'); check_get(1001, 'c');
    space_stats_nvm_start_at(&database, 2 * LIBDB_NVM_FLUSH_POLL_NS);
    CHECK(!space_stats_nvm_flush_allowed(&database));
    for(int i = 0; i < 48; i++) foreground_put(i + 1, 'd', 1024);
    CHECK(!queued_count);
    space_stats_nvm_poll_at(&database, 3 * LIBDB_NVM_FLUSH_POLL_NS);
    maintenance_poll(&worker); CHECK(!queued_count);
    foreground_put(1, 'd', 1024); CHECK(queued_count); drain();
    space_stats_nvm_poll_at(&database, 4 * LIBDB_NVM_FLUSH_POLL_NS);
    CHECK(!space_stats_nvm_flush_allowed(&database));
    check_get(1, 'd');
}
static void write_groups_test(void){
    struct subspace_info *groups[20];
    for(int g = 0; g < 20; g++){
        groups[g] = group(g * 1000);
        for(int k = 0; k < 5; k++) buffer_put(groups[g], g * 1000 + k + 1, 'a', BLOCK_SIZE);
    }
    allow_pressure();
    for(int i = 0; i < 100; i++){ maintenance_poll(&worker); check_get(1, 'a'); }
    CHECK(!queued_count && !worker.maintenance);
    foreground_put(1, 'b', BLOCK_SIZE);
    CHECK(queued_count == 5 && groups[0]->flush_block_num == 1);
    for(int i = 1; i < 20; i++) CHECK(!groups[i]->flush_block_num && groups[i]->buffered_size == 5 * BLOCK_SIZE);
    drain(); maintenance_poll(&worker);
    CHECK(!queued_count && atomic_load(&test_wbf.used_slot_bytes) == 19 * 5 * BLOCK_SIZE);
    check_get(1, 'b'); check_get(1001, 'a');
}
static void write_busy_test(void){
    allow_pressure();
    struct task *held[TASKS];
    for(int i = 0; i < TASKS; i++){ held[i] = get_free_task(&ns, 0); CHECK(held[i]); }
    for(int kind = 0; kind < 4; kind++){
        struct subspace_info *a = group(kind * 1000);
        for(int k = 1; k <= 20; k++) buffer_put(a, kind * 1000 + k, 'a', 1024);
        if(kind == 0) a->flush_block_num = 1;
        if(kind == 1) a->is_spliting = true;
        if(kind == 2) a->is_gc = true;
        if(kind == 3) a->is_merging = true;
        uint32_t polls = completion_polls;
        foreground_put(kind * 1000 + 1, 'b', 1024);
        CHECK(completion_polls == polls && !queued_count && !worker.maintenance);
        CHECK(a->buffered_size == 20 * 1024);
        check_get(kind * 1000 + 1, 'b'); /* Readable while the group is still busy. */
        a->flush_block_num = 0; a->is_spliting = a->is_gc = a->is_merging = false;
        maintenance_poll(&worker); CHECK(!queued_count);
        check_get(kind * 1000 + 1, 'b');
    }
    for(int i = 0; i < TASKS; i++) free_task(held[i]);
    for(int kind = 0; kind < 4; kind++){
        foreground_put(kind * 1000 + 1, 'c', 1024);
        CHECK(queued_count); drain(); maintenance_poll(&worker); CHECK(!queued_count);
        check_get(kind * 1000 + 1, 'c');
    }
}
static void write_merge_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    for(int i = 0; i < 20; i++){
        buffer_put(a, i + 1, 'a', 1024); buffer_put(b, i + 1001, 'b', 1024);
    }
    allow_pressure();
    db_delete(999, &worker); drain(); maintenance_poll(&worker);
    CHECK(thread_subspaces[0] == 1 && a->buffered_size == 40 * 1024);
    CHECK(!queued_count && !worker.maintenance);
    foreground_put(1, 'c', 1024); CHECK(queued_count); drain();
    CHECK(!a->buffered_size && !queued_count);
    check_get(1, 'c'); check_get(1001, 'b');
}
static void write_split_test(void){
    struct subspace_info *a = group(0);
    for(int i = 0; i < 300; i++) buffer_put(a, i + 1, 's', BLOCK_SIZE);
    allow_pressure();
    for(int i = 0; i < 100; i++) maintenance_poll(&worker);
    CHECK(thread_subspaces[0] == 1 && !queued_count && a->buffered_size == 300 * BLOCK_SIZE);
    foreground_put(1, 's', BLOCK_SIZE);
    CHECK(a->flush_block_num == 1 && a->buffered_size == (300 - 64) * BLOCK_SIZE);
    drain(); maintenance_poll(&worker); CHECK(!queued_count);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == (300 - 64) * BLOCK_SIZE);
    /* Continue only through writes, including writes after completed splits. */
    for(int round = 0; round < 4; round++) for(int k = 1; k <= 300; k++){
        foreground_put(k, 's', BLOCK_SIZE);
        drain(); maintenance_poll(&worker); CHECK(!queued_count);
    }
    CHECK(thread_subspaces[0] > 1 && !worker.maintenance);
    for(int k = 1; k <= 300; k++) check_get(k, 's');
}
static void write_fixed_test(void){
    struct subspace_info *a = group(0), *b = group(1001);
    CHECK(space_stats_nvm_flush_allowed(&database)); /* load/fixed phase */
    for(int i = 1; i <= 20; i++){ buffer_put(a, i, 'a', 1024); buffer_put(b, i + 1000, 'b', 1024); }
    maintenance_poll(&worker); CHECK(!queued_count);
    foreground_put(1, 'c', 1024); CHECK(queued_count && !b->flush_block_num);
    drain(); maintenance_poll(&worker); CHECK(!queued_count && b->buffered_size == 20 * 1024);
    database.recovery_mode = 1;
    foreground_put(1001, 'd', 1024); CHECK(queued_count); drain();
    check_get(1, 'c'); check_get(1001, 'd');
}

static struct subspace_info *wait_group;
static int wait_action;
static uint32_t wait_block_start;
static void wait_poll_hook(void){
    /* Waiting must leave the buffer attached and SSD allocation untouched. */
    CHECK(wait_group->buffered_size == 20 * 1024 && !wait_group->flush_block_num);
    CHECK(next_block == wait_block_start && !pending_writes && !worker.maintenance);
    CHECK(!wait_group->buffer_index->next);
    check_get(1, 'b'); /* The foreground update precedes the task wait. */
    CHECK(completion_polls < 8);
    if(completion_polls != 2) return;
    if(wait_action == 1) atomic_store(&test_wbf.pressure, false);
    if(wait_action == 2) space_stats_nvm_stop(&database);
    if(wait_action == 3) wait_group->is_gc = true;
    if(wait_action == 4) benchmark_completed = 1;
    if(wait_action == 5) wait_group->buffered_size = 16 * 1024;
}
static void release_task_cb(void *ctx, const struct spdk_nvme_cpl *cpl){
    CHECK(!spdk_nvme_cpl_is_error(cpl)); free_task(ctx);
}
static void write_task_wait_case(int action){
    allow_pressure(); wait_action = action;
    struct subspace_info *a = wait_group = group(0);
    for(int k = 1; k <= 20; k++) buffer_put(a, k, 'a', 1024);
    struct task *held[TASKS];
    for(int i = 0; i < TASKS; i++){ held[i] = get_free_task(&ns, 0); CHECK(held[i]); }
    CHECK(!spdk_nvme_ns_cmd_read(ns.ns, qpairs[0], held[0]->buffer, 0, 1, release_task_cb, held[0], 0));
    wait_block_start = next_block;
    completion_polls = 0; poll_delay = 2; completion_hook = wait_poll_hook;
    foreground_put(1, 'b', 1024);
    completion_hook = NULL;
    if(action == 0){
        CHECK(completion_polls == 3 && a->flush_block_num == 1 && pending_writes == 5);
    } else {
        CHECK(completion_polls == 2 && !pending_writes && !a->flush_block_num);
        CHECK(next_block == wait_block_start);
    }
    a->is_gc = false; benchmark_completed = 0;
    if(action == 5) a->buffered_size = 20 * 1024;
    drain();
    for(int i = 1; i < TASKS; i++) free_task(held[i]);
    if(action){
        allow_pressure(); maintenance_poll(&worker); CHECK(!queued_count);
        foreground_put(1, 'b', 1024); CHECK(queued_count); drain();
    }
    check_get(1, 'b'); CHECK(!worker.maintenance);
}
static void write_task_wait_test(void){ write_task_wait_case(0); }
static void write_wait_pressure_test(void){ write_task_wait_case(1); }
static void write_wait_stop_test(void){ write_task_wait_case(2); }
static void write_wait_busy_test(void){ write_task_wait_case(3); }
static void write_wait_end_test(void){ write_task_wait_case(4); }
static void write_wait_threshold_test(void){ write_task_wait_case(5); }
static void wait_error_exit_check(void){
    CHECK(!wait_group->flush_block_num && wait_group->buffered_size == 20 * 1024);
    CHECK(!pending_writes && next_block == wait_block_start && !worker.maintenance);
}
static void write_wait_error_test(void){
    allow_pressure(); wait_group = group(0);
    for(int k = 1; k <= 20; k++) buffer_put(wait_group, k, 'a', 1024);
    for(int i = 0; i < TASKS; i++) CHECK(get_free_task(&ns, 0));
    wait_block_start = next_block;
    pid_t pid = fork(); CHECK(pid >= 0);
    if(!pid){
        CHECK(!atexit(wait_error_exit_check)); fail_completion_poll = true;
        foreground_put(1, 'b', 1024); _exit(42);
    }
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}
static void wide_buffer_test(void){
    struct subspace_info *a = group(0);
    space_stats_nvm_start_at(&database, 0);
    a->buffered_size = UINT32_MAX - 31ULL;
    buffered(a, 1, 1);
    CHECK(a->buffered_size == UINT64_C(4294967328));
    if(LIBDB_DYNAMIC_FLUSH_ENABLED) CHECK(subspace_maybe_flush(a, &worker) && !queued_count);
    a->buffered_size = RECORD_SIZE;
}

/* Finish only GC reads, leaving every new SSD write pending. */
static void gc_stage_fraction_test(struct subspace_info *g, double fraction){
    g->is_gc = true;
    garbadge_collection_with_fraction(g, &worker, fraction);
    CHECK(queued_count);
    while(queued_count && !queued[0].write) complete_at(0);
    CHECK(g->is_gc && pending_writes && commits == 0 && reclaimed == 0);
    for(int i = 0; i < queued_count; i++) CHECK(queued[i].flags == 0);
}
static void gc_stage_test(struct subspace_info *g){
    gc_stage_fraction_test(g, 0.0);
}
static void gc_check_layout(struct subspace_info *g){
    uint64_t bytes = 0;
    key_type last = 0;
    uint32_t keys = 0, blocks[(SUBSPACE_MAX_SLOT * SLOT_MAX_ELEMENT)], count = 0;
    for(uint32_t i = 0; i < g->slot_num; i++){
        CHECK(g->slot_key_num[i] && g->min_key[i] == subspace_slot_elements(g, i)[0].key);
        CHECK(subspace_slot_boundary(g, i) == g->boundary);
        for(uint32_t j = 0; j < g->slot_key_num[i]; j++){
            struct slot_element e = subspace_slot_elements(g, i)[j];
            CHECK(!keys || e.key > last); last = e.key; keys++;
            CHECK(e.addr.off + e.addr.size <= BLOCK_SIZE && allocated[e.addr.block]);
            struct kv_record_view view;
            CHECK(!kv_record_decode_for_key(disk[e.addr.block] + e.addr.off, e.addr.size, e.key, &view));
            if(!block_exist(e.addr.block, blocks, count)) blocks[count++] = e.addr.block;
            bytes += e.addr.size;
        }
    }
    CHECK(keys == g->key_num && count == g->total_block_num);
    CHECK(g->redundant_size == (uint64_t)count * BLOCK_SIZE - bytes);
    for(uint32_t i = 0; i < count; i++){
        unsigned refs = 0;
        for(uint32_t s = 0; s < g->slot_num; s++)
            for(uint32_t j = 0; j < g->slot_key_num[s]; j++)
                refs += subspace_slot_elements(g, s)[j].addr.block == blocks[i];
        CHECK(g_map[blocks[i]] == refs);
    }
}
static void gc_local_plain_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = BLOCK_SIZE; stored(a, 1, 1, 1);
    stored_record_size = 1024; stored(a, 10, 1, 1);
    stored_record_size = BLOCK_SIZE; stored(a, 1000, 1, 1);
    PMEMoid slots[3]; memcpy(slots, a->slot, sizeof(slots));
    struct nvm_index_slot left = *(struct nvm_index_slot *)pmemobj_direct(slots[0]);
    struct nvm_index_slot right = *(struct nvm_index_slot *)pmemobj_direct(slots[2]);
    gc_stage_fraction_test(a, 0.5); drain();
    CHECK(a->slot_num == 3 && a->key_num == 3 && !a->buffered_size);
    CHECK(last_commit_count == 1 && OID_EQUALS(last_commit_slots[0], slots[1]));
    for(unsigned i = 0; i < 3; i++) CHECK(OID_EQUALS(slots[i], a->slot[i]) && a->slot_key_num[i] == 1);
    CHECK(!memcmp(&left, pmemobj_direct(slots[0]), sizeof(left)));
    CHECK(!memcmp(&right, pmemobj_direct(slots[2]), sizeof(right)));
    check_get(10, 'a'); gc_check_layout(a);
}
static void gc_local_slot_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = BLOCK_SIZE; stored(a, 1, 1, 1);
    stored_record_size = 1024; stored(a, 10, 6, 6);
    PMEMoid first = a->slot[0], changed = a->slot[1];
    struct nvm_index_slot original = *(struct nvm_index_slot *)pmemobj_direct(first);
    struct nvm_index_slot before = *(struct nvm_index_slot *)pmemobj_direct(changed);
    gc_stage_fraction_test(a, 0.5);
    CHECK(!memcmp(&before, pmemobj_direct(changed), sizeof(before)));
    drain();
    CHECK(last_commit_count == 1 && OID_EQUALS(last_commit_slots[0], changed));
    CHECK(a->slot_num == 2 && OID_EQUALS(a->slot[0], first) && OID_EQUALS(a->slot[1], changed));
    CHECK(!memcmp(&original, pmemobj_direct(first), sizeof(original)));
    CHECK(a->slot_key_num[0] == 1 && a->min_key[0] == 1 && a->pool[0] == pool);
    CHECK(a->slot_key_num[1] == 4 && a->min_key[1] == 10);
    check_get(1, 'a'); check_get(14, 'a'); gc_check_layout(a);
}
static void gc_local_split_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = BLOCK_SIZE; stored(a, 1, 1, 1);
    stored_record_size = 1024; stored(a, 100, SLOT_MAX_ELEMENT, SLOT_MAX_ELEMENT);
    stored_record_size = BLOCK_SIZE; stored(a, 1000, 1, 1);
    PMEMoid left = a->slot[0], target = a->slot[1], right = a->slot[2];
    struct nvm_index_slot left_image = *(struct nvm_index_slot *)pmemobj_direct(left);
    struct nvm_index_slot right_image = *(struct nvm_index_slot *)pmemobj_direct(right);
    CHECK(SLOT_MAX_ELEMENT % 4);
    buffer_put(a, 150, 'b', BLOCK_SIZE - (SLOT_MAX_ELEMENT % 4) * 1024);
    gc_stage_fraction_test(a, 0.5); drain();
    CHECK(a->slot_num == 4 && OID_EQUALS(a->slot[0], left) && OID_EQUALS(a->slot[1], target));
    CHECK(OID_EQUALS(a->slot[3], right));
    CHECK(last_commit_count == 2 && OID_EQUALS(last_commit_slots[0], target));
    CHECK(OID_EQUALS(last_commit_slots[1], a->slot[2]));
    CHECK(!memcmp(&left_image, pmemobj_direct(left), sizeof(left_image)));
    CHECK(!memcmp(&right_image, pmemobj_direct(right), sizeof(right_image)));
    CHECK(a->slot_key_num[1] == (SLOT_MAX_ELEMENT + 1) / 2);
    CHECK(a->slot_key_num[2] == (SLOT_MAX_ELEMENT + 1) - a->slot_key_num[1]);
    check_get(150, 'b'); gc_check_layout(a);
}
static void gc_local_capacity_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, SLOT_MAX_ELEMENT, SLOT_MAX_ELEMENT);
    stored_record_size = BLOCK_SIZE;
    for(unsigned i = 1; i < SUBSPACE_MAX_SLOT; i++) stored(a, i * 100, 1, 1);
    CHECK(a->key_num < SUBSPACE_MAX_KEY && SLOT_MAX_ELEMENT % 4);
    PMEMoid slots[SUBSPACE_MAX_SLOT]; memcpy(slots, a->slot, sizeof(slots));
    buffer_put(a, 50, 'b', BLOCK_SIZE - (SLOT_MAX_ELEMENT % 4) * 1024);
    PMEMoid candidate = write_buffer_index_search(a->buffer_index, 50)->id;
    size_t allocated_slots = slot_cursor;
    gc_stage_fraction_test(a, 0.5); drain();
    CHECK(a->slot_num == SUBSPACE_MAX_SLOT && slot_cursor == allocated_slots);
    CHECK(!a->buffer_index->next && !nvm_index_search(a, 50).size);
    CHECK(OID_EQUALS(candidate, write_buffer_index_search(a->buffer_index, 50)->id));
    CHECK(last_commit_count == 1 && OID_EQUALS(last_commit_slots[0], slots[0]));
    for(unsigned i = 0; i < SUBSPACE_MAX_SLOT; i++) CHECK(OID_EQUALS(slots[i], a->slot[i]));
    CHECK(a->slot_key_num[0] == SLOT_MAX_ELEMENT - SLOT_MAX_ELEMENT % 4);
    check_get(50, 'b'); check_get(SLOT_MAX_ELEMENT, 'a'); gc_check_layout(a);
}
static void gc_local_insert_first_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 10, 6, 6);
    stored_record_size = BLOCK_SIZE; stored(a, 1000, 1, 1);
    PMEMoid first = a->slot[0], last = a->slot[1];
    buffer_put(a, 1, 'b', 2048);
    gc_stage_fraction_test(a, 0.5); drain();
    CHECK(a->slot_num == 2 && a->min_key[0] == 1 && a->min_key[1] == 1000);
    CHECK(OID_EQUALS(first, a->slot[0]) && OID_EQUALS(last, a->slot[1]));
    CHECK(last_commit_count == 1 && OID_EQUALS(last_commit_slots[0], first));
    check_get(1, 'b'); check_get(15, 'a'); gc_check_layout(a);
}

static skipnode *gc_cached_node(struct subspace_info *g, key_type key){
    skipnode *n = g->kvcache->head->forward[0];
    while(n && n->key < key) n = n->forward[0];
    return n && n->key == key ? n : NULL;
}
static void gc_cache_put(struct subspace_info *g, key_type key, size_t record_size){
    char value[BLOCK_SIZE]; memset(value, 'a', sizeof(value));
    kvcache_insert(g->kvcache, worker.lru_list, key, value,
                   record_size - KV_RECORD_HEADER_SIZE, 0);
}
static void gc_cache_preserve_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = BLOCK_SIZE; stored(a, 1, 1, 1);
    stored_record_size = 1024; stored(a, 10, 6, 6);
    key_type keys[] = {1, 10, 11, 12, 13, 14, 15};
    skipnode *nodes[7], images[7];
    uint32_t old_blocks[7];
    for(int i = 0; i < 7; i++){
        gc_cache_put(a, keys[i], i ? 1024 : BLOCK_SIZE);
        nodes[i] = gc_cached_node(a, keys[i]); CHECK(nodes[i]);
        old_blocks[i] = nvm_index_search(a, keys[i]).block;
    }
    for(int i = 0; i < 7; i++) images[i] = *nodes[i];
    lru_list lru_before = *worker.lru_list;
    track_gc_cache = true;
    gc_stage_fraction_test(a, 0.5);
    for(int i = 0; i < 7; i++) CHECK(g_map[old_blocks[i]] == 1);
    drain();
    CHECK(!gc_cache_deletes);
    CHECK(!memcmp(&lru_before, worker.lru_list, sizeof(lru_before)));
    for(int i = 0; i < 7; i++){
        CHECK(gc_cached_node(a, keys[i]) == nodes[i]);
        CHECK(!memcmp(nodes[i], &images[i], sizeof(images[i])));
        for(int j = 0; j < LIBDB_PMEM_VALUE_PREFIX_SIZE; j++) CHECK(nodes[i]->value[j] == 'a');
        CHECK(g_map[old_blocks[i]] == (i == 0));
    }
    CHECK(allocated[old_blocks[0]] && reclaimed == 6);
    CHECK(atomic_load(&g_used_data_blocks) == 2);
    gc_check_layout(a);
    for(int i = 0; i < 7; i++) check_get(keys[i], 'a');
}
static void gc_cache_fill_scope_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    stored_record_size = BLOCK_SIZE; stored(a, 500, 1, 1);
    for(int i = 1; i <= 6; i++) gc_cache_put(a, i, 1024);
    gc_cache_put(a, 100, 1024); gc_cache_put(a, 500, BLOCK_SIZE);
    skipnode *kept = gc_cached_node(a, 2), *unselected = gc_cached_node(a, 500);
    buffer_put(a, 1, 'b', 1024); buffer_put(a, 100, 'c', 1024);
    track_gc_cache = true;
    gc_stage_fraction_test(a, 0.5);
    CHECK(gc_cached_node(a, 1) && gc_cached_node(a, 100) && !gc_cache_deletes);
    complete_at(1); CHECK(!gc_cache_deletes && a->buffer_index->next);
    drain();
    CHECK(gc_cache_deletes == 2 && gc_deleted_keys[0] == 1 && gc_deleted_keys[1] == 100);
    CHECK(!gc_cached_node(a, 1) && !gc_cached_node(a, 100));
    CHECK(gc_cached_node(a, 2) == kept && gc_cached_node(a, 500) == unselected);
    check_get(1, 'b'); check_get(100, 'c'); check_get(500, 'a'); gc_check_layout(a);
}
/* Verify the allocation at the last read completion, before any output write
 * completes. Every byte outside live records must retain zero padding. */
static void gc_check_allocation(struct subspace_info *a, unsigned source_blocks){
    a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0);
    CHECK(queued_count == source_blocks);
    while(queued_count > 1) complete_at(0);
    capture_gc_allocation = true;
    complete_at(0);
    CHECK(!capture_gc_allocation && gc_allocation_bytes == (size_t)source_blocks * BLOCK_SIZE);
    drain();
    for(uint32_t b = 1; b < next_block; b++){
        if(!allocated[b]) continue;
        bool used[BLOCK_SIZE] = {0};
        for(uint32_t i = 0; i < a->slot_num; i++)
            for(uint32_t j = 0; j < a->slot_key_num[i]; j++){
                struct address_type addr = subspace_slot_elements(a, i)[j].addr;
                if(addr.block == b) memset(used + addr.off, 1, addr.size);
            }
        for(unsigned off = 0; off < BLOCK_SIZE; off++) if(!used[off]) CHECK(disk[b][off] == 0);
    }
    gc_check_layout(a);
}
static void gc_allocation_single_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 1, 1);
    gc_check_allocation(a, 1); check_get(1, 'a');
}
static void gc_allocation_full_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = BLOCK_SIZE; stored(a, 1, 3, 3);
    gc_check_allocation(a, 3);
}
static void gc_allocation_variable_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 3072; stored(a, 1, 1, 1);
    stored_record_size = 2048; stored(a, 2, 1, 1);
    stored_record_size = 1024; stored(a, 3, 1, 1);
    buffer_put(a, 100, 'b', 1024); /* Exact fill, with padding in the first block. */
    gc_check_allocation(a, 3); check_get(100, 'b');
}

static void gc_tail_return_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 1);
    PMEMoid retired = a->slot[5], retained[4];
    memcpy(retained, a->slot, sizeof(retained));
    /* Pressure does not veto return, and a stale block-count estimate must not
     * introduce zero/uninitialized candidates. */
    a->total_block_num += 3;
    atomic_store(&test_wbf.pressure, true);
    gc_stage_test(a);
    CHECK(pending_writes == 1 && a->buffered_size == 2048);
    CHECK(a->key_num == 6 && nvm_index_search(a, 5).size == 1024);
    CHECK(!worker.maintenance);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 2048);
    struct write_buffer_index_node *n = write_buffer_index_search(a->buffer_index, 5);
    struct kv_record_view v;
    CHECK(n && !kv_record_decode(pmemobj_direct(n->id), n->size, &v));
    CHECK(v.version == 5 && v.value_size == 1024 - KV_RECORD_HEADER_SIZE);
    check_get(5, 'a');
    buffer_put(a, 6, 'c', 1024); /* overwrite while GC SSD write is pending */
    drain();
    CHECK(a->key_num == 4 && a->slot_num == 4 && a->total_block_num == 1);
    for(int i = 0; i < 4; i++) CHECK(OID_EQUALS(a->slot[i], retained[i]) && a->slot_key_num[i] == 1);
    CHECK(!((struct nvm_index_slot *)pmemobj_direct(retired))->entries[0].addr.size);
    CHECK(!nvm_index_search(a, 5).size && !a->is_gc && reclaimed == 6);
    CHECK(!worker.maintenance);
    maintenance_poll(&worker); CHECK(!queued_count);
    check_get(5, 'a'); check_get(6, 'c'); gc_check_layout(a);
}
static void gc_tail_fill_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    buffer_put(a, 1, 'b', 1024); buffer_put(a, 100, 'c', 1024);
    PMEMoid frozen = write_buffer_index_search(a->buffer_index, 1)->id;
    char value[1024]; memset(value, 'a', sizeof(value));
    kvcache_insert(a->kvcache, worker.lru_list, 1, value, sizeof(value), 0);
    gc_stage_test(a);
    CHECK(pending_writes == 2 && a->buffered_size == 0 && a->buffer_index->next);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 2048);
    check_get(1, 'b'); check_get(100, 'c');
    buffer_put(a, 1, 'd', 1024);
    complete_at(1); /* out-of-order write completion must not free prefix */
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 3072 && *(uint16_t *)pmemobj_direct(frozen));
    drain();
    CHECK(a->key_num == 7 && a->total_block_num == 2 && !a->buffer_index->next);
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 1024 && !*(uint16_t *)pmemobj_direct(frozen));
    check_get(1, 'd'); check_get(100, 'c'); gc_check_layout(a);
}
static void gc_tail_prefix_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    buffer_put(a, 100, 'b', 1024); buffer_put(a, 101, 'c', 1536);
    buffer_put(a, 102, 'd', 1024); /* would fit if the oversized key were skipped */
    PMEMoid first = write_buffer_index_search(a->buffer_index, 100)->id;
    gc_stage_test(a); drain();
    CHECK(a->key_num == 4 && a->buffered_size == 5632 && !a->buffer_index->next);
    CHECK(OID_EQUALS(first, write_buffer_index_search(a->buffer_index, 100)->id));
    for(int i = 100; i <= 102; i++) check_get(i, 'b' + i - 100);
    gc_check_layout(a);
}
static void gc_tail_insufficient_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    buffer_put(a, 100, 'b', 1024);
    gc_stage_test(a); drain();
    CHECK(a->buffered_size == 3072 && a->key_num == 4);
    check_get(100, 'b'); gc_check_layout(a);
}
static void gc_tail_versions_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0);
    buffer_put(a, 5, 'b', 3072); /* newer active value; too large to fill */
    PMEMoid id = write_buffer_index_search(a->buffer_index, 5)->id;
    drain();
    CHECK(OID_EQUALS(id, write_buffer_index_search(a->buffer_index, 5)->id));
    CHECK(atomic_load(&test_wbf.used_slot_bytes) == 5120);
    CHECK(a->buffered_size == 4096 && a->key_num == 4);
    check_get(5, 'b'); check_get(6, 'a'); gc_check_layout(a);
}
static void gc_tail_equal_older_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    buffer_put(a, 5, 'a', 3072); buffer_put(a, 6, 'b', 1024);
    struct write_buffer_index_node *five = write_buffer_index_search(a->buffer_index, 5);
    struct write_buffer_index_node *six = write_buffer_index_search(a->buffer_index, 6);
    ((struct kv_pmem_record *)pmemobj_direct(five->id))->version = 5;
    ((struct kv_pmem_record *)pmemobj_direct(six->id))->version = 1;
    PMEMoid equal = five->id, older = six->id;
    gc_stage_test(a); drain();
    CHECK(OID_EQUALS(equal, write_buffer_index_search(a->buffer_index, 5)->id));
    CHECK(!OID_EQUALS(older, write_buffer_index_search(a->buffer_index, 6)->id));
    CHECK(!*(uint16_t *)pmemobj_direct(older));
    check_get(5, 'a'); check_get(6, 'a'); gc_check_layout(a);
}
static void gc_tail_capacity_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 64; stored(a, 1, 127, SLOT_MAX_ELEMENT);
    buffer_put(a, 1000, 'b', 64); buffer_put(a, 1001, 'c', 64);
    stored_record_size = 4032; stored(a, 2000, 1, 1);
    /* Packed tail has 64 bytes free. Adding a new SSD key exceeds 128. */
    gc_stage_test(a); drain();
    CHECK(a->key_num == 127 && a->buffered_size == 4160);
    CHECK(write_buffer_index_search(a->buffer_index, 1000));
    CHECK(!nvm_index_search(a, 2000).size); check_get(2000, 'a'); gc_check_layout(a);
}
static void gc_tail_prune_output_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 3000; stored(a, 1, 2, 2);
    buffer_put(a, 1, 'b', 1096);
    gc_stage_test(a); CHECK(pending_writes == 1); drain();
    CHECK(a->total_block_num == 1 && a->redundant_size == 0 && a->key_num == 2);
    check_get(1, 'b'); check_get(2, 'a'); gc_check_layout(a);
}
static void gc_tail_unselected_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 4096; stored(a, 1, 1, 1);
    uint32_t unselected = nvm_index_search(a, 1).block;
    stored_record_size = 1024; stored(a, 2, 6, 6);
    buffer_put(a, 1, 'b', 2048);
    a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.5);
    CHECK(queued_count == 6); drain();
    CHECK(!allocated[unselected] && reclaimed == 7);
    CHECK(a->key_num == 7 && a->total_block_num == 2);
    check_get(1, 'b'); gc_check_layout(a);
}
static void gc_tail_full_single_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 8, 8);
    buffer_put(a, 100, 'b', 1024);
    gc_stage_test(a); drain();
    CHECK(a->buffered_size == 1024 && a->key_num == 8 && a->total_block_num == 2);
    struct subspace_info *b = group(1000);
    stored(b, 1001, 2, 2);
    b->is_gc = true; garbadge_collection_with_fraction(b, &worker, 0.0); drain();
    CHECK(!b->buffered_size && b->key_num == 2 && b->total_block_num == 1);
    gc_check_layout(a); gc_check_layout(b);
}
static void gc_tail_no_flush_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    for(int k = 100; k < 105; k++) buffer_put(a, k, 'b', BLOCK_SIZE);
    gc_stage_test(a); drain();
    CHECK(a->buffered_size > BLOCK_SIZE * Flush_Blcok_Num);
    CHECK(!worker.maintenance);
    space_stats_nvm_start_at(&database, 0);
    atomic_store(&test_wbf.pressure, true);
    maintenance_poll(&worker); CHECK(!queued_count);
    space_stats_nvm_stop(&database); maintenance_poll(&worker);
    CHECK(!queued_count && !worker.maintenance);
}
static void gc_tail_grow_slots_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, SLOT_MAX_ELEMENT, SLOT_MAX_ELEMENT);
    buffer_put(a, 100, 'b', BLOCK_SIZE - (SLOT_MAX_ELEMENT % 4) * 1024);
    gc_stage_test(a); drain();
    CHECK(a->slot_num == 2 && a->key_num == SLOT_MAX_ELEMENT + 1);
    CHECK(!atomic_load(&test_wbf.used_slot_bytes));
    check_get(100, 'b'); gc_check_layout(a);
}
static void gc_tail_write_trigger_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    for(int k = 100; k < 105; k++) buffer_put(a, k, 'b', BLOCK_SIZE);
    gc_stage_test(a);
    foreground_put(104, 'c', BLOCK_SIZE); /* Busy GC: write cannot schedule a retry. */
    drain(); maintenance_poll(&worker);
    CHECK(!worker.maintenance && !a->is_gc && !queued_count);
    CHECK(a->buffered_size > BLOCK_SIZE * Flush_Blcok_Num);
    foreground_put(104, 'd', BLOCK_SIZE); CHECK(queued_count); drain();
    check_get(5, 'a'); check_get(104, 'd');
}
static void gc_tail_read_scan_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    uint32_t pinned = nvm_index_search(a, 5).block;
    key_type keys[6]; char *values[6] = {0}; int sizes[6];
    struct scan_kv result = {6, keys, values, sizes};
    CHECK(scan(1, 6, (char *)&result, &worker, 0) == 0);
    int reads = queued_count;
    a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0);
    while(queued_count > reads) complete_at(queued_count - 1);
    CHECK(!a->is_gc && allocated[pinned] && worker.read_pins);
    drain(); CHECK(last_result && last_result->size == 6 && !allocated[pinned]);
    for(int i = 0; i < 6; i++){ CHECK(keys[i] == i + 1 && values[i][0] == 'a'); free(values[i]); }
    free(last_result); last_result = NULL;
    CHECK(!worker.read_pins && !a->read_refs);
    memset(values, 0, sizeof(values));
    CHECK(scan(1, 6, (char *)&result, &worker, 0) == 0); drain();
    CHECK(last_result && last_result->size == 6);
    for(int i = 0; i < 6; i++){ CHECK(keys[i] == i + 1 && values[i][0] == 'a'); free(values[i]); }
    free(last_result); last_result = NULL; gc_check_layout(a);
}
static void gc_failure_exit_check(void){
    struct subspace_info *a = dram_index_search(hybrid.dram_index, 0, NULL, NULL);
    char ok = a->is_gc && reclaimed == 0 && commits == 0 && a->key_num >= 6;
    CHECK(write(failure_exit_fd, &ok, 1) == 1);
}
/* Two full source blocks can require three blocks when variable-length
 * records are repacked in key order. Match the original allocation size, but
 * fail at its boundary instead of overwriting the packed allocation. */
static void gc_allocation_boundary_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 3072; stored(a, 1, 2, 2);
    stored_record_size = 512; stored(a, 3, 4, 4);
    struct slot_element *large = subspace_slot_elements(a, 0);
    struct slot_element *small = subspace_slot_elements(a, 1);
    for(int i = 0; i < 4; i++){
        uint32_t old = small[i].addr.block;
        uint32_t target = large[i / 2].addr.block;
        unsigned off = 3072 + (i % 2) * 512;
        memcpy(disk[target] + off, disk[old], 512);
        small[i].addr.block = target; small[i].addr.off = off;
        block_ref_add(target); free_block(&old, 1, NULL, NULL);
    }
    pmemobj_persist(pool, small, 4 * sizeof(*small));
    a->total_block_num = 2; reclaimed = 0;
    int pipes[2]; CHECK(!pipe(pipes));
    pid_t pid = fork(); CHECK(pid >= 0);
    if(!pid){
        close(pipes[0]); failure_exit_fd = pipes[1]; atexit(gc_failure_exit_check);
        a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0);
        CHECK(queued_count == 2);
        complete_at(0); capture_gc_allocation = true;
        drain(); _exit(42);
    }
    close(pipes[1]); char ok = 0; CHECK(read(pipes[0], &ok, 1) == 1 && ok); close(pipes[0]);
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
    CHECK(!commits && !reclaimed);
}
static void gc_tail_failure_test(void){
    struct subspace_info *a = group(0);
    stored_record_size = 1024; stored(a, 1, 6, 6);
    for(int kind = 0; kind < 8; kind++){
        int pipes[2]; CHECK(!pipe(pipes));
        pid_t pid = fork(); CHECK(pid >= 0);
        if(!pid){
            close(pipes[0]); failure_exit_fd = pipes[1]; atexit(gc_failure_exit_check);
            if(kind == 0) fail_read = true;
            if(kind == 1) fail_write = true;
            if(kind == 2) fail_commit = true;
            if(kind == 3) fail_submit = true;
            if(kind == 4) fail_wbf = true;
            if(kind == 5) fail_space_at = next_block;
            if(kind == 6){
                /* Fill grows a full slot by one, forcing slot allocation. */
                stored_record_size = 1024; stored(a, 7, SLOT_MAX_ELEMENT - 6, SLOT_MAX_ELEMENT - 6);
                buffer_put(a, 100, 'b', BLOCK_SIZE - (SLOT_MAX_ELEMENT % 4) * 1024);
                fail_slot_alloc = true;
                /* Keep the same key count but one fully occupied source slot. */
                struct slot_element all[SLOT_MAX_ELEMENT];
                int n = 0;
                for(unsigned i = 0; i < a->slot_num; i++)
                    for(unsigned j = 0; j < a->slot_key_num[i]; j++) all[n++] = subspace_slot_elements(a, i)[j];
                memcpy(subspace_slot_elements(a, 0), all, sizeof(all));
                a->slot_num = 1; a->slot_key_num[0] = SLOT_MAX_ELEMENT;
            }
            a->is_gc = true; garbadge_collection_with_fraction(a, &worker, 0.0);
            if(kind == 7){
                while(queued_count > 1) complete_at(0);
                capture_gc_allocation = fail_gc_allocation = true;
            }
            drain(); _exit(42);
        }
        close(pipes[1]); char ok = 0; CHECK(read(pipes[0], &ok, 1) == 1 && ok); close(pipes[0]);
        int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
    }
}

struct test_case { const char *name; void (*fn)(void); };
int main(void){
    struct test_case cases[] = {
        {"flush-boundary", flush_boundary_test},
        {"gc-cache-preserve", gc_cache_preserve_test}, {"gc-cache-fill-scope", gc_cache_fill_scope_test},
        {"gc-allocation-single", gc_allocation_single_test}, {"gc-allocation-full", gc_allocation_full_test},
        {"gc-allocation-variable", gc_allocation_variable_test}, {"gc-allocation-boundary", gc_allocation_boundary_test},
        {"gc-local-plain", gc_local_plain_test}, {"gc-local-slot", gc_local_slot_test}, {"gc-local-split", gc_local_split_test},
        {"gc-local-capacity", gc_local_capacity_test}, {"gc-local-insert-first", gc_local_insert_first_test},
        {"gc-tail-return", gc_tail_return_test}, {"gc-tail-fill", gc_tail_fill_test},
        {"gc-tail-prefix", gc_tail_prefix_test}, {"gc-tail-insufficient", gc_tail_insufficient_test},
        {"gc-tail-versions", gc_tail_versions_test}, {"gc-tail-equal-older", gc_tail_equal_older_test},
        {"gc-tail-capacity", gc_tail_capacity_test}, {"gc-tail-prune", gc_tail_prune_output_test},
        {"gc-tail-unselected", gc_tail_unselected_test}, {"gc-tail-full-single", gc_tail_full_single_test},
        {"gc-tail-no-flush", gc_tail_no_flush_test}, {"gc-tail-grow-slots", gc_tail_grow_slots_test},
        {"gc-tail-write-trigger", gc_tail_write_trigger_test}, {"gc-tail-read-scan", gc_tail_read_scan_test},
        {"gc-tail-failures", gc_tail_failure_test},
#if LIBDB_DYNAMIC_FLUSH_ENABLED
        {"flush-policy-boundary", flush_policy_boundary_test},
        {"dynamic-cycle", dynamic_cycle_test},
        {"write-wait-pressure", write_wait_pressure_test}, {"write-wait-stop", write_wait_stop_test},
#endif
        {"write-groups", write_groups_test}, {"write-busy", write_busy_test},
        {"write-merge", write_merge_test}, {"write-split", write_split_test},
        {"write-fixed", write_fixed_test}, {"write-task-wait", write_task_wait_test},
        {"write-wait-busy", write_wait_busy_test}, {"write-wait-end", write_wait_end_test},
        {"write-wait-threshold", write_wait_threshold_test}, {"write-wait-error", write_wait_error_test},
        {"wide-buffer", wide_buffer_test},
        {"gc-default", gc_amplification_threshold_test},
        {"gc-block", gc_block_threshold_test}, {"gc-relaxed", gc_relaxed_group_test},
        {"gc-snapshot", gc_relaxed_block_test},
        {"threshold", threshold_test}, {"capacity", capacity_test}, {"neighbors", neighbor_test},
        {"tie-empty", tie_and_empty_test}, {"partitions", partition_test}, {"index", index_test},
        {"delete", delete_test}, {"migration", migration_test_fn}, {"pending-read", pending_read_test},
        {"delete-wait-cache", delete_wait_and_cache_test}, {"busy-retry", busy_retry_test},
        {"allocation", allocation_test}, {"failures", failure_test}, {"bounded-prefix", prefix_test},
        {"scan-during-merge", scan_during_merge_test}, {"backlog-split", backlog_and_split_test},
        {"gc-after-merge", gc_after_merge_test}, {"normal-restart", restart_test},
        {"no-task-retry", no_task_retry_test}, {"buffer-union", buffer_union_test},
        {"variable-records", variable_records_test}, {"space-rollback", space_rollback_test}
    };
    char directory[] = "/tmp/libdb-merge-test-XXXXXX";
    CHECK(mkdtemp(directory));
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++){
        char path[512]; snprintf(path, sizeof(path), "%s/%zu.pool", directory, i);
        pid_t pid = fork(); CHECK(pid >= 0);
        if(!pid){ setup(path); cases[i].fn(); uint64_t used = 0; for(unsigned b = 0; b < g_block_num; b++) used += g_map[b] != 0; CHECK(atomic_load(&g_used_data_blocks) == used); pmemobj_close(pool); _exit(0); }
        int status; CHECK(waitpid(pid, &status, 0) == pid);
        unlink(path);
        if(!WIFEXITED(status) || WEXITSTATUS(status)){
            fprintf(stderr, "FAIL %s (status=%d)\n", cases[i].name, status); rmdir(directory); return 1;
        }
        printf("PASS %s\n", cases[i].name); fflush(stdout);
    }
    rmdir(directory);
    return 0;
}
