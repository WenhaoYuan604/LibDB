#ifndef C90A1CE7_9C18_4E79_A8BB_0E7458341846
#define C90A1CE7_9C18_4E79_A8BB_0E7458341846

#include "insert.h"


int recovery_rebuild_subspaces(struct db * db);





int recovery_verify_post(struct db * db);


int recovery_report_duplicate_keys(struct db * db);





int recovery_rebuild_write_buffer_indices(struct db * db);






int recovery_rebuild_freespace_and_bitmap(struct db * db);









int recovery_rebuild_index_and_write_buffer_parallel(struct db * db,
                                                      uint64_t *out_index_end_ticks,
                                                      uint64_t *out_wb_end_ticks,
                                                      uint64_t *out_fs_end_ticks);

#endif
