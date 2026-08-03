/* strcasestr(3) 需要 glibc 特性宏，且必须出现在任何会包含 <string.h> 的头文件之前 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "./spdk_struct.h"
#include "base.h"
#include <stdint.h>
#include <string.h>

int add_trid(const char *trid_str)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;
	char *ns;
	char *hostnqn;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
        printf("trid_entry allocted memory failed\n");
		return -1;
	}
	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		printf("Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}
	ns = strcasestr(trid_str, "ns:");
	if (ns) {
		char nsid_str[6]; /* 5 digits maximum in an nsid */
		int len;
		int nsid;
		ns += 3;
		len = strcspn(ns, " \t\n");
		if (len > 5) {
			printf("NVMe namespace IDs must be 5 digits or less\n");
			free(trid_entry);
			return 1;
		}
		memcpy(nsid_str, ns, len);
		nsid_str[len] = '\0';
		nsid = spdk_strtol(nsid_str, 10);
		if (nsid <= 0 || nsid > 65535) {
			fprintf(stderr, "NVMe namespace IDs must be less than 65536 and greater than 0\n");
			free(trid_entry);
			return 1;
		}
		trid_entry->nsid = (uint16_t)nsid;
	}
	hostnqn = strcasestr(trid_str, "hostnqn:");
	if (hostnqn) {
		size_t len;
		hostnqn += strlen("hostnqn:");
		len = strcspn(hostnqn, " \t\n");
		if (len > (sizeof(trid_entry->hostnqn) - 1)) {
			fprintf(stderr, "Host NQN is too long\n");
			free(trid_entry);
			return 1;
		}
		memcpy(trid_entry->hostnqn, hostnqn, len);
		trid_entry->hostnqn[len] = '\0';
	}
    // printf("insert g_trid_list %d ,trid_entry is %d\n",&g_trid_list,trid_entry);
	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
    // printf("after insert g_trid_list %d ,g_trid_list.first is %d\n",&g_trid_list.tqh_first);
	return 0;
}

bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	opts->io_queue_size = LIBDB_SPDK_IO_QUEUE_SIZE;
	opts->io_queue_requests = LIBDB_SPDK_IO_QUEUE_REQUESTS;
	// opts->num_io_queues = 1024;
	printf("Attaching to %s\n", trid->traddr);
	printf("opts->num_io_queues is %u\n" , opts->num_io_queues);
	printf("opts->io_queue_size is %u\n" , opts->io_queue_size);
	return true;
}

void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

#if 0
/* 历史示例代码：向 LBA0 写入/读回 "Hello world!"。
 * LibDB 作为数据库不应在初始化阶段写入用户数据块，因此禁用。
 */
struct hello_world_sequence {
	struct ns_entry	*ns_entry;
	char		*buf;
	unsigned        using_cmb_io;
	int		is_completed;
};



static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair[0], (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	printf("%s", sequence->buf);
	spdk_free(sequence->buf);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;
	struct ns_entry			*ns_entry = sequence->ns_entry;
	int				rc;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair[0], (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY , SPDK_MALLOC_DMA);

	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair[0], sequence->buf,
				   0, /* LBA start */
				   1 * physical_block_to_logical_block, /* number of LBAs */
				   read_complete, (void *)sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}


