#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "./insert.h"
#include "base.h"
#include "hist.h"
#include "spdk_struct.h"
#include "recovery.h"
#include "subspace.h"
#include "write_buffer_index.h"
#include "kv_record.h"
#include "wal.h"
#include "./math.h"
#include <spdk/env.h>
#include <time.h>




key_type ** subspace_split_initialize(int thread , int size){
    key_type ** boundary = calloc(thread, sizeof(*boundary));
    if(!boundary) return NULL;

    key_type x = pow(10 , 19) / thread / size, y = 0;
    for(int i = 0 ; i< thread ; i++){
        boundary[i] = (key_type * )malloc(sizeof(key_type) * size);
        if(!boundary[i]) {
            for(int k = 0; k < i; k++) free(boundary[k]);
            free(boundary); return NULL;
        }
    }
    for (int j = 0; j < size; j++) {
        for (int i = 0; i < thread; i++) {
            boundary[i][j] = y;
            y += x;
        }
    }
    return boundary;
}


int key_to_thread(key_type key , struct db * db){
    assert(__glibc_likely(db->thread > 0));
    int l = 0, r = db->boundary_size - 1;
    while (l < r) {
        int m = (l + r + 1) / 2;
        if (db->boundary[0][m] <= key)
            l = m;
        else
            r = m - 1;
    }
    int j = l;
    l = 0, r = db->thread - 1;
    while (l < r) {
        int m = (l + r + 1) / 2;
        if (db->boundary[m][j] <= key)
            l = m;
        else
            r = m - 1;
    }
    return l;
}


static int init_db_common_storage(struct db * db){
    if(db->recovery_mode && !db->wal_enabled){
        fprintf(stderr, "recovery requires --wal=1\n");
        return -1;
    }
    printf("initialize_nvmessd start\n");
    db->ns_entry = initialize_nvmessd(db->thread, db->tasks_per_thread,
                                      db->thread + db->load_injectors + 1,
                                      g_libdb_config.pci_addr);
    if(db->ns_entry == NULL){
        printf("db->ns_entry == NULL\n");
        return -1;
    }

    printf("db->ns_entry->block_num is %lu\n" , db->ns_entry->block_num);
    db->data_blocks = db->ns_entry->block_num;
    if(db->wal_enabled && wal_layout(db->ns_entry->block_num, db->thread,
            db->wal_mb_per_worker, &db->data_blocks, &db->wal_worker_blocks)) {
        fprintf(stderr, "invalid WAL geometry or insufficient namespace capacity\n");
        return -1;
    }
    if(db->data_blocks > INT_MAX) {
        fprintf(stderr, "data area exceeds allocator block-ID limit\n");
        return -1;
    }
    db->fs = db->recovery_mode ? initialize_freespace_recovery((int)db->data_blocks) :
        initialize_freespace((int)db->data_blocks);
    if(!db->fs){
        printf("initialize freespace failed\n");
        return -1;
    }
    else{
        printf("initialize freespace done\n");
    }

    db->boundary = subspace_split_initialize(db->thread , db->boundary_size);
    return db->boundary ? 0 : -1;
}


static struct thread * attach_workers_after_index(struct db * db , struct thread * thread){
    db->workers = thread;
    db->spdk_result_ring = (struct spdk_ring **)calloc(db->load_injectors, sizeof(struct spdk_ring *));
    if(!db->spdk_result_ring) return NULL;
    for(int i = 0 ; i < db->load_injectors ; i++){
        db->spdk_result_ring[i] = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024, SPDK_ENV_SOCKET_ID_ANY);
        if(db->spdk_result_ring[i] == NULL){
            printf("ERROR:fail to init spdk ring\n");
            return NULL;
        }
    }

    db->spdk_ring = (struct spdk_ring **)calloc(db->thread, sizeof(struct spdk_ring *));
    if(!db->spdk_ring) return NULL;
    for(int i = 0 ; i < db->thread ; i++){
        write_buffer_initialize(&thread[i].kvbuffer);
        thread[i].index = &(db->index[i]);
        slot_allocator_initialize(&thread[i].slots);
        thread[i].ns_entry = db->ns_entry;
        thread[i].fs = db->fs;
        thread[i].curr_segment = db->recovery_mode ? NULL : get_free_segment(db->fs);
        thread[i].thread = i;
        thread[i].maintenance = NULL;
        thread[i].servicing_maintenance = false;
        thread[i].merge_count = thread[i].merge_read_blocks = thread[i].merge_write_blocks = 0;
        thread[i].wal = NULL;
        thread[i].db = db;
        phase_io_init(&thread[i].phase_io, db->phase_io_stats);
        if(db->wal_enabled && wal_init(&thread[i],
                db->data_blocks + i * db->wal_worker_blocks, db->wal_worker_blocks)) {
            fprintf(stderr, "WAL initialization failed for worker %d\n", i);
            return NULL;
        }
        thread[i].kv_version_next = 0;
        thread[i].kv_version_limit = 0;
        thread[i].lru_list = lru_list_init(db->cachesize * 1024 * 1024 / db->thread);
        if(!thread[i].lru_list) return NULL;
        thread_subspaces[i] = db->recovery_mode ? 0 : db->boundary_size;
        db->spdk_ring[i] = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024 , SPDK_ENV_SOCKET_ID_ANY);
        if(db->spdk_ring[i] == NULL){
            printf("ERROR:fail to init spdk ring\n");
            return NULL;
        }
        thread[i].spdk_ring = db->spdk_ring[i];
        thread[i].spdk_result_ring = db->spdk_result_ring;
    }
    printf("kvcache_init done\n");

    for(int i = 0 ; i < db->thread ; i++){
        if(thread[i].index->dram_index == NULL){
            printf("thread[%d].index->dram_index  == NULL\n" , i);
        }
    }

    return thread;
}



