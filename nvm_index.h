#ifndef C38687D3_0F09_4FFA_881C_01C4090B36F5
#define C38687D3_0F09_4FFA_881C_01C4090B36F5
#ifndef NVM_INDEX_H_
#define NVM_INDEX_H_

#include "leaf_node_file.h"
#include "./leaf_node_file.h"
#include "./subspace.h"
#include "base.h"
#include "nvme_pipe.h"
#include <libpmemobj/base.h>
#include <string.h>
#include "write_buffer_index.h"
#include "kvbuffer.h"

struct nvm_slot_update{
    PMEMoid slot;
    struct nvm_index_slot image;
};

int nvm_index_commit_slot_updates(struct nvm_index *nvm_index,
                                  const struct nvm_slot_update *updates,
                                  uint32_t update_count);



struct write_buffer_return_type nvm_index_insert(struct subspace_info * subspace_info , PMEMoid id , key_type key ,
int size , uint16_t WBF_seq , int thread);

struct address_type nvm_index_search(struct subspace_info * subspace_info , key_type key);

int nvm_index_insert_after_flush(struct subspace_info * subspace_info , struct nvm_index * nvm_index,

key_type * buffer_key , struct address_type * buffered_addr ,int buffered_num , struct nvm_index_free_node ** free_list_head);


struct thread;
bool nvm_index_delete(struct subspace_info *group, key_type key, struct thread *thread);

#endif
#endif /* C38687D3_0F09_4FFA_881C_01C4090B36F5 */
