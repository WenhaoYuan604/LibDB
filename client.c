#include "./client.h"
#include "./base.h"
#include "index.h"
#include "scan.h"
#include "subspace.h"
#include <spdk/env.h>
#include <spdk/event.h>
#include <stdint.h>
#include "pthread.h"
#include "hist.h"

pthread_barrier_t barrier;
pthread_barrier_t barrier_complete;

float complete_threshold = 1;
struct pending_scan_request pending_scans[MAX_PENDING_SCANS];
int pending_scan_count = 0;      // 当前跟踪的scan请求数量
pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;  // 保护访问scan数组的互斥锁
int benchmark_start = 0;
int test_completed ;

uint64_t ssd_block_space_consumed = 0;


void insert_breakdown_output(struct db * db){
    // double time;
    uint64_t ticks_hz = spdk_get_ticks_hz();
    double total_buffer_insert_time = 0;
    double total_index_insert_time = 0;
    double total_flush_time = 0;
    double total_flush_cb_time = 0;
    double total_gc_time = 0;
    double total_gc_cb_time = 0;
    double total_split_time = 0;
    double total_split_cb_time = 0;

    fprintf(db->output_fp, "\nInsert Breakdown Per Thread (seconds):\n");
    // 输出每个线程的breakdown
    for(int i = 0; i < db->thread; i++){
        double buffer_insert = (double)insert_breakdown[i].buffer_insert_time / ticks_hz;
        double index_insert = (double)insert_breakdown[i].index_insert_time / ticks_hz;
        double flush = (double)insert_breakdown[i].flush_time / ticks_hz;
        double flush_cb = (double)insert_breakdown[i].flush_cb_time / ticks_hz;
        double gc = (double)insert_breakdown[i].gc_time / ticks_hz;
        double gc_cb = (double)insert_breakdown[i].gc_cb_time / ticks_hz;
        double split = (double)insert_breakdown[i].split_time / ticks_hz;
        double split_cb = (double)insert_breakdown[i].split_cb_time / ticks_hz;

        fprintf(db->output_fp, "Thread %d:\n", i);
        fprintf(db->output_fp, "  Buffer Insert: %.6f\n", buffer_insert);
        fprintf(db->output_fp, "  Index Insert: %.6f\n", index_insert);
        fprintf(db->output_fp, "  Flush: %.6f\n", flush);
        fprintf(db->output_fp, "  Flush Callback: %.6f\n", flush_cb);
        fprintf(db->output_fp, "  GC: %.6f\n", gc);
        fprintf(db->output_fp, "  GC Callback: %.6f\n", gc_cb);
        fprintf(db->output_fp, "  Split: %.6f\n", split);
        fprintf(db->output_fp, "  Split Callback: %.6f\n", split_cb);
        fprintf(db->output_fp, "  Thread Total: %.6f\n\n",
            buffer_insert + index_insert + flush + flush_cb + gc + gc_cb + split + split_cb);

        // 累加总时间
        total_buffer_insert_time += buffer_insert;
        total_index_insert_time += index_insert;
        total_flush_time += flush;
        total_flush_cb_time += flush_cb;
        total_gc_time += gc;
        total_gc_cb_time += gc_cb;
        total_split_time += split;
        total_split_cb_time += split_cb;
    }

    // 输出所有线程的总计
    fprintf(db->output_fp, "All Threads Total (seconds):\n");
    fprintf(db->output_fp, "  Buffer Insert: %.6f\n", total_buffer_insert_time);
    fprintf(db->output_fp, "  Index Insert: %.6f\n", total_index_insert_time);
    fprintf(db->output_fp, "  Flush: %.6f\n", total_flush_time);
    fprintf(db->output_fp, "  Flush Callback: %.6f\n", total_flush_cb_time);
    fprintf(db->output_fp, "  GC: %.6f\n", total_gc_time);
    fprintf(db->output_fp, "  GC Callback: %.6f\n", total_gc_cb_time);
    fprintf(db->output_fp, "  Split: %.6f\n", total_split_time);
    fprintf(db->output_fp, "  Split Callback: %.6f\n", total_split_cb_time);
    fprintf(db->output_fp, "  Total: %.6f\n",
        total_buffer_insert_time + total_index_insert_time +
        total_flush_time + total_flush_cb_time +
        total_gc_time + total_gc_cb_time +
        total_split_time + total_split_cb_time);

    // 重置breakdown计数器
    fflush(db->output_fp);
    memset(insert_breakdown, 0, sizeof(struct insert_time_breakdown) * db->thread);
}