static void discard_recovery_startup(struct db *db, struct thread *workers) {
    for(int i = 0; i < db->thread; i++) {
        wal_destroy(&workers[i]);
        free(workers[i].lru_list);
        free(workers[i].curr_segment);
        if(db->spdk_ring && db->spdk_ring[i]) spdk_ring_free(db->spdk_ring[i]);
        if(db->index && db->index[i].dram_index) {
            free(db->index[i].dram_index->head);
            free(db->index[i].dram_index);
        }
        if(db->boundary) free(db->boundary[i]);
        thread_subspaces[i] = 0;
    }
    for(int i = 0; i < db->load_injectors; i++)
        if(db->spdk_result_ring && db->spdk_result_ring[i]) spdk_ring_free(db->spdk_result_ring[i]);
    free(db->spdk_ring); db->spdk_ring = NULL;
    free(db->spdk_result_ring); db->spdk_result_ring = NULL;
    free(db->index); db->index = NULL;
    free(db->boundary); db->boundary = NULL;
    if(db->fs) {
        while(db->fs->head) { struct segment *next = db->fs->head->next; free(db->fs->head); db->fs->head = next; }
        free(db->fs); db->fs = NULL;
        pthread_mutex_destroy(&g_mutex_lock);
    }
    free(g_map); g_map = NULL; g_block_num = 0; atomic_store(&g_used_data_blocks, 0);
    if(db->ns_entry) {
        release_nvmessd_startup(db->ns_entry, db->thread, db->tasks_per_thread);
        db->ns_entry = NULL;
    }
    free(workers); db->workers = NULL;
}

static int prepare_recovery_indices(struct db *db) {
    db->index = calloc(db->thread, sizeof(*db->index));
    if(!db->index) return -1;
    for(int i = 0; i < db->thread; i++) {
        struct dram_index *index = calloc(1, sizeof(*index));
        if(!index) return -1;
        db->index[i].dram_index = index;
        index->head = calloc(1, sizeof(*index->head));
        if(!index->head) return -1;
        index->head->is_leaf = true;
    }
    return 0;
}

static struct thread * initialize_mode(struct db * db){
    struct thread *thread = NULL;
    if(posix_memalign((void **)&thread, _Alignof(struct thread),
                     sizeof(*thread) * db->thread) != 0){
        return NULL;
    }

    memset(thread, 0, sizeof(*thread) * db->thread);
    printf("initialize start\n");

    if(init_db_common_storage(db) != 0) goto fail;

    printf("hybrid_index_initialize start\n");
    if(db->recovery_mode) {
        if(prepare_recovery_indices(db)) goto fail;
    } else db->index = hybrid_index_initialize(db->boundary, db->thread, db->boundary_size);
    printf("hybrid_index_initialize done\n");

    if(db->index == NULL || db->index[0].dram_index == NULL) goto fail;

    printf("initialize done\n");
    if(!attach_workers_after_index(db, thread)) goto fail;
    if(db->recovery_mode && !recovery_rebuild_index_and_write_buffer_parallel(db, NULL, NULL, NULL)) goto fail;

    load_injector_completed = malloc(sizeof(*load_injector_completed) * db->load_injectors);
    if(!load_injector_completed) { fprintf(stderr, "completion counters allocation failed\n"); exit(EXIT_FAILURE); }
    memset(load_injector_completed, 0, sizeof(*load_injector_completed) * db->load_injectors);
    random_seed_initialize(db->thread);
    histogram_init(db->load_injectors);
    histogram_reset_all(db->load_injectors);

    return thread;
fail:
    if(db->recovery_mode) discard_recovery_startup(db, thread);
    else free(thread);
    return NULL;
}

struct thread * initialize_recovery(struct db *db){
    if(!db || !db->wal_enabled || db->thread <= 0 || db->thread > ctr_max_qp ||
       db->boundary_size <= 0 || db->tasks_per_thread <= 0 || db->load_injectors <= 0) return NULL;
    db->recovery_mode = 1;
    return initialize_mode(db);
}

struct thread * initialize(struct db *db){
    if(!db) return NULL;
    return db->recovery_mode ? initialize_recovery(db) : initialize_mode(db);
}

void write_buffer_index_split(struct subspace_info * subspace_info , struct subspace_info * new_subspace_info ,
 key_type boundary){

    struct write_buffer_index_node * node;
    struct write_buffer_index * index ;
    struct write_buffer_index  * new_index = NULL;
    struct write_buffer_index  * prev_index;

    node = subspace_info->buffer_index->head;
    for (int i = subspace_info->buffer_index->levels - 1; i >= 0; i--) {
        while (node->forward[i] && node->forward[i]->key < boundary)
            node = node->forward[i];
        new_subspace_info->buffer_index->head->forward[i] = node->forward[i];
        node->forward[i] = NULL;
    }
}

void subspace_info_buffer_resize(struct subspace_info * subspace_info){
    subspace_info->buffered_size = 0;
    struct write_buffer_index_node * node = subspace_info->buffer_index->head;
    while( node->forward[0] != NULL){
        node = node->forward[0];
        subspace_info->buffered_size += node->size;
    }
    return ;
}

void write_buffer_index_output(struct subspace_info * subspace_info){
    struct write_buffer_index_node * node = subspace_info->buffer_index->head;
    while( node->forward[0] != NULL){
        node = node->forward[0];
        printf("%lu %d\n" , node->key , node->size);
    }
}

struct split_side_plan {
    key_type boundary;
    uint32_t key_num;
    uint32_t slot_num;
    uint32_t total_block_num;
    uint8_t slot_key_num[SUBSPACE_MAX_SLOT];
    key_type min_key[SUBSPACE_MAX_SLOT];
    struct index_slot *slot[SUBSPACE_MAX_SLOT];
    uint8_t update_index[SUBSPACE_MAX_SLOT];
};

struct split_plan {


    char *original_task_buffer;
    uint32_t recovery_buffer_blocks;
    key_type boundary;
    uint32_t update_count;
    uint32_t original_update_count;
    uint32_t boundary_slot_update;
    uint32_t new_right_slot_update;
    int boundary_aligned;
    struct index_slot boundary_slot_original;
    struct slot_update updates[2 * SUBSPACE_MAX_SLOT];
    struct index_slot logical_slots[2 * SUBSPACE_MAX_SLOT];
    struct index_slot *old_shadow[SUBSPACE_MAX_SLOT];
    struct index_slot *new_shadow[SUBSPACE_MAX_SLOT];
    struct split_side_plan old_side;
    struct split_side_plan new_side;
};

static void split_apply_side_plan(
    struct subspace_info *subspace_info,
    const struct split_side_plan *side);

