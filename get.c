#include "./get.h"
#include "./task.h"
#include "./insert.h"
#include "base.h"
#include "client.h"
#include "dram_index.h"
#include "index.h"
#include "subspace.h"
#include <inttypes.h>
#include <stdint.h>
#include "hist.h"
#include "kv_record.h"

static const key_type DEBUG_TRACE_KEY = (key_type)3483562197984606726ULL;

static void debug_dump_kv_header(const char *where, key_type key,
                                 const char *record, int available){
    if(key != DEBUG_TRACE_KEY){
        return;
    }
    struct kv_record_view view;
    if(available < 0 ||
       kv_record_decode(record, (size_t)available, &view) != 0){
        printf("[trace-get] %s key=%" PRIu64 " header: <invalid avail=%d>\n",
               where, (uint64_t)key, available);
        return;
    }
    printf("[trace-get] %s key=%" PRIu64
           " header: key_size=%u key_in_record=%" PRIu64
           " version=%" PRIu64 " value_size=%u record_size=%zu avail=%d\n",
           where, (uint64_t)key, (unsigned)view.key_size,
           (uint64_t)view.key, (uint64_t)view.version,
           (unsigned)view.value_size, view.record_size, available);
}


void server_submit_result(key_type key , char * buffer , int size,  int type , int exct_result,
struct spdk_ring * spdk_result_ring , uint64_t start , void * break_down){
    struct result * result = (struct result *)malloc(sizeof(struct result));
    result->key = key;
    result->value = buffer;
    result->type = type;
    result->result = exct_result;
    result->size = size;
    result->start = start;
    result->end = spdk_get_ticks();
    result->break_down = break_down;
    while(!spdk_ring_enqueue(spdk_result_ring , (void **)&result ,1 , NULL)){
        // printf("spdk_result_ring is full\n");
        // sleep(100);
    }
    // printf("server_submit_result done\n");
    return ;
}

void get_cb(void *ctx, const struct spdk_nvme_cpl *cpl) {
    struct task *task = (struct task *)ctx;
    struct subspace_info *subspace_info =
        (struct subspace_info *)task->subspace_info;
    struct thread *thread = (struct thread *)task->thread;
    const char *record = task->buffer + task->addr.off;
    struct kv_record_view view;

    if(spdk_nvme_cpl_is_error(cpl) ||
       kv_record_decode_for_key(record, task->addr.size, task->key, &view) != 0){
        fprintf(stderr, "get: invalid SSD KV record for key=%" PRIu64 "\n",
                (uint64_t)task->key);
        exit(EXIT_FAILURE);
    }

    char *result = (char *)malloc(view.value_size);
    if(result == NULL){
        fprintf(stderr, "get: failed to allocate %u-byte value\n",
                (unsigned)view.value_size);
        exit(EXIT_FAILURE);
    }
    if(kv_record_copy_valid_value(result, view.value_size, &view) != 0){
        fprintf(stderr, "get: failed to copy valid SSD value prefix\n");
        exit(EXIT_FAILURE);
    }

    debug_dump_kv_header("SSD-buffer", task->key, record, task->addr.size);
    kvcache_insert(subspace_info->kvcache, thread->lru_list, task->key,
                   result, view.value_size, thread->thread);
    server_submit_result(task->key, result, view.value_size, 0, 0,
        thread->spdk_result_ring[task->key % thread->db->load_injectors],
        task->start, task->break_down);
    free_task(task);
}

char *  get(key_type key , struct thread * thread , int * size_o , uint64_t get_start ,
struct read_time_breakdown * break_down){
    struct write_buffer_index_node * node;
    struct address_type addr;
    struct subspace_info * subspace_info;
    struct write_buffer_index * index;
    char * result;
    int length;
    char * cache_value = NULL;
    uint64_t start , end;


    // start = spdk_get_ticks();
    subspace_info = dram_index_search(thread->index->dram_index , key , NULL , NULL);
    // end = spdk_get_ticks();

