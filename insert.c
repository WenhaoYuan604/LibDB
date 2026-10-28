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
#include "./math.h"
#include <spdk/env.h>
#include <time.h>


/*将整个键空间划分为 thread * size个大小相等的子空间*/

key_type ** subspace_split_initialize(int thread , int size){
    key_type ** boundary = (key_type **)malloc(sizeof(key_type*) * thread);

    int64_t x = pow(10 , 19) / thread / size, y = 0;
    for(int i = 0 ; i< thread ; i++){
        boundary[i] = (key_type * )malloc(sizeof(key_type) * size);
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

/** NVMe / 空闲空间 / 键空间边界 / write_buffer — 正常启动与 recovery 共用 */
static int init_db_common_storage(struct db * db){
    printf("initialize_nvmessd start\n");
    db->ns_entry = initialize_nvmessd(db->thread, db->tasks_per_thread,
                                      db->thread + db->load_injectors + 1,
                                      g_libdb_config.pci_addr);
    if(db->ns_entry == NULL){
        printf("db->ns_entry == NULL\n");
        return -1;
    }

    printf("db->ns_entry->block_num is %lu\n" , db->ns_entry->block_num);
    if(db->recovery_mode){
        db->fs = initialize_freespace_recovery(db->ns_entry->block_num);
    }
    else{
        db->fs = initialize_freespace(db->ns_entry->block_num);
    }
    if(!db->fs){
        printf("initialize freespace failed\n");
    }
    else{
        printf("initialize freespace done\n");
    }

    db->boundary = subspace_split_initialize(db->thread , db->boundary_size);

    printf("write_buffer_initialize start\n");
    if(db->recovery_mode){
        db->write_buffer = write_buffer_open_existing();
        if(db->write_buffer == NULL){
            printf("write_buffer_open_existing failed (no existing write_buffer_file*)\n");
            return -1;
        }
    }
    else{
        db->write_buffer = write_buffer_initialize();
    }
    if(db->write_buffer == NULL){
        printf("db->write_buffer == NULL\n");
        return -1;
    }
    return 0;
}

/** hybrid_index 就绪之后：SPDK ring、每线程状态、histogram（调用方已保证 dram_index 有效） */
static struct thread * attach_workers_after_index(struct db * db , struct thread * thread){
    db->workers = thread;
    db->spdk_result_ring = (struct spdk_ring **)malloc(sizeof(struct spdk_ring *) * db->load_injectors);
    for(int i = 0 ; i < db->load_injectors ; i++){
        db->spdk_result_ring[i] = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024, SPDK_ENV_SOCKET_ID_ANY);
        if(db->spdk_result_ring[i] == NULL){
            printf("ERROR:fail to init spdk ring\n");
        }
    }

    db->spdk_ring = (struct spdk_ring **)malloc(sizeof(struct spdk_ring *) * db->thread);
    for(int i = 0 ; i < db->thread ; i++){
        thread[i].kvbuffer = db->write_buffer;
        thread[i].index = &(db->index[i]);
        thread[i].buffer_free_head = NULL;
        thread[i].index_free_head =
            db->recovery_index_free_heads == NULL ? NULL :
            db->recovery_index_free_heads[i];
        thread[i].ns_entry = db->ns_entry;
        thread[i].fs = db->fs;
        thread[i].curr_segment = get_free_segment(db->fs);
        thread[i].thread = i;
        thread[i].maintenance = NULL;
        thread[i].read_pins = NULL;
        thread[i].servicing_maintenance = false;
        thread[i].merge_count = thread[i].merge_read_blocks = thread[i].merge_write_blocks = 0;
        ssd_write_stats_init(&thread[i].ssd_writes);
        thread[i].kv_version_next = 0;
        thread[i].kv_version_limit = 0;
        thread[i].lru_list = lru_list_init(db->cachesize * 1024 * 1024 / db->thread);
        thread_subspaces[i] = db->boundary_size;
        db->spdk_ring[i] = spdk_ring_create(SPDK_RING_TYPE_MP_SC, 1024 , SPDK_ENV_SOCKET_ID_ANY);
        if(db->spdk_ring[i] == NULL){
            printf("ERROR:fail to init spdk ring\n");
        }
        thread[i].spdk_ring = db->spdk_ring[i];
        thread[i].spdk_result_ring = db->spdk_result_ring;
    }
    free(db->recovery_index_free_heads);
    db->recovery_index_free_heads = NULL;
    printf("kvcache_init done\n");

    for(int i = 0 ; i < db->thread ; i++){
        if(thread[i].index->dram_index == NULL){
            printf("thread[%d].index->dram_index  == NULL\n" , i);
        }
    }

    load_injector_completed = (uint64_t *)malloc(sizeof(uint64_t) * db->load_injectors);
    if(!load_injector_completed) { perror("completion counters"); exit(EXIT_FAILURE); }
    memset(load_injector_completed, 0, sizeof(uint64_t) * db->load_injectors);
    random_seed_initialize(db->thread);
    histogram_init(db->load_injectors);
    histogram_reset_all(db->load_injectors);

    return thread;
}

struct thread * initialize(struct db * db){
    struct thread * thread = (struct thread *)malloc(sizeof(struct thread) * (db->thread));
    if(thread == NULL){
        return NULL;
    }

    printf("initialize start\n");

    if(init_db_common_storage(db) != 0){
        free(thread);
        return NULL;
    }

    printf("hybrid_index_initialize start\n");
    db->index = hybrid_index_initialize(db->boundary , db->thread , db->boundary_size);
    printf("hybrid_index_initialize done\n");

    if(db->index == NULL || db->index[0].dram_index == NULL){
        free(thread);
        return NULL;
    }

    printf("initialize done\n");
    return attach_workers_after_index(db , thread);
}

struct thread * initialize_recovery(struct db * db){
    struct thread * thread = (struct thread *)malloc(sizeof(struct thread) * (db->thread));
    if(thread == NULL){
        return NULL;
    }

    printf("initialize_recovery start\n");

    struct timespec ts_total0, ts_recovery1;
    struct timespec ts_init0, ts_init1;
    struct timespec ts_check0, ts_check1;
    clock_gettime(CLOCK_MONOTONIC, &ts_total0);
    ts_init0 = ts_total0;

    double ticks_hz = 0.0;
    uint64_t t_index0 = 0, t_index1 = 0;
    uint64_t t_wb0 = 0, t_wb1 = 0;
    uint64_t t_fs0 = 0, t_fs1 = 0;

    if(init_db_common_storage(db) != 0){
        free(thread);
        return NULL;
    }

    struct nvm_index * recovered_nvm_index = nvm_index_open_existing();
    if(recovered_nvm_index == NULL){
        fprintf(stderr, "recovery 失败：未找到可打开的 NVM 索引文件（请确认 %s 中的 leaf_node_file* 存在且可 pmemobj_open）\n",
                g_libdb_config.pmem_dir);
        free(thread);
        return NULL;
    }

    db->index = hybrid_index_initialize_empty_with_nvm(db->thread , recovered_nvm_index);
    if(db->index == NULL){
        printf("hybrid_index_initialize_empty_with_nvm failed\n");
        free(thread);
        return NULL;
    }

    /* 到这里 SPDK 环境应已初始化，ticks/ticks_hz 才可靠 */
    clock_gettime(CLOCK_MONOTONIC, &ts_init1);
    ticks_hz = (double)spdk_get_ticks_hz();
    t_index0 = spdk_get_ticks();

    /* 只 pin 一次：同一批 worker 内依次完成 dram_index/subspace_info + write_buffer_index */
    if(!recovery_rebuild_index_and_write_buffer_parallel(db, &t_index1, &t_wb1, &t_fs1)){
        printf("recovery_rebuild_index_and_write_buffer_parallel failed\n");
        free(thread);
        return NULL;
    }

    t_wb0 = t_index1;
    t_fs0 = t_wb1;

    /* Recovery ends here.  Optional diagnostic checks are timed separately. */
    clock_gettime(CLOCK_MONOTONIC, &ts_recovery1);

    double check_sec = 0.0;
    if(db->recovery_dupcheck){
        clock_gettime(CLOCK_MONOTONIC, &ts_check0);
        if(!recovery_report_duplicate_keys(db)){
            fprintf(stderr , "recovery_report_duplicate_keys 失败（如内存不足）；未得到重复键统计\n");
        }
        clock_gettime(CLOCK_MONOTONIC, &ts_check1);
        check_sec = (double)(ts_check1.tv_sec - ts_check0.tv_sec) +
                    (double)(ts_check1.tv_nsec - ts_check0.tv_nsec) / 1e9;
    }

    double total_sec = (double)(ts_recovery1.tv_sec - ts_total0.tv_sec) +
                       (double)(ts_recovery1.tv_nsec - ts_total0.tv_nsec) / 1e9;
    double init_sec = (double)(ts_init1.tv_sec - ts_init0.tv_sec) +
                      (double)(ts_init1.tv_nsec - ts_init0.tv_nsec) / 1e9;

    /* rebuild breakdown 用 SPDK ticks（此时已可用） */
    double index_sec = (double)(t_index1 - t_index0) / ticks_hz;
    double wb_sec = (double)(t_wb1 - t_wb0) / ticks_hz;
    double fs_sec = (double)(t_fs1 - t_fs0) / ticks_hz;

    printf("recovery timing: total=%.6fs recovery_init=%.6fs rebuild_index=%.6fs rebuild_write_buffer=%.6fs rebuild_freespace=%.6fs\n",
           total_sec, init_sec, index_sec, wb_sec, fs_sec);
    if(db->recovery_dupcheck){
        printf("recovery check timing (excluded from total): duplicate_key_check=%.6fs\n",
               check_sec);
    }
    if(db->output_fp){
        fprintf(db->output_fp,
                "recovery timing: total=%.6fs recovery_init=%.6fs rebuild_index=%.6fs rebuild_write_buffer=%.6fs rebuild_freespace=%.6fs\n",
                total_sec, init_sec, index_sec, wb_sec, fs_sec);
        if(db->recovery_dupcheck){
            fprintf(db->output_fp,
                    "recovery check timing (excluded from total): duplicate_key_check=%.6fs\n",
                    check_sec);
        }
        fflush(db->output_fp);
    }

    printf("initialize_recovery done（dram_index / subspace_info 已从 NVM 恢复）\n");
    return attach_workers_after_index(db , thread);
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
    PMEMoid slot[SUBSPACE_MAX_SLOT];
    PMEMobjpool *pool[SUBSPACE_MAX_SLOT];
    uint8_t update_index[SUBSPACE_MAX_SLOT];
};

struct split_plan {
    key_type boundary;
    uint32_t update_count;
    uint32_t original_update_count;
    uint32_t boundary_slot_update;
    uint32_t new_right_slot_update;
    int boundary_aligned;
    struct nvm_index_slot boundary_slot_original;
    struct nvm_slot_update updates[2 * SUBSPACE_MAX_SLOT];
    struct nvm_index_slot logical_slots[2 * SUBSPACE_MAX_SLOT];
    struct nvm_index_slot *old_shadow[SUBSPACE_MAX_SLOT];
    struct nvm_index_slot *new_shadow[SUBSPACE_MAX_SLOT];
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
    side->pool[pos] = pmemobj_pool_by_oid(side->slot[pos]);
    side->slot_key_num[pos] = key_num;
    side->min_key[pos] = plan->updates[update_index].image.entries[0].key;
    return 0;
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
        free(plan);
        free(block_array);
        return NULL;
    }


    int block_count = 0;
    int persistent_key_count = 0;
    int split_slot = -1;
    int split_off = -1;
    int half = (int)subspace_info->key_num / 2;

    plan->update_count = subspace_info->slot_num;
    plan->original_update_count = subspace_info->slot_num;
    plan->boundary_slot_update = UINT32_MAX;
    plan->new_right_slot_update = UINT32_MAX;
    access_nvm();
    for(uint32_t i = 0; i < subspace_info->slot_num; i++){
        PMEMoid oid = subspace_info->slot[i];
        void *src = pmemobj_direct(oid);
        uint8_t slot_keys = subspace_info->slot_key_num[i];
        if(src == NULL || slot_keys == 0 || slot_keys > SLOT_MAX_ELEMENT){
            access_nvm_release();
            free(plan);
            free(block_array);
            return NULL;
        }

        plan->updates[i].slot = oid;
        memcpy(&plan->updates[i].image, src, LNF_SLOT_MIN_SIZE);
        struct slot_element *elements = plan->updates[i].image.entries;
        for(uint8_t j = 0; j < slot_keys; j++){
            if(!block_exist(elements[j].addr.block, block_array, block_count)){
                block_array[block_count++] = elements[j].addr.block;
            }
        }

        if(split_slot < 0 && persistent_key_count + slot_keys > half){
            split_slot = (int)i;
            split_off = half - persistent_key_count;
        }
        persistent_key_count += slot_keys;
    }
    access_nvm_release();

    if(split_slot < 0 || split_off < 0 ||
       persistent_key_count < half || half == 0){
        free(plan);
        free(block_array);
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
            free(plan);
            free(block_array);
            return NULL;
        }
    }

    if(split_off == 0){
        for(uint32_t i = (uint32_t)split_slot; i < subspace_info->slot_num; i++){
            if(split_side_add_slot(plan, &plan->new_side, i,
                                   subspace_info->slot_key_num[i]) != 0){
                free(plan);
                free(block_array);
                return NULL;
            }
        }
    }else{
        uint8_t original_count = subspace_info->slot_key_num[split_slot];
        uint8_t right_count = (uint8_t)(original_count - split_off);
        if(right_count == 0 ||
           !nvm_index_reserve_free_nodes(thread->index->nvm_index,
                                         &thread->index_free_head, 1)){
            free(plan);
            free(block_array);
            return NULL;
        }

        PMEMobjpool *new_pool = NULL;
        PMEMoid new_slot = nvm_index_get_free_node(
            thread->index->nvm_index, &thread->index_free_head, &new_pool);
        if(OID_IS_NULL(new_slot) ||
           plan->update_count >= 2U * SUBSPACE_MAX_SLOT){
            free(plan);
            free(block_array);
            return NULL;
        }

        uint32_t new_update = plan->update_count++;
        plan->new_right_slot_update = new_update;
        plan->updates[new_update].slot = new_slot;
        memset(&plan->updates[new_update].image, 0,
               sizeof(plan->updates[new_update].image));

        struct nvm_index_slot *left_image =
            &plan->updates[split_slot].image;
        struct nvm_index_slot *right_image =
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
            free(plan);
            free(block_array);
            return NULL;
        }

        for(uint32_t i = (uint32_t)split_slot + 1;
            i < subspace_info->slot_num; i++){
            if(split_side_add_slot(plan, &plan->new_side, i,
                                   subspace_info->slot_key_num[i]) != 0){
                free(plan);
                free(block_array);
                return NULL;
            }
        }
    }

    if(plan->old_side.slot_num == 0 || plan->new_side.slot_num == 0){
        free(plan);
        free(block_array);
        return NULL;
    }

    plan->boundary = plan->updates[
        plan->new_side.update_index[0]].image.entries[0].key;
    if(plan->boundary == LIBDB_INVALID_GROUP_BOUNDARY){
        fprintf(stderr, "split boundary collides with reserved invalid marker\n");
        free(plan);
        free(block_array);
        return NULL;
    }
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