static int split_side_add_slot(struct split_plan *plan, struct split_side_plan *side,
                               uint32_t update_index, uint8_t key_num){
    if(plan == NULL || side == NULL || key_num == 0 ||
       side->slot_num >= SUBSPACE_MAX_SLOT || update_index >= plan->update_count){
        return -1;
    }

    uint32_t pos = side->slot_num++;
    side->update_index[pos] = (uint8_t)update_index;
    side->slot[pos] = plan->updates[update_index].slot;
    side->slot_key_num[pos] = key_num;
    side->min_key[pos] = plan->updates[update_index].image.entries[0].key;
    return 0;
}


static void split_prepare_discard(struct split_plan *plan, struct subspace_info *right,
                                  uint32_t *blocks, struct slot_allocator *allocator) {
    if(plan) for(uint32_t i = plan->original_update_count; i < plan->update_count; i++)
        slot_index_free(allocator, plan->updates[i].slot);
    if(right) {
        write_buffer_index_destroy(right->buffer_index);
        kvcache_destroy_empty(right->kvcache);
        free(right);
    }
    free(blocks);
    free(plan);
}

static uint32_t *block_to_read_and_prepare_split(
    struct subspace_info *subspace_info, struct thread *thread,
    int *block_array_size, key_type *key_out,
    struct subspace_info **new_subspace_info_out, struct split_plan **plan_out){
    if(subspace_info == NULL || thread == NULL || block_array_size == NULL ||
       key_out == NULL || new_subspace_info_out == NULL || plan_out == NULL ||
       subspace_info->slot_num == 0 || subspace_info->slot_num > SUBSPACE_MAX_SLOT){
        return NULL;
    }

    struct split_plan *plan = (struct split_plan *)calloc(1, sizeof(*plan));
    struct subspace_info *new_subspace_info = subspace_info_initialize();
    uint32_t *block_array =
        (uint32_t *)malloc(sizeof(uint32_t) * subspace_info->key_num);
    if(plan == NULL || new_subspace_info == NULL || block_array == NULL){
        split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
        return NULL;
    }


    int block_count = 0;
    int indexed_key_count = 0;
    int split_slot = -1;
    int split_off = -1;
    int half = (int)subspace_info->key_num / 2;

    plan->update_count = subspace_info->slot_num;
    plan->original_update_count = subspace_info->slot_num;
    plan->boundary_slot_update = UINT32_MAX;
    plan->new_right_slot_update = UINT32_MAX;

    for(uint32_t i = 0; i < subspace_info->slot_num; i++){
        struct index_slot *slot = subspace_info->slot[i];
        void *src = slot;
        uint8_t slot_keys = subspace_info->slot_key_num[i];
        if(src == NULL || slot_keys == 0 || slot_keys > SLOT_MAX_ELEMENT){

            split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
            return NULL;
        }

        plan->updates[i].slot = slot;
        memcpy(&plan->updates[i].image, src, INDEX_SLOT_SIZE);
        struct slot_element *elements = plan->updates[i].image.entries;
        for(uint8_t j = 0; j < slot_keys; j++){
            if(!block_exist(elements[j].addr.block, block_array, block_count)){
                block_array[block_count++] = elements[j].addr.block;
            }
        }

        if(split_slot < 0 && indexed_key_count + slot_keys > half){
            split_slot = (int)i;
            split_off = half - indexed_key_count;
        }
        indexed_key_count += slot_keys;
    }

    if(split_slot < 0 || split_off < 0 ||
       indexed_key_count < half || half == 0){
        split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
        return NULL;
    }

    plan->boundary_slot_update = (uint32_t)split_slot;
    plan->boundary_aligned = (split_off == 0);
    plan->boundary_slot_original = plan->updates[split_slot].image;

    plan->old_side.boundary = subspace_info->boundary;
    plan->old_side.key_num = (uint32_t)half;
    plan->new_side.key_num = subspace_info->key_num - (uint32_t)half;

    for(int i = 0; i < split_slot; i++){
        if(split_side_add_slot(plan, &plan->old_side, (uint32_t)i,
                               subspace_info->slot_key_num[i]) != 0){
            split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
            return NULL;
        }
    }

    if(split_off == 0){
        for(uint32_t i = (uint32_t)split_slot; i < subspace_info->slot_num; i++){
            if(split_side_add_slot(plan, &plan->new_side, i,
                                   subspace_info->slot_key_num[i]) != 0){
                split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
                return NULL;
            }
        }
    }else{
        uint8_t original_count = subspace_info->slot_key_num[split_slot];
        uint8_t right_count = (uint8_t)(original_count - split_off);
        if(right_count == 0 || plan->update_count >= 2U * SUBSPACE_MAX_SLOT){
            split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
            return NULL;
        }
        struct index_slot *new_slot = slot_index_allocate(&thread->slots);
        if(!new_slot){
            split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
            return NULL;
        }

        uint32_t new_update = plan->update_count++;
        plan->new_right_slot_update = new_update;
        plan->updates[new_update].slot = new_slot;
        memset(&plan->updates[new_update].image, 0,
               sizeof(plan->updates[new_update].image));

        struct index_slot *left_image =
            &plan->updates[split_slot].image;
        struct index_slot *right_image =
            &plan->updates[new_update].image;
        struct slot_element *split_elements = left_image->entries;
        memcpy(right_image->entries, &split_elements[split_off],
               (size_t)right_count * sizeof(struct slot_element));
        memset(&split_elements[split_off], 0,
               (size_t)(SLOT_MAX_ELEMENT - split_off) *
                   sizeof(struct slot_element));

        if(split_side_add_slot(plan, &plan->old_side, (uint32_t)split_slot,
                               (uint8_t)split_off) != 0 ||
           split_side_add_slot(plan, &plan->new_side, new_update,
                               right_count) != 0){
            split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
            return NULL;
        }

        for(uint32_t i = (uint32_t)split_slot + 1;
            i < subspace_info->slot_num; i++){
            if(split_side_add_slot(plan, &plan->new_side, i,
                                   subspace_info->slot_key_num[i]) != 0){
                split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
                return NULL;
            }
        }
    }

    if(plan->old_side.slot_num == 0 || plan->new_side.slot_num == 0){
        split_prepare_discard(plan, new_subspace_info, block_array, &thread->slots);
        return NULL;
    }

    plan->boundary = plan->updates[
        plan->new_side.update_index[0]].image.entries[0].key;
    plan->new_side.boundary = plan->boundary;
    for(uint32_t i = 0; i < plan->old_side.slot_num; i++){
        plan->updates[plan->old_side.update_index[i]].image.boundary =
            plan->old_side.boundary;
    }
    for(uint32_t i = 0; i < plan->new_side.slot_num; i++){
        plan->updates[plan->new_side.update_index[i]].image.boundary =
            plan->new_side.boundary;
    }
    *key_out = plan->boundary;
    new_subspace_info->is_spliting = 1;

    for(uint32_t i = 0; i < plan->update_count; i++){
        plan->logical_slots[i] = plan->updates[i].image;
    }
    split_apply_side_plan(subspace_info, &plan->old_side);
    split_apply_side_plan(new_subspace_info, &plan->new_side);
    for(uint32_t i = 0; i < plan->old_side.slot_num; i++){
        plan->old_shadow[i] =
            &plan->logical_slots[plan->old_side.update_index[i]];
    }
    for(uint32_t i = 0; i < plan->new_side.slot_num; i++){
        plan->new_shadow[i] =
            &plan->logical_slots[plan->new_side.update_index[i]];
    }
    subspace_info->migration_slot_shadow = plan->old_shadow;
    new_subspace_info->migration_slot_shadow = plan->new_shadow;

    write_buffer_index_split(
        subspace_info, new_subspace_info, plan->boundary);
    kvcache_split(
        subspace_info->kvcache, new_subspace_info->kvcache, plan->boundary);
    subspace_info_buffer_resize(subspace_info);
    subspace_info_buffer_resize(new_subspace_info);

    thread_subspaces[thread->thread]++;
    *block_array_size = block_count;
    *new_subspace_info_out = new_subspace_info;
    *plan_out = plan;
    return block_array;
}








