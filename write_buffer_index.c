#include "write_buffer_index.h"
#include "subspace.h"

struct write_buffer_return_type RETURN_NULL = {{0,0},0,0};

void write_buffer_index_init(struct write_buffer_index *list) {
    list->head = (struct write_buffer_index_node*)malloc(sizeof(struct write_buffer_index_node));
    list->levels = 1;
    list->reserve_levels = SKIPLIST_RESERVE_LEVEL;
    list->head->forward = (struct write_buffer_index_node**)malloc(sizeof(struct write_buffer_index_node*) * list->reserve_levels);
    memset(list->head->forward , 0 , sizeof(struct write_buffer_index_node*) * list->reserve_levels);
    list->head->key = 0;
    list->next = NULL;
    list->prev = NULL;
}

void write_buffer_index_destroy(struct write_buffer_index *list) {
    struct write_buffer_index_node * node , *pre_node;

    for (node = list->head->forward[0]; node ; ){
        pre_node = node;
        node = node->forward[0];
        free(pre_node->forward);
        free(pre_node);
    }

    free(list->head->forward);
    free(list->head);
    free(list);
    return ;
}


struct write_buffer_index_node* write_buffer_index_search(struct write_buffer_index *list, key_type key) {
    struct write_buffer_index_node *node = write_buffer_index_before(list, key);
    if (node != list->head && node->key == key)
        return node;
    else
        return NULL;
}



struct write_buffer_index_node* write_buffer_index_before(struct write_buffer_index *list, key_type key) {
    struct write_buffer_index_node *node = list->head;
    for (ssize_t i = list->levels - 1; i >= 0; i--) {
        while (node->forward[i]) {
            if (node->forward[i]->key > key)
                break;
            if (node->forward[i]->key == key)
                return node->forward[i];
            node = node->forward[i];
        }
    }
    return node;
}


struct write_buffer_return_type write_buffer_index_insert(struct write_buffer_index *list, key_type key, int size ,
PMEMoid id , uint16_t WBF_seq , int level) {
    struct write_buffer_return_type value_return ;

    struct write_buffer_index_node **update = (struct write_buffer_index_node**)malloc(sizeof(struct write_buffer_index_node*) * (level + 1));
    struct write_buffer_index_node *node = list->head;
    for (ssize_t i = list->levels - 1; i >= 0; i--) {
        while (node->forward[i]) {

            if (node->forward[i]->key > key)
                break;
            if (node->forward[i]->key == key) {
                value_return.id = node->forward[i]->id;
                value_return.size = node->forward[i]->size;
                value_return.WBF_seq = node->forward[i]->WBF_seq;
                node->forward[i]->id = id;
                node->forward[i]->size = size;
                node->forward[i]->WBF_seq = WBF_seq;
                free(update);
                return value_return;
            }
            node = node->forward[i];
        }
        if (i <= level){
            update[i] = node;
        }
    }

    struct write_buffer_index_node *new_node = (struct write_buffer_index_node*)malloc(sizeof(struct write_buffer_index_node));
    new_node->key = key;
    new_node->id = id;
    new_node->size = size;
    new_node->WBF_seq = WBF_seq;
    new_node->forward = (struct write_buffer_index_node**)malloc(sizeof(struct write_buffer_index_node*) * (level + 1));
    memset(new_node->forward , 0 , sizeof(struct write_buffer_index_node*) * (level + 1));

    // If levels increased
    if(level > list->reserve_levels){
        printf("ERROR\n");
        sleep(100);
    }
    if (level >= list->levels) {
        level = list->levels;//逐层增加
        list->levels = level + 1;
        if (list->levels > list->reserve_levels) {
            list->reserve_levels = list->levels + 2;
            list->head->forward = (struct write_buffer_index_node**)realloc(list->head->forward, sizeof(struct write_buffer_index_node*) * list->reserve_levels);
            printf("ERROR\n");
            sleep(100);
        }
        list->head->forward[level] = new_node;
        new_node->forward[level] = NULL;
        level--;
    }

    for (size_t i = 0; i <= level; i++) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;

        if(update[i]->key >= new_node->key || (new_node->forward[i] != NULL && new_node->key >= new_node->forward[i]->key)){
            printf("ERROR update[i]->key is %lu new_node->key is %lu new_node->forward[i]->key is %lu\n" , update[i]->key , new_node->key , new_node->forward[i]->key);
            sleep(100);
        }
    }
    free(update);
    return RETURN_NULL;
}