/*
split后的block写后，需要回收block和修改索引(NVM上)
先修改索引，再释放block
*/


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
    memcpy(subspace_info->slot_key_num, side->slot_key_num,
           side->slot_num * sizeof(side->slot_key_num[0]));
    memcpy(subspace_info->min_key, side->min_key,
           side->slot_num * sizeof(side->min_key[0]));
    memcpy(subspace_info->slot, side->slot,
           side->slot_num * sizeof(side->slot[0]));
    memcpy(subspace_info->pool, side->pool,
           side->slot_num * sizeof(side->pool[0]));
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

static int split_slot_update_min_key_compare(const void *a, const void *b){
    const struct nvm_slot_update *left =
        (const struct nvm_slot_update *)a;
    const struct nvm_slot_update *right =
        (const struct nvm_slot_update *)b;
    key_type left_key = left->image.entries[0].key;
    key_type right_key = right->image.entries[0].key;
    if(left_key < right_key){
        return -1;
    }
    if(left_key > right_key){
        return 1;
    }
    return 0;
}

static int split_validate_persistent_plan(const struct split_plan *plan){
    if(plan == NULL || plan->original_update_count == 0 ||
       plan->original_update_count > plan->update_count ||
       plan->update_count > 2U * SUBSPACE_MAX_SLOT ||
       plan->boundary_slot_update >= plan->original_update_count ||
       plan->boundary == LIBDB_INVALID_GROUP_BOUNDARY ||
       plan->old_side.boundary == LIBDB_INVALID_GROUP_BOUNDARY ||
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
        if(OID_IS_NULL(plan->updates[i].slot) ||
           pmemobj_direct(plan->updates[i].slot) == NULL ||
           pmemobj_pool_by_oid(plan->updates[i].slot) == NULL){
            return -1;
        }
        for(uint32_t j = 0; j < i; j++){
            if(OID_EQUALS(plan->updates[i].slot, plan->updates[j].slot)){
                return -1;
            }
        }
    }
    return 0;
}

