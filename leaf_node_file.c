#include "./leaf_node_file.h"
#include <errno.h>
#include <sys/stat.h>

char * leaf_node_file_str(int file_seq){
    return libdb_config_make_pmem_path("leaf_node_file", (unsigned int)file_seq);
}

PMEMobjpool * leaf_node_file_new(int file_seq){
    PMEMobjpool * pool;
    uint64_t file_size = LNF_SIZE;
    if(file_size < PMEMOBJ_MIN_POOL)
        file_size = PMEMOBJ_MIN_POOL;
    char * file_name = leaf_node_file_str(file_seq);
    if(file_name == NULL){
        fprintf(stderr, "failed to build leaf-node PMEM path\n");
        return NULL;
    }

    pool = pmemobj_create(file_name , "leaf_node" , file_size , 0666);
    if(pool == NULL){
        printf("%s pmemobj_create failed\n", file_name);
        free(file_name);
        return NULL;
    }
    free(file_name);

     /*root*/
    PMEMoid root;
    struct leaf_node_file_root * metadata;
    root = pmemobj_root(pool , sizeof(struct leaf_node_file_root));
    if(OID_IS_NULL(root)){
        printf("Couldn't access root obj\n");
        return 0;
    }
    metadata = (struct leaf_node_file_root *)pmemobj_direct(root);
    metadata->segment_num = 0;
    pmemobj_persist(pool , metadata , sizeof(*metadata));
    return pool;
}

static PMEMobjpool * leaf_node_file_open(int file_seq){
    char * file_name = leaf_node_file_str(file_seq);
    if(file_name == NULL){
        return NULL;
    }
    PMEMobjpool * pool = pmemobj_open(file_name , "leaf_node");
    free(file_name);
    return pool;
}

static int leaf_node_file_validate(PMEMobjpool *pool, uint32_t file_seq){
    PMEMoid root_oid =
        pmemobj_root(pool, sizeof(struct leaf_node_file_root));
    struct leaf_node_file_root *root =
        (struct leaf_node_file_root *)pmemobj_direct(root_oid);
    if(OID_IS_NULL(root_oid) || root == NULL ||
       root->segment_num > LNS_NUM_MAX){
        fprintf(stderr,
                "recovery refused: invalid metadata in leaf_node_file%u\n",
                file_seq);
        return 0;
    }

    for(uint16_t i = 0; i < root->segment_num; i++){
        PMEMoid segment = root->segment[i];
        if(OID_IS_NULL(segment) || segment.pool_uuid_lo != root_oid.pool_uuid_lo ||
           segment.off % LNF_SLOT_MIN_SIZE != 0 ||
           pmemobj_pool_by_oid(segment) != pool ||
           pmemobj_direct(segment) == NULL){
            fprintf(stderr,
                    "recovery refused: invalid segment %u in leaf_node_file%u\n",
                    (unsigned int)i, file_seq);
            return 0;
        }
    }
    return 1;
}

