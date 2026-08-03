#ifndef C29F8F3E_ADFA_435D_9690_CB8EAEFD760B
#define C29F8F3E_ADFA_435D_9690_CB8EAEFD760B

#include "./subspace.h"
#include "./index.h"
#include "./task.h"

struct redundant_element{
    uint16_t block_redundant_size;
    uint32_t block;
};

int block_exist(uint32_t block ,uint32_t * block_array ,int block_array_size_tmp);

void garbadge_collection(struct subspace_info * subspace_info ,struct thread * thread);

/*
 * 计算write_buffer_index中所有数据的总大小
 * 返回值：所有KV数据的总大小（字节）
 */
int calculate_buffer_index_size(struct write_buffer_index *buffer_index);

/*
 * 从write_buffer_index中按照键大小从小到大读取足够的数据
 * buffer: 输出缓冲区
 * buffer_index: 输入的write_buffer_index
 * needed_size: 需要读取的数据大小
 * extracted_keys: 输出参数，记录被提取的键，用于后续删除
 * extracted_keys_count: 输出参数，被提取的键的个数
 * 返回值：实际读取的数据大小
 */
int extract_buffer_index_data(char *buffer, struct write_buffer_index *buffer_index,
                              int needed_size, key_type *extracted_keys, int *extracted_keys_count);

#endif /* C29F8F3E_ADFA_435D_9690_CB8EAEFD760B */