/*
 * Persist a split using the paper's per-slot failure protocol:
 *   internal boundary: mark s -> remaining old slots by min key
 *                      -> new right slot -> final left image of s
 *   aligned boundary:  mark s -> remaining old slots by min key
 *                      -> final right image of s
 *
 * Each call below is failure-atomic for one slot. Recovery support for the
 * distinguished marker is intentionally implemented separately.
 */
static int split_commit_persistent_index(
    struct nvm_index *nvm_index, const struct split_plan *plan){
    struct nvm_slot_update ordered[2 * SUBSPACE_MAX_SLOT];
    uint32_t ordered_count = 0;

    if(nvm_index == NULL || split_validate_persistent_plan(plan) != 0){
        fprintf(stderr, "split persistent plan validation failed\n");
        return -1;
    }

    struct nvm_slot_update marker;
    marker.slot = plan->updates[plan->boundary_slot_update].slot;
    marker.image = plan->boundary_slot_original;
    marker.image.boundary = LIBDB_INVALID_GROUP_BOUNDARY;
    if(nvm_index_commit_slot_updates(nvm_index, &marker, 1) != 0){
        fprintf(stderr, "split failed while marking boundary slot\n");
        return -1;
    }

    for(uint32_t i = 0; i < plan->original_update_count; i++){
        if(i != plan->boundary_slot_update){
            ordered[ordered_count++] = plan->updates[i];
        }
    }
    qsort(ordered, ordered_count, sizeof(ordered[0]),
          split_slot_update_min_key_compare);
    if(nvm_index_commit_slot_updates(
           nvm_index, ordered, ordered_count) != 0){
        fprintf(stderr, "split failed while committing ordered slots\n");
        return -1;
    }

    if(!plan->boundary_aligned){
        const struct nvm_slot_update *new_right =
            &plan->updates[plan->new_right_slot_update];
        if(nvm_index_commit_slot_updates(nvm_index, new_right, 1) != 0){
            fprintf(stderr, "split failed while committing new right slot\n");
            return -1;
        }
    }

    const struct nvm_slot_update *boundary_slot =
        &plan->updates[plan->boundary_slot_update];
    if(nvm_index_commit_slot_updates(nvm_index, boundary_slot, 1) != 0){
        fprintf(stderr, "split failed while finalizing boundary slot\n");
        return -1;
    }
    return 0;
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
    } else {
        ssd_write_completed(&thread->ssd_writes, SSD_WRITE_SPLIT, BLOCK_SIZE);
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
       split_commit_persistent_index(thread->index->nvm_index, plan) != 0){
        fprintf(stderr,
                "split persistent-index protocol failed; old SSD blocks were retained\n");
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
    char *new_buffer =
        (char *)calloc((size_t)old_block_count, BLOCK_SIZE);
    char *old_buffer =
        (char *)calloc((size_t)old_block_count, BLOCK_SIZE);
    int block_num_to_write[2] = {0, 0};
    if(new_buffer == NULL || old_buffer == NULL ||
       split_fill_buffers(
           new_buffer, old_buffer, plan, task->buffer,
           old_blocks, old_block_count, block_num_to_write) != 0){
        fprintf(stderr, "failed to build split output buffers\n");
        exit(EXIT_FAILURE);
    }

    memcpy(task->buffer, new_buffer,
           (size_t)block_num_to_write[0] * BLOCK_SIZE);
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
                    "failed to submit split SSD write; PMEM slots were not changed\n");
            exit(EXIT_FAILURE);
        }
    }
}


