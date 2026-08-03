#include "./kvbuffer.h"
#include <libpmem.h>
#include "kv_record.h"

char * write_buffer_file_str(int file_seq){
    return libdb_config_make_pmem_path("write_buffer_file", (unsigned int)file_seq);
}

PMEMobjpool * write_buffer_file_new(int file_seq){
    PMEMobjpool * pool;
    uint64_t file_size = WBF_SIZE;
    if(file_size < PMEMOBJ_MIN_POOL)
        file_size = PMEMOBJ_MIN_POOL;
    char * file_name = write_buffer_file_str(file_seq);
    if(file_name == NULL){
        fprintf(stderr, "failed to build write-buffer PMEM path\n");
        return NULL;
    }

    pool = pmemobj_create(file_name , "write_buffer" , file_size , 0666);
    if(pool == NULL){
        printf("%s pmemobj_create failed\n" , file_name);
        free(file_name);
        return NULL;
    }
    else{
        printf("%s pmemobj_create done\n" , file_name);
    }
    free(file_name);

     /*root*/
    PMEMoid root;
    struct write_buffer_file_root * metadata;
    root = pmemobj_root(pool , sizeof(struct write_buffer_file_root));
    if(OID_IS_NULL(root)){
        printf("Couldn't access root obj\n");
        return 0;
    }
    metadata = (struct write_buffer_file_root *)pmemobj_direct(root);
    metadata->segment_num = 0;
    // pmemobj_memset_persist(pool , metadata , 0 , sizeof(struct write_buffer_file_root));
    pmemobj_persist(pool , metadata , sizeof(metadata));
    return pool;
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

struct write_buffer * write_buffer_initialize(){
    struct write_buffer * write_buffer = (struct write_buffer *)malloc(sizeof(struct write_buffer));
    write_buffer->file_num = 0;

    write_buffer->pool[0] = write_buffer_file_new(0);
    if(write_buffer->pool[0] == NULL){
        printf("create write_buffer_file failed\n");
        return NULL;
    }
    write_buffer->file_num ++;

    if(pthread_mutex_init(&g_mutex_lock_wbs , NULL) != 0){
		printf("ERROR:fail to init g_mutex_lock_wbs\n");
		return 0;
	}

    // write_buffer->free_list_head = NULL;

    return write_buffer;
}

struct write_buffer * write_buffer_open_existing(){
    struct write_buffer * write_buffer = (struct write_buffer *)malloc(sizeof(struct write_buffer));
    if(write_buffer == NULL){
        return NULL;
    }
    write_buffer->file_num = 0;
    for(uint32_t i = 0 ; i < MAX_FILE_NUM ; i++){
        PMEMobjpool * pool = write_buffer_file_open(i);
        if(pool == NULL){
            break;
        }
        write_buffer->pool[write_buffer->file_num++] = pool;
    }
    if(write_buffer->file_num == 0){
        free(write_buffer);
        return NULL;
    }
    if(pthread_mutex_init(&g_mutex_lock_wbs , NULL) != 0){
        printf("ERROR:fail to init g_mutex_lock_wbs\n");
        free(write_buffer);
        return NULL;
    }
    return write_buffer;
}

int write_buffer_segement_new(struct write_buffer * write_buffer , int size , struct write_buffer_free_node ** free_list_head){
    pthread_mutex_lock(&g_mutex_lock_wbs);

    PMEMobjpool * pool = write_buffer->pool[write_buffer->file_num - 1];
    PMEMoid root = pmemobj_root( pool , sizeof(struct write_buffer_file_root));
    struct write_buffer_file_root * metadata = (struct write_buffer_file_root *)pmemobj_direct(root);

    int q_size = WBF_SLOT_MIN_SIZE;//正则化后的size
    while(size > q_size){
        q_size *= 2;
    }
    struct write_buffer_free_node * node = (struct write_buffer_free_node *)malloc(sizeof(struct write_buffer_free_node));
    if(pmemobj_alloc(pool, &node->buffer , WSB_SIZE , 0 , 0 , 0)){
        // printf("failed to alloc memory for WBS\ncreate new WBF\n");
        printf("write_buffer_file_%d no free space , consists of %d WBSs\n" , write_buffer->file_num-1 , ((struct write_buffer_file_root *)pmemobj_direct(pmemobj_root(pool , sizeof(struct write_buffer_file_root))))->segment_num);
        // printf("WBF num is %d\ncurr WBF consists of %d WBSs\n" , write_buffer->file_num,((struct write_buffer_file_root *)pmemobj_direct(pmemobj_root(pool , sizeof(struct write_buffer_file_root))))->segment_num);
        printf("create write_buffer_file_%d\n" , write_buffer->file_num);
        write_buffer->pool[write_buffer->file_num] = write_buffer_file_new(write_buffer->file_num);
        if(write_buffer->pool[write_buffer->file_num] == NULL){
            printf("create write_buffer_file %d failed\n" , write_buffer->file_num);
            return 0;
        }
        write_buffer->file_num++;
        printf("create write_buffer_file_%d success , write_buffer->file_num is %d\n" , write_buffer->file_num-1 , write_buffer->file_num);
        free(node);
        pthread_mutex_unlock(&g_mutex_lock_wbs);
        return write_buffer_segement_new(write_buffer , size , free_list_head);
    }

    // char * str = (char *)pmemobj_direct(node->buffer);
    // pmemobj_memset_persist(pool , str , 0 , WSB_SIZE);
    //
    //WBS创建成功
    if(node->buffer.off % LNF_SLOT_MIN_SIZE != 0){
        node->buffer.off = (node->buffer.off / LNF_SLOT_MIN_SIZE) * LNF_SLOT_MIN_SIZE;
        node->num = WSB_SIZE / q_size - 1;
    }
    else{
         node->num = WSB_SIZE / q_size;
    }
    /*修改metadata*/
    // metadata->segment_element_size[metadata->segment_num] = q_size;
    // metadata->segment[metadata->segment_num] = node->buffer;
    metadata->segment_element_size[metadata->segment_num] = q_size;
    metadata->segment[metadata->segment_num] = node->buffer;
    pmem_persist(&metadata->segment_element_size[metadata->segment_num] , sizeof(q_size));
    pmem_persist(&metadata->segment[metadata->segment_num] , sizeof(PMEMoid));

    // pmemobj_memcpy_persist(pool , &metadata->segment_element_size[metadata->segment_num] , &q_size , sizeof(int));
    // pmemobj_memcpy_persist(pool , &metadata->segment[metadata->segment_num] , &node->buffer , sizeof(PMEMoid));

    metadata->segment_num ++;
    pmem_persist(&metadata->segment_num, sizeof(short));
    // pmemobj_persist(pool , &metadata->segment_num , sizeof(short));
    pthread_mutex_unlock(&g_mutex_lock_wbs);
    /*修改free_node*/
    // node->num = WSB_SIZE / q_size;
    node->size = q_size;
    node->next = *free_list_head;
    node->WBF_seq = write_buffer->file_num - 1;
    *free_list_head = node;
    return 1;
}

PMEMoid write_buffer_get_free_node(int size , uint16_t * WBF_seq ,struct write_buffer * write_buffer ,
struct write_buffer_free_node ** free_list_head , PMEMobjpool ** pool){
    struct write_buffer_free_node * pre_node = NULL;
    struct write_buffer_free_node * node = *free_list_head;
    PMEMoid id = OID_NULL;
    // printf("write_buffer_get_free_node start\n");
    while(node != NULL){
        if(node->size >= size && (node->size / 2) < size){//
            break;
        }
        else{
            pre_node = node;
            node = node->next;
        }
    }
    // printf("loop done\n");
    if(node == NULL){//没有合适size的空闲结点
        if( write_buffer_segement_new(write_buffer , size , free_list_head) ){
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
    node->num -- ;
    node->buffer.off += node->size;
    *pool = write_buffer->pool[node->WBF_seq];
    *WBF_seq = node->WBF_seq;
    // printf("node->buffer.off is %lu node->size is %d\n" , node->buffer.off , node->size);
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

/*
 * The logical record size follows --kvsize, but this PRISM-style experiment
 * only assigns and persists its header plus the first 32 value bytes.
 */
PMEMoid write_buffer_insert(key_type key, kv_version_type version,
 const char *value, int value_size, struct write_buffer *write_buffer,
 uint16_t *WBF_seq, struct write_buffer_free_node **free_list_head, int thread){
    PMEMoid id;
    PMEMobjpool *pool;
    int size;

    if(value == NULL || value_size < (int)KV_RECORD_VALID_VALUE_SIZE ||
       kv_record_size((size_t)value_size) > LIBDB_WBF_MAX_RECORD_SIZE){
        fprintf(stderr,
                "write_buffer_insert: value size %d cannot hold a %zu-byte "
                "valid prefix or makes the record too large\n",
                value_size, KV_RECORD_VALID_VALUE_SIZE);
        return OID_NULL;
    }
    size = (int)kv_record_size((size_t)value_size);

    id = write_buffer_get_free_node(size, WBF_seq, write_buffer,
                                    free_list_head, &pool);
    if(OID_IS_NULL(id)){
        printf("write buffer no free node\n");
        exit(EXIT_FAILURE);
    }

    // end[thread->thread] = spdk_get_ticks();
    // if(end[thread->thread] > start[thread->thread]){
    //     total[thread->thread] += (end[thread->thread] - start[thread->thread]);
    //     // printf("end2 - start is %u\n" , end2 - start);
    // }
    // else{
    //     total[thread->thread] += (end[thread->thread] + g_ticks - start[thread->thread]);
    //     // printf("end2 is %u , start is %u  ,ticks is %u\n" , end , start , g_ticks);
    //     // printf("end2 + g_ticks - start %u\n" , end + g_ticks - start);
    // }

    // else{
    // printf("write buffer get free node %lu %lu pool %p\n", id.pool_uuid_lo , id.off , pool);
    // }

    char * dest = (char *)pmemobj_direct(id);

    // end[thread] = spdk_get_ticks();
    // if(end[thread] > start[thread]){
    //     total[thread] += (end[thread] - start[thread]);
    //     // printf("end2 - start is %u\n" , end2 - start);
    // }
    // else{
    //     total[thread] += (end[thread] + g_ticks - start[thread]);
    //     // printf("end2 is %u , start is %u  ,ticks is %u\n" , end , start , g_ticks);
    //     // printf("end2 + g_ticks - start %u\n" , end + g_ticks - start);
    // }
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

int write_buffer_free(PMEMoid id , uint16_t WBF_seq , int size, struct write_buffer_free_node ** free_list_head){
    int q_size = WBF_SLOT_MIN_SIZE;
    struct write_buffer_free_node * node = (struct write_buffer_free_node *)malloc(sizeof(struct write_buffer_free_node));
    if(node == NULL){
        printf("fail to malloc in func write_buffer_free\n");
        return 0;
    }
    while(q_size < size){
        q_size *= 2;
    }

    node->buffer = id;
    node->num = 1;
    node->size = q_size;
    node->WBF_seq = WBF_seq;
    node->next = *free_list_head;
    *free_list_head = node;
    return 1;
}
