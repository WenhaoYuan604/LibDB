#ifndef KVBUFFER_H_
#define KVBUFFER_H_

#include "./pmdk.h"
#include "./base.h"
#include "./subspace.h"
#include "./kv_record.h"
#include "./spdk_struct.h"


#define WBF_FORMAT_MAGIC UINT64_C(0x574246505245414c)
#define WBF_FORMAT_VERSION 1U
#define WBF_NO_FILE UINT32_MAX

struct write_buffer_file_root{
    PMEMoid segment[WSB_NUM_MAX];
    uint16_t segment_element_size[WSB_NUM_MAX];
    uint16_t segment_num;

    uint64_t magic, file_size, segment_size;
    uint32_t format_version, initialized, file_count, last_used_wbf_seq;
    uint16_t preallocated_segments[MAX_FILE_NUM];
};

struct write_buffer_free_node{
    uint32_t num;
    uint16_t size;
    uint16_t WBF_seq;
    PMEMoid buffer;
    struct write_buffer_free_node * next;
};




struct wbf_worker_stats {
    _Alignas(64) int64_t used_delta;
    uint64_t reusable;
    _Alignas(64) uint64_t published_used_delta;
    uint64_t published_reusable;
};
_Static_assert(sizeof(struct wbf_worker_stats) == 128, "WBF shard cache-line layout");
_Static_assert(offsetof(struct wbf_worker_stats, published_used_delta) == 64,
               "WBF publication must not share the writer cache line");

struct wbf_snapshot {
    uint64_t used_slot_bytes, reusable_slot_bytes, pressure_bytes;
    uint64_t worker_reusable_bytes, reserved_segment_bytes;
};

struct write_buffer{

    uint64_t recovered_used_bytes;
    struct wbf_worker_stats worker_stats[MAX_THREAD];
    atomic_bool pressure;
    uint64_t file_bytes, file_peak_bytes, limit_rejections, pressure_changes;
    bool pressure_tracking;
    uint32_t file_num;
    uint32_t opened_file_count, last_used_wbf_seq, recovery_file_count;
    uint32_t next_file, next_segment;
    uint64_t reserved_segment_bytes;
    uint16_t preallocated_segments[MAX_FILE_NUM];
    PMEMobjpool * pool[MAX_FILE_NUM];
};

char * write_buffer_file_str(int file_seq);


static inline uint32_t write_buffer_used_file_count(const struct write_buffer *wb){
    return wb->last_used_wbf_seq == WBF_NO_FILE ? 0 : wb->last_used_wbf_seq + 1;
}
void write_buffer_close(struct write_buffer *write_buffer);







#if !defined(__x86_64__) || (!defined(__GNUC__) && !defined(__clang__))
#error "WBF snapshot publication requires GNU-compatible x86-64 movq"
#endif
static inline void wbf_snapshot_store(uint64_t *address, uint64_t value){
    __asm__ __volatile__("movq %1, %0" : "=m"(*address) : "r"(value));
}
static inline uint64_t wbf_snapshot_load(const uint64_t *address){
    uint64_t value;
    __asm__ __volatile__("movq %1, %0" : "=r"(value) : "m"(*address));
    return value;
}




static inline void write_buffer_account(struct write_buffer *wb, int worker,
                                        int64_t used_delta, int64_t reusable_delta){
    if(worker < 0 || worker >= MAX_THREAD) abort();
    struct wbf_worker_stats *s = &wb->worker_stats[worker];
    if((used_delta > 0 && s->used_delta > INT64_MAX - used_delta) ||
       (used_delta < 0 && s->used_delta < INT64_MIN - used_delta)) abort();
    if(reusable_delta < 0){
        uint64_t amount = (uint64_t)(-(reusable_delta + 1)) + 1;
        if(s->reusable < amount) abort();
        s->reusable -= amount;
    } else {
        if(s->reusable > UINT64_MAX - (uint64_t)reusable_delta) abort();
        s->reusable += (uint64_t)reusable_delta;
    }
    s->used_delta += used_delta;
    wbf_snapshot_store(&s->published_used_delta, (uint64_t)s->used_delta);
    wbf_snapshot_store(&s->published_reusable, s->reusable);
}




static inline struct wbf_snapshot write_buffer_snapshot_locked(const struct write_buffer *wb){
    __int128 used = wb->recovered_used_bytes;
    unsigned __int128 reusable = 0;
    for(int i = 0; i < MAX_THREAD; i++){
        used += (int64_t)wbf_snapshot_load(&wb->worker_stats[i].published_used_delta);
        reusable += wbf_snapshot_load(&wb->worker_stats[i].published_reusable);
    }
    uint64_t worker_reusable = reusable > wb->file_bytes ? wb->file_bytes : (uint64_t)reusable;
    reusable += wb->reserved_segment_bytes;
    struct wbf_snapshot snapshot = {
        .worker_reusable_bytes = worker_reusable,
        .reserved_segment_bytes = wb->reserved_segment_bytes,
        .used_slot_bytes = used < 0 ? 0 : used > wb->file_bytes ? wb->file_bytes : (uint64_t)used,
        .reusable_slot_bytes = reusable > wb->file_bytes ? wb->file_bytes : (uint64_t)reusable
    };
    snapshot.pressure_bytes = wb->file_bytes - snapshot.reusable_slot_bytes;
    return snapshot;
}
static inline uint64_t write_buffer_pressure_bytes_locked(const struct write_buffer *wb){
    return write_buffer_snapshot_locked(wb).pressure_bytes;
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


struct write_buffer * write_buffer_open_existing();

int write_buffer_rebuild_occupancy(struct write_buffer *write_buffer);

int write_buffer_segement_new(struct write_buffer * write_buffer , int size , struct write_buffer_free_node ** free_list_head, int worker);

PMEMoid write_buffer_get_free_node(int size , uint16_t * WBF_seq ,struct write_buffer * write_buffer ,struct write_buffer_free_node ** free_list_head , PMEMobjpool ** pool, int worker);

PMEMoid write_buffer_insert(key_type key, kv_version_type version,
 const char *value, int value_size, struct write_buffer *write_buffer,
 uint16_t *WBF_seq, struct write_buffer_free_node **free_list_head, int thread);



void write_buffer_update_in_place(PMEMoid id, size_t record_size, key_type key,
    kv_version_type version, const char *value, int value_size);


int write_buffer_free(struct write_buffer *write_buffer, PMEMoid id , uint16_t WBF_seq , int size, struct write_buffer_free_node ** free_list_head, int worker);


#endif