/*
split 开始时先在 DRAM 中完成 subspace metadata、write buffer 和 KV cache 的
逻辑 partition，并通过 shadow slots 继续读取旧 SSD 地址；PMEM slots 不变。
读取旧块并写完两侧新 SSD 块后，两种切分都先标记边界 slot，再更新有序原
slots；内部切分随后写新右 slot 并最终裁剪边界 slot，边界对齐时不创建新
slot，只在最后把原边界 slot 归属右孩子。
全部最终slot持久化后才撤掉shadow并释放旧块。
*/
struct subspace_info * subspace_split(struct subspace_info * subspace_info , struct thread * thread , key_type * key_out){
    uint32_t * block_array;
    int block_array_size;
    struct task * task;
    struct subspace_info * new_subspace_info;
    struct split_plan *plan = NULL;
    int qpair_excuting_once , rc;
    struct ns_entry * ns_entry = thread->ns_entry;
    int qpair_num = thread->thread % ctr_max_qp;
    /*其实有一些块是不用读的，以待优化 , block_array不是从小到大排列的*/

    block_array = block_to_read_and_prepare_split(
        subspace_info, thread, &block_array_size, key_out,
        &new_subspace_info, &plan);
    if(block_array == NULL || new_subspace_info == NULL ||
       plan == NULL || block_array_size <= 0){
        fprintf(stderr, "ERROR: failed to prepare logical subspace split\n");
        exit(EXIT_FAILURE);
    }
    task = get_free_task(ns_entry , thread->thread);
    while(task == NULL){//qpair已满
        if((qpair_excuting_once = spdk_nvme_qpair_process_completions(
                ns_entry->qpair[qpair_num], 0)) < 0){
                printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
                return 0;
        }
        task = get_free_task(ns_entry , thread->thread);
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
    /*check*/
    if(spdk_nvme_cpl_is_error(completnio)){
        task->flush_io_failed = 1;
        fprintf(stderr, "flush SSD write completion reported an error\n");
    } else {
        ssd_write_completed(&thread->ssd_writes, SSD_WRITE_FLUSH, BLOCK_SIZE);
    }
    task->block_completed ++;
    if(task->block_completed < task->total_block_num){
        return ;
    }
    if(task->flush_io_failed){
        fprintf(stderr,
                "flush aborted before slot transaction because an SSD write failed\n");
        exit(EXIT_FAILURE);
    }
    flushed_block_num = task->total_block_num;
    /*释放被缓存的键值对占用的write_buffer*/
    if(nvm_index_insert_after_flush(subspace_info, thread->index->nvm_index, task->buffered_key,
                                    (struct address_type *)task->buffered_addr, task->buffered_num,
                                    &(thread->index_free_head)) != 0){
        fprintf(stderr, "fatal: leaf-slot update failed\n");
        exit(EXIT_FAILURE);
    }

    /*
     * The item index now publishes the newly flushed SSD records.  Refresh any
     * matching cache entries before unlinking the frozen GWB index; otherwise a
     * subsequent lookup could miss the GWB and return the previously cached
     * SSD value.  The refresh is a linear merge over the ordered cache and
     * ordered flush batch and intentionally does not change LRU recency.
     */
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

    write_buffer_index_destroy(old_index);

    for(int i = 0  ; i < task->buffered_num ; i++){
        write_buffer_free(thread->kvbuffer, buffer_to_free[i].id , buffer_to_free[i].WBF_seq , buffer_to_free[i].size , &(thread->buffer_free_head));
    }
    free(task->buffer_to_free);
    free(task->buffered_addr);
    free(task->buffered_key);
    /*更新write_buffer_index*/
    free_task(task);
    /*检查更新后的subspace的容量*/
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
            subspace_info_new_subspace->is_spliting = 1;
            /*将分裂后新产生的subspace插入索引*/
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
    }//一个subspace只能由一个thread访问

