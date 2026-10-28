#include "merge_flush.h"
#include "insert.h"
#include "kv_record.h"
#include <errno.h>

struct block_pin {
    uint32_t block, refs;
    bool reclaim;
    struct block_pin *next;
};

void read_pin_blocks(struct thread *thread, const uint32_t *blocks, size_t count){
    for(size_t i = 0; i < count; i++){
        struct block_pin *p = thread->read_pins;
        while(p && p->block != blocks[i]) p = p->next;
        if(!p){
            p = calloc(1, sizeof(*p));
            if(!p) abort();
            p->block = blocks[i];
            p->next = thread->read_pins;
            thread->read_pins = p;
        }
        p->refs++;
    }
}

void read_unpin_blocks(struct thread *thread, const uint32_t *blocks, size_t count){
    for(size_t i = 0; i < count; i++){
        struct block_pin **link = &thread->read_pins;
        while(*link && (*link)->block != blocks[i]) link = &(*link)->next;
        if(!*link || !(*link)->refs) abort();
        struct block_pin *p = *link;
        if(--p->refs == 0){
            if(p->reclaim) free_block(&p->block, 1, &thread->curr_segment, thread->fs);
            *link = p->next;
            free(p);
        }
    }
}

void reclaim_blocks(struct thread *thread, const uint32_t *blocks, size_t count){
    for(size_t i = 0; i < count; i++){
        struct block_pin *p = thread->read_pins;
        while(p && p->block != blocks[i]) p = p->next;
        if(p) p->reclaim = true;
        else{
            uint32_t block = blocks[i];
            free_block(&block, 1, &thread->curr_segment, thread->fs);
        }
    }
}

bool read_cache_mapping_current(struct thread *thread, key_type key, struct address_type addr){
    struct subspace_info *group = dram_index_search(thread->index->dram_index, key, NULL, NULL);
    if(!group) return false;
    for(struct write_buffer_index *b = group->buffer_index; b; b = b->next)
        if(write_buffer_index_search(b, key)) return false;
    struct address_type current = nvm_index_search(group, key);
    return current.size && current.block == addr.block && current.off == addr.off && current.size == addr.size;
}

/* Worker-owned FIFO plus hash membership: deferral does not rescan all groups
 * on every insert, and queue entries survive pressure-free polling unchanged. */
#define DEFERRED_BUCKETS 4096U
struct deferred_flush_entry {
    key_type key;
    struct deferred_flush_entry *next, *hash_next;
};
struct deferred_flush_queue {
    struct deferred_flush_entry *head, *tail, *buckets[DEFERRED_BUCKETS];
    size_t count;
};
static unsigned deferred_bucket(key_type key){
    uint64_t h = key;
    h ^= h >> 33;
    h *= UINT64_C(0xff51afd7ed558ccd);
    h ^= h >> 33;
    return h & (DEFERRED_BUCKETS - 1);
}
void maintenance_defer_flush(struct thread *thread, key_type key){
    struct deferred_flush_queue *q = thread->deferred_flush;
    if(!q){
        q = calloc(1, sizeof(*q));
        if(!q) abort();
        thread->deferred_flush = q;
    }
    unsigned bucket = deferred_bucket(key);
    for(struct deferred_flush_entry *p = q->buckets[bucket]; p; p = p->hash_next)
        if(p->key == key) return;
    struct deferred_flush_entry *p = malloc(sizeof(*p));
    if(!p) abort();
    *p = (struct deferred_flush_entry){.key=key, .hash_next=q->buckets[bucket]};
    q->buckets[bucket] = p;
    if(q->tail) q->tail->next = p; else q->head = p;
    q->tail = p;
    q->count++;
}
void maintenance_clear_deferred(struct thread *thread){
    struct deferred_flush_queue *q = thread->deferred_flush;
    if(!q) return;
    while(q->head){
        struct deferred_flush_entry *p = q->head;
        q->head = p->next;
        free(p);
    }
    free(q);
    thread->deferred_flush = NULL;
}
static void deferred_flush_poll(struct thread *thread){
    struct deferred_flush_queue *q = thread->deferred_flush;
    if(!q || !space_stats_nvm_flush_allowed(thread->db)) return;
    size_t budget = q->count < 16 ? q->count : 16;
    for(size_t i = 0; i < budget && q->head; i++){
        if(!space_stats_nvm_flush_allowed(thread->db)) break;
        struct deferred_flush_entry *p = q->head;
        struct subspace_info *group = dram_index_search(thread->index->dram_index, p->key, NULL, NULL);
        bool done = !group || subspace_maybe_flush(group, thread);
        /* Main may have removed pressure during the call. Preserve eligibility
         * even if subspace_maybe_flush just deduplicated against this entry. */
        if(group && group->buffered_size > BLOCK_SIZE * Flush_Blcok_Num &&
           !space_stats_nvm_flush_allowed(thread->db)) done = false;
        q->head = p->next;
        if(q->tail == p) q->tail = NULL;
        if(!done){
            p->next = NULL;
            if(q->tail) q->tail->next = p; else q->head = p;
            q->tail = p;
        } else {
            struct deferred_flush_entry **link = &q->buckets[deferred_bucket(p->key)];
            while(*link != p) link = &(*link)->hash_next;
            *link = p->hash_next;
            q->count--;
            free(p);
        }
    }
}

