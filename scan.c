#include "./scan.h"
#include "base.h"
#include "subspace.h"
#include "index.h"
#include "kv_record.h"


static void merge(uint32_t arr[], int left, int mid, int right, uint32_t temp[]) {
    int i = left;      // 左子数组起始位置
    int j = mid + 1;   // 右子数组起始位置
    int k = 0;         // 临时数组索引

    // 合并两个子数组到临时数组
    while (i <= mid && j <= right) {
        if (arr[i] <= arr[j]) {
            temp[k++] = arr[i++];
        } else {
            temp[k++] = arr[j++];
        }
    }

    // 处理剩余元素
    while (i <= mid) temp[k++] = arr[i++];
    while (j <= right) temp[k++] = arr[j++];

    // 将合并结果复制回原数组
    for (i = 0; i < k; i++) {
        arr[left + i] = temp[i];
    }
}

// 归并排序递归辅助函数
static void merge_sort_helper(uint32_t arr[], int left, int right, uint32_t temp[]) {
    if (left >= right) return;

    int mid = left + (right - left) / 2;
    merge_sort_helper(arr, left, mid, temp);
    merge_sort_helper(arr, mid + 1, right, temp);
    merge(arr, left, mid, right, temp);
}

// 主函数：归并排序并去重
size_t dedup_merge_sort(uint32_t block[], size_t n) {
    if (n == 0) return 0;

    // 创建临时数组用于归并排序
    uint32_t *temp = (uint32_t *)malloc(n * sizeof(uint32_t));
    if (!temp) return 0; // 内存分配失败处理

    // 执行归并排序
    merge_sort_helper(block, 0, n - 1, temp);
    free(temp);

    // 去重处理
    size_t write_idx = 0;
    for (size_t read_idx = 1; read_idx < n; read_idx++) {
        if (block[read_idx] != block[write_idx]) {
            write_idx++;
            block[write_idx] = block[read_idx];
        }
    }

    return write_idx + 1; // 返回新长度
}

int binary_search(uint32_t *array, int n, uint32_t block) {
    int left = 0;
    int right = n - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2; // 防止溢出
        if (array[mid] == block) {
            return mid; // 找到目标，返回下标
        } else if (array[mid] < block) {
            left = mid + 1; // 目标在右半部分
        } else {
            right = mid - 1; // 目标在左半部分
        }
    }

    return -1; // 未找到目标
}


void scan_cb(void * ctx, const struct spdk_nvme_cpl * cpl){
    struct task * task = (struct task *)ctx;
    struct subspace_info ** subspace_info_array = (struct subspace_info **)task->subspace_info_array;
    struct thread * thread = (struct thread *)task->thread;
    struct scan_kv * scan_result = (struct scan_kv *) task->scan_result;
    struct address_type * addr = (struct address_type *)task->buffered_addr;
    int k = 0;

    task->block_completed ++;

    if(task->block_completed >= task->total_block_num){
        for(int i = 0 ; i < task->buffered_num ; i++){
            if(task->kvcache_hit[i] || task->is_in_write_buffer[i]){
                continue;
            }
            k = binary_search(task->block_array , task->total_block_num,  addr[i].block);
            if(k < 0){
                exit(EXIT_FAILURE);
            }
            else{
                const char *record = task->buffer + k * BLOCK_SIZE + addr[i].off;
                struct kv_record_view view;
                if(kv_record_decode_for_key(record, addr[i].size,
                                            scan_result->key[i], &view) != 0){
                    fprintf(stderr, "fatal: invalid SSD KV record for scan key %lu\n",
                            (unsigned long)scan_result->key[i]);
                    exit(EXIT_FAILURE);
                }
                scan_result->value_size[i] = view.value_size;
                if(kv_record_copy_valid_value(scan_result->value[i],
                                                  view.value_size, &view) != 0){
                    fprintf(stderr, "fatal: failed to copy valid SSD value prefix\n");
                    exit(EXIT_FAILURE);
                }
            }
        }
        thread_scan_key[thread->thread] = scan_result->key[0];

        int n = 0;
        for(int i = 0 ; i < task->subspace_num ; i++){
            kvcache_update_batch(subspace_info_array[i]->kvcache , thread->lru_list , &scan_result->key[n] ,
            &scan_result->value[n] , &scan_result->value_size[n] , task->valid_key_per_subspace[i] , &task->kvcache_hit[n]
            , &task->is_in_write_buffer[n] , thread->thread);
            n += task->valid_key_per_subspace[i];
        }
        server_submit_result(task->key, (char *)scan_result , task->buffered_num , 2 , 0 ,
        thread->spdk_result_ring[task->key % thread->db->load_injectors] , task->start , NULL);
        free(task->block_array);
        free(task->buffered_addr);
        free(task->is_in_write_buffer);
        free(task->kvcache_hit);
        free(task->valid_key_per_subspace);
        free(task->subspace_info_array);
        free_task(task);

    }
    return ;
}


