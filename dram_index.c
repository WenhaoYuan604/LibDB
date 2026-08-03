#include "./dram_index.h"

inline static key_type * lower_bound(key_type * begin, key_type * end, key_type key){
    ptrdiff_t r = end - begin;
    while(r > 0){
        size_t mid = r >> 1;
        if(begin[mid] < key){
            begin += mid + 1;
            r -= mid + 1;
        }
        else
            r = mid;
    }
    return begin;
}

inline static key_type * upper_bound(key_type * begin, key_type * end, key_type key){
    ptrdiff_t r = end - begin;
    while(r > 0){
        size_t mid = r >> 1;
        if(begin[mid] <= key){
            begin += mid + 1;
            r -= mid + 1;
        }
        else
            r = mid;
    }
    return begin;
}

inline static struct dram_index_node * make_node(struct dram_index_node * parent){
    struct dram_index_node * node = (struct dram_index_node *)malloc(sizeof(struct dram_index_node));
    node->parent = parent;
    node->is_leaf = false;
    node->nchild = 0;
    return node;
}

inline static struct dram_index_node * make_leaf(struct dram_index_node * parent){
    struct dram_index_node * node = (struct dram_index_node *)malloc(sizeof(struct dram_index_node));
    node->parent = parent;
    node->is_leaf = true;
    node->nchild = 0;
    return node;
}

inline static struct dram_index_node * node_split(struct dram_index_node * node){
    struct dram_index_node * new_node = (struct dram_index_node *)malloc(sizeof(struct dram_index_node));
    new_node->parent = node->parent;
    new_node->is_leaf = node->is_leaf;
    node->nchild = DRAM_INDEX_MAX_NODE >> 1;
    new_node->nchild = DRAM_INDEX_MAX_NODE - node->nchild;
    memcpy(new_node->key_child, node->key_child + node->nchild, new_node->nchild * sizeof(key_type));
    memcpy(new_node->children, node->children + node->nchild, new_node->nchild * sizeof(struct dram_index_node *));
    if(!new_node->is_leaf)
        for(unsigned i = 0 ; i < new_node->nchild ; i++)
            new_node->children[i]->parent = new_node;
    return new_node;
}

static void node_insert(struct dram_index * dram_index, struct dram_index_node * node,
                        key_type key, struct dram_index_node * child){
    if(node->nchild == DRAM_INDEX_MAX_NODE){
        struct dram_index_node * new_node = node_split(node);
        key_type new_min = new_node->key_child[0];
        if(node->parent == NULL){ // root node
            key_type old_min = node->key_child[0];
            struct dram_index_node * root = make_node(NULL);
            root->nchild = 2;
            root->key_child[0] = old_min;
            root->key_child[1] = new_min;
            root->children[0] = node;
            root->children[1] = new_node;
            dram_index->head = new_node->parent = node->parent = root;
        }
        else
            node_insert(dram_index, node->parent, new_min, new_node);
        if(key >= new_min)
            node = new_node;
    }
    key_type * pivot = upper_bound(node->key_child, node->key_child + node->nchild, key);
    size_t index = pivot - node->key_child;
    memmove(pivot + 1, pivot, (node->nchild - index) * sizeof(key_type));
    memmove(node->children + index + 1, node->children + index,
            (node->nchild - index) * sizeof(struct dram_index_node *));
    node->nchild++;
    node->key_child[index] = key;
    node->children[index] = child;
    if(!node->is_leaf)
        child->parent = node;
}

/*node可能为空*/
void dram_index_insert(struct dram_index * dram_index , key_type key , struct subspace_info * subspace , struct dram_index_node * node_opt){
    subspace->boundary = key;
    struct dram_index_node * node = node_opt ? node_opt : dram_index->head;
    while(1){
        if(!node->is_leaf){
            key_type * pivot = upper_bound(node->key_child + 1, node->key_child + node->nchild, key) - 1;
            size_t index = pivot - node->key_child;
            if(key < node->key_child[0])
                node->key_child[0] = key;
            node = node->children[index];
        }
        else
            return node_insert(dram_index, node, key, (struct dram_index_node *)subspace);
    }
}

/**/
struct subspace_info * dram_index_search(struct dram_index * hybrid_index , key_type key, struct dram_index_node ** leaf_out , int * index_out){
    if(hybrid_index == NULL){
        printf("dram_index head is NULL\n");
    }
    struct dram_index_node * node = hybrid_index->head;
    while(1){
        // printf("0000\n");
        key_type * pivot = upper_bound(node->key_child + 1, node->key_child + node->nchild, key) - 1;
        size_t index = pivot - node->key_child;

        if(!node->is_leaf)
            node = node->children[index];
        else{

            if(__glibc_unlikely(node->nchild == 0))
                return NULL;
            if(leaf_out)
                *leaf_out = node;
            if(index_out)
                *index_out = index;
            return node->subspaces[index];
        }
    }
}

struct dram_index * dram_index_initialize(key_type *subspace_array , int size){
    struct dram_index * dram_index = (struct dram_index *)malloc(sizeof(struct dram_index));
    dram_index->head = make_leaf(NULL);
    // printf("size is %d\n" , size);
    for(int i = 0 ; i < size ; i++ ){
        struct subspace_info * subspace = subspace_info_initialize();
        dram_index_insert( dram_index , subspace_array[i] , subspace , NULL);
    }
    return dram_index;
}
