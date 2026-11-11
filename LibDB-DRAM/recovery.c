#include "recovery.h"
#include "insert.h"
#include "kv_record.h"
#include "wal.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <sched.h>


struct avl { struct avl *left, *right; key_type key; int height; };
static int height(struct avl *n) { return n ? n->height : 0; }
static void fix_height(struct avl *n) {
    int a = height(n->left), b = height(n->right); n->height = 1 + (a > b ? a : b);
}
static struct avl *rotate_right(struct avl *n) {
    struct avl *p = n->left; n->left = p->right; p->right = n;
    fix_height(n); fix_height(p); return p;
}
static struct avl *rotate_left(struct avl *n) {
    struct avl *p = n->right; n->right = p->left; p->left = n;
    fix_height(n); fix_height(p); return p;
}
static struct avl *balance(struct avl *n) {
    fix_height(n);
    if(height(n->left) - height(n->right) > 1) {
        if(height(n->left->right) > height(n->left->left)) n->left = rotate_left(n->left);
        return rotate_right(n);
    }
    if(height(n->right) - height(n->left) > 1) {
        if(height(n->right->left) > height(n->right->right)) n->right = rotate_right(n->right);
        return rotate_left(n);
    }
    return n;
}
static struct avl *avl_insert(struct avl *root, struct avl *n) {
    if(!root) { n->height = 1; return n; }
    if(n->key < root->key) root->left = avl_insert(root->left, n);
    else root->right = avl_insert(root->right, n);
    return balance(root);
}
static struct avl *detach_min(struct avl *root, struct avl **out) {
    if(!root->left) { *out = root; return root->right; }
    root->left = detach_min(root->left, out); return balance(root);
}
static struct avl *avl_remove(struct avl *root, key_type key) {
    if(key < root->key) root->left = avl_remove(root->left, key);
    else if(key > root->key) root->right = avl_remove(root->right, key);
    else {
        struct avl *a = root->left, *b = root->right, *next;
        root->left = root->right = NULL;
        if(!b) return a;
        b = detach_min(b, &next); next->left = a; next->right = b;
        return balance(next);
    }
    return balance(root);
}
static struct avl *avl_find(struct avl *n, key_type key) {
    while(n && key != n->key) n = key < n->key ? n->left : n->right;
    return n;
}
static struct avl *avl_bound(struct avl *n, key_type key, bool floor) {
    struct avl *best = NULL;
    while(n) {
        if(n->key == key) return n;
        if((floor && n->key < key) || (!floor && n->key > key)) {
            best = n; n = floor ? n->right : n->left;
        } else n = floor ? n->left : n->right;
    }
    return best;
}


struct record {
    struct avl order;
    struct block *source;
    key_type key;
    uint64_t version, generation, lsn;
    struct address_type addr;
    unsigned char prefix[LIBDB_VALID_VALUE_PREFIX_SIZE];
    uint32_t value_size, segment, wal_worker;
    bool wal;
    struct record *block_prev, *block_next;
};

struct recovery_slot {
    struct avl order;
    struct recovery_slot *prev, *next;
    struct index_slot *slot;
    unsigned count;
};
struct table { struct avl *root; size_t count; };

struct recovery_buffer {
    struct write_buffer_index *index;
    size_t keys;
};
struct component;
struct block {
    key_type min, max;
    size_t count;
    int partition;
    struct record *winner;
    struct component *component;
    struct block *next, *allocated_next;
    struct subspace_info *group;
};
struct component {
    struct avl order;
    key_type max;
    bool dirty;
    struct component *parent, *allocated_next;
    struct block *head, *tail;
    struct recovery_buffer buffer;
};
struct block_batch {
    struct transfer *transfer;
    uint32_t *offsets;
    uint32_t count;
};
struct transfer {
    struct block_batch *batches;
    uint32_t *offsets;

    unsigned references;
    unsigned char *data;
    uint64_t begin;
    uint32_t count, completed, submitted, segment, owner, retries;
    bool wal, busy, in_flight, io_failed;
};
#ifdef LIBDB_RECOVERY_BATCH_TEST

extern void recovery_test_batch_dispatched(uint64_t, unsigned, const void *, const uint32_t *, uint32_t,
                                           const atomic_bool *);
extern void recovery_test_batch_returned(uint64_t, unsigned, const void *, unsigned);
extern void recovery_test_incremental(unsigned, bool, uint64_t, void *, struct index_slot *, unsigned);
#endif

struct queue {
    struct block_batch **items;
    size_t capacity;
    atomic_size_t head, tail;
    size_t peak;
};
struct launch_state { atomic_uint ready; atomic_int decision; };
struct scan_state {
    atomic_bool aborted, finished;
    uint64_t cutoff, first_empty, data_read_bytes, wal_read_bytes, data_segments;
    double data_seconds, io_seconds;
    struct job *jobs;
    struct transfer *pool;
    unsigned pool_count, in_use, peak_buffers;
    uint32_t max_blocks;
};
struct job {
    struct db *db;
    unsigned id;
    uint32_t core;
    const char *stage;
    void *(*run)(void *);
    atomic_int failed;
    struct launch_state *launch;
    struct scan_state *scan;
    struct queue input, returned;
    uint64_t data_blocks, data_batches, wal_segments, recomputed_groups;
    double processing_seconds, ssd_processing_seconds, wal_processing_seconds;
    struct avl **components;
    struct component *allocated_components;
    struct block *allocated_blocks;
    size_t block_count;
    struct table table;
    struct avl *slot_tree;
    struct recovery_slot *slot_head;
    struct recovery_buffer *empty_buffers;
    uint64_t ssd_index_updates, buffer_updates, buffer_removals;
    uint64_t wal_buffer_groups, max_wal_buffer_keys, buffer_search_steps;
    double group_seconds, buffer_bind_seconds;
    size_t count;
    uint64_t *generations;
    uint64_t max_version, max_lsn, max_sequence;
    uint32_t tail;
    uint64_t parsed, stopped, malformed_wal, batches, first_stop;
    unsigned first_offset, first_nonzero_offset;
    uint64_t nonzero_stops, first_nonzero_stop;
    struct subspace_info **groups;
    size_t group_count, group_capacity;
    struct dram_index *index;
};
static int error(struct job *j, const char *message) {
    if(!atomic_exchange(&j->failed, 1)) fprintf(stderr, "recovery worker %u: %s\n", j->id, message);
    if(j->scan) atomic_store(&j->scan->aborted, true);
    return -1;
}
static uint64_t hash(key_type key) {
    key ^= key >> 30; key *= UINT64_C(0xbf58476d1ce4e5b9);
    key ^= key >> 27; key *= UINT64_C(0x94d049bb133111eb);
    return key ^ (key >> 31);
}
static int partition(struct job *j, key_type key);
static struct component *component_root(struct component *c) {
    struct component *root = c;
    while(root->parent) root = root->parent;
    while(c->parent) { struct component *next = c->parent; c->parent = root; c = next; }
    return root;
}
static int add_block(struct job *j, struct block *b) {
    struct component *c = calloc(1, sizeof(*c));
    if(!c) return error(j, "temporary group allocation failed");
    c->order.key = b->min; c->max = b->max; c->head = c->tail = b; b->component = c;
    c->allocated_next = j->allocated_components; j->allocated_components = c;
    struct avl **tree = &j->components[b->partition];
    struct component *other = (struct component *)avl_bound(*tree, b->min, true);
    if(!other || other->max < b->min) other = (struct component *)avl_bound(*tree, b->min, false);
    while(other && other->order.key <= c->max) {
        *tree = avl_remove(*tree, other->order.key);
        if(other->order.key < c->order.key) c->order.key = other->order.key;
        if(other->max > c->max) c->max = other->max;
        c->dirty |= other->dirty;
        c->tail->next = other->head; c->tail = other->tail;
        other->parent = c; other->head = other->tail = NULL;
        other = (struct component *)avl_bound(*tree, c->order.key, false);
    }
    *tree = avl_insert(*tree, &c->order);
    return 0;
}
static struct record *lookup(struct table *t, key_type key) {
    return (struct record *)avl_find(t->root, key);
}


