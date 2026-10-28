#include "./kvcache.h"
#include "base.h"
#include "client.h"
#include "kv_record.h"
#include <string.h>

// 创建新节点
static skipnode* create_node(int level, key_type key, const char* value , int length,
skipnode * head) {
    skipnode *node = (skipnode*)malloc(sizeof(skipnode));
    if (!node) return NULL;

    node->key = key;
    node->value = (char *)malloc(length);
    memcpy(node->value, value, (size_t)length);

    node->length = length;
    node->head = head;
    node->level = level;
    node->forward = (skipnode**)malloc(level * sizeof(skipnode*));
    node->lru_prev = node->lru_next = NULL;

    return node;
}



// 调整LRU链表
static void adjust_lru(lru_list *list, skipnode *node) {
    if (node == list->lru_head) return;

    // 从原位置解链
    if (node->lru_prev) node->lru_prev->lru_next = node->lru_next;
    if (node->lru_next) node->lru_next->lru_prev = node->lru_prev;
    if (node == list->lru_tail) list->lru_tail = node->lru_prev;

    // 插入到链表头部
    node->lru_prev = NULL;
    node->lru_next = list->lru_head;
    if (list->lru_head) list->lru_head->lru_prev = node;
    list->lru_head = node;
    if (!list->lru_tail) list->lru_tail = node;
}


lru_list* lru_list_init(size_t capacity) {
    lru_list *list = (lru_list*)malloc(sizeof(lru_list));
    if (!list) return NULL;

    list->capacity = capacity;
    list->size = 0;
    list->lru_head = list->lru_tail = NULL;

    return list;
}

kvcache* kvcache_init() {
    kvcache *cache = (kvcache*)malloc(sizeof(kvcache));
    if (!cache) return NULL;

    cache->current_level = 1;

    // 初始化跳表头节点
    /* 字面量 "head" 仅 5 字节（含 '\\0'），length 不得大于源长度以免 memcpy 越界读 */
    cache->head = create_node(MAX_LEVEL, 0, "head", (int)sizeof("head") , NULL);
    cache->head->head = cache->head;
    cache->head->level = 1;
    for (int i = 0; i < MAX_LEVEL; i++)
        cache->head->forward[i] = NULL;

    srand(time(NULL));
    return cache;
}

char * kvcache_get_emulation(kvcache *cache, lru_list *list, key_type key, int *length , int thread){
    int rand = rand_r(&random_seed[thread]) % 10;
    int size = 1024;
    if(rand <= 6){
        char * value = (char *)malloc(size);
        *length = size;
        client_key_to_value(key , value , size);
        return value;
    }
    return NULL;
}

// 查找缓存
char* kvcache_get(kvcache *cache, lru_list *list, key_type key, int *length) {
    skipnode *curr = cache->head;

    // 跳表查找
    for (int i = cache->current_level-1; i >= 0; i--) {
        while (curr->forward[i] && curr->forward[i]->key < key)
            curr = curr->forward[i];
    }
    curr = curr->forward[0];

    if (curr && curr->key == key) {
        adjust_lru(list, curr);  // 更新LRU位置
        *length = curr->length;
        char * value = (char *)malloc(*length);
        memcpy(value, curr->value, (size_t)*length);
        return value;
    }
    return NULL;
}

void kvcache_insert_emulation(kvcache *cache, lru_list *list, key_type key, const char* value,
int length, int thread){
    char * new_value = (char *)malloc(length);
    memcpy(new_value ,value , length );
    free(new_value);
    return ;
}


