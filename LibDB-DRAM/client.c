#include "./client.h"
#include "kv_record.h"
#include "./base.h"
#include "index.h"
#include "scan.h"
#include "subspace.h"
#include <spdk/env.h>
#include <spdk/event.h>
#include <stdint.h>
#include "pthread.h"
#include <rte_launch.h>
#include "hist.h"

pthread_barrier_t barrier;
pthread_barrier_t barrier_complete;

float complete_threshold = 1;
struct pending_scan_request pending_scans[MAX_PENDING_SCANS];
int pending_scan_count = 0;
pthread_mutex_t scan_mutex = PTHREAD_MUTEX_INITIALIZER;
int benchmark_start = 0;
atomic_int test_completed;

uint64_t ssd_block_space_consumed = 0;


void insert_breakdown_output(struct db * db){
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


        total_buffer_insert_time += buffer_insert;
        total_index_insert_time += index_insert;
        total_flush_time += flush;
        total_flush_cb_time += flush_cb;
        total_gc_time += gc;
        total_gc_cb_time += gc_cb;
        total_split_time += split;
        total_split_cb_time += split_cb;
    }


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


    fflush(db->output_fp);
    memset(insert_breakdown, 0, sizeof(struct insert_time_breakdown) * db->thread);
}

void read_breakdown_output(struct db * db){
    double time;
    uint64_t ticks_hz = spdk_get_ticks_hz();
    double total_dram_index_time = 0;
    double total_write_buffer_index_time = 0;
    double total_search_cache_time = 0;
    double total_slot_index_time = 0;
    double total_spdk_read_time = 0;
    double total_read_cb_time = 0;

    fprintf(db->output_fp, "\nRead Breakdown Per Thread (seconds):\n");

    for(int i = 0; i < db->thread; i++){
        double dram_index = (double)read_breakdown[i].dram_index_time / ticks_hz;
        double write_buffer_index = (double)read_breakdown[i].write_buffer_index_time / ticks_hz;
        double search_cache = (double)read_breakdown[i].search_cache_time / ticks_hz;
        double slot_index = (double)read_breakdown[i].slot_index_time / ticks_hz;
        double spdk_read = (double)read_breakdown[i].spdk_read_time / ticks_hz;
        double read_cb = (double)read_breakdown[i].read_cb_time / ticks_hz;

        fprintf(db->output_fp, "Thread %d:\n", i);
        fprintf(db->output_fp, "  DRAM Index: %.6f\n", dram_index);
        fprintf(db->output_fp, "  Write Buffer Index: %.6f\n", write_buffer_index);
        fprintf(db->output_fp, "  Search Cache: %.6f\n", search_cache);
        fprintf(db->output_fp, "  DRAM Slot Index: %.6f\n", slot_index);
        fprintf(db->output_fp, "  SPDK Read: %.6f\n", spdk_read);
        fprintf(db->output_fp, "  Read Callback: %.6f\n", read_cb);
        fprintf(db->output_fp, "  Thread Total: %.6f\n\n",
            dram_index + write_buffer_index + search_cache + slot_index + spdk_read + read_cb);


        total_dram_index_time += dram_index;
        total_write_buffer_index_time += write_buffer_index;
        total_search_cache_time += search_cache;
        total_slot_index_time += slot_index;
        total_spdk_read_time += spdk_read;
        total_read_cb_time += read_cb;
    }


    fprintf(db->output_fp, "All Threads Total (seconds):\n");
    fprintf(db->output_fp, "  DRAM Index: %.6f\n", total_dram_index_time);
    fprintf(db->output_fp, "  Write Buffer Index: %.6f\n", total_write_buffer_index_time);
    fprintf(db->output_fp, "  Search Cache: %.6f\n", total_search_cache_time);
    fprintf(db->output_fp, "  DRAM Slot Index: %.6f\n", total_slot_index_time);
    fprintf(db->output_fp, "  SPDK Read: %.6f\n", total_spdk_read_time);
    fprintf(db->output_fp, "  Read Callback: %.6f\n", total_read_cb_time);
    fprintf(db->output_fp, "  Total: %.6f\n",
        total_dram_index_time + total_write_buffer_index_time +
        total_search_cache_time + total_slot_index_time +
        total_spdk_read_time + total_read_cb_time);


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
    uint64_t merges = 0, merge_reads = 0, merge_writes = 0;
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
        merges += db->workers[i].merge_count;
        merge_reads += db->workers[i].merge_read_blocks;
        merge_writes += db->workers[i].merge_write_blocks;
        db->workers[i].merge_count = 0;
        db->workers[i].merge_read_blocks = db->workers[i].merge_write_blocks = 0;
    }

    print_g_map_nonzero_count();

    ssd_block_space_consumed += (total_flush_block + total_gc_write_block + total_split_block_write - (total_gc_block - total_gc_write_block) -
     (total_split_block - total_split_block_write) + merge_writes - merge_reads);

    fprintf(db->output_fp ,"total bandwidth consumed : block %lu consume %luMB\n" , total_block ,  total_block * BLOCK_SIZE /  1024 / 1024);

    fprintf(db->output_fp , "GC total consumed: block %lu consume %luMB\n" , total_gc_block , total_gc_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "GC write consumed: block %lu consume %luMB\n" , total_gc_write_block , total_gc_write_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "Split total consumed: block %lu consume %luMB\n" , total_split_block, total_split_block * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp , "Split write consumed: block %lu consume %luMB\n" , total_split_block_write, total_split_block_write * BLOCK_SIZE / 1024 / 1024);

    fprintf(db->output_fp, "Merge completed: %lu; read blocks: %lu; write blocks: %lu\n", merges, merge_reads, merge_writes);


    pthread_mutex_lock(&g_mutex_lock);
    uint64_t assigned_blocks = db->fs->head ?
        (uint64_t)db->fs->head->segment_num * alloc_grain : db->data_blocks;
    pthread_mutex_unlock(&g_mutex_lock);
    fprintf(db->output_fp, "total consumed SSD spaces : %luGB\n",
            assigned_blocks * BLOCK_SIZE / 1024 / 1024 / 1024);
    fprintf(db->output_fp ,"total consumed SSD spaces : %luGB total_flush_block is %lu\n", ssd_block_space_consumed * BLOCK_SIZE / 1024 / 1024 / 1024 , total_flush_block);
    fprintf(db->output_fp ,"total invalid SSD spaces : %luGB\n", total_invalid_block * BLOCK_SIZE / 1024 / 1024 / 1024);

    fprintf(db->output_fp , "total : kvcache hit %lu, write buffer hit %lu\n", total_cache_hit , total_buffer_hit);

    fprintf(db->output_fp , "scan : scan %lu keys on block %lu read %lu block\n", total_scan_key, total_scan_key_on_block
    , total_scan_block);


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


    return ;
}




