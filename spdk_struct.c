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
	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
	return 0;
}

bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	opts->io_queue_size = LIBDB_SPDK_IO_QUEUE_SIZE;
	opts->io_queue_requests = LIBDB_SPDK_IO_QUEUE_REQUESTS;
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
	ns_entry = TAILQ_FIRST(&g_namespaces);
	ns_entry->block_num = spdk_nvme_ns_get_size(g_namespaces.tqh_first->ns) / BLOCK_SIZE;

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
		}
	}
	printf("ns_entry is %p ns_entry->task_array_head[0] is %p\n" , ns_entry,  ns_entry->task_array_head[0]);
    return ns_entry;
}

struct free_space * initialize_freespace(int block_num){
	struct free_space * fs = (struct free_space *)malloc(sizeof(struct free_space));
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
			segment->cur =  n;
		}
		else{
			segment->cur = n;
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
		}
		if((*segment_p)->cur == 0 && block_num > 0){
			pthread_mutex_lock(&g_mutex_lock);
			(*segment_p)->next = fs->head;
			fs->head = (*segment_p);
			(*segment_p) = (struct segment *)malloc(sizeof(struct segment));
			(*segment_p)->cur = alloc_grain;
			(*segment_p)->segment_num = fs->head->segment_num;
			pthread_mutex_unlock(&g_mutex_lock);
		}
	}
	return ;
}