void read_breakdown_output(struct db * db){
    double time;
    uint64_t ticks_hz = spdk_get_ticks_hz();
    double total_dram_index_time = 0;
    double total_write_buffer_index_time = 0;
    double total_search_cache_time = 0;
    double total_nvm_index_time = 0;
    double total_spdk_read_time = 0;
    double total_read_cb_time = 0;

    fprintf(db->output_fp, "\nRead Breakdown Per Thread (seconds):\n");
    // 输出每个线程的breakdown
    for(int i = 0; i < db->thread; i++){
        double dram_index = (double)read_breakdown[i].dram_index_time / ticks_hz;
        double write_buffer_index = (double)read_breakdown[i].write_buffer_index_time / ticks_hz;
        double search_cache = (double)read_breakdown[i].search_cache_time / ticks_hz;
        double nvm_index = (double)read_breakdown[i].nvm_index_time / ticks_hz;
        double spdk_read = (double)read_breakdown[i].spdk_read_time / ticks_hz;
        double read_cb = (double)read_breakdown[i].read_cb_time / ticks_hz;

        fprintf(db->output_fp, "Thread %d:\n", i);
        fprintf(db->output_fp, "  DRAM Index: %.6f\n", dram_index);
        fprintf(db->output_fp, "  Write Buffer Index: %.6f\n", write_buffer_index);
        fprintf(db->output_fp, "  Search Cache: %.6f\n", search_cache);
        fprintf(db->output_fp, "  NVM Index: %.6f\n", nvm_index);
        fprintf(db->output_fp, "  SPDK Read: %.6f\n", spdk_read);
        fprintf(db->output_fp, "  Read Callback: %.6f\n", read_cb);
        fprintf(db->output_fp, "  Thread Total: %.6f\n\n",
            dram_index + write_buffer_index + search_cache + nvm_index + spdk_read + read_cb);

        // 累加总时间
        total_dram_index_time += dram_index;
        total_write_buffer_index_time += write_buffer_index;
        total_search_cache_time += search_cache;
        total_nvm_index_time += nvm_index;
        total_spdk_read_time += spdk_read;
        total_read_cb_time += read_cb;
    }

    // 输出所有线程的总计
    fprintf(db->output_fp, "All Threads Total (seconds):\n");
    fprintf(db->output_fp, "  DRAM Index: %.6f\n", total_dram_index_time);
    fprintf(db->output_fp, "  Write Buffer Index: %.6f\n", total_write_buffer_index_time);
    fprintf(db->output_fp, "  Search Cache: %.6f\n", total_search_cache_time);
    fprintf(db->output_fp, "  NVM Index: %.6f\n", total_nvm_index_time);
    fprintf(db->output_fp, "  SPDK Read: %.6f\n", total_spdk_read_time);
    fprintf(db->output_fp, "  Read Callback: %.6f\n", total_read_cb_time);
    fprintf(db->output_fp, "  Total: %.6f\n",
        total_dram_index_time + total_write_buffer_index_time +
        total_search_cache_time + total_nvm_index_time +
        total_spdk_read_time + total_read_cb_time);

    // 重置breakdown计数器
    fflush(db->output_fp);
    memset(read_breakdown, 0, sizeof(struct read_time_breakdown) * db->thread);
}



void print_g_map_nonzero_count(void)
{
    size_t count = 0;
    int block_num = g_block_num;

    if (g_map == NULL) {
        printf("g_map is NULL, nothing to count\n");
        return;
    }

    for (size_t i = 0; i < block_num; ++i) {
        if (g_map[i] != 0) {
            ++count;
        }
    }
    printf("Number of non-zero entries in g_map: %zu SSD spaces: %lu GB\n", count , count * BLOCK_SIZE / 1024 / 1024 /1024);
}