struct __u32_pair {
    uint32_t first, second;
};

static int __u32_pair_comp(const void *a, const void *b) {
    return ((struct __u32_pair*)a)->first - ((struct __u32_pair*)b)->first;
}

static uint32_t __block_map_find(uint32_t block, struct __u32_pair *block_map, int block_map_size) {
    int l = 0, r = block_map_size;
    while (l < r) {
        int m = (l + r) / 2;
        if (block_map[m].first < block)
            l = m + 1;
        else if (block_map[m].first > block)
            r = m;
        else
            return block_map[m].second;
    }
    return UINT32_MAX;
}

static int split_fill_side_buffer(
    char *new_buffer, const struct split_plan *plan,
    const struct split_side_plan *side, const char *source_buffer,
    struct __u32_pair *block_map, int block_map_size){
    int block_off = 0;
    int block_curr = 0;

    for(uint32_t i = 0; i < side->slot_num; i++){
        uint32_t update_index = side->update_index[i];
        const struct slot_element *elements =
            plan->updates[update_index].image.entries;
        for(uint8_t j = 0; j < side->slot_key_num[i]; j++){
            const struct address_type *addr = &elements[j].addr;
            uint32_t source_index =
                __block_map_find(addr->block, block_map, block_map_size);
            if(source_index == UINT32_MAX){
                fprintf(stderr, "split source block not found in block map\n");
                return -1;
            }
            if(block_off + addr->size > BLOCK_SIZE){
                block_curr++;
                block_off = 0;
            }
            memcpy(new_buffer + (size_t)block_curr * BLOCK_SIZE + block_off,
                   source_buffer + (size_t)source_index * BLOCK_SIZE + addr->off,
                   addr->size);
            block_off += addr->size;
        }
    }
    return block_curr + (block_off > 0);
}

static int split_fill_buffers(
    char *new_buffer, char *old_buffer, const struct split_plan *plan,
    const char *source_buffer, uint32_t *block_array, int block_array_size,
    int *block_num_out){
    struct __u32_pair *block_map = (struct __u32_pair *)malloc(
        sizeof(struct __u32_pair) * block_array_size);
    if(block_map == NULL){
        return -1;
    }
    for(int i = 0; i < block_array_size; i++){
        block_map[i].first = block_array[i];
        block_map[i].second = (uint32_t)i;
    }
    qsort(block_map, block_array_size, sizeof(struct __u32_pair),
          __u32_pair_comp);

    block_num_out[0] = split_fill_side_buffer(
        new_buffer, plan, &plan->new_side, source_buffer,
        block_map, block_array_size);
    block_num_out[1] = split_fill_side_buffer(
        old_buffer, plan, &plan->old_side, source_buffer,
        block_map, block_array_size);
    free(block_map);
    return block_num_out[0] > 0 && block_num_out[1] > 0 ? 0 : -1;
}

static int split_stage_side_addresses(
    struct split_plan *plan, struct split_side_plan *side,
    const uint32_t *blocks, uint32_t block_count){
    if(block_count == 0){
        return -1;
    }

    uint32_t block_curr = 0;
    uint32_t off = 0;
    for(uint32_t i = 0; i < side->slot_num; i++){
        uint32_t update_index = side->update_index[i];
        struct slot_element *elements =
            plan->updates[update_index].image.entries;
        for(uint8_t j = 0; j < side->slot_key_num[i]; j++){
            if(off + elements[j].addr.size > BLOCK_SIZE){
                block_curr++;
                off = 0;
            }
            if(block_curr >= block_count){
                return -1;
            }
            elements[j].addr.block = blocks[block_curr];
            elements[j].addr.off = off;
            off += elements[j].addr.size;
        }
    }
    return block_curr + 1U == block_count ? 0 : -1;
}

static void split_apply_side_plan(
    struct subspace_info *subspace_info,
    const struct split_side_plan *side){
    subspace_info->boundary = side->boundary;
    subspace_info->key_num = side->key_num;
    subspace_info->slot_num = side->slot_num;
    if(side->total_block_num != 0){
        subspace_info->total_block_num = side->total_block_num;
    }
    memset(subspace_info->slot, 0, sizeof(subspace_info->slot));
    memset(subspace_info->slot_key_num, 0, sizeof(subspace_info->slot_key_num));
    memset(subspace_info->min_key, 0, sizeof(subspace_info->min_key));
    memcpy(subspace_info->slot_key_num, side->slot_key_num,
           side->slot_num * sizeof(side->slot_key_num[0]));
    memcpy(subspace_info->min_key, side->min_key,
           side->slot_num * sizeof(side->min_key[0]));
    memcpy(subspace_info->slot, side->slot,
           side->slot_num * sizeof(side->slot[0]));
}

