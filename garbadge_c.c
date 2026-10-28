#include "space_stats.h"
#include "./garbadge_c.h"
#include "merge_flush.h"
#include "kv_record.h"
#include "dram_index.h"
#include "nvm_index.h"
#include "spdk_struct.h"
#include "subspace.h"
#include "kvbuffer.h"
#include "write_buffer_index.h"
#include <stdint.h>
int block_exist(uint32_t block ,uint32_t * block_array ,int block_array_size_tmp){
    for (int i = 0; i < block_array_size_tmp; i++)
        if (block_array[i] == block)
            return i+1;
    return 0;
}

static int __redundant_comp(const void * a, const void * b) {
    return ((struct redundant_element *)b)->block_redundant_size - ((struct redundant_element *)a)->block_redundant_size;
}

static int redundant_element_upper_bound(uint32_t block,
    struct redundant_element *redundant_array, int size) {
    int l = 0, r = size;
    while (l < r) {
        int m = (l + r) / 2;
        if (redundant_array[m].block < block)
            l = m + 1;
        else
            r = m;
    }
    return l;
}

static struct redundant_element * redundant_element_find(uint32_t block,
    struct redundant_element *redundant_array, int size) {
    int pos = redundant_element_upper_bound(block, redundant_array, size);
    if (!(pos < size && redundant_array[pos].block == block)) {
        memmove(redundant_array + pos + 1, redundant_array + pos, sizeof(struct redundant_element) * (size - pos));
        redundant_array[pos].block = block;
        redundant_array[pos].block_redundant_size = BLOCK_SIZE;
    }
    return redundant_array + pos;
}


/* Private, volatile GC state. Persistent slots remain unchanged until every
 * destination is durable. A packed block number is only a staging ordinal. */
#define GC_MAX_ENTRIES (SUBSPACE_MAX_SLOT * SLOT_MAX_ELEMENT)
struct gc_entry {
    struct slot_element item;
    int packed_block;
};
struct gc_plan {
    struct slot_element old[GC_MAX_ENTRIES];
    struct gc_entry entries[GC_MAX_ENTRIES];
    uint32_t old_count, count, slot_count, update_count;
    struct nvm_slot_update updates[SUBSPACE_MAX_SLOT];
    struct write_buffer_index *frozen;
    bool returned_tail;
    uint32_t destinations[LIBDB_TASK_BUFFER_BLOCK_COUNT];
    uint32_t write_count;
};

static void gc_fatal(const char *reason){
    fprintf(stderr, "GC: %s; source blocks retained\n", reason);
    exit(EXIT_FAILURE);
}

static int gc_entry_compare(const void *a, const void *b){
    key_type x = ((const struct gc_entry *)a)->item.key;
    key_type y = ((const struct gc_entry *)b)->item.key;
    return (x > y) - (x < y);
}

/* Validate the entire prefix before detaching anything. Never skip an oversized
 * first/next record, and never consume a partial fit. */
static size_t gc_fill_prefix_count(struct gc_plan *plan,
                                  struct subspace_info *group, size_t needed){
    size_t bytes = 0, keys = 0, projected = plan->count;
    for(struct write_buffer_index_node *n = group->buffer_index->head->forward[0];
        n && bytes < needed; n = n->forward[0]){
        if(n->size > needed - bytes) return 0;
        struct kv_record_view view;
        if(kv_record_decode_for_key(pmemobj_direct(n->id), n->size, n->key, &view) ||
           view.record_size != n->size) gc_fatal("invalid PMEM fill record");
        bool exists = false;
        for(uint32_t i = 0; i < plan->count; i++)
            if(plan->entries[i].item.key == n->key) { exists = true; break; }
        if(!exists) projected++;
        if(projected > SUBSPACE_MAX_KEY || projected > GC_MAX_ENTRIES) return 0;
        bytes += n->size;
        keys++;
    }
    return bytes == needed ? keys : 0;
}

