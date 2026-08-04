/*
 * recovery.c
 *
 * - 从 NVM leaf_node_file（nvm_index）扫描有效 slot，重建 DRAM 索引（dram_index）与 subspace_info。
 * - 可选：全量键扫描/排序，统计重复键。
 * - 从 write_buffer_file 扫描写缓存条目，重建各 subspace 的 write_buffer_index。
 * - 从已恢复的 subspace_info 反推 SSD 块占用 bitmap（g_map）并重建 free_space 的空闲 segment 链表。
 */

#include "recovery.h"
#include "kv_record.h"

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "dram_index.h"
#include "kvbuffer.h"
#include "nvm_index.h"
#include "spdk_struct.h"
#include "write_buffer_index.h"
#include <stdatomic.h>

struct recovered_slot_meta{
    key_type group_boundary;
    key_type min_key;
    key_type max_key;
    PMEMoid slot;
    PMEMobjpool * pool;
    uint8_t slot_key_num;
    uint8_t was_marked;
    uint32_t blocks[SUBSPACE_MAX_KEY];
    int block_num;
};

struct recovered_subspace_tmp{
    struct subspace_info * subspace;
    uint32_t blocks[SUBSPACE_MAX_KEY];
    int block_num;
};

static int cmp_slot_meta(const void *a, const void *b){
    const struct recovered_slot_meta *pa = (const struct recovered_slot_meta *)a;
    const struct recovered_slot_meta *pb = (const struct recovered_slot_meta *)b;
    if(pa->min_key < pb->min_key){
        return -1;
    }
    if(pa->min_key > pb->min_key){
        return 1;
    }
    return 0;
}

static void atomic_max_u64(atomic_uint_fast64_t *obj, uint64_t v){
    uint64_t cur = atomic_load(obj);
    while(v > cur){
        uint64_t expected = cur;
        if(atomic_compare_exchange_weak(obj, &expected, v)){
            return;
        }
        cur = expected;
    }
}

/* write_buffer recovery：共享输出统计（每个 worker 写自己的下标，不冲突） */
static uint64_t *g_wb_total_entries = NULL;
static uint64_t *g_wb_inserted = NULL;
static uint64_t *g_wb_skipped_older = NULL;

/* recovery 阶段的结束 ticks（由 worker atomic_max 写入） */
static atomic_uint_fast64_t *g_index_end_ticks_ptr = NULL;
static atomic_uint_fast64_t *g_wb_end_ticks_ptr = NULL;
static atomic_uint_fast64_t *g_fs_end_ticks_ptr = NULL;
/* Parallel scans take a read lock; persistent split repair takes write lock. */
static pthread_rwlock_t g_recovery_leaf_index_lock = PTHREAD_RWLOCK_INITIALIZER;

/* freespace recovery：并行标记 used_cnt（每个 worker 负责 db->index[t] 的 subspaces） */
static uint32_t *g_fs_used_cnt = NULL; /* shared array: size block_num */
static uint32_t g_fs_block_num = 0;
static atomic_uint_fast64_t g_fs_total_addrs;
static atomic_uint_fast64_t g_fs_bad_addrs;

/* forward decl: defined later */
typedef void (*subspace_visit_fn)(struct subspace_info *s, void *ctx);
static void dram_visit_subspaces(struct dram_index_node *node, subspace_visit_fn fn, void *ctx);
static struct free_space * build_freespace_from_used(const uint32_t *used_cnt, uint32_t block_num);

struct fs_parallel_ctx{
    uint32_t block_num;
    uint32_t * used_cnt; /* shared array */
    uint64_t total_addrs;
    uint64_t bad_addrs;
};

static void fs_mark_used_from_subspace_parallel(struct subspace_info *s, void *vctx){
    struct fs_parallel_ctx *ctx = (struct fs_parallel_ctx *)vctx;
    if(s == NULL || ctx == NULL){
        return;
    }
    for(uint32_t i = 0 ; i < s->slot_num ; i++){
        struct nvm_index_slot *slot =
            (struct nvm_index_slot *)pmemobj_direct(s->slot[i]);
        if(slot == NULL){
            continue;
        }
        for(uint32_t j = 0 ; j < s->slot_key_num[i] ; j++){
            struct address_type ad = slot->entries[j].addr;
            if(ad.size <= 0){
                continue;
            }
            ctx->total_addrs++;
            if(ad.block >= ctx->block_num){
                ctx->bad_addrs++;
                continue;
            }
            __atomic_fetch_add(&ctx->used_cnt[ad.block], 1u, __ATOMIC_RELAXED);
        }
    }
}

static int recovery_block_in_u32_array(uint32_t block, const uint32_t * blocks, int n){
    for(int i = 0 ; i < n ; i++){
        if(blocks[i] == block){
            return 1;
        }
    }
    return 0;
}

static void add_block_unique(uint32_t block, uint32_t * blocks, int * n){
    if(!recovery_block_in_u32_array(block, blocks, *n) && *n < SUBSPACE_MAX_KEY){
        blocks[*n] = block;
        (*n)++;
    }
}

/* forward decl: write buffer entry parsing (defined later) */
static int wbf_entry_parse_sane(const char *p, int q_size, key_type *key_out,
                                int *entry_size_out, kv_version_type *version_out);

/* Return 1 when installed, 0 for an older/equal candidate, -1 for corruption. */
static int recovery_wbf_install_latest(struct subspace_info *ss, key_type key,
                                       int entry_size, PMEMoid id,
                                       uint16_t wbf_seq,
                                       kv_version_type candidate_version){
    struct write_buffer_index_node *existing =
        write_buffer_index_search(ss->buffer_index, key);
    if(existing != NULL){
        const char *old_record = (const char *)pmemobj_direct(existing->id);
        struct kv_record_view old_view;
        if(old_record == NULL ||
           kv_record_decode_for_key(old_record, existing->size, key, &old_view) != 0){
            fprintf(stderr, "recovery: invalid indexed write-buffer record for key=%" PRIu64 "\n",
                    (uint64_t)key);
            return -1;
        }
        if(old_view.version >= candidate_version){
            return 0;
        }
    }

    struct write_buffer_return_type old =
        write_buffer_index_insert(ss->buffer_index, key, entry_size, id, wbf_seq, 0);
    if(ss->buffered_size >= old.size){
        ss->buffered_size -= old.size;
    }
    else{
        ss->buffered_size = 0;
    }
    ss->buffered_size += (uint32_t)entry_size;
    return 1;
}

/* 判断 slot entries 中的有效键数；persistent lower fence 不属于 entries。 */
static int slot_valid_key_num(const struct slot_element *slot){
    if(slot == NULL){
        return 0;
    }
    int n = 0;
    key_type last_key = 0;
    for(int i = 0 ; i < SLOT_MAX_ELEMENT ; i++){
        struct address_type addr = slot[i].addr;
        if(addr.size <= 0 || addr.size > BLOCK_SIZE){
            break;
        }
        if(addr.off < 0 || addr.off >= BLOCK_SIZE || addr.off + addr.size > BLOCK_SIZE){
            break;
        }
        if(n > 0 && slot[i].key <= last_key){
            break;
        }
        last_key = slot[i].key;
        n++;
    }
    return n;
}

static void tmp_init_from_slot(struct recovered_subspace_tmp * tmp, const struct recovered_slot_meta * meta){
    tmp->subspace = subspace_info_initialize();
    tmp->subspace->boundary = meta->group_boundary;
    tmp->subspace->slot_num = 1;
    tmp->subspace->key_num = meta->slot_key_num;
    tmp->subspace->slot[0] = meta->slot;
    tmp->subspace->pool[0] = meta->pool;
    tmp->subspace->min_key[0] = meta->min_key;
    tmp->subspace->slot_key_num[0] = meta->slot_key_num;
    tmp->block_num = meta->block_num;
    memcpy(tmp->blocks, meta->blocks, sizeof(uint32_t) * meta->block_num);
    tmp->subspace->total_block_num = tmp->block_num;
}


static int merge_slot_into_tmp(struct recovered_subspace_tmp * tmp, const struct recovered_slot_meta * meta){
    if(tmp->subspace->slot_num >= SUBSPACE_MAX_SLOT){
        return 0;
    }
    int idx = tmp->subspace->slot_num;
    tmp->subspace->slot[idx] = meta->slot;
    tmp->subspace->pool[idx] = meta->pool;
    tmp->subspace->min_key[idx] = meta->min_key;
    tmp->subspace->slot_key_num[idx] = meta->slot_key_num;
    tmp->subspace->slot_num++;
    tmp->subspace->key_num += meta->slot_key_num;
    for(int i = 0 ; i < meta->block_num ; i++){
        add_block_unique(meta->blocks[i], tmp->blocks, &tmp->block_num);
    }
    tmp->subspace->total_block_num = tmp->block_num;
    return 1;
}

/** 遍历 NVM 上有效 slot：总键数、slot 数、全局最小/最大键（不排序） */
static int scan_nvm_extent(struct db * db, uint64_t * out_keys, int * out_slots,
                           key_type * out_lo, key_type * out_hi){
    struct nvm_index * nvm_index = db->index[0].nvm_index;
    uint64_t tk = 0;
    int ns = 0;
    int has_key = 0;
    key_type glo = 0, ghi = 0;

    access_nvm();
    for(uint32_t file_i = 0 ; file_i < nvm_index->file_num ; file_i++){
        PMEMobjpool * pool = nvm_index->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
        struct leaf_node_file_root * root = (struct leaf_node_file_root *)pmemobj_direct(root_oid);
        for(uint16_t seg_i = 0 ; seg_i < root->segment_num ; seg_i++){
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            int slot_per_seg = LNS_SIZE / LNF_SLOT_MIN_SIZE;
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                int key_num = slot_valid_key_num(persistent_slot->entries);
                if(key_num <= 0){
                    continue;
                }
                ns++;
                tk += (uint64_t)key_num;
                key_type smin = persistent_slot->entries[0].key;
                key_type smax = persistent_slot->entries[key_num - 1].key;
                if(!has_key){
                    glo = smin;
                    ghi = smax;
                    has_key = 1;
                }
                else{
                    if(smin < glo){ glo = smin; }
                    if(smax > ghi){ ghi = smax; }
                }
            }
        }
    }
    access_nvm_release();

    *out_keys = tk;
    *out_slots = ns;
    if(has_key){
        *out_lo = glo;
        *out_hi = ghi;
    }
    return 1;
}