static void split_account_new_addresses(
    const struct split_plan *plan, const struct split_side_plan *side){
    for(uint32_t i = 0; i < side->slot_num; i++){
        const struct slot_element *elements =
            plan->updates[side->update_index[i]].image.entries;
        for(uint8_t j = 0; j < side->slot_key_num[i]; j++){
            block_ref_add(elements[j].addr.block);
        }
    }
}

static int split_validate_plan(const struct split_plan *plan){
    if(plan == NULL || plan->original_update_count == 0 ||
       plan->original_update_count > plan->update_count ||
       plan->update_count > 2U * SUBSPACE_MAX_SLOT ||
       plan->boundary_slot_update >= plan->original_update_count ||
       plan->boundary_slot_original.boundary != plan->old_side.boundary){
        return -1;
    }

    if(plan->boundary_aligned){
        if(plan->new_right_slot_update != UINT32_MAX ||
           plan->update_count != plan->original_update_count ||
           plan->updates[plan->boundary_slot_update].image.boundary !=
               plan->boundary ||
           plan->updates[plan->boundary_slot_update].image.entries[0].key !=
               plan->boundary){
            return -1;
        }
    }else{
        if(plan->new_right_slot_update < plan->original_update_count ||
           plan->new_right_slot_update >= plan->update_count ||
           plan->updates[plan->boundary_slot_update].image.boundary !=
               plan->old_side.boundary ||
           plan->updates[plan->new_right_slot_update].image.boundary !=
               plan->boundary ||
           plan->updates[plan->new_right_slot_update].image.entries[0].key !=
               plan->boundary){
            return -1;
        }
    }

    for(uint32_t i = 0; i < plan->update_count; i++){
        if(plan->updates[i].slot == NULL){
            return -1;
        }
        for(uint32_t j = 0; j < i; j++){
            if(plan->updates[i].slot == plan->updates[j].slot){
                return -1;
            }
        }
    }
    return 0;
}


static int split_publish_index(const struct split_plan *plan) {
    if(split_validate_plan(plan)) return -1;
    return slot_index_apply_updates(plan->updates, plan->update_count);
}

void split_write_cb(void *arg, const struct spdk_nvme_cpl *completnio){
    struct task *task = (struct task *)arg;
    struct thread *thread = (struct thread *)task->thread;
    struct subspace_info *old_subspace_info =
        (struct subspace_info *)task->subspace_info;
    struct subspace_info *new_subspace_info =
        (struct subspace_info *)task->new_subspace_info;
    struct split_plan *plan = (struct split_plan *)task->split_plan;

    if(spdk_nvme_cpl_is_error(completnio)){
        task->split_io_failed = 1;
        fprintf(stderr, "split SSD write completion reported an error\n");
    }
    task->block_completed++;
    if(task->block_completed < task->total_block_num){
        return;
    }
    if(task->split_io_failed){
        fprintf(stderr, "split aborted before slot updates because an SSD write failed\n");
        exit(EXIT_FAILURE);
    }
    if(plan == NULL ||
       split_publish_index(plan) != 0){
        fprintf(stderr,
                "split DRAM-index publication failed; old SSD blocks were retained\n");
        exit(EXIT_FAILURE);
    }

    old_subspace_info->total_block_num = plan->old_side.total_block_num;
    new_subspace_info->total_block_num = plan->new_side.total_block_num;
    split_account_new_addresses(plan, &plan->old_side);
    split_account_new_addresses(plan, &plan->new_side);

    old_subspace_info->migration_slot_shadow = NULL;
    new_subspace_info->migration_slot_shadow = NULL;
    new_subspace_info->redundant_size = 0;
    old_subspace_info->redundant_size = 0;
    new_subspace_info->is_spliting = 0;
    old_subspace_info->is_spliting = 0;

    reclaim_blocks(thread, task->block_array_to_gc, task->total_block_num_to_gc);
    free(task->block_array);
    free(task->block_array_to_gc);
    if(plan->original_task_buffer) {
        spdk_free(task->buffer);
        task->buffer = plan->original_task_buffer;
    }
    free(plan);
    task->split_plan = NULL;
    free_task(task);
}


