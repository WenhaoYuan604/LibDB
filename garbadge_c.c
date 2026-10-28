#include "space_stats.h"
#include "./garbadge_c.h"
#include "merge_flush.h"
#include "kv_record.h"
#include "dram_index.h"
#include "nvm_index.h"
#include "spdk_struct.h"
#include "subspace.h"
#include "kvbuffer.h"
#include "write_buffer_index.h"
#include <stdint.h>
/*
先修改索引
再释放块
*/

/*
 * 计算write_buffer_index中所有数据的总大小
 */
int calculate_buffer_index_size(struct write_buffer_index *buffer_index) {
    if (buffer_index == NULL || buffer_index->head == NULL) {
        return 0;
    }

    int total_size = 0;
    struct write_buffer_index_node *node = buffer_index->head->forward[0];

    while (node != NULL) {
        total_size += node->size;
        node = node->forward[0];
    }

    return total_size;
}

/*
 * 从write_buffer_index中按照键大小从小到大读取足够的数据
 * 返回实际读取的数据大小
 */
int extract_buffer_index_data(char *buffer, struct write_buffer_index *buffer_index,
                              int needed_size, key_type *extracted_keys, int *extracted_keys_count) {
    if (buffer_index == NULL || buffer_index->head == NULL) {
        *extracted_keys_count = 0;
        return 0;
    }

    int total_size = 0;
    int keys_count = 0;
    struct write_buffer_index_node *node = buffer_index->head->forward[0];

    // 从前向后遍历write_buffer_index（键从小到大）
    while (node != NULL && total_size < needed_size) {
        // 从NVM缓存中读取数据
        char *data = (char *)pmemobj_direct(node->id);
        if (data != NULL) {
            memcpy(buffer + total_size, data, node->size);
            extracted_keys[keys_count] = node->key;
            keys_count++;
            total_size += node->size;
        }
        node = node->forward[0];
    }

    *extracted_keys_count = keys_count;
    return total_size;
}

int block_exist(uint32_t block ,uint32_t * block_array ,int block_array_size_tmp){
    for (int i = 0; i < block_array_size_tmp; i++)
        if (block_array[i] == block)
            return i+1;
    return 0;
}

static int __redundant_comp(const void * a, const void * b) {
    return ((struct redundant_element *)b)->block_redundant_size - ((struct redundant_element *)a)->block_redundant_size;
}

static int redundant_element_upper_bound(uint32_t block,
    struct redundant_element *redundant_array, int size) {
    int l = 0, r = size;
    while (l < r) {
        int m = (l + r) / 2;
        if (redundant_array[m].block < block)
            l = m + 1;
        else
            r = m;
    }
    return l;
}

static struct redundant_element * redundant_element_find(uint32_t block,
    struct redundant_element *redundant_array, int size) {
    int pos = redundant_element_upper_bound(block, redundant_array, size);
    if (!(pos < size && redundant_array[pos].block == block)) {
        memmove(redundant_array + pos + 1, redundant_array + pos, sizeof(struct redundant_element) * (size - pos));
        redundant_array[pos].block = block;
        redundant_array[pos].block_redundant_size = BLOCK_SIZE;
    }
    return redundant_array + pos;
}