    return ;
}


static uint32_t projected_slot_count_after_flush(struct subspace_info *, struct write_buffer_index *);

int flush_write_buffer_to_ssd(struct subspace_info *group, struct thread *thread,
    struct ns_entry *ns_entry, uint32_t unused_block,
    void (*cb)(void *, const struct spdk_nvme_cpl *), key_type last_insert,
    struct insert_time_breakdown *break_down){
    if(subspace_busy(group)) return 0;
    struct task *task = get_free_task(ns_entry, thread->thread);
    if(!task) return 0;
    /* Bound both DMA bytes and index growth after a long migration. */
    struct write_buffer_index *batch = write_buffer_index_take_prefix(group->buffer_index,
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
        dram_index_insert(thread->index->dram_index, fence, right, NULL);
        return 1;
    }
    size_t size = write_buffer_index_flush_size(batch, last_insert);
    uint32_t count = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t blocks[LIBDB_TASK_BUFFER_BLOCK_COUNT];
    memset(task->buffer, 0, (size_t)count * BLOCK_SIZE);
    size_t offset = 0;
    for(struct write_buffer_index_node *node = batch->head->forward[0]; node; node = node->forward[0]){
        if(offset % BLOCK_SIZE + node->size > BLOCK_SIZE) offset += BLOCK_SIZE - offset % BLOCK_SIZE;
        memcpy(task->buffer + offset, pmemobj_direct(node->id), node->size);
        offset += node->size;
        group->buffered_size -= node->size;
    }
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
    }
    return 1;
}

