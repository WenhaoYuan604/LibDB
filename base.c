#include "./base.h"
#include "kv_record.h"
#include <stdatomic.h>
#include <time.h>

struct address_type ADDR_NULL = {0,0,0};
atomic_int active_thread = 0;
int MAX_NVM_CONCURRRENT = 8;
atomic_int nvm_access_conflicts = 0;

atomic_int active_thread_disk = 0;
int MAX_DISK_CONCURRENT = 1;

#define KV_VERSION_BATCH_SIZE 65536ULL

static atomic_uint_fast64_t g_kv_record_version_next = 0;

static kv_version_type kv_record_clock_seed(void){
    struct timespec now;
    if(clock_gettime(CLOCK_REALTIME, &now) != 0){
        perror("clock_gettime(CLOCK_REALTIME)");
        abort();
    }
    kv_version_type seed =
        (kv_version_type)now.tv_sec * 1000000000ULL +
        (kv_version_type)now.tv_nsec;
    return seed == 0 ? 1 : seed;
}

static kv_version_type kv_record_reserve_version_batch(void){
    uint_fast64_t observed = atomic_load_explicit(
        &g_kv_record_version_next, memory_order_relaxed);

    for(;;){
        kv_version_type batch_start = (kv_version_type)observed;
        if(batch_start == 0){
            batch_start = kv_record_clock_seed();
        }
        if(batch_start > UINT64_MAX - KV_VERSION_BATCH_SIZE){
            fprintf(stderr, "global KV version counter exhausted\n");
            abort();
        }

        uint_fast64_t desired =
            (uint_fast64_t)(batch_start + KV_VERSION_BATCH_SIZE);
        if(atomic_compare_exchange_weak_explicit(
                &g_kv_record_version_next, &observed, desired,
                memory_order_relaxed, memory_order_relaxed)){
            return batch_start;
        }
    }
}

kv_version_type kv_record_next_thread_version(
        kv_version_type *next_version, kv_version_type *version_limit){
    if(next_version == NULL || version_limit == NULL){
        fprintf(stderr, "kv_record_next_thread_version: NULL state\n");
        abort();
    }
    if(*next_version > *version_limit){
        fprintf(stderr, "invalid per-worker KV version range\n");
        abort();
    }
    if(*next_version == *version_limit){
        *next_version = kv_record_reserve_version_batch();
        *version_limit = *next_version + KV_VERSION_BATCH_SIZE;
    }
    return (*next_version)++;
}

void access_disk(){
	// int expected;
	// int success;
	// int desired;
	// struct timespec ts = {0, 10};

	// do{
	// 	expected = atomic_load(&active_thread_disk);
	// 	if(expected >= MAX_NVM_CONCURRRENT){
	// 		success = 0;
	// 		nanosleep(&ts, NULL);
	// 		continue;
	// 	}
	// 	desired = expected + 1;
	// 	success = atomic_compare_exchange_weak(&active_thread_disk,&expected , desired);

	// }while(!success);
	// return ;
}

void access_disk_release(){
	// atomic_fetch_sub(&active_thread_disk , 1);
	// return ;
}


void access_nvm(){
	// int expected;
	// int success;
	// int desired;
	// struct timespec ts = {0, 10};

	// do{
	// 	expected = atomic_load(&active_thread);
	// 	if(expected >= MAX_NVM_CONCURRRENT){
	// 		// atomic_fetch_add(&nvm_access_conflicts, 1);  // 记录冲突次数
	// 		success = 0;
	// 		nanosleep(&ts, NULL);
	// 		continue;
	// 	}
	// 	desired = expected + 1;
	// 	success = atomic_compare_exchange_weak(&active_thread,&expected , desired);

	// }while(!success);
	// return ;
}

// 定期输出统计信息
void print_nvm_stats() {
    printf("NVM access conflicts: %d active_thread is %d\n", atomic_load(&nvm_access_conflicts) , active_thread);
	atomic_store(&nvm_access_conflicts , 0);
}

void access_nvm_release(){
	// atomic_fetch_sub(&active_thread , 1);
	// return ;
}

void random_seed_initialize(int thread){
    random_seed = (unsigned int *)malloc(sizeof(unsigned int) * thread);
    for(int i = 0 ; i < thread ; i++){
        random_seed[i] = i;
    }

	thread_scan_block = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_scan_block , 0 , sizeof(uint64_t) * thread);

	thread_block_rw = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_block_rw , 0 , sizeof(uint64_t) * thread);


	thread_flush_block_write = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_flush_block_write , 0 , sizeof(uint64_t) * thread);

	thread_gc_block_rw = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_gc_block_rw , 0 , sizeof(uint64_t) * thread);

	thread_gc_block_write = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_gc_block_write , 0 , sizeof(uint64_t) * thread);


	thread_split_block_write = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_split_block_write , 0 , sizeof(uint64_t) * thread);

	invalid_block_num = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(invalid_block_num , 0 , sizeof(uint64_t) * thread);

	thread_split_block_rw = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_split_block_rw , 0 , sizeof(uint64_t) * thread);

	thread_kvcache_hit = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_kvcache_hit , 0 , sizeof(uint64_t) * thread);

	thread_kvbuffer_hit = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_kvbuffer_hit , 0 , sizeof(uint64_t) * thread);

	thread_scan_total = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_scan_total , 0 , sizeof(uint64_t) * thread);

	thread_scan_on_block = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_scan_on_block , 0 , sizeof(uint64_t) * thread);

	thread_completed = (uint64_t *)malloc(sizeof(uint64_t) * thread);
	memset(thread_completed , 0 , sizeof(uint64_t) * thread);

	insert_breakdown = (struct insert_time_breakdown *)malloc(sizeof(struct insert_time_breakdown) * thread);
	memset(insert_breakdown , 0 , sizeof(struct insert_time_breakdown) * thread);

	read_breakdown = (struct read_time_breakdown *)malloc(sizeof(struct read_time_breakdown) * thread);
	memset(read_breakdown , 0 , sizeof(struct read_time_breakdown) * thread);

    return ;
}

/*返回值为0到max*/
// int random_level(int max , unsigned int * seed)
// {
// 	int i = 0, level = 0;
// 	for (i = 0; i < max; i++)
// 	{
// 		if ((rand_r(seed) % 4) == 0)
// 		{
// 			level++;
// 		}
// 		else{
// 			break;
// 		}
// 	}
// 	// printf("level is %d\n" , level);
// 	return level;
// }

int random_level(int max , unsigned int * seed)
{
    int level = 0;  // 从0
    while (level < max && !((rand_r(seed) & 0x3))) {  // 使用位运算代替取模，概率为1/4
        level++;
    }
    return level;
}


// int random_level(int max , unsigned int * seed)
// {
// 	return 1;
// }

// void qpair_lock(int thread , int place_for_debug){
// 	int qp_num = thread % ctr_max_qp;
// 	if(g_qpair_lock_is_used[qp_num] == 1){//该qp被共享
// 		if(pthread_mutex_lock(&g_qpair_lock[qp_num]) != 0){
// 			printf("ERROR:pthread_mutex_lock %d failed\n" , qp_num);
// 		}
// 		// else{
// 		// 	printf("thread %d lock %d\n" , thread , qp_num);
// 		// }
// 	}
// 	return ;
// }

// void qpair_unlock(int thread){
// 	int qp_num = thread % ctr_max_qp;
// 	if(g_qpair_lock_is_used[qp_num] == 1){//该qp被共享
// 		pthread_mutex_unlock(&g_qpair_lock[qp_num]);
// 		// printf("thread %d unlock %d\n" , thread , qp_num);
// 	}

// 	return ;
// }
