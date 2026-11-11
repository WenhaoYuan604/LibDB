#include <stdio.h>
#include <math.h>
#include <strings.h>
#include "base.h"
#include "kv_record.h"
#include "wal.h"
#include "kvbuffer.h"
#include "index.h"
#include "insert.h"
#include "./server.h"
#include "./client.h"


char * unquote(char* str){
    size_t len = strlen(str);
    if (len >= 2) {
        char first = str[0];
        char last = str[len-1];
        if ((first == '"' && last == '"') ||
            (first == '\'' && last == '\'')) {
            str[len-1] = '\0';
            return str + 1;
        }
    }
    return str;
};

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "Usage: %s --thread=N --kvsize=BYTES --kvs=N --cachesize=MB\n"
        "          --pci-addr=DDDD:BB:DD.F [--pmem-dir=PATH]\n"
        "          --warm=FILE [--test=FILE] [options]\n\n"
        "Runtime configuration:\n"
        "  --pci-addr=ADDR          NVMe controller PCI BDF\n"
        "  --pmem-dir=PATH          Ignored compatibility option; no PMEM path is accessed\n"
        "  --warm=FILE              Load/warmup trace\n"
        "  --test=FILE              Test trace (required unless --load-only=1)\n"
        "  --load-only=0|1         Only YCSB load, then normal shutdown (default 0)\n"
        "  --precondition-a=FILE    Run A after cache warmup, before test (YCSB only)\n"
        "  --phase-io-stats=0|1     Emit independent stage I/O counters (default 0)\n"
        "Runtime tuning options:\n"
        "  --thread=N               Server worker count\n"
        "  --kvsize=BYTES           Total persistent record size\n"
        "  --kvs=N                  KV count used for queue sizing\n"
        "  --cachesize=MB           KV cache capacity in MiB\n"
        "  --load-injectors=N       Client injector count (default %d)\n"
        "  --tasks-per-thread=N     Outstanding tasks per worker (default %d)\n"
        "  --boundary-size=N        Initial boundary sample size (default %d)\n"
        "  --wal=0|1                Enable per-worker PUT WAL (default 0)\n"
        "  --wal-size-mb-per-worker=N  WAL capacity, multiple of 4 MiB (default %u)\n"
        "  --wal-max-inflight=N     Submitted, unapplied WAL batches: 1..16 (default 1)\n"
        "  --wal-wait-us=N          Positive WAL batch wait in microseconds (default 10)\n"
        "  --output=FILE            Append statistics to FILE\n"
        "  --recovery[=0|1]         Rebuild DRAM metadata and buffer from SSD/WAL\n"
        "  --recovery-scan=first-empty|full  Data scan mode (default first-empty)\n"
        "  --recovery-dupcheck=0|1  Deprecated compatibility option; recovery dupcheck is disabled\n"
        "  --help                   Show this message\n\n"
        "The PCI address and trace files may also be set with\n"
        "LIBDB_PCI_ADDR, LIBDB_WARM_FILE, and LIBDB_TEST_FILE; LIBDB_PMEM_DIR is ignored.\n"
        "Persistent layout parameters are centralized in config.h.\n",
        program, LIBDB_DEFAULT_LOAD_INJECTORS,
        LIBDB_DEFAULT_TASKS_PER_THREAD, LIBDB_DEFAULT_BOUNDARY_SIZE, WAL_DEFAULT_MB);
}

int parse_number(const char* name, const char* value) {
    char *end;
    errno = 0;
    long num = strtol(value, &end, 10);

    if (end == value || *end != '\0') {
        fprintf(stderr, "无效数值：%s=%s(必须为整数)\n", name, value);
        exit(EXIT_FAILURE);
    }
    if (errno == ERANGE || num < INT_MIN || num > INT_MAX) {
        fprintf(stderr, "数值超出范围：%s=%s\n", name, value);
        exit(EXIT_FAILURE);
    }
    return (int)num;
};

static int parse_bool_arg(const char *value) {
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        return 1;
    }
    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        return 0;
    }
    fprintf(stderr, "布尔参数值无效（请用 0/1、true/false、yes/no）: %s\n", value);
    exit(EXIT_FAILURE);
}