// 插入缓存
void kvcache_insert(kvcache *cache, lru_list *list, key_type key, const char* value,
int length, int thread) {
    skipnode *update[MAX_LEVEL];
    skipnode *curr = cache->head;

    if(list->capacity < length){
        return;
    }

    // 查找插入位置并记录路径
    for (int i = cache->current_level-1; i >= 0; i--) {
        while (curr->forward[i] && curr->forward[i]->key < key)
            curr = curr->forward[i];
        update[i] = curr;
    }
    curr = curr->forward[0];

    // 键已存在则更新值
    if (curr && curr->key == key) {
        free(curr->value);
        curr->value = (char *)malloc(length);
        memcpy(curr->value, value, (size_t)length);
        curr->length = length;
        adjust_lru(list, curr);
        return;
    }

    // 创建新节点
    int level = random_level(MAX_LEVEL - 1, &random_seed[thread]) + 1;
    if (level > cache->current_level) {
        for (int i = cache->current_level; i < level; i++)
            update[i] = cache->head;
        cache->current_level = level;
        cache->head->level = level;
    }

    skipnode *node = create_node(level, key, value, length ,cache->head);
    if (!node) return;


    // 更新跳表指针
    for (int i = 0; i < level; i++) {
        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;
    }

    // 更新LRU链表
    node->lru_next = list->lru_head;
    if (list->lru_head) list->lru_head->lru_prev = node;
    list->lru_head = node;
    if (!list->lru_tail) list->lru_tail = node;
    list->size = list->size + length;

    // 淘汰策略
    while (list->size > list->capacity) {
        skipnode *old = list->lru_tail;
        if (!old) break;

        // 从跳表删除
        skipnode *curr = old->head;
        for (int i = old->head->level - 1; i >= 0; i--) {
            while (curr->forward[i] && curr->forward[i]->key < old->key)
                curr = curr->forward[i];
            if (curr->forward[i] == old){
                curr->forward[i] = old->forward[i];
            }
            else if(i <= old->level - 1){
                printf("kvcache insert:curr->forward[%d] is %p level %d old is %p level %d\n", i, curr->forward[i], curr->forward[i]->level, old, old->level);
                sleep(200);
            }
        }

        // 从LRU链表删除
        if (old->lru_prev) old->lru_prev->lru_next = NULL;
        list->lru_tail = old->lru_prev;
        if (old == list->lru_head) list->lru_head = NULL;

        // 释放资源
        list->size -= old->length;
        free(old->value);
        free(old->forward);
        free(old);
    }
}


uint64_t kvcache_get_batch(kvcache *cache, lru_list *list, key_type *keys, char **values
, int *value_size, int num_keys, int *cache_hits , int * buffer_hits) {
    // 初始化所有键为未命中
    uint64_t hit_num = 0;
    for (int i = 0; i < num_keys; i++) {
        cache_hits[i] = false;
    }

    // 如果没有键需要查询，直接返回
    if (num_keys == 0) return 0;

    // 找到第一个键的位置
    skipnode *curr = cache->head;
    key_type first_key = keys[0];

    // 使用跳表查找第一个键的位置
    for (int i = cache->current_level-1; i >= 0; i--) {
        while (curr->forward[i] && curr->forward[i]->key < first_key)
            curr = curr->forward[i];
    }
    curr = curr->forward[0];

    // 从找到的位置开始，遍历底层链表
    int key_idx = 0;
    while (curr && key_idx < num_keys) {
        // 如果当前节点的键等于要查找的键

        if(buffer_hits[key_idx] == 1){
            key_idx ++;
            continue;
        }

        if (curr->key == keys[key_idx]) {
            // 更新LRU位置
            hit_num++;
            adjust_lru(list, curr);

            // 为值分配内存并复制
            values[key_idx] = (char *)malloc(curr->length);
            value_size[key_idx] = curr->length;
            if (values[key_idx]) {
                memcpy(values[key_idx], curr->value, curr->length);
                cache_hits[key_idx] = true; // 标记命中
            }

            // 移动到下一个要查找的键
            key_idx++;
        }
        // 如果当前节点的键大于要查找的键，说明要查找的键不在缓存中
        else if (curr->key > keys[key_idx]) {
            key_idx++;
            continue; // 不移动 curr，继续比较下一个要查找的键
        }

        // 移动到跳表的下一个节点
        curr = curr->forward[0];
    }
    return hit_num;
}