int leaf_node_segement_new(struct nvm_index * nvm_index  , struct nvm_index_free_node ** free_list_head){
    pthread_mutex_lock(&g_mutex_lock_lns);

    struct nvm_index_free_node * node = (struct nvm_index_free_node *)malloc(sizeof(struct nvm_index_free_node));
    if(node == NULL){
        fprintf(stderr, "failed to allocate leaf-node free-list metadata\n");
        pthread_mutex_unlock(&g_mutex_lock_lns);
        return 0;
    }

    PMEMobjpool * pool = NULL;
    struct leaf_node_file_root * metadata = NULL;
    uint32_t pool_seq = 0;
    for(;;){
        if(nvm_index->file_num == 0 || nvm_index->file_num > MAX_FILE_NUM){
            fprintf(stderr, "invalid leaf-node PMDK pool count: %u\n",
                    nvm_index->file_num);
            free(node);
            pthread_mutex_unlock(&g_mutex_lock_lns);
            return 0;
        }

        pool_seq = nvm_index->file_num - 1;
        pool = nvm_index->pool[pool_seq];
        PMEMoid root = pmemobj_root(pool, sizeof(struct leaf_node_file_root));
        metadata = (struct leaf_node_file_root *)pmemobj_direct(root);
        if(metadata == NULL){
            fprintf(stderr, "failed to access leaf_node_file%u metadata\n",
                    pool_seq);
            free(node);
            pthread_mutex_unlock(&g_mutex_lock_lns);
            return 0;
        }

        if(metadata->segment_num < LNS_NUM_MAX &&
           pmemobj_zalloc(pool, &node->buffer,
                          LNS_SIZE + LNF_SLOT_MIN_SIZE, 0) == 0){
            break;
        }

        fprintf(stdout,
                "leaf_node_file%u has no room for another segment after %u segments\n",
                pool_seq, (unsigned int)metadata->segment_num);
        if(nvm_index->file_num >= MAX_FILE_NUM){
            fprintf(stderr,
                    "leaf-node PMDK pool limit reached (%u files)\n",
                    (unsigned int)MAX_FILE_NUM);
            free(node);
            pthread_mutex_unlock(&g_mutex_lock_lns);
            return 0;
        }

        PMEMobjpool *new_pool = leaf_node_file_new((int)nvm_index->file_num);
        if(new_pool == NULL){
            fprintf(stderr, "failed to create leaf_node_file%u\n",
                    nvm_index->file_num);
            free(node);
            pthread_mutex_unlock(&g_mutex_lock_lns);
            return 0;
        }
        nvm_index->pool[nvm_index->file_num] = new_pool;
        nvm_index->file_num++;
        fprintf(stdout, "created leaf_node_file%u\n", nvm_index->file_num - 1);
    }
    if(node->buffer.off % LNF_SLOT_MIN_SIZE != 0){
        node->buffer.off += LNF_SLOT_MIN_SIZE - (node->buffer.off % LNF_SLOT_MIN_SIZE);
    }
    node->num = LNS_SIZE / LNF_SLOT_MIN_SIZE;
    //WBS创建成功
    /*修改metadata*/
    metadata->segment[metadata->segment_num] = node->buffer;
    pmem_persist(&metadata->segment[metadata->segment_num] , sizeof(PMEMoid));
    metadata->segment_num ++;
    pmem_persist(&metadata->segment_num , sizeof(short));
    /*修改free_node*/
    pthread_mutex_unlock(&g_mutex_lock_lns);
    node->next = *free_list_head;
    node->LNF_seq = (uint16_t)pool_seq;
    *free_list_head = node;
    return 1;
}


