#ifndef WRITE_BUFFER_INDEX_H_
#define WRITE_BUFFER_INDEX_H_
#include "./pmdk.h"
#include "./base.h"
/* 不可 #include subspace.h：会与 subspace.h 互相包含，导致 subspace_info 未定义即被使用 */
struct subspace_info;
// #include "./kvbuffer.h"

struct write_buffer_return_type{
    PMEMoid id;
    uint16_t WBF_seq;
    uint16_t size;
};


// struct write_buffer_index_node {

//     key_type key;
//     uint16_t size;
//     PMEMoid id;
//     uint8_t WBF_seq;
//     struct write_buffer_index_node **forward;
// };


struct write_buffer_index_node {
    key_type key;
    uint16_t size;
    uint16_t WBF_seq;
    PMEMoid id;
    struct write_buffer_index_node **forward;
};

struct write_buffer_index {
    struct write_buffer_index_node *head;
    struct write_buffer_index * next;
    struct write_buffer_index * prev;

    /*next和prev的作用是记录刷写的多个write_buffer_list*/
    /*head_to_free的每个元素记录一次flush的头结点*/
    // struct write_buffer_index_node * head_to_free[8];
    // int num;
    size_t levels;
    size_t reserve_levels;
};

#ifdef __cplusplus
extern "C" {
#endif

extern struct write_buffer_return_type RETURN_NULL ;

void write_buffer_index_init(struct write_buffer_index *list);

void write_buffer_index_destroy(struct write_buffer_index *list);

struct write_buffer_index_node* write_buffer_index_before(struct write_buffer_index *list, key_type key);

struct write_buffer_index_node* write_buffer_index_search(struct write_buffer_index *list, key_type key);

struct write_buffer_return_type write_buffer_index_insert(struct write_buffer_index *list, key_type key, int size,
PMEMoid id , uint16_t WBF_seq ,int level);

bool write_buffer_index_delete(struct write_buffer_index *list, key_type key, bool free_value);

size_t write_buffer_index_flush_size(struct write_buffer_index *buffer_index, key_type last_insert);

/*返回write_buffer_index中除了last_insert的key的数目
将buffer_index中的PM地址转化为SSD地址，并保存在*addr*/
int write_buffer_index_to_array(struct write_buffer_index * buffer_index , uint32_t * block ,  key_type last_insert , key_type ** key ,
struct address_type ** addr , struct write_buffer_return_type ** return_value);

struct write_buffer_index * write_buffer_copy (struct write_buffer_index * buffer_index , char * copy ,
 key_type last_insert , int thread);

 void write_buffer_index_check(struct subspace_info * subspace_info);
// void write_buffer_index_update_before_flush(struct write_buffer_index * write_buffer_index , key_type last_insert );

// void  write_buffer_index_update_after_flush(struct write_buffer_index * write_buffer_index , key_type last_insert);

#ifdef __cplusplus
}
#endif

#endif
