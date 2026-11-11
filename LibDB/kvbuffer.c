#include "./kvbuffer.h"
#include <libpmem.h>
#include "kv_record.h"
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <errno.h>

char * write_buffer_file_str(int file_seq){
    return libdb_config_make_pmem_path("write_buffer_file", (unsigned int)file_seq);
}

static uint64_t wbf_file_size(void){
    return WBF_SIZE < PMEMOBJ_MIN_POOL ? PMEMOBJ_MIN_POOL : WBF_SIZE;
}


static PMEMobjpool *write_buffer_file_new(struct write_buffer *wb){
    uint64_t size = wbf_file_size();
    if(wb->file_num >= MAX_FILE_NUM || wb->file_bytes > LIBDB_WBF_CAPACITY_LIMIT_BYTES ||
       size > LIBDB_WBF_CAPACITY_LIMIT_BYTES - wb->file_bytes) return NULL;
    char *path = write_buffer_file_str(wb->file_num);
    if(!path) return NULL;
    PMEMobjpool *pool = pmemobj_create(path, "write_buffer", size, 0666);
    if(!pool){
        fprintf(stderr, "%s pmemobj_create failed: %s\n", path, pmemobj_errormsg());
        free(path); return NULL;
    }
    wb->pool[wb->file_num++] = pool;
    wb->opened_file_count++;
    wb->file_bytes += size;
    wb->file_peak_bytes = wb->file_bytes;
    PMEMoid oid = pmemobj_root(pool, sizeof(struct write_buffer_file_root));
    if(OID_IS_NULL(oid)){ free(path); return NULL; }
    struct write_buffer_file_root *root = pmemobj_direct(oid);
    root->magic = WBF_FORMAT_MAGIC;
    root->format_version = WBF_FORMAT_VERSION;
    root->file_size = size;
    root->segment_size = WSB_SIZE;
    root->last_used_wbf_seq = WBF_NO_FILE;
    pmemobj_persist(pool, root, sizeof(*root));
    printf("%s pmemobj_create done\n", path);
    free(path);
    return pool;
}



static int write_buffer_inventory(uint64_t *bytes, uint32_t *count){
    *bytes = 0; *count = 0;
    DIR *dir = opendir(g_libdb_config.pmem_dir);
    if(!dir) return 0;
    struct dirent *entry;
    bool seen[MAX_FILE_NUM] = {0};
    int ok = 1;
    while((entry = readdir(dir))){
        const char *prefix = "write_buffer_file";
        size_t len = strlen(prefix);
        if(strncmp(entry->d_name, prefix, len)) continue;
        const char *suffix = entry->d_name + len;
        char *end;
        errno = 0;
        unsigned long seq = strtoul(suffix, &end, 10);
        if(!*suffix || *suffix < '0' || *suffix > '9' || *end || errno || seq >= MAX_FILE_NUM || seen[seq]){
            fprintf(stderr, "WBF invalid filename: %s\n", entry->d_name); ok = 0; break;
        }
        struct stat st;
        if(fstatat(dirfd(dir), entry->d_name, &st, 0) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != wbf_file_size()){
            fprintf(stderr, "WBF cannot inspect file: %s\n", entry->d_name); ok = 0; break;
        }
        if((uint64_t)st.st_size > LIBDB_WBF_CAPACITY_LIMIT_BYTES - *bytes){
            fprintf(stderr, "WBF recovery hard limit exceeded: file=%s hard_limit_bytes=%" PRIu64 "\n",
                    entry->d_name, (uint64_t)LIBDB_WBF_CAPACITY_LIMIT_BYTES); ok = 0; break;
        }
        *bytes += (uint64_t)st.st_size;
        seen[seq] = true;
        (*count)++;
    }
    closedir(dir);
    for(uint32_t i = 0; ok && i < *count; i++)
        if(!seen[i]) { fprintf(stderr, "WBF file sequence contains a gap\n"); ok = 0; }
    return ok;
}

static PMEMobjpool * write_buffer_file_open(int file_seq){
    char * file_name = write_buffer_file_str(file_seq);
    if(file_name == NULL){
        return NULL;
    }
    PMEMobjpool * pool = pmemobj_open(file_name , "write_buffer");
    free(file_name);
    return pool;
}