/*
 * 计算当前 active write buffer 被 flush 后需要的 slot 数。该计算使用与
 * nvm_index_insert_after_flush 相同的 per-slot merge 规则，并排除对已存在 key
 * 的覆盖，因此可以在写 SSD 前判断本次 flush 是否会突破 slot 上限。
 */
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
    access_nvm();
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
            access_nvm_release();
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
    access_nvm_release();

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

/*此处的value和size分别是不包括其他信息的value和value_size*/
/*在调用时需要其他信息，如版本号，size，key*/
void insert(key_type key , int size , char * value ,  struct thread * thread , struct insert_time_breakdown * break_down){
    const int size_encap = (int)kv_record_size((size_t)size);
    kv_version_type version;
    struct subspace_info * subspace_info;
    struct write_buffer_return_type buffer_to_free;
    PMEMoid id;
    uint16_t WBF_seq;
    uint32_t block;
    version = kv_record_next_thread_version(&thread->kv_version_next,
                                            &thread->kv_version_limit);
    id = write_buffer_insert(key, version, value, size, thread->kvbuffer,
                             &WBF_seq, &(thread->buffer_free_head),
                             thread->thread);
    if(OID_IS_NULL(id)){
        printf("write_buffer_insert failed id is NULL\n");
        return ;
    }
    subspace_info = hybrid_index_insert(thread->index , key , size_encap , id , WBF_seq , &buffer_to_free , thread->thread);
    if(buffer_to_free.size != 0){
        write_buffer_free(thread->kvbuffer, buffer_to_free.id , buffer_to_free.WBF_seq,  buffer_to_free.size ,&(thread->buffer_free_head));
    }

    (void)subspace_maybe_flush(subspace_info, thread);
}

/* Called only after a foreground insert/update. Never schedule a retry. */
bool subspace_maybe_flush(struct subspace_info *group, struct thread *thread){
    for(;;){
        if(group->buffered_size <= BLOCK_SIZE * Flush_Blcok_Num ||
           !space_stats_nvm_flush_allowed(thread->db) || benchmark_completed) return true;
        /* A busy group waits for its next write, not for this operation. */
        if(subspace_busy(group)) return false;
        if(!no_free_task(thread->ns_entry, thread->thread)) break;
        /* Wait before detaching a batch or reserving SSD blocks. Only drive
         * in-flight I/O, never maintenance. This idle group is worker-owned;
         * recheck its state and the policy after completion callbacks. */
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