void garbadge_collection_cb_write_cb(void *arg, const struct spdk_nvme_cpl *completnio){
    struct task * task = (struct task *)arg;
    struct thread * thread = (struct thread *)task->thread;
    uint64_t start_time , end_time;
    struct subspace_info * subspace_info = (struct subspace_info *)task->subspace_info;
    start_time = spdk_get_ticks();
    if(spdk_nvme_cpl_is_error(completnio)){
        task->gc_io_failed = 1;
        fprintf(stderr, "GC SSD write completion reported an error\n");
    }
    task->block_completed ++;
    if(task->block_completed >= task->total_block_num){
        if(task->gc_io_failed){
            fprintf(stderr, "GC aborted before slot updates because an SSD write failed\n");
            exit(EXIT_FAILURE);
        }
        if(nvm_index_commit_slot_updates(
               thread->index->nvm_index,
               (const struct nvm_slot_update *)task->gc_slot_updates,
               (uint32_t)task->gc_slot_update_count) != 0){
            fprintf(stderr, "GC slot update failed; old SSD blocks were not released\n");
            exit(EXIT_FAILURE);
        }

        free(task->gc_slot_updates);
        task->gc_slot_updates = NULL;
        task->gc_slot_update_count = 0;
        reclaim_blocks(thread, task->block_array_to_gc, task->total_block_num_to_gc);
        maintenance_request_flush(thread, subspace_info->boundary);
        subspace_info->is_gc = 0;
        free(task->block_array_to_gc);
        subspace_info->total_block_num -=
            (task->total_block_num_to_gc - task->block_completed);
        free_task(task);
    }
    end_time = spdk_get_ticks();
    insert_breakdown[thread->thread].gc_cb_time += end_time - start_time;
    return ;
}

/*
1. 重组被 GC 块中的有效 KV。
2. 只在 DRAM 中构造受影响 slot 的完整后镜像；SSD 写完成后再逐个持久化。
*/
int fill_buffer_and_update_index(char * buffer , struct subspace_info * subspace_info , struct thread * thread,
 uint32_t * block , int block_size , uint32_t * new_block,
 struct nvm_slot_update **slot_updates_out, int *slot_update_count_out){
    struct slot_element * slot_element;
    uint32_t block_curr = 0;
    int off = 0;
    /* 记录当前块中 KV 个数及最后一个 KV 的信息，用于尾部小块优化 */
    char * buffer_to_store = (char *)malloc(sizeof(char)* BLOCK_SIZE * block_size);
    struct nvm_slot_update *slot_updates = (struct nvm_slot_update *)calloc(
        SUBSPACE_MAX_SLOT, sizeof(struct nvm_slot_update));
    int slot_update_count = 0;
    if(buffer_to_store == NULL || slot_updates == NULL){
        fprintf(stderr, "failed to allocate GC staging buffers\n");
        free(buffer_to_store);
        free(slot_updates);
        return -1;
    }
    int used_block;
    int k;

    new_block[0] = get_free_block(&thread->curr_segment , thread->fs);
    access_nvm();
    for(int i = 0 ; i < subspace_info->slot_num ; i++){
        void *persistent_slot = pmemobj_direct(subspace_info->slot[i]);
        if(persistent_slot == NULL){
            access_nvm_release();
            free(buffer_to_store);
            free(slot_updates);
            return -1;
        }
        struct nvm_slot_update *slot_update = &slot_updates[slot_update_count];
        slot_update->slot = subspace_info->slot[i];
        memcpy(&slot_update->image, persistent_slot, LNF_SLOT_MIN_SIZE);
        slot_element = slot_update->image.entries;
        int slot_changed = 0;
        for(int j = 0 ; j < subspace_info->slot_key_num[i] ; j++){
            /*需要把键值对复制到buffer里*/
            k = block_exist(slot_element->addr.block , block , block_size);
            if(k){//被gc的键值对的索引
                if(off + slot_element->addr.size > BLOCK_SIZE){
                    new_block[block_curr + 1] = get_free_block(&thread->curr_segment , thread->fs);
                    block_curr ++;
                    off = 0;
                    /* 开始新的块，重置当前块内计数 */
                    if(block_curr >= block_size){
                    }
                }

                /*将buffer中的键值对复制到buffer*/
                memcpy(buffer_to_store + BLOCK_SIZE * block_curr + off ,
                buffer + BLOCK_SIZE * (k - 1) + slot_element->addr.off, slot_element->addr.size);
                /* 记录当前块最后一个 KV 的信息（用于尾部少量数据写回 NVM 写缓存） */
                slot_element->addr.block = new_block[block_curr];
                slot_element->addr.off = off;
                off += slot_element->addr.size;
                slot_changed = 1;
                block_ref_add(new_block[block_curr]);
            }
            slot_element ++;
        }
        if(slot_changed){
            slot_update_count++;
        }
    }
    access_nvm_release();

    /* Keep the partial tail on SSD. Moving it into the active GWB here
     * invalidates the staged slot addresses and may overwrite newer writes.
     * Every staged address must name a destination that is actually written. */
    used_block = block_curr + 1;

    memcpy(buffer , buffer_to_store , used_block * BLOCK_SIZE);
    free(buffer_to_store);
    *slot_updates_out = slot_updates;
    *slot_update_count_out = slot_update_count;
    return used_block;
}