static struct write_buffer *write_buffer_control_new(void){
    struct write_buffer *wb = NULL;
    if(posix_memalign((void **)&wb, _Alignof(struct write_buffer), sizeof(*wb))) return NULL;
    memset(wb, 0, sizeof(*wb));
    for(int i = 0; i < MAX_THREAD; i++){
        wbf_snapshot_store(&wb->worker_stats[i].published_used_delta, 0);
        wbf_snapshot_store(&wb->worker_stats[i].published_reusable, 0);
    }
    wb->last_used_wbf_seq = WBF_NO_FILE;
    atomic_init(&wb->pressure, false);
    if(pthread_mutex_init(&g_mutex_lock_wbs, NULL)) { free(wb); return NULL; }
    return wb;
}

void write_buffer_close(struct write_buffer *wb){
    if(!wb) return;
    for(uint32_t f = 0; f < wb->file_num; f++)
        if(wb->pool[f]) pmemobj_close(wb->pool[f]);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
    free(wb);
}

static struct write_buffer_file_root *wbf_root(PMEMobjpool *pool){
    if(!pool || pmemobj_root_size(pool) < sizeof(struct write_buffer_file_root)) return NULL;
    struct write_buffer_file_root *root = pmemobj_direct(pmemobj_root(pool, 0));
    if(!root || root->magic != WBF_FORMAT_MAGIC || root->format_version != WBF_FORMAT_VERSION ||
       root->file_size != wbf_file_size() || root->segment_size != WSB_SIZE ||
       root->segment_num == 0 || root->segment_num > WSB_NUM_MAX) return NULL;
    return root;
}

struct write_buffer * write_buffer_initialize(){
    uint64_t bytes, target = LIBDB_WBF_CAPACITY_LIMIT_BYTES / wbf_file_size();
    uint32_t count;
    if(!target || target > MAX_FILE_NUM){
        fprintf(stderr, "WBF invalid preallocation file count: %" PRIu64 "\n", target);
        return NULL;
    }
    if(!write_buffer_inventory(&bytes, &count) || count){
        fprintf(stderr, "WBF fresh initialization requires an empty WBF file set\n");
        return NULL;
    }
    struct write_buffer *wb = write_buffer_control_new();
    if(!wb) return NULL;
    for(uint32_t f = 0; f < target; f++){
        PMEMobjpool *pool = write_buffer_file_new(wb);
        if(!pool) goto fail;
        struct write_buffer_file_root *root = pmemobj_direct(pmemobj_root(pool, 0));
        while(root->segment_num < WSB_NUM_MAX){
            PMEMoid segment = OID_NULL;
            errno = 0;
            if(pmemobj_zalloc(pool, &segment, WSB_SIZE, 0)){
                if(errno != ENOMEM) goto fail;
                break;
            }
            uint16_t n = root->segment_num;
            root->segment[n] = segment;
            root->segment_element_size[n] = 0;
            pmemobj_persist(pool, &root->segment[n], sizeof(root->segment[n]));
            pmemobj_persist(pool, &root->segment_element_size[n], sizeof(uint16_t));
            root->segment_num = n + 1;
            pmemobj_persist(pool, &root->segment_num, sizeof(root->segment_num));
        }
        if(!root->segment_num){
            fprintf(stderr, "WBF file %u cannot hold a segment of %zu bytes\n", f, (size_t)WSB_SIZE);
            goto fail;
        }
        wb->preallocated_segments[f] = root->segment_num;
        wb->reserved_segment_bytes += (uint64_t)root->segment_num * WSB_SIZE;
    }
    struct write_buffer_file_root *manifest = wbf_root(wb->pool[0]);
    if(!manifest) goto fail;
    manifest->file_count = wb->file_num;
    memcpy(manifest->preallocated_segments, wb->preallocated_segments,
           sizeof(wb->preallocated_segments));
    pmemobj_persist(wb->pool[0], manifest, sizeof(*manifest));

    volatile int committed = 0;
    TX_BEGIN(wb->pool[0]) {
        pmemobj_tx_add_range_direct(&manifest->initialized, sizeof(manifest->initialized));
        manifest->initialized = 1;
    } TX_ONCOMMIT { committed = 1; } TX_END
    if(!committed) goto fail;
    write_buffer_raise_pressure_locked(wb);
    return wb;
fail:
    fprintf(stderr, "WBF preallocation failed; incomplete files retained\n");
    write_buffer_close(wb);
    return NULL;
}