static void gc_return_record(struct thread *thread, struct subspace_info *group,
                             const struct slot_element *entry, const char *record){
    struct kv_record_view view;
    if(kv_record_decode_for_key(record, entry->addr.size, entry->key, &view) ||
       view.record_size != entry->addr.size) gc_fatal("invalid SSD tail record");
    for(struct write_buffer_index *b = group->buffer_index; b; b = b->next){
        struct write_buffer_index_node *n = write_buffer_index_search(b, entry->key);
        if(!n) continue;
        struct kv_record_view current;
        if(kv_record_decode_for_key(pmemobj_direct(n->id), n->size, n->key, &current))
            gc_fatal("invalid existing PMEM record");
        if(current.version >= view.version) return;
    }
    uint16_t seq;
    /* The normal allocator enforces the WBF hard limit and owns all accounting.
     * Do not use insert(): migration must neither assign a version nor flush. */
    PMEMoid id = write_buffer_insert(view.key, view.version, view.value,
        view.value_size, thread->kvbuffer, &seq, &thread->buffer_free_head, thread->thread);
    if(OID_IS_NULL(id)) gc_fatal("cannot persist tail in PMEM");
    struct write_buffer_return_type old = nvm_index_insert(group, id, view.key,
        view.record_size, seq, thread->thread);
    if(old.size) write_buffer_free(thread->kvbuffer, old.id, old.WBF_seq,
                                  old.size, &thread->buffer_free_head);
}

static struct gc_plan *gc_stage(struct task *task){
    struct thread *thread = task->thread;
    struct subspace_info *group = task->subspace_info;
    struct gc_plan *plan = calloc(1, sizeof(*plan));
    char *packed = calloc(LIBDB_TASK_BUFFER_BLOCK_COUNT, BLOCK_SIZE);
    if(!plan || !packed) gc_fatal("cannot allocate staging buffers");
    int block = 0;
    size_t off = 0;
    access_nvm();
    for(uint32_t i = 0; i < group->slot_num; i++){
        struct slot_element *entries = subspace_slot_elements(group, i);
        if(!entries || group->slot_key_num[i] > SLOT_MAX_ELEMENT)
            gc_fatal("invalid source slot");
        for(uint32_t j = 0; j < group->slot_key_num[i]; j++){
            if(plan->count == GC_MAX_ENTRIES) gc_fatal("source index too large");
            struct gc_entry *e = &plan->entries[plan->count++];
            e->item = entries[j];
            e->packed_block = -1;
            plan->old[plan->old_count++] = e->item;
            int source = block_exist(e->item.addr.block, task->block_array, task->total_block_num);
            if(!source) continue;
            struct kv_record_view view;
            size_t size = e->item.addr.size;
            if(size > BLOCK_SIZE || e->item.addr.off + size > BLOCK_SIZE)
                gc_fatal("record crosses source block");
            const char *record = task->buffer + (size_t)(source - 1) * BLOCK_SIZE + e->item.addr.off;
            if(kv_record_decode_for_key(record, size, e->item.key, &view) || view.record_size != size)
                gc_fatal("invalid SSD record");
            if(off + size > BLOCK_SIZE) { block++; off = 0; }
            if(block >= LIBDB_TASK_BUFFER_BLOCK_COUNT) gc_fatal("packed output exceeds DMA buffer");
            memcpy(packed + (size_t)block * BLOCK_SIZE + off, record, size);
            e->packed_block = block;
            e->item.addr.off = off;
            off += size;
        }
    }
    if(!off) gc_fatal("empty GC selection");
    if(block > 0 && off < BLOCK_SIZE){
        size_t needed = BLOCK_SIZE - off;
        size_t keys = gc_fill_prefix_count(plan, group, needed);
        if(keys){
            plan->frozen = write_buffer_index_take_prefix(group->buffer_index, keys, needed);
            if(!plan->frozen) gc_fatal("cannot freeze PMEM fill prefix");
            plan->frozen->next = group->buffer_index->next;
            if(plan->frozen->next) plan->frozen->next->prev = plan->frozen;
            plan->frozen->prev = group->buffer_index;
            group->buffer_index->next = plan->frozen;
            for(struct write_buffer_index_node *n = plan->frozen->head->forward[0]; n; n = n->forward[0]){
                uint32_t i = 0;
                while(i < plan->count && plan->entries[i].item.key != n->key) i++;
                if(i == plan->count) plan->count++;
                struct gc_entry *e = &plan->entries[i];
                e->item = (struct slot_element){.key = n->key,
                    .addr = {.off = off, .size = n->size}};
                e->packed_block = block;
                memcpy(packed + (size_t)block * BLOCK_SIZE + off, pmemobj_direct(n->id), n->size);
                off += n->size;
                group->buffered_size -= n->size;
            }
            if(off != BLOCK_SIZE) gc_fatal("incomplete PMEM fill prefix");
        } else {
            plan->returned_tail = true;
            uint32_t kept = 0;
            for(uint32_t i = 0; i < plan->count; i++){
                struct gc_entry *e = &plan->entries[i];
                if(e->packed_block == block)
                    gc_return_record(thread, group, &e->item,
                        packed + (size_t)block * BLOCK_SIZE + e->item.addr.off);
                else plan->entries[kept++] = *e;
            }
            plan->count = kept;
        }
    }
    access_nvm_release();