static void
hello_world(int num_qpair)
{
	struct ns_entry			*ns_entry;
	struct hello_world_sequence	sequence;
	int				rc;
	size_t				sz;
	struct spdk_nvme_io_qpair_opts opts;
	printf("hello world start\n");
	// printf("opts.num_io_queues is %u\n" , opts.num_io_queues);
	memset(g_qpair_lock_is_used , 0 , sizeof(g_qpair_lock_is_used));
	if(num_qpair > ctr_max_qp){//每个ns最多有31个qpair
		for( int i = 0 ; i < ctr_max_qp ; i++){
			if(num_qpair > i + ctr_max_qp){
				if(pthread_mutex_init(&g_qpair_lock[i] , NULL) != 0){
					printf("ERROR:pthread_mutex_init lock %d fail\n" , i);
				}
				g_qpair_lock_is_used[i] = 1;
				printf("mutex lock %d init done\n" , i);
			}
		}
		num_qpair = ctr_max_qp;
	}
	printf("spdk_nvme_ctrlr_alloc_io_qpair\n");

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		// printf("Controller max IO queue size = %d\n",spdk_nvme_ctrlr_get_max_io_queue_size(ns_entry->ctrlr));
		//
		printf("CAP.MQES = %u\n", spdk_nvme_ctrlr_get_regs_cap(ns_entry->ctrlr).bits.mqes);
		spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &opts, sizeof(opts));
		printf("opts.io_queue_requests = %u\n", opts.io_queue_requests);
		printf("spdk_nvme_ctrlr_get_default_io_qpair_opts done\n");
		printf("opts.io_queue_size is %u\n" , opts.io_queue_size);
		// opts.io_queue_size = 512;
		ns_entry->qpair = (struct spdk_nvme_qpair **)malloc(sizeof(struct spdk_nvme_qpair *) * num_qpair);
		for(int i = 0 ; i < num_qpair ; i++ ){
			ns_entry->qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &opts, sizeof(opts));
			// ns_entry->qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
			if (ns_entry->qpair[i] == NULL) {
				printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() %d failed\n" , i);
				return ;
			}
			// printf("qpair->num_requests = %u\n", (ns_entry->qpair[i])->);
			// printf("spdk_nvme_ctrlr_alloc_io_qpair done\n");
		}

		printf("opts.io_queue_size is %u\n" , opts.io_queue_size);
		printf("opts.io_queue_requests is %u\n" , opts.io_queue_requests);
		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
		if (sequence.buf == NULL || sz < 0x1000) {
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return;
		}
		if (sequence.using_cmb_io) {
			printf("INFO: using controller memory buffer for IO\n");
		} else {
			printf("INFO: using host memory buffer for IO\n");
		}
		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;

		/*
		 * If the namespace is a Zoned Namespace, rather than a regular
		 * NVM namespace, we need to reset the first zone, before we
		 * write to it. This not needed for regular NVM namespaces.
		 */
		/*
		 * Print "Hello world!" to sequence.buf.  We will write this data to LBA
		 *  0 on the namespace, and then later read it back into a separate buffer
		 *  to demonstrate the full I/O path.
		 */
		snprintf(sequence.buf, 0x1000, "%s", "Hello world!\n");

		/*
		 * Write the data buffer to LBA 0 of this namespace.  "write_complete" and
		 *  "&sequence" are specified as the completion callback function and
		 *  argument respectively.  write_complete() will be called with the
		 *  value of &sequence as a parameter when the write I/O is completed.
		 *  This allows users to potentially specify different completion
		 *  callback routines for each I/O, as well as pass a unique handle
		 *  as an argument so the application knows which I/O has completed.
		 *
		 * Note that the SPDK NVMe driver will only check for completions
		 *  when the application calls spdk_nvme_qpair_process_completions().
		 *  It is the responsibility of the application to trigger the polling
		 *  process.
		 */
		rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair[0], sequence.buf,
					    0, /* LBA start */
					    1 * physical_block_to_logical_block , /* number of LBAs */
					    write_complete, &sequence, SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}

		/*
		 * Poll for completions.  0 here means process all available completions.
		 *  In certain usage models, the caller may specify a positive integer
		 *  instead of 0 to signify the maximum number of completions it should
		 *  process.  This function will never block - if there are no
		 *  completions pending on the specified qpair, it will return immediately.
		 *
		 * When the write I/O completes, write_complete() will submit a new I/O
		 *  to read LBA 0 into a separate buffer, specifying read_complete() as its
		 *  completion routine.  When the read I/O completes, read_complete() will
		 *  print the buffer contents and set sequence.is_completed = 1.  That will
		 *  break this loop and then exit the program.
		 */
		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair[0], 0);
		}

		/*
		 * Free the I/O qpair.  This typically is done when an application exits.
		 *  But SPDK does support freeing and then reallocating qpairs during
		 *  operation.  It is the responsibility of the caller to ensure all
		 *  pending I/O are completed before trying to free the qpair.
		 */
		// spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}
#endif

/* LibDB 不应在初始化阶段向 SSD 写入 hello world */


void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);
	printf("opts->num_io_queues is %u\n" , opts->num_io_queues);
	printf("opts->io_queue_size is %u\n" , opts->io_queue_size);
	printf("opts->io_queue_requests is %u\n" , opts->io_queue_requests);

	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

    // printf("Max IO Queues Supported: %u\n", cdata->max_io_qpairs);

	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
	 * Note that in NVMe, namespace IDs start at 1, not 0.
	 */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}
}