static int wbf_validate_file(const struct write_buffer *wb, uint32_t f,
                             uint32_t *claimed){
    struct write_buffer_file_root *root = wbf_root(wb->pool[f]);
    if(!root || root->segment_num != wb->preallocated_segments[f]) return 0;
    *claimed = 0;
    bool unclaimed = false;
    for(unsigned s = 0; s < root->segment_num; s++){
        PMEMoid segment = root->segment[s];
        size_t stride = root->segment_element_size[s];
        if(OID_IS_NULL(segment) || pmemobj_pool_by_oid(segment) != wb->pool[f]) return 0;
        size_t actual = pmemobj_alloc_usable_size(segment);
        if(actual < WSB_SIZE || actual >= 2ULL * WSB_SIZE){
            fprintf(stderr, "WBF incompatible segment: file=%u segment=%u "
                    "actual_bytes=%zu required_bytes=%zu; create a fresh database\n",
                    f, s, actual, (size_t)WSB_SIZE);
            return 0;
        }
        if(!stride){ unclaimed = true; continue; }
        if(unclaimed || stride < WBF_SLOT_MIN_SIZE || stride > LIBDB_WBF_MAX_RECORD_SIZE ||
           (stride & (stride - 1))) return 0;
        (*claimed)++;
    }
    return 1;
}

struct write_buffer * write_buffer_open_existing(){
    uint64_t bytes, target = LIBDB_WBF_CAPACITY_LIMIT_BYTES / wbf_file_size();
    uint32_t count;
    if(!write_buffer_inventory(&bytes, &count)) return NULL;
    if(!target || target > MAX_FILE_NUM || count != target){
        fprintf(stderr, "WBF recovery: incomplete file set or incompatible budget: "
                "found=%u expected=%" PRIu64 "\n", count, target);
        return NULL;
    }
    struct write_buffer *wb = write_buffer_control_new();
    if(!wb) return NULL;
    wb->file_num = count;
    wb->file_bytes = wb->file_peak_bytes = bytes;
    wb->pool[0] = write_buffer_file_open(0);
    if(!wb->pool[0]) goto fail;
    wb->opened_file_count = 1;
    struct write_buffer_file_root *manifest = wbf_root(wb->pool[0]);
    if(!manifest || manifest->initialized != 1 || manifest->file_count != count ||
       manifest->segment_num != manifest->preallocated_segments[0] ||
       (manifest->last_used_wbf_seq != WBF_NO_FILE && manifest->last_used_wbf_seq >= count)) goto fail;
    wb->last_used_wbf_seq = manifest->last_used_wbf_seq;
    memcpy(wb->preallocated_segments, manifest->preallocated_segments,
           sizeof(wb->preallocated_segments));
    for(uint32_t f = 0; f < count; f++){
        uint32_t n = wb->preallocated_segments[f];
        if(!n || n > WSB_NUM_MAX || (uint64_t)n * WSB_SIZE >= wbf_file_size()) goto fail;
        wb->reserved_segment_bytes += (uint64_t)n * WSB_SIZE;
    }
    wb->recovery_file_count = write_buffer_used_file_count(wb);
    for(uint32_t f = 0; f < wb->recovery_file_count; f++){
        if(f){
            wb->pool[f] = write_buffer_file_open(f);
            if(!wb->pool[f]) goto fail;
            wb->opened_file_count++;
        }
        uint32_t claimed;
        if(!wbf_validate_file(wb, f, &claimed) ||
           (f < wb->last_used_wbf_seq && claimed != wb->preallocated_segments[f])) goto fail;
        wb->reserved_segment_bytes -= (uint64_t)claimed * WSB_SIZE;
        wb->next_file = f;
        wb->next_segment = claimed;
    }
    if(!write_buffer_rebuild_occupancy(wb)) goto fail;

    write_buffer_raise_pressure_locked(wb);
    return wb;
fail:
    fprintf(stderr, "WBF recovery: invalid/incomplete preallocation metadata or geometry; create a fresh database\n");
    write_buffer_close(wb);
    return NULL;
}

int write_buffer_rebuild_occupancy(struct write_buffer *wb){
    if(!wb || wb->file_num > MAX_FILE_NUM || write_buffer_used_file_count(wb) > wb->file_num) return 0;
    uint32_t files = write_buffer_used_file_count(wb);
    for(uint32_t f = 0; f < files; f++){
        uint32_t claimed;
        if(!wbf_validate_file(wb, f, &claimed)) return 0;
    }
    uint64_t bytes = 0;
    for(uint32_t f = 0; f < files; f++){
        struct write_buffer_file_root *root = wbf_root(wb->pool[f]);
        for(unsigned s = 0; s < root->segment_num; s++){
            size_t stride = root->segment_element_size[s];
            if(!stride) continue;
            const char *base = pmemobj_direct(root->segment[s]);
            for(size_t off = 0; off + stride <= WSB_SIZE; off += stride){
                uint16_t key_size;
                memcpy(&key_size, base + off, sizeof(key_size));
                if(!key_size) continue;
                struct kv_record_view view;
                if(kv_record_decode(base + off, stride, &view) || !view.value_size){
                    fprintf(stderr, "WBF invalid slot: file=%u segment=%u offset=%zu stride=%zu key_size=%u\n",
                            f, s, off, stride, (unsigned)key_size);
                    return 0;
                }
                bytes += stride;
            }
        }
    }
    wb->recovered_used_bytes = bytes;
    for(int i = 0; i < MAX_THREAD; i++){
        wb->worker_stats[i].used_delta = 0;
        wbf_snapshot_store(&wb->worker_stats[i].published_used_delta, 0);
    }
    return 1;
}