    /* Allocate only referenced outputs. A fill can supersede the last live
     * key in an earlier packed block, so even a non-tail block may disappear. */
    for(int b = 0; b <= block; b++){
        bool used = false;
        for(uint32_t i = 0; i < plan->count; i++)
            if(plan->entries[i].packed_block == b) { used = true; break; }
        if(!used) continue;
        int dest = get_free_block(&thread->curr_segment, thread->fs);
        if(dest < 0) gc_fatal("out of SSD space");
        plan->destinations[plan->write_count] = dest;
        memcpy(task->buffer + (size_t)plan->write_count++ * BLOCK_SIZE,
               packed + (size_t)b * BLOCK_SIZE, BLOCK_SIZE);
        for(uint32_t i = 0; i < plan->count; i++)
            if(plan->entries[i].packed_block == b) plan->entries[i].item.addr.block = dest;
    }
    free(packed);
    qsort(plan->entries, plan->count, sizeof(*plan->entries), gc_entry_compare);
    plan->slot_count = (plan->count + SLOT_MAX_ELEMENT - 1) / SLOT_MAX_ELEMENT;
    if(plan->slot_count > SUBSPACE_MAX_SLOT) gc_fatal("output index exceeds slot capacity");
    plan->update_count = plan->slot_count > group->slot_num ? plan->slot_count : group->slot_num;
    if(plan->slot_count > group->slot_num &&
       !nvm_index_reserve_free_nodes(thread->index->nvm_index, &thread->index_free_head,
                                    plan->slot_count - group->slot_num))
        gc_fatal("cannot reserve output slots");
    for(uint32_t i = 0; i < plan->update_count; i++){
        PMEMobjpool *pool;
        plan->updates[i].slot = i < group->slot_num ? group->slot[i] :
            nvm_index_get_free_node(thread->index->nvm_index, &thread->index_free_head, &pool);
        if(OID_IS_NULL(plan->updates[i].slot)) gc_fatal("cannot allocate output slot");
        if(i >= plan->slot_count) continue; /* zero retired slot */
        plan->updates[i].image.boundary = group->boundary;
        for(uint32_t j = 0; j < SLOT_MAX_ELEMENT && i * SLOT_MAX_ELEMENT + j < plan->count; j++)
            plan->updates[i].image.entries[j] = plan->entries[i * SLOT_MAX_ELEMENT + j].item;
    }
    return plan;
}

