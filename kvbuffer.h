#ifndef KVBUFFER_H_
#define KVBUFFER_H_

#include "./pmdk.h"
#include "./base.h"
#include "./subspace.h"
#include "./kv_record.h"
#include "./spdk_struct.h"


struct write_buffer_file_root{
    PMEMoid segment[WSB_NUM_MAX];
    uint16_t segment_element_size[WSB_NUM_MAX];
    uint16_t segment_num;
};

struct write_buffer_free_node{
    uint32_t num;
    uint16_t size;
    uint16_t WBF_seq;
    PMEMoid buffer;
    struct write_buffer_free_node * next;
};

struct write_buffer{//考虑多线程
    _Atomic uint64_t used_slot_bytes; /* Allocated slots, including in-flight flushes. */
    _Atomic uint64_t reusable_slot_bytes;
    atomic_bool pressure; /* Worker-readable; updates below require g_mutex_lock_wbs. */
    uint64_t file_bytes, file_peak_bytes, limit_rejections, pressure_changes;
    bool pressure_tracking; /* Count transitions only during the measured workload. */
    uint32_t file_num;
    PMEMobjpool * pool[MAX_FILE_NUM];
};

char * write_buffer_file_str(int file_seq);

/* Must hold g_mutex_lock_wbs. Owns quota check, pool publication and accounting. */
PMEMobjpool * write_buffer_file_new(struct write_buffer *write_buffer);

/* File geometry is stable while holding g_mutex_lock_wbs. Slot reuse is atomic. */
static inline uint64_t write_buffer_pressure_bytes_locked(const struct write_buffer *wb){
    uint64_t reusable = atomic_load_explicit(&wb->reusable_slot_bytes, memory_order_relaxed);
    if(reusable > wb->file_bytes){
        fprintf(stderr, "WBF reusable capacity exceeds file capacity\n");
        abort();
    }
    return wb->file_bytes - reusable;
}
static inline void write_buffer_set_pressure_locked(struct write_buffer *wb, bool pressure){
    bool old = atomic_load_explicit(&wb->pressure, memory_order_relaxed);
    if(old != pressure && wb->pressure_tracking) wb->pressure_changes++;
    atomic_store_explicit(&wb->pressure, pressure, memory_order_relaxed);
}
static inline void write_buffer_raise_pressure_locked(struct write_buffer *wb){
    if(write_buffer_pressure_bytes_locked(wb) >= LIBDB_NVM_FLUSH_HIGH_BYTES)
        write_buffer_set_pressure_locked(wb, true);
}

struct write_buffer * write_buffer_initialize();

/* recovery: 打开已有 write_buffer_file*（layout = "write_buffer"）；找不到返回 NULL */
struct write_buffer * write_buffer_open_existing();
/* Startup only: scan physical slots once, before any worker can mutate them. */
int write_buffer_rebuild_occupancy(struct write_buffer *write_buffer);

int write_buffer_segement_new(struct write_buffer * write_buffer , int size , struct write_buffer_free_node ** free_list_head);

PMEMoid write_buffer_get_free_node(int size , uint16_t * WBF_seq ,struct write_buffer * write_buffer ,struct write_buffer_free_node ** free_list_head , PMEMobjpool ** pool);

PMEMoid write_buffer_insert(key_type key, kv_version_type version,
 const char *value, int value_size, struct write_buffer *write_buffer,
 uint16_t *WBF_seq, struct write_buffer_free_node **free_list_head, int thread);

/* Persist key_size=0 before returning the allocation unit to the DRAM free list. */
int write_buffer_free(struct write_buffer *write_buffer, PMEMoid id , uint16_t WBF_seq , int size, struct write_buffer_free_node ** free_list_head);


#endif