void client_output_bw(struct db * db){
    uint64_t total_block = 0;
    uint64_t total_gc_block = 0;
    uint64_t total_gc_write_block = 0;
    uint64_t total_split_block = 0;
    uint64_t total_split_block_write = 0;
    uint32_t total_subspace = 0;
    uint64_t total_subspace_dram_cost;
    uint64_t total_cache_hit = 0;
    uint64_t total_buffer_hit = 0;
    uint64_t total_scan_key = 0;
    uint64_t total_scan_key_on_block = 0;
    uint64_t total_scan_block = 0;
    uint64_t total_flush_block = 0;
    uint64_t total_invalid_block = 0;

    for(int i = 0 ; i < db->thread ; i++){
        fprintf(db->output_fp , "thread %d : consume %luMB\n" , i , thread_block_rw[i] * BLOCK_SIZE / 1000000);
        total_block += thread_block_rw[i];
        total_subspace += thread_subspaces[i];

        total_cache_hit += thread_kvcache_hit[i];
        total_buffer_hit += thread_kvbuffer_hit[i];
        total_scan_key += thread_scan_total[i];
        total_scan_key_on_block += thread_scan_on_block[i];
        total_scan_block += thread_scan_block[i];
        total_gc_block += thread_gc_block_rw[i];
        total_gc_write_block += thread_gc_block_write[i];
        total_split_block += thread_split_block_rw[i];
        total_split_block_write += thread_split_block_write[i];
        total_flush_block += thread_flush_block_write[i];
        total_invalid_block += invalid_block_num[i];
    }

    print_g_map_nonzero_count();

    ssd_block_space_consumed += (total_flush_block + total_gc_write_block + total_split_block_write - (total_gc_block - total_gc_write_block) -
     (total_split_block - total_split_block_write));
    // ssd_block_space_consumed += ((total_block - total_gc_block - total_split_block) + total_gc_write_block + total_split_block_write - );

    fprintf(db->output_fp ,"total bandwidth consumed : block %lu consume %luMB\n" , total_block ,  total_block * BLOCK_SIZE /  1024 / 1024);

    fprintf(db->output_fp , "GC total consumed: block %lu consume %luMB\n" , total_gc_block , total_gc_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "GC write consumed: block %lu consume %luMB\n" , total_gc_write_block , total_gc_write_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "Split total consumed: block %lu consume %luMB\n" , total_split_block, total_split_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "Split write consumed: block %lu consume %luMB\n" , total_split_block_write, total_split_block_write * BLOCK_SIZE / 1024 / 1024);

    total_subspace_dram_cost = sizeof(struct subspace_info) * total_subspace;
    fprintf(db->output_fp ,"total : Subspaces %u  consume DRAM %luMB\n" ,  total_subspace , total_subspace_dram_cost /  1024 / 1024);

    fprintf(db->output_fp, "total : WBF %lluGB  LNF %lluGB\n",
            (unsigned long long)(WBF_SIZE / (1024 * 1024 * 1024) * db->write_buffer->file_num),
            (unsigned long long)(LNF_SIZE / (1024 * 1024 * 1024) * db->index->nvm_index->file_num));

    fprintf(db->output_fp ,"total consumed SSD spaces : %dGB\n", db->fs->head->segment_num * alloc_grain * BLOCK_SIZE / 1024 / 1024 / 1024);
    fprintf(db->output_fp ,"total consumed SSD spaces : %luGB total_flush_block is %lu\n", ssd_block_space_consumed * BLOCK_SIZE / 1024 / 1024 / 1024 , total_flush_block);
    fprintf(db->output_fp ,"total invalid SSD spaces : %luGB\n", total_invalid_block * BLOCK_SIZE / 1024 / 1024 / 1024);

    fprintf(db->output_fp , "total : kvcache hit %lu, write buffer hit %lu\n", total_cache_hit , total_buffer_hit);

    fprintf(db->output_fp , "scan : scan %lu keys on block %lu read %lu block\n", total_scan_key, total_scan_key_on_block
    , total_scan_block);

    fprintf(db->output_fp , "WA is %lu / %lu = %f\n", (total_gc_write_block + total_split_block_write + total_flush_block) * BLOCK_SIZE / 1024 / 1024
    , (uint64_t)50000000UL * 1024UL / 1024UL / 1024UL , (double)((total_gc_write_block + total_split_block_write + total_flush_block) * BLOCK_SIZE) / (uint64_t)50000000 * 1024);


    memset(thread_scan_block , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_block_rw ,0 , sizeof(uint64_t) * db->thread);
    memset(thread_flush_block_write ,0 , sizeof(uint64_t) * db->thread);
    memset(thread_split_block_rw , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_split_block_write , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_kvcache_hit , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_kvbuffer_hit , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_scan_total , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_scan_on_block , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_gc_block_rw , 0 , sizeof(uint64_t) * db->thread);
    memset(thread_gc_block_write , 0 , sizeof(uint64_t) * db->thread);

    // insert_breakdown_output(db);
    // read_breakdown_output(db);

    return ;
}



