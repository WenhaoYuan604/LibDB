#include "./nvm_index.h"
#include "base.h"
#include "dram_index.h"
#include "leaf_node_file.h"
#include "subspace.h"
#include "write_buffer_index.h"
#include <errno.h>
#include <libpmemobj/base.h>

/*
 * Commit exactly one complete leaf-slot image.  The transaction is scoped to
 * the PMDK pool that owns slot, so this remains valid when different slots in
 * one higher-level operation belong to different leaf_node_fileN pools.
 *
 * This deliberately provides per-slot failure atomicity only.  Callers that
 * update multiple slots still commit one independent transaction per slot.
 */
static int nvm_index_commit_one_slot(PMEMobjpool *expected_pool,
                                     PMEMoid slot, const void *image){
    if(OID_IS_NULL(slot) || image == NULL){
        return -1;
    }

    void *dest = pmemobj_direct(slot);
    PMEMobjpool *pool = pmemobj_pool_by_oid(slot);
    if(dest == NULL || pool == NULL ||
       (expected_pool != NULL && expected_pool != pool)){
        fprintf(stderr,
                "leaf-slot transaction has an invalid pool or destination "
                "(pool=%llu off=%llu)\n",
                (unsigned long long)slot.pool_uuid_lo,
                (unsigned long long)slot.off);
        return -1;
    }

    int aborted = 0;
    int committed = 0;
    int tx_errno = 0;
    TX_BEGIN(pool) {
        /* On failure PMDK automatically moves the transaction to ONABORT. */
        (void)pmemobj_tx_add_range_direct(dest, LNF_SLOT_MIN_SIZE);
        memcpy(dest, image, LNF_SLOT_MIN_SIZE);
    } TX_ONABORT {
        aborted = 1;
        tx_errno = pmemobj_tx_errno();
    } TX_ONCOMMIT {
        committed = 1;
    } TX_END

    if(aborted || !committed){
        fprintf(stderr,
                "leaf-slot transaction failed (pool=%llu off=%llu): %s\n",
                (unsigned long long)slot.pool_uuid_lo,
                (unsigned long long)slot.off,
                strerror(tx_errno != 0 ? tx_errno : EIO));
        return -1;
    }
    return 0;
}

int nvm_index_commit_slot_updates(struct nvm_index *nvm_index,
                                  const struct nvm_slot_update *updates,
                                  uint32_t update_count){
    if(nvm_index == NULL || (updates == NULL && update_count != 0) ||
       update_count > 2U * SUBSPACE_MAX_SLOT){
        return -1;
    }
    if(update_count == 0){
        return 0;
    }
    for(uint32_t i = 0; i < update_count; i++){
        PMEMobjpool *pool = pmemobj_pool_by_oid(updates[i].slot);
        if(OID_IS_NULL(updates[i].slot) ||
           pmemobj_direct(updates[i].slot) == NULL ||
           pool == NULL){
            fprintf(stderr, "slot update contains an invalid leaf slot\n");
            return -1;
        }
        for(uint32_t j = 0; j < i; j++){
            if(OID_EQUALS(updates[i].slot, updates[j].slot)){
                fprintf(stderr, "slot update contains a duplicate slot\n");
                return -1;
            }
        }
    }
    for(uint32_t i = 0; i < update_count; i++){
        PMEMobjpool *pool = pmemobj_pool_by_oid(updates[i].slot);
        if(nvm_index_commit_one_slot(pool, updates[i].slot,
                                     &updates[i].image) != 0){
            return -1;
        }
    }
    return 0;
}


static int nvm_index_output_has_room(int pool_curr, int id_curr, int key_min_curr){
    return pool_curr >= 0 && pool_curr <= SUBSPACE_MAX_SLOT &&
           id_curr >= 0 && id_curr <= SUBSPACE_MAX_SLOT &&
           key_min_curr >= 0 && key_min_curr <= SUBSPACE_MAX_SLOT;
}