static void gc_commit(struct task *task){
    struct thread *thread = task->thread;
    struct subspace_info *group = task->subspace_info;
    struct gc_plan *plan = task->gc_plan;
    if(nvm_index_commit_slot_updates(thread->index->nvm_index, plan->updates, plan->update_count))
        gc_fatal("slot commit failed");
    for(uint32_t i = 0; i < plan->old_count; i++){
        block_ref_remove(plan->old[i].addr.block);
        kvcache_delete(group->kvcache, thread->lru_list, plan->old[i].key);
    }
    uint32_t blocks[GC_MAX_ENTRIES], count = 0;
    uint64_t bytes = 0;
    for(uint32_t i = 0; i < plan->count; i++){
        struct slot_element *e = &plan->entries[i].item;
        block_ref_add(e->addr.block);
        if(!block_exist(e->addr.block, blocks, count)) blocks[count++] = e->addr.block;
        bytes += e->addr.size;
        kvcache_delete(group->kvcache, thread->lru_list, e->key);
    }
    group->total_block_num = count;
    group->redundant_size = (uint64_t)count * BLOCK_SIZE - bytes;
    group->key_num = plan->count;
    group->slot_num = plan->slot_count;
    for(uint32_t i = 0; i < plan->update_count; i++){
        if(i >= plan->slot_count){
            nvm_index_recycle_slot(thread->index->nvm_index, &thread->index_free_head, plan->updates[i].slot);
            group->slot[i] = OID_NULL;
            group->slot_key_num[i] = 0;
            group->min_key[i] = 0;
            group->pool[i] = NULL;
        } else {
            group->slot[i] = plan->updates[i].slot;
            group->pool[i] = pmemobj_pool_by_oid(group->slot[i]);
            uint32_t remaining = plan->count - i * SLOT_MAX_ELEMENT;
            group->slot_key_num[i] = remaining < SLOT_MAX_ELEMENT ? remaining : SLOT_MAX_ELEMENT;
            group->min_key[i] = plan->updates[i].image.entries[0].key;
        }
        group->need_persist[i] = false;
    }
    if(plan->frozen){
        struct write_buffer_index *batch = plan->frozen;
        batch->prev->next = batch->next;
        if(batch->next) batch->next->prev = batch->prev;
        for(struct write_buffer_index_node *n = batch->head->forward[0]; n; n = n->forward[0])
            write_buffer_free(thread->kvbuffer, n->id, n->WBF_seq, n->size, &thread->buffer_free_head);
        write_buffer_index_destroy(batch);
    }
    /* This also reclaims unselected source blocks whose last key was replaced
     * by a PMEM fill record. Outstanding readers keep their physical blocks. */
    count = 0;
    for(uint32_t i = 0; i < plan->old_count; i++){
        uint32_t b = plan->old[i].addr.block;
        if(!g_map[b] && !block_exist(b, blocks, count)) blocks[count++] = b;
    }
    reclaim_blocks(thread, blocks, count);
    group->is_gc = false;
    if(!plan->returned_tail) maintenance_request_flush(thread, group->boundary);
    free(task->block_array);
    free(plan);
    task->gc_plan = NULL;
    free_task(task);
}

static void garbadge_collection_cb_write_cb(void *arg, const struct spdk_nvme_cpl *cpl){
    struct task *task = arg;
    struct thread *thread = task->thread;
    uint64_t start = spdk_get_ticks();
    if(spdk_nvme_cpl_is_error(cpl)) task->gc_io_failed = 1;
    if(++task->block_completed == task->total_block_num){
        if(task->gc_io_failed) gc_fatal("SSD write completion failed");
        gc_commit(task);
    }
    insert_breakdown[thread->thread].gc_cb_time += spdk_get_ticks() - start;
}

static void garbadge_collection_cb(void *arg, const struct spdk_nvme_cpl *cpl){
    struct task *task = arg;
    struct thread *thread = task->thread;
    struct ns_entry *ns = thread->ns_entry;
    uint64_t start = spdk_get_ticks();
    if(spdk_nvme_cpl_is_error(cpl)) task->gc_io_failed = 1;
    if(++task->block_completed == task->total_block_num){
        if(task->gc_io_failed) gc_fatal("SSD read completion failed");
        struct gc_plan *plan = gc_stage(task);
        task->gc_plan = plan;
        task->total_block_num = plan->write_count;
        task->block_completed = 0;
        if(!plan->write_count) gc_commit(task);
        else for(uint32_t i = 0; i < plan->write_count; i++){
            thread_block_rw[thread->thread]++;
            thread_gc_block_rw[thread->thread]++;
            thread_gc_block_write[thread->thread]++;
            if(spdk_nvme_ns_cmd_write(ns->ns, ns->qpair[thread->thread % ctr_max_qp],
                task->buffer + (size_t)i * BLOCK_SIZE,
                (uint64_t)plan->destinations[i] * physical_block_to_logical_block,
                physical_block_to_logical_block, garbadge_collection_cb_write_cb, task,
                SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS)) gc_fatal("SSD write submission failed");
        }
    }
    insert_breakdown[thread->thread].gc_cb_time += spdk_get_ticks() - start;
}