struct ns_entry * initialize_nvmessd(int qpair_num, int qpair_size,
                                     int thread_num, const char *pci_addr){
    int rc , i  ;
	unsigned __int128  k , core = 0;
	struct trid_entry* trid_entry;
	struct ns_entry * ns_entry;
	struct spdk_env_opts opts ;
	struct spdk_nvme_io_qpair_opts qopts;
	char trid_initialize[128];

	char core_mask[40] = {0};
	for( i = 0 , k = 1 ; i <= thread_num - 1 ; i ++){
		core += k;
		k *= 2;
	}

	if ((uint64_t)(core >> 64) != 0) {
		snprintf(core_mask, sizeof(core_mask), "%llx%016llx",
		         (unsigned long long)(core >> 64),
		         (unsigned long long)core);
	} else {
		snprintf(core_mask, sizeof(core_mask), "%llx", (unsigned long long)core);
	}

	printf("core_mask id %s\n" , core_mask);
    printf("Initializing NVMe Controllers\n");
	spdk_env_opts_init(&opts);//初始化opts
	opts.core_mask = core_mask;
    opts.name = "initialize";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 0;
	}

	if (snprintf(trid_initialize, sizeof(trid_initialize),
	             "trtype:PCIe traddr:%s", pci_addr) >= (int)sizeof(trid_initialize) ||
	    add_trid(trid_initialize) != 0) {
		fprintf(stderr, "Invalid NVMe PCI transport address: %s\n", pci_addr);
		return NULL;
	}

	    trid_entry = g_trid_list.tqh_first;
	if (trid_entry == NULL) {
		fprintf(stderr, "No NVMe transport ID was configured\n");
		return NULL;
	}

    //printf("&trid_entry->trid is %d,g_trid_list is %d,g_trid_list.tqh_first is %d\n",&trid_entry->trid,&g_trid_list,g_trid_list.tqh_first);
    // printf("trid_entry->trid.traddr is %s\n",trid_entry->trid.traddr);

	rc = spdk_nvme_probe(&trid_entry->trid, trid_entry, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		printf("error while spdk_nvme_probe\n");
	}
    if (rc != 0) {
		printf("spdk_nvme_probe() failed\n");
		rc = 1;
        return 0;
	}


	if (TAILQ_EMPTY(&g_controllers)) {
		printf("no NVMe controllers found\n");
		rc = 1;
        return 0;
	}
    if (TAILQ_EMPTY(&g_namespaces)) {
		printf("no NVMe namespace found\n");
		rc = 1;
        return 0;
	}

    /* 注意：不要在初始化阶段写入任何用户数据块（例如 LBA0） */

	printf("spdk_nvme_ns_get_size(g_namespaces.tqh_first->ns) is %lu\n" , spdk_nvme_ns_get_size(g_namespaces.tqh_first->ns));
    // g_blocknum = spdk_nvme_ns_get_size(g_namespaces.tqh_first->ns) / 4096;
	// printf("g_blocknum is %p\n" , &g_blocknum);
	ns_entry = TAILQ_FIRST(&g_namespaces);
	ns_entry->block_num = spdk_nvme_ns_get_size(g_namespaces.tqh_first->ns) / BLOCK_SIZE;

	/* hello_world 之前负责分配 ns_entry->qpair；这里搬回初始化，但不做任何写盘 */
	memset(g_qpair_lock_is_used , 0 , sizeof(g_qpair_lock_is_used));
	if(qpair_num > ctr_max_qp){
		qpair_num = ctr_max_qp;
	}
	for(int qi = 0 ; qi < qpair_num ; qi++){
		if(pthread_mutex_init(&g_qpair_lock[qi] , NULL) != 0){
			printf("ERROR:pthread_mutex_init lock %d fail\n" , qi);
		}
		g_qpair_lock_is_used[qi] = 1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(ns_entry->ctrlr, &qopts, sizeof(qopts));
	ns_entry->qpair = (struct spdk_nvme_qpair **)malloc(sizeof(struct spdk_nvme_qpair *) * qpair_num);
	if(ns_entry->qpair == NULL){
		printf("ERROR:failed to alloc memory for ns_entry->qpair\n");
		return 0;
	}
	for(int qi = 0 ; qi < qpair_num ; qi++ ){
		ns_entry->qpair[qi] = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, &qopts, sizeof(qopts));
		if (ns_entry->qpair[qi] == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() %d failed\n" , qi);
			return 0;
		}
	}

	// if(!initialize_freespace_management(&ns_entry->fs , g_blocknum)){
	// 	printf("failed to initialize freespace\n");
	// 	return 0;
	// }

	// ns_entry->curr_segment = (struct segment **)malloc(sizeof(struct segment *)*num_server_thread);
	// for( int i = 0 ; i < num_server_thread ; i++ ){
	// 	ns_entry->curr_segment[i] = get_free_segment(&ns_entry->fs);
	// 	if(ns_entry->curr_segment[i] == NULL){
	// 		printf("no free segment available\n");
	// 		return NULL;
	// 	}
	// }
	/*为task_array分配空间*/
	ns_entry->task_array = (struct task **)malloc(sizeof(struct task *) * qpair_num);
	ns_entry->task_array_head = (struct task **)malloc(sizeof(struct task *) * qpair_num);
	ns_entry->task_array_tail = (struct task **)malloc(sizeof(struct task *) * qpair_num);
	for(i = 0 ; i < qpair_num ; i++){
		ns_entry->task_array[i] = (struct task *)malloc(sizeof(struct task) * qpair_size);
		if(ns_entry == NULL){
			printf("ERROR:failed to alloc memory for ns_entry->task_array[i]\n");
		}
		for(k = 0 ; k < qpair_size - 1 ; k++){
			ns_entry->task_array[i][k].next_free = &(ns_entry->task_array[i][k+1]);
		}
		ns_entry->task_array[i][k].next_free = NULL;

		ns_entry->task_array_head[i] = &(ns_entry->task_array[i][0]);
		ns_entry->task_array_tail[i] = &(ns_entry->task_array[i][qpair_size-1]);

		for(k = 0 ; k < qpair_size  ; k++){
			ns_entry->task_array[i][k].buffer = spdk_zmalloc(
				BLOCK_SIZE * LIBDB_TASK_BUFFER_BLOCK_COUNT, BLOCK_SIZE, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
			if(ns_entry->task_array[i][k].buffer == NULL){
				printf("ERROR:failed to alloc memory for ns_entry->task_array[i][k].buffer\n");
			}
			ns_entry->task_array[i][k].ns_entry = ns_entry;
				ns_entry->task_array[i][k].gc_slot_updates = NULL;
				ns_entry->task_array[i][k].gc_slot_update_count = 0;
				ns_entry->task_array[i][k].flush_io_failed = 0;
				ns_entry->task_array[i][k].gc_io_failed = 0;
			ns_entry->task_array[i][k].split_plan = NULL;
			ns_entry->task_array[i][k].split_io_failed = 0;
			// ns_entry->task_array[i][k].thread = i;
		}
	}
	printf("ns_entry is %p ns_entry->task_array_head[0] is %p\n" , ns_entry,  ns_entry->task_array_head[0]);
    return ns_entry;
}