static int
nvm_index_insert_after_flush_body(struct subspace_info *subspace_info, struct nvm_index *nvm_index,
                                  key_type *buffer_key, struct address_type *buffered_addr,
                                  int buffered_num, struct nvm_index_free_node **free_list_head){
    u_int32_t i = 0 , k = 0 , j , n;
    uint32_t last_k ;
    struct slot_element * slot_element;
    struct slot_element slot_element_array[256];
    struct slot_element * slot_element_array_p;
    struct nvm_index_slot initial_slot_image = {0};

    PMEMoid id_array[SUBSPACE_MAX_SLOT + 1];
    struct pmemobjpool * pool_array[SUBSPACE_MAX_SLOT + 1];
    key_type key_min_array[SUBSPACE_MAX_KEY];
    uint8_t slot_key_num_array[SUBSPACE_MAX_KEY];
    int id_curr = 0  , pool_curr = 0 , key_min_curr = 0;
    int slot_subspace = 0;

    PMEMoid id;
    PMEMobjpool * pool;
    int slot_num;
    int slot_num_sta;
    int slot_aver_key;
    int is_initialize = 0;
    /*首先找到nvm_index中需要修改的最小的leaf_node*/
    if(subspace_info->slot_num == 0){//初始化
        id = nvm_index_get_free_node(nvm_index ,free_list_head , &pool);
        if(OID_IS_NULL(id)){
            fprintf(stderr, "failed to allocate initial leaf slot\n");
            return -1;
        }
        subspace_info->pool[0] = pool;
        subspace_info->slot[0] = id;
        subspace_info->slot_key_num[0] = 1;
        subspace_info->slot_num = 1;
        subspace_info->min_key[0] = buffer_key[0];
        subspace_info->key_num = 1;
        initial_slot_image.boundary = subspace_info->boundary;
        slot_element = initial_slot_image.entries;
        slot_element->key = buffer_key[0];
        is_initialize = 1;
    }

    while(i < subspace_info->slot_num && subspace_info->min_key[i] <= buffer_key[0]){
        pool_array[i] = subspace_info->pool[i];
        id_array[i] = subspace_info->slot[i];
        key_min_array[i] = subspace_info->min_key[i];
        slot_key_num_array[i] = subspace_info->slot_key_num[i];

        pool_curr++;
        id_curr++;
        key_min_curr++;
        i ++;
    }
    if( i == 0 ){
    }
    else{
        pool_curr --;
        id_curr --;
        key_min_curr --;
        i--;
    }

    slot_subspace = subspace_info->slot_num;
    access_nvm();
    for( ; i < subspace_info->slot_num ; i++){
        last_k = k;
        j = 0;
        n = 0;
        if(i == subspace_info->slot_num - 1){//判断是否是最后一个slot
            k = buffered_num;
        }
        else{
            while(k < buffered_num && buffer_key[k] < subspace_info->min_key[i+1] ){
                k++;
            }
        }
        //该slot内的索引不参与合并
        if(last_k == k){
            if(!nvm_index_output_has_room(pool_curr, id_curr, key_min_curr)){
                fprintf(stderr, "slot update output exceeds local capacity\n");
                return -1;
            }
            key_min_array[key_min_curr] = subspace_info->min_key[i];
            pool_array[pool_curr] = subspace_info->pool[i];
            id_array[id_curr] = subspace_info->slot[i];
            slot_key_num_array[key_min_curr] = subspace_info->slot_key_num[i];

            pool_curr++;
            id_curr++;
            key_min_curr++;
            continue;
        }

        if(is_initialize && i == 0){
            slot_element = initial_slot_image.entries;
        }
        else{
            struct nvm_index_slot *persistent_slot =
                (struct nvm_index_slot *)pmemobj_direct(subspace_info->slot[i]);
            slot_element = persistent_slot == NULL ? NULL : persistent_slot->entries;
        }
        if(slot_element == NULL){
            return -1;
        }

        /*merge_sort*/
        while(last_k < k || j < subspace_info->slot_key_num[i]){
            if( j < subspace_info->slot_key_num[i] && (last_k == k || buffer_key[last_k] > slot_element->key)){
                slot_element_array[n] = *slot_element;
                slot_element ++;
                j++;
            }
            else if(last_k < k && (j == subspace_info->slot_key_num[i] ||  buffer_key[last_k] < slot_element->key)){
                slot_element_array[n].key = buffer_key[last_k];
                slot_element_array[n].addr = buffered_addr[last_k];
                subspace_info->key_num++;
                g_map[buffered_addr[last_k].block]++;
                last_k++;
            }
            else{//插入已经存在的键值对
                if(!is_initialize){
                    subspace_info->redundant_size += slot_element->addr.size;
                    g_map[slot_element->addr.block]--;
                }
                slot_element_array[n].key = buffer_key[last_k];
                slot_element_array[n].addr = buffered_addr[last_k];
                g_map[buffered_addr[last_k].block]++;
                last_k ++;
                slot_element ++;
                j++;
            }
            n++;
        }
        /*persist*/
        slot_element_array_p = slot_element_array;
        if(n > SLOT_MAX_ELEMENT){//slot splt
            slot_num = ((n - 1 )/ SLOT_MAX_ELEMENT) + 1;
            slot_num_sta = slot_num;
            for(j = 0 ; j < slot_num_sta ; j++){
                slot_aver_key = n / slot_num;
                if( j == 0 ){//
                    struct nvm_index_slot slot_image = {0};
                    slot_image.boundary = subspace_info->boundary;
                    memcpy(slot_image.entries, slot_element_array_p,
                           slot_aver_key * sizeof(struct slot_element));
                    if(nvm_index_commit_one_slot(
                           subspace_info->pool[i], subspace_info->slot[i],
                           &slot_image) != 0){
                        return -1;
                    }

                    if(!nvm_index_output_has_room(pool_curr, id_curr, key_min_curr)){
                        fprintf(stderr, "slot update output exceeds local capacity\n");
                        return -1;
                    }
                    pool_array[pool_curr] = subspace_info->pool[i];
                    id_array[id_curr] = subspace_info->slot[i];
                    key_min_array[key_min_curr] = slot_element_array_p->key;
                    slot_key_num_array[key_min_curr] = slot_aver_key;
                    pool_curr++;
                    id_curr++;
                    key_min_curr++;
                    slot_element_array_p += slot_aver_key;
                }
                else{
                    id = nvm_index_get_free_node(nvm_index ,free_list_head , &pool);
                    if(OID_IS_NULL(id)){
                        fprintf(stderr, "failed to allocate split leaf slot\n");
                        return -1;
                    }
                    struct nvm_index_slot slot_image = {0};
                    slot_image.boundary = subspace_info->boundary;
                    memcpy(slot_image.entries, slot_element_array_p,
                           slot_aver_key * sizeof(struct slot_element));
                    if(nvm_index_commit_one_slot(pool, id, &slot_image) != 0){
                        return -1;
                    }


                    if(!nvm_index_output_has_room(pool_curr, id_curr, key_min_curr)){
                        fprintf(stderr, "slot update output exceeds local capacity\n");
                        return -1;
                    }
                    pool_array[pool_curr] = pool;
                    id_array[id_curr] = id;
                    key_min_array[key_min_curr] = slot_element_array_p->key;
                    slot_key_num_array[key_min_curr] = slot_aver_key;

                    pool_curr++;
                    id_curr++;
                    key_min_curr++;

                    slot_element_array_p += slot_aver_key;
                    slot_subspace++;
                }
                n = n - slot_aver_key;
                slot_num -- ;
            }
        }
        else{//no split

            struct nvm_index_slot slot_image = {0};
            slot_image.boundary = subspace_info->boundary;
            memcpy(slot_image.entries, slot_element_array,
                   n * sizeof(struct slot_element));
            if(nvm_index_commit_one_slot(
                   subspace_info->pool[i], subspace_info->slot[i],
                   &slot_image) != 0){
                return -1;
            }



            if(!nvm_index_output_has_room(pool_curr, id_curr, key_min_curr)){
                fprintf(stderr, "slot update output exceeds local capacity\n");
                return -1;
            }
            key_min_array[key_min_curr] = slot_element_array_p->key;
            pool_array[pool_curr] = subspace_info->pool[i];
            id_array[id_curr] = subspace_info->slot[i];
            slot_key_num_array[key_min_curr] = n;

            pool_curr++;
            id_curr++;
            key_min_curr++;

        }

    }
    access_nvm_release();
    if(pool_curr > SUBSPACE_MAX_SLOT || id_curr > SUBSPACE_MAX_SLOT ||
       key_min_curr > SUBSPACE_MAX_SLOT || slot_subspace > SUBSPACE_MAX_SLOT ||
       pool_curr != id_curr || pool_curr != key_min_curr ||
       pool_curr != slot_subspace){
        fprintf(stderr,
                "slot update produced an invalid subspace layout: "
                "slots=%d pools=%d ids=%d min_keys=%d max=%u key_num=%u buffered=%d\n",
                slot_subspace, pool_curr, id_curr, key_min_curr,
                (unsigned int)SUBSPACE_MAX_SLOT, subspace_info->key_num, buffered_num);
        return -1;
    }
    memcpy(subspace_info->pool , pool_array , pool_curr * sizeof(PMEMobjpool * ));
    memcpy(subspace_info->slot , id_array , id_curr * sizeof(PMEMoid));
    memcpy(subspace_info->min_key , key_min_array , key_min_curr * sizeof(key_type));
    memcpy(subspace_info->slot_key_num , slot_key_num_array , key_min_curr * sizeof(uint8_t));
    subspace_info->slot_num = slot_subspace;
    return 0;
}

