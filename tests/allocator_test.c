#include "spdk_struct.h"
#include <assert.h>

/* Exhaustion must release the allocator mutex and preserve the worker's
 * segment so a merge can return partially allocated destination blocks. */
int main(void){
    struct free_space fs = {0};
    struct segment exhausted = {.cur = alloc_grain};
    struct segment *current = &exhausted;
    uint_fast8_t map[128] = {0};
    g_map = map;
    pthread_mutex_init(&g_mutex_lock, NULL);
    assert(get_free_block(&current, &fs) == -1);
    assert(current == &exhausted);
    assert(get_free_block(&current, &fs) == -1);
    uint32_t block = 42;
    block_ref_add(block); block_ref_add(block);
    assert(atomic_load(&g_used_data_blocks) == 1);
    free_block(&block, 1, &current, &fs);
    assert(atomic_load(&g_used_data_blocks) == 0);
    assert(get_free_block(&current, &fs) == 42);
    struct segment available = {.cur = 0};
    available.block_array[0] = 99;
    fs.head = fs.tail = &available;
    assert(get_free_block(&current, &fs) == 99);
    assert(current == &available && !fs.head && !fs.tail);
    assert(atomic_load(&g_used_data_blocks) == 0);
    pthread_mutex_destroy(&g_mutex_lock);
    return 0;
}