// void db_request_filling(struct db *db, int request_size, struct user_rq *request) {
//     int i, thread;
//     struct request req;
//     int * filling_num = (int *)malloc(sizeof(int) * db->thread);
//     memset(filling_num , 0 , sizeof(int) * db->thread);
//     // struct request * request_p = db->ring[thread]->ring;

//     // 添加变量来记录各类请求的数量
//     int read_count = 0;
//     int write_count = 0;
//     int scan_count = 0;
//     struct scan_kv * scan_result;

//     for (i = 0; i < request_size; i++) {
//         req.key = request[i].key;
//         req.type = request[i].type;
//         if(req.type == 0){//read
//             read_count++;
//         }
//         else if(req.type == 1){//write
//             write_count++;
//             req.size = request[i].size;
//             req.value = (char *)malloc(req.size);
//             if (req.value == NULL) {
//                 printf("Memory allocation failed for key %lu\n", req.key);
//                 benchmark_completed = 1;
//                 exit(EXIT_FAILURE);
//             }
//             client_key_to_value(req.key, req.value, req.size);
//         }
//         else if(req.type == 2){//scan

//             // pthread_mutex_lock(&scan_mutex);
//             if (pending_scan_count < MAX_PENDING_SCANS) {
//                 pending_scans[pending_scan_count].key = req.key;
//                 pending_scans[pending_scan_count].scan_length = request[i].size;
//                 pending_scans[pending_scan_count].submit_time = spdk_get_ticks();
//                 pending_scans[pending_scan_count].completed = false;
//                 pending_scans[pending_scan_count].thread_id = key_to_thread(req.key, db);
//                 pending_scan_count++;
//             }
//             // pthread_mutex_unlock(&scan_mutex);


//             scan_count++;
//             req.value = (char *)malloc(sizeof(struct scan_kv));
//             req.size = request[i].size;
//             if (req.value == NULL) {
//                 printf("Memory allocation failed for key %lu\n", req.key);
//                 benchmark_completed = 1;
//                 exit(EXIT_FAILURE);
//             }
//             scan_result = (struct scan_kv *)req.value;
//             scan_result->key = (key_type *)malloc(sizeof(key_type) * request[i].size);
//             scan_result->value = (char **)malloc(sizeof(char *) * request[i].size);
//             scan_result->value_size = (int *)malloc(sizeof(int) *request[i].size);

//             if(scan_result->value == NULL || scan_result->key == NULL || scan_result->value_size == NULL){
//                 printf("Memory allocation failed for key %lu\n", req.key);
//                 benchmark_completed = 1;
//                 exit(EXIT_FAILURE);
//             }
//         }

//         thread = key_to_thread(req.key, db);
//         // printf("thread is %d , req key value size is %lu %s %d\n" ,thread ,  req.key , req.value , req.size);
//         // if(req.key == 6063691547004636268){
//         //     printf("key: %lu thread %d\n" , req.key , thread);
//         // }
//         if (thread < 0 || thread >= db->thread) {
//             printf("Invalid thread %d for key %lu\n", thread, req.key);
//             benchmark_completed = 1;
//             exit(EXIT_FAILURE);
//         }
//         db->ring[thread]->ring[(db->ring[thread]->server_ring_tail + filling_num[thread]) % db->server_ring_size ] = req;


//         // memcpy(&db->ring[thread]->ring[db->ring[thread]->server_ring_head + i] , &req , sizeof(struct request));
//         // printf("thread is %d\n" , thread);
//         // printf("submit key %d, value %s ,db->server_ring_size%d\n", req.key, req.value ,db->server_ring_size);

//         thread_submitted[thread] ++;
//         filling_num[thread] ++;
//         // printf("submit thread %d key %d completed is %d\n" , thread , req.key , completed);
//         // printf("requst_p->type is %d\n" , db->ring[thread]->ring[i].type);

