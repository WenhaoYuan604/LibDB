#include "./task.h"

int no_free_task(struct ns_entry * ns_entry , int thread){
    if(ns_entry->task_array_head[thread] == NULL){//没有空task
        // printf("ns_entry->task_array_head[thread] == NULL no free task\n");
        return 1;
    }
    return 0;
}

struct task * get_free_task(struct ns_entry * ns_entry , int thread){
    struct task * task;
    if(ns_entry->task_array_head[thread] == NULL){//没有空task
        // printf("ns_entry->task_array_head[thread] == NULL no free task\n");
        return NULL;
    }
    else if(ns_entry->task_array_head[thread] == ns_entry->task_array_tail[thread]){//只有一个空task
        task = ns_entry->task_array_head[thread];
        ns_entry->task_array_head[thread] = NULL;
        ns_entry->task_array_tail[thread] = NULL;
        task->thread_seq = thread;
        return task;
    }
    else{
        task = ns_entry->task_array_head[thread];
        ns_entry->task_array_head[thread] = task->next_free;
        task->thread_seq = thread;
        return task;
    }
}

void free_task(struct task * task){
    struct ns_entry * ns_entry = task->ns_entry;
    int thread = task->thread_seq;
    if(ns_entry->task_array_tail[thread] == NULL){//没有空task
        ns_entry->task_array_head[thread] = task;
        ns_entry->task_array_tail[thread] = task;
        task->next_free = NULL;
    }
    else{
        ns_entry->task_array_tail[thread]->next_free = task;
        ns_entry->task_array_tail[thread] = task;
        task->next_free = NULL;
    }
    // memset(task->buffer , 0 , BLOCK_SIZE * Flush_Blcok_Num);
    return ;
}