int test_get_num = 0;
int test_insert_num = 0;
int test_scan_num = 0;
int test_update_num = 0;


struct client_result_counts {
    uint64_t completed, get, put, scan, missing, rejected, mismatched, scan_invalid;
};
static struct client_result_counts phase_results[MAX_THREAD];
static bool phase_results_enabled;

static void client_results_finish(struct db *db) {
    struct client_result_counts total = {0};
    for(int i = 0; i < db->load_injectors; i++) {
#define SUM_RESULT(field) total.field += phase_results[i].field
        SUM_RESULT(completed); SUM_RESULT(get); SUM_RESULT(put); SUM_RESULT(scan);
        SUM_RESULT(missing); SUM_RESULT(rejected); SUM_RESULT(mismatched); SUM_RESULT(scan_invalid);
#undef SUM_RESULT
    }
    FILE *streams[] = {stdout, db->output_fp};
    for(int i = 0; i < 2; i++) {
        FILE *fp = streams[i];
        if(!fp || (i && fp == stdout)) continue;
        fprintf(fp, "PHASE_RESULTS version=1 phase=%s completed=%" PRIu64
                " get=%" PRIu64 " put=%" PRIu64 " scan=%" PRIu64
                " missing=%" PRIu64 " rejected=%" PRIu64 " mismatched=%" PRIu64
                " scan_invalid=%" PRIu64 "\n", db->phase_name ? db->phase_name : "test",
                total.completed, total.get, total.put, total.scan,
                total.missing, total.rejected, total.mismatched, total.scan_invalid);
    }
}