bool write_buffer_index_delete(struct write_buffer_index *list, key_type key, bool free_value) {
    struct write_buffer_index_node *node = list->head;
    for (ssize_t i = list->levels - 1; i >= 0; i--) {
        while (node->forward[i]) {
            if (node->forward[i]->key > key)
                break;
            if (node->forward[i]->key == key) {
                struct write_buffer_index_node *tmp = node->forward[i];
                node->forward[i] = tmp->forward[i];
                if (i == 0) {
                    free(tmp->forward);
                    free(tmp);
                    return true;
                }
                break;
            }
            node = node->forward[i];
        }
    }
    return false;
}



size_t write_buffer_index_flush_size(struct write_buffer_index *buffer_index, key_type last_insert){
    size_t size = 0;
    (void)last_insert;
    for(struct write_buffer_index_node *node = buffer_index->head->forward[0]; node; node = node->forward[0]){
        if(node->size > BLOCK_SIZE){
            return SIZE_MAX;
        }
        size_t block_offset = size % BLOCK_SIZE;
        if(block_offset != 0 && block_offset + node->size > BLOCK_SIZE){
            size += BLOCK_SIZE - block_offset;
        }
        if(size > SIZE_MAX - node->size){
            return SIZE_MAX;
        }
        size += node->size;
    }
    return size;
}

int write_buffer_index_to_array(struct write_buffer_index * buffer_index , uint32_t * block ,  key_type last_insert , key_type ** key ,
struct address_type ** addr , struct write_buffer_return_type ** return_value){
    int size = 0;
    (void)last_insert;
    for (struct write_buffer_index_node * node = buffer_index->head->forward[0]; node; node = node->forward[0])
        size++;
    *key = (key_type*)malloc(sizeof(key_type) * size);
    *addr = (struct address_type*)malloc(sizeof(struct address_type) * size);
    *return_value = (struct write_buffer_return_type*)malloc(sizeof(struct write_buffer_return_type) * size);
    int i = 0;
    int off = 0;
    for (struct write_buffer_index_node * node = buffer_index->head->forward[0]; node; node = node->forward[0])
        {
            size_t block_offset = off % BLOCK_SIZE;
            if(block_offset != 0 && block_offset + node->size > BLOCK_SIZE){
                off += BLOCK_SIZE - block_offset;
            }

            (*key)[i] = node->key;
            (*addr)[i].block = block[off / BLOCK_SIZE];
            (*addr)[i].off = off % BLOCK_SIZE;
            (*addr)[i].no_use = 0;
            (*addr)[i].size = node->size;
            (*return_value)[i].id = node->id;
            (*return_value)[i].size = node->size;
            (*return_value)[i].WBF_seq = node->WBF_seq;
            off += node->size;
            i++;
        }
    return size;
}

struct write_buffer_index * write_buffer_copy (struct write_buffer_index * buffer_index , char * copy , key_type last_insert , int thread){
    struct write_buffer_index * new_index = (struct write_buffer_index *)malloc(sizeof(struct write_buffer_index));
    write_buffer_index_init(new_index);
    size_t off = 0;
    (void)last_insert;
    (void)thread;
    access_nvm();
    for (struct write_buffer_index_node * node = buffer_index->head->forward[0]; node; node = node->forward[0]){
        size_t block_offset = off % BLOCK_SIZE;
        if(block_offset != 0 && block_offset + node->size > BLOCK_SIZE){
            off += BLOCK_SIZE - block_offset;
        }
        memcpy(copy + off, pmemobj_direct(node->id), node->size);
        off += node->size;
    }
    access_nvm_release();
    return new_index;
}


// 比较函数用于qsort
static int compare_keys(const void *a, const void *b) {
    key_type key_a = *(key_type *)a;
    key_type key_b = *(key_type *)b;
    if (key_a < key_b) return -1;
    if (key_a > key_b) return 1;
    return 0;
}


