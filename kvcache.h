
#ifndef DDE2613F_91F7_46C9_B811_A96CE855E3BF
#define DDE2613F_91F7_46C9_B811_A96CE855E3BF
#include "dram_index.h"
#include "subspace.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "./base.h"

// 跳表节点结构
typedef struct skipnode {
    key_type key;
    char *value;
    int length;
    struct skipnode **forward;   // 跳表指针数组
    struct skipnode *lru_prev;   // LRU双向链表前驱
    struct skipnode *lru_next;   // LRU双向链表后继
    struct skipnode * head;//在分裂时需要修改每个节点的subspace_info
    int level;                   // 节点层数
} skipnode;

// 缓存结构
typedef struct kvcache {
    skipnode *head;              // 跳表头节点
    int current_level;           // 当前最大层数
} kvcache;

typedef struct lru_list{
    size_t capacity;             // 缓存容量
    size_t size;
    skipnode *lru_head;          // LRU链表头
    skipnode *lru_tail;
}lru_list;

lru_list * lru_list_init(size_t capacity);

kvcache* kvcache_init();

char* kvcache_get(kvcache *cache, lru_list *list ,  key_type key , int * length);

char * kvcache_get_emulation(kvcache *cache, lru_list *list, key_type key, int *length , int thread);


void kvcache_insert(kvcache *cache, lru_list *list , key_type key, const char* value ,
 int length ,  int thread);
void kvcache_insert_emulation(kvcache *cache, lru_list *list, key_type key, const char* value,
int length, int thread);

void kvcache_merge(kvcache *left, kvcache *right);
void kvcache_destroy_empty(kvcache *cache);

void kvcache_split(kvcache *cache, kvcache *new_cache, key_type boundary);

uint64_t kvcache_get_batch(kvcache *cache , lru_list *list , key_type *keys, char **values,
 int * value_size, int num_keys, int *cache_hits , int * buffer_hits);

void kvcache_update_batch(kvcache *cache, lru_list *list ,  key_type *keys, char **values,
int * value_size, int num_keys,  int *cache_hits , int * buffer_hit , int thread);

void kvcache_delete(kvcache * cache , lru_list *list, key_type key);

/*
 * Refresh only cache entries that already exist for a completed flush batch.
 * keys and records in buffer must use the same ascending-key, block-aligned
 * packing order produced by write_buffer_copy/write_buffer_index_to_array.
 * Returns the number of refreshed entries, or -1 when the batch is invalid.
 */
int kvcache_refresh_after_flush(kvcache *cache, lru_list *list,
                                const char *buffer, size_t buffer_size,
                                const struct address_type *addr,
                                const key_type *keys, int num_keys);

#endif
