/* Run the real worker loop against delayed in-memory completion callbacks. */
#include "server.h"
#include "scan.h"
#include <assert.h>
static struct thread worker;
static struct db db;
static struct ns_entry ns;
static struct task tasks[2], *heads[1], *tails[1];
static struct spdk_nvme_qpair *qpairs[31];
static struct spdk_ring *result_rings[1];
static unsigned polls, dequeues, maintenance_calls, reads;
static bool queued_request, pending;
static uint64_t completed[1];
uint32_t spdk_env_get_current_core(void) { return 0; }
uint32_t spdk_env_get_socket_id(uint32_t core) { return 0; }
uint64_t spdk_get_ticks_hz(void) { return 1000000; }
uint64_t spdk_get_ticks(void) { return 1; }
size_t spdk_ring_dequeue(struct spdk_ring *ring, void **objects, size_t count) {
    assert(++dequeues < 20);
    if(queued_request) {
        struct request *r = calloc(1, sizeof(*r)); assert(r);
        r->type = 0; r->key = 1; objects[0] = r;
        queued_request = false; return 1;
    }
    return 0;
}
int spdk_nvme_qpair_process_completions(struct spdk_nvme_qpair *q, uint32_t max) {
    assert(++polls < 20);
    if(pending && polls == 3) {
        /* Publish the final data block only when the pending write completes. */
        block_ref_add(2);
        free_task(&tasks[1]); pending = false;
    }
    return 0;
}
void maintenance_poll(struct thread *t) {
    maintenance_calls++;
    /* A queued merge retry cannot finish until the pending write completes. */
    if(!pending) t->maintenance = NULL;
}
char *get(key_type key, struct thread *t, int *size, uint64_t start,
          struct read_time_breakdown *b) { reads++; *size = 0; return NULL; }
void server_submit_result(key_type key, char *buffer, int size, int type, int result,
                          struct spdk_ring *ring, uint64_t start, void *b) {}
void insert(key_type key, int size, char *value, struct thread *t,
            struct insert_time_breakdown *b) { assert(!"unexpected insert/forced flush"); }
void db_delete(key_type key, struct thread *t) { assert(0); }
bool scan(key_type key, int length, char *value, struct thread *t, uint64_t start) {
    assert(0); return false;
}
static void setup(bool delayed) {
    static uint_fast8_t map[8];
    memset(map, 0, sizeof(map)); g_map = map; g_block_num = 8;
    atomic_store(&g_used_data_blocks, 0);
    memset(&worker, 0, sizeof(worker)); memset(tasks, 0, sizeof(tasks));
    db = (struct db){.workers=&worker, .thread=1, .tasks_per_thread=2,
                     .load_injectors=1, .ns_entry=&ns, .data_blocks=8};
    db.initial_load.complete = true; db.initial_load.record_bytes = BLOCK_SIZE;
    ns.task_array_head = heads; ns.task_array_tail = tails; ns.qpair = qpairs;
    worker.db = &db; worker.ns_entry = &ns;
    worker.spdk_result_ring = result_rings;
    heads[0] = tails[0] = NULL;
    for(int i=0; i<2; i++) { tasks[i].ns_entry=&ns; tasks[i].thread_seq=0; }
    free_task(&tasks[0]); if(!delayed) free_task(&tasks[1]);
    pending = delayed;
    worker.maintenance = delayed ? (struct pending_maintenance *)&worker : NULL;
    space_stats_nvm_init(&db);
    atomic_store(&db.nvm_flush.phase, NVM_FLUSH_STOPPED);
    thread_completed = completed; completed[0] = 0;
    polls = dequeues = maintenance_calls = reads = 0;
    benchmark_completed = 1;
}
int main(void) {
    setup(true); queued_request = true;
    assert(server_executor(&worker) == 0);
    assert(polls >= 3 && !pending && !worker.maintenance && reads == 1);
    assert(completed[0] == 1 && maintenance_calls >= 3);
    struct space_snapshot snapshot;
    assert(!space_stats_snapshot(&db, &snapshot) && snapshot.used_blocks == 1);
    setup(false); queued_request = false;
    assert(server_executor(&worker) == 0);
    assert(polls == 1 && reads == 0);
    assert(!space_stats_snapshot(&db, &snapshot) && snapshot.used_blocks == 0);
    return 0;
}
