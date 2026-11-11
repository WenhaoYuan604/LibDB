#ifndef LIBDB_WB_SNAPSHOT_H
#define LIBDB_WB_SNAPSHOT_H
#include <stdint.h>





#if !defined(__x86_64__) || (!defined(__GNUC__) && !defined(__clang__))
#error "Write-buffer snapshots require GNU-compatible x86-64 movq"
#endif
static inline void wb_snapshot_store(uint64_t *address, uint64_t value) {
    __asm__ __volatile__("movq %1, %0" : "=m"(*address) : "r"(value));
}
static inline uint64_t wb_snapshot_load(const uint64_t *address) {
    uint64_t value;
    __asm__ __volatile__("movq %1, %0" : "=r"(value) : "m"(*address));
    return value;
}
#endif
