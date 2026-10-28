#include "insert.h"
#include <assert.h>

int main(void) {
    struct write_buffer wb = {0};
    struct db db = {.write_buffer = &wb};
    space_stats_nvm_init(&db);
    const uint64_t limit = LIBDB_NVM_FLUSH_LIMIT_BYTES;
    const uint64_t tick = LIBDB_NVM_FLUSH_POLL_NS;
    atomic_store(&wb.used_slot_bytes, limit + 1);
    space_stats_nvm_poll_at(&db, 0);
    assert(!db.nvm_flush.sampled && space_stats_nvm_flush_allowed(&db));
    /* Warmup/recovery has fixed behavior regardless of GC load state. */
    db.recovery_mode = 1;
    atomic_store(&wb.used_slot_bytes, limit - 1);
    space_stats_nvm_start_at(&db, 0);
    if(!LIBDB_DYNAMIC_FLUSH_ENABLED) {
        assert(!db.nvm_flush.sampled && space_stats_nvm_flush_allowed(&db));
        space_stats_nvm_stop(&db);
        assert(space_stats_nvm_flush_allowed(&db));
        return 0;
    }
    assert(!space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.sampled && !db.initial_load.complete);
    atomic_store(&wb.used_slot_bytes, limit);
    space_stats_nvm_poll_at(&db, tick - 1);
    assert(!space_stats_nvm_flush_allowed(&db));
    space_stats_nvm_poll_at(&db, tick);
    assert(space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.pressure_changes == 1);
    atomic_store(&wb.used_slot_bytes, limit + 4096);
    space_stats_nvm_poll_at(&db, 2 * tick);
    assert(db.nvm_flush.peak_bytes == limit + 4096);
    atomic_store(&wb.used_slot_bytes, limit - 1);
    space_stats_nvm_poll_at(&db, 3 * tick);
    assert(!space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.pressure_changes == 2);
    space_stats_nvm_stop(&db);
    atomic_store(&wb.used_slot_bytes, limit + 1);
    space_stats_nvm_poll_at(&db, 4 * tick);
    assert(!space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.last_sample_ns == 3 * tick);
    /* Starting above the limit publishes pressure immediately. */
    space_stats_nvm_start_at(&db, 5 * tick);
    assert(space_stats_nvm_flush_allowed(&db));
    assert(db.nvm_flush.peak_bytes == limit + 1);
    space_stats_nvm_stop(&db);
    assert(!space_stats_nvm_flush_allowed(&db));
    return 0;
}