struct pending_maintenance {
    key_type key;
    bool merge;
    struct pending_maintenance *next;
};

static void request(struct thread *thread, key_type key, bool merge){
    for(struct pending_maintenance *p = thread->maintenance; p; p = p->next)
        if(p->key == key && p->merge == merge) return;
    struct pending_maintenance *p = malloc(sizeof(*p));
    if(!p) abort();
    *p = (struct pending_maintenance){key, merge, thread->maintenance};
    thread->maintenance = p;
}

void maintenance_request_flush(struct thread *thread, key_type key){
    if(LIBDB_DYNAMIC_FLUSH_ENABLED && thread->db &&
       atomic_load_explicit(&thread->db->nvm_flush.phase, memory_order_acquire) != NVM_FLUSH_FIXED){
        struct subspace_info *group = dram_index_search(thread->index->dram_index, key, NULL, NULL);
        if(group && group->buffered_size > BLOCK_SIZE * Flush_Blcok_Num)
            maintenance_defer_flush(thread, group->boundary);
    } else request(thread, key, false);
}

/* Initial ranges are striped across workers. Never remove their fences. */
static int partition(struct thread *thread, key_type key){
    int lo = 0, hi = thread->db->boundary_size;
    key_type *fences = thread->db->boundary[thread->thread];
    while(lo + 1 < hi){
        int mid = lo + (hi - lo) / 2;
        if(fences[mid] <= key) lo = mid;
        else hi = mid;
    }
    return lo;
}

bool merge_same_partition(struct thread *thread, key_type left, key_type right){
    return left < right && key_to_thread(left, thread->db) == thread->thread &&
           key_to_thread(right, thread->db) == thread->thread &&
           partition(thread, left) == partition(thread, right);
}

struct merge_plan {
    struct thread *thread;
    struct task *task;
    struct subspace_info *group;
    uint32_t old_slot_count, slot_count, key_count;
    uint32_t source_count, dest_count, completed;
    bool failed;
    PMEMoid old_slots[2 * SUBSPACE_MAX_SLOT];
    struct nvm_index_slot logical[SUBSPACE_MAX_SLOT];
    struct nvm_index_slot *shadow[SUBSPACE_MAX_SLOT];
    struct nvm_slot_update updates[2 * SUBSPACE_MAX_SLOT];
    uint32_t sources[SUBSPACE_MAX_KEY], destinations[SUBSPACE_MAX_KEY];
    char *packed;
};