void garbadge_collection_cb(void *arg, const struct spdk_nvme_cpl *completnio){
    struct task * task = (struct task *)arg;
    struct thread * thread = (struct thread *)task->thread;
    struct ns_entry * ns_entry = (struct ns_entry *)thread->ns_entry;

    int qpair_num = thread->thread % ctr_max_qp;
    int rc ;
    uint32_t * new_block;
    uint64_t start_time , end_time;

    start_time = spdk_get_ticks();
    if(spdk_nvme_cpl_is_error(completnio)){
        task->gc_io_failed = 1;
        fprintf(stderr, "GC SSD read completion reported an error\n");
    }
    task->block_completed ++;
    if(task->block_completed >= task->total_block_num){//块已经全部读取完
        if(task->gc_io_failed){
            fprintf(stderr, "GC cannot continue because an SSD read failed\n");
            exit(EXIT_FAILURE);
        }

        new_block = (uint32_t *)malloc(sizeof(uint32_t) * task->total_block_num);
        if(new_block == NULL){
            fprintf(stderr, "failed to allocate GC block array\n");
            exit(EXIT_FAILURE);
        }
        memset(new_block, 0, sizeof(uint32_t) * task->total_block_num);
        task->block_array_to_gc = task->block_array;
        task->total_block_num_to_gc = task->total_block_num;
        /*提交写任务*/
        struct nvm_slot_update *slot_updates = NULL;
        int slot_update_count = 0;
        int blocks_to_write = fill_buffer_and_update_index(task->buffer,
        (struct subspace_info *)task->subspace_info, (struct thread *)task->thread, task->block_array,
         task->total_block_num, new_block, &slot_updates, &slot_update_count);
        if(blocks_to_write <= 0){
            fprintf(stderr, "GC failed while staging slot updates\n");
            exit(EXIT_FAILURE);
        }
        task->gc_slot_updates = slot_updates;
        task->gc_slot_update_count = slot_update_count;
        task->total_block_num = blocks_to_write;
        task->gc_io_failed = 0;
        task->block_completed = 0;
        /*new_subspace*/
        for(int i = 0 ; i < task->total_block_num ; i++){
            thread_block_rw[thread->thread] ++;
            thread_gc_block_rw[thread->thread]++;
            thread_gc_block_write[thread->thread]++;
            rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[qpair_num], (task->buffer + i * BLOCK_SIZE),
                new_block[i] * physical_block_to_logical_block  ,1 * physical_block_to_logical_block , garbadge_collection_cb_write_cb , task , 0);
            if (rc != 0) {
                fprintf(stderr, "ERROR: starting GC write I/O failed; slots were not committed\n");
                exit(EXIT_FAILURE);
            }
        }

        free(new_block);

    }
    end_time = spdk_get_ticks();
    insert_breakdown[thread->thread].gc_cb_time += end_time - start_time;
    return ;
}


