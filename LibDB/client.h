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

extern struct db *client_monitor_db;
extern int test_get_num, test_insert_num, test_update_num, test_scan_num;
bool client_initialize_load_injectors(struct db *, struct user_rq *, int);
void client_executor(struct db * db);
static inline bool client_phase_is_final(const struct db *db)
{
    return db->benchmark_phase &&
        !strcmp(db->benchmark_phase, g_libdb_config.post_load_path[0] ? "measured_e100" : "measured");
}

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
