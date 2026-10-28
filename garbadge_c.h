#ifndef C29F8F3E_ADFA_435D_9690_CB8EAEFD760B
#define C29F8F3E_ADFA_435D_9690_CB8EAEFD760B

#include "./subspace.h"
#include "./index.h"
#include "./task.h"

struct redundant_element{
    uint16_t block_redundant_size;
    uint32_t block;
};

int block_exist(uint32_t block ,uint32_t * block_array ,int block_array_size_tmp);

void garbadge_collection_with_fraction(struct subspace_info *, struct thread *, double fraction);
void garbadge_collection(struct subspace_info * subspace_info ,struct thread * thread);

#endif /* C29F8F3E_ADFA_435D_9690_CB8EAEFD760B */