static void split_read_cb(
    void *arg, const struct spdk_nvme_cpl *completnio){
    struct task *task = (struct task *)arg;
    struct thread *thread = (struct thread *)task->thread;
    struct ns_entry *ns_entry = thread->ns_entry;
    struct split_plan *plan = (struct split_plan *)task->split_plan;

    if(spdk_nvme_cpl_is_error(completnio)){
        task->split_io_failed = 1;
        fprintf(stderr, "split SSD read completion reported an error\n");
    }
    task->block_completed++;
    if(task->block_completed < task->total_block_num){
        return;
    }
    if(task->split_io_failed || plan == NULL){
        fprintf(stderr, "split cannot continue because an SSD read failed\n");
        exit(EXIT_FAILURE);
    }

    uint32_t *old_blocks = task->block_array;
    int old_block_count = task->total_block_num;
    size_t scratch_blocks = plan->recovery_buffer_blocks ? plan->recovery_buffer_blocks : (size_t)old_block_count;
    char *new_buffer = (char *)calloc(scratch_blocks, BLOCK_SIZE);
    char *old_buffer = (char *)calloc(scratch_blocks, BLOCK_SIZE);
    int block_num_to_write[2] = {0, 0};
    if(new_buffer == NULL || old_buffer == NULL ||
       split_fill_buffers(
           new_buffer, old_buffer, plan, task->buffer,
           old_blocks, old_block_count, block_num_to_write) != 0){
        fprintf(stderr, "failed to build split output buffers\n");
        exit(EXIT_FAILURE);
    }

    memcpy(task->buffer, new_buffer, (size_t)block_num_to_write[0] * BLOCK_SIZE);
    memcpy(task->buffer + (size_t)block_num_to_write[0] * BLOCK_SIZE,
           old_buffer, (size_t)block_num_to_write[1] * BLOCK_SIZE);
    free(new_buffer);
    free(old_buffer);

    plan->new_side.total_block_num = (uint32_t)block_num_to_write[0];
    plan->old_side.total_block_num = (uint32_t)block_num_to_write[1];

    task->block_array_to_gc = old_blocks;
    task->total_block_num_to_gc = old_block_count;
    task->total_block_num =
        block_num_to_write[0] + block_num_to_write[1];
    task->block_completed = 0;
    task->block_array =
        (uint32_t *)malloc(sizeof(uint32_t) * task->total_block_num);
    if(task->block_array == NULL){
        fprintf(stderr, "failed to allocate split destination block array\n");
        exit(EXIT_FAILURE);
    }

    for(int i = 0; i < task->total_block_num; i++){
        task->block_array[i] =
            get_free_block(&thread->curr_segment, thread->fs);
    }
    if(split_stage_side_addresses(
           plan, &plan->new_side, task->block_array,
           plan->new_side.total_block_num) != 0 ||
       split_stage_side_addresses(
           plan, &plan->old_side,
           &task->block_array[plan->new_side.total_block_num],
           plan->old_side.total_block_num) != 0){
        fprintf(stderr, "failed to stage split slot addresses\n");
        exit(EXIT_FAILURE);
    }

    task->split_io_failed = 0;
    int qpair_num = thread->thread % ctr_max_qp;
    for(int i = 0; i < task->total_block_num; i++){
        thread_block_rw[thread->thread]++;
        thread_split_block_rw[thread->thread]++;
        thread_split_block_write[thread->thread]++;
        int rc = spdk_nvme_ns_cmd_write(
            ns_entry->ns, ns_entry->qpair[qpair_num],
            task->buffer + (size_t)i * BLOCK_SIZE,
            task->block_array[i] * physical_block_to_logical_block,
            physical_block_to_logical_block,
            split_write_cb, task, 0);
        if(rc != 0){
            fprintf(stderr,
                    "failed to submit split SSD write; DRAM slots were not changed\n");
            exit(EXIT_FAILURE);
        }
        phase_io_add(&thread->phase_io, PHASE_SPLIT, 1);
    }
}





struct subspace_info * subspace_split(struct subspace_info * subspace_info , struct thread * thread , key_type * key_out){
    uint32_t * block_array;
    int block_array_size = 0;
    struct task * task;
    struct subspace_info * new_subspace_info = NULL;
    struct split_plan *plan = NULL;
    int rc;
    struct ns_entry * ns_entry = thread->ns_entry;
    int qpair_num = thread->thread % ctr_max_qp;


    block_array = block_to_read_and_prepare_split(
        subspace_info, thread, &block_array_size, key_out,
        &new_subspace_info, &plan);
    if(block_array == NULL || new_subspace_info == NULL ||
       plan == NULL || block_array_size <= 0) {
        fprintf(stderr, "ERROR: failed to prepare logical subspace split\n");
        exit(EXIT_FAILURE);
    }

    task = get_free_task(ns_entry, thread->thread);
    while(!task) {
        if(spdk_nvme_qpair_process_completions(ns_entry->qpair[qpair_num], 0) < 0) {
            fprintf(stderr, "fatal: split task wait completion polling failed\n");
            exit(EXIT_FAILURE);
        }
        wal_check_error(thread);
        task = get_free_task(ns_entry, thread->thread);
    }

