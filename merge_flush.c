#include "./merge_flush.h"

/*将write_buffer_index中不等于last_insert的键值对写入buffer*/
// void task_buffer_fill(struct write_buffer_index * write_buffer_index , char * buffer , key_type last_insert){

//     for(int i = 0 ; i < write_buffer_index->key_num ; i++){
//         strncpy(buffer , pmemobj_direct(write_buffer_index->children[i]->id) , write_buffer_index->children[i]->size);
//         // strcpy_s(buffer, write_buffer_index->children[i]->size , pmemobj_direct(write_buffer_index->children[i]->id));
//         buffer += write_buffer_index->children[i]->size;
//     }

//     return ;
// }

// int flush_write_buffer_to_ssd(struct subspace_info * subspace_info , int thread , struct ns_entry * ns_entry , uint32_t block , void (*cb)(void *, const struct spdk_nvme_cpl *) , key_type last_insert){
//     struct task * task;
//     int qpair_excuting_once , rc ;
//     int qpair_num = thread % ctr_max_qp;

//     qpair_lock(thread , 2);
//     task = get_free_task(ns_entry , thread);
//     while(task == NULL){//qpair已满
//         if((qpair_excuting_once = spdk_nvme_qpair_process_completions(ns_entry->qpair[qpair_num], 0)) < 0){
//                 printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
//                 return 0;
//         }
//         if(qpair_excuting_once != 0){
//             task = get_free_task(ns_entry , thread);
//         }
//     }
//     write_buffer_copy(subspace_info->buffer_index , task->buffer  , last_insert);

//     task->block = block;
//     task->subspace_info = subspace_info;
//     task->last_insert = last_insert;
//     // block = get_free_block();

//     rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[qpair_num], task->buffer, block * physical_block_to_logical_block ,physical_block_to_logical_block
//                         , cb , task , 0);
//     if (rc != 0) {
//         printf("ERROR:starting write I/O failed\n");
//         qpair_unlock(thread);
//         return 0;
//     }
//     qpair_unlock(thread);
//     return 1;
// }

/*flush_cb首先会更改nvm_index索引，从而在系统崩溃时无数据丢失
然后根据subspace_info里的write_buffer_index回收write_buffer，然后再清空write_buffer_index
然后需要检查subspace_info里的键的数据是否大于阈值，从而决定是否分裂
*/