int write_buffer_segement_new(struct write_buffer *wb, int size,
                              struct write_buffer_free_node **head, int worker){
    if(!wb || !head || worker < 0 || worker >= MAX_THREAD || size <= 0 ||
       size > LIBDB_WBF_MAX_RECORD_SIZE) return 0;
    unsigned q_size = WBF_SLOT_MIN_SIZE;
    while(q_size < (unsigned)size) q_size *= 2;
    struct write_buffer_free_node *node = malloc(sizeof(*node));
    if(!node) return 0;
    pthread_mutex_lock(&g_mutex_lock_wbs);
    while(wb->next_file < wb->file_num &&
          wb->next_segment == wb->preallocated_segments[wb->next_file]){
        wb->next_file++;
        wb->next_segment = 0;
    }
    if(wb->next_file == wb->file_num){
        wb->limit_rejections++;
        fprintf(stderr, "WBF hard limit: file_bytes=%" PRIu64
                " requested_bytes=%" PRIu64 " hard_limit_bytes=%" PRIu64
                " limit_rejections=%" PRIu64 " reason=preallocated_segments_exhausted\n",
                wb->file_bytes, (uint64_t)WSB_SIZE, (uint64_t)LIBDB_WBF_CAPACITY_LIMIT_BYTES,
                wb->limit_rejections);
        goto fail;
    }
    uint32_t f = wb->next_file, s = wb->next_segment;
    if(!wb->pool[f]){

        wb->pool[f] = write_buffer_file_open(f);
        if(!wb->pool[f]) goto fail;
        wb->opened_file_count++;
        uint32_t claimed;
        if(!wbf_validate_file(wb, f, &claimed) || claimed){
            pmemobj_close(wb->pool[f]); wb->pool[f] = NULL; wb->opened_file_count--;
            goto fail;
        }
    }
    struct write_buffer_file_root *root = wbf_root(wb->pool[f]);
    if(!root || root->segment_element_size[s] || wb->reserved_segment_bytes < WSB_SIZE) goto fail;
    struct write_buffer_file_root *manifest = wbf_root(wb->pool[0]);
    if(!manifest) goto fail;
    if(wb->last_used_wbf_seq == WBF_NO_FILE || f > wb->last_used_wbf_seq){
        uint32_t claimed;
        if(!wbf_validate_file(wb, f, &claimed) || claimed) goto fail;
        volatile int committed = 0;
        TX_BEGIN(wb->pool[0]) {
            pmemobj_tx_add_range_direct(&manifest->last_used_wbf_seq, sizeof(manifest->last_used_wbf_seq));
            manifest->last_used_wbf_seq = f;
        } TX_ONCOMMIT { committed = 1; } TX_END
        if(!committed) goto fail;
        wb->last_used_wbf_seq = f;
    }

    root->segment_element_size[s] = q_size;
    pmemobj_persist(wb->pool[f], &root->segment_element_size[s], sizeof(uint16_t));
    node->buffer = root->segment[s];
    node->WBF_seq = f;
    node->size = q_size;
    node->num = WSB_SIZE / q_size;
    node->next = *head;
    wb->next_segment++;
    wb->reserved_segment_bytes -= WSB_SIZE;
    write_buffer_account(wb, worker, 0, WSB_SIZE);
    *head = node;
    write_buffer_raise_pressure_locked(wb);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
    return 1;
fail:
    pthread_mutex_unlock(&g_mutex_lock_wbs);
    free(node);
    return 0;
}