    if(thread->db && thread->db->recovery_mode) {


        plan->recovery_buffer_blocks = plan->old_side.key_num + plan->new_side.key_num;
        if(plan->recovery_buffer_blocks > LIBDB_TASK_BUFFER_BLOCK_COUNT) {
            char *expanded = spdk_zmalloc((size_t)plan->recovery_buffer_blocks * BLOCK_SIZE,
                BLOCK_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
            if(!expanded) {
                fprintf(stderr, "split: cannot allocate DMA for oversized recovered group\n");
                exit(EXIT_FAILURE);
            }
            plan->original_task_buffer = task->buffer;
            task->buffer = expanded;
        }
    }
    task->thread = thread;
    task->total_block_num = block_array_size;
    task->block_completed = 0;
    task->subspace_info = subspace_info;
    task->new_subspace_info = new_subspace_info;
    task->block_array = block_array;
    task->split_plan = plan;
    task->split_io_failed = 0;
    task->key = *key_out;

    for(int i = 0 ; i < block_array_size ; i ++){
        thread_block_rw[thread->thread] ++;
        thread_split_block_rw[thread->thread]++;

        while ((rc = spdk_nvme_ns_cmd_read(
            ns_entry->ns, ns_entry->qpair[qpair_num],
            task->buffer + (size_t)i * BLOCK_SIZE,
            block_array[i] * physical_block_to_logical_block,
            physical_block_to_logical_block,
            split_read_cb, task, 0)) == -EAGAIN){
            spdk_nvme_qpair_process_completions(
                ns_entry->qpair[qpair_num], 0);
        }
        if(rc != 0){
            fprintf(stderr, "ERROR: failed to submit split SSD read\n");
            exit(EXIT_FAILURE);
        }
    }
    fflush(stdout);

    return new_subspace_info;

}

void flush_cb(void *arg, const struct spdk_nvme_cpl *completnio){
    struct task * task = (struct task *)arg;
    struct thread * thread = (struct thread *)task->thread;
    struct subspace_info * subspace_info = (struct subspace_info *)task->subspace_info;
    key_type key_new_subspace;
    struct subspace_info * subspace_info_new_subspace;
    struct write_buffer_index * old_index = (struct write_buffer_index *)task->index_to_free;
    struct write_buffer_index * index;
    struct write_buffer_return_type * buffer_to_free = (struct write_buffer_return_type *) task->buffer_to_free;
    uint64_t start_time , end_time;
    int flushed_block_num;

    if(spdk_nvme_cpl_is_error(completnio)){
        task->flush_io_failed = 1;
        fprintf(stderr, "flush SSD write completion reported an error\n");
    }
    task->block_completed ++;
    if(task->block_completed < task->total_block_num){
        return ;
    }
    if(task->flush_io_failed){
        fprintf(stderr,
                "flush aborted before slot publication because an SSD write failed\n");
        exit(EXIT_FAILURE);
    }
    flushed_block_num = task->total_block_num;

    if(slot_index_insert_after_flush(subspace_info, task->buffered_key,
                                    (struct address_type *)task->buffered_addr, task->buffered_num,
                                    &thread->slots) != 0){
        fprintf(stderr, "fatal: leaf-slot update failed\n");
        exit(EXIT_FAILURE);
    }








    if(kvcache_refresh_after_flush(
           subspace_info->kvcache, thread->lru_list, task->buffer,
           (size_t)task->total_block_num * BLOCK_SIZE,
           (const struct address_type *)task->buffered_addr,
           task->buffered_key, task->buffered_num) < 0){
        fprintf(stderr, "fatal: failed to refresh cache after flush\n");
        exit(EXIT_FAILURE);
    }


    if(old_index == NULL){
        printf("ERROR:flush_cb old_index = NULL\n");
        exit(EXIT_FAILURE);
    }
    else if(old_index->prev == NULL){
        printf("ERROR:flush_cb old_index->prev = NULL\n");
        exit(EXIT_FAILURE);
    }
    else if(old_index->next == NULL){
        old_index->prev->next = NULL;
    }
    else{
        old_index->prev->next = old_index->next;
        old_index->next->prev = old_index->prev;
    }

    for(struct write_buffer_index_node *n = old_index->head->forward[0]; n; n = n->forward[0])
        wal_release_dependencies(n);
    write_buffer_index_destroy(old_index);

    for(int i = 0  ; i < task->buffered_num ; i++){
        write_buffer_free(buffer_to_free[i].record , buffer_to_free[i].size , &thread->kvbuffer);
    }
    free(task->buffer_to_free);
    free(task->buffered_addr);
    free(task->buffered_key);

    free_task(task);

    subspace_info->flush_block_num -- ;
    subspace_info->total_block_num += flushed_block_num;

    double gc_fraction = space_stats_gc_fraction(thread->db);
    if(subspace_info->key_num >= SUBSPACE_MAX_KEY){
        if(subspace_info->flush_block_num != 0 || subspace_info->is_gc || subspace_info->is_merging){
            return ;
        }
        else{
            subspace_info->is_spliting = 1;
            subspace_info_new_subspace =  subspace_split(subspace_info ,  thread , &key_new_subspace);
            if(!subspace_info_new_subspace) return;
            subspace_info_new_subspace->is_spliting = 1;

            dram_index_insert(thread->index->dram_index , key_new_subspace , subspace_info_new_subspace , 0);
        }
    }
    else if(subspace_info->total_block_num != 0 && subspace_info->redundant_size >= subspace_info->total_block_num * BLOCK_SIZE * gc_fraction){
        if(subspace_info->flush_block_num != 0 || subspace_info->is_spliting || subspace_info->is_merging){
            return ;
        }
        if(!subspace_info->is_gc){
            start_time = spdk_get_ticks();
            subspace_info->is_gc = 1;
            garbadge_collection_with_fraction(subspace_info , thread, gc_fraction);
            end_time = spdk_get_ticks();
            insert_breakdown[thread->thread].gc_time += end_time - start_time;
        }
    }

    return ;
}


static uint32_t projected_slot_count_after_flush(struct subspace_info *, struct write_buffer_index *);

static int flush_write_buffer(struct subspace_info *group, struct thread *thread,
    struct ns_entry *ns_entry,
    void (*cb)(void *, const struct spdk_nvme_cpl *), key_type last_insert,
    struct insert_time_breakdown *break_down, bool include_partial){
    if(subspace_busy(group)) return 0;
    struct task *task = get_free_task(ns_entry, thread->thread);
    if(!task) return 0;



    struct write_buffer_index *batch = include_partial ?
        write_buffer_index_take_prefix(group->buffer_index,
            SUBSPACE_MAX_KEY / 2U, (size_t)LIBDB_TASK_BUFFER_BLOCK_COUNT * BLOCK_SIZE) :
        write_buffer_index_take_flush_prefix(group->buffer_index,
            SUBSPACE_MAX_KEY / 2U, (size_t)LIBDB_TASK_BUFFER_BLOCK_COUNT * BLOCK_SIZE);
    if(!batch){ free_task(task); return 0; }
    if(projected_slot_count_after_flush(group, batch) > SUBSPACE_MAX_SLOT){
        write_buffer_index_merge(batch, group->buffer_index);
        write_buffer_index_destroy(group->buffer_index);
        group->buffer_index = batch;
        free_task(task);
        key_type fence;
        group->is_spliting = true;
        struct subspace_info *right = subspace_split(group, thread, &fence);
        if(!right) return 0;
        dram_index_insert(thread->index->dram_index, fence, right, NULL);
        return 1;
    }
    size_t size = write_buffer_index_flush_size(batch, last_insert);
    uint32_t count = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t blocks[LIBDB_TASK_BUFFER_BLOCK_COUNT];
    size_t offset = 0;
    for(struct write_buffer_index_node *node = batch->head->forward[0]; node; node = node->forward[0]){
        if(offset % BLOCK_SIZE + node->size > BLOCK_SIZE){
            size_t padding = BLOCK_SIZE - offset % BLOCK_SIZE;
            memset(task->buffer + offset, 0, padding);
            offset += padding;
        }
        memcpy(task->buffer + offset, node->record, KV_RECORD_ENCODED_SIZE);
        offset += node->size;
        group->buffered_size -= node->size;
    }


    memset(task->buffer + offset, 0, (size_t)count * BLOCK_SIZE - offset);
    for(uint32_t i = 0; i < count; i++){
        int block = get_free_block(&thread->curr_segment, thread->fs);
        if(block < 0){ fprintf(stderr, "flush: out of SSD space\n"); exit(EXIT_FAILURE); }
        blocks[i] = block;
    }
    group->buffer_index->next = batch;
    batch->prev = group->buffer_index;
    group->flush_block_num++;
    task->buffered_num = write_buffer_index_to_array(batch, blocks, last_insert,
        &task->buffered_key, (struct address_type **)&task->buffered_addr,
        (struct write_buffer_return_type **)&task->buffer_to_free);
    task->subspace_info = group;
    task->thread = thread;
    task->index_to_free = batch;
    task->break_down = break_down;
    task->total_block_num = count;
    task->block_completed = 0;
    task->flush_io_failed = 0;
    for(uint32_t i = 0; i < count; i++){
        thread_block_rw[thread->thread]++;
        thread_flush_block_write[thread->thread]++;
        int rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[thread->thread % ctr_max_qp],
            task->buffer + (size_t)i * BLOCK_SIZE, (uint64_t)blocks[i] * physical_block_to_logical_block,
            physical_block_to_logical_block, cb, task, 0);
        if(rc){ fprintf(stderr, "flush: SSD submission failed\n"); exit(EXIT_FAILURE); }
        phase_io_add(&thread->phase_io, PHASE_FLUSH, 1);
    }
    return 1;
}