static void merge_fatal(const char *reason){
    fprintf(stderr, "merge: %s; source blocks retained\n", reason);
    exit(EXIT_FAILURE);
}

static void merge_commit(struct merge_plan *plan){
    struct thread *thread = plan->thread;
    struct subspace_info *group = plan->group;
    if(nvm_index_commit_slot_updates(thread->index->nvm_index, plan->updates, plan->old_slot_count) != 0)
        merge_fatal("persistent item-index commit failed");
    /* All destinations and all final/retired slot images are now durable. */
    for(uint32_t i = plan->slot_count; i < plan->old_slot_count; i++)
        nvm_index_recycle_slot(thread->index->nvm_index, &thread->index_free_head, plan->old_slots[i]);
    for(uint32_t i = 0; i < plan->slot_count; i++)
        for(uint32_t j = 0; j < group->slot_key_num[i]; j++)
            block_ref_add(plan->updates[i].image.entries[j].addr.block);
    group->migration_slot_shadow = NULL;
    group->total_block_num = plan->dest_count;
    group->redundant_size = 0;
    group->is_merging = false;
    reclaim_blocks(thread, plan->sources, plan->source_count);
    thread->merge_count++;
    maintenance_request_flush(thread, group->boundary);
    spdk_free(plan->packed);
    free_task(plan->task);
    free(plan);
}

static void merge_write_cb(void *arg, const struct spdk_nvme_cpl *cpl){
    struct merge_plan *plan = arg;
    if(spdk_nvme_cpl_is_error(cpl)) plan->failed = true;
    if(++plan->completed != plan->dest_count) return;
    if(plan->failed) merge_fatal("SSD write failed");
    merge_commit(plan);
}