//     }

//     // 在提交请求之前输出各类请求的统计信息
//     printf("请求统计: 总计 %d 请求 (读取: %d, 写入: %d, 扫描: %d)\n",
//            request_size, read_count, write_count, scan_count);

//     client_submit_batched_request(db , filling_num);
//     free(filling_num);
//     return;
// }

//

int test_get_num = 0;
int test_insert_num = 0;
int test_scan_num = 0;
int test_update_num = 0;

void load_injector_pooling_completion(struct result ** result , int result_num , int thread){
    struct scan_kv * scan_result;
    char value[BLOCK_SIZE];

    for(int i = 0 ; i < result_num ; i++){
        histogram_update(thread, result[i]->end - result[i]->start);
        if (result[i]->type == 0) {//read
            client_key_to_value(result[i]->key , value , result[i]->size);
            if(strcmp(value , result[i]->value) != 0){
                printf("result error:key %lu\nresult.value %s\n" , result[i]->key , result[i]->value);
                return ;
            }
            free(result[i]->value);

        }
        else if(result[i]->type == 1){//write
            free(result[i]->value);
        }
        else if(result[i]->type == 2){//scan
            scan_result = (struct scan_kv *)result[i]->value;
            // printf("result[%d]->size is %d scan_result keu num is %d\n" , i , result[i]->size, scan_result->key_num);
            for(int j = 0 ; j < result[i]->size ; j++){
                if(scan_result->value[j] == NULL){
                    printf("scan_result->value %d is NULL\n" , j);
                    return ;
                }
                // printf("scan_result %d key is %lu value is %s\n" ,j ,scan_result->key[j],  scan_result->value[j]);
                free(scan_result->value[j]);
                // printf("free done\n");
            }
            // printf("start to free scan_result->value_size %p\n" , scan_result->value_size );
            free(scan_result->value_size);
            // printf("start to free scan_result->value %p\n" , scan_result->value );
            free(scan_result->value);
            // printf("start to free scan_result->key %p\n" , scan_result->key );
            free(scan_result->key);
            // printf("start to free scan_result %p\n" , scan_result );
            free(scan_result);
        }
        free(result[i]);
    }
}

void client_polling_completed(struct db * db , int polling_num){
    // 添加时间跟踪变量
    uint64_t ticks_hz = spdk_get_ticks_hz();
    uint64_t last_report_time = spdk_get_ticks();
    uint64_t current_time;
    uint64_t five_seconds_ticks = 5 * ticks_hz; // 5秒对应的tick数
    uint64_t completed;

    while (!test_completed) {
        completed = 0;
        for(int i=0 ; i < db->load_injectors ; i++){
            completed += load_injector_completed[i];
        }
        if(completed >= polling_num){
            test_completed = 1;
            break;
        }
        // 检查是否已经过了5秒
        current_time = spdk_get_ticks();
        if (current_time - last_report_time > five_seconds_ticks) {
            // completed = 0;
            // for(int i = 0 ; i < db->thread ; i++){
            //     completed += thread_completed[i];
            // }
            printf("已完成 %lu 个请求 \n", completed);
            fflush(stdout);
            // for(int i = 0 ; i < db->thread ; i++){
            //     printf("thread %d scan key is %lu\n" , i , thread_scan_key[i]);
            // }
            // printf("已完成 %d 个请求，总共 %d 个请求（读取: %d, 写入: %d, 扫描: %d)\n",
            //       completed, request_size, read_completed, write_completed, scan_completed);
            last_report_time = current_time;
            // last_report_time = current_time;
        }
    }
}