int flush_write_buffer_to_ssd(struct subspace_info *group, struct thread *thread,
    struct ns_entry *ns_entry, uint32_t unused_block,
    void (*cb)(void *, const struct spdk_nvme_cpl *), key_type last_insert,
    struct insert_time_breakdown *break_down){
    return flush_write_buffer(group, thread, ns_entry, cb, last_insert, break_down, false);
}



bool subspace_force_flush(struct subspace_info *group, struct thread *thread){
    if(subspace_busy(group) || !group->buffer_index->head->forward[0] ||
       no_free_task(thread->ns_entry, thread->thread)) return false;
    return flush_write_buffer(group, thread, thread->ns_entry,
        flush_cb, 0, &insert_breakdown[thread->thread], true) != 0;
}






static uint32_t projected_slot_count_after_flush(
    struct subspace_info *subspace_info, struct write_buffer_index *buffer_index){
    if(subspace_info == NULL || buffer_index == NULL || buffer_index->head == NULL ||
       subspace_info->slot_num > SUBSPACE_MAX_SLOT){
        return SUBSPACE_MAX_SLOT + 1U;
    }

    uint32_t projected_keys[SUBSPACE_MAX_SLOT] = {0};
    for(uint32_t i = 0; i < subspace_info->slot_num; i++){
        projected_keys[i] = subspace_info->slot_key_num[i];
    }

    uint32_t empty_subspace_keys = 0;

    for(struct write_buffer_index_node *node = buffer_index->head->forward[0];
        node != NULL; node = node->forward[0]){
        if(subspace_info->slot_num == 0){
            empty_subspace_keys++;
            continue;
        }

        uint32_t slot_index = 0;
        while(slot_index + 1U < subspace_info->slot_num &&
              node->key >= subspace_info->min_key[slot_index + 1U]){
            slot_index++;
        }

        struct slot_element *elements =
            subspace_slot_elements(subspace_info, slot_index);
        if(elements == NULL){

            return SUBSPACE_MAX_SLOT + 1U;
        }

        int left = 0;
        int right = (int)subspace_info->slot_key_num[slot_index] - 1;
        bool exists = false;
        while(left <= right){
            int middle = left + (right - left) / 2;
            if(elements[middle].key == node->key){
                exists = true;
                break;
            }
            if(elements[middle].key < node->key){
                left = middle + 1;
            }else{
                right = middle - 1;
            }
        }
        if(!exists){
            projected_keys[slot_index]++;
        }
    }

    if(subspace_info->slot_num == 0){
        return empty_subspace_keys == 0 ? 0 :
            (empty_subspace_keys + SLOT_MAX_ELEMENT - 1U) / SLOT_MAX_ELEMENT;
    }

    uint32_t projected_slots = 0;
    for(uint32_t i = 0; i < subspace_info->slot_num; i++){
        projected_slots +=
            (projected_keys[i] + SLOT_MAX_ELEMENT - 1U) / SLOT_MAX_ELEMENT;
    }
    return projected_slots;
}



void insert_version(key_type key, int size, char *value, struct thread *thread,
        struct insert_time_breakdown *break_down, uint64_t version, struct wal_dependency *dependency){
    const int size_encap = (int)kv_record_size((size_t)size);
    struct subspace_info * subspace_info;
    struct write_buffer_return_type buffer_to_free;
    char *record;
    subspace_info = dram_index_search(thread->index->dram_index, key, NULL, NULL);
    struct write_buffer_index_node *active =
        write_buffer_index_search(subspace_info->buffer_index, key);
    if(active && size >= (int)KV_RECORD_VALID_VALUE_SIZE &&
       (size_t)active->size == kv_record_size((size_t)size)){
        write_buffer_update_in_place(active->record, active->size, key, version, value, size);
        wal_attach_dependencies(active, dependency);
        return;
    }
    record = write_buffer_insert(key, version, value, size, &thread->kvbuffer);
    if(record == NULL){
        fprintf(stderr, "fatal: write_buffer_insert returned NULL\n");
        exit(EXIT_FAILURE);
    }
    buffer_to_free = slot_index_insert(subspace_info, record, key, size_encap, thread->thread);
    wal_attach_dependencies(write_buffer_index_search(subspace_info->buffer_index, key), dependency);
    if(buffer_to_free.size != 0){
        write_buffer_free(buffer_to_free.record ,  buffer_to_free.size ,&thread->kvbuffer);
        return;
    }

    (void)subspace_maybe_flush(subspace_info, thread);
}

void insert(key_type key, int size, char *value, struct thread *thread,
        struct insert_time_breakdown *break_down) {
    if(thread->wal) {
        fprintf(stderr, "fatal: WAL PUT must enter through the worker queue\n");
        exit(EXIT_FAILURE);
    }
    insert_version(key, size, value, thread, break_down,
        kv_record_next_thread_version(&thread->kv_version_next, &thread->kv_version_limit), NULL);
}


bool subspace_maybe_flush(struct subspace_info *group, struct thread *thread){
    for(;;){

        if(benchmark_completed || group->buffered_size < LIBDB_FLUSH_THRESHOLD_BYTES ||
           !space_stats_nvm_flush_allowed_for_size(thread->db, group->buffered_size)) return true;

        if(subspace_busy(group)) return false;
        if(!no_free_task(thread->ns_entry, thread->thread)) break;



        int rc = spdk_nvme_qpair_process_completions(
            thread->ns_entry->qpair[thread->thread % ctr_max_qp], 0);
        if(rc < 0){
            fprintf(stderr, "flush: worker %d completion processing failed while waiting for a task (rc=%d)\n",
                    thread->thread, rc);
            exit(EXIT_FAILURE);
        }
    }
    return flush_write_buffer_to_ssd(group, thread, thread->ns_entry, 0,
        flush_cb, 0, &insert_breakdown[thread->thread]) != 0;
}
