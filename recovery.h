#ifndef C90A1CE7_9C18_4E79_A8BB_0E7458341846
#define C90A1CE7_9C18_4E79_A8BB_0E7458341846

#include "insert.h"

/** 从 NVM slot 重建 dram_index / subspace；成功返回 1。内部会做一致性校验并打印报告。 */
int recovery_rebuild_subspaces(struct db * db);

/**
 * recovery 完成后可做二次校验（与 rebuild 末尾逻辑相同，便于单独调用）。
 * 返回 1 表示 NVM 总键数与各 subspace 汇总一致，且抽样键可经 dram+nvm 索引解析。
 */
int recovery_verify_post(struct db * db);

/** 扫描 NVM 上全部索引项中的键，排序后统计重复；约 键数×8 字节内存。qsort 耗时。成功返回 1。 */
int recovery_report_duplicate_keys(struct db * db);

/**
 * 从 write_buffer_file* 扫描所有 segment，重建每个 subspace_info 的 write_buffer_index（不改变原数据结构）。
 * 返回 1 表示扫描与插入流程正常完成（不保证条目数与原始完全一致，取决于掉电时写入的原子性）。
 */
int recovery_rebuild_write_buffer_indices(struct db * db);

/**
 * 从已恢复的 subspace_info（NVM 索引）反推 SSD 块占用 bitmap（g_map），并重建空闲 segment 链表（free_space）。
 * 必须在 recovery_rebuild_subspaces() 之后调用、在 attach_workers_after_index() 之前调用。
 * 返回 1 表示重建完成。
 */
int recovery_rebuild_freespace_and_bitmap(struct db * db);

/**
 * 并行（仅一次 pin to core）同时恢复：
 * 1) dram_index / subspace_info（索引重构）
 * 2) write_buffer_index（写缓存索引重构）
 *
 * out_*_ticks 返回“相对 recovery 开始时刻的 ticks”会更准确，
 * 这里约定为函数内部以 spdk_get_ticks() 读取到的绝对 ticks 值。
 */
int recovery_rebuild_index_and_write_buffer_parallel(struct db * db,
                                                      uint64_t *out_index_end_ticks,
                                                      uint64_t *out_wb_end_ticks,
                                                      uint64_t *out_fs_end_ticks);

#endif