void kvcache_update_batch(kvcache *cache, lru_list *list, key_type *keys, char **values, int *value_size, int num_keys,
int *cache_hits, int *buffer_hit, int thread) {
    if (num_keys == 0) return;

    skipnode *update[MAX_LEVEL];
    skipnode *curr = cache->head;
    skipnode *new_list_head = NULL;
    skipnode *old;
    size_t total_new_size = 0;
    int last_level = 0;
    for (int i = 0; i < num_keys; i++) {
        cache_hits[i] += buffer_hit[i];
        if(!cache_hits[i]){
            total_new_size += value_size[i];
        }
    }

    while (list->size + total_new_size > list->capacity && list->lru_tail) {
        old = list->lru_tail;
        list->size -= old->length;

        // 从跳表中删除
        skipnode *temp = old->head;
        for (int i = old->head->level - 1; i >= 0; i--) {
            while (temp->forward[i] && temp->forward[i]->key < old->key) {
                temp = temp->forward[i];
            }
            if (temp->forward[i] == old) {
                temp->forward[i] = old->forward[i];
            }
            else if(i <= old->level - 1){
                printf("batch update:temp->forward[%d] is %p key %lu old is %p level %d key %lu\n", i, temp->forward[i], temp->forward[i]->key, old, old->level, old->key);
                sleep(200);
            }
        }

        // 从LRU链表中删除
        list->lru_tail = old->lru_prev;
        if (list->lru_tail) {
            list->lru_tail->lru_next = NULL;
        }
        if (old == list->lru_head) {
            list->lru_head = NULL;
        }

        free(old->value);
        free(old->forward);
        free(old);
    }

    list->size += total_new_size;

    // 第二步：创建所有新节点并链接成一个有序链表
    for (int i = 0; i < num_keys; i++) {
        if (cache_hits[i]){
            continue;
        }
        /*查找插入節點在條表中的位置*/
        else if(new_list_head == NULL){//cache不命中且是第一個插入的節點
            curr = cache->head;
            for (int j = cache->current_level-1; j >= 0; j--) {
                while (curr->forward[j] && curr->forward[j]->key < keys[i]) {
                    curr = curr->forward[j];
                }
                update[j] = curr;//記錄第一個插入節點的前驅
            }
            if(curr->forward[0] && curr->forward[0]->key == keys[i]){//不命中但是kvcache存在該節點
                continue;
            }
        }
        else{
            if(curr == NULL){
                printf("curr is NULL\n");
                sleep(200);
            }
            last_level = curr->level;
            for(int j = last_level - 1 ; j >= 0 ; j--){
                while (curr->forward[j] && curr->forward[j]->key < keys[i]) {
                    curr = curr->forward[j];
                    if(curr->level > last_level){
                        for(int k = last_level ; k < curr->level ; k++){
                            update[k] = curr;
                        }
                    }
                }
                update[j] = curr;
            }

            if(curr->forward[0] && curr->forward[0]->key == keys[i]){//不命中但是kvcache存在該節點,说明是
                for(int j = curr->forward[0]->level - 1 ; j >= 0 ; j--){
                    update[j] = curr->forward[j];
                }
                curr = curr->forward[0];
                continue;
            }
        }

        if (value_size[i] >= list->capacity) continue;

        int level = random_level(MAX_LEVEL - 1, &random_seed[thread]) + 1;
        if (level > cache->current_level) {
            for (int j = cache->current_level; j < level; j++){
                update[j] = cache->head;
            }
            cache->current_level = level;
            cache->head->level = level;
        }

        skipnode *node = create_node(level, keys[i], values[i], value_size[i] ,cache->head);
        if (!node) continue;

        // 设置subspace_info

        for (int j = 0; j < level; j++) {
            node->forward[j] = update[j]->forward[j];
            update[j]->forward[j] = node;
            update[j] = node;
        }
        new_list_head = node;

        node->lru_next = list->lru_head;
        if (list->lru_head) {
            list->lru_head->lru_prev = node;
        }
        node->lru_prev = NULL;
        list->lru_head = node;
        if (!list->lru_tail) {
            list->lru_tail = node;
        }
        curr = node;
    }

    return;
}

void kvcache_split(kvcache *cache, kvcache *new_cache, key_type boundary) {
    // 确保两个缓存都已初始化
    if (!cache || !new_cache) return;

    // 从原缓存的头节点开始遍历
    skipnode *curr = cache->head;
    new_cache->current_level = 1;

    // 为每一层找到分割点
    for (int i = cache->current_level - 1; i >= 0; i--) {
        // 找到每层中小于boundary的最后一个节点
        while (curr->forward[i] && curr->forward[i]->key < boundary) {
            curr = curr->forward[i];
        }

        // 将大于等于boundary的节点链接到新缓存
        new_cache->head->forward[i] = curr->forward[i];
        if((new_cache->current_level == 1 && curr->forward[i] != NULL)){
            new_cache->current_level = i+1;
            new_cache->head->level = i+1;
        }

        // 断开原缓存与这些节点的连接
        curr->forward[i] = NULL;
    }


    // 更新所有移动到新缓存的节点的head指针
    skipnode *node = new_cache->head->forward[0];
    while (node) {
        node->head = new_cache->head;
        node = node->forward[0];
    }
    return ;
}

