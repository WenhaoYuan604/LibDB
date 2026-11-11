#ifndef LIBDB_RECOVERY_H
#define LIBDB_RECOVERY_H
#include <stdint.h>
struct db;



int recovery_rebuild_write_buffer_indices(struct db *db);


int recovery_rebuild_index_and_write_buffer_parallel(struct db *db,
    uint64_t *index_end, uint64_t *buffer_end, uint64_t *fs_end);
#endif