void load_executor(void * arg){
    struct load_injectors * load_injector = (struct load_injectors *)arg;
    struct user_rq * test = (struct user_rq *)load_injector->load_array;
    struct db * db = load_injector->db;
    int request = load_injector->thread;
    int thread;

    struct request * req;
    struct scan_kv * scan_result;

    struct result * result[10];
    int result_num;

    int read_count = 0;
    int write_count = 0;
    int scan_count = 0;


    pthread_barrier_wait(&barrier);
    printf("load_injector %d start to work\n" , load_injector->thread);
    fflush(stdout);
    // sleep(100);
    while((request < load_injector->load_num) || !test_completed){
        if(request < load_injector->load_num){
            req = (struct request *)malloc(sizeof(struct request));
            req->key = test[request].key;
            req->type = test[request].type;
            req->size = test[request].size;
            if(req->type == 0){
                // req->break_down = (void *)malloc(sizeof(struct read_time_breakdown));
                // memset(req->break_down , 0 , sizeof(struct read_time_breakdown));
                read_count ++;
            }
            else if(req->type == 1){
                write_count ++;
                req->value = (char *)malloc(req->size);
                req->break_down = (void *)malloc(sizeof(struct insert_time_breakdown));
                memset(req->break_down , 0 , sizeof(struct insert_time_breakdown));
                if (req->value == NULL) {
                    printf("Memory allocation failed for key %lu\n", req->key);
                    benchmark_completed = 1;
                    exit(EXIT_FAILURE);
                }
                client_key_to_value(req->key, req->value, req->size);
            }
            else if(req->type == 2){
                scan_count ++;
                req->value = (char *)malloc(sizeof(struct scan_kv));
                if (req->value == NULL) {
                    printf("Memory allocation failed for key %lu\n", req->key);
                    benchmark_completed = 1;
                    exit(EXIT_FAILURE);
                }
                scan_result = (struct scan_kv *)req->value;
                scan_result->key = (key_type *)malloc(sizeof(key_type) * req->size);
                scan_result->value = (char **)malloc(sizeof(char *) * req->size);
                scan_result->value_size = (int *)malloc(sizeof(int) *req->size);

                if(scan_result->value == NULL || scan_result->key == NULL || scan_result->value_size == NULL){
                    printf("Memory allocation failed for key %lu\n", req->key);
                    benchmark_completed = 1;
                    exit(EXIT_FAILURE);
                }
            }

            thread = key_to_thread(req->key, db);
            if (thread < 0 || thread >= db->thread) {
                printf("Invalid thread %d for key %lu\n", thread, req->key);
                benchmark_completed = 1;
                exit(EXIT_FAILURE);
            }
            while(!spdk_ring_enqueue(db->spdk_ring[thread] , (void **)&req , 1 , NULL)){
                result_num = spdk_ring_dequeue(load_injector->spdk_result_ring, (void **)result , 10);
                load_injector_pooling_completion(result , result_num , load_injector->thread);
                load_injector_completed[load_injector->thread] += result_num;
            }
            request += load_injector->nb_load_injectors;
        }
        result_num = spdk_ring_dequeue(load_injector->spdk_result_ring, (void **)result , 10);
        load_injector_pooling_completion(result , result_num , load_injector->thread);
        // printf("result num is %d\n" , result_num);
        load_injector_completed[load_injector->thread] += result_num;
    }

    pthread_barrier_wait(&barrier_complete);
    printf("load_injector[%d] completed %lu\n" ,load_injector->thread, load_injector_completed[load_injector->thread]);
    // spdk_app_stop(0);
    fflush(stdout);
}

void client_initialize_load_injectors(struct db * db , struct user_rq * load, int load_num){
    uint64_t ticks = spdk_get_ticks_hz() ;
    uint64_t start ;
    uint64_t end ;
    double time;

    struct load_injectors * load_injectors = (struct load_injectors *)malloc(sizeof(struct load_injectors) * db->load_injectors);

    pthread_barrier_init(&barrier, NULL, db->load_injectors + 1);
    pthread_barrier_init(&barrier_complete, NULL, db->load_injectors );
    test_completed = 0;
    for(int i = 0 ; i < db->load_injectors ; i++){
        load_injectors[i].db = db;
        load_injectors[i].spdk_result_ring = db->spdk_result_ring[i];
        load_injectors[i].spdk_ring = db->spdk_ring;
        load_injectors[i].thread = i;
        load_injectors[i].core = i + db->thread + 1;
        // load_injectors[i].core = ((i + db->thread + 1) * 4 + 0) % 80 ;
        load_injectors[i].load_array = (struct user_req * )load;
        load_injectors[i].load_num = load_num;
        load_injectors[i].nb_load_injectors = db->load_injectors;
        printf("start load thread %d core %d(numa node %d)\n" , i , load_injectors[i].core,spdk_env_get_socket_id(load_injectors[i].core));
        fflush(stdout);
        if(spdk_env_thread_launch_pinned(load_injectors[i].core , (thread_start_fn)load_executor , &load_injectors[i])){
            printf("failed to start load thread %d\n" , i);
			return;
        }
        else{
            printf("start load thread %d done\n" , i);
        }
        fflush(stdout);
    }

    pthread_barrier_wait(&barrier);
    start = spdk_get_ticks();
    // printf("client start to client_polling_completed\n");
    // fflush(stdout);
    client_polling_completed(db , load_num);
    end = spdk_get_ticks();
    printf("load done\n");
    time = (double)(end - start) / ticks;
    printf("%d ops , time is %lf , throughput is %lf\n" , load_num , time , load_num / time);
    fflush(stdout);
    client_output_bw(db);
    if(db->output_fp){
        fprintf(db->output_fp, "test:total %d ops Insert %d Update %d Get %d Scan %d time %lfs\n" ,
        load_num , test_insert_num , test_update_num , test_get_num , test_scan_num, time);
    }
    histogram_compact(db->load_injectors);
    histogram_reset_all(db->load_injectors);
    fflush(db->output_fp);
    memset(load_injector_completed , 0 , sizeof(uint64_t) * db->load_injectors);
    free(load_injectors);
}