static int collect_all_slots(struct db * db, struct recovered_slot_meta ** out, int * out_n, uint64_t * out_total_keys){
    struct nvm_index * nvm_index = db->index[0].nvm_index;
    int cap = 1024;
    int n = 0;
    uint64_t total_keys = 0;
    struct recovered_slot_meta * arr = (struct recovered_slot_meta *)malloc(sizeof(struct recovered_slot_meta) * cap);
    if(arr == NULL){
        return 0;
    }

    access_nvm();
    for(uint32_t file_i = 0 ; file_i < nvm_index->file_num ; file_i++){
        PMEMobjpool * pool = nvm_index->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
        struct leaf_node_file_root * root = (struct leaf_node_file_root *)pmemobj_direct(root_oid);
        for(uint16_t seg_i = 0 ; seg_i < root->segment_num ; seg_i++){
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            int slot_per_seg = LNS_SIZE / LNF_SLOT_MIN_SIZE;
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                PMEMoid slot_oid = root->segment[seg_i];
                slot_oid.off += (uint64_t)slot_i * LNF_SLOT_MIN_SIZE;
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                int key_num = slot_valid_key_num(persistent_slot->entries);
                if(key_num <= 0){
                    continue;
                }
                if(n == cap){
                    cap *= 2;
                    struct recovered_slot_meta * new_arr = (struct recovered_slot_meta *)realloc(arr, sizeof(struct recovered_slot_meta) * cap);
                    if(new_arr == NULL){
                        access_nvm_release();
                        free(arr);
                        return 0;
                    }
                    arr = new_arr;
                }
                arr[n].group_boundary = persistent_slot->boundary;
                arr[n].min_key = persistent_slot->entries[0].key;
                arr[n].max_key = persistent_slot->entries[key_num - 1].key;
                arr[n].slot = slot_oid;
                arr[n].pool = pool;
                arr[n].slot_key_num = (uint8_t)key_num;
                arr[n].was_marked =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY;
                arr[n].block_num = 0;
                for(int k = 0 ; k < key_num ; k++){
                    add_block_unique(persistent_slot->entries[k].addr.block,
                                     arr[n].blocks, &arr[n].block_num);
                }
                total_keys += (uint64_t)key_num;
                n++;
            }
        }
    }
    access_nvm_release();

    if(out_total_keys){
        *out_total_keys = total_keys;
    }
    *out = arr;
    *out_n = n;
    return 1;
}

static void dram_accumulate_subspaces(struct dram_index_node * node, uint64_t * key_sum, int * sub_cnt, int * bad_slot_sum){
    if(node == NULL){
        return;
    }
    if(!node->is_leaf){
        for(unsigned i = 0 ; i < node->nchild ; i++){
            dram_accumulate_subspaces(node->children[i], key_sum, sub_cnt, bad_slot_sum);
        }
        return;
    }
    for(unsigned i = 0 ; i < node->nchild ; i++){
        struct subspace_info * s = node->subspaces[i];
        (*sub_cnt)++;
        *key_sum += (uint64_t)s->key_num;
        uint32_t ssum = 0;
        for(int j = 0 ; j < (int)s->slot_num ; j++){
            ssum += s->slot_key_num[j];
        }
        if(ssum != s->key_num){
            (*bad_slot_sum)++;
        }
    }
}

static int spot_nvm_lookup(struct db * db, key_type key){
    int t = key_to_thread(key , db);
    struct dram_index_node * leaf_unused;
    struct subspace_info * ss = dram_index_search(db->index[t].dram_index , key , &leaf_unused , NULL);
    if(ss == NULL){
        return 0;
    }
    access_nvm();
    struct address_type ad = nvm_index_search(ss , key);
    access_nvm_release();
    return !ADDR_IS_NULL(ad);
}

/** 返回 1：键数一致、slot 累加与 subspace->key_num 一致、抽样键可查 */
static int recovery_verify_report(struct db * db, uint64_t nvm_keys, int nvm_slots,
                                  key_type key_lo, key_type key_hi, int nvm_nonempty){
    uint64_t dram_keys = 0;
    int dram_subs = 0;
    int bad_slot_sum = 0;

    for(int t = 0 ; t < db->thread ; t++){
        dram_accumulate_subspaces(db->index[t].dram_index->head , &dram_keys , &dram_subs , &bad_slot_sum);
    }

    int keys_match = (dram_keys == nvm_keys);
    int internal_ok = (bad_slot_sum == 0);

    printf("\n======== recovery 校验报告 ========\n");
    printf("NVM: 有效 slot 数 = %d, 键总数 = %" PRIu64 "\n", nvm_slots, nvm_keys);
    printf("DRAM: subspace 个数 = %d（按 worker 汇总）, 键总数 = %" PRIu64 "\n", dram_subs, dram_keys);
    if(nvm_nonempty){
        printf("NVM 键范围（全局）: %" PRIu64 " .. %" PRIu64 "\n", (uint64_t)key_lo, (uint64_t)key_hi);
    }
    if(keys_match){
        printf("键数核对: OK（NVM 与各 subspace->key_num 之和一致）\n");
    }
    else{
        printf("键数核对: **失败** NVM=%" PRIu64 " DRAM 汇总=%" PRIu64 "\n", nvm_keys, dram_keys);
    }
    if(internal_ok){
        printf("subspace 内部: OK（各 slot_key_num 之和 == key_num）\n");
    }
    else{
        printf("subspace 内部: **失败** 有 %d 个子空间 slot 计数与 key_num 不一致\n", bad_slot_sum);
    }

    int spot_lo = 1, spot_hi = 1;
    if(nvm_nonempty){
        spot_lo = spot_nvm_lookup(db , key_lo);
        spot_hi = spot_nvm_lookup(db , key_hi);
        printf("抽样 dram_index + nvm_index 查键: min_key -> %s, max_key -> %s\n",
               spot_lo ? "OK" : "**失败**", spot_hi ? "OK" : "**失败**");
    }
    printf("===================================\n\n");

    return keys_match && internal_ok && spot_lo && spot_hi;
}

int recovery_verify_post(struct db * db){
    uint64_t nvm_keys = 0;
    int nvm_slots = 0;
    key_type klo = 0, khi = 0;

    if(!scan_nvm_extent(db , &nvm_keys , &nvm_slots , &klo , &khi)){
        return 0;
    }
    if(nvm_slots == 0){
        printf("recovery_verify_post: NVM 无有效键，跳过数值校验\n");
        return 1;
    }
    return recovery_verify_report(db , nvm_keys , nvm_slots , klo , khi , 1);
}

static int cmp_key_u64(const void * a, const void * b){
    key_type x = *(const key_type *)a;
    key_type y = *(const key_type *)b;
    if(x < y){
        return -1;
    }
    if(x > y){
        return 1;
    }
    return 0;
}

/**
 * 与 collect/scan 相同的 NVM 遍历：统计每个 slot 内相邻相等键对数，并把所有键写入 buf（调用方保证 buf 足够大）。
 */
static uint64_t nvm_scan_keys_and_within_slot_dups(struct db * db, key_type * buf,
                                                   uint64_t buf_cap, uint64_t * within_adj_dup_out){
    struct nvm_index * nvm_index = db->index[0].nvm_index;
    uint64_t w = 0;
    uint64_t within_adj = 0;

    access_nvm();
    for(uint32_t file_i = 0 ; file_i < nvm_index->file_num ; file_i++){
        PMEMobjpool * pool = nvm_index->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
        struct leaf_node_file_root * root = (struct leaf_node_file_root *)pmemobj_direct(root_oid);
        for(uint16_t seg_i = 0 ; seg_i < root->segment_num ; seg_i++){
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            int slot_per_seg = LNS_SIZE / LNF_SLOT_MIN_SIZE;
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                const struct slot_element *entries = persistent_slot->entries;
                int kn = slot_valid_key_num(entries);
                if(kn <= 0){
                    continue;
                }
                for(int j = 1 ; j < kn ; j++){
                    if(entries[j].key == entries[j - 1].key){
                        within_adj++;
                    }
                }
                if(buf != NULL){
                    for(int j = 0 ; j < kn ; j++){
                        if(w >= buf_cap){
                            access_nvm_release();
                            if(within_adj_dup_out){
                                *within_adj_dup_out = within_adj;
                            }
                            return w;
                        }
                        buf[w++] = entries[j].key;
                    }
                }
            }
        }
    }
    access_nvm_release();
    if(within_adj_dup_out){
        *within_adj_dup_out = within_adj;
    }
    return w;
}