static uint32_t * block_redundant_compute_fraction(struct subspace_info * subspace_info , int * array_size ,struct thread * thread, double fraction){
    struct redundant_element * redundant_array = (struct redundant_element *)malloc
    (sizeof(struct redundant_element) * subspace_info->total_block_num);
    memset(redundant_array , 0 , sizeof(struct redundant_element) * subspace_info->total_block_num);
    int size = 0;
    int j = 0;
    access_nvm();
    for (int i = 0; i < subspace_info->slot_num; i++) {
        struct slot_element *slot_elements =
            subspace_slot_elements(subspace_info, (uint32_t)i);
        for (int j = 0; j < subspace_info->slot_key_num[i]; j++) {
            struct address_type *addr = &slot_elements[j].addr;
            struct redundant_element * redundant_element = redundant_element_find(addr->block, redundant_array, size);
            if (redundant_element->block_redundant_size == BLOCK_SIZE)
                size++;
            redundant_element->block_redundant_size -= addr->size;
        }
    }
    access_nvm_release();

    if (subspace_info->total_block_num > size) {
        invalid_block_num[thread->thread] += subspace_info->total_block_num - size;
        // 这里 invalid_block_num 就是“索引中完全看不到”的块数量
    }

    qsort(redundant_array, subspace_info->total_block_num, sizeof(struct redundant_element), __redundant_comp);
    uint32_t * arr = (uint32_t *)malloc(sizeof(uint32_t) * subspace_info->total_block_num);
    subspace_info->redundant_size = 0;
    for (int i = 0; i < subspace_info->total_block_num; i++) {
        if(redundant_array[i].block_redundant_size >= BLOCK_SIZE * fraction){//被选中gc
            arr[j] = redundant_array[i].block;
            j++;
        }
        else{
            subspace_info->redundant_size += redundant_array[i].block_redundant_size;//未被选中gc
        }
    }
    free(redundant_array);
    *array_size = j;
    return arr;
}
/*垃圾回收*/
uint32_t *block_redundant_compute(struct subspace_info *group, int *count, struct thread *thread) {
    return block_redundant_compute_fraction(group, count, thread, space_stats_gc_fraction(thread->db));
}
void garbadge_collection(struct subspace_info *group, struct thread *thread) {
    garbadge_collection_with_fraction(group, thread, space_stats_gc_fraction(thread->db));
}
void garbadge_collection_with_fraction(struct subspace_info * subspace_info ,struct thread * thread, double fraction){
    if(subspace_info->is_merging || subspace_info->is_spliting || subspace_info->flush_block_num) return;

    struct ns_entry * ns_entry = (struct ns_entry * )thread->ns_entry;
    uint32_t * redundant_array;
    int redundant_array_size;
    struct task * task;
    int qpair_excuting_once , rc;
    int qpair_num = thread->thread % ctr_max_qp;

    /*计算每个块的冗余度*/
    redundant_array = block_redundant_compute_fraction(subspace_info , &redundant_array_size , thread, fraction);
    if(redundant_array_size == 0){
        free(redundant_array);
        subspace_info->is_gc = false;
        return;
    }
    task = get_free_task(ns_entry , thread->thread);
    while(task == NULL){//qpair已满
        if((qpair_excuting_once = spdk_nvme_qpair_process_completions(
                ns_entry->qpair[qpair_num], 0)) < 0){
                printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
                return;
        }
        if(qpair_excuting_once != 0){
            task = get_free_task(ns_entry , thread->thread);
        }
    }
    task->subspace_info = subspace_info;
    task->thread = thread;
    task->block_completed = 0;
    task->gc_slot_updates = NULL;
    task->gc_slot_update_count = 0;
    task->gc_io_failed = 0;
    task->total_block_num = redundant_array_size;
    task->block_array = redundant_array;
    /*选择冗余度最高的部分块*/
    /*提交读取任务*/
    for(int i = 0 ; i < redundant_array_size ; i++){
        thread_block_rw[thread->thread] ++;
        thread_gc_block_rw[thread->thread] ++;
        rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair[qpair_num], (task->buffer + i * BLOCK_SIZE),
        redundant_array[i] * physical_block_to_logical_block  ,1 *physical_block_to_logical_block , garbadge_collection_cb , task , 0);
        if (rc != 0) {
            fprintf(stderr, "ERROR: starting GC read I/O failed\n");
            exit(EXIT_FAILURE);
        }
    }
    return ;
}