int nvm_index_insert_after_flush(struct subspace_info *subspace_info, struct nvm_index *nvm_index,
                                 key_type *buffer_key, struct address_type *buffered_addr,
                                 int buffered_num, struct nvm_index_free_node **free_list_head){
    if(subspace_info == NULL || nvm_index == NULL || buffer_key == NULL ||
       buffered_addr == NULL || free_list_head == NULL || buffered_num < 0){
        return -1;
    }
    if(buffered_num == 0){
        return 0;
    }
    return nvm_index_insert_after_flush_body(
        subspace_info, nvm_index, buffer_key, buffered_addr, buffered_num,
        free_list_head);
}




/*在write_buffer_index里插入*/
struct write_buffer_return_type nvm_index_insert(struct subspace_info * subspace_info , PMEMoid id , key_type key ,
int size , uint16_t WBF_seq , int thread){
    struct write_buffer_index * buffer_index = subspace_info->buffer_index;
    struct write_buffer_return_type return_value;
    int level = random_level(buffer_index->reserve_levels - 1 , &random_seed[thread]);
    return_value =  write_buffer_index_insert(buffer_index , key , size , id , WBF_seq , level);

    subspace_info->buffered_size -= return_value.size;
    subspace_info->buffered_size += size;

    return return_value;
}

