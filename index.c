#include "./index.h"
#include "dram_index.h"
#include "write_buffer_index.h"

struct hybrid_index * hybrid_index_initialize(key_type ** subspace_array , int thread,  int size){
    struct hybrid_index * hybrid_index = (struct hybrid_index *)malloc(sizeof(struct hybrid_index) * thread);
    int i;
    printf("dram_index_initialize start\n");
    for(i = 0 ; i < thread ; i++){
        hybrid_index[i].dram_index = dram_index_initialize(subspace_array[i] , size);
    }
    printf("sizeof(struct subspace_info) is %zu\n" , sizeof(struct subspace_info));
    // hybrid_index->dram_index = dram_index_initialize(subspace_array , size);
    printf("nvm_index_initialize start\n");
    hybrid_index[0].nvm_index = nvm_index_initialize();
    printf("nvm_index_initialize done\n");
    i = 1;
    while( i < thread){
        hybrid_index[i].nvm_index = hybrid_index[0].nvm_index;
        i++;
    }

    return hybrid_index;
}

struct hybrid_index * hybrid_index_initialize_empty_with_nvm(int thread, struct nvm_index * nvm_index){
    struct hybrid_index * hybrid_index = (struct hybrid_index *)malloc(sizeof(struct hybrid_index) * thread);
    if(hybrid_index == NULL){
        return NULL;
    }

    for(int i = 0 ; i < thread ; i++){
        hybrid_index[i].dram_index = dram_index_initialize(NULL , 0);
        hybrid_index[i].nvm_index = nvm_index;
    }
    return hybrid_index;
}

struct subspace_info * hybrid_index_insert(struct hybrid_index * hybrid_index, key_type key , int size, PMEMoid id ,
 uint16_t WBF_seq , struct write_buffer_return_type * buffer_to_free , int thread){
    struct subspace_info * subspace_info;
    struct dram_index_node * dram_node_p;
    // struct subspace_info * new_subspace_info;
    // printf("dram_index_search %d start\n" , key);
    subspace_info = dram_index_search(hybrid_index->dram_index , key , &dram_node_p , NULL);
    // printf("nvm_index_insert %d start\n" , key);
    //nvm_index_insert向nvm_index中插入
    *buffer_to_free = nvm_index_insert(subspace_info , id , key , size , WBF_seq , thread);
    // dram_index_insert(hybrid_index->dram_index , key , new_subspace_info , dram_node_p);

    //返回要插入的键所在的subspace
    return subspace_info;
    // spdk_te
}


int hybrid_index_search(struct hybrid_index * hybrid_index, key_type key , struct write_buffer_index_node *  node_out , struct address_type * addr_out){
    struct subspace_info * subspace_info;
    struct dram_index_node * dram_node_p;
    struct write_buffer_index_node * buffer_index_node;
    struct write_buffer_index * index;
    // PMEMoid id;
    struct address_type addr;
    // SLOT_MAX_ELEMENT
    // struct address_type addr;
    subspace_info = dram_index_search(hybrid_index->dram_index , key , &dram_node_p , NULL);
    // write_buffer_index_check(subspace_info);
    index = subspace_info->buffer_index;
    // if(key == 6284101983969716453){
    //     printf("get 6284101983969716453 subspace_info is %p\n" , subspace_info);
    //     subspace_info_output_detail(subspace_info);
    // }

    // printf("key %d , subspace min key %d\n" , key , subspace_info->min_key[0]);
    while(index){
        buffer_index_node = write_buffer_index_search(index , key);
        if(buffer_index_node != NULL){
            break;
        }
        else{
            index = index->next;
        }
    }

    // sleep(2);

    if(buffer_index_node == NULL){//不存在于write buffer
        addr = nvm_index_search(subspace_info , key);
        if(ADDR_IS_NULL(addr)){
            return 0;
        }
        else{
            *addr_out = addr;
            return 1;
        }
    }
    else{//存在于subspace_info
        *node_out = *buffer_index_node;
        return 2;
    }

}


void subspace_info_output_all(struct dram_index * dram_index){

}




































































// struct index * initialize_index(int key_num , int thread_num){
//     struct index * index;
//     PMEMoid root;

