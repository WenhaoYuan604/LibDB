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
    }
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
    if(read_cache_mapping_current(thread, task->key, task->addr)){
        struct subspace_info *current = dram_index_search(thread->index->dram_index, task->key, NULL, NULL);
        kvcache_insert(current->kvcache, thread->lru_list, task->key, result, view.value_size, thread->thread);
    }
    read_unpin_blocks(thread, &task->addr.block, 1);
    subspace_unref(subspace_info);
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


    subspace_info = dram_index_search(thread->index->dram_index , key , NULL , NULL);

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
            return result;
        }
        else{
            index = index->next;
        }
    }

    cache_value = kvcache_get(subspace_info->kvcache ,thread->lru_list ,  key , &length);

    if(cache_value != NULL){//kvcache命中
        thread_kvcache_hit[thread->thread]++;
        *size_o = length;
        return cache_value;
    }

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

            subspace_ref(subspace_info);
            read_pin_blocks(thread, &addr.block, 1);
            task = get_free_task(thread->ns_entry , thread->thread);

            while(task == NULL){//qpair已满
                if((qpair_excuting_once = spdk_nvme_qpair_process_completions(thread->ns_entry->qpair[qpair_num], 0)) < 0){
                    fprintf(stderr, "get: completion processing failed\n");
                    exit(EXIT_FAILURE);
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
                fprintf(stderr, "get: read submission failed\n");
                exit(EXIT_FAILURE);
            }
            *size_o = addr.size;
        }
    }
    return NULL;
}

void db_delete(key_type key, struct thread *thread){
    struct subspace_info *group;
    for(;;){
        group = dram_index_search(thread->index->dram_index, key, NULL, NULL);
        if(!group) return;
        if(!subspace_busy(group)) break;
        if(spdk_nvme_qpair_process_completions(thread->ns_entry->qpair[thread->thread % ctr_max_qp], 0) < 0){
            fprintf(stderr, "delete: failed to wait for structural I/O\n");
            exit(EXIT_FAILURE);
        }
    }
    struct write_buffer_index_node *node = write_buffer_index_search(group->buffer_index, key);
    if(node){
        if(!write_buffer_free(thread->kvbuffer, node->id, node->WBF_seq, node->size, &thread->buffer_free_head)){
            fprintf(stderr, "delete: failed to retire buffered record\n");
            exit(EXIT_FAILURE);
        }
        group->buffered_size -= node->size;
        write_buffer_index_delete(group->buffer_index, key, false);
    }
    kvcache_delete(group->kvcache, thread->lru_list, key);
    nvm_index_delete(group, key, thread);
    merge_check_after_delete(thread, key);
}
