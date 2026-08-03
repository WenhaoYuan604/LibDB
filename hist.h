#ifndef D3574784_C8A5_4E9F_AF20_3DCFF95C2B23
#define D3574784_C8A5_4E9F_AF20_3DCFF95C2B23

#include "spdk/env.h"
static void
_check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far);

void histogram_init(int thread_num);

void histogram_reset_all(int thread_num);


void histogram_compact(int thread_num);

void histogram_update(int thread , uint64_t data);

void histogram_reset_all(int thread_num);

#endif /* D3574784_C8A5_4E9F_AF20_3DCFF95C2B23 */