struct slot_element * binarySearch(struct slot_element arr[], int left, int right, key_type target) {
    while (left <= right) {
        int mid = left + (right - left) / 2;

        // 检查目标值是否在中间位置
        if (arr[mid].key == target) {
            return &arr[mid];
        }

        // 如果目标值大于中间值，忽略左半部分
        if (arr[mid].key < target) {
            left = mid + 1;
        }
        // 如果目标值小于中间值，忽略右半部分
        else {
            right = mid - 1;
        }
    }

    // 如果未找到目标值，返回NULL
    return NULL;
}


/*在nvm_index中检索*/
struct address_type nvm_index_search(struct subspace_info * subspace_info , key_type key){
    int i;
    for(i = 0 ; i < subspace_info->slot_num - 1 ; i++){
        if(key >= subspace_info->min_key[i+1] ){
            continue;
        }
        else{
            break;
        }
    }
    struct address_type result = ADDR_NULL;
    access_nvm();
    struct slot_element *slot_elements =
        subspace_slot_elements(subspace_info, (uint32_t)i);
    struct slot_element *found = slot_elements == NULL ? NULL :
        binarySearch(slot_elements, 0, subspace_info->slot_key_num[i] - 1, key);
    if(found != NULL){
        result = found->addr;
    }
    access_nvm_release();
    return result;
}


void nvm_index_delete(struct subspace_info * subspace_info ,key_type key){
    int i ;
    int left , right;
    for( i = 0 ; i < subspace_info->slot_num - 1 ; i++){
        if(key >= subspace_info->min_key[i+1] ){
            continue;
        }
        else{
            break;
        }
    }
    struct nvm_index_slot *persistent_slot =
        (struct nvm_index_slot *)pmemobj_direct(subspace_info->slot[i]);
    if(persistent_slot == NULL){
        return;
    }
    struct slot_element *slot_element = persistent_slot->entries;
    left = 0;
    right = subspace_info->slot_key_num[i]-1;

    while (left <= right) {
        int mid = left + (right - left) / 2;

        // 检查目标值是否在中间位置
        if (slot_element[mid].key == key) {//找到目标
            struct nvm_index_slot slot_image;
            memcpy(&slot_image, persistent_slot, sizeof(slot_image));
            struct slot_element *image_elements = slot_image.entries;
            memmove(&image_elements[mid], &image_elements[mid + 1],
                    sizeof(struct slot_element) *
                        (subspace_info->slot_key_num[i] - mid - 1));
            memset(&image_elements[subspace_info->slot_key_num[i] - 1], 0,
                   sizeof(struct slot_element));
            if(nvm_index_commit_one_slot(
                   subspace_info->pool[i], subspace_info->slot[i],
                   &slot_image) != 0){
                fprintf(stderr, "leaf-slot delete transaction failed\n");
                return;
            }
            subspace_info->min_key[i] = image_elements[0].key;
            return ;
        }

        // 如果目标值大于中间值，忽略左半部分
        if (slot_element[mid].key < key) {
            left = mid + 1;
        }
        // 如果目标值小于中间值，忽略右半部分
        else {
            right = mid - 1;
        }
    }
    return ;
}
