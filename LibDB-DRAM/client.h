#ifndef C0724F11_21CD_4965_B6C4_2D104046E603
#define C0724F11_21CD_4965_B6C4_2D104046E603
#include "./server.h"
#include "stdlib.h"

struct pending_scan_request {
    key_type key;
    int scan_length;
    uint64_t submit_time;
    bool completed;
    int thread_id;
};



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

#endif
