#ifndef B73ECCDA_5323_4D06_ABC9_5FA9569A0861
#define B73ECCDA_5323_4D06_ABC9_5FA9569A0861
#include "./task.h"
#include "./insert.h"
#include "client.h"

struct scan_kv{
    int key_num;
    key_type * key;
    char ** value;
    int * value_size;
};

bool scan(key_type key1 , int length , char * value,  struct thread * thread, uint64_t start);

#endif /* B73ECCDA_5323_4D06_ABC9_5FA9569A0861 */