static int stage_ssd(struct job *j, const struct record *r) {
    struct slot_allocator *allocator = &j->db->workers[j->id].slots;
    struct recovery_slot *leaf = (struct recovery_slot *)avl_bound(j->slot_tree, r->key, true);
    if(!leaf) leaf = j->slot_head;
    if(!leaf) {
        leaf = calloc(1, sizeof(*leaf));
        if(!leaf) return error(j, "incremental slot metadata allocation failed");
        leaf->slot = slot_index_allocate(allocator);
        if(!leaf->slot) { free(leaf); return error(j, "incremental slot allocation failed"); }
        leaf->order.key = r->key;
        j->slot_head = leaf; j->slot_tree = avl_insert(j->slot_tree, &leaf->order);
    }
    unsigned at = 0;
    while(at < leaf->count && leaf->slot->entries[at].key < r->key) at++;
    if(at < leaf->count && leaf->slot->entries[at].key == r->key) {
        leaf->slot->entries[at].addr = r->addr; j->ssd_index_updates++; return 0;
    }
    if(leaf->count == SLOT_MAX_ELEMENT) {
        struct recovery_slot *right = calloc(1, sizeof(*right));
        if(!right) return error(j, "incremental slot split allocation failed");
        right->slot = slot_index_allocate(allocator);
        if(!right->slot) { free(right); return error(j, "incremental slot split allocation failed"); }
        unsigned left_count = leaf->count / 2;
        right->count = leaf->count - left_count;
        memcpy(right->slot->entries, leaf->slot->entries + left_count,
               right->count * sizeof(*right->slot->entries));
        leaf->count = left_count;
        right->order.key = right->slot->entries[0].key;
        right->next = leaf->next; right->prev = leaf;
        if(right->next) right->next->prev = right;
        leaf->next = right;
        j->slot_tree = avl_insert(j->slot_tree, &right->order);
        if(r->key >= right->order.key) { leaf = right; at -= left_count; }
    }
    memmove(leaf->slot->entries + at + 1, leaf->slot->entries + at,
            (leaf->count - at) * sizeof(*leaf->slot->entries));
    leaf->slot->entries[at] = (struct slot_element){r->key, r->addr}; leaf->count++;
    if(r->key < leaf->order.key) {
        j->slot_tree = avl_remove(j->slot_tree, leaf->order.key);
        leaf->order.key = r->key; j->slot_tree = avl_insert(j->slot_tree, &leaf->order);
    }
    j->ssd_index_updates++;
    return 0;
}
static void remove_staged_ssd(struct job *j, key_type key) {
    struct recovery_slot *leaf = (struct recovery_slot *)avl_bound(j->slot_tree, key, true);
    unsigned at = 0;
    while(leaf->slot->entries[at].key != key) at++;
    memmove(leaf->slot->entries + at, leaf->slot->entries + at + 1,
            (--leaf->count - at) * sizeof(*leaf->slot->entries));
    if(!at) {
        j->slot_tree = avl_remove(j->slot_tree, leaf->order.key);
        if(leaf->count) {
            leaf->order.key = leaf->slot->entries[0].key;
            j->slot_tree = avl_insert(j->slot_tree, &leaf->order);
        } else {
            if(leaf->prev) leaf->prev->next = leaf->next; else j->slot_head = leaf->next;
            if(leaf->next) leaf->next->prev = leaf->prev;
            slot_index_free(&j->db->workers[j->id].slots, leaf->slot); free(leaf);
        }
    }
}
static unsigned buffer_level(key_type key) {
    unsigned level = 0;
    uint64_t bits = hash(key);
    while(level + 1 < SKIPLIST_RESERVE_LEVEL && (bits & 3) == 0) { level++; bits >>= 2; }
    return level;
}



