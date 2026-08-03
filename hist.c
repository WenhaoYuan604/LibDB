#include "spdk/histogram_data.h"
#include "hist.h"
static uint64_t g_tsc_rate;
static struct spdk_histogram_data **g_his_data;
static struct spdk_histogram_data *g_his_data_compact;

static const double g_latency_cutoffs[] = {
	0.01,
	0.10,
	0.25,
	0.50,
	0.75,
	0.90,
	0.95,
	0.98,
	0.99,
	0.995,
	0.999,
	0.9999,
	0.99999,
	0.999999,
	0.9999999,
	-1,
};

static void
_check_cutoff(void *ctx, uint64_t start, uint64_t end, uint64_t count,
	     uint64_t total, uint64_t so_far)
{
	double so_far_pct;
	double **cutoff = ctx;
	static uint64_t max_latency_ticks = 0;
	static uint64_t total_latency_ticks = 0;
	static uint64_t total_count = 0;

	if (count == 0) {
		return;
	}

	if (end > max_latency_ticks) {
		max_latency_ticks = end;
	}

    double cur = (double)end * 1000 * 1000 / g_tsc_rate;
	total_latency_ticks += (start + end) / 2 * count; // 使用区间中点作为估计值
	total_count += count;

	so_far_pct = (double)so_far / total;
	while (so_far_pct >= **cutoff && **cutoff > 0) {
		printf("%9.5f%% : %9.3fus\n", **cutoff * 100, cur);
		(*cutoff)++;
	}
	if(so_far_pct >= 1){
		double max_latency_us = (double)max_latency_ticks * 1000 * 1000 / g_tsc_rate;
		double avg_latency_us = (double)total_latency_ticks * 1000 * 1000 / (g_tsc_rate * total_count);
		printf("最大延迟: %9.3fus\n", max_latency_us);
		printf("平均延迟: %9.3fus\n", avg_latency_us);

		// 重置统计数据，为下一次统计做准备
		max_latency_ticks = 0;
		total_latency_ticks = 0;
		total_count = 0;
	}
}


void histogram_init(int thread_num){
	g_his_data = (struct spdk_histogram_data **)malloc(sizeof(struct spdk_histogram_data *) * thread_num);

    for(int i = 0 ; i < thread_num ; i ++){
        g_his_data[i] = spdk_histogram_data_alloc();
    }
	 g_his_data_compact =  spdk_histogram_data_alloc();
    g_tsc_rate = spdk_get_ticks_hz();
}

void histogram_reset_all(int thread_num){
    for(int i = 0 ; i < thread_num ; i ++){
        spdk_histogram_data_reset(g_his_data[i]);
    }
	spdk_histogram_data_reset(g_his_data_compact);
}

void histogram_update(int thread , uint64_t data){
    spdk_histogram_data_tally(g_his_data[thread],data);
}


void histogram_compact(int thread_num){
    for(int i = 0 ; i < thread_num ; i++){
		spdk_histogram_data_merge(g_his_data_compact , g_his_data[i] );
    }

    const double *cutoff = g_latency_cutoffs;
    spdk_histogram_data_iterate(g_his_data_compact, _check_cutoff, &cutoff);
}