void load_injector_pooling_completion(struct result ** result , int result_num , int thread){
    struct scan_kv * scan_result;
    char value[BLOCK_SIZE];

    for(int i = 0 ; i < result_num ; i++){
        histogram_update(thread, result[i]->end - result[i]->start);
        struct client_result_counts *counts = phase_results_enabled ? &phase_results[thread] : NULL;
        if(counts) {
            counts->completed++;
            counts->get += result[i]->type == 0;
            counts->put += result[i]->type == 1;
            counts->scan += result[i]->type == 2;
        }
        if(result[i]->result != 0) {
            if(counts) counts->rejected++;
            fprintf(stderr, "request rejected: type=%d key=%lu status=%d (DELETE unsupported with WAL)\n",
                    result[i]->type, result[i]->key, result[i]->result);
            free(result[i]->value);
            free(result[i]);
            continue;
        }
        if (result[i]->type == 0) {
            if(result[i]->size == 0){
                if(counts) counts->missing++;
                free(result[i]->value);
                free(result[i]);
                continue;
            }
            client_key_to_value(result[i]->key , value , result[i]->size);
            if(strcmp(value , result[i]->value) != 0){
                if(counts) counts->mismatched++;
                printf("result error:key %lu\nresult.value %s\n" , result[i]->key , result[i]->value);
                return ;
            }
            free(result[i]->value);

        }
        else if(result[i]->type == 1){
            free(result[i]->value);
        }
        else if(result[i]->type == 3) { free(result[i]->value); }
        else if(result[i]->type == 2){
            scan_result = (struct scan_kv *)result[i]->value;
            for(int j = 0 ; j < result[i]->size ; j++){
                if(scan_result->value[j] == NULL){
                    if(counts) counts->scan_invalid++;
                    printf("scan_result->value %d is NULL\n" , j);
                    return ;
                }
                free(scan_result->value[j]);
            }
            free(scan_result->value_size);
            free(scan_result->value);
            free(scan_result->key);
            free(scan_result);
        }
        free(result[i]);
    }
}

static struct db *client_monitor_db;

void client_polling_completed(struct db * db , int polling_num){

    uint64_t ticks_hz = spdk_get_ticks_hz();
    uint64_t last_report_time = spdk_get_ticks();
    uint64_t current_time;
    uint64_t five_seconds_ticks = 5 * ticks_hz;
    uint64_t completed;

    while (!test_completed) {
        space_stats_gc_poll(db);
        space_stats_nvm_poll(db);
        completed = 0;
        for(int i=0 ; i < db->load_injectors ; i++){
            completed += load_injector_completed[i];
        }
        if(completed >= polling_num){
            space_stats_nvm_check_capacity(db);
            if(db->phase_name && (!strcmp(db->phase_name, "E") || !strcmp(db->phase_name, "test")))
                space_stats_nvm_stop(db);
            test_completed = 1;
            break;
        }

        current_time = spdk_get_ticks();
        if (current_time - last_report_time > five_seconds_ticks) {
            printf("已完成 %lu 个请求 \n", completed);
            fflush(stdout);
            last_report_time = current_time;
        }
    }
}