void write_buffer_index_check(struct subspace_info * subspace_info) {
    struct write_buffer_index * buffer_index = subspace_info->buffer_index;
    if (buffer_index == NULL) {
        return;
    }

    if (buffer_index->head == NULL) {
        return;
    }
    int i = 0;
    // 检查每一层的键顺序
    for (int level = 0; level < buffer_index->levels; level++) {
        struct write_buffer_index_node *current = buffer_index->head;
        key_type prev_key = 0; // head 节点的键值
        int node_count = 0;
        i = 0;

        while (current->forward[level] != NULL) {
            // 移动到下一个节点
            current = current->forward[level];
            node_count++;

            // 检查键顺序
            if (current->key < prev_key) {
                printf("  错误: 第 %d 层键顺序混乱: 前一个键 %d %lu > 当前键 %d %lu 当前结点的forward[0] %p\n",level,  i-1 ,prev_key,
                 i, current->key , current->forward[0]);
                printf(" 当前子空间buffer size是 %llu\n", (unsigned long long)subspace_info->buffered_size);
                sleep(10000);
                exit(EXIT_FAILURE);
            }

            // 检查 forward 数组
            if (current->forward == NULL) {
                printf("  错误: 节点%d %p (键=%lu) 的 forward 数组为空\n", i,
                       current, current->key);
                printf(" 当前子空间buffer size是 %llu\n", (unsigned long long)subspace_info->buffered_size);
                sleep(10000);
                exit(EXIT_FAILURE);
                break;
            }
            i++;
            prev_key = current->key;
        }

    }
}

/* Both ranges are disjoint, left strictly precedes right. Transfer ownership. */
void write_buffer_index_merge(struct write_buffer_index *left, struct write_buffer_index *right){
    if(left->reserve_levels < right->levels){
        size_t old = left->reserve_levels;
        left->head->forward = realloc(left->head->forward, right->levels * sizeof(*left->head->forward));
        if(!left->head->forward) abort();
        memset(left->head->forward + old, 0, (right->levels - old) * sizeof(*left->head->forward));
        left->reserve_levels = right->levels;
    }
    for(size_t i = 0; i < right->levels; i++){
        struct write_buffer_index_node *tail = left->head;
        while(tail->forward[i]) tail = tail->forward[i];
        tail->forward[i] = right->head->forward[i];
        right->head->forward[i] = NULL;
    }
    if(left->levels < right->levels) left->levels = right->levels;
}

struct write_buffer_index *write_buffer_index_take_prefix(struct write_buffer_index *index, size_t max_keys, size_t max_bytes){
    struct write_buffer_index_node *last = NULL;
    size_t bytes = 0, keys = 0;
    for(struct write_buffer_index_node *n = index->head->forward[0]; n && keys < max_keys; n = n->forward[0]){
        size_t padded = bytes;
        if(bytes % BLOCK_SIZE + n->size > BLOCK_SIZE)
            padded += BLOCK_SIZE - bytes % BLOCK_SIZE;
        if(n->size > BLOCK_SIZE || padded + n->size > max_bytes) break;
        bytes = padded + n->size;
        last = n;
        keys++;
    }
    if(!last) return NULL;
    struct write_buffer_index *prefix = malloc(sizeof(*prefix));
    if(!prefix) return NULL;
    write_buffer_index_init(prefix);
    free(prefix->head->forward);
    prefix->reserve_levels = index->reserve_levels;
    prefix->levels = index->levels;
    prefix->head->forward = calloc(prefix->reserve_levels, sizeof(*prefix->head->forward));
    if(!prefix->head->forward) abort();
    key_type fence = last->key;
    for(size_t i = 0; i < index->levels; i++){
        struct write_buffer_index_node *tail = index->head;
        while(tail->forward[i] && tail->forward[i]->key <= fence) tail = tail->forward[i];
        if(tail != index->head){
            prefix->head->forward[i] = index->head->forward[i];
            index->head->forward[i] = tail->forward[i];
            tail->forward[i] = NULL;
        }
    }
    return prefix;
}
