#ifndef SPDK_STRUCT_H_
#define SPDK_STRUCT_H_

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include <string.h>
#include "base.h"

struct segment{
	struct segment * next;
	int cur;
	int segment_num;
	int block_array[alloc_grain];
};

struct free_space{
	struct segment * head;
	struct segment * tail;
};

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	uint16_t			nsid;
	char				hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	TAILQ_ENTRY(trid_entry)		tailq;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};


struct task{
    key_type key;
    int is_completed;
	struct address_type addr;
    char * buffer;//要写入的数据和要读到的数据保存的地址
    struct task * next_free;
    struct ns_entry * ns_entry;

	void * thread;
	int thread_seq;
	void * subspace_info;
	void * new_subspace_info;
	void ** subspace_info_array;


	void * scan_result;
	int * kvcache_hit;
	int * is_in_write_buffer;
	void * index_to_free;
	void * break_down;

	key_type * buffered_key;
    void * buffer_to_free;
    void * buffered_addr;
    int buffered_num;

	int subspace_num;
	int * valid_key_per_subspace;


	int total_block_num;
	int block_completed;
	uint32_t * block_array;
	uint32_t * block_array_to_gc;
	int total_block_num_to_gc;
	int flush_io_failed;
	void * gc_slot_updates;
	int gc_slot_update_count;
	int gc_io_failed;
	void * split_plan;
	int split_io_failed;
	uint64_t start;
};


struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	** qpair;
	struct task ** task_array;
	struct task ** task_array_head;
	struct task ** task_array_tail;
	struct result_ring ** result_ring;
	u_int64_t block_num;
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);

int add_trid(const char *trid_str);

bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,struct spdk_nvme_ctrlr_opts *opts);

void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts);

/*
启动SPDK，初始化nvme ssd,获得的namespace存储在全局变量g_namespace中
qpair_num为qpair的数目
qpair_size为每个qpair对应的task数目,即每个qpair能同时执行的任务数目
thread为总线程数
*/
struct ns_entry * initialize_nvmessd(int qpair_num, int qpair_size,
                                     int thread_num, const char *pci_addr);

struct free_space * initialize_freespace(int block_num);

/* recovery: 不预填充空闲 segment；仅初始化 g_map/锁，返回空 fs（后续由 recovery 重建） */
struct free_space * initialize_freespace_recovery(int block_num);

struct segment * get_free_segment(struct free_space * fs);

int get_free_block(struct segment ** segment_p , struct free_space * fs);

int get_n_free_block(struct segment ** segment_p , struct free_space * fs , int n);

void free_block(uint32_t * block_array , int block_num ,  struct segment ** segment_p , struct free_space * fs);
#endif
