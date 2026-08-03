#ifndef B22CAF91_ECF5_4F29_B03E_F2A9C0D1C7B9
#define B22CAF91_ECF5_4F29_B03E_F2A9C0D1C7B9
#ifndef DRAM_INDEX_H_
#define DRAM_INDEX_H_
#include <stdbool.h>
#include "./nvm_index.h"

struct dram_index_node{
    struct dram_index_node * parent;
    bool is_leaf;
    uint16_t nchild;
    key_type key_child[DRAM_INDEX_MAX_NODE];
    union{
        struct dram_index_node * children[DRAM_INDEX_MAX_NODE];
        struct subspace_info * subspaces[DRAM_INDEX_MAX_NODE];
    };
};

struct dram_index{
    struct dram_index_node * head;
};

void dram_index_insert(struct dram_index * dram_index , key_type key , struct subspace_info * subspace , struct dram_index_node * node_opt);

struct subspace_info * dram_index_search(struct dram_index * hybrid_index , key_type key, struct dram_index_node ** leaf_out , int * index_out);

struct dram_index * dram_index_initialize(key_type *subspace_array , int size);

#endif

#endif /* B22CAF91_ECF5_4F29_B03E_F2A9C0D1C7B9 */