int recovery_report_duplicate_keys(struct db * db){
    uint64_t nvm_keys = 0;
    int nvm_slots = 0;
    key_type klo = 0, khi = 0;

    if(!scan_nvm_extent(db , &nvm_keys , &nvm_slots , &klo , &khi)){
        return 0;
    }
    if(nvm_keys == 0){
        printf("\n-------- recovery 重复键分析 --------\n");
        printf("NVM 上无有效键，跳过。\n");
        printf("------------------------------------\n\n");
        return 1;
    }

    printf("\n-------- recovery 重复键分析 --------\n");
    printf("NVM 总键条数（含可能重复）: %" PRIu64 "\n", nvm_keys);

    if(nvm_keys > (uint64_t)(SIZE_MAX / sizeof(key_type))){
        fprintf(stderr , "recovery 重复键检查: 键数过多无法一次性分配排序缓冲区\n");
        printf("------------------------------------\n\n");
        return 0;
    }

    size_t need = (size_t)nvm_keys;
    key_type * all = (key_type *)malloc(need * sizeof(key_type));
    if(all == NULL){
        fprintf(stderr , "recovery 重复键检查: malloc(%zu 字节) 失败，跳过全局排序查重\n",
               need * sizeof(key_type));
        printf("------------------------------------\n\n");
        return 0;
    }

    uint64_t within_adj = 0;
    uint64_t w = nvm_scan_keys_and_within_slot_dups(db , all , nvm_keys , &within_adj);
    printf("同一 slot 内相邻相等键对数: %" PRIu64 "（正常应为 0；>0 表示 slot 内出现并列键）\n", within_adj);
    if(w != nvm_keys){
        fprintf(stderr , "recovery 重复键检查: 填充键数 %" PRIu64 " 与预期 %" PRIu64 " 不一致\n", w , nvm_keys);
        free(all);
        return 0;
    }

    qsort(all , need , sizeof(key_type) , cmp_key_u64);

    uint64_t distinct = 0;
    if(nvm_keys > 0){
        distinct = 1;
        for(uint64_t i = 1 ; i < nvm_keys ; i++){
            if(all[i] != all[i - 1]){
                distinct++;
            }
        }
    }
    uint64_t redundant = nvm_keys - distinct;

    printf("全局排序后不同键个数: %" PRIu64 "\n", distinct);
    printf("重复出现的键条数（总条数 - 不同键数）: %" PRIu64 "\n", redundant);
    if(redundant > 0){
        printf("说明: 若你预期约 100M 次插入、唯一键约 100M，则此处「不同键」应接近 100M；\n");
        printf("      「重复条数」>0 表示同一键在 NVM 索引中出现多次（跨 slot 或同 slot 非相邻）。\n");
        int shown = 0;
        for(uint64_t i = 1 ; i < nvm_keys && shown < 8 ; i++){
            if(all[i] == all[i - 1]){
                printf("  重复键示例 #%d: %" PRIu64 "\n", shown + 1, (uint64_t)all[i]);
                shown++;
                while(i + 1 < nvm_keys && all[i + 1] == all[i]){
                    i++;
                }
            }
        }
    }
    else{
        printf("全局无重复键（每条键在 NVM 中唯一）。\n");
    }

    free(all);
    printf("------------------------------------\n\n");
    return 1;
}

struct recovery_slot_owner_scan_ctx{
    struct db *db;
    int t; /* 本 worker 负责的线程区间 */
    struct recovered_slot_meta *arr;
    int n;
    int cap;
    int scan_ok;
    int group_build_ok;
    uint64_t total_keys;
    key_type key_lo;
    key_type key_hi;
    atomic_int done;
};

static void recovery_scan_ctx_append_slot(struct recovery_slot_owner_scan_ctx *ctx, const struct recovered_slot_meta *meta){
    if(ctx->n >= ctx->cap){
        int new_cap = ctx->cap == 0 ? 256 : (ctx->cap * 2);
        struct recovered_slot_meta *new_arr =
            (struct recovered_slot_meta *)realloc(ctx->arr, sizeof(struct recovered_slot_meta) * (size_t)new_cap);
        if(new_arr == NULL){
            ctx->scan_ok = 0;
            return;
        }
        ctx->arr = new_arr;
        ctx->cap = new_cap;
    }
    ctx->arr[ctx->n] = *meta;
    ctx->n++;
    ctx->total_keys += (uint64_t)meta->slot_key_num;
    if(ctx->n == 1){
        ctx->key_lo = meta->min_key;
        ctx->key_hi = meta->max_key;
    }
    else{
        if(meta->min_key < ctx->key_lo){ ctx->key_lo = meta->min_key; }
        if(meta->max_key > ctx->key_hi){ ctx->key_hi = meta->max_key; }
    }
}

struct recovered_key_range{
    key_type min_key;
    key_type max_key;
    int valid;
};

static int recovery_ranges_overlap(const struct recovered_key_range *a,
                                   const struct recovered_key_range *b){
    return a->valid && b->valid &&
           a->min_key <= b->max_key && b->min_key <= a->max_key;
}

static int recovery_range_covers(const struct recovered_key_range *outer,
                                 const struct recovered_key_range *inner){
    return outer->valid && inner->valid &&
           outer->min_key <= inner->min_key &&
           outer->max_key >= inner->max_key;
}

static int recovery_group_range(
    const struct recovery_slot_owner_scan_ctx *ctx, key_type boundary,
    struct recovered_key_range *range){
    range->valid = 0;
    for(int i = 0 ; i < ctx->n ; i++){
        const struct recovered_slot_meta *meta = &ctx->arr[i];
        if(meta->group_boundary != boundary){
            continue;
        }
        if(!range->valid){
            range->min_key = meta->min_key;
            range->max_key = meta->max_key;
            range->valid = 1;
        }
        else{
            if(meta->min_key < range->min_key){
                range->min_key = meta->min_key;
            }
            if(meta->max_key > range->max_key){
                range->max_key = meta->max_key;
            }
        }
    }
    return range->valid;
}

static int recovery_find_slot_meta(
    const struct recovery_slot_owner_scan_ctx *ctx, PMEMoid slot){
    for(int i = 0 ; i < ctx->n ; i++){
        if(OID_EQUALS(ctx->arr[i].slot, slot)){
            return i;
        }
    }
    return -1;
}

static int recovery_find_left_boundary(
    const struct recovery_slot_owner_scan_ctx *ctx, int marker_index,
    key_type *boundary_out){
    for(int i = marker_index - 1 ; i >= 0 ; i--){
        if(ctx->arr[i].group_boundary != LIBDB_INVALID_GROUP_BOUNDARY){
            *boundary_out = ctx->arr[i].group_boundary;
            return 1;
        }
    }
    return 0;
}

static int recovery_find_right_boundary(
    const struct recovery_slot_owner_scan_ctx *ctx, key_type left_boundary,
    key_type *boundary_out){
    int found = 0;
    key_type right_boundary = KEY_MAX;
    for(int i = 0 ; i < ctx->n ; i++){
        key_type boundary = ctx->arr[i].group_boundary;
        if(boundary != LIBDB_INVALID_GROUP_BOUNDARY &&
           boundary > left_boundary &&
           (!found || boundary < right_boundary)){
            right_boundary = boundary;
            found = 1;
        }
    }
    if(found){
        *boundary_out = right_boundary;
    }
    return found;
}

static void recovery_refresh_slot_meta(
    struct recovered_slot_meta *meta,
    const struct nvm_index_slot *image, uint8_t key_num){
    meta->group_boundary = image->boundary;
    meta->slot_key_num = key_num;
    meta->min_key = image->entries[0].key;
    meta->max_key = image->entries[key_num - 1].key;
    meta->block_num = 0;
    for(uint8_t i = 0 ; i < key_num ; i++){
        add_block_unique(image->entries[i].addr.block,
                         meta->blocks, &meta->block_num);
    }
}

static int recovery_load_slot_image(
    const struct recovered_slot_meta *meta,
    struct nvm_index_slot *image){
    const struct nvm_index_slot *persistent_slot =
        (const struct nvm_index_slot *)pmemobj_direct(meta->slot);
    if(persistent_slot == NULL){
        return 0;
    }
    memcpy(image, persistent_slot, sizeof(*image));
    return image->boundary == meta->group_boundary &&
           slot_valid_key_num(image->entries) == meta->slot_key_num;
}

static int recovery_commit_meta_image(
    struct recovery_slot_owner_scan_ctx *ctx, int meta_index,
    const struct nvm_index_slot *image, uint8_t key_num){
    if(meta_index < 0 || meta_index >= ctx->n || key_num == 0 ||
       key_num > SLOT_MAX_ELEMENT){
        return 0;
    }

    struct nvm_slot_update update = {
        .slot = ctx->arr[meta_index].slot,
        .image = *image,
    };
    if(nvm_index_commit_slot_updates(
           ctx->db->index[0].nvm_index, &update, 1) != 0){
        return 0;
    }
    recovery_refresh_slot_meta(&ctx->arr[meta_index], image, key_num);
    return 1;
}

static int recovery_commit_meta_boundary(
    struct recovery_slot_owner_scan_ctx *ctx, int meta_index,
    key_type boundary){
    struct nvm_index_slot image;
    if(meta_index < 0 || meta_index >= ctx->n ||
       !recovery_load_slot_image(&ctx->arr[meta_index], &image)){
        return 0;
    }
    image.boundary = boundary;
    return recovery_commit_meta_image(
        ctx, meta_index, &image, ctx->arr[meta_index].slot_key_num);
}

static int recovery_partition_marker_image(
    const struct recovered_slot_meta *marker, key_type split_boundary,
    key_type left_boundary, struct nvm_index_slot *left_image,
    uint8_t *left_count_out, struct nvm_index_slot *right_image,
    uint8_t *right_count_out){
    struct nvm_index_slot original;
    if(!recovery_load_slot_image(marker, &original)){
        return 0;
    }

    uint8_t split_off = 0;
    while(split_off < marker->slot_key_num &&
          original.entries[split_off].key < split_boundary){
        split_off++;
    }
    if(split_off == 0 || split_off >= marker->slot_key_num ||
       original.entries[split_off].key != split_boundary){
        return 0;
    }

    uint8_t right_count = marker->slot_key_num - split_off;
    *left_image = original;
    left_image->boundary = left_boundary;
    memset(&left_image->entries[split_off], 0,
           (SLOT_MAX_ELEMENT - split_off) * sizeof(struct slot_element));

    memset(right_image, 0, sizeof(*right_image));
    right_image->boundary = split_boundary;
    memcpy(right_image->entries, &original.entries[split_off],
           (size_t)right_count * sizeof(struct slot_element));

    *left_count_out = split_off;
    *right_count_out = right_count;
    return 1;
}

