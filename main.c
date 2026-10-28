#include <stdio.h>
#include <math.h>
#include <strings.h>
#include "base.h"
#include "kv_record.h"
#include "kvbuffer.h"
#include "index.h"
#include "leaf_node_file.h"
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
        "          --pci-addr=DDDD:BB:DD.F --pmem-dir=PATH\n"
        "          --warm=FILE --test=FILE [options]\n\n"
        "Required runtime configuration:\n"
        "  --pci-addr=ADDR          NVMe controller PCI BDF\n"
        "  --pmem-dir=PATH          Directory for PMDK pool files\n"
        "  --warm=FILE              Load/warmup trace\n"
        "  --test=FILE              Test trace\n\n"
        "Runtime tuning options:\n"
        "  --thread=N               Server worker count\n"
        "  --kvsize=BYTES           Total persistent record size\n"
        "  --kvs=N                  KV count used for queue sizing\n"
        "  --cachesize=MB           KV cache capacity in MiB\n"
        "  --load-injectors=N       Client injector count (default %d)\n"
        "  --tasks-per-thread=N     Outstanding tasks per worker (default %d)\n"
        "  --boundary-size=N        Initial boundary sample size (default %d)\n"
        "  --output=FILE            Append statistics to FILE\n"
        "  --recovery[=0|1]         Rebuild indexes from persistent data\n"
        "  --recovery-dupcheck=0|1  Check duplicate keys after recovery\n"
        "  --help                   Show this message\n\n"
        "The PCI address, PMEM directory, and trace files may also be set with\n"
        "LIBDB_PCI_ADDR, LIBDB_PMEM_DIR, LIBDB_WARM_FILE, and LIBDB_TEST_FILE.\n"
        "Persistent layout parameters are centralized in config.h.\n",
        program, LIBDB_DEFAULT_LOAD_INJECTORS,
        LIBDB_DEFAULT_TASKS_PER_THREAD, LIBDB_DEFAULT_BOUNDARY_SIZE);
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

        // 验证参数格式
        if (strncmp(param, "--", 2)) {
            fprintf(stderr, "参数'%s'格式错误：未知开关（recovery 可用 --recovery 或 -recovery；其它参数须为 --key=value）\n", param);
            exit(EXIT_FAILURE);
        }

        char *eq = strchr(param, '=');
        if (!eq) {
            fprintf(stderr, "参数'%s'格式错误：缺少等号（布尔开关请用 --recovery 或 --recovery=1）\n", param);
            exit(EXIT_FAILURE);
        }

        // 提取键值对
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


        // 处理不同参数
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
            output_file = unquote(value);  // 处理路径引号
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
        else if (strcmp(key, "recovery") == 0) {
            db->recovery_mode = parse_bool_arg(value);
        }
        else if (strcmp(key, "recovery-dupcheck") == 0) {
            db->recovery_dupcheck = parse_bool_arg(value);
        }
        else {
            fprintf(stderr, "未知参数：%s\n", key);
            exit(EXIT_FAILURE);
        }
    }

    // 验证必须参数
    if (db->thread == 0 || db->size_kv == 0 || kvs == 0) {
        fprintf(stderr, "缺少必要参数：\n");
        if (db->thread == 0) fprintf(stderr, "  --thread 必须指定\n");
        if (db->size_kv == 0) fprintf(stderr, "  --kvsize 必须指定\n");
        if (kvs == 0) fprintf(stderr, "  --kvs 必须指定\n");
        exit(EXIT_FAILURE);
    }
    if ((size_t)db->size_kv < KV_RECORD_PERSISTED_SIZE) {
        fprintf(stderr,
                "--kvsize 至少为 %zu 字节（%zu字节header + %zu字节有效value前缀）\n",
                KV_RECORD_PERSISTED_SIZE, KV_RECORD_HEADER_SIZE,
                KV_RECORD_VALID_VALUE_SIZE);
        exit(EXIT_FAILURE);
    }
    if ((size_t)db->size_kv > LIBDB_WBF_MAX_RECORD_SIZE) {
        fprintf(stderr,
                "--kvsize 不能超过 %u 字节（write-buffer的2次幂分配粒度存储为uint16_t）\n",
                (unsigned)LIBDB_WBF_MAX_RECORD_SIZE);
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
    if (libdb_config_validate(config_error, sizeof(config_error)) != 0) {
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

    printf("参数解析成功：\n");
    printf("模式: %s%s\n", db->recovery_mode ? "recovery（从 NVM 重建索引）" : "正常初始化",
           db->recovery_dupcheck ? "；recovery 后将执行全量键重复检查" : "");
    printf("线程数: %d\n", db->thread);
    printf("键值大小: %d\n", db->size_kv);
    printf("PMEM有效value前缀: %zu 字节（逻辑value仍为 %d 字节）\n",
           KV_RECORD_VALID_VALUE_SIZE, db->size_value);
    printf("客户线程提交队列深度: %d\n" , db->server_ring_size);
    printf("kvcache大小: %lu MB\n" , db->cachesize);
    printf("load injectors: %d\n", db->load_injectors);
    printf("tasks per thread: %d\n", db->tasks_per_thread);
    printf("boundary size: %d\n", db->boundary_size);
    libdb_config_print(stdout);

    if (output_file) {
        printf(" 输出文件: %s\n", output_file);
        fprintf(db->output_fp, "pci: %s pmem: %s warm: %s test: %s subspace_size %zu\n",
                g_libdb_config.pci_addr, g_libdb_config.pmem_dir,
                g_libdb_config.warm_path, g_libdb_config.test_path,
                (size_t)SUBSPACE_MAX_KEY);
        fprintf(db->output_fp, "参数配置：\nthread=%d\nkvsize=%d\nkvs=%d\ncachesize=%luMB\n",
                db->thread, db->size_kv, kvs , db->cachesize);
        fprintf(db->output_fp,
                "LIBDB_FLUSH_BLOCK_COUNT=%d\nLIBDB_SUBSPACE_MAX_KEYS=%zu\n"
                "LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD=%zu\nLIBDB_SUBSPACE_MAX_SLOTS=%zu\nLIBDB_LNF_SLOT_ENTRY_CAPACITY=%zu\n"
                "LIBDB_SUBSPACE_REDUNDANT_THRESHOLD=%f\nLIBDB_LNF_SLOT_SIZE=%u\n"
                "LIBDB_LNF_POOL_SIZE_BYTES=%llu\nLIBDB_FREE_SPACE_GROUP_BLOCKS=%d\n",
                Flush_Blcok_Num, (size_t)SUBSPACE_MAX_KEY, (size_t)LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD, (size_t)SUBSPACE_MAX_SLOT,
                (size_t)LIBDB_LNF_SLOT_ENTRY_CAPACITY, SUBSPACE_REDUNDANT_PERCENT_THRESHOLD,
                LNF_SLOT_MIN_SIZE, (unsigned long long)LNF_SIZE, alloc_grain);
    }
    fflush(db->output_fp);

    return ;
}

void db_get_default(struct db * db){
    memset(db, 0, sizeof(*db));
    db->boundary_size = LIBDB_DEFAULT_BOUNDARY_SIZE;
    db->load_injectors = LIBDB_DEFAULT_LOAD_INJECTORS;
    db->tasks_per_thread = LIBDB_DEFAULT_TASKS_PER_THREAD;
    db->recovery_mode = 0;
    db->recovery_dupcheck = 0;
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
    db_get_default(db);
    atomic_init(&db->gc_space.relaxed, false);
    space_stats_nvm_init(db);
    parse_arg(argc , argv , db);
    space_stats_nvm_print(db, db->output_fp);

    thread = db->recovery_mode ? initialize_recovery(db) : initialize(db);
    if(thread == NULL){
        fprintf(stderr, "初始化失败（%s）\n", db->recovery_mode ? "recovery" : "正常路径");
        return EXIT_FAILURE;
    }
    db->data_blocks = g_block_num;
    main_core = spdk_env_get_current_core();//main_core = 0;
    printf("main_core is %d\n" , main_core);
    fflush(stdout);
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
        if(spdk_env_thread_launch_pinned( i , server_executor , &thread[j-1])){
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
    spdk_env_thread_wait_all();
    space_stats_nvm_print(db, stdout);
    if(db->output_fp != stdout) space_stats_nvm_print(db, db->output_fp);
    space_stats_print(db, stdout);
    if(db->output_fp != stdout) space_stats_print(db, db->output_fp);
    return 0;
}