static struct dram_index_node *get_next_leaf(struct dram_index_node *leaf) {
    if (leaf->parent == NULL) {
        return NULL; // 根节点无兄弟
    }

    struct dram_index_node *current = leaf;
    while (1) {
        struct dram_index_node *parent = current->parent;
        int index = -1;
        key_type current_min = current->key_child[0];
        key_type *keys = parent->key_child;
        int left = 0;
        int right = parent->nchild - 1;

        /* 二分查找定位子节点索引 */
        while (left <= right) {
            int mid = left + (right - left) / 2;
            if (keys[mid] == current_min) {
                index = mid;
                break;
            } else if (keys[mid] < current_min) {
                left = mid + 1;
            } else {
                right = mid - 1;
            }
        }

        if (index == -1) {
            return NULL; // 错误情况
        }

        if (index < parent->nchild - 1) {
            // 存在右侧兄弟，找到其最左叶子
            struct dram_index_node *sibling = parent->children[index + 1];
            while (!sibling->is_leaf) {
                sibling = sibling->children[0];
            }
            return sibling;
        } else {
            // 继续向上查找父节点
            current = parent;
            if (current->parent == NULL) {
                return NULL; // 根节点无兄弟
            }
        }
    }
}

struct subspace_info ** dram_index_scan(struct thread * thread , key_type key, int length, int * subspace_num , int * found_key) {
    int index = -1 ;
    int max_subspace = 2;
    struct dram_index_node *leaf_node = NULL;
    struct subspace_info *subspace = dram_index_search(thread->index->dram_index, key, &leaf_node , &index);

    *subspace_num = 0;
    if (!subspace || !leaf_node || index == -1) {
        printf("!subspace || !leaf_node || index == -1\n");
        exit(EXIT_FAILURE);
    }

    // 找到subspace在leaf_node中的索引i

    struct subspace_info **result = (struct subspace_info **)malloc(sizeof(struct subspace_info *) * max_subspace);
    if(result == NULL){
        printf("ERROR:scan result == NULL\n");
        return NULL;
    }
    result[0] = subspace;
    int count = 1;
    int total = 0;
    struct dram_index_node *current_leaf = leaf_node;
    int current_i = index + 1;


    while (current_leaf != NULL) {
        while (current_i < current_leaf->nchild) {
            if(count >= max_subspace){
                max_subspace *= 2;
                result = (struct subspace_info **)realloc(result, sizeof(struct subspace_info *) * max_subspace);
            }
            struct subspace_info *current_sub = current_leaf->subspaces[current_i];

            result[count] = current_sub;
            count++;
            total += current_sub->key_num;

            if (total >= length) {
                *subspace_num = count;
                *found_key = length;
                return result;
            }
            current_i++;
        }

        // 获取下一个叶子节点
        struct dram_index_node *next_leaf = get_next_leaf(current_leaf);
        current_leaf = next_leaf;
        current_i = 0;
    }
    *found_key = total;
    *subspace_num = count;
    return result;
}

// 辅助函数：获取下一个叶子节点