PMEMoid nvm_index_get_free_node(struct nvm_index * nvm_index ,struct nvm_index_free_node ** free_list_head , PMEMobjpool ** pool){
    struct nvm_index_free_node * pre_node = NULL;
    struct nvm_index_free_node * node = *free_list_head;
    PMEMoid id = OID_NULL;
    if(node == NULL){//没有合适size的空闲结点
        if( leaf_node_segement_new(nvm_index , free_list_head) ){
            node = *free_list_head;
            pre_node = NULL;
        }
    }
    if(node == NULL){
        printf("node is still NULL\n");
        return OID_NULL;
    }
    id = node->buffer;
    node->buffer.off += LNF_SLOT_MIN_SIZE;
    node->num -- ;
    *pool = nvm_index->pool[node->LNF_seq];
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
int nvm_index_reserve_free_nodes(struct nvm_index *nvm_index,
                                 struct nvm_index_free_node **free_list_head,
                                 uint32_t required){
    uint32_t available = 0;
    for(struct nvm_index_free_node *node = *free_list_head; node; node = node->next){
        available += node->num;
    }
    while(available < required){
        if(!leaf_node_segement_new(nvm_index, free_list_head)){
            return 0;
        }
        available = 0;
        for(struct nvm_index_free_node *node = *free_list_head; node; node = node->next){
            available += node->num;
        }
    }
    return 1;
}


struct nvm_index * nvm_index_initialize(){
    struct nvm_index * nvm_index = (struct nvm_index *)calloc(1, sizeof(struct nvm_index));
    if(nvm_index == NULL){
        return NULL;
    }
    nvm_index->file_num = 0;
    nvm_index->pool[0] = leaf_node_file_new(0);
    if(nvm_index->pool[0] == NULL){
        printf("create leaf_node_file failed\n");
        free(nvm_index);
        return NULL;
    }
    nvm_index->file_num ++;

	if(pthread_mutex_init(&g_mutex_lock_lns , NULL) != 0){
			printf("ERROR:fail to init g_mutex_lock_lns\n");
			pmemobj_close(nvm_index->pool[0]);
			free(nvm_index);
			return NULL;
		}

    return nvm_index;
}

struct nvm_index * nvm_index_open_existing(){
    struct nvm_index * nvm_index = (struct nvm_index *)calloc(1, sizeof(struct nvm_index));
    if(nvm_index == NULL){
        return NULL;
    }
    for(uint32_t i = 0; i < MAX_FILE_NUM; i++){
        char *pool_path = leaf_node_file_str((int)i);
        struct stat pool_stat;
        if(pool_path == NULL){
            goto fail;
        }
        if(stat(pool_path, &pool_stat) != 0){
            int stat_errno = errno;
            free(pool_path);
            if(stat_errno == ENOENT){
                for(uint32_t later = i + 1; later <= MAX_FILE_NUM; later++){
                    char *later_path = leaf_node_file_str((int)later);
                    struct stat later_stat;
                    if(later_path == NULL){
                        goto fail;
                    }
                    int later_rc = stat(later_path, &later_stat);
                    int later_errno = errno;
                    free(later_path);
                    if(later_rc == 0){
                        fprintf(stderr,
                                "recovery refused: leaf-node pool numbering has a gap before leaf_node_file%u\n",
                                later);
                        goto fail;
                    }
                    if(later_errno != ENOENT){
                        fprintf(stderr,
                                "recovery failed while checking leaf_node_file%u: %s\n",
                                later, strerror(later_errno));
                        goto fail;
                    }
                }
                break;
            }
            fprintf(stderr, "recovery failed to stat leaf_node_file%u: %s\n",
                    i, strerror(stat_errno));
            goto fail;
        }
        if((uint64_t)pool_stat.st_size != (uint64_t)LNF_SIZE){
            fprintf(stderr,
                    "recovery refused: leaf_node_file%u size is %llu bytes, expected %llu bytes\n",
                    i, (unsigned long long)pool_stat.st_size,
                    (unsigned long long)LNF_SIZE);
            free(pool_path);
            goto fail;
        }
        free(pool_path);

        PMEMobjpool *pool = leaf_node_file_open((int)i);
        if(pool == NULL){
            fprintf(stderr, "recovery failed to open leaf_node_file%u: %s\n",
                    i, pmemobj_errormsg());
            goto fail;
        }
        if(!leaf_node_file_validate(pool, i)){
            pmemobj_close(pool);
            goto fail;
        }
        nvm_index->pool[nvm_index->file_num++] = pool;
    }

    if(nvm_index->file_num == 0){
        goto fail;
    }

    if(nvm_index->file_num == MAX_FILE_NUM){
        char *extra_path = leaf_node_file_str((int)MAX_FILE_NUM);
        struct stat extra_stat;
        if(extra_path != NULL && stat(extra_path, &extra_stat) == 0){
            fprintf(stderr,
                    "recovery refused: more than %u leaf-node PMDK pools exist\n",
                    (unsigned int)MAX_FILE_NUM);
            free(extra_path);
            goto fail;
        }
        free(extra_path);
    }

    if(pthread_mutex_init(&g_mutex_lock_lns , NULL) != 0){
        printf("ERROR:fail to init g_mutex_lock_lns\n");
        goto fail;
    }
    return nvm_index;

fail:
    for(uint32_t i = 0; i < nvm_index->file_num; i++){
        pmemobj_close(nvm_index->pool[i]);
    }
    free(nvm_index);
    return NULL;
}

/* The caller has already durably zeroed this slot. */
void nvm_index_recycle_slot(struct nvm_index *index, struct nvm_index_free_node **head, PMEMoid slot){
    PMEMobjpool *pool = pmemobj_pool_by_oid(slot);
    uint32_t seq;
    for(seq = 0; seq < index->file_num && index->pool[seq] != pool; seq++);
    if(seq == index->file_num) abort();
    struct nvm_index_free_node *node = malloc(sizeof(*node));
    if(!node) return; /* durable empty slot will be recovered on restart */
    node->num = 1;
    node->LNF_seq = seq;
    node->buffer = slot;
    node->next = *head;
    *head = node;
}