    index = subspace_info->buffer_index;
    while(index){
        node = write_buffer_index_search(index , key);
        if(node != NULL){
            if(key == DEBUG_TRACE_KEY){
                printf("[trace-get] hit WRITE_BUFFER key=%" PRIu64 " node.size=%u WBF_seq=%u id.off=%" PRIu64 " id.pool=%" PRIu64 "\n",
                       (uint64_t)key, (unsigned)node->size, (unsigned)node->WBF_seq,
                       (uint64_t)node->id.off, (uint64_t)node->id.pool_uuid_lo);
                access_nvm();
                debug_dump_kv_header("WB-record", key, (const char *)pmemobj_direct(node->id), (int)node->size);
                access_nvm_release();
            }
            thread_kvbuffer_hit[thread->thread]++;
            access_nvm();
            const char *record = (const char *)pmemobj_direct(node->id);
            struct kv_record_view view;
            if(kv_record_decode_for_key(record, node->size, key, &view) != 0){
                access_nvm_release();
                fprintf(stderr, "get: invalid write-buffer KV record for key=%" PRIu64 "\n",
                        (uint64_t)key);
                exit(EXIT_FAILURE);
            }
            *size_o = view.value_size;
            result = (char *)malloc((size_t)*size_o);
            if(result == NULL){
                access_nvm_release();
                fprintf(stderr, "get: failed to allocate write-buffer value\n");
                exit(EXIT_FAILURE);
            }
            if(kv_record_copy_valid_value(result, (size_t)*size_o, &view) != 0){
                access_nvm_release();
                fprintf(stderr, "get: failed to copy valid write-buffer value prefix\n");
                exit(EXIT_FAILURE);
            }
            access_nvm_release();
            // kvcache_insert(subspace_info->kvcache , thread->lru_list , key , result , *size_o , thread->thread);
            // kvcache_insert_emulation(subspace_info->kvcache , thread->lru_list , key , result , *size_o , thread->thread);
            // int key_sz = *(uint16_t*)pmemobj_direct(node.id);
            // *size_o = node.size - 2 - key_sz - 2 - 8;
            // memcpy(result , pmemobj_direct(node.id) + 2 + key_sz + 2 , *size_o);
            // printf("get key %d on PM done\n" , key);
            // end = spdk_get_ticks();
            // read_breakdown[thread->thread].write_buffer_index_time += end - start;
            return result;
        }
        else{
            index = index->next;
        }
    }
    // end = spdk_get_ticks();
    // read_breakdown[thread->thread].write_buffer_index_time += end - start;

    // start = spdk_get_ticks();
    cache_value = kvcache_get(subspace_info->kvcache ,thread->lru_list ,  key , &length);

    // cache_value = kvcache_get_emulation(subspace_info->kvcache ,thread->lru_list ,  key , &length,thread->thread);
    if(cache_value != NULL){//kvcache命中
        thread_kvcache_hit[thread->thread]++;
        *size_o = length;
        // end = spdk_get_ticks();
        // read_breakdown[thread->thread].search_cache_time += end - start;
        return cache_value;
    }
    // end = spdk_get_ticks();
    // read_breakdown[thread->thread].search_cache_time += end - start;

    // start = spdk_get_ticks();
    if(node == NULL && cache_value == NULL){//不存在于write buffer
        addr = nvm_index_search(subspace_info , key);
        if(ADDR_IS_NULL(addr)){//不存在
            printf("key %lu not exist\n" , key);
            *size_o = 0;
            return 0;
        }
        else{//在SSD上
            if(key == DEBUG_TRACE_KEY){
                printf("[trace-get] miss WB+cache, hit NVM_INDEX key=%" PRIu64 " -> SSD addr.block=%u off=%u size=%u\n",
                       (uint64_t)key, addr.block, addr.off, addr.size);
            }
            struct task * task;
            int qpair_excuting_once , rc ;
            int qpair_num = thread->thread % ctr_max_qp;

            // qpair_lock(thread->thread , 2);
            task = get_free_task(thread->ns_entry , thread->thread);

            while(task == NULL){//qpair已满
                if((qpair_excuting_once = spdk_nvme_qpair_process_completions(thread->ns_entry->qpair[qpair_num], 0)) < 0){
                    printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
                    return 0;
                }
                if(qpair_excuting_once != 0){
                    task = get_free_task(thread->ns_entry , thread->thread);
                }
            }

            task->addr = addr;
            task->thread = thread;
            task->key = key;
            task->subspace_info = subspace_info;
            task->start = get_start;
            task->break_down = read_breakdown;

            thread_block_rw[thread->thread] ++;
            rc = spdk_nvme_ns_cmd_read(thread->ns_entry->ns, thread->ns_entry->qpair[qpair_num], task->buffer, addr.block * physical_block_to_logical_block,
                physical_block_to_logical_block, get_cb , task , 0);
            if (rc != 0) {
                printf("ERROR:starting read I/O failed\n");
                // qpair_unlock(thread->thread);
                return 0;
            }
            *size_o = addr.size;
        }
    }
    // end = spdk_get_ticks();
    // read_breakdown[thread->thread].nvm_index_time += end - start;
    return NULL;
}

void db_delete(key_type key , struct thread * thread ){
    struct subspace_info * subspace_info;


    subspace_info = dram_index_search(thread->index->dram_index , key , NULL , NULL);

    write_buffer_index_delete(subspace_info->buffer_index , key , NULL);

    kvcache_delete(subspace_info->kvcache , thread->lru_list , key);

    nvm_index_delete(subspace_info , key);

    return ;
}