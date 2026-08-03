#include "./nvme_pipe.h"

// struct task * get_free_task(struct ns_entry * ns_entry , int thread){
//     struct task * task;
//     if(ns_entry->task_array_head[thread] == NULL){//没有空task
//         return NULL;
//     }
//     else if(ns_entry->task_array_head[thread] == ns_entry->task_array_tail[thread]){//只有一个空task
//         task = ns_entry->task_array_head[thread];
//         ns_entry->task_array_head[thread] = NULL;
//         ns_entry->task_array_tail[thread] = NULL;
//         return task;
//     }
//     else{
//         task = ns_entry->task_array_head[thread];
//         ns_entry->task_array_head[thread] = task->next_free;
//     }
// }

// void write_complete(void *arg, const struct spdk_nvme_cpl *completnio){
// 	struct task * task = arg;
// 	task->is_completed = 1;


// 	if(task->buffer != NULL){
// 		free(task->buffer);
// 		task->buffer = NULL;
// 	}
// 	if(task != NULL){
// 		free(task);
// 		task = NULL;
// 	}
// 	printf("task is completed\n");
// 	return ;
// }

// int nvme_write_block(int block_seq ,  struct ns_entry * ns_entry , char * buffer){
//     int rc;
// 	struct task * task;
// 	task = (struct task * )malloc(sizeof(struct task));
// 	task->is_completed = 0;
// 	task->buffer  = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

// 	memcpy(task->buffer , buffer , 4096);

// 	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[0], task->buffer,
// 			block_seq , 1 , write_complete, task, 0);
// 	if (rc != 0) {
// 		fprintf(stderr, "starting write I/O failed\n");
// 		return 0;
// 	}

// 	while (!task->is_completed) {
// 		spdk_nvme_qpair_process_completions(ns_entry->qpair[0], 0);
// 	}

// 	return 1;
// }

// int nvme_read_block(int block_seq ,  struct ns_entry * ns_entry , char * buffer){
//     int rc;
// 	struct task * task;
// 	task = (struct task * )malloc(sizeof(struct task));
// 	task->is_completed = 0;
// 	task->buffer  = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

// 	memcpy(task->buffer , buffer , 4096);

// 	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[0], task->buffer,
// 			block_seq , 1 , write_complete, task, 0);
// 	if (rc != 0) {
// 		fprintf(stderr, "starting write I/O failed\n");
// 		return 0;
// 	}

// 	while (!task->is_completed) {
// 		spdk_nvme_qpair_process_completions(ns_entry->qpair[0], 0);
// 	}

// 	return 1;
// }