//     index = (struct index *)malloc(sizeof(struct index) * thread_num);
//     if(index == NULL){
//         printf("failed to alloc memory for index\n");
//         return NULL;
//     }

//     unsigned long index_size = (sizeof(struct skiplist_node_index_pm) + 128);
//     printf("index size is %lu\n",index_size);
//     index_size = index_size * key_num;
//     if(index_size < PMEMOBJ_MIN_POOL)
//         index_size = PMEMOBJ_MIN_POOL;

//     index[0].pool_index = pmemobj_open("<pmem-dir>/index.pool","index");
//     if(index[0].pool_index == NULL){
//         index[0].pool_index = pmemobj_create("<pmem-dir>/index.pool","index",index_size,0666);
//         if(index[0].pool_index == NULL){
//             perror("pmemobj_create(index) failed\n");
//             return NULL;
//         }
//     }
//     root = pmemobj_root(index[0].pool_index,sizeof(struct root_for_index));
//     if(OID_IS_NULL(root)){
//         printf("Couldn't access root obj\n");
//         return 0;
//     }

//     /*插入第一个结点*/
//     struct skiplist_node_index_memory * first_node;
//     for(int i = 0 ; i < thread_num ; i++){
//         first_node = new_node(max_level_index , index->pool_index , i);
//         first_node->key = -1;
//         first_node->level = max_level_index;
//         for(int j = 0 ; j < max_level_index ; j++){
//             first_node->next[j] = NULL;
//         }
//         index[i].head_memory = first_node;
//         index[i].pool_index = index[0].pool_index;
//         index[i].key_num = key_num;
//         index[i].rand_seed = time(NULL);
//     }
//     return index;
// }


// struct skiplist_node_index_memory * new_node(int level , PMEMobjpool * pool_index , int thread){
//     struct skiplist_node_index_memory * node ;
//     node = (struct skiplist_node_index_memory *)malloc(sizeof(struct skiplist_node_index_memory));
//     if(node == NULL){
//         printf("failed to alloc memory for skiplist_node_memory\n");
//         return NULL;
//     }
//     node->level = level;
//     node->next = (struct skiplist_node_index_memory **)malloc(sizeof(struct skiplist_node_index_memory *)*(level));
//     if(node->next == NULL){
//         printf("failed to alloc memory for skiplist_node_memory 2\n");
//     }

//     start2_n[thread][7] = spdk_get_ticks();
//     node->down = new_node_pm(pool_index);
//     if(pmemoid_is_null(node->down))
//         return NULL;

//     end2_n[thread][7] = spdk_get_ticks();
//     if(end2_n[thread][7] > start2_n[thread][7]){
//         total_2_n[thread][7] += (end2_n[thread][7] - start2_n[thread][7]);
//     }
//     else{
//         total_2_n[thread][7] += (end2_n[thread][7] + g_ticks - start2_n[thread][7]);
//     }

//     return node;
// }

// PMEMoid new_node_pm(PMEMobjpool * pool_index){
//     PMEMoid node;

//     if(!pmemobj_alloc(pool_index,&(node),sizeof(struct skiplist_node_index_pm),0,0,0)){
//         return node;
//     }
//     else
//     {
//         printf("failed to alloc memory for skiplist_node_pm\n");
//         pmemoid_set_null(&node);
//         return node;
//     }

// }



// struct address_type index_search(key_type key ,struct  index * index ){
//     int i;
//     struct address_type addr;
//     struct skiplist_node_index_memory *cur = NULL;
//     struct skiplist_node_index_memory *prev = NULL;
//     prev = index->head_memory;

//     PMEMoid id;
//     struct skiplist_node_index_pm * node_pm;
//     // printf("1111\n");
//     for(i = max_level_index - 1; i >= 0 ; i-- ){
//         while ( ((cur = prev->next[i]) != NULL) && (cur->key < key) )
// 		{
// 			prev = cur; /* 向后移动 */
// 		}
//         if(cur != NULL && cur->key == key ){
//             return cur->address;
//         }
//     }
//     // printf("2222\n");
//     ADDR_SET_NULL(&addr);
//     return addr;
// }