int subspace_write_buffer_leaf_node_scan(struct subspace_info ** subspace_info_array, int subspace_num, key_type key1, int length,
    key_type *key, struct address_type *addr, struct write_buffer_index_node **buffer_node, int *is_in_write_buffer
    , int * valid_subspace , int * valid_key_per_subspace) {
    int i;
    int found_keys = 0;
    int found_keys_last_subspace = 0;


    // 遍历所有 subspace
    for (i = 0; i < subspace_num && found_keys < length; i++) {
        struct subspace_info *current_subspace = subspace_info_array[i];

        // 初始化 nvm_index 扫描位置
        int slot_idx = 0;
        struct slot_element *slot_element = NULL;
        int slot_element_idx = 0;

        // 在 nvm_index 中找到大于等于 key1 的起始位置
        for (slot_idx = 0; slot_idx < current_subspace->slot_num; slot_idx++) {
            if (current_subspace->min_key[slot_idx] > key1) {
                break;
            }
            if (slot_idx < current_subspace->slot_num - 1 && current_subspace->min_key[slot_idx+1] > key1) {
                break;
            }
        }

        if (slot_idx == current_subspace->slot_num) {
            slot_idx = current_subspace->slot_num - 1;
        }

        if (slot_idx >= 0 && slot_idx < current_subspace->slot_num) {
            slot_element = subspace_slot_elements(current_subspace, (uint32_t)slot_idx);
            for (slot_element_idx = 0; slot_element_idx < current_subspace->slot_key_num[slot_idx]; slot_element_idx++) {
                if (slot_element[slot_element_idx].key >= key1) {
                    break;
                }
            }
        }

        // 初始化所有 write_buffer_index 的扫描位置
        struct write_buffer_index *curr_buffer_index = current_subspace->buffer_index;
        struct write_buffer_index_node **buffer_curr_array = NULL;
        int buffer_index_count = 0;

        // 计算 write_buffer_index 的数量并分配数组
        while (curr_buffer_index != NULL) {
            buffer_index_count++;
            curr_buffer_index = curr_buffer_index->next;
        }

        if (buffer_index_count > 0) {
            buffer_curr_array = (struct write_buffer_index_node **)malloc(sizeof(struct write_buffer_index_node *) * buffer_index_count);
            curr_buffer_index = current_subspace->buffer_index;

            // 初始化每个 write_buffer_index 的扫描位置
            for (int j = 0; j < buffer_index_count; j++) {//没有考虑到有相同的值
                buffer_curr_array[j] = curr_buffer_index->head->forward[0];  // 跳过头节点
                while (buffer_curr_array[j] != NULL && buffer_curr_array[j]->key < key1) {
                    buffer_curr_array[j] = buffer_curr_array[j]->forward[0];
                }
                curr_buffer_index = curr_buffer_index->next;
            }
        }

        // 同时迭代 nvm_index 和所有 write_buffer_index，合并结果
        while (found_keys < length) {
            key_type nvm_key = UINT64_MAX;
            key_type min_buffer_key = UINT64_MAX;
            int min_buffer_idx = -1;
            struct write_buffer_index_node *min_buffer_node = NULL;

            // 获取 nvm_index 中的下一个键
            if (slot_idx < current_subspace->slot_num &&
                slot_element_idx < current_subspace->slot_key_num[slot_idx]) {
                nvm_key = slot_element[slot_element_idx].key;
            }

            // 在所有 write_buffer_index 中找到最小的键
            for (int j = 0; j < buffer_index_count; j++) {
                if (buffer_curr_array[j] != NULL) {
                    if (buffer_curr_array[j]->key < min_buffer_key) {
                        min_buffer_key = buffer_curr_array[j]->key;
                        min_buffer_idx = j;
                        min_buffer_node = buffer_curr_array[j];
                    }
                }
            }


            // 如果两个来源都没有更多的键，跳出循环
            if (nvm_key == UINT64_MAX && min_buffer_key == UINT64_MAX) {
                break;
            }

            // 选择较小的键
            if (min_buffer_key != UINT64_MAX && (nvm_key == UINT64_MAX || min_buffer_key <= nvm_key)) {
                // 使用 write_buffer_index 中的键
                key[found_keys] = min_buffer_key;
                buffer_node[found_keys] = min_buffer_node;  // 保存实际的节点指针
                is_in_write_buffer[found_keys] = true;
                found_keys++;

                // 移动到下一个节点
                for (int j = 0; j < buffer_index_count; j++) {
                    if (buffer_curr_array[j] != NULL) {
                        if (buffer_curr_array[j]->key == min_buffer_key) {
                            buffer_curr_array[j] = buffer_curr_array[j]->forward[0];
                        }
                    }
                }

                // 如果 nvm_index 中有相同的键，跳过它
                if (nvm_key == min_buffer_key) {
                    slot_element_idx++;
                    if (slot_element_idx >= current_subspace->slot_key_num[slot_idx]) {
                        slot_idx++;
                        slot_element_idx = 0;
                        if (slot_idx < current_subspace->slot_num) {
                            slot_element = subspace_slot_elements(current_subspace, (uint32_t)slot_idx);
                        }
                    }
                }
            } else {
                // 使用 nvm_index 中的键
                key[found_keys] = nvm_key;
                addr[found_keys] = slot_element[slot_element_idx].addr;
                is_in_write_buffer[found_keys] = false;
                found_keys++;

                // 移动到 nvm_index 中的下一个位置
                slot_element_idx++;
                if (slot_element_idx >= current_subspace->slot_key_num[slot_idx]) {
                    slot_idx++;
                    slot_element_idx = 0;
                    if (slot_idx < current_subspace->slot_num) {
                        slot_element = subspace_slot_elements(current_subspace, (uint32_t)slot_idx);
                    }
                }
            }
        }

        // 清理本轮分配的内存
        if (buffer_curr_array) {
            free(buffer_curr_array);
        }

        // 如果已经找到足够的键，跳出循环
        valid_key_per_subspace[i] = found_keys - found_keys_last_subspace;
        found_keys_last_subspace = found_keys;

        if (found_keys >= length) {
            break;
        }
    }
    if(i == subspace_num){
        *valid_subspace = i;
    }
    else{
        *valid_subspace = i+1;
    }

    return found_keys;
}