void parse_arg (int argc , char ** argv , struct db * db){
    char *output_file = NULL;
    int kvs=0;
    char config_error[512];
    db->cachesize = 0;
    for (int i = 1; i < argc; ++i) {
        char *param = argv[i];

        if (strcmp(param, "--help") == 0 || strcmp(param, "-h") == 0) {
            print_usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(param, "--recovery") == 0 || strcmp(param, "-recovery") == 0) {
            db->recovery_mode = 1;
            continue;
        }


        if (strncmp(param, "--", 2)) {
            fprintf(stderr, "参数'%s'格式错误：未知开关（recovery 可用 --recovery 或 -recovery；其它参数须为 --key=value）\n", param);
            exit(EXIT_FAILURE);
        }

        char *eq = strchr(param, '=');
        if (!eq) {
            fprintf(stderr, "参数'%s'格式错误：缺少等号（布尔开关请用 --recovery 或 --recovery=1）\n", param);
            exit(EXIT_FAILURE);
        }


        size_t key_len = eq - (param + 2);
        if (key_len == 0 || key_len >= 256) {
            fprintf(stderr, "参数'%s'格式错误：键名为空或过长\n", param);
            exit(EXIT_FAILURE);
        }

        char key[256];
        strncpy(key, param + 2, key_len);
        key[key_len] = '\0';

        char *value = eq + 1;
        if (*value == '\0') {
            fprintf(stderr, "参数'%s'格式错误：值为空\n", key);
            exit(EXIT_FAILURE);
        }



        if (strcmp(key, "thread") == 0) {
            db->thread = parse_number(key, value);
            if (db->thread <= 0) {
                fprintf(stderr, "线程数必须大于0(当前值：%d)\n", db->thread);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(key, "kvsize") == 0) {
            db->size_kv = parse_number(key, value);
            if (db->size_kv <= 0) {
                fprintf(stderr, "键值大小必须大于0(当前值：%d)\n", db->size_kv);
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(key, "output") == 0) {
            output_file = unquote(value);
        } else if (strcmp(key, "warm") == 0) {
            if (libdb_config_set_warm_path(unquote(value)) != 0) {
                fprintf(stderr, "warm workload 路径过长\n");
                exit(EXIT_FAILURE);
            }
        } else if (strcmp(key, "test") == 0) {
            if (libdb_config_set_test_path(unquote(value)) != 0) {
                fprintf(stderr, "test workload 路径过长\n");
                exit(EXIT_FAILURE);
            }
        }
        else if(strcmp(key, "phase-io-stats") == 0) {
            db->phase_io_stats = parse_bool_arg(value);
        }
        else if(strcmp(key, "load-only") == 0) {
            db->load_only = parse_bool_arg(value);
        }
        else if(strcmp(key, "precondition-a") == 0) {
            const char *path = unquote(value);
            if(strlen(path) >= sizeof(db->precondition_a)) {
                fprintf(stderr, "precondition-a path too long\n"); exit(EXIT_FAILURE);
            }
            strcpy(db->precondition_a, path);
        }
        else if (strcmp(key, "pci-addr") == 0) {
            if (libdb_config_set_pci_addr(unquote(value)) != 0) {
                fprintf(stderr, "PCI 地址过长\n");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "pmem-dir") == 0) {
            if (libdb_config_set_pmem_dir(unquote(value)) != 0) {
                fprintf(stderr, "PMEM 目录路径过长\n");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "kvs") == 0) {
            kvs = parse_number(key, value);
            if (kvs <= 0) {
                fprintf(stderr, "键值对数目必须大于0(当前值：%d)\n", kvs);
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "cachesize") == 0) {
            db->cachesize = parse_number(key , value);
            if (db->cachesize < 0) {
                fprintf(stderr, "cache size必须大于0(当前值：%lu)\n", db->cachesize);
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "load-injectors") == 0) {
            db->load_injectors = parse_number(key, value);
            if (db->load_injectors <= 0) {
                fprintf(stderr, "load injector 数必须大于 0\n");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "tasks-per-thread") == 0) {
            db->tasks_per_thread = parse_number(key, value);
            if (db->tasks_per_thread <= 0) {
                fprintf(stderr, "每线程并行任务数必须大于 0\n");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "boundary-size") == 0) {
            db->boundary_size = parse_number(key, value);
            if (db->boundary_size <= 0) {
                fprintf(stderr, "boundary size 必须大于 0\n");
                exit(EXIT_FAILURE);
            }
        }
        else if (strcmp(key, "wal") == 0) {
            db->wal_enabled = parse_bool_arg(value);
        }
        else if (strcmp(key, "wal-size-mb-per-worker") == 0) {
            int mb = parse_number(key, value);
            if(mb < 8 || mb % 4) {
                fprintf(stderr, "WAL size must be a multiple of 4 MiB, at least 8 MiB\n");
                exit(EXIT_FAILURE);
            }
            db->wal_mb_per_worker = (unsigned)mb;
        }
        else if (strcmp(key, "wal-max-inflight") == 0) {
            int n = parse_number(key, value);
            if(n < 1 || n > WAL_MAX_INFLIGHT) {
                fprintf(stderr, "WAL max-inflight must be between 1 and 16\n");
                exit(EXIT_FAILURE);
            }
            db->wal_max_inflight = (unsigned)n;
        }
        else if (strcmp(key, "wal-wait-us") == 0) {
            int wait_us = parse_number(key, value);
            if(wait_us <= 0) {
                fprintf(stderr, "WAL batch wait must be positive microseconds\n");
                exit(EXIT_FAILURE);
            }
            db->wal_wait_us = (unsigned)wait_us;
        }
        else if (strcmp(key, "recovery") == 0) {
            db->recovery_mode = parse_bool_arg(value);
        }
        else if (strcmp(key, "recovery-scan") == 0) {
            if(strcmp(value, "first-empty") && strcmp(value, "full")) {
                fprintf(stderr, "recovery-scan must be first-empty or full\n");
                exit(EXIT_FAILURE);
            }
            db->recovery_scan_full = !strcmp(value, "full");
        }
        else if (strcmp(key, "recovery-dupcheck") == 0) {
            db->recovery_dupcheck = parse_bool_arg(value);
        }
        else {
            fprintf(stderr, "未知参数：%s\n", key);
            exit(EXIT_FAILURE);
        }
    }

    if(db->load_only && (db->recovery_mode || db->precondition_a[0])) {
        fprintf(stderr, "--load-only cannot be combined with recovery or --precondition-a\n");
        exit(EXIT_FAILURE);
    }
    if(db->recovery_mode && !db->wal_enabled){
        fprintf(stderr, "recovery requires --wal=1\n");
        exit(EXIT_FAILURE);
    }

    if(db->precondition_a[0] &&
       (strstr(g_libdb_config.test_path, "Production") || access(db->precondition_a, R_OK))) {
        fprintf(stderr, "precondition-a requires a readable YCSB trace and non-Production mode\n");
        exit(EXIT_FAILURE);
    }

    if (db->thread == 0 || db->size_kv == 0 || kvs == 0) {
        fprintf(stderr, "缺少必要参数：\n");
        if (db->thread == 0) fprintf(stderr, "  --thread 必须指定\n");
        if (db->size_kv == 0) fprintf(stderr, "  --kvsize 必须指定\n");
        if (kvs == 0) fprintf(stderr, "  --kvs 必须指定\n");
        exit(EXIT_FAILURE);
    }
    if ((size_t)db->size_kv < KV_RECORD_ENCODED_SIZE) {
        fprintf(stderr,
                "--kvsize 至少为 %zu 字节\n", KV_RECORD_ENCODED_SIZE);
        exit(EXIT_FAILURE);
    }
    if ((size_t)db->size_kv > LIBDB_WBF_MAX_RECORD_SIZE) {
        fprintf(stderr,
                "--kvsize 不能超过 %u 字节（write-buffer的2次幂分配粒度存储为uint16_t）\n",
                (unsigned)LIBDB_WBF_MAX_RECORD_SIZE);
        exit(EXIT_FAILURE);
    }
    if(db->wal_enabled && (db->thread > ctr_max_qp || db->size_kv > BLOCK_SIZE)) {
        fprintf(stderr, "WAL requires at most %d workers and records no larger than %u bytes\n", ctr_max_qp, BLOCK_SIZE);
        exit(EXIT_FAILURE);
    }
    if (db->thread + db->load_injectors + 1 > LIBDB_MAX_THREADS) {
        fprintf(stderr, "thread + load-injectors + 1 不能超过 %d\n", LIBDB_MAX_THREADS);
        exit(EXIT_FAILURE);
    }
    if (db->recovery_dupcheck && !db->recovery_mode) {
        fprintf(stderr, "--recovery-dupcheck=1 需要同时启用 --recovery\n");
        exit(EXIT_FAILURE);
    }
    if (libdb_config_validate_inputs(config_error, sizeof(config_error), !db->load_only) != 0) {
        fprintf(stderr, "配置错误：%s\n", config_error);
        print_usage(stderr, argv[0]);
        exit(EXIT_FAILURE);
    }
    db->size_value = db->size_kv - (int)KV_RECORD_HEADER_SIZE;
    db->server_ring_size = kvs * 3 / db->thread;

    if (output_file) {
        db->output_fp = fopen(output_file, "a");
        if (!db->output_fp) {
            fprintf(stderr, "无法打开输出文件 '%s': %s\n",
                    output_file, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    fflush(db->output_fp);

    return ;
}

void db_get_default(struct db * db){
    memset(db, 0, sizeof(*db));
    db->boundary_size = LIBDB_DEFAULT_BOUNDARY_SIZE;
    db->load_injectors = LIBDB_DEFAULT_LOAD_INJECTORS;
    db->tasks_per_thread = LIBDB_DEFAULT_TASKS_PER_THREAD;
    db->wal_mb_per_worker = WAL_DEFAULT_MB;
    db->wal_wait_us = WAL_DEFAULT_WAIT_US;
    db->wal_max_inflight = WAL_DEFAULT_MAX_INFLIGHT;
    db->load_only = false;
    db->recovery_mode = 0;
    db->recovery_dupcheck = 0;
    db->recovery_scan_full = false;
    db->output_fp = stdout;
}

int main( int argc, char *argv[] ){
    struct db * db = (struct db *)calloc(1, sizeof(struct db));
    struct thread * thread;
    int main_core;
    int i , j ;

    if (db == NULL || libdb_config_init_from_env() != 0) {
        fprintf(stderr, "无法初始化运行时配置\n");
        free(db);
        return EXIT_FAILURE;
    }
    atomic_init(&db->gc_space.relaxed, false);
    db_get_default(db);
    space_stats_nvm_init(db);
    parse_arg(argc , argv , db);

    thread = initialize(db);
    if(thread == NULL){
        fprintf(stderr, "初始化失败（%s）\n", db->recovery_mode ? "recovery" : "正常路径");
        return EXIT_FAILURE;
    }
    main_core = spdk_env_get_current_core();
    printf("main_core is %d\n" , main_core);
    fflush(stdout);
    g_ticks = spdk_get_ticks_hz();
    benchmark_completed = 0;
    for( i = 0 , j = 0 ; i < db->thread + 1; i ++ ){
        if( i == main_core ){
            continue;
        }
        thread[j].thread = j;
        thread[j].db = db;
        thread[j].core = i;
        j++;
        printf("start server thread %d , core is %d\n" , j-1 , i);
        if(spdk_env_thread_launch_pinned( i , (thread_start_fn)server_executor , &thread[j-1])){
            printf("failed to start server thread\n");
			return 0;
        }
        else{
            printf("start server thread %d done\n" , j-1);
        }
    }

    printf("start client thread\n");
    client_executor(db);

    fflush(db->output_fp);
    benchmark_completed = 1;
    space_stats_nvm_check_capacity(db);
    spdk_env_thread_wait_all();
    space_stats_nvm_check_capacity(db);
    space_stats_print(db, stdout);
    if(db->output_fp != stdout) space_stats_print(db, db->output_fp);
    for(int worker = 0; worker < db->thread; worker++) {
        wal_print_stats(&thread[worker], stdout);
        if(db->output_fp != stdout) wal_print_stats(&thread[worker], db->output_fp);
        wal_destroy(&thread[worker]);
    }
    return db->load_only && (!db->initial_load.complete || db->initial_load.error)
        ? EXIT_FAILURE : EXIT_SUCCESS;
}
