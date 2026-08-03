#ifndef D0A840E9_1466_4CCE_82B6_4C6F8437F57F
#define D0A840E9_1466_4CCE_82B6_4C6F8437F57F

#include "./pmdk.h"
#include "./base.h"

struct leaf_node_file_root{
    PMEMoid segment[LNS_NUM_MAX];
    uint16_t segment_num;
};

struct nvm_index{
    uint32_t file_num;
    PMEMobjpool * pool[MAX_FILE_NUM];
};


struct nvm_index_free_node{
    uint32_t num;
    uint16_t LNF_seq;
    PMEMoid buffer;
    struct nvm_index_free_node * next;
};

struct nvm_index * nvm_index_initialize();
struct nvm_index * nvm_index_open_existing();

PMEMoid nvm_index_get_free_node(struct nvm_index * nvm_index ,struct nvm_index_free_node ** free_list_head , PMEMobjpool ** pool);
int nvm_index_reserve_free_nodes(struct nvm_index *nvm_index,
                                 struct nvm_index_free_node **free_list_head,
                                 uint32_t required);



#endif