static int recovery_repair_one_marker(
    struct recovery_slot_owner_scan_ctx *ctx, PMEMoid marker_oid){
    qsort(ctx->arr, (size_t)ctx->n,
          sizeof(struct recovered_slot_meta), cmp_slot_meta);

    int marker_index = recovery_find_slot_meta(ctx, marker_oid);
    if(marker_index < 0){
        return 0;
    }
    if(ctx->arr[marker_index].group_boundary !=
       LIBDB_INVALID_GROUP_BOUNDARY){
        return 1;
    }

    key_type left_boundary = 0;
    key_type right_boundary = 0;
    if(!recovery_find_left_boundary(ctx, marker_index, &left_boundary)){
        fprintf(stderr, "recovery split: marked slot has no left group\n");
        return 0;
    }

    struct recovered_key_range marker_range = {
        .min_key = ctx->arr[marker_index].min_key,
        .max_key = ctx->arr[marker_index].max_key,
        .valid = 1,
    };
    struct recovered_key_range left_range = {0};
    struct recovered_key_range right_range = {0};
    if(!recovery_group_range(ctx, left_boundary, &left_range)){
        return 0;
    }
    int has_right = recovery_find_right_boundary(
        ctx, left_boundary, &right_boundary);
    if(has_right &&
       !recovery_group_range(ctx, right_boundary, &right_range)){
        return 0;
    }

    int left_covers_marker =
        recovery_range_covers(&left_range, &marker_range);
    int groups_overlap =
        has_right && recovery_ranges_overlap(&left_range, &right_range);
    int left_covers_right =
        has_right && recovery_range_covers(&left_range, &right_range);
    int marker_overlaps_left =
        recovery_ranges_overlap(&marker_range, &left_range);
    int marker_overlaps_right =
        has_right && recovery_ranges_overlap(&marker_range, &right_range);
    int marker_is_aligned =
        has_right && marker_range.min_key == right_boundary;

    if(left_covers_marker && (!has_right || !groups_overlap)){
        if(!recovery_commit_meta_boundary(
               ctx, marker_index, left_boundary)){
            return 0;
        }
        printf("recovery split: state 1, rolled parent group back "
               "(marker off=%" PRIu64 ")\n", marker_oid.off);
        return 1;
    }

    if(left_covers_right){
        for(int i = 0 ; i < ctx->n ; i++){
            if(ctx->arr[i].group_boundary == right_boundary &&
               !recovery_commit_meta_boundary(ctx, i, left_boundary)){
                return 0;
            }
        }
        marker_index = recovery_find_slot_meta(ctx, marker_oid);
        if(!recovery_commit_meta_boundary(
               ctx, marker_index, left_boundary)){
            return 0;
        }
        printf("recovery split: state 2, merged partial right child "
               "back into parent (marker off=%" PRIu64 ")\n",
               marker_oid.off);
        return 1;
    }

    if(!has_right){
        fprintf(stderr,
                "recovery split: marked slot has no right group for "
                "roll-forward\n");
        return 0;
    }

    /*
     * Aligned split: f' is already the smallest key in s. Once both child
     * indexes are durable, the marked s is absent from both reconstructed
     * groups, so its entry range is disjoint from both children. Distinguish
     * this from an internal split by requiring min(s) == f'. No entries are
     * partitioned: atomically replacing the marker with f' assigns the
     * original slot to the right child and completes the split.
     */
    if(!marker_overlaps_left && !marker_overlaps_right &&
       marker_is_aligned){
        if(!recovery_commit_meta_boundary(
               ctx, marker_index, right_boundary)){
            return 0;
        }
        printf("recovery split: aligned state 3, assigned marked slot "
               "to right child (marker off=%" PRIu64 ")\n",
               marker_oid.off);
        return 1;
    }

    struct nvm_index_slot left_image;
    struct nvm_index_slot right_image;
    uint8_t left_count = 0;
    uint8_t right_count = 0;
    if(!marker_overlaps_left &&
       !recovery_partition_marker_image(
           &ctx->arr[marker_index], right_boundary, left_boundary,
           &left_image, &left_count, &right_image, &right_count)){
        fprintf(stderr,
                "recovery split: cannot partition marker at right fence=%"
                PRIu64 "\n", (uint64_t)right_boundary);
        return 0;
    }

    if(!marker_overlaps_left && !marker_overlaps_right){
        struct nvm_index_free_node **free_head =
            &ctx->db->recovery_index_free_heads[ctx->t];
        if(!nvm_index_reserve_free_nodes(
               ctx->db->index[0].nvm_index, free_head, 1)){
            return 0;
        }

        PMEMobjpool *new_pool = NULL;
        PMEMoid new_slot = nvm_index_get_free_node(
            ctx->db->index[0].nvm_index, free_head, &new_pool);
        if(OID_IS_NULL(new_slot) || new_pool == NULL){
            return 0;
        }

        struct nvm_slot_update new_update = {
            .slot = new_slot,
            .image = right_image,
        };
        if(nvm_index_commit_slot_updates(
               ctx->db->index[0].nvm_index, &new_update, 1) != 0){
            return 0;
        }

        struct recovered_slot_meta new_meta = {
            .group_boundary = right_boundary,
            .slot = new_slot,
            .pool = new_pool,
            .was_marked = 0,
        };
        recovery_refresh_slot_meta(&new_meta, &right_image, right_count);
        int old_n = ctx->n;
        recovery_scan_ctx_append_slot(ctx, &new_meta);
        if(!ctx->scan_ok || ctx->n != old_n + 1){
            return 0;
        }

        marker_index = recovery_find_slot_meta(ctx, marker_oid);
        if(!recovery_commit_meta_image(
               ctx, marker_index, &left_image, left_count)){
            return 0;
        }
        ctx->total_keys -= right_count;
        printf("recovery split: state 3, created missing right slot and "
               "rolled split forward (marker off=%" PRIu64 ")\n",
               marker_oid.off);
        return 1;
    }

    if(!marker_overlaps_left && marker_overlaps_right){
        if(!recovery_commit_meta_image(
               ctx, marker_index, &left_image, left_count)){
            return 0;
        }
        ctx->total_keys -= right_count;
        printf("recovery split: state 4, finalized marked slot and "
               "rolled split forward (marker off=%" PRIu64 ")\n",
               marker_oid.off);
        return 1;
    }

    fprintf(stderr,
            "recovery split: marker/group ranges do not match any "
            "recoverable state (marker off=%" PRIu64 ")\n",
            marker_oid.off);
    return 0;
}

static int recovery_repair_marked_splits(
    struct recovery_slot_owner_scan_ctx *ctx){
    if(ctx == NULL || !ctx->scan_ok){
        return 0;
    }
    uint32_t marker_count = 0;
    for(int i = 0 ; i < ctx->n ; i++){
        if(ctx->arr[i].group_boundary ==
           LIBDB_INVALID_GROUP_BOUNDARY){
            marker_count++;
        }
    }
    if(marker_count == 0){
        return 1;
    }

    PMEMoid *markers =
        (PMEMoid *)malloc((size_t)marker_count * sizeof(*markers));
    if(markers == NULL){
        return 0;
    }

    uint32_t pos = 0;
    for(int i = 0 ; i < ctx->n ; i++){
        if(ctx->arr[i].group_boundary ==
           LIBDB_INVALID_GROUP_BOUNDARY){
            markers[pos++] = ctx->arr[i].slot;
        }
    }

    int ok = 1;
    for(uint32_t i = 0 ; i < marker_count ; i++){
        if(!recovery_repair_one_marker(ctx, markers[i])){
            ok = 0;
            break;
        }
    }
    free(markers);
    return ok;
}

static uint32_t recovery_count_unresolved_markers(
    struct recovery_slot_owner_scan_ctx *ctxs, int thread_num){
    uint32_t count = 0;
    for(int t = 0 ; t < thread_num ; t++){
        for(int i = 0 ; i < ctxs[t].n ; i++){
            if(ctxs[t].arr[i].group_boundary ==
               LIBDB_INVALID_GROUP_BOUNDARY){
                count++;
            }
        }
    }
    return count;
}

static int recovery_prepare_index_free_heads(struct db *db){
    if(db->recovery_index_free_heads != NULL){
        return 1;
    }
    db->recovery_index_free_heads =
        (struct nvm_index_free_node **)calloc(
            (size_t)db->thread, sizeof(*db->recovery_index_free_heads));
    return db->recovery_index_free_heads != NULL;
}

/*
 * Slots are globally ordered by their smallest entry key.  Group membership is
 * recovered exclusively from the persistent lower fence; block overlap and a
 * target key count are not group identifiers.
 *
 * Split repair runs before this function. Any still-marked slot is omitted so
 * an unresolved split cannot be routed through the DRAM index; historically
 * marked PMEMoids remain available in db for diagnostics after workers join.
 */