struct free_space * initialize_freespace(int block_num){
	struct free_space * fs = (struct free_space *)malloc(sizeof(struct free_space));
	// uint64_t  g_block_num = ((uint64_t)400 * 1024 * 1024 * 1024 / 4096);
	// g_map = (uint_fast8_t *)malloc(sizeof(uint_fast8_t) * g_block_num);
	int segment_num  = block_num / alloc_grain;
	struct segment * segment;
	int i , j;

	if (g_map == NULL) {
		g_map = (uint_fast8_t *)malloc(sizeof(uint_fast8_t) * block_num);
		if (g_map == NULL) {
			printf("failed to alloc memory for g_map\n");
			return 0;
		}
		memset(g_map, 0, sizeof(uint_fast8_t) * block_num);
	}
	g_block_num = block_num;
	printf("segment_num is %d\n" , segment_num);
	if(pthread_mutex_init(&g_mutex_lock , NULL) != 0){
		printf("ERROR:fail to init mutex lock\n");
		return 0;
	}
	segment  = (struct segment *)malloc(sizeof(struct segment));
	if(segment == NULL){
		printf("failed to alloc memory for segment\n");
		return 0;
	}
	for(j = 0 ; j < alloc_grain ; j++ ){
		segment->block_array[j] = j;
	}
	segment->cur = 0;
	segment->segment_num = 0;
	fs->head = segment;
	fs->tail = segment;

	for(i = 1 ; i < segment_num ; i++ ){
		segment  = (struct segment *)malloc(sizeof(struct segment));
		if(segment == NULL){
			printf("failed to alloc memory for segment\n");
			return 0;
		}
		for(j = 0 ; j < alloc_grain ; j++ ){
			segment->block_array[j] = (i*alloc_grain) + j;
		}
		segment->cur = 0;
		segment->segment_num = i;
		fs->tail->next = segment;
		fs->tail = segment;
	}
	segment->next = NULL;
	return fs;
}

struct free_space * initialize_freespace_recovery(int block_num){
	struct free_space * fs = (struct free_space *)malloc(sizeof(struct free_space));
	if(fs == NULL){
		return NULL;
	}
	fs->head = NULL;
	fs->tail = NULL;