static struct recovery_buffer *buffer_for_key(struct job *j, key_type key) {
    int part = partition(j, key);
    struct avl *root = j->components[part];
    if(!root) return &j->empty_buffers[part];
    struct avl *owner = avl_bound(root, key, true);
    if(!owner) { owner = root; while(owner->left) owner = owner->left; }
    return &((struct component *)owner)->buffer;
}
#ifdef LIBDB_RECOVERY_BATCH_TEST
struct write_buffer_index *recovery_test_buffer_for_key(void *context, key_type key) {
    return buffer_for_key(context, key)->index;
}
#endif
static void buffer_predecessors(struct job *j, struct write_buffer_index *buffer, key_type key,
                                 struct write_buffer_index_node **before) {
    struct write_buffer_index_node *node = buffer->head;
    for(int l = SKIPLIST_RESERVE_LEVEL - 1; l >= 0; l--) {
        while(node->forward[l] && node->forward[l]->key < key) {
            node = node->forward[l]; j->buffer_search_steps++;
        }
        before[l] = node;
    }
}
static void remove_staged_buffer(struct job *j, key_type key) {
    struct recovery_buffer *buffer = buffer_for_key(j, key);
    struct write_buffer_index_node *before[SKIPLIST_RESERVE_LEVEL];
    buffer_predecessors(j, buffer->index, key, before);
    struct write_buffer_index_node *node = before[0]->forward[0];
    for(unsigned l = 0; l < SKIPLIST_RESERVE_LEVEL; l++)
        if(before[l]->forward[l] == node) before[l]->forward[l] = node->forward[l];
    wal_release_dependencies(node);
    write_buffer_free(node->record, node->size, &j->db->workers[j->id].kvbuffer);
    free(node->forward); free(node); buffer->keys--; j->buffer_removals++;
}
static int stage_buffer(struct job *j, const struct record *r) {
    struct thread *t = &j->db->workers[j->id];
    struct recovery_buffer *buffer = buffer_for_key(j, r->key);
    if(!buffer->index) {
        struct write_buffer_index *index = calloc(1, sizeof(*index));
        if(!index || write_buffer_index_try_init(index)) {
            free(index); return error(j, "temporary group buffer allocation failed");
        }
        buffer->index = index; j->wal_buffer_groups++;
    }
    struct write_buffer_index_node *before[SKIPLIST_RESERVE_LEVEL];
    buffer_predecessors(j, buffer->index, r->key, before);
    struct write_buffer_index_node *node = before[0]->forward[0];
    bool replace = node && node->key == r->key;
    size_t size = kv_record_size(r->value_size), charge = write_buffer_allocation_size(size);
    size_t old_size = replace ? node->size : 0;
    size_t old_charge = replace ? write_buffer_allocation_size(old_size) : 0;
    struct wal_segment *seg = &t->wal->segments[r->segment];
    if(!charge || t->kvbuffer.live_bytes - old_size > UINT64_MAX - size ||
       t->kvbuffer.slot_bytes - old_charge > UINT64_MAX - charge || seg->pending == UINT64_MAX)
        return error(j, "recovered buffer accounting overflow");


    struct wal_dependency *d = calloc(1, sizeof(*d));
    char *value = malloc(charge);
    if(!replace) {
        node = calloc(1, sizeof(*node));
        if(node) node->forward = calloc(SKIPLIST_RESERVE_LEVEL, sizeof(*node->forward));
    }
    if(!node || !node->forward || !d || !value) {
        if(!replace && node) { free(node->forward); free(node); }
        free(d); free(value); return error(j, "incremental buffer allocation failed");
    }
    if(kv_record_encode(value, size, r->key, r->version, (const char *)r->prefix, r->value_size)) abort();
    if(replace) {
        wal_release_dependencies(node);
        write_buffer_free(node->record, node->size, &t->kvbuffer);
        j->buffer_removals++;
    } else {
        unsigned level = buffer_level(r->key);
        for(unsigned l = 0; l <= level; l++) { node->forward[l] = before[l]->forward[l]; before[l]->forward[l] = node; }
        if(buffer->index->levels <= level) buffer->index->levels = level + 1;
        if(++buffer->keys > j->max_wal_buffer_keys) j->max_wal_buffer_keys = buffer->keys;
    }
    node->key = r->key; node->size = size; node->record = value;
    t->kvbuffer.live_bytes += size; t->kvbuffer.slot_bytes += charge;
    *d = (struct wal_dependency){.segment = seg, .generation = r->generation, .count = 1,
        .key = r->key, .owner = node, .seg_next = seg->dependencies};
    if(d->seg_next) d->seg_next->seg_prev = d;
    seg->dependencies = d; seg->pending++; node->wal_dependencies = d;
    j->buffer_updates++;
    return 0;
}
static void unlink_winner(struct record *r) {
    struct block *b = r->source;
    if(r->block_prev) r->block_prev->block_next = r->block_next; else b->winner = r->block_next;
    if(r->block_next) r->block_next->block_prev = r->block_prev;
    if(!--b->count && b->component) component_root(b->component)->dirty = true;
    block_ref_remove(r->addr.block);
}
static int select_record(struct job *j, struct table *t, const struct record *r) {
    struct record *old = lookup(t, r->key);
    if(old) {
        if(old->version > r->version) return 0;
        if(old->version == r->version) {
            if(old->value_size != r->value_size || memcmp(old->prefix, r->prefix, sizeof(r->prefix)))
                return error(j, "equal-version records disagree");
            if(!old->wal && (r->wal || old->addr.block < r->addr.block ||
               (old->addr.block == r->addr.block && old->addr.off <= r->addr.off))) return 0;
            if(old->wal && r->wal && old->lsn >= r->lsn) return 0;
        }
    }

    struct record *created = old ? NULL : malloc(sizeof(*created));
    if(!old && !created) return error(j, "winner AVL allocation failed");
    if(r->wal ? stage_buffer(j, r) : stage_ssd(j, r)) { free(created); return -1; }
    if(old) {
        if(old->wal && !r->wal) remove_staged_buffer(j, old->key);
        if(old->source) { if(r->wal) remove_staged_ssd(j, old->key); unlink_winner(old); }
        struct avl order = old->order;
        *old = *r; old->order = order;
    } else {
        old = created; *old = *r; old->order = (struct avl){.key = r->key};
        t->root = avl_insert(t->root, &old->order); t->count++;
    }
    if(r->source) {
        old->block_next = r->source->winner;
        if(old->block_next) old->block_next->block_prev = old;
        r->source->winner = old; r->source->count++;
        block_ref_add(r->addr.block);
    }
    return 0;
}
static void free_records(struct avl *node) {
    if(!node) return;
    free_records(node->left); free_records(node->right); free(node);
}
static int scan_block(struct job *j, const unsigned char *data, uint32_t block) {
    struct record records[BLOCK_SIZE / KV_RECORD_ENCODED_SIZE];
    size_t count = 0, off = 0;
    key_type min = KEY_MAX, max = 0;
    while(BLOCK_SIZE - off >= KV_RECORD_ENCODED_SIZE) {
        struct kv_record_view v;
        if(kv_record_decode((const char *)data + off, BLOCK_SIZE - off, &v) || !v.version) break;
        struct record *r = &records[count++];
        *r = (struct record){.key = v.key, .version = v.version, .value_size = v.value_size,
            .addr = {block, (int)off, (int)v.record_size, 0}};
        memcpy(r->prefix, v.value, sizeof(r->prefix));
        if(v.key < min) min = v.key;
        if(v.key > max) max = v.key;
        if(v.version > j->max_version) j->max_version = v.version;
        off += v.record_size;
    }


    if(off != BLOCK_SIZE) {
        if(!j->stopped) { j->first_stop = block; j->first_offset = off; }
        j->stopped++;
        for(size_t i = off; i < BLOCK_SIZE; i++) if(data[i]) {
            if(!j->nonzero_stops) { j->first_nonzero_stop = block; j->first_nonzero_offset = off; }
            j->nonzero_stops++;
            break;
        }
    }
    j->parsed += count;
    if(!count) return 0;
    struct block *source = calloc(1, sizeof(*source));
    if(!source) return error(j, "block metadata allocation failed");
    *source = (struct block){.min = min, .max = max,
                            .partition = partition(j, records[0].key)};
    source->allocated_next = j->allocated_blocks; j->allocated_blocks = source;
    j->block_count++;
    for(size_t i = 0; i < count; i++) {
        records[i].source = source;
        if(select_record(j, &j->table, &records[i])) return -1;
    }
    if(source->count) return add_block(j, source);
    j->allocated_blocks = source->allocated_next; j->block_count--; free(source);
    return 0;
}
static int scan_wal(struct job *j, unsigned char *buffer, uint32_t seg) {


    for(size_t off = 0; off < WAL_SEGMENT_BYTES; off += BLOCK_SIZE) {
        struct wal_recovery_batch b;
        int rc = wal_recovery_header(buffer + off, WAL_SEGMENT_BYTES - off, j->id, seg, &b);
        if(rc == -2) return error(j, "WAL format or layout identity mismatch");
        if(rc) continue;
        if(b.generation > j->generations[seg]) j->generations[seg] = b.generation;
        if(b.sequence > j->max_sequence) { j->max_sequence = b.sequence; j->tail = seg; }
        if(!wal_recovery_decode(buffer + off, WAL_SEGMENT_BYTES - off, j->id, seg, &b)) {
            if(b.max_lsn > j->max_lsn) j->max_lsn = b.max_lsn;
            if(b.max_version > j->max_version) j->max_version = b.max_version;
            off += b.bytes - BLOCK_SIZE;
        }
    }


    j->db->workers[j->id].wal->segments[seg].generation = j->generations[seg];
    uint64_t previous_sequence = 0, previous_lsn = 0;
    for(size_t off = 0; off < WAL_SEGMENT_BYTES; off += BLOCK_SIZE) {
        struct wal_recovery_batch b;
        if(wal_recovery_decode(buffer + off, WAL_SEGMENT_BYTES - off, j->id, seg, &b)) {
            if(!memcmp(buffer + off, "LDBWAL02", 8)) j->malformed_wal++;
            continue;
        }
        if(b.generation == j->generations[seg]) {
            if(b.sequence <= previous_sequence) return error(j, "nonmonotonic WAL batch sequence");
            previous_sequence = b.sequence;
            size_t pos = off + WAL_HEADER_BYTES;
            for(uint32_t i = 0; i < b.count; i++) {
                struct wal_recovery_record v;
                wal_recovery_record(buffer + pos, &v);
                if(v.lsn <= previous_lsn || key_to_thread(v.key, j->db) != (int)j->id)
                    return error(j, "WAL LSN or worker ownership mismatch");
                previous_lsn = v.lsn;
                struct record r = {.key = v.key, .version = v.version, .value_size = v.value_size,
                    .wal = true, .wal_worker = j->id, .segment = seg, .generation = b.generation, .lsn = v.lsn};
                memcpy(r.prefix, v.value, sizeof(r.prefix));
                if(select_record(j, &j->table, &r)) return -1;
                pos += WAL_RECORD_HEADER_BYTES + v.value_size;
            }
            j->batches++;
        }
        off += b.bytes - BLOCK_SIZE;
    }
    return 0;
}
static double scan_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static bool all_zero(const unsigned char *data, size_t bytes) {


    while(bytes >= sizeof(uint64_t)) {
        uint64_t word;
        memcpy(&word, data, sizeof(word));
        if(word) return false;
        data += sizeof(word); bytes -= sizeof(word);
    }
    while(bytes--) if(*data++) return false;
    return true;
}
static bool queue_push(struct queue *q, struct block_batch *item) {
    size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if(tail - head == q->capacity) return false;
    q->items[tail % q->capacity] = item;
    if(tail - head + 1 > q->peak) q->peak = tail - head + 1;
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}
static struct block_batch *queue_pop(struct queue *q) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    if(head == atomic_load_explicit(&q->tail, memory_order_acquire)) return NULL;
    struct block_batch *t = q->items[head % q->capacity];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return t;
}
static void *scan_job(void *arg) {
    struct job *j = arg;
    for(;;) {
        struct block_batch *batch = queue_pop(&j->input);
        if(!batch) {
            if(atomic_load_explicit(&j->scan->finished, memory_order_acquire)) {

                batch = queue_pop(&j->input);
                if(!batch) break;
            } else { sched_yield(); continue; }
        }
        struct transfer *t = batch->transfer;
        if(!atomic_load(&j->scan->aborted)) {
            double started = scan_clock();
            if(t->wal) { j->wal_segments++; scan_wal(j, t->data, t->segment); }
            else {
                j->data_batches++;
                for(uint32_t i = 0; i < batch->count && !atomic_load(&j->scan->aborted); i++) {
                    uint32_t b = batch->offsets[i];
                    j->data_blocks++;
                    if(scan_block(j, t->data + (size_t)b * BLOCK_SIZE, t->begin + b)) break;
                }
            }
            double elapsed = scan_clock() - started;
            j->processing_seconds += elapsed;
            if(t->wal) j->wal_processing_seconds += elapsed;
            else j->ssd_processing_seconds += elapsed;
#ifdef LIBDB_RECOVERY_BATCH_TEST
            if(!atomic_load(&j->scan->aborted)) recovery_test_incremental(j->id, t->wal, t->begin, j,
                j->slot_head ? j->slot_head->slot : NULL, j->slot_head ? j->slot_head->count : 0);
#endif
        }


        while(!queue_push(&j->returned, batch)) sched_yield();
    }
    return NULL;
}
static void release_reference(struct scan_state *s, struct transfer *t) {
    if(!--t->references) { t->busy = false; s->in_use--; }
}
static void recycle(struct scan_state *s, unsigned count) {
    for(unsigned w = 0; w < count; w++) {
        struct block_batch *batch;
        while((batch = queue_pop(&s->jobs[w].returned))) {
#ifdef LIBDB_RECOVERY_BATCH_TEST
            if(!batch->transfer->wal) recovery_test_batch_returned(batch->transfer->begin, w,
                batch->transfer->data, batch->transfer->references - 1);
#endif
            release_reference(s, batch->transfer);
        }
    }
}
static void poll_reads(struct scan_state *s) {
    if(spdk_nvme_qpair_process_completions(s->jobs[0].db->ns_entry->qpair[0], 0) < 0) {

        fprintf(stderr, "fatal: recovery completion polling failed with outstanding reads\n");
        exit(EXIT_FAILURE);
    }
}
static void read_done(void *arg, const struct spdk_nvme_cpl *cpl) {
    struct transfer *t = arg;
    t->io_failed |= spdk_nvme_cpl_is_error(cpl);
    t->completed += t->submitted; t->in_flight = false;
}
static void read_window(struct scan_state *s, struct transfer **window, unsigned n) {
    struct ns_entry *ns = s->jobs[0].db->ns_entry;
    double started = scan_clock();
    for(;;) {
        bool waiting = false;
        for(unsigned i = 0; i < n; i++) {
            struct transfer *t = window[i];
            if(t->io_failed) error(s->jobs, "recovery read completion failed");
            if(!atomic_load(&s->aborted) && !t->in_flight && t->completed < t->count) {
                t->submitted = t->count - t->completed;
                if(t->submitted > s->max_blocks) t->submitted = s->max_blocks;
                t->in_flight = true;
                int rc = spdk_nvme_ns_cmd_read(ns->ns, ns->qpair[0],
                    t->data + (size_t)t->completed * BLOCK_SIZE, t->begin + t->completed,
                    t->submitted, read_done, t, 0);
                if(rc) {
                    t->in_flight = false;
                    if((rc != -ENOMEM && rc != -EAGAIN) || ++t->retries >= 100000)
                        error(s->jobs, "cannot submit recovery read");
                } else t->retries = 0;
            }
            waiting |= t->in_flight || (!atomic_load(&s->aborted) && t->completed < t->count);
        }
        if(!waiting) break;
        poll_reads(s); recycle(s, s->jobs[0].db->thread);
    }
    for(unsigned i = 0; i < n; i++) {
        struct transfer *t = window[i];
        if(t->io_failed) error(s->jobs, "recovery read completion failed");
        if(t->wal) s->wal_read_bytes += (uint64_t)t->completed * BLOCK_SIZE;
        else s->data_read_bytes += (uint64_t)t->completed * BLOCK_SIZE;
    }
    s->io_seconds += scan_clock() - started;
}
static struct transfer *take_buffer(struct scan_state *s) {
    for(;;) {
        recycle(s, s->jobs[0].db->thread);
        if(atomic_load(&s->aborted)) return NULL;
        for(unsigned i = 0; i < s->pool_count; i++) if(!s->pool[i].busy) {
            struct transfer *t = &s->pool[i];
            unsigned char *data = t->data;
            struct block_batch *batches = t->batches;
            uint32_t *offsets = t->offsets;
            *t = (struct transfer){.data = data, .batches = batches, .offsets = offsets, .busy = true};
            if(++s->in_use > s->peak_buffers) s->peak_buffers = s->in_use;
            return t;
        }
        poll_reads(s); sched_yield();
    }
}
static unsigned route_block(struct scan_state *s, struct transfer *t, uint32_t b) {
    struct kv_record_view v;

    if(!kv_record_decode((char *)t->data + (size_t)b * BLOCK_SIZE, BLOCK_SIZE, &v) && v.version)
        return key_to_thread(v.key, s->jobs[0].db);
    return (t->begin + b) % s->jobs[0].db->thread;
}
static void dispatch_window(struct scan_state *s, struct transfer **window, unsigned n) {
    unsigned workers = s->jobs[0].db->thread;
    for(unsigned i = 0; i < n; i++) {
        struct transfer *t = window[i];
        if(atomic_load(&s->aborted) || (!t->wal && t->begin >= s->cutoff)) {
            t->busy = false; s->in_use--; continue;
        }
        for(unsigned w = 0; w < workers; w++)
            t->batches[w] = (struct block_batch){.transfer = t};
        if(t->wal) t->batches[t->owner].count = t->count;
        else {


            for(uint32_t b = 0; b < t->count; b++) t->batches[route_block(s, t, b)].count++;
            uint32_t offset = 0;
            for(unsigned w = 0; w < workers; w++) {
                struct block_batch *batch = &t->batches[w];
                batch->offsets = t->offsets + offset;
                offset += batch->count; batch->count = 0;
            }
            for(uint32_t b = 0; b < t->count; b++) {
                struct block_batch *batch = &t->batches[route_block(s, t, b)];
                batch->offsets[batch->count++] = b;
            }
            s->data_segments++;
        }


        for(unsigned w = 0; w < workers; w++) t->references += t->batches[w].count != 0;
        for(unsigned w = 0; w < workers; w++) {
            struct block_batch *batch = &t->batches[w];
            if(!batch->count) continue;
            for(;;) {
                if(atomic_load(&s->aborted)) { release_reference(s, t); break; }
                if(queue_push(&s->jobs[w].input, batch)) {
#ifdef LIBDB_RECOVERY_BATCH_TEST
                    if(!t->wal) recovery_test_batch_dispatched(t->begin, w, t->data,
                        batch->offsets, batch->count, &s->aborted);
#endif
                    break;
                }
                recycle(s, workers); poll_reads(s);
            }
        }
    }
}
static void produce(struct scan_state *s) {
    struct db *db = s->jobs[0].db;
    struct transfer *window[ctr_max_qp];
    double started = scan_clock();
    uint64_t stride = (uint64_t)db->thread * alloc_grain;
    for(uint64_t base = 0; base < db->data_blocks && !atomic_load(&s->aborted); base += stride) {
        unsigned n = 0;
        for(unsigned i = 0; i < (unsigned)db->thread && base + (uint64_t)i * alloc_grain < db->data_blocks; i++) {
            struct transfer *t = take_buffer(s); if(!t) break;
            t->begin = base + (uint64_t)i * alloc_grain;
            t->count = db->data_blocks - t->begin < alloc_grain ? db->data_blocks - t->begin : alloc_grain;
            window[n++] = t;
        }
        read_window(s, window, n);
        if(!atomic_load(&s->aborted) && !db->recovery_scan_full)
            for(unsigned i = 0; i < n; i++) if(all_zero(window[i]->data, (size_t)window[i]->count * BLOCK_SIZE)) {
                s->cutoff = window[i]->begin; s->first_empty = s->cutoff / alloc_grain; break;
            }
        dispatch_window(s, window, n);
        if(s->first_empty != UINT64_MAX) break;
    }
    s->data_seconds = scan_clock() - started;

    unsigned segments = db->workers[0].wal->segment_count;
    for(unsigned seg = 0; seg < segments && !atomic_load(&s->aborted); seg++) {
        unsigned n = 0;
        for(unsigned w = 0; w < (unsigned)db->thread; w++) {
            struct transfer *t = take_buffer(s); if(!t) break;
            t->wal = true; t->owner = w; t->segment = seg;
            t->begin = db->workers[w].wal->start_block + (uint64_t)seg * (WAL_SEGMENT_BYTES / BLOCK_SIZE);
            t->count = WAL_SEGMENT_BYTES / BLOCK_SIZE; window[n++] = t;
        }
        read_window(s, window, n); dispatch_window(s, window, n);
    }
    atomic_store_explicit(&s->finished, true, memory_order_release);
}
static int prepare_pipeline(struct scan_state *s) {
    unsigned workers = s->jobs[0].db->thread;
    s->max_blocks = spdk_nvme_ns_get_max_io_xfer_size(s->jobs[0].db->ns_entry->ns) / BLOCK_SIZE;
    if(!s->max_blocks) return error(s->jobs, "device transfer limit is smaller than a block");
    s->pool_count = 2 * workers;
    s->pool = calloc(s->pool_count, sizeof(*s->pool));
    if(!s->pool) return error(s->jobs, "DMA pool allocation failed");
    size_t bytes = (size_t)alloc_grain * BLOCK_SIZE;
    if(bytes < WAL_SEGMENT_BYTES) bytes = WAL_SEGMENT_BYTES;
    for(unsigned i = 0; i < s->pool_count; i++) {
        s->pool[i].batches = calloc(workers, sizeof(*s->pool[i].batches));
        s->pool[i].offsets = calloc(alloc_grain, sizeof(*s->pool[i].offsets));
        if(!s->pool[i].batches || !s->pool[i].offsets) return error(s->jobs, "block batch allocation failed");
        s->pool[i].data = spdk_zmalloc(bytes, BLOCK_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
        if(!s->pool[i].data) return error(s->jobs, "scan DMA allocation failed");
    }
    for(unsigned i = 0; i < workers; i++) {
        struct job *j = &s->jobs[i];
        struct queue *queues[] = {&j->input, &j->returned};
        for(unsigned q = 0; q < 2; q++) {
            queues[q]->capacity = s->pool_count;
            atomic_init(&queues[q]->head, 0); atomic_init(&queues[q]->tail, 0);
            queues[q]->items = calloc(s->pool_count, sizeof(*queues[q]->items));
            if(!queues[q]->items) return error(j, "pipeline queue allocation failed");
        }
        unsigned count = j->db->workers[i].wal->segment_count;
        j->generations = calloc(count, sizeof(*j->generations));
        j->components = calloc(j->db->boundary_size, sizeof(*j->components));
        j->tail = count - 1;
        j->empty_buffers = calloc(j->db->boundary_size, sizeof(*j->empty_buffers));
        if(!j->generations || !j->components || !j->empty_buffers) return error(j, "worker recovery state allocation failed");
    }
    return 0;
}

static int assign_cores(struct job *jobs, unsigned count) {
    uint32_t core, main_core = spdk_env_get_current_core();
    unsigned assigned = 0;
    SPDK_ENV_FOREACH_CORE(core) {
        if(core == main_core) continue;
        jobs[assigned++].core = core;
        if(assigned == count) return 0;
    }
    return error(jobs, "not enough SPDK worker cores for recovery");
}
static int run_pinned_job(void *arg) {
    struct job *j = arg;
    uint32_t actual = spdk_env_get_current_core();
    FILE *outputs[] = {stdout, j->db->output_fp};
    for(unsigned i = 0; i < 2; i++) {
        FILE *fp = outputs[i];
        if(!fp || (i && fp == stdout)) continue;
        fprintf(fp, "RECOVERY affinity stage=%s worker=%u assigned_core=%" PRIu32
                    " actual_core=%" PRIu32 "\n", j->stage, j->id, j->core, actual);
        fflush(fp);
    }
    if(actual != j->core) error(j, "recovery worker core does not match assignment");
    atomic_fetch_add_explicit(&j->launch->ready, 1, memory_order_release);
    int decision;
    while(!(decision = atomic_load_explicit(&j->launch->decision, memory_order_acquire))) sched_yield();
    if(decision != 1) return -1;
    j->run(j);
    return j->failed ? -1 : 0;
}
static int parallel(struct job *jobs, unsigned count, const char *stage, void *(*fn)(void *)) {
    struct launch_state state;
    atomic_init(&state.ready, 0); atomic_init(&state.decision, 0);
    unsigned launched = 0;
    for(; launched < count; launched++) {
        struct job *j = &jobs[launched];
        j->stage = stage;
        j->run = fn;
        j->launch = &state;
        if(spdk_env_thread_launch_pinned(j->core, run_pinned_job, j)) break;
    }
    while(atomic_load_explicit(&state.ready, memory_order_acquire) != launched) sched_yield();
    bool cancel = launched != count;
    for(unsigned i = 0; i < launched; i++) cancel |= jobs[i].failed != 0;
    atomic_store_explicit(&state.decision, cancel ? 2 : 1, memory_order_release);


    if(fn == scan_job && !cancel) produce(jobs[0].scan);
    spdk_env_thread_wait_all();
    if(fn == scan_job) recycle(jobs[0].scan, count);
    if(launched != count) return error(&jobs[launched], "cannot launch pinned recovery worker");
    for(unsigned i = 0; i < count; i++) if(jobs[i].failed) return -1;
    return 0;
}
static int by_range(const void *a, const void *b) {
    const struct block *x = *(struct block *const *)a, *y = *(struct block *const *)b;
    if(x->min != y->min) return (x->min > y->min) - (x->min < y->min);
    return (x->max > y->max) - (x->max < y->max);
}
static int partition(struct job *j, key_type key) {
    int lo = 0, hi = j->db->boundary_size;
    key_type *fences = j->db->boundary[j->id];
    while(lo + 1 < hi) { int m = lo + (hi - lo) / 2; if(fences[m] <= key) lo = m; else hi = m; }
    return lo;
}
static void free_nodes(struct dram_index_node *n) {
    if(!n) return;
    if(!n->is_leaf) for(unsigned i = 0; i < n->nchild; i++) free_nodes(n->children[i]);
    free(n);
}
static struct dram_index_node *build_tree(struct subspace_info **groups, size_t n,
                                          struct dram_index_node *parent) {
    struct dram_index_node *node = calloc(1, sizeof(*node));
    if(!node) return NULL;
    node->parent = parent;
    node->is_leaf = n <= DRAM_INDEX_MAX_NODE;
    size_t span = 1;
    while((n + span - 1) / span > DRAM_INDEX_MAX_NODE) {
        if(span > SIZE_MAX / DRAM_INDEX_MAX_NODE) { free(node); return NULL; }
        span *= DRAM_INDEX_MAX_NODE;
    }
    for(size_t at = 0; at < n; at += span) {
        unsigned i = node->nchild++;
        node->key_child[i] = groups[at]->boundary;
        if(node->is_leaf) node->subspaces[i] = groups[at];
        else {
            size_t count = n - at < span ? n - at : span;
            node->children[i] = build_tree(groups + at, count, node);
            if(!node->children[i]) { free_nodes(node); return NULL; }
        }
    }
    return node;
}
static struct subspace_info *new_group(struct job *j, key_type boundary) {
    if(j->group_count == j->group_capacity) {
        size_t cap = j->group_capacity ? j->group_capacity * 2 : 128;
        if(cap < j->group_capacity || cap > SIZE_MAX / sizeof(*j->groups)) return NULL;
        void *p = realloc(j->groups, cap * sizeof(*j->groups));
        if(!p) return NULL;
        j->groups = p; j->group_capacity = cap;
    }
    struct subspace_info *g = subspace_info_initialize();
    if(!g) return NULL;
    g->boundary = boundary;
    j->groups[j->group_count++] = g;
    return g;
}
static int validate_block(struct job *j, struct record *r) {
    int part = partition(j, r->key);
    if(key_to_thread(r->source->min, j->db) != (int)j->id || key_to_thread(r->source->max, j->db) != (int)j->id ||
       partition(j, r->source->min) != part || partition(j, r->source->max) != part) {
        int min_worker = key_to_thread(r->source->min, j->db);
        int max_worker = key_to_thread(r->source->max, j->db);
        struct job min_owner = {.db = j->db, .id = (unsigned)min_worker};
        struct job max_owner = {.db = j->db, .id = (unsigned)max_worker};
        bool has_upper = j->id + 1 < (unsigned)j->db->thread || part + 1 < j->db->boundary_size;
        key_type upper = j->id + 1 < (unsigned)j->db->thread ? j->db->boundary[j->id + 1][part] :
            part + 1 < j->db->boundary_size ? j->db->boundary[0][part + 1] : 0;
        fprintf(stderr, "RECOVERY block_conflict worker=%u block=%u winner_offset=%d"
            " winner_key=%" PRIu64 " winner_version=%" PRIu64 " worker_ssd_winners=%zu"
            " range_min=%" PRIu64 " range_max=%" PRIu64 " min_worker=%d max_worker=%d"
            " winner_partition=%d min_partition=%d max_partition=%d"
            " partition_begin=%" PRIu64 " partition_has_upper=%d partition_upper=%" PRIu64 "\n",
            j->id, (unsigned)r->addr.block, (int)r->addr.off, (uint64_t)r->key, r->version, r->source->count,
            (uint64_t)r->source->min, (uint64_t)r->source->max, min_worker, max_worker, part,
            partition(&min_owner, r->source->min), partition(&max_owner, r->source->max),
            (uint64_t)j->db->boundary[j->id][part], has_upper, (uint64_t)upper);
        error(j, "effective block crosses a worker or initial partition"); return -1;
    }
    return 0;
}
static int seal_group(struct job *j, struct block **blocks, size_t begin, size_t end,
                      size_t keys, key_type boundary) {
    if(keys > (size_t)SUBSPACE_MAX_SLOT * SLOT_MAX_ELEMENT)
        return error(j, "overlap group exceeds fixed slot capacity");
    struct subspace_info *g = new_group(j, boundary);
    if(!g) return error(j, "group allocation failed");
    if(end - begin > UINT32_MAX) return error(j, "group block count overflow");
    g->total_block_num = end - begin;
    g->redundant_size = (uint64_t)g->total_block_num * BLOCK_SIZE;
    for(size_t b = begin; b < end; b++) blocks[b]->group = g;
    return 0;
}
struct final_component { key_type min; size_t begin, end, keys; };
static void collect_components(struct job *j, struct avl *node, struct block **blocks,
    size_t *nb, struct final_component *components, size_t *nc) {
    if(!node) return;
    collect_components(j, node->left, blocks, nb, components, nc);
    struct component *c = (struct component *)node;
    size_t begin = *nb, keys = 0;
    for(struct block *b = c->head; b; b = b->next) if(b->count) {
        if(validate_block(j, b->winner)) return;
        blocks[(*nb)++] = b; keys += b->count;
    }
    if(begin != *nb) {
        if(!c->dirty) components[(*nc)++] = (struct final_component){c->order.key, begin, *nb, keys};
        else {
            j->recomputed_groups++;
            qsort(blocks + begin, *nb - begin, sizeof(*blocks), by_range);
            size_t at = begin;
            while(at < *nb) {
                size_t first = at; keys = blocks[at]->count;
                key_type max = blocks[at++]->max;
                while(at < *nb && blocks[at]->min <= max) {
                    keys += blocks[at]->count;
                    if(blocks[at]->max > max) max = blocks[at]->max;
                    at++;
                }
                components[(*nc)++] = (struct final_component){blocks[first]->min, first, at, keys};
            }
        }
    }
    collect_components(j, node->right, blocks, nb, components, nc);
}


static int bind_slots(struct job *j) {
    size_t gi = 0;
    j->slot_tree = NULL;
    while(j->slot_head) {
        struct recovery_slot *leaf = j->slot_head;
        key_type key = leaf->slot->entries[0].key;
        while(gi + 1 < j->group_count && key >= j->groups[gi + 1]->boundary) gi++;
        struct subspace_info *g = j->groups[gi];
        unsigned length = 0;
        while(length < leaf->count &&
              (gi + 1 == j->group_count || leaf->slot->entries[length].key < j->groups[gi + 1]->boundary)) length++;
        bool new_slot = !g->slot_num || g->slot_key_num[g->slot_num - 1] == SLOT_MAX_ELEMENT;
        if(new_slot && g->slot_num == SUBSPACE_MAX_SLOT) return error(j, "final slot capacity exceeded");
        bool adopt = new_slot && length == leaf->count;
        if(new_slot) {
            struct index_slot *slot = adopt ? leaf->slot : slot_index_allocate(&j->db->workers[j->id].slots);
            if(!slot) return error(j, "boundary slot allocation failed");
            slot->boundary = g->boundary;
            g->min_key[g->slot_num] = key; g->slot[g->slot_num++] = slot;
        }
        unsigned at = g->slot_num - 1, used = g->slot_key_num[at];
        if(length > SLOT_MAX_ELEMENT - used) length = SLOT_MAX_ELEMENT - used;
        for(unsigned i = 0; i < length; i++) g->redundant_size -= leaf->slot->entries[i].addr.size;
        if(!adopt) memcpy(g->slot[at]->entries + used, leaf->slot->entries, length * sizeof(*leaf->slot->entries));
        g->slot_key_num[at] += length; g->key_num += length;
        leaf->count -= length;
        if(!leaf->count) {
            j->slot_head = leaf->next;
            if(!adopt) slot_index_free(&j->db->workers[j->id].slots, leaf->slot);
            free(leaf);
        } else memmove(leaf->slot->entries, leaf->slot->entries + length, leaf->count * sizeof(*leaf->slot->entries));
    }
    return 0;
}
static void *build_job(void *arg) {
    struct job *j = arg;
    double started = scan_clock();
    struct block **blocks = NULL;
    struct final_component *components = NULL;
    j->count = j->table.count;
    if(j->block_count > SIZE_MAX / sizeof(*components)) { error(j, "recovery list size overflow"); goto out; }
    blocks = malloc((j->block_count ? j->block_count : 1) * sizeof(*blocks));
    components = malloc((j->block_count ? j->block_count : 1) * sizeof(*components));
    if(!blocks || !components) { error(j, "ordered recovery list allocation failed"); goto out; }
    size_t nb = 0, nc = 0;
    for(int part = 0; part < j->db->boundary_size; part++) {
        size_t first_component = nc;
        collect_components(j, j->components[part], blocks, &nb, components, &nc);
        if(j->failed) goto out;
        key_type fence = j->db->boundary[j->id][part];
        if(first_component == nc) {
            if(!new_group(j, fence)) { error(j, "empty group allocation failed"); goto out; }
            continue;
        }
        size_t begin = components[first_component].begin, population = 0;
        for(size_t i = first_component; i < nc; i++) {
            struct final_component *c = &components[i];
            if(population && (population > SUBSPACE_MAX_KEY || c->keys > SUBSPACE_MAX_KEY - population)) {
                if(seal_group(j, blocks, begin, c->begin, population, fence)) goto out;
                begin = c->begin; population = 0; fence = c->min;
            }
            population += c->keys;
        }
        if(seal_group(j, blocks, begin, nb, population, fence)) goto out;
    }
    if(bind_slots(j)) goto out;
    j->index = calloc(1, sizeof(*j->index));
    if(!j->index || !(j->index->head = build_tree(j->groups, j->group_count, NULL)))
        error(j, "group index allocation failed");
out:
    free(blocks); free(components);
    j->group_seconds = scan_clock() - started;
    return NULL;
}
static void release_temporary(struct job *j);



struct buffer_binding {
    size_t group;
    struct subspace_info *last;
    struct write_buffer_index_node *tails[SKIPLIST_RESERVE_LEVEL];
};
static int bind_buffer(struct job *j, struct recovery_buffer *buffer, struct buffer_binding *binding) {
    if(!buffer->index) return 0;
    struct write_buffer_index_node *head = buffer->index->head;
    while(head->forward[0]) {
        struct write_buffer_index_node *node = head->forward[0];
        while(binding->group + 1 < j->group_count && node->key >= j->groups[binding->group + 1]->boundary)
            binding->group++;
        struct subspace_info *g = j->groups[binding->group];
        if(g->buffered_size > UINT32_MAX - node->size) return error(j, "recovered group buffer size overflow");
        unsigned level = buffer_level(node->key);
        for(unsigned l = 0; l <= level; l++) {
            head->forward[l] = node->forward[l]; node->forward[l] = NULL;
        }
        buffer->keys--;
        if(g != binding->last) {
            for(unsigned l = 0; l < SKIPLIST_RESERVE_LEVEL; l++) binding->tails[l] = g->buffer_index->head;
            binding->last = g;
        }
        for(unsigned l = 0; l <= level; l++) { binding->tails[l]->forward[l] = node; binding->tails[l] = node; }
        if(g->buffer_index->levels <= level) g->buffer_index->levels = level + 1;
        g->buffered_size += node->size;
    }
    return 0;
}
static int bind_component_buffers(struct job *j, struct avl *root, struct buffer_binding *binding) {
    if(!root) return 0;
    if(bind_component_buffers(j, root->left, binding) ||
       bind_buffer(j, &((struct component *)root)->buffer, binding)) return -1;
    return bind_component_buffers(j, root->right, binding);
}
static void *replay_job(void *arg) {
    struct job *j = arg;
    struct thread *t = &j->db->workers[j->id];
    double started = scan_clock();
    struct buffer_binding binding = {0};
    for(int part = 0; part < j->db->boundary_size; part++) {
        if(bind_component_buffers(j, j->components[part], &binding) ||
           bind_buffer(j, &j->empty_buffers[part], &binding)) return NULL;
    }
    wb_snapshot_store(&t->kvbuffer.published_live_bytes, t->kvbuffer.live_bytes);
    wb_snapshot_store(&t->kvbuffer.published_slot_bytes, t->kvbuffer.slot_bytes);

    release_temporary(j);
    j->buffer_bind_seconds = scan_clock() - started;
    return NULL;
}
static void free_segments(struct free_space *fs) {
    if(!fs) return;
    while(fs->head) { struct segment *next = fs->head->next; free(fs->head); fs->head = next; }
    fs->tail = NULL;
}
static int build_free_space(struct db *db) {


    struct segment *s = NULL;
    unsigned count = 0;
    for(uint64_t b = 0; b < db->data_blocks; b++) if(!g_map[b]) {
        if(!s) { s = calloc(1, sizeof(*s)); if(!s) goto fail; s->segment_num = -1; }
        s->block_array[count++] = b;
        if(count == alloc_grain) {
            if(db->fs->tail) db->fs->tail->next = s; else db->fs->head = s;
            db->fs->tail = s; s = NULL; count = 0;
        }
    }
    if(s) {
        s->cur = alloc_grain - count;
        memmove(s->block_array + s->cur, s->block_array, count * sizeof(s->block_array[0]));
        if(db->fs->tail) db->fs->tail->next = s; else db->fs->head = s;
        db->fs->tail = s;
    }
    for(int i = 0; i < db->thread; i++) {
        struct segment *current = calloc(1, sizeof(*current));
        if(!current) goto fail;
        current->cur = alloc_grain; current->segment_num = -1;
        db->workers[i].curr_segment = current;
    }
    return 0;
fail:
    free_segments(db->fs);
    for(int i = 0; i < db->thread; i++) { free(db->workers[i].curr_segment); db->workers[i].curr_segment = NULL; }
    return -1;
}
static void destroy_group(struct thread *t, struct subspace_info *g) {
    for(uint32_t i = 0; i < g->slot_num; i++) slot_index_free(&t->slots, g->slot[i]);
    for(struct write_buffer_index_node *n = g->buffer_index->head->forward[0]; n; n = n->forward[0]) {
        wal_release_dependencies(n);
        write_buffer_free(n->record, n->size, &t->kvbuffer);
    }
    write_buffer_index_destroy(g->buffer_index);
    kvcache_destroy_empty(g->kvcache);
    free(g);
}
static void destroy_buffer(struct job *j, struct recovery_buffer *buffer) {
    if(!buffer->index) return;
    for(struct write_buffer_index_node *n = buffer->index->head->forward[0]; n; n = n->forward[0]) {
        wal_release_dependencies(n);
        write_buffer_free(n->record, n->size, &j->db->workers[j->id].kvbuffer);
    }
    write_buffer_index_destroy(buffer->index);
    buffer->index = NULL; buffer->keys = 0;
}
static void release_temporary(struct job *j) {
    while(j->slot_head) {
        struct recovery_slot *next = j->slot_head->next;
        slot_index_free(&j->db->workers[j->id].slots, j->slot_head->slot);
        free(j->slot_head); j->slot_head = next;
    }
    j->slot_tree = NULL;
    if(j->empty_buffers) {
        for(int part = 0; part < j->db->boundary_size; part++) destroy_buffer(j, &j->empty_buffers[part]);
        free(j->empty_buffers); j->empty_buffers = NULL;
    }
    free(j->generations); j->generations = NULL;
    free_records(j->table.root); j->table.root = NULL;
    free(j->input.items); j->input.items = NULL;
    free(j->returned.items); j->returned.items = NULL;
    free(j->components); j->components = NULL;
    while(j->allocated_blocks) {
        struct block *b = j->allocated_blocks; j->allocated_blocks = b->allocated_next; free(b);
    }
    while(j->allocated_components) {
        struct component *c = j->allocated_components; j->allocated_components = c->allocated_next;
        destroy_buffer(j, &c->buffer); free(c);
    }
}
static void release_dma(struct scan_state *s) {
    if(!s->pool) return;
    for(unsigned i = 0; i < s->pool_count; i++) {
        spdk_free(s->pool[i].data);
        free(s->pool[i].batches); free(s->pool[i].offsets);
    }
    free(s->pool); s->pool = NULL;
}

int recovery_rebuild_index_and_write_buffer_parallel(struct db *db,
        uint64_t *index_end, uint64_t *buffer_end, uint64_t *fs_end) {
    if(index_end) *index_end = 0;
    if(buffer_end) *buffer_end = 0;
    if(fs_end) *fs_end = 0;
    if(!db || !db->wal_enabled || db->thread <= 0 || db->thread > ctr_max_qp ||
       !db->workers || !db->index || !db->boundary || db->boundary_size <= 0 || !db->ns_entry ||
       !db->ns_entry->qpair || !db->fs || db->fs->head || db->fs->tail || !g_map || !db->data_blocks ||
       db->data_blocks > INT_MAX || g_block_num != db->data_blocks) return 0;
    for(int i = 0; i < db->thread; i++) {
        struct thread *t = &db->workers[i];
        if(!t->wal || t->thread != i || t->db != db || t->index != &db->index[i] ||
           t->wal->start_block != db->data_blocks + (uint64_t)i * db->wal_worker_blocks ||
           (uint64_t)t->wal->segment_count * (WAL_SEGMENT_BYTES / BLOCK_SIZE) != db->wal_worker_blocks ||
           wal_busy(t) || t->curr_segment || t->kvbuffer.live_bytes || slot_allocator_live_slots(&t->slots) ||
           !db->index[i].dram_index || !db->index[i].dram_index->head ||
           db->index[i].dram_index->head->nchild) return 0;
    }
    struct job *jobs = calloc(db->thread, sizeof(*jobs));
    if(!jobs) return 0;
    size_t total_keys = 0;
    double wall_started = scan_clock(), convergence_started = 0;
    bool success = false, free_space_built = false;
    struct scan_state scan = {.cutoff = db->data_blocks, .first_empty = UINT64_MAX, .jobs = jobs};
    atomic_init(&scan.aborted, false); atomic_init(&scan.finished, false);
    uint64_t max_version = 0, started = spdk_get_ticks();
    for(int i = 0; i < db->thread; i++) {
        jobs[i].db = db; jobs[i].id = i; jobs[i].scan = &scan; atomic_init(&jobs[i].failed, 0);
    }
    if(assign_cores(jobs, db->thread)) goto out;
    memset(g_map, 0, db->data_blocks * sizeof(*g_map));
    atomic_store(&g_used_data_blocks, 0);
    if(prepare_pipeline(&scan) || parallel(jobs, db->thread, "scan", scan_job)) goto out;
    uint64_t data_read_bytes = scan.data_read_bytes;
    double data_seconds = scan.data_seconds;
    convergence_started = scan_clock();
    FILE *scan_outputs[] = {stdout, db->output_fp};
    for(unsigned i = 0; i < 2; i++) {
        FILE *fp = scan_outputs[i];
        if(!fp || (i && fp == stdout)) continue;
        fprintf(fp, "RECOVERY scan version=1 mode=%s segment_blocks=%u data_blocks=%" PRIu64
            " first_empty_segment=", db->recovery_scan_full ? "full" : "first-empty",
            (unsigned)alloc_grain, (uint64_t)db->data_blocks);
        if(scan.first_empty == UINT64_MAX) fputs("none", fp);
        else fprintf(fp, "%" PRIu64, scan.first_empty);
        fprintf(fp, " cutoff_block=%" PRIu64 " read_bytes=%" PRIu64 " unread_bytes=%" PRIu64
            " elapsed_seconds=%.6f\n", scan.cutoff, data_read_bytes,
            (uint64_t)db->data_blocks * BLOCK_SIZE - data_read_bytes, data_seconds);
    }
    for(int i = 0; i < db->thread; i++) {
        struct job *j = &jobs[i];
        if(j->max_version > max_version) max_version = j->max_version;
        total_keys += j->table.count;
        if(wal_recovery_finish(&db->workers[i], j->generations, j->tail, j->max_lsn, j->max_sequence)) {
            error(j, "WAL counter overflow or invalid restart state"); goto out;
        }
    }
    if(kv_record_advance_version(max_version)) { error(jobs, "KV version exhausted"); goto out; }
    if(parallel(jobs, db->thread, "finalize_groups", build_job)) goto out;
    if(index_end) *index_end = spdk_get_ticks();
    if(build_free_space(db)) { error(jobs, "free-space reconstruction allocation failed"); goto out; }
    free_space_built = true;
    if(fs_end) *fs_end = spdk_get_ticks();
    if(parallel(jobs, db->thread, "bind_buffers", replay_job)) goto out;
    release_dma(&scan);
    if(buffer_end) *buffer_end = spdk_get_ticks();
    uint64_t finished = spdk_get_ticks();
    double wall_finished = scan_clock();
    for(int i = 0; i < db->thread; i++) {
        free_nodes(db->index[i].dram_index->head); free(db->index[i].dram_index);
        db->index[i].dram_index = jobs[i].index; jobs[i].index = NULL;
        thread_subspaces[i] = jobs[i].group_count;
        db->workers[i].kv_version_next = db->workers[i].kv_version_limit = 0;
    }
    db->initial_load.complete = false;
    db->initial_load.error = "unavailable_after_recovery";
    db->initial_load.record_bytes = 0;
    FILE *outputs[] = {stdout, db->output_fp};
    for(unsigned out = 0; out < 2; out++) {
        FILE *fp = outputs[out];
        if(!fp || (out && fp == stdout)) continue;
        for(int i = 0; i < db->thread; i++) {
            struct job *j = &jobs[i];
            fprintf(fp, "RECOVERY worker=%d keys=%zu groups=%zu parsed_ssd_records=%" PRIu64
                " block_parse_stops=%" PRIu64 " first_stop_block=%" PRIu64 " first_stop_offset=%u"
                " nonzero_parse_stops=%" PRIu64 " first_nonzero_stop_block=%" PRIu64 " first_nonzero_stop_offset=%u"
                " wal_batches=%" PRIu64 " malformed_wal_batches=%" PRIu64 "\n",
                i, j->count, j->group_count, j->parsed, j->stopped, j->first_stop, j->first_offset,
                j->nonzero_stops, j->first_nonzero_stop, j->first_nonzero_offset, j->batches, j->malformed_wal);
        }
        fprintf(fp, "RECOVERY pipeline version=4 io_seconds=%.6f data_read_bytes=%" PRIu64
            " wal_read_bytes=%" PRIu64 " data_segments=%" PRIu64 " buffer_capacity=%u peak_buffers=%u convergence_seconds=%.6f elapsed_seconds=%.6f\n",
            scan.io_seconds, scan.data_read_bytes, scan.wal_read_bytes, scan.data_segments, scan.pool_count, scan.peak_buffers,
            wall_finished - convergence_started, wall_finished - wall_started);
        for(int i = 0; i < db->thread; i++) {
            struct job *j = &jobs[i];
            fprintf(fp, "RECOVERY pipeline_worker worker=%d data_blocks=%" PRIu64 " data_batches=%" PRIu64 " wal_segments=%" PRIu64
                " processing_seconds=%.6f queue_peak=%zu recomputed_groups=%" PRIu64
                " ssd_index_updates=%" PRIu64 " buffer_updates=%" PRIu64 " buffer_removals=%" PRIu64
                " group_seconds=%.6f buffer_bind_seconds=%.6f"
                " wal_buffer_groups=%" PRIu64 " max_wal_buffer_keys=%" PRIu64 " buffer_search_steps=%" PRIu64
                " ssd_processing_seconds=%.6f wal_processing_seconds=%.6f\n",
                i, j->data_blocks, j->data_batches, j->wal_segments, j->processing_seconds, j->input.peak, j->recomputed_groups,
                j->ssd_index_updates, j->buffer_updates, j->buffer_removals, j->group_seconds, j->buffer_bind_seconds,
                j->wal_buffer_groups, j->max_wal_buffer_keys, j->buffer_search_steps,
                j->ssd_processing_seconds, j->wal_processing_seconds);
        }
        fprintf(fp, "RECOVERY dupcheck=disabled\n");
        fprintf(fp, "RECOVERY complete keys=%zu data_blocks_used=%" PRIu64 " elapsed_ticks=%" PRIu64 "\n",
            total_keys, (uint64_t)atomic_load(&g_used_data_blocks), finished - started);
    }
    success = true;
out:
    release_dma(&scan);
    for(int i = 0; i < db->thread; i++) {
        struct job *j = &jobs[i];
        if(!success) {
            if(j->index) { free_nodes(j->index->head); free(j->index); }
            for(size_t k = 0; k < j->group_count; k++) destroy_group(&db->workers[i], j->groups[k]);
        }
        free(j->groups); release_temporary(j);
    }
    if(!success) {
        if(free_space_built) {
            free_segments(db->fs);
            for(int i = 0; i < db->thread; i++) { free(db->workers[i].curr_segment); db->workers[i].curr_segment = NULL; }
        }
        memset(g_map, 0, db->data_blocks * sizeof(*g_map)); atomic_store(&g_used_data_blocks, 0);
        if(index_end) *index_end = 0;
        if(buffer_end) *buffer_end = 0;
        if(fs_end) *fs_end = 0;
    }
    free(jobs);
    return success;
}
int recovery_rebuild_write_buffer_indices(struct db *db) {

    return recovery_rebuild_index_and_write_buffer_parallel(db, NULL, NULL, NULL);
}