static void merge_read_cb(void *arg, const struct spdk_nvme_cpl *cpl){
    struct merge_plan *plan = arg;
    struct thread *thread = plan->thread;
    if(spdk_nvme_cpl_is_error(cpl)) plan->failed = true;
    if(++plan->completed != plan->source_count) return;
    if(plan->failed) merge_fatal("SSD read failed");
    size_t offset = 0;
    for(uint32_t i = 0; i < plan->slot_count; i++){
        for(uint32_t j = 0; j < plan->group->slot_key_num[i]; j++){
            struct slot_element *entry = &plan->updates[i].image.entries[j];
            struct address_type old = entry->addr;
            uint32_t src = 0;
            while(src < plan->source_count && plan->sources[src] != old.block) src++;
            if(src == plan->source_count || old.size > BLOCK_SIZE || old.off + old.size > BLOCK_SIZE)
                merge_fatal("invalid source address");
            const char *record = plan->task->buffer + (size_t)src * BLOCK_SIZE + old.off;
            struct kv_record_view view;
            if(kv_record_decode_for_key(record, old.size, entry->key, &view) != 0)
                merge_fatal("invalid source record");
            if(offset % BLOCK_SIZE + old.size > BLOCK_SIZE) offset += BLOCK_SIZE - offset % BLOCK_SIZE;
            memcpy(plan->packed + offset, record, old.size);
            entry->addr.block = plan->destinations[offset / BLOCK_SIZE];
            entry->addr.off = offset % BLOCK_SIZE;
            offset += old.size;
        }
    }
    plan->completed = 0;
    for(uint32_t i = 0; i < plan->dest_count; i++){
        thread_block_rw[thread->thread]++;
        thread->merge_write_blocks++;
        int rc = spdk_nvme_ns_cmd_write(thread->ns_entry->ns,
            thread->ns_entry->qpair[thread->thread % ctr_max_qp],
            plan->packed + (size_t)i * BLOCK_SIZE,
            (uint64_t)plan->destinations[i] * physical_block_to_logical_block,
            physical_block_to_logical_block, merge_write_cb, plan,
            SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
        if(rc) merge_fatal("cannot submit SSD write");
    }
}

/* No completion polling before publication: both candidates stay stable. */
static bool merge_start(struct thread *thread, struct subspace_info *left, struct subspace_info *right){
    struct task *task = get_free_task(thread->ns_entry, thread->thread);
    if(!task) return false;
    struct merge_plan *plan = calloc(1, sizeof(*plan));
    if(!plan){ free_task(task); return false; }
    plan->thread = thread;
    plan->task = task;
    plan->group = left;
    uint32_t keys = 0;
    size_t bytes = 0;
    struct subspace_info *groups[2] = {left, right};
    for(int g = 0; g < 2; g++){
        for(uint32_t i = 0; i < groups[g]->slot_num; i++){
            plan->old_slots[plan->old_slot_count++] = groups[g]->slot[i];
            struct slot_element *entries = subspace_slot_elements(groups[g], i);
            for(uint32_t j = 0; j < groups[g]->slot_key_num[i]; j++){
                if(keys >= SUBSPACE_MAX_KEY || entries[j].addr.size <= 0 ||
                   entries[j].addr.size > BLOCK_SIZE || entries[j].addr.off < 0 ||
                   entries[j].addr.off + entries[j].addr.size > BLOCK_SIZE) goto cancel;
                plan->logical[keys / SLOT_MAX_ELEMENT].entries[keys % SLOT_MAX_ELEMENT] = entries[j];
                keys++;
                uint32_t b = entries[j].addr.block, k = 0;
                while(k < plan->source_count && plan->sources[k] != b) k++;
                if(k == plan->source_count) plan->sources[plan->source_count++] = b;
                if(bytes % BLOCK_SIZE + entries[j].addr.size > BLOCK_SIZE) bytes += BLOCK_SIZE - bytes % BLOCK_SIZE;
                bytes += entries[j].addr.size;
            }
        }
    }
    plan->key_count = keys;
    plan->slot_count = (keys + SLOT_MAX_ELEMENT - 1) / SLOT_MAX_ELEMENT;
    plan->dest_count = (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if(plan->source_count > LIBDB_TASK_BUFFER_BLOCK_COUNT || plan->dest_count > LIBDB_TASK_BUFFER_BLOCK_COUNT) goto cancel;
    /* DMA buffer and every destination are allocated before the fence moves. */
    if(plan->dest_count){
        plan->packed = spdk_zmalloc((size_t)plan->dest_count * BLOCK_SIZE, BLOCK_SIZE, NULL,
                                   SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        if(!plan->packed) goto cancel;
    }
    for(uint32_t i = 0; i < plan->dest_count; i++){
        int block = get_free_block(&thread->curr_segment, thread->fs);
        if(block < 0){
            reclaim_blocks(thread, plan->destinations, i);
            goto cancel;
        }
        plan->destinations[i] = block;
    }
    for(uint32_t i = 0; i < plan->old_slot_count; i++){
        plan->updates[i].slot = plan->old_slots[i];
        if(i < plan->slot_count){
            plan->logical[i].boundary = left->boundary;
            plan->updates[i].image = plan->logical[i];
            plan->shadow[i] = &plan->logical[i];
        }
    }
    left->is_merging = right->is_merging = true;
    write_buffer_index_merge(left->buffer_index, right->buffer_index);
    write_buffer_index_destroy(right->buffer_index);
    right->buffer_index = NULL;
    kvcache_merge(left->kvcache, right->kvcache);
    kvcache_destroy_empty(right->kvcache);
    right->kvcache = NULL;
    left->buffered_size += right->buffered_size;
    left->total_block_num += right->total_block_num;
    left->redundant_size += right->redundant_size;
    left->key_num = keys;
    left->slot_num = plan->slot_count;
    memset(left->slot, 0, sizeof(left->slot));
    memset(left->pool, 0, sizeof(left->pool));
    memset(left->slot_key_num, 0, sizeof(left->slot_key_num));
    memset(left->min_key, 0, sizeof(left->min_key));
    memset(left->need_persist, 0, sizeof(left->need_persist));
    for(uint32_t i = 0; i < plan->slot_count; i++){
        left->slot[i] = plan->old_slots[i];
        left->pool[i] = pmemobj_pool_by_oid(left->slot[i]);
        left->slot_key_num[i] = keys - i * SLOT_MAX_ELEMENT < SLOT_MAX_ELEMENT ? keys - i * SLOT_MAX_ELEMENT : SLOT_MAX_ELEMENT;
        left->min_key[i] = plan->logical[i].entries[0].key;
    }
    left->migration_slot_shadow = plan->shadow;
    if(!dram_index_remove(thread->index->dram_index, right->boundary)) abort();
    thread_subspaces[thread->thread]--;
    subspace_ref(right); /* temporary reference while retiring it */
    subspace_ref(left);  /* redirect keeps successor alive */
    right->redirect = left;
    subspace_unref(right);
    if(!plan->source_count){ merge_commit(plan); return true; }
    for(uint32_t i = 0; i < plan->source_count; i++){
        thread_block_rw[thread->thread]++;
        thread->merge_read_blocks++;
        int rc = spdk_nvme_ns_cmd_read(thread->ns_entry->ns,
            thread->ns_entry->qpair[thread->thread % ctr_max_qp],
            task->buffer + (size_t)i * BLOCK_SIZE,
            (uint64_t)plan->sources[i] * physical_block_to_logical_block,
            physical_block_to_logical_block, merge_read_cb, plan, 0);
        if(rc) merge_fatal("cannot submit SSD read");
    }
    return true;
cancel:
    spdk_free(plan->packed);
    free(plan);
    free_task(task);
    return false;
}

/* true means consumed; false asks the owner worker to retry after I/O. */
static bool merge_try(struct thread *thread, key_type key){
    struct subspace_info *group = dram_index_search(thread->index->dram_index, key, NULL, NULL);
    if(!group) return true;
    if(subspace_busy(group)) return false;
    struct subspace_info *best = NULL;
    uint32_t best_sum = UINT32_MAX, count = subspace_live_keys(group);
    bool busy_neighbor = false;
    for(int direction = -1; direction <= 1; direction += 2){
        struct subspace_info *other = dram_index_neighbor(thread->index->dram_index, group->boundary, direction);
        if(!other) continue;
        key_type left = direction < 0 ? other->boundary : group->boundary;
        key_type right = direction < 0 ? group->boundary : other->boundary;
        if(!merge_same_partition(thread, left, right)) continue;
        if(subspace_busy(other)){ busy_neighbor = true; continue; }
        uint32_t n = subspace_live_keys(other);
        if(count >= LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD && n >= LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD) continue;
        uint64_t sum = (uint64_t)count + n;
        if(sum <= SUBSPACE_MAX_KEY && sum < best_sum){ best = other; best_sum = sum; }
    }
    if(!best) return !busy_neighbor;
    return best->boundary < group->boundary ? merge_start(thread, best, group) : merge_start(thread, group, best);
}

void merge_check_after_delete(struct thread *thread, key_type key){
    if(!merge_try(thread, key)) request(thread, key, true);
}

void maintenance_poll(struct thread *thread){
    if(thread->servicing_maintenance) return;
    thread->servicing_maintenance = true;
    struct pending_maintenance *batch = thread->maintenance;
    thread->maintenance = NULL;
    while(batch){
        struct pending_maintenance *p = batch;
        batch = p->next;
        bool done;
        if(p->merge) done = merge_try(thread, p->key);
        else{
            struct subspace_info *group = dram_index_search(thread->index->dram_index, p->key, NULL, NULL);
            done = !group || subspace_maybe_flush(group, thread);
        }
        if(!done){
            if(p->merge) request(thread, p->key, true);
            else maintenance_request_flush(thread, p->key);
        }
        free(p);
    }
    deferred_flush_poll(thread);
    thread->servicing_maintenance = false;
}