int addr_to_block(struct address_type *addr, int *is_in_write_buffer, int *kvcache_hit,
                 uint32_t *block, int found_key) {
    int block_count = 0;

    // 首先收集所有需要从磁盘读取的块号
    for (int i = 0; i < found_key; i++) {
        // 只处理既不在写缓冲区中，也不在缓存中的地址
        if (!is_in_write_buffer[i] && !kvcache_hit[i]) {
            block[block_count++] = addr[i].block;
        }
    }

    // 如果没有需要从磁盘读取的块，直接返回
    if (block_count == 0) {
        return 0;
    }

    // 使用已有的归并排序并去重函数
    block_count = dedup_merge_sort(block, block_count);

    return block_count;
}

bool scan(key_type key1 , int length , char * value , struct thread * thread , uint64_t start){

    struct scan_kv * scan_result = (struct scan_kv *)value;
    struct subspace_info ** subspace_info_array;
    struct address_type * addr = (struct address_type *)malloc(sizeof(struct address_type) * length);
    struct write_buffer_index_node ** buffer_node = (struct write_buffer_index_node **)malloc(sizeof(struct write_buffer_index_node *) * length);
    int * is_in_write_buffer = (int *)malloc(sizeof(int) * length);
    int * kvcache_hit = (int *)malloc(sizeof(int) * length);
    struct task * task;
    uint32_t * block = (uint32_t *)malloc(sizeof(uint32_t) * length);
    int block_num;
    int subspace_num;
    int valid_subspace;
    int * valid_key_per_subspace;
    int found_key;
    int write_buffer_hit_num = 0;
    int kvcache_hit_num = 0;
    int n = 0;


    subspace_info_array = dram_index_scan(thread, key1 , length , &subspace_num , &found_key);

    if(subspace_info_array == NULL){
        printf("ERROR:scan:fail to get subspace for key %lu\n" , key1);
        return 1;
    }
    scan_result->key_num = length;


    valid_key_per_subspace = (int *)malloc(sizeof(int) * subspace_num);
    found_key = subspace_write_buffer_leaf_node_scan(subspace_info_array , subspace_num,  key1 ,length ,
    scan_result->key,  addr , buffer_node , is_in_write_buffer , &valid_subspace , valid_key_per_subspace);

    if(found_key == 0){
        free(addr);
        free(buffer_node);
        free(is_in_write_buffer);
        free(block);
        free(kvcache_hit);
        server_submit_result(key1 , value , found_key , 2, 0 , thread->spdk_result_ring[key1 % thread->db->load_injectors]
         , start , NULL);
        return EXIT_SUCCESS;
    }
    //优先减少磁盘的带宽占用
    memset(kvcache_hit , 0 , sizeof(int) * length);

    for(int i = 0 ; i < valid_subspace ; i++){
        kvcache_hit_num += kvcache_get_batch(subspace_info_array[i]->kvcache ,thread->lru_list ,
        &scan_result->key[n] ,&scan_result->value[n] , &scan_result->value_size[n] ,  valid_key_per_subspace[i] ,
        &kvcache_hit[n] , &is_in_write_buffer[n]);
        n += valid_key_per_subspace[i];
    }

    block_num = addr_to_block(addr , is_in_write_buffer , kvcache_hit ,  block , found_key);

    for(int i = 0 ; i < found_key ; i++){
        if(kvcache_hit[i] == 1){//kvcache命中
            continue;
        }
        else if(is_in_write_buffer[i] == 1){//kvcache不命中但在NVM上
            write_buffer_hit_num ++;
            const char *record = (const char *)pmemobj_direct(buffer_node[i]->id);
            struct kv_record_view view;
            access_nvm();
            int decode_rc = kv_record_decode_for_key(record, buffer_node[i]->size,
                                                     scan_result->key[i], &view);
            if(decode_rc == 0){
                scan_result->value[i] = (char *)malloc(view.value_size);
                scan_result->value_size[i] = view.value_size;
                if(kv_record_copy_valid_value(scan_result->value[i],
                                                  view.value_size, &view) != 0){
                    decode_rc = -1;
                }
            }
            access_nvm_release();
            if(decode_rc != 0){
                fprintf(stderr, "fatal: invalid write-buffer KV record for scan key %lu\n",
                        (unsigned long)scan_result->key[i]);
                exit(EXIT_FAILURE);
            }
        }
        else{//需要从ssd中读
            if(addr[i].size < KV_RECORD_HEADER_SIZE){
                fprintf(stderr, "fatal: SSD KV record is smaller than its header\n");
                exit(EXIT_FAILURE);
            }
            scan_result->value[i] = (char *)malloc(addr[i].size - KV_RECORD_HEADER_SIZE);
            scan_result->value_size[i] = addr[i].size - KV_RECORD_HEADER_SIZE;
            continue;
        }
    }


    thread_kvcache_hit[thread->thread] += kvcache_hit_num;
    thread_kvbuffer_hit[thread->thread] += write_buffer_hit_num;
    thread_scan_total[thread->thread] += found_key;
    thread_scan_on_block[thread->thread] += (found_key - kvcache_hit_num - write_buffer_hit_num);


    if(block_num == 0){
        free(addr);
        free(buffer_node);
        free(is_in_write_buffer);
        free(block);
        free(kvcache_hit);
        free(subspace_info_array);
        free(valid_key_per_subspace);
        server_submit_result(key1 , value , found_key , 2, 0 ,thread->spdk_result_ring[key1 % thread->db->load_injectors]
         , start , NULL);
        return 0;
    }



    int qpair_excuting_once , rc ;
    int qpair_num = thread->thread % ctr_max_qp;
    task = get_free_task(thread->ns_entry , thread->thread);
    while(task == NULL){//qpair已满
        if((qpair_excuting_once = spdk_nvme_qpair_process_completions(thread->ns_entry->qpair[qpair_num], 0)) < 0){
            printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
            return 1;
        }
        if(qpair_excuting_once != 0){
            task = get_free_task(thread->ns_entry , thread->thread);
        }
    }

    task->key = key1;
    task->block_array = block;
    task->total_block_num = block_num;
    task->thread = thread;
    task->block_completed = 0;
    task->buffered_addr = addr;
    task->buffered_num = found_key;
    task->scan_result = scan_result;
    task->is_in_write_buffer = is_in_write_buffer;
    task->kvcache_hit = kvcache_hit;
    task->start = start;

    task->subspace_info_array = (void **)subspace_info_array;
    task->subspace_num = valid_subspace;
    task->valid_key_per_subspace = valid_key_per_subspace;


    for(int i = 0 ; i < block_num ; i++){
        thread_block_rw[thread->thread] ++;
        thread_scan_block[thread->thread] ++;
        rc = spdk_nvme_ns_cmd_read(thread->ns_entry->ns, thread->ns_entry->qpair[qpair_num], task->buffer + i * BLOCK_SIZE,
        block[i] * physical_block_to_logical_block , physical_block_to_logical_block, scan_cb , task , 0);
        if (rc != 0) {
            printf("ERROR:starting read I/O failed\n");
            return 1;
        }
        else{
        }
    }


    free(buffer_node);
    return 0;
}
