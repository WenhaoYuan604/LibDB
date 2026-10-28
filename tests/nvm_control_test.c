#include "insert.h"
#include <assert.h>

/* Vary budget pressure independently of live records to catch use of the old metric. */
static void pressure(struct write_buffer *wb, uint64_t bytes) {
    atomic_store(&wb->reusable_slot_bytes, wb->file_bytes - bytes);
}
int main(void) {
    struct write_buffer wb = {.file_bytes = LIBDB_WBF_CAPACITY_LIMIT_BYTES};
    struct db db = {.write_buffer = &wb};
    assert(!pthread_mutex_init(&g_mutex_lock_wbs, NULL));
    space_stats_nvm_init(&db);
    const uint64_t high = LIBDB_NVM_FLUSH_HIGH_BYTES, low = LIBDB_NVM_FLUSH_LOW_BYTES;
    const uint64_t tick = LIBDB_NVM_FLUSH_POLL_NS;
    atomic_store(&wb.used_slot_bytes, 123);
    pressure(&wb, high + 1);
    space_stats_nvm_poll_at(&db, 0);
    assert(!db.nvm_flush.sampled && space_stats_nvm_flush_allowed(&db));
    db.recovery_mode = 1;
    pressure(&wb, low);
    space_stats_nvm_start_at(&db, 0);
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED) {
        assert(!db.nvm_flush.sampled && space_stats_nvm_flush_allowed(&db));
        space_stats_nvm_stop(&db);
        assert(space_stats_nvm_flush_allowed(&db));
        return 0;
    }
    assert(!space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.sampled && !db.initial_load.complete);
    pressure(&wb, high - 1);
    space_stats_nvm_poll_at(&db, tick);
    assert(!space_stats_nvm_flush_allowed(&db));
    pressure(&wb, high);
    space_stats_nvm_poll_at(&db, 2 * tick - 1);
    assert(!space_stats_nvm_flush_allowed(&db));
    space_stats_nvm_poll_at(&db, 2 * tick);
    assert(space_stats_nvm_flush_allowed(&db) && db.nvm_flush.pressure_changes == 1);
    pressure(&wb, low + 1);
    space_stats_nvm_poll_at(&db, 3 * tick);
    assert(space_stats_nvm_flush_allowed(&db));
    pressure(&wb, low);
    space_stats_nvm_poll_at(&db, 4 * tick);
    assert(!space_stats_nvm_flush_allowed(&db) && db.nvm_flush.pressure_changes == 2);
    assert(db.nvm_flush.peak_bytes == 123 && db.nvm_flush.pressure_peak_bytes == high);
    /* The expansion path raises the same flag synchronously, without a main poll. */
    pressure(&wb, high + 1);
    pthread_mutex_lock(&g_mutex_lock_wbs);
    write_buffer_raise_pressure_locked(&wb);
    pthread_mutex_unlock(&g_mutex_lock_wbs);
    assert(space_stats_nvm_flush_allowed(&db));
    space_stats_nvm_stop(&db);
    assert(db.nvm_flush.pressure_changes == 3 && !space_stats_nvm_flush_allowed(&db));
    space_stats_nvm_poll_at(&db, 5 * tick);
    assert(db.nvm_flush.last_sample_ns == 4 * tick);
    pthread_mutex_destroy(&g_mutex_lock_wbs);
    return 0;
}
