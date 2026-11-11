#include "server.h"
#include "client.h"
#include "scan.h"
#include "wal.h"

static bool tasks_idle(const struct thread *t) {
    unsigned free_count = 0;
    for(struct task *task = t->ns_entry->task_array_head[t->thread]; task; task = task->next_free)
        free_count++;
    return free_count == (unsigned)t->db->tasks_per_thread;
}

int server_executor(void *arg) {
    struct thread *t = arg;
    struct db *db = t->db;
    struct request *requests[1024];
    uint64_t start = 0;
    bool request_started = false;
    unsigned count = 0, next = 0;
    printf("worker %d running on core %u\n", t->thread, spdk_env_get_current_core());
    while(true) {
        struct request *completed_request = NULL;
        wal_apply_ready(t);
        if(next == count) {
            count = spdk_ring_dequeue(t->spdk_ring, (void **)requests, 1024);
            next = 0;
        }
        while(next < count) {
            wal_check_error(t);
            struct request *r = requests[next];
            if(t->wal && r->type == 1) {

                if(!request_started) {
                    start = spdk_get_ticks();
                    request_started = true;
                }
                if(!wal_enqueue_put(t, r, start)) break;
                request_started = false;
                next++;
                continue;
            }



            start = spdk_get_ticks();
            int size;
            char *value;
            switch(r->type) {
            case 0:
                value = get(r->key, t, &size, start, r->break_down);
                wal_check_error(t);
                if(value || size == 0)
                    server_submit_result(r->key, value, size, 0, 0,
                        t->spdk_result_ring[r->key % db->load_injectors], start, NULL);
                break;
            case 1:
                insert(r->key, r->size, r->value, t, r->break_down);
                server_submit_result(r->key, r->value, 0, 1, 0,
                    t->spdk_result_ring[r->key % db->load_injectors], start, r->break_down);
                break;
            case 2:
                if(scan(r->key, r->size, r->value, t, start)) {
                    fprintf(stderr, "fatal: scan failed\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 3: {
                int rc = db_delete(r->key, t);
                server_submit_result(r->key, r->value, 0, 3, rc,
                    t->spdk_result_ring[r->key % db->load_injectors], start, NULL);
                break;
            }
            default:
                fprintf(stderr, "fatal: unknown request type\n");
                exit(EXIT_FAILURE);
            }
            completed_request = r;
            request_started = false;
            next++;

            break;
        }
        wal_poll(t, benchmark_completed);
        maintenance_poll(t);
        if(completed_request) {
            thread_completed[t->thread]++;
            free(completed_request);
        }
        wal_pressure_poll(t);
        if(spdk_nvme_qpair_process_completions(t->ns_entry->qpair[t->thread % ctr_max_qp], 0) < 0) {
            fprintf(stderr, "fatal: worker completion polling failed\n");
            exit(EXIT_FAILURE);
        }
        wal_check_error(t);


        while(no_free_task(t->ns_entry, t->thread)) {
            if(spdk_nvme_qpair_process_completions(
                    t->ns_entry->qpair[t->thread % ctr_max_qp], 0) < 0) {
                fprintf(stderr, "fatal: worker completion polling failed\n");
                exit(EXIT_FAILURE);
            }
            wal_check_error(t);
        }


        if(benchmark_completed && !count && !wal_busy(t) &&
           !t->maintenance && tasks_idle(t)) break;
    }
    return 0;
}
