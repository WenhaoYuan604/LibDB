#ifndef C5488194_3362_4013_85D6_6BD732597AE3
#define C5488194_3362_4013_85D6_6BD732597AE3

#include "./spdk_struct.h"

int no_free_task(struct ns_entry * ns_entry , int thread);

struct task * get_free_task(struct ns_entry * ns_entry , int thread);

void free_task(struct task * task);

#endif /* C5488194_3362_4013_85D6_6BD732597AE3 */
