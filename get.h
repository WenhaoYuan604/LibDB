#ifndef C11C9687_5046_4A4B_A2E3_AA49743DE1D3
#define C11C9687_5046_4A4B_A2E3_AA49743DE1D3
#include "./index.h"

char * get(key_type key , struct thread * thread , int * size_o , uint64_t get_start ,
 struct read_time_breakdown * read_breakdown);

void server_submit_result(key_type key , char * buffer , int size,  int type , int exct_result,
struct spdk_ring * spdk_result_ring , uint64_t start , void * break_down);

void db_delete(key_type key , struct thread * thread );


#endif /* C11C9687_5046_4A4B_A2E3_AA49743DE1D3 */
