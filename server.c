#include "./server.h"
#include "./base.h"
#include "./client.h"
#include "./scan.h"


static bool tasks_idle(const struct thread *t) {
    unsigned free_count = 0;
    for(struct task *task = t->ns_entry->task_array_head[t->thread]; task; task = task->next_free)
        free_count++;
    return free_count == (unsigned)t->db->tasks_per_thread;
}

int server_executor(void * arg){
    struct thread * thread = (struct thread *)arg;
    struct request * req;
    struct request * requests[1024];
    struct db * db = thread->db;
    int qpair_excuting_once;
    int size;
    char * result;
    int thread_seq;
    uint64_t start;
    int req_num;

    uint32_t core = spdk_env_get_current_core();

    // 获取 NUMA 节点 ID (SPDK 中通常用 socket_id 表示 NUMA 节点)
    int numa_node = spdk_env_get_socket_id(core);
    printf("this is thread %d, starting , running on core %u (NUMA node %d)\n" , thread->thread , core, numa_node);

    g_ticks = spdk_get_ticks_hz();
    while(1){
        req_num = spdk_ring_dequeue(thread->spdk_ring , (void ** )requests , 1024);
        if(req_num){
            for(int i = 0 ; i < req_num; i++){
                req = requests[i];
                start = spdk_get_ticks();
                if(req->type == 0){//get
                    result = get(req->key , thread , &size , start , (struct read_time_breakdown *)req->break_down);
                    if(result == NULL && size == 0 ){//not exist
                        server_submit_result(req->key , NULL , 0 , 0 , 0 ,
                        thread->spdk_result_ring[req->key % db->load_injectors] , start , NULL);
                    }
                    else if (result == NULL)//on ssd
                    {

                    }
                    else{//om PM
                        server_submit_result(req->key , result , size , 0 , 0 ,
                        thread->spdk_result_ring[req->key % db->load_injectors] , start , NULL);
                    }
                }
                else if(req->type == 1){//put
                    insert(req->key , req->size , req->value , thread , (struct insert_time_breakdown *)req->break_down);
                    server_submit_result(req->key , req->value , 0,  1 , 0 ,
                    thread->spdk_result_ring[req->key % db->load_injectors]  , start ,
                    req->break_down);
                }
                else if(req->type == 2){//scan
                    if(scan(req->key, req->size, req->value,  thread , start)){
                        printf("ERROR:scan failed\n");
                    }
                }
                else if(req->type == 3){
                    db_delete(req->key , thread);

                    server_submit_result(req->key , req->value , 0,  3 , 0 ,
                    thread->spdk_result_ring[req->key % db->load_injectors] , start , NULL);
                }
                else{
                }
                maintenance_poll(thread);
                thread_completed[thread->thread]++;
                free(req);

                if((qpair_excuting_once = spdk_nvme_qpair_process_completions(db->ns_entry->qpair[thread->thread % ctr_max_qp], 0)) < 0){
                    printf("ERROR:fail to process_completions qpair[%d]\n" , thread->thread);
                    while(1);
                }
                while(no_free_task(db->ns_entry , thread->thread)){
                    if((qpair_excuting_once = spdk_nvme_qpair_process_completions(db->ns_entry->qpair[thread->thread % ctr_max_qp], 0)) < 0){
                        printf("ERROR:fail to process_completions qpair[%d]\n" , thread->thread);
                        while(1);
                    }
                }
            }//for(int i = 0 ; i < req_num; i++)
        }//if(req_num)
        else{
            maintenance_poll(thread);
            if((qpair_excuting_once = spdk_nvme_qpair_process_completions(db->ns_entry->qpair[thread->thread % ctr_max_qp], 0)) < 0){
                printf("ERROR:fail to process_completions qpair[%d]\n" , thread->thread);
                while(1);
            }
            while(no_free_task(db->ns_entry , thread->thread)){
                if((qpair_excuting_once = spdk_nvme_qpair_process_completions(db->ns_entry->qpair[thread->thread % ctr_max_qp], 0)) < 0){
                    printf("ERROR:fail to process_completions qpair[%d]\n" , thread->thread);
                    while(1);
                }
            }
        }
        /* Normal results have been consumed before stop is requested.
         * Drain submitted I/O and scheduled maintenance, without forcing
         * buffered data below the normal flush threshold onto SSD. */
        if(benchmark_completed && !req_num && !thread->maintenance && tasks_idle(thread))
            break;
    }
    maintenance_clear_deferred(thread);
    return 0;
}//server_excutor