void client_executor(struct db * db){


    struct user_rq * warm = NULL;
    int warm_num = 0;
    struct user_rq * test;
    int test_num;
    int cache_key_num = 0;

    /*读取文件warm.txt和test.txt中的数据*/

    int is_production = 0;
    if (strstr(g_libdb_config.test_path, "Production") != NULL) {
        is_production = 1;
    }

    if (is_production) {
        warm = client_read_production_request(g_libdb_config.warm_path , &warm_num);
        cache_key_num = warm_num * 1 / 10;
    } else {
        warm = client_read_request(g_libdb_config.warm_path , &warm_num , db->size_value);
        cache_key_num = warm_num * 5 / 10;
    }
    if (warm == NULL || warm_num == 0) {
        fprintf(stderr, "failed to load warm workload: %s\n", g_libdb_config.warm_path);
        benchmark_completed = 1;
        return;
    }

    /* recovery：数据已在 NVM，跳过整批 load；仍读 warm 并做 cache 预热（末尾改读） */
    if (!db->recovery_mode) {
        printf("start to load\n");
        fflush(stdout);
        client_initialize_load_injectors(db ,warm, warm_num);
        sleep(10);
    } else {
        printf("recovery mode: skip load, keep cache warmup\n");
        fflush(stdout);
    }

    /*预热cache*/
    for(int i = warm_num - cache_key_num ; i < warm_num ; i++){
        warm[i].type = 0;
    }
    if (cache_key_num > 0) {
        client_initialize_load_injectors(db ,&warm[warm_num - cache_key_num], cache_key_num);
    }

    sleep(10);
    test_get_num = 0;
    test_insert_num = 0;
    test_update_num = 0;
    test_scan_num = 0;
    if (is_production) {
        test = client_read_production_request(g_libdb_config.test_path , &test_num);
    } else {
        test = client_read_request(g_libdb_config.test_path , &test_num , db->size_value);
    }
    if (test == NULL || test_num == 0) {
        fprintf(stderr, "failed to load test workload: %s\n", g_libdb_config.test_path);
    } else {
        client_initialize_load_injectors(db ,test, test_num);
        free(test);
    }
    fflush(stdout);
    fflush(db->output_fp);


    // test = client_read_request(test_path , &test_num , db->size_value);

    // pthread_barrier_wait(&barrier);
    // // sleep(100);
    // start = spdk_get_ticks();
    // printf("client start to client_polling_completed\n");
    // client_polling_completed(db , test_num);
    // end = spdk_get_ticks();
    // printf("load done\n");
    // time = (double)(end - start) / ticks;
    // client_output_bw(db);
    benchmark_completed = 1;

    EXIT_SUCCESS ;
}



struct user_rq * client_read_request(char * file  , int * num , int kvsize){
    FILE * fp;
    char s[100];
    key_type user;
    int array_size = 100000;
    int add_size = 100000;
    int i = 0;
    int length;
    struct user_rq * request = (struct user_rq *)malloc(sizeof(struct user_rq) * array_size);