PMEMoid write_buffer_get_free_node(int size , uint16_t * WBF_seq ,struct write_buffer * write_buffer ,
struct write_buffer_free_node ** free_list_head , PMEMobjpool ** pool, int worker){
    struct write_buffer_free_node * pre_node = NULL;
    struct write_buffer_free_node * node = *free_list_head;
    PMEMoid id = OID_NULL;
    while(node != NULL){
        if(node->size >= size && (node->size / 2) < size){
            break;
        }
        else{
            pre_node = node;
            node = node->next;
        }
    }
    if(node == NULL){
        if( write_buffer_segement_new(write_buffer , size , free_list_head, worker) ){
            node = *free_list_head;
            pre_node = NULL;
            if(node->size < size || node->size / 2 >= size){
                printf("new segment wrong size\n");
                return id;
            }
        }
    }
    if(node == NULL){
        printf("node is still NULL\n");
        return OID_NULL;
    }
    id = node->buffer;
    write_buffer_account(write_buffer, worker, node->size, -(int64_t)node->size);
    node->num -- ;
    node->buffer.off += node->size;
    *pool = write_buffer->pool[node->WBF_seq];
    *WBF_seq = node->WBF_seq;
    if(node->num == 0){
        if(pre_node == NULL){
            *free_list_head = node->next;
        }
        else{
            pre_node->next = node->next;
        }
        free(node);
    }
    return id;
}

PMEMoid write_buffer_insert(key_type key, kv_version_type version,
 const char *value, int value_size, struct write_buffer *write_buffer,
 uint16_t *WBF_seq, struct write_buffer_free_node **free_list_head, int thread){
    PMEMoid id;
    PMEMobjpool *pool;
    int size;

    if(value == NULL || value_size < (int)KV_RECORD_VALID_VALUE_SIZE ||
       kv_record_size((size_t)value_size) > LIBDB_WBF_MAX_RECORD_SIZE){
        fprintf(stderr,
                "write_buffer_insert: invalid value or record size (value_size=%d)\n",
                value_size);
        return OID_NULL;
    }
    size = (int)kv_record_size((size_t)value_size);

    id = write_buffer_get_free_node(size, WBF_seq, write_buffer,
                                    free_list_head, &pool, thread);
    if(OID_IS_NULL(id)){
        printf("write buffer no free node\n");
        exit(EXIT_FAILURE);
    }

    char * dest = (char *)pmemobj_direct(id);
    access_nvm();
    if(kv_record_encode(dest, (size_t)size, key, version,
                        value, (size_t)value_size) != 0){
        access_nvm_release();
        fprintf(stderr, "write_buffer_insert: failed to assign PMEM record\n");
        exit(EXIT_FAILURE);
    }
    pmem_persist(dest, KV_RECORD_PERSISTED_SIZE);
    access_nvm_release();
    return id;
}

void write_buffer_update_in_place(PMEMoid id, size_t record_size, key_type key,
    kv_version_type version, const char *value, int value_size){
    if(OID_IS_NULL(id) || value == NULL ||
       value_size < (int)KV_RECORD_VALID_VALUE_SIZE ||
       record_size > LIBDB_WBF_MAX_RECORD_SIZE ||
       record_size != kv_record_size((size_t)value_size)){
        fprintf(stderr, "write_buffer_update_in_place: invalid arguments\n");
        exit(EXIT_FAILURE);
    }
    char *dest = pmemobj_direct(id);
    struct kv_record_view old;
    access_nvm();
    if(kv_record_decode_for_key(dest, record_size, key, &old) != 0 ||
       old.record_size != record_size || old.value_size != value_size){
        access_nvm_release();
        fprintf(stderr, "write_buffer_update_in_place: invalid original record or size mismatch\n");
        exit(EXIT_FAILURE);
    }
    if(kv_record_encode(dest, record_size, key, version, value, (size_t)value_size) != 0){
        access_nvm_release();
        fprintf(stderr, "write_buffer_update_in_place: failed to assign PMEM record\n");
        exit(EXIT_FAILURE);
    }

    pmem_persist(dest, KV_RECORD_PERSISTED_SIZE);
    access_nvm_release();
}

int write_buffer_free(struct write_buffer *write_buffer, PMEMoid id , uint16_t WBF_seq , int size, struct write_buffer_free_node ** free_list_head, int worker){
    int q_size = WBF_SLOT_MIN_SIZE;
    if(!write_buffer || OID_IS_NULL(id) || free_list_head == NULL || size <= 0 ||
       size > LIBDB_WBF_MAX_RECORD_SIZE){
        return 0;
    }
    while(q_size < size){
        q_size *= 2;
    }




    struct write_buffer_free_node * node =
        (struct write_buffer_free_node *)malloc(sizeof(struct write_buffer_free_node));
    if(node == NULL){
        printf("fail to malloc in func write_buffer_free\n");
        return 0;
    }

    node->buffer = id;
    node->num = 1;
    node->size = q_size;
    node->WBF_seq = WBF_seq;
    node->next = *free_list_head;
    *free_list_head = node;
    write_buffer_account(write_buffer, worker, -q_size, q_size);
    return 1;
}