int load_executor(void * arg){
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
    while((request < load_injector->load_num) || !test_completed){
        if(request < load_injector->load_num){
            req = (struct request *)calloc(1, sizeof(struct request));
            if(!req) { fprintf(stderr, "request allocation failed\n"); exit(EXIT_FAILURE); }
            req->key = test[request].key;
            req->type = test[request].type;
            req->size = test[request].size;
            if(req->type == 0){
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
        load_injector_completed[load_injector->thread] += result_num;
    }

    pthread_barrier_wait(&barrier_complete);
    printf("load_injector[%d] completed %lu\n" ,load_injector->thread, load_injector_completed[load_injector->thread]);
    fflush(stdout);
    return 0;
}

bool client_initialize_load_injectors(struct db * db , struct user_rq * load, int load_num){
    space_stats_nvm_check_capacity(db);
    phase_results_enabled = db->phase_io_stats;
    memset(phase_results, 0, sizeof(phase_results));
    struct phase_io_snapshot phase_start = {0};
    uint64_t logical_bytes = 0;
    if(db->phase_io_stats) {
        const char *error = space_stats_load_bytes(load, load_num, &logical_bytes);
        if(error) { fprintf(stderr, "phase input: %s\n", error); exit(EXIT_FAILURE); }
        phase_io_snapshot(db, &phase_start);
    }
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
        load_injectors[i].load_array = (struct user_req * )load;
        load_injectors[i].load_num = load_num;
        load_injectors[i].nb_load_injectors = db->load_injectors;
        printf("start load thread %d core %d(numa node %d)\n" , i , load_injectors[i].core,spdk_env_get_socket_id(load_injectors[i].core));
        fflush(stdout);
        if(spdk_env_thread_launch_pinned(load_injectors[i].core , (thread_start_fn)load_executor , &load_injectors[i])){
            printf("failed to start load thread %d\n" , i);
			return false;
        }
        else{
            printf("start load thread %d done\n" , i);
        }
        fflush(stdout);
    }

    pthread_barrier_wait(&barrier);
    start = spdk_get_ticks();
    client_polling_completed(db , load_num);
    end = spdk_get_ticks();


    for(int i = 0; i < db->load_injectors; i++)
        rte_eal_wait_lcore(load_injectors[i].core);
    space_stats_nvm_check_capacity(db);
    if(db->phase_io_stats) {
        phase_io_finish(db, &phase_start, load_num, logical_bytes);
        client_results_finish(db);
    }
    pthread_barrier_destroy(&barrier);
    pthread_barrier_destroy(&barrier_complete);
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
    memset(load_injector_completed, 0, sizeof(*load_injector_completed) * db->load_injectors);
    free(load_injectors);
    return true;
}


void client_executor(struct db * db){
    client_monitor_db = db;


    struct user_rq * warm = NULL;
    int warm_num = 0;
    struct user_rq * test;
    int test_num;
    int cache_key_num = 0;



    int is_production = 0;
    if (!db->load_only && strstr(g_libdb_config.test_path, "Production") != NULL) {
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
        client_monitor_db = NULL;
        return;
    }


    if (!db->recovery_mode) {
        printf("start to load\n");
        fflush(stdout);
        db->phase_name = "load";
        db->initial_load.complete = client_initialize_load_injectors(db ,warm, warm_num);
        if(db->initial_load.complete)
            db->initial_load.error = space_stats_load_bytes(
                warm, warm_num, &db->initial_load.record_bytes);
        if(db->load_only) {
            if(!db->initial_load.complete || db->initial_load.error)
                fprintf(stderr, "load-only failed: %s\n",
                        db->initial_load.error ? db->initial_load.error : "load_not_completed");
            free(warm);
            goto completed;
        }
        space_stats_gc_poll(db);
        space_stats_gc_wait(db, 10);
    } else {
        printf("recovery mode: skip load, keep cache warmup\n");
        fflush(stdout);
    }


    for(int i = warm_num - cache_key_num ; i < warm_num ; i++){
        warm[i].type = 0;
    }
    if (cache_key_num > 0) {
        db->phase_name = "warmup";
        client_initialize_load_injectors(db ,&warm[warm_num - cache_key_num], cache_key_num);
    }

    space_stats_gc_wait(db, 10);
    free(warm);
    if(db->precondition_a[0]) {
        test_get_num = test_insert_num = test_update_num = test_scan_num = 0;
        int a_num = 0;
        struct user_rq *a = client_read_request(db->precondition_a, &a_num, db->size_value);
        if(!a || a_num == 0) {
            fprintf(stderr, "failed to load precondition A: %s\n", db->precondition_a);
            free(a); exit(EXIT_FAILURE);
        }
        db->phase_name = "A";
        space_stats_nvm_start(db);
        bool a_ok = client_initialize_load_injectors(db, a, a_num);
        free(a);
        space_stats_nvm_check_capacity(db);
        if(db->output_fp && db->output_fp != stdout) space_stats_nvm_check_capacity(db);
        if(!a_ok) { fprintf(stderr, "precondition A failed\n"); exit(EXIT_FAILURE); }
    }
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
        db->phase_name = db->precondition_a[0] ? "E" : "test";
        if(nvm_flush_phase_get(&db->nvm_flush) == NVM_FLUSH_FIXED)
            space_stats_nvm_start(db);
        else
            space_stats_nvm_reset_stats(db);
        client_initialize_load_injectors(db ,test, test_num);
        space_stats_nvm_stop(db);
        space_stats_nvm_check_capacity(db);
        if(db->output_fp && db->output_fp != stdout) space_stats_nvm_check_capacity(db);
        free(test);
    }
completed:

    fflush(stdout);
    fflush(db->output_fp);

    benchmark_completed = 1;

    client_monitor_db = NULL;
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
    unsigned long long counts[4] = {0}, executed[4] = {0};
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
        if(client_monitor_db && (i % 1024 == 0)) {
            space_stats_gc_poll(client_monitor_db);
            space_stats_nvm_poll(client_monitor_db);
        }

        if (fscanf(fp , "%s user%lu " , s , &user) != 2) {
            break;
        }

        int operation = -1;
        request[i].key = user;
        request[i].size = kvsize;
        if(strcmp(s , "I") == 0 || strcmp(s , "U") == 0){
            if(strcmp(s , "I") == 0){
                test_insert_num ++;
                operation = 0;
            }
            else{
                test_update_num ++;
                operation = 1;
            }

            request[i].type = 1;
            if (fscanf(fp , "%s" , s) != 1) {
                break;
            }
        }
        else if(strcmp(s , "R") == 0){
            request[i].type = 0;
            test_get_num++;
            operation = 2;
        }
        else if(strcmp(s , "S") == 0){
            if (fscanf(fp , "%d\n" , &length) != 1) {
                break;
            }
            request[i].type = 2;
            test_scan_num ++;
            operation = 3;
            request[i].size = length;
        }
        else {
        }
        if(operation >= 0) counts[operation]++;
        i++;


        if(i % 1000 == 0) memcpy(executed, counts, sizeof(executed));

        if( i >= array_size){
            array_size += add_size;
            add_size = array_size;
            request = (struct user_rq *)realloc(request , sizeof(struct user_rq) * array_size);
        }
    }
    *num = i / 1000 * 1000;
    printf("read file %s %d request done\n" , file , *num);
    printf("WORKLOAD_COUNTS version=1 raw_requests=%d requests=%d tail_requests=%d"
           " insert=%llu update=%llu get=%llu scan=%llu"
           " tail_insert=%llu tail_update=%llu tail_get=%llu tail_scan=%llu\n",
           i, *num, i - *num, counts[0], counts[1], counts[2], counts[3],
           counts[0] - executed[0], counts[1] - executed[1],
           counts[2] - executed[2], counts[3] - executed[3]);
    return request;
}


void client_key_to_value(key_type key, char *value, int size) {
    if (value == NULL || size <= 0) {
        return;
    }

    size_t valid = (size_t)size < KV_RECORD_VALID_VALUE_SIZE ? (size_t)size : KV_RECORD_VALID_VALUE_SIZE;
    memset(value, 0, valid);

    snprintf(value, valid, "ID%" PRIu64, key);
}

void client_key_to_value_update(key_type key , char * value , int size){
    int random = rand();
    if (value == NULL || size <= 0) {
        return;
    }
    size_t valid = (size_t)size < KV_RECORD_VALID_VALUE_SIZE ? (size_t)size : KV_RECORD_VALID_VALUE_SIZE;
    memset(value, 0, valid);

    snprintf(value, valid, "ID%" PRIu64 "-%d", key, random);
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
        if(client_monitor_db && (i % 1024 == 0)) {
            space_stats_gc_poll(client_monitor_db);
            space_stats_nvm_poll(client_monitor_db);
        }

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