static void kvcache_unlink_and_free_node(lru_list *list, skipnode *node);
void kvcache_delete(kvcache *cache, lru_list *list, key_type key){
    skipnode *node = cache->head;
    for(int i = cache->current_level - 1; i >= 0; i--)
        while(node->forward[i] && node->forward[i]->key < key) node = node->forward[i];
    node = node->forward[0];
    if(node && node->key == key) kvcache_unlink_and_free_node(list, node);
}

void kvcache_merge(kvcache *left, kvcache *right){
    for(skipnode *n = right->head->forward[0]; n; n = n->forward[0]) n->head = left->head;
    for(int i = 0; i < right->current_level; i++){
        skipnode *tail = left->head;
        while(tail->forward[i]) tail = tail->forward[i];
        tail->forward[i] = right->head->forward[i];
        right->head->forward[i] = NULL;
    }
    if(left->current_level < right->current_level) left->current_level = right->current_level;
    left->head->level = left->current_level;
}

void kvcache_destroy_empty(kvcache *cache){
    assert(!cache->head->forward[0]);
    free(cache->head->value);
    free(cache->head->forward);
    free(cache->head);
    free(cache);
}

static void kvcache_unlink_and_free_node(lru_list *list, skipnode *node){
    if(list == NULL || node == NULL || node->head == NULL){
        return;
    }

    skipnode *head = node->head;
    for(int level = head->level - 1; level >= 0; level--){
        skipnode *prev = head;
        while(prev->forward[level] && prev->forward[level]->key < node->key){
            prev = prev->forward[level];
        }
        if(prev->forward[level] == node){
            prev->forward[level] = node->forward[level];
        }
    }

    if(node->lru_prev){
        node->lru_prev->lru_next = node->lru_next;
    }
    else{
        list->lru_head = node->lru_next;
    }
    if(node->lru_next){
        node->lru_next->lru_prev = node->lru_prev;
    }
    else{
        list->lru_tail = node->lru_prev;
    }
    if(list->size >= (size_t)node->length){
        list->size -= (size_t)node->length;
    }
    else{
        list->size = 0;
    }

    free(node->value);
    free(node->forward);
    free(node);
}

int kvcache_refresh_after_flush(kvcache *cache, lru_list *list,
                                const char *buffer, size_t buffer_size,
                                const struct address_type *addr,
                                const key_type *keys, int num_keys){
    if(cache == NULL || cache->head == NULL || list == NULL ||
       buffer == NULL || addr == NULL || keys == NULL || num_keys < 0){
        return -1;
    }
    if(num_keys == 0){
        return 0;
    }

    skipnode *cached = cache->head->forward[0];
    size_t packed_offset = 0;
    int refreshed = 0;

    for(int i = 0; i < num_keys; i++){
        if(i > 0 && keys[i] <= keys[i - 1]){
            return -1;
        }
        if(addr[i].size <= 0 || addr[i].size > (int)BLOCK_SIZE){
            return -1;
        }

        size_t record_size = (size_t)addr[i].size;
        size_t block_offset = packed_offset % BLOCK_SIZE;
        if(block_offset != 0 && block_offset + record_size > BLOCK_SIZE){
            packed_offset += BLOCK_SIZE - block_offset;
        }
        if(packed_offset > buffer_size ||
           record_size > buffer_size - packed_offset ||
           addr[i].off != (int)(packed_offset % BLOCK_SIZE)){
            return -1;
        }

        while(cached != NULL && cached->key < keys[i]){
            cached = cached->forward[0];
        }
        if(cached != NULL && cached->key == keys[i]){
            struct kv_record_view view;
            if(kv_record_decode_for_key(buffer + packed_offset, record_size,
                                        keys[i], &view) != 0){
                return -1;
            }

            char *new_value = (char *)malloc(view.value_size);
            if(new_value == NULL ||
               kv_record_copy_valid_value(new_value, view.value_size, &view) != 0){
                free(new_value);
                return -1;
            }

            if(list->size >= (size_t)cached->length){
                list->size -= (size_t)cached->length;
            }
            else{
                list->size = 0;
            }
            list->size += view.value_size;
            free(cached->value);
            cached->value = new_value;
            cached->length = (int)view.value_size;
            cached = cached->forward[0];
            refreshed++;
        }

        packed_offset += record_size;
    }

    /* A variable-size update may grow an existing cached value. */
    while(list->size > list->capacity && list->lru_tail != NULL){
        kvcache_unlink_and_free_node(list, list->lru_tail);
    }
    return refreshed;
}
