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
		char nsid_str[6];
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









	cdata = spdk_nvme_ctrlr_get_data(ctrlr);


	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);









	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}
}




static int initialize_task_buffers(struct ns_entry *ns, int workers, int tasks){
    struct task **arrays = NULL, **heads = NULL, **tails = NULL;
    const size_t buffer_bytes = (size_t)BLOCK_SIZE * LIBDB_TASK_BUFFER_BLOCK_COUNT;
    size_t requested_bytes = 0, allocated_buffers = 0;
    const char *allocation = "invalid geometry";
    int worker = -1, task = -1;
    if(workers <= 0 || tasks <= 0) goto failed;

    allocation = "task array pointers";
    requested_bytes = (size_t)workers * sizeof(*arrays);
    arrays = calloc((size_t)workers, sizeof(*arrays));
    if(!arrays) goto failed;
    allocation = "free list heads";
    heads = calloc((size_t)workers, sizeof(*heads));
    if(!heads) goto failed;
    allocation = "free list tails";
    tails = calloc((size_t)workers, sizeof(*tails));
    if(!tails) goto failed;

    for(worker = 0; worker < workers; worker++){
        allocation = "task array";
        task = -1;
        requested_bytes = (size_t)tasks * sizeof(**arrays);
        arrays[worker] = calloc((size_t)tasks, sizeof(**arrays));
        if(!arrays[worker]) goto failed;
        for(task = 0; task < tasks; task++){
            struct task *current = &arrays[worker][task];
            allocation = "DMA buffer";
            requested_bytes = buffer_bytes;
            current->buffer = spdk_zmalloc(buffer_bytes, BLOCK_SIZE, NULL,
                                          SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
            if(!current->buffer) goto failed;
            allocated_buffers++;
            current->ns_entry = ns;
            current->thread_seq = worker;
            current->next_free = task + 1 < tasks ? &arrays[worker][task + 1] : NULL;
        }
        heads[worker] = arrays[worker];
        tails[worker] = &arrays[worker][tasks - 1];
    }
    ns->task_array = arrays;
    ns->task_array_head = heads;
    ns->task_array_tail = tails;
    printf("Task buffers ready: workers=%d tasks_per_worker=%d buffers=%zu buffer_bytes=%zu total_buffer_bytes=%zu\n",
           workers, tasks, allocated_buffers, buffer_bytes, allocated_buffers * buffer_bytes);
    return 0;

failed:
    fprintf(stderr, "Task buffer initialization failed: allocation=%s worker=%d task=%d requested_bytes=%zu allocated_buffers=%zu; load will not start\n",
            allocation, worker, task, requested_bytes, allocated_buffers);
    if(arrays){
        for(int i = 0; i < workers; i++){
            if(!arrays[i]) continue;
            for(int j = 0; j < tasks; j++)
                if(arrays[i][j].buffer) spdk_free(arrays[i][j].buffer);
            free(arrays[i]);
        }
    }
    free(tails);
    free(heads);
    free(arrays);
    return -1;
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
	spdk_env_opts_init(&opts);
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

    if(initialize_task_buffers(ns_entry, qpair_num, qpair_size) != 0){
        return NULL;
    }
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
		atomic_store(&g_used_data_blocks, 0);
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
    atomic_store(&g_used_data_blocks, 0);
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
		pthread_mutex_unlock(&g_mutex_lock);
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

    if(segment == NULL || segment->cur == alloc_grain){
        struct segment *next = get_free_segment(fs);
        if(next == NULL){

            return -1;
        }
        *segment_p = segment = next;
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
		if(alloc_grain - segment_prev->cur  <= n){
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
			block_ref_clear(block_array[block_num]);
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