    printf("start to read file %s\n" , file);
    if((fp = fopen(file , "r")) == NULL){
        printf("ERROR:fail to open %s\n",file);
        return NULL;
    }
    else{
        printf("read file %s done\n" , file);
    }

    while(!feof(fp)){

        if (fscanf(fp , "%s user%lu " , s , &user) != 2) {
            break;
        }

        // printf("s is %s user is %d\n" , s , user);
        request[i].key = user;
        request[i].size = kvsize;
        if(strcmp(s , "I") == 0 || strcmp(s , "U") == 0){
            if(strcmp(s , "I") == 0){
                test_insert_num ++;
            }
            else{
                test_update_num ++;
            }

            request[i].type = 1;
            if (fscanf(fp , "%s" , s) != 1) {
                break;
            }
        }
        else if(strcmp(s , "R") == 0){
            request[i].type = 0;
            test_get_num++;
        }
        else if(strcmp(s , "S") == 0){
            if (fscanf(fp , "%d\n" , &length) != 1) {
                break;
            }
            request[i].type = 2;
            test_scan_num ++;
            request[i].size = length;
        }
        else {
        }
        // printf("request type is %d , key is %d\n" , request[i].type , request[i].key);
        i++;
        /**/
        if( i >= array_size){
            array_size += add_size;
            add_size = array_size;
            request = (struct user_rq *)realloc(request , sizeof(struct user_rq) * array_size);
        }
    }
    *num = i / 1000 * 1000;
    printf("read file %s %d request done\n" , file , *num);
    return request;
}


void client_key_to_value(key_type key, char *value, int size) {
    if (value == NULL || size <= 0) {
        return;
    }
    // 使用snprintf安全格式化，确保不超过缓冲区大小
    snprintf(value, (size_t)size, "ID%" PRIu64, key);
}

void client_key_to_value_update(key_type key , char * value , int size){
    int random = rand();
    if (value == NULL || size <= 0) {
        return; // 无效参数直接返回
    }
    // 格式化字符串：ID + key字符串 + 随机数字符串
    snprintf(value, (size_t)size, "ID%" PRIu64 "-%d", key, random);
}

struct user_rq * client_read_production_request(char * file  , int * num){
    FILE * fp;
    char s[100];
    key_type user;
    int array_size = 100000;
    int add_size = 100000;
    int i = 0;
    int length;
    struct user_rq * request = (struct user_rq *)malloc(sizeof(struct user_rq) * array_size);

    printf("start to read file %s\n" , file);
    if((fp = fopen(file , "r")) == NULL){
        printf("ERROR:fail to open %s\n",file);
        return NULL;
    }
    else{
        printf("read file %s done\n" , file);
    }

    while(!feof(fp)){

        if (fscanf(fp , "%s user%lu " , s , &user) != 2) {
            break;
        }

        request[i].key = (user == 0) ? 1 : user ;

        if(strcmp(s , "I") == 0 || strcmp(s , "U") == 0){
            if(strcmp(s , "I") == 0){
                test_insert_num ++;
            }
            else{
                test_update_num ++;
            }

            request[i].type = 1;
            /* 跳过中间一个字段并读取最后的长度，例如：
             * U user363252581 363252581 368
             * I user333532767 333532767 368
             * 最后的数字368表示value长度
             */
            if (fscanf(fp , "%*s %d" , &length) != 1) {
                printf("ERROR: read length for I/U request failed\n");
                break;
            }
            request[i].size = length;
        }
        else if(strcmp(s , "R") == 0){
            request[i].type = 0;
            test_get_num++;
        }
        else if(strcmp(s , "S") == 0){
            if (fscanf(fp , "%d\n" , &length) != 1) {
                printf("ERROR: read length for S request failed\n");
                break;
            }
            request[i].type = 2;
            test_scan_num ++;
            request[i].size = length;
        }
        else {
            /* 未知操作类型，尝试跳过本行 */
            printf("WARN: unknown operation type %s\n", s);
            if (fgets(s, sizeof(s), fp) == NULL) {
                break;
            }
            continue;
        }

        i++;
        if( i >= array_size){
            array_size += add_size;
            add_size = array_size;
            request = (struct user_rq *)realloc(request , sizeof(struct user_rq) * array_size);
        }
    }

    fclose(fp);
    *num = i / 1000 * 1000;
    printf("read file %s %d request done\n" , file , *num);
    return request;
}
