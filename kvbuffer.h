#ifndef KVBUFFER_H_
#define KVBUFFER_H_

#include "./pmdk.h"
#include "./base.h"
#include "./subspace.h"
#include "./kv_record.h"
#include "./spdk_struct.h"


struct write_buffer_file_root{
    PMEMoid segment[WSB_NUM_MAX];
    uint16_t segment_element_size[WSB_NUM_MAX];
    uint16_t segment_num;
};

struct write_buffer_free_node{
    uint32_t num;
    uint16_t size;
    uint16_t WBF_seq;
    PMEMoid buffer;
    struct write_buffer_free_node * next;
};

struct write_buffer{//考虑多线程
    uint32_t file_num;
    PMEMobjpool * pool[MAX_FILE_NUM];
};

char * write_buffer_file_str(int file_seq);

PMEMobjpool * write_buffer_file_new(int file_seq);

struct write_buffer * write_buffer_initialize();

/* recovery: 打开已有 write_buffer_file*（layout = "write_buffer"）；找不到返回 NULL */
struct write_buffer * write_buffer_open_existing();

int write_buffer_segement_new(struct write_buffer * write_buffer , int size , struct write_buffer_free_node ** free_list_head);

PMEMoid write_buffer_get_free_node(int size , uint16_t * WBF_seq ,struct write_buffer * write_buffer ,struct write_buffer_free_node ** free_list_head , PMEMobjpool ** pool);

PMEMoid write_buffer_insert(key_type key, kv_version_type version,
 const char *value, int value_size, struct write_buffer *write_buffer,
 uint16_t *WBF_seq, struct write_buffer_free_node **free_list_head, int thread);

int write_buffer_free(PMEMoid id , uint16_t WBF_seq , int size, struct write_buffer_free_node ** free_list_head);


#endif