static int recovery_build_dram_groups(struct recovery_slot_owner_scan_ctx *ctx){
    if(ctx == NULL || ctx->db == NULL || !ctx->scan_ok){
        return 0;
    }
    if(ctx->n == 0){
        return 1;
    }

    qsort(ctx->arr, (size_t)ctx->n,
          sizeof(struct recovered_slot_meta), cmp_slot_meta);

    struct recovered_subspace_tmp curr;
    int has_curr = 0;
    for(int i = 0 ; i < ctx->n ; i++){
        const struct recovered_slot_meta *meta = &ctx->arr[i];
        if(meta->group_boundary == LIBDB_INVALID_GROUP_BOUNDARY){
            continue;
        }

        if(!has_curr){
            tmp_init_from_slot(&curr, meta);
            has_curr = 1;
            continue;
        }

        if(meta->group_boundary == curr.subspace->boundary){
            if(!merge_slot_into_tmp(&curr, meta)){
                fprintf(stderr,
                        "recovery: group boundary=%" PRIu64
                        " exceeds SUBSPACE_MAX_SLOT while merging slots\n",
                        (uint64_t)meta->group_boundary);
                return 0;
            }
            continue;
        }

        if(meta->group_boundary < curr.subspace->boundary){
            fprintf(stderr,
                    "recovery: non-monotonic group fences remain after "
                    "split repair (%" PRIu64 " after %" PRIu64 ")\n",
                    (uint64_t)meta->group_boundary,
                    (uint64_t)curr.subspace->boundary);
            return 0;
        }
        dram_index_insert(ctx->db->index[ctx->t].dram_index,
                          curr.subspace->boundary, curr.subspace, NULL);
        tmp_init_from_slot(&curr, meta);
    }

    if(has_curr){
        dram_index_insert(ctx->db->index[ctx->t].dram_index,
                          curr.subspace->boundary, curr.subspace, NULL);
    }
    return 1;
}

static int recovery_store_marked_slot_addresses(
    struct db *db, struct recovery_slot_owner_scan_ctx *ctxs, int thread_num){
    uint32_t marked_count = 0;
    for(int t = 0 ; t < thread_num ; t++){
        for(int i = 0 ; i < ctxs[t].n ; i++){
            if(ctxs[t].arr[i].was_marked){
                marked_count++;
            }
        }
    }

    PMEMoid *marked_slots = NULL;
    if(marked_count != 0){
        marked_slots = (PMEMoid *)malloc(
            (size_t)marked_count * sizeof(*marked_slots));
        if(marked_slots == NULL){
            return 0;
        }
    }

    uint32_t pos = 0;
    for(int t = 0 ; t < thread_num ; t++){
        for(int i = 0 ; i < ctxs[t].n ; i++){
            if(ctxs[t].arr[i].was_marked){
                marked_slots[pos++] = ctxs[t].arr[i].slot;
            }
        }
    }

    free(db->recovery_marked_slots);
    db->recovery_marked_slots = marked_slots;
    db->recovery_marked_slot_count = marked_count;
    printf("recovery: recorded %u marked split slot address(es)\n",
           marked_count);
    return 1;
}

static void *recovery_scan_and_build_owner_worker(void *arg){
    struct recovery_slot_owner_scan_ctx *ctx = (struct recovery_slot_owner_scan_ctx *)arg;
    struct db *db = ctx->db;
    int t = ctx->t;
    struct nvm_index *nvm_index = db->index[0].nvm_index;
    if(nvm_index == NULL){
        atomic_store(&ctx->done, 1);
        return NULL;
    }

    int slot_per_seg = LNS_SIZE / LNF_SLOT_MIN_SIZE;

    pthread_rwlock_rdlock(&g_recovery_leaf_index_lock);
    access_nvm();
    for(uint32_t file_i = 0 ; file_i < nvm_index->file_num ; file_i++){
        PMEMobjpool * pool = nvm_index->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
        struct leaf_node_file_root * root = (struct leaf_node_file_root *)pmemobj_direct(root_oid);
        for(uint16_t seg_i = 0 ; seg_i < root->segment_num ; seg_i++){
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            if(seg_base == NULL){
                continue;
            }

            /* 判定该 NVM segment 属于哪个 worker（检查 segment 内任意一个有效 slot） */
            int owner_t = -1;
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                int key_num = slot_valid_key_num(persistent_slot->entries);
                if(key_num <= 0){
                    continue;
                }
                key_type routing_key =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY ?
                    persistent_slot->entries[0].key : persistent_slot->boundary;
                owner_t = key_to_thread(routing_key, db);
                break;
            }
            if(owner_t != t){
                continue;
            }

            /* 属于本 worker：收集该 segment 内所有有效 slot */
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                const struct slot_element *entries = persistent_slot->entries;
                int key_num = slot_valid_key_num(entries);
                if(key_num <= 0){
                    continue;
                }

                key_type routing_key =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY ?
                    entries[0].key : persistent_slot->boundary;
                int slot_owner = key_to_thread(routing_key, db);
                if(slot_owner != t){
                    continue;
                }

                struct recovered_slot_meta meta;
                meta.group_boundary = persistent_slot->boundary;
                meta.min_key = entries[0].key;
                meta.max_key = entries[key_num - 1].key;
                meta.pool = pool;
                meta.slot_key_num = (uint8_t)key_num;
                meta.was_marked =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY;
                meta.block_num = 0;
                for(int k = 0 ; k < key_num ; k++){
                    add_block_unique(entries[k].addr.block, meta.blocks, &meta.block_num);
                }
                meta.slot = root->segment[seg_i];
                meta.slot.off += (uint64_t)slot_i * LNF_SLOT_MIN_SIZE;

                recovery_scan_ctx_append_slot(ctx, &meta);
            }
        }
    }
    access_nvm_release();
    pthread_rwlock_unlock(&g_recovery_leaf_index_lock);

    pthread_rwlock_wrlock(&g_recovery_leaf_index_lock);
    int split_repair_ok = recovery_repair_marked_splits(ctx);
    pthread_rwlock_unlock(&g_recovery_leaf_index_lock);

    /* build dram_index/subspace_info (only for this t) */
    ctx->group_build_ok =
        split_repair_ok && recovery_build_dram_groups(ctx);
    if(!ctx->group_build_ok){
        fprintf(stderr, "recovery: failed to rebuild DRAM groups for worker %d\n", t);
    }

    atomic_store(&ctx->done, 1);
    return NULL;
}

int recovery_rebuild_subspaces(struct db * db){
    if(db == NULL || db->index == NULL){
        return 0;
    }
    if(db->index[0].dram_index == NULL || db->index[0].nvm_index == NULL){
        return 0;
    }

    int thread_num = db->thread;
    if(thread_num <= 0){
        return 0;
    }
    if(!recovery_prepare_index_free_heads(db)){
        return 0;
    }

    int main_core = spdk_env_get_current_core();

    struct recovery_slot_owner_scan_ctx *scan_ctxs =
        (struct recovery_slot_owner_scan_ctx *)calloc((size_t)thread_num, sizeof(struct recovery_slot_owner_scan_ctx));
    if(scan_ctxs == NULL){
        return 0;
    }

    /* 单次 pin：每个 worker 从 scan 到 build 都在同一个 pinned core 内完成 */
    for(int t = 0 ; t < thread_num ; t++){
        scan_ctxs[t].db = db;
        scan_ctxs[t].t = t;
        scan_ctxs[t].arr = NULL;
        scan_ctxs[t].n = 0;
        scan_ctxs[t].cap = 0;
        scan_ctxs[t].total_keys = 0;
        scan_ctxs[t].key_lo = KEY_MAX;
        scan_ctxs[t].key_hi = 0;
        scan_ctxs[t].scan_ok = 1;
        scan_ctxs[t].group_build_ok = 1;
        atomic_init(&scan_ctxs[t].done, 0);
    }

    for(int t = 0 ; t < thread_num ; t++){
        int core = t + 1;
        if(core == main_core){
            core++;
        }
        if(spdk_env_thread_launch_pinned(core,
                (thread_start_fn)recovery_scan_and_build_owner_worker,
                &scan_ctxs[t])){
            recovery_scan_and_build_owner_worker(&scan_ctxs[t]);
        }
    }

    for(int t = 0 ; t < thread_num ; t++){
        while(atomic_load(&scan_ctxs[t].done) == 0){
            /* busy wait */
        }
    }

    int workers_ok = 1;
    for(int t = 0 ; t < thread_num ; t++){
        if(!scan_ctxs[t].scan_ok || !scan_ctxs[t].group_build_ok){
            workers_ok = 0;
        }
    }

    int marked_slots_ok =
        recovery_store_marked_slot_addresses(db, scan_ctxs, thread_num);
    if(!marked_slots_ok){
        fprintf(stderr,
                "recovery: failed to retain marked split-slot addresses\n");
    }
    uint32_t unresolved_markers =
        recovery_count_unresolved_markers(scan_ctxs, thread_num);

    /* 汇总用于校验：key_hi/key_lo 与 slot_n/total_keys */
    uint64_t total_keys = 0;
    int slot_n = 0;
    key_type key_lo = KEY_MAX;
    key_type key_hi = 0;
    for(int t = 0 ; t < thread_num ; t++){
        total_keys += scan_ctxs[t].total_keys;
        slot_n += scan_ctxs[t].n;
        if(scan_ctxs[t].n > 0){
            if(scan_ctxs[t].key_lo < key_lo){ key_lo = scan_ctxs[t].key_lo; }
            if(scan_ctxs[t].key_hi > key_hi){ key_hi = scan_ctxs[t].key_hi; }
        }
    }

    int verify_ok = 0;
    if(workers_ok && marked_slots_ok && unresolved_markers == 0){
        verify_ok = recovery_verify_report(
            db, total_keys, slot_n, key_lo, key_hi, 1);
    }
    else if(workers_ok && marked_slots_ok &&
            unresolved_markers != 0){
        fprintf(stderr,
                "recovery: %u split marker(s) remain after repair\n",
                unresolved_markers);
    }

    /* cleanup */
    for(int t = 0 ; t < thread_num ; t++){
        free(scan_ctxs[t].arr);
    }
    free(scan_ctxs);
    return verify_ok;
}

/* ---------------- write buffer index recovery ---------------- */