static uint32_t * block_redundant_compute_fraction(struct subspace_info * subspace_info , int * array_size ,struct thread * thread, double fraction){
    struct redundant_element *redundant_array = calloc(GC_MAX_ENTRIES, sizeof(*redundant_array));
    if(!redundant_array) gc_fatal("cannot allocate GC candidates");
    int size = 0;
    int j = 0;
    access_nvm();
    for (int i = 0; i < subspace_info->slot_num; i++) {
        struct slot_element *slot_elements =
            subspace_slot_elements(subspace_info, (uint32_t)i);
        for (int j = 0; j < subspace_info->slot_key_num[i]; j++) {
            struct address_type *addr = &slot_elements[j].addr;
            struct redundant_element * redundant_element = redundant_element_find(addr->block, redundant_array, size);
            if (redundant_element->block_redundant_size == BLOCK_SIZE)
                size++;
            redundant_element->block_redundant_size -= addr->size;
        }
    }
    access_nvm_release();

    if (subspace_info->total_block_num > size) {
        invalid_block_num[thread->thread] += subspace_info->total_block_num - size;
        // 这里 invalid_block_num 就是“索引中完全看不到”的块数量
    }

    qsort(redundant_array, size, sizeof(struct redundant_element), __redundant_comp);
    uint32_t *arr = malloc(sizeof(*arr) * (size ? size : 1));
    if(!arr) gc_fatal("cannot allocate GC selection");
    subspace_info->redundant_size = 0;
    for (int i = 0; i < size; i++) {
        if(redundant_array[i].block_redundant_size >= BLOCK_SIZE * fraction){//被选中gc
            arr[j] = redundant_array[i].block;
            j++;
        }
        else{
            subspace_info->redundant_size += redundant_array[i].block_redundant_size;//未被选中gc
        }
    }
    free(redundant_array);
    *array_size = j;
    return arr;
}
/*垃圾回收*/
uint32_t *block_redundant_compute(struct subspace_info *group, int *count, struct thread *thread) {
    return block_redundant_compute_fraction(group, count, thread, space_stats_gc_fraction(thread->db));
}
void garbadge_collection(struct subspace_info *group, struct thread *thread) {
    garbadge_collection_with_fraction(group, thread, space_stats_gc_fraction(thread->db));
}
void garbadge_collection_with_fraction(struct subspace_info * subspace_info ,struct thread * thread, double fraction){
    if(subspace_info->is_merging || subspace_info->is_spliting || subspace_info->flush_block_num) return;

    struct ns_entry * ns_entry = (struct ns_entry * )thread->ns_entry;
    uint32_t * redundant_array;
    int redundant_array_size;
    struct task * task;
    int qpair_excuting_once , rc;
    int qpair_num = thread->thread % ctr_max_qp;

    /*计算每个块的冗余度*/
    redundant_array = block_redundant_compute_fraction(subspace_info , &redundant_array_size , thread, fraction);
    if(redundant_array_size == 0){
        free(redundant_array);
        subspace_info->is_gc = false;
        return;
    }
    task = get_free_task(ns_entry , thread->thread);
    while(task == NULL){//qpair已满
        if((qpair_excuting_once = spdk_nvme_qpair_process_completions(
                ns_entry->qpair[qpair_num], 0)) < 0){
                printf("ERROR:fail to process_completions qpair[%d]\n" , qpair_num);
                return;
        }
        if(qpair_excuting_once != 0){
            task = get_free_task(ns_entry , thread->thread);
        }
    }
    task->subspace_info = subspace_info;
    task->thread = thread;
    task->block_completed = 0;
    task->gc_plan = NULL;
    task->gc_io_failed = 0;
    if(redundant_array_size > LIBDB_TASK_BUFFER_BLOCK_COUNT) gc_fatal("selection exceeds DMA buffer");
    task->total_block_num = redundant_array_size;
    task->block_array = redundant_array;
    /*选择冗余度最高的部分块*/
    /*提交读取任务*/
    for(int i = 0 ; i < redundant_array_size ; i++){
        thread_block_rw[thread->thread] ++;
        thread_gc_block_rw[thread->thread] ++;
        rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair[qpair_num], (task->buffer + i * BLOCK_SIZE),
        redundant_array[i] * physical_block_to_logical_block  ,1 *physical_block_to_logical_block , garbadge_collection_cb , task , 0);
        if (rc != 0) {
            fprintf(stderr, "ERROR: starting GC read I/O failed\n");
            exit(EXIT_FAILURE);
        }
    }
    return ;
}