// int index_insert(key_type key , struct address_type addr , struct index * index , int thread){

//     struct skiplist_node_index_memory *update[max_level_index];
//     struct skiplist_node_index_memory *cur = NULL;
//     struct skiplist_node_index_memory *prev = NULL;
//     struct skiplist_node_index_memory *insert = NULL;
//     int i , level;

//     struct skiplist_node_index_memory * node;
//     struct skiplist_node_index_pm * node_pm;


//     PMEMoid id;
//     start2_n[thread][6] = spdk_get_ticks();

//     // update = (struct skiplist_node_index_memory **)malloc(sizeof(struct skiplist_node_index_memory *) * max_level_index);
//     // if(update == NULL){
//     //     printf("failed malloc update\n");
//     //     return 0;
//     // }
//     prev = index->head_memory;
//     //从最高层开始向下，检索key
//     for(i = max_level_index - 1; i >= 0 ; i-- ){
//         while ( ((cur = prev->next[i]) != NULL) && (cur->key < key) )
// 		{
// 			prev = cur; /* 向后移动 */
//             // index_search_time[thread] ++;
// 		}
// 		update[i] = prev; /* 各层要插入节点的前驱节点 */
//     }

//     if(cur != NULL && cur->key == key){//当前结点已经存在于内存索引中
//         cur->address = addr;
//         id = cur->down;
//         node_pm = (struct skiplist_node_index_pm *)pmemobj_direct(id);
//         pmemobj_memcpy_persist(index->pool_index,&(node_pm->address),&addr,sizeof(addr));
//         return 1;
//     }
//     // start2[thread] = spdk_get_ticks();
//     //key在索引中不存在


//     // printf("index 1\n");
//     // start2_n[thread][6] = spdk_get_ticks();
//     level = random_level(max_level_index , &(index->rand_seed));
//     // printf("index 2\n");
//     end2_n[thread][6] = spdk_get_ticks();
//     if(end2_n[thread][6] > start2_n[thread][6]){
//         total_2_n[thread][6] += (end2_n[thread][6] - start2_n[thread][6]);
//     }
//     else{
//         total_2_n[thread][6] += (end2_n[thread][6] + g_ticks - start2_n[thread][6]);
//     }
//     // start2_n[thread][6] = spdk_get_ticks();
//     // start2[thread] = spdk_get_ticks();
//     /*在内存索引中插入结点*/

//     // start2_n[thread][7] = spdk_get_ticks();
//     node = new_node(level,index->pool_index , thread);
//     if(node == NULL ){
//         printf("new_node failed\n");
//         return 0;
//     }

//     // end2_n[thread][7] = spdk_get_ticks();
//     // if(end2_n[thread][7] > start2_n[thread][7]){
//     //     total_2_n[thread][7] += (end2_n[thread][7] - start2_n[thread][7]);
//     // }
//     // else{
//     //     total_2_n[thread][7] += (end2_n[thread][7] + g_ticks - start2_n[thread][7]);
//     // }

//     node->key = key;
//     node->level = level;
//     node->address = addr;
//     for (i = 0 ; i < level ; i++ )
//     {
//         node->next[i] = update[i]->next[i];
//         update[i]->next[i] = node;
//     }

//     /*在pm索引中插入结点*/
//     id = node->down;
//     node_pm = (struct skiplist_node_index_pm *)pmemobj_direct(id);

//     node_pm->key = key;
//     node_pm->address = addr;
//     // start2_n[thread][7] = spdk_get_ticks();
//     // start2[thread] = spdk_get_ticks();
//     pmemobj_persist(index->pool_index,node_pm,sizeof(node_pm));
//     // start2[thread] = spdk_get_ticks();
//     // end2_n[thread][7] = spdk_get_ticks();
//     // if(end2_n[thread][7] > start2_n[thread][7]){
//     //     total_2_n[thread][7] += (end2_n[thread][7] - start2_n[thread][7]);
//     // }
//     // else{
//     //     total_2_n[thread][7] += (end2_n[thread][7] + g_ticks - start2_n[thread][7]);
//     // }
//     return 1;
// }