static void recovery_rebuild_wb_for_thread(struct db *db, int t, uint64_t *out_entries,
                                           uint64_t *out_inserted, uint64_t *out_skipped_older){
    uint64_t total_entries = 0;
    uint64_t inserted = 0;
    uint64_t skipped_older = 0;

    access_nvm();
    for(uint32_t file_i = 0 ; file_i < db->write_buffer->file_num ; file_i++){
        PMEMobjpool * pool = db->write_buffer->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct write_buffer_file_root));
        if(OID_IS_NULL(root_oid)){
            continue;
        }
        struct write_buffer_file_root * root = (struct write_buffer_file_root *)pmemobj_direct(root_oid);
        uint16_t seg_num = root->segment_num;
        for(uint16_t seg_i = 0 ; seg_i < seg_num ; seg_i++){
            int q_size = (int)root->segment_element_size[seg_i];
            if(q_size <= 0){
                continue;
            }
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            if(seg_base == NULL){
                continue;
            }

            /* 判断该 write_buffer segment 属于哪个 worker（用第一个可解析 entry 的 key） */
            int seg_owner_t = -1;
            key_type seg_key = 0;
            int first_entry_size = 0;
            for(int off0 = 0 ; off0 + q_size <= WSB_SIZE ; off0 += q_size){
                int ok0 = wbf_entry_parse_sane(seg_base + off0, q_size, &seg_key, &first_entry_size, NULL);
                if(ok0 == 1){
                    seg_owner_t = key_to_thread(seg_key, db);
                    break;
                }
                if(ok0 == -1){
                    continue; /* freed slot; later fixed-size slots may be live */
                }
                if(ok0 == 0){
                    break;
                }
            }
            if(seg_owner_t != t){
                continue;
            }

            /* 属于本 worker：逐 entry 恢复写缓存索引 */
            for(int off = 0 ; off + q_size <= WSB_SIZE ; off += q_size){
                key_type key;
                int entry_size;
                kv_version_type version;
                int ok = wbf_entry_parse_sane(seg_base + off, q_size, &key, &entry_size, &version);
                if(ok == -1){
                    continue; /* key_size=0 marks only this slot invalid */
                }
                if(ok == 0){
                    break;
                }
                total_entries++;

                if(key_to_thread(key, db) != t){
                    continue;
                }

                struct dram_index_node * leaf_unused = NULL;
                struct subspace_info * ss = dram_index_search(db->index[t].dram_index, key, &leaf_unused, NULL);
                if(ss == NULL || ss->buffer_index == NULL){
                    continue;
                }

                PMEMoid id = root->segment[seg_i];
                id.off += (uint64_t)off;

                int install_rc = recovery_wbf_install_latest(
                    ss, key, entry_size, id, (uint16_t)file_i, version);
                if(install_rc < 0){
                    exit(EXIT_FAILURE);
                }
                if(install_rc == 0){
                    skipped_older++;
                }
                else{
                    inserted++;
                }
            }
        }
    }
    access_nvm_release();

    if(out_entries){
        *out_entries = total_entries;
    }
    if(out_inserted){
        *out_inserted = inserted;
    }
    if(out_skipped_older){
        *out_skipped_older = skipped_older;
    }
}

static void *recovery_scan_and_build_index_and_wb_owner_worker(void *arg){
    struct recovery_slot_owner_scan_ctx *ctx = (struct recovery_slot_owner_scan_ctx *)arg;
    struct db *db = ctx->db;
    int t = ctx->t;
    struct nvm_index *nvm_index = db->index[0].nvm_index;
    if(nvm_index == NULL){
        atomic_store(&ctx->done, 1);
        return NULL;
    }

    int slot_per_seg = LNS_SIZE / LNF_SLOT_MIN_SIZE;

    /* -------- scan stage: 收集本 worker 的 slots meta -------- */
    pthread_rwlock_rdlock(&g_recovery_leaf_index_lock);
    access_nvm();
    for(uint32_t file_i = 0 ; file_i < nvm_index->file_num ; file_i++){
        PMEMobjpool * pool = nvm_index->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
        struct leaf_node_file_root * root = (struct leaf_node_file_root *)pmemobj_direct(root_oid);
        for(uint16_t seg_i = 0 ; seg_i < root->segment_num ; seg_i++){
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            if(seg_base == NULL){
                continue;
            }

            int owner_t = -1;
            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                int key_num = slot_valid_key_num(persistent_slot->entries);
                if(key_num <= 0){
                    continue;
                }
                key_type routing_key =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY ?
                    persistent_slot->entries[0].key : persistent_slot->boundary;
                owner_t = key_to_thread(routing_key, db);
                break;
            }
            if(owner_t != t){
                continue;
            }

            for(int slot_i = 0 ; slot_i < slot_per_seg ; slot_i++){
                struct nvm_index_slot *persistent_slot =
                    (struct nvm_index_slot *)(seg_base +
                        (uint64_t)slot_i * LNF_SLOT_MIN_SIZE);
                const struct slot_element *entries = persistent_slot->entries;
                int key_num = slot_valid_key_num(entries);
                if(key_num <= 0){
                    continue;
                }

                key_type routing_key =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY ?
                    entries[0].key : persistent_slot->boundary;
                int slot_owner = key_to_thread(routing_key, db);
                if(slot_owner != t){
                    continue;
                }

                struct recovered_slot_meta meta;
                meta.group_boundary = persistent_slot->boundary;
                meta.min_key = entries[0].key;
                meta.max_key = entries[key_num - 1].key;
                meta.pool = pool;
                meta.slot_key_num = (uint8_t)key_num;
                meta.was_marked =
                    persistent_slot->boundary == LIBDB_INVALID_GROUP_BOUNDARY;
                meta.block_num = 0;
                for(int k = 0 ; k < key_num ; k++){
                    add_block_unique(entries[k].addr.block, meta.blocks, &meta.block_num);
                }
                meta.slot = root->segment[seg_i];
                meta.slot.off += (uint64_t)slot_i * LNF_SLOT_MIN_SIZE;

                recovery_scan_ctx_append_slot(ctx, &meta);
            }
        }
    }
    access_nvm_release();
    pthread_rwlock_unlock(&g_recovery_leaf_index_lock);

    pthread_rwlock_wrlock(&g_recovery_leaf_index_lock);
    int split_repair_ok = recovery_repair_marked_splits(ctx);
    pthread_rwlock_unlock(&g_recovery_leaf_index_lock);

    /* -------- build index stage -------- */
    ctx->group_build_ok =
        split_repair_ok && recovery_build_dram_groups(ctx);
    if(!ctx->group_build_ok){
        fprintf(stderr, "recovery: failed to rebuild DRAM groups for worker %d\n", t);
        atomic_store(&ctx->done, 1);
        return NULL;
    }

    if(g_index_end_ticks_ptr){
        atomic_max_u64(g_index_end_ticks_ptr, spdk_get_ticks());
    }

    /* -------- rebuild write_buffer_index stage -------- */
    uint64_t local_entries = 0, local_inserted = 0, local_skipped_older = 0;
    recovery_rebuild_wb_for_thread(db, t, &local_entries, &local_inserted,
                                   &local_skipped_older);
    if(g_wb_total_entries){
        g_wb_total_entries[t] = local_entries;
    }
    if(g_wb_inserted){
        g_wb_inserted[t] = local_inserted;
    }
    if(g_wb_skipped_older){
        g_wb_skipped_older[t] = local_skipped_older;
    }

    if(g_wb_end_ticks_ptr){
        atomic_max_u64(g_wb_end_ticks_ptr, spdk_get_ticks());
    }

    /* -------- mark SSD used blocks stage (parallel) --------
     * 每个 worker 只遍历自己线程 t 的 dram_index subspaces。
     * used_cnt 用 __atomic_fetch_add 做无锁累加；后续主线程统一 build g_map/free_space。
     */
    if(g_fs_used_cnt != NULL && g_fs_block_num != 0 && db->index[t].dram_index != NULL){
        access_nvm();
        struct fs_parallel_ctx local_fs_ctx = {
            .block_num = g_fs_block_num,
            .used_cnt = g_fs_used_cnt,
            .total_addrs = 0,
            .bad_addrs = 0,
        };
        dram_visit_subspaces(db->index[t].dram_index->head, fs_mark_used_from_subspace_parallel, &local_fs_ctx);
        access_nvm_release();

        atomic_fetch_add(&g_fs_total_addrs, (uint64_t)local_fs_ctx.total_addrs);
        atomic_fetch_add(&g_fs_bad_addrs, (uint64_t)local_fs_ctx.bad_addrs);
    }

    if(g_fs_end_ticks_ptr){
        atomic_max_u64(g_fs_end_ticks_ptr, spdk_get_ticks());
    }

    atomic_store(&ctx->done, 1);
    return NULL;
}

