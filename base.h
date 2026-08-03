#ifndef BASE_H_
#define BASE_H_
#include "config.h"
#define key_type uint64_t
#define KEY_MAX 18446744073709551615LU

#include "stdlib.h"
#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "pthread.h"

#define NVME_RING_IS_FULL(m , n) ((m)->nvme_request_ring_head == ((m)->nvme_request_ring_tail + 1) % n)

#define NVME_RING_IS_EMPTY( m ) ((m)->nvme_request_ring_head == (m)->nvme_request_ring_tail)

#define ADDR_IS_NULL(o) ((o).size == 0)

#define RING_IS_FULL(m , n) ((m)->server_ring_head == ((m)->server_ring_tail + 1) % n)
#define RESULT_RING_IS_FULL(m , n) ((m)->result_ring_head == ((m)->result_ring_tail + 1) % n)

#define RING_IS_EMPTY( m ) ((m)->server_ring_head == (m)->server_ring_tail)
#define RESULT_RING_IS_EMPTY( m ) ((m)->result_ring_head == ( m )->result_ring_tail)

int benchmark_completed ;

int ** boundary_signal;


int server_submitted;
int update_cache_hit[MAX_THREAD];


int thread_submitted[MAX_THREAD];
uint32_t thread_subspaces[MAX_THREAD];
key_type thread_scan_key[MAX_THREAD];

uint64_t g_ticks;



int ** index_signal;
pthread_mutex_t g_mutex_lock;
pthread_mutex_t g_mutex_lock_wbs;
pthread_mutex_t g_mutex_lock_lns;


pthread_mutex_t g_qpair_lock[31];
int g_qpair_lock_is_used[31];

int read_buffer_freed ;
int read_submitted ;
int read_spdk_submitted;
int qp_is_shared;

unsigned int * random_seed;

uint64_t * thread_scan_block;
uint64_t  * thread_block_rw ;
uint64_t * thread_gc_block_rw;
uint64_t * thread_flush_block_write;
uint64_t * thread_gc_block_write;
uint64_t * thread_split_block_write;

uint_fast8_t * g_map;
uint64_t g_block_num;

uint64_t * thread_split_block_rw;
uint64_t * thread_kvcache_hit;
uint64_t * thread_kvbuffer_hit;
uint64_t * thread_scan_total;
uint64_t * thread_scan_on_block;
uint64_t * thread_completed;
uint64_t * load_injector_completed;
uint64_t * invalid_block_num;

struct insert_time_breakdown {
    uint64_t buffer_insert_time;
    uint64_t index_insert_time;
    uint64_t flush_time;
    uint64_t flush_cb_time;
    uint64_t gc_time;
    uint64_t gc_cb_time;
    uint64_t split_time;
    uint64_t split_cb_time;
};

struct read_time_breakdown {
    uint64_t dram_index_time;
    uint64_t write_buffer_index_time;
    uint64_t search_cache_time;
    uint64_t nvm_index_time;
    uint64_t spdk_read_time;
    uint64_t read_cb_time;
};

struct insert_time_breakdown * insert_breakdown;
struct read_time_breakdown * read_breakdown;

struct kv
{
    key_type key;
    char * value;
};

struct request
{
    int type;
    key_type key;
    int size;
    char * value;
    void * break_down;
};

struct nvme_request{
    struct spdk_nvme_ns *ns ;
    void *payload ;
    uint64_t lba ;
    uint32_t lba_count ;
    void *cb_arg ;
    uint32_t io_flags;
    int type;
};

struct nvme_request_ring{
    struct nvme_request * ring;
    int nvme_request_ring_head;
    int nvme_request_ring_tail;
    int ring_size;
};

struct server_ring{
    struct request * ring;
    int  server_ring_head;
    int  server_ring_tail;
    int  ring_size;
};

struct address_type{
    uint32_t block;
    int off;
    int size;
    int no_use;
};

struct result{
    int type;
    key_type key;
    int size;
    char  * value;
    int result;
    uint64_t  start;
    uint64_t  end;
    void * break_down;
};
struct result_ring{
    struct result * ring;
    int result_ring_head;
    int result_ring_tail;
    int ring_size;
};

struct user_rq{
    key_type key;
    int type;
    int size;
};

struct slot_element{
    key_type key;
    struct address_type addr;
};

#define SLOT_MAX_ELEMENT LIBDB_LNF_SLOT_ENTRY_CAPACITY

/* Persistent NVM-index slot: boundary, entries, then alignment padding. */
struct nvm_index_slot{
    key_type boundary;
    struct slot_element entries[SLOT_MAX_ELEMENT];
    unsigned char reserved[LIBDB_LNF_SLOT_PADDING_BYTES];
};

_Static_assert(LIBDB_LNF_SLOT_SIZE > sizeof(key_type),
               "leaf-node slot must have room after its boundary key");
_Static_assert(LIBDB_LNF_SLOT_ENTRY_CAPACITY > 0,
               "leaf-node slot must hold at least one key/address entry");
_Static_assert(LIBDB_LNF_SLOT_PADDING_BYTES < sizeof(struct slot_element),
               "leaf-node slot padding must be smaller than one entry");
_Static_assert(sizeof(struct nvm_index_slot) == LIBDB_LNF_SLOT_SIZE,
               "nvm_index_slot must exactly match the persistent slot size");
_Static_assert(LIBDB_SUBSPACE_MAX_SLOTS * LIBDB_LNF_SLOT_ENTRY_CAPACITY >=
                   2U * LIBDB_SUBSPACE_MAX_KEYS,
               "subspace slot array must hold twice SUBSPACE_MAX_KEYS");

void access_disk();

void access_disk_release();

void access_nvm();

void access_nvm_release();

extern struct address_type ADDR_NULL ;

int random_level(int max , unsigned int * seed);


void random_seed_initialize(int thread);

#endif
