#ifndef C0724F11_21CD_4965_B6C4_2D104046E603
#define C0724F11_21CD_4965_B6C4_2D104046E603
#include "./server.h"
#include "stdlib.h"

struct pending_scan_request {
    key_type key;           // scan请求的键
    int scan_length;        // 请求的scan长度
    uint64_t submit_time;   // 提交时间（以ticks为单位）
    bool completed;         // 是否已完成
    int thread_id;          // 处理该请求的线程ID
};
//若tail = head表示空，若head = tail+1表示满

// #define REQUEST_SIZE value_size + 8

void client_initialize(struct db * db);

void client_executor(struct db * db)  ;

int client_initialize_request_ring(struct db * db) ;

void client_submit_request(struct request req , struct server_ring * ring , int ring_size) ;

int client_initialize_result_ring(struct db * db);

struct result  client_get_result(struct result_ring * ring );

int client_get_total_result(struct result_ring * ring);

void client_key_to_value(key_type key , char * value , int size);

void client_key_to_value_update(key_type key , char * value , int size);

struct user_rq * client_read_request(char * file  , int * num , int size_value);

struct user_rq * client_read_production_request(char * file  , int * num);

#endif /* C0724F11_21CD_4965_B6C4_2D104046E603 */