int recovery_rebuild_index_and_write_buffer_parallel(struct db * db,
                                                      uint64_t *out_index_end_ticks,
                                                      uint64_t *out_wb_end_ticks,
                                                      uint64_t *out_fs_end_ticks){
    if(db == NULL || db->index == NULL){
        return 0;
    }
    if(db->index[0].dram_index == NULL || db->index[0].nvm_index == NULL){
        return 0;
    }
    if(db->write_buffer == NULL){
        return 0;
    }
    if(db->thread <= 0){
        return 0;
    }

    int thread_num = db->thread;
    if(!recovery_prepare_index_free_heads(db)){
        return 0;
    }
    int main_core = spdk_env_get_current_core();

    atomic_uint_fast64_t index_end_ticks;
    atomic_uint_fast64_t wb_end_ticks;
    atomic_uint_fast64_t fs_end_ticks;
    atomic_init(&index_end_ticks, 0);
    atomic_init(&wb_end_ticks, 0);
    atomic_init(&fs_end_ticks, 0);

    g_index_end_ticks_ptr = &index_end_ticks;
    g_wb_end_ticks_ptr = &wb_end_ticks;
    g_fs_end_ticks_ptr = &fs_end_ticks;

    g_wb_total_entries = (uint64_t *)calloc((size_t)thread_num, sizeof(uint64_t));
    g_wb_inserted = (uint64_t *)calloc((size_t)thread_num, sizeof(uint64_t));
    g_wb_skipped_older = (uint64_t *)calloc((size_t)thread_num, sizeof(uint64_t));

    /* freespace: shared used_cnt（并行填充），主线程统一 build */
    if(db->ns_entry == NULL || db->ns_entry->block_num == 0){
        return 0;
    }
    g_fs_block_num = (uint32_t)db->ns_entry->block_num;
    g_fs_used_cnt = (uint32_t *)calloc((size_t)g_fs_block_num, sizeof(uint32_t));
    if(g_fs_used_cnt == NULL){
        return 0;
    }
    atomic_init(&g_fs_total_addrs, 0);
    atomic_init(&g_fs_bad_addrs, 0);

    struct recovery_slot_owner_scan_ctx *scan_ctxs =
        (struct recovery_slot_owner_scan_ctx *)calloc((size_t)thread_num, sizeof(struct recovery_slot_owner_scan_ctx));
    if(scan_ctxs == NULL){
        return 0;
    }

    for(int t = 0 ; t < thread_num ; t++){
        scan_ctxs[t].db = db;
        scan_ctxs[t].t = t;
        scan_ctxs[t].arr = NULL;
        scan_ctxs[t].n = 0;
        scan_ctxs[t].cap = 0;
        scan_ctxs[t].total_keys = 0;
        scan_ctxs[t].key_lo = KEY_MAX;
        scan_ctxs[t].key_hi = 0;
        scan_ctxs[t].scan_ok = 1;
        scan_ctxs[t].group_build_ok = 1;
        atomic_init(&scan_ctxs[t].done, 0);
    }

    /* 单次 pin to core：每个 t worker 负责“索引+写缓存索引” */
    for(int t = 0 ; t < thread_num ; t++){
        int core = t + 1;
        if(core == main_core){
            core++;
        }
        if(spdk_env_thread_launch_pinned(core,
                (thread_start_fn)recovery_scan_and_build_index_and_wb_owner_worker,
                &scan_ctxs[t])){
            recovery_scan_and_build_index_and_wb_owner_worker(&scan_ctxs[t]);
        }
    }

    for(int t = 0 ; t < thread_num ; t++){
        while(atomic_load(&scan_ctxs[t].done) == 0){
            /* busy wait */
        }
    }

    int workers_ok = 1;
    for(int t = 0 ; t < thread_num ; t++){
        if(!scan_ctxs[t].scan_ok || !scan_ctxs[t].group_build_ok){
            workers_ok = 0;
        }
    }

    int marked_slots_ok =
        recovery_store_marked_slot_addresses(db, scan_ctxs, thread_num);
    if(!marked_slots_ok){
        fprintf(stderr,
                "recovery: failed to retain marked split-slot addresses\n");
    }
    uint32_t unresolved_markers =
        recovery_count_unresolved_markers(scan_ctxs, thread_num);
    int unresolved_split = marked_slots_ok && unresolved_markers != 0;

    uint64_t idx_end = atomic_load(&index_end_ticks);
    uint64_t wb_end = atomic_load(&wb_end_ticks);
    uint64_t fs_end = atomic_load(&fs_end_ticks);
    if(out_index_end_ticks){
        *out_index_end_ticks = idx_end;
    }
    if(out_wb_end_ticks){
        *out_wb_end_ticks = wb_end;
    }
    if(out_fs_end_ticks){
        *out_fs_end_ticks = fs_end;
    }

    /* 统计并输出 wb 条目数 */
    uint64_t total_entries = 0, total_inserted = 0, total_skipped_older = 0;
    for(int t = 0 ; t < thread_num ; t++){
        total_entries += g_wb_total_entries[t];
        total_inserted += g_wb_inserted[t];
        total_skipped_older += g_wb_skipped_older[t];
    }
    printf("（并行合并）write_buffer recovered entries=%" PRIu64
           " installed=%" PRIu64 " skipped_older=%" PRIu64 "\n",
           total_entries, total_inserted, total_skipped_older);

    /* build g_map + free_space (single thread) */
    uint32_t block_num = g_fs_block_num;
    if(g_map == NULL){
        g_map = (uint_fast8_t *)malloc(sizeof(uint_fast8_t) * block_num);
        if(g_map == NULL){
            return 0;
        }
    }
    for(uint32_t b = 0 ; b < block_num ; b++){
        uint32_t c = g_fs_used_cnt[b];
        g_map[b] = (c == 0) ? 0 : (c > 255 ? 255 : (uint_fast8_t)c);
    }
    g_block_num = block_num;

    struct free_space *new_fs = build_freespace_from_used(g_fs_used_cnt, block_num);
    if(new_fs == NULL){
        return 0;
    }
    db->fs = new_fs;

    uint64_t used_blocks = 0;
    uint64_t dup_refs = 0;
    for(uint32_t b = 0 ; b < block_num ; b++){
        if(g_fs_used_cnt[b] != 0){
            used_blocks++;
            if(g_fs_used_cnt[b] > 1){
                dup_refs += (uint64_t)(g_fs_used_cnt[b] - 1);
            }
        }
    }
    printf("\n======== recovery SSD 空闲空间重构（并行标记 + 串行构建） ========\n");
    printf("SSD block_num=%u\n", block_num);
    printf("索引引用到的 addr 条目数=%" PRIu64 "（越界=%" PRIu64 "）\n",
           (uint64_t)atomic_load(&g_fs_total_addrs), (uint64_t)atomic_load(&g_fs_bad_addrs));
    printf("被占用 block 数=%" PRIu64 "（占用率 %.2f%%）\n", used_blocks, (double)used_blocks * 100.0 / (double)block_num);
    printf("block 被重复引用次数(used_cnt-1 累加)=%" PRIu64 "\n", dup_refs);
    printf("===========================================================\n\n");

    /* cleanup */
    for(int t = 0 ; t < thread_num ; t++){
        free(scan_ctxs[t].arr);
    }
    free(scan_ctxs);
    free(g_wb_total_entries);
    free(g_wb_inserted);
    free(g_wb_skipped_older);
    g_wb_total_entries = NULL;
    g_wb_inserted = NULL;
    g_wb_skipped_older = NULL;
    g_index_end_ticks_ptr = NULL;
    g_wb_end_ticks_ptr = NULL;
    g_fs_end_ticks_ptr = NULL;
    free(g_fs_used_cnt);
    g_fs_used_cnt = NULL;
    g_fs_block_num = 0;

    if(unresolved_split){
        fprintf(stderr,
                "recovery: %u split marker(s) remain after repair\n",
                unresolved_markers);
    }
    return workers_ok && marked_slots_ok && !unresolved_split;
}


/* write_buffer entry format:
 * [u16 key_size][key][u64 version][u16 logical_value_size]
 * [32-byte valid value prefix][runtime-sized invalid/unwritten value tail]
 * entry_size = KV_RECORD_HEADER_SIZE + logical_value_size = runtime --kvsize
 *
 * 运行时 write_buffer 以 q_size（2^k）为单位分配，每条 entry 从 segment_base + off 写入，off 按 q_size 递增。
 * recovery 只依赖已持久化的 header/key/version，并按该 segment 的 q_size stride 扫描。
 */
static int wbf_entry_parse_sane(const char *p, int q_size, key_type *key_out,
                                int *entry_size_out, kv_version_type *version_out){
    if(p == NULL || key_out == NULL || entry_size_out == NULL){
        return 0;
    }
    if(q_size < (int)KV_RECORD_HEADER_SIZE){
        return 0;
    }
    uint16_t key_size;
    memcpy(&key_size, p, sizeof(key_size));
    if(key_size == 0){
        return -1; /* invalid/freed slot: caller must continue this segment */
    }
    struct kv_record_view view;
    if(kv_record_decode(p, (size_t)q_size, &view) != 0 ||
       view.value_size == 0){
        return 0;
    }
    *key_out = view.key;
    *entry_size_out = (int)view.record_size;
    if(version_out != NULL){
        *version_out = view.version;
    }
    return 1;
}

struct write_buffer_rebuild_ctx{
    struct db *db;
    int t;
    atomic_int done;
    uint64_t total_entries;
    uint64_t inserted;
    uint64_t skipped_older;
};