	if (g_map == NULL) {
		g_map = (uint_fast8_t *)malloc(sizeof(uint_fast8_t) * block_num);
		if (g_map == NULL) {
			printf("failed to alloc memory for g_map\n");
			free(fs);
			return NULL;
		}
	}
	memset(g_map, 0, sizeof(uint_fast8_t) * block_num);
	g_block_num = block_num;

	if(pthread_mutex_init(&g_mutex_lock , NULL) != 0){
		printf("ERROR:fail to init mutex lock\n");
		free(fs);
		return NULL;
	}
	return fs;
}


struct segment * get_free_segment(struct free_space * fs){
	struct segment * segment;
	uint64_t lock_start = spdk_get_ticks();
	pthread_mutex_lock(&g_mutex_lock);
	if(fs->head == NULL){
		printf("no free segment available\n");
		return NULL;
	}
	segment = fs->head;
	fs->head = fs->head->next;
	if(fs->head == NULL)
		fs->tail = NULL;
	pthread_mutex_unlock(&g_mutex_lock);
	uint64_t lock_end = spdk_get_ticks();
	// printf("lock ticks is %lu\n" , lock_end - lock_start);
	return segment;
}


int get_free_block(struct segment ** segment_p , struct free_space * fs){
    int block;
	struct segment * segment = * segment_p;

    if( segment->cur == alloc_grain ){
        *segment_p = get_free_segment(fs);
		segment = * segment_p;
        if(segment == NULL){
            printf("ERROR:segment is NULL\n");
            return -1;
        }
    }
    block = segment->block_array[segment->cur];
    segment->cur ++ ;
    return block;
}

int get_n_free_block(struct segment ** segment_p , struct free_space * fs , int n){
    int block;
	int g_cur;
	int i;
	struct segment * segment = * segment_p;
	struct segment * segment_prev = * segment_p;
	g_cur = segment->cur;
	while(1){
		if( g_cur + n > alloc_grain ){
			*segment_p = get_free_segment(fs);
			segment = * segment_p;
			if(segment == NULL){
				printf("ERROR:segment is NULL\n");
				return -1;
			}
			g_cur = 0;
		}
		for(i = 1; i < n ; i++){
			if(segment->block_array[g_cur + i] == segment->block_array[g_cur] + i){
				continue;
			}
			else{
				break;
			}
		}
		if(i < n){
			g_cur++;
		}
		else{
			break;
		}
	}

	block = segment->block_array[g_cur];

	if(segment == segment_prev){
		if(g_cur == segment->cur){
		}
		else{
			memmove(&segment->block_array[segment->cur + n] , &segment->block_array[segment->cur] , (g_cur - segment->cur) * sizeof(uint32_t));
		}
		segment->cur += n;
	}
	else{
		memmove(&segment->block_array[n] , &segment->block_array[0] , g_cur * sizeof(int));
		if(alloc_grain - segment_prev->cur  <= n){//转移到新的segment
			// memmove(&segment_prev->block_array[segment_prev->cur] , &segment->block_array[0] , alloc_grain - segment_prev->cur);
			// memmove(segment->block_array[n - (alloc_grain - segment_prev->cur)] , segment_prev->block_array[segment_prev->cur] ,
			// (alloc_grain - segment_prev->cur) * sizeof(int));
			// segment->cur =  n - (alloc_grain - segment_prev->cur);
			segment->cur =  n;
		}
		else{
			segment->cur = n;
			// free_block(&segment_prev->block_array[segment_prev->cur] , alloc_grain - segment_prev->cur , segment_p , fs);
		}
	}
    return block;
}



void free_block(uint32_t * block_array , int block_num ,  struct segment ** segment_p , struct free_space * fs){
	while(block_num > 0){
		while(block_num > 0 && (*segment_p)->cur > 0){
			block_num--;
			(*segment_p)->cur--;
			g_map[block_array[block_num]] = 0;
			(*segment_p)->block_array[(*segment_p)->cur] = block_array[block_num];
			// printf("free block %d\n" , block_array[block_num]);
		}
		if((*segment_p)->cur == 0 && block_num > 0){
			pthread_mutex_lock(&g_mutex_lock);
			(*segment_p)->next = fs->head;
			// (*segment_p)->segment_num = fs->head->segment_num;
			fs->head = (*segment_p);
			(*segment_p) = (struct segment *)malloc(sizeof(struct segment));
			(*segment_p)->cur = alloc_grain;
			(*segment_p)->segment_num = fs->head->segment_num;
			pthread_mutex_unlock(&g_mutex_lock);
		}
	}
	return ;
}