static void recovery_rebuild_wb_worker(void *arg){
    struct write_buffer_rebuild_ctx *ctx = (struct write_buffer_rebuild_ctx *)arg;
    struct db *db = ctx->db;
    int t = ctx->t;

    ctx->total_entries = 0;
    ctx->inserted = 0;
    ctx->skipped_older = 0;

    access_nvm();
    for(uint32_t file_i = 0 ; file_i < db->write_buffer->file_num ; file_i++){
        PMEMobjpool * pool = db->write_buffer->pool[file_i];
        PMEMoid root_oid = pmemobj_root(pool , sizeof(struct write_buffer_file_root));
        if(OID_IS_NULL(root_oid)){
            continue;
        }
        struct write_buffer_file_root * root = (struct write_buffer_file_root *)pmemobj_direct(root_oid);
        uint16_t seg_num = root->segment_num;
        for(uint16_t seg_i = 0 ; seg_i < seg_num ; seg_i++){
            int q_size = (int)root->segment_element_size[seg_i];
            if(q_size <= 0){
                continue;
            }
            char * seg_base = (char *)pmemobj_direct(root->segment[seg_i]);
            if(seg_base == NULL){
                continue;
            }

            /* 判断该 write_buffer segment 属于哪个 worker（用第一个可解析 entry 的 key） */
            int seg_owner_t = -1;
            key_type seg_key = 0;
            int first_entry_size = 0;
            for(int off0 = 0 ; off0 + q_size <= WSB_SIZE ; off0 += q_size){
                int ok0 = wbf_entry_parse_sane(seg_base + off0, q_size, &seg_key, &first_entry_size, NULL);
                if(ok0 == 1){
                    seg_owner_t = key_to_thread(seg_key, db);
                    break;
                }
                if(ok0 == -1){
                    continue; /* freed slot; later fixed-size slots may be live */
                }
                if(ok0 == 0){
                    break;
                }
            }
            if(seg_owner_t != t){
                continue;
            }

            /* 属于本 worker：逐 entry 恢复写缓存索引 */
            for(int off = 0 ; off + q_size <= WSB_SIZE ; off += q_size){
                key_type key;
                int entry_size;
                kv_version_type version;
                int ok = wbf_entry_parse_sane(seg_base + off, q_size, &key, &entry_size, &version);
                if(ok == -1){
                    continue; /* key_size=0 marks only this slot invalid */
                }
                if(ok == 0){
                    break;
                }
                ctx->total_entries++;

                /* 再次按 key_to_thread 容错过滤 */
                if(key_to_thread(key, db) != t){
                    continue;
                }

                struct dram_index_node * leaf_unused = NULL;
                struct subspace_info * ss = dram_index_search(db->index[t].dram_index, key, &leaf_unused, NULL);
                if(ss == NULL || ss->buffer_index == NULL){
                    continue;
                }

                PMEMoid id = root->segment[seg_i];
                id.off += (uint64_t)off;

                /* 不依赖 random_seed：level=0 即可 */
                int install_rc = recovery_wbf_install_latest(
                    ss, key, entry_size, id, (uint16_t)file_i, version);
                if(install_rc < 0){
                    exit(EXIT_FAILURE);
                }
                if(install_rc == 0){
                    ctx->skipped_older++;
                }
                else{
                    ctx->inserted++;
                }
            }
        }
    }

    access_nvm_release();
    atomic_store(&ctx->done, 1);
}

int recovery_rebuild_write_buffer_indices(struct db * db){
    if(db == NULL || db->write_buffer == NULL || db->index == NULL){
        return 0;
    }

    int thread_num = db->thread;
    if(thread_num <= 0){
        return 0;
    }

    printf("\n======== recovery 写缓存索引重构（并行） ========\n");
    printf("write_buffer_file 数量（已打开）: %u\n", db->write_buffer->file_num);

    int main_core = spdk_env_get_current_core();

    struct write_buffer_rebuild_ctx *ctxs =
        (struct write_buffer_rebuild_ctx *)calloc((size_t)thread_num, sizeof(struct write_buffer_rebuild_ctx));
    if(ctxs == NULL){
        return 0;
    }

    for(int t = 0 ; t < thread_num ; t++){
        ctxs[t].db = db;
        ctxs[t].t = t;
        atomic_init(&ctxs[t].done, 0);
        ctxs[t].total_entries = 0;
        ctxs[t].inserted = 0;
        ctxs[t].skipped_older = 0;

        int core = t + 1;
        if(core == main_core){
            core++;
        }

        if(spdk_env_thread_launch_pinned(core, (thread_start_fn)recovery_rebuild_wb_worker, &ctxs[t])){
            recovery_rebuild_wb_worker(&ctxs[t]);
        }
    }

    for(int t = 0 ; t < thread_num ; t++){
        while(atomic_load(&ctxs[t].done) == 0){
            /* busy wait */
        }
    }

    uint64_t total_entries = 0;
    uint64_t total_inserted = 0;
    uint64_t total_skipped_older = 0;
    for(int t = 0 ; t < thread_num ; t++){
        total_entries += ctxs[t].total_entries;
        total_inserted += ctxs[t].inserted;
        total_skipped_older += ctxs[t].skipped_older;
    }

    printf("扫描到的写缓存条目数: %" PRIu64 "\n", total_entries);
    printf("安装/替换 write_buffer_index 次数: %" PRIu64 "\n", total_inserted);
    printf("因版本较旧或相同而跳过的条目数: %" PRIu64 "\n", total_skipped_older);
    printf("========================================\n\n");

    free(ctxs);
    return 1;
}

/* ---------------- SSD freespace + bitmap recovery ---------------- */

/* 遍历 dram_index 的所有 leaf subspace，执行回调 */
typedef void (*subspace_visit_fn)(struct subspace_info *s, void *ctx);

static void dram_visit_subspaces(struct dram_index_node *node, subspace_visit_fn fn, void *ctx){
    if(node == NULL || fn == NULL){
        return;
    }
    if(!node->is_leaf){
        for(unsigned i = 0 ; i < node->nchild ; i++){
            dram_visit_subspaces(node->children[i], fn, ctx);
        }
        return;
    }
    for(unsigned i = 0 ; i < node->nchild ; i++){
        fn(node->subspaces[i], ctx);
    }
}

struct fs_rebuild_ctx{
    uint32_t block_num;
    uint32_t * used_cnt;   /* 每个块被索引引用次数 */
    uint64_t total_addrs;
    uint64_t bad_addrs;
};

static void fs_mark_used_from_subspace(struct subspace_info *s, void *vctx){
    struct fs_rebuild_ctx *ctx = (struct fs_rebuild_ctx *)vctx;
    if(s == NULL || ctx == NULL){
        return;
    }
    access_nvm();
    for(uint32_t i = 0 ; i < s->slot_num ; i++){
        struct nvm_index_slot *slot =
            (struct nvm_index_slot *)pmemobj_direct(s->slot[i]);
        if(slot == NULL){
            continue;
        }
        for(uint32_t j = 0 ; j < s->slot_key_num[i] ; j++){
            struct address_type ad = slot->entries[j].addr;
            if(ad.size <= 0){
                continue;
            }
            ctx->total_addrs++;
            if(ad.block >= ctx->block_num){
                ctx->bad_addrs++;
                continue;
            }
            ctx->used_cnt[ad.block]++;
        }
    }
    access_nvm_release();
}

/* 根据 used_cnt 生成 free_space 链表（每个 segment 最多 alloc_grain 个块） */
static struct free_space * build_freespace_from_used(const uint32_t *used_cnt, uint32_t block_num){
    struct free_space *fs = (struct free_space *)malloc(sizeof(struct free_space));
    if(fs == NULL){
        return NULL;
    }
    fs->head = NULL;
    fs->tail = NULL;

    int filled = 0;
    struct segment *seg = NULL;

    for(uint32_t b = 0 ; b < block_num ; b++){
        if(used_cnt[b] != 0){
            continue;
        }
        if(seg == NULL){
            seg = (struct segment *)malloc(sizeof(struct segment));
            if(seg == NULL){
                return fs;
            }
            seg->next = NULL;
            filled = 0;
        }
        seg->block_array[filled++] = (int)b;
        if(filled == alloc_grain){
            seg->cur = 0;
            if(fs->head == NULL){
                fs->head = fs->tail = seg;
            }
            else{
                fs->tail->next = seg;
                fs->tail = seg;
            }
            seg = NULL;
            filled = 0;
        }
    }

    if(seg != NULL){
        int start = alloc_grain - filled;
        for(int i = 0 ; i < filled ; i++){
            seg->block_array[start + i] = seg->block_array[i];
        }
        seg->cur = start;
        seg->next = NULL;
        if(fs->head == NULL){
            fs->head = fs->tail = seg;
        }
        else{
            fs->tail->next = seg;
            fs->tail = seg;
        }
    }
    return fs;
}

int recovery_rebuild_freespace_and_bitmap(struct db * db){
    if(db == NULL || db->ns_entry == NULL || db->index == NULL){
        return 0;
    }
    if(db->ns_entry->block_num == 0){
        return 0;
    }

    uint32_t block_num = (uint32_t)db->ns_entry->block_num;
    uint32_t *used = (uint32_t *)calloc((size_t)block_num, sizeof(uint32_t));
    if(used == NULL){
        return 0;
    }

    struct fs_rebuild_ctx ctx = {
        .block_num = block_num,
        .used_cnt = used,
        .total_addrs = 0,
        .bad_addrs = 0,
    };

    for(int t = 0 ; t < db->thread ; t++){
        if(db->index[t].dram_index == NULL){
            continue;
        }
        dram_visit_subspaces(db->index[t].dram_index->head, fs_mark_used_from_subspace, &ctx);
    }

    if(g_map == NULL){
        g_map = (uint_fast8_t *)malloc(sizeof(uint_fast8_t) * block_num);
        if(g_map == NULL){
            free(used);
            return 0;
        }
    }
    for(uint32_t b = 0 ; b < block_num ; b++){
        uint32_t c = used[b];
        g_map[b] = (c == 0) ? 0 : (c > 255 ? 255 : (uint_fast8_t)c);
    }
    g_block_num = block_num;

    struct free_space *new_fs = build_freespace_from_used(used, block_num);
    if(new_fs == NULL){
        free(used);
        return 0;
    }
    db->fs = new_fs;

    uint64_t used_blocks = 0;
    uint64_t dup_refs = 0;
    for(uint32_t b = 0 ; b < block_num ; b++){
        if(used[b] != 0){
            used_blocks++;
            if(used[b] > 1){
                dup_refs += (uint64_t)(used[b] - 1);
            }
        }
    }
    printf("\n======== recovery SSD 空闲空间重构 ========\n");
    printf("SSD block_num=%u\n", block_num);
    printf("索引引用到的 addr 条目数=%" PRIu64 "（越界=%" PRIu64 "）\n", ctx.total_addrs, ctx.bad_addrs);
    printf("被占用 block 数=%" PRIu64 "（占用率 %.2f%%）\n", used_blocks, (double)used_blocks * 100.0 / (double)block_num);
    printf("block 被重复引用次数(used_cnt-1 累加)=%" PRIu64 "（>0 表示索引层存在重复/重叠引用）\n", dup_refs);
    printf("==========================================\n\n");

    free(used);
    return 1;
}
