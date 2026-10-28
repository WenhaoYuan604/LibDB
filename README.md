# LibDB

当前版本：**0.1.0**。版本更新和兼容性说明见 [CHANGELOG.md](CHANGELOG.md)。

本仓库提供 LibDB 的可复现实验代码和脚本。下面只给出从编译到运行 YCSB A/C 与 recovery 的最短流程。

> [!CAUTION]
> LibDB 通过 SPDK 直接访问 NVMe SSD，实验会覆盖目标设备上的数据；测试脚本还会清理指定的 LibDB PMEM 子目录。请只使用允许完全丢弃数据的专用 SSD 和 PMEM 路径，绝不能使用系统盘或包含重要数据的设备。

## 1. 环境要求

- Linux、CMake 3.13+、C 编译器和 `pkg-config`
- 已编译的 [SPDK](https://spdk.io/)
- PMDK（`libpmem`、`libpmemobj`）和 `libnuma`
- 4 KiB sector 的专用 NVMe SSD
- 已挂载且可写的 PMEM/DAX 文件系统
- Bash、Git、Java 和 Maven（仅生成 YCSB workload 时需要）

## 2. 编译

```bash
git clone https://github.com/WenhaoYuan604/LibDB.git
cd LibDB

export SPDK_ROOT=/path/to/spdk
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT"
cmake --build build --parallel
```

编译成功后，可执行文件为 `build/test`。

## 3. 准备 workload

如果已经有 `warm.txt`、`ycsba.txt` 和 `ycsbc.txt`，可以跳过本节。

快速生成每个文件 1,000 条请求，用于检查生成流程：

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/path/to/new/disposable/ycsb-rocksdb \
RECORD_COUNT=1000 \
YCSBA_OPERATION_COUNT=1000 \
YCSBC_OPERATION_COUNT=1000 \
YCSBE_OPERATION_COUNT=1000 \
./artifact/ycsb/generate_workloads.sh
```

生成完整实验 workload：

```bash
YCSB_ALLOW_DESTRUCTIVE=1 \
YCSB_DB_DIR=/path/to/new/disposable/ycsb-rocksdb \
./artifact/ycsb/generate_workloads.sh
```

默认输出目录为 `artifact/ycsb/generated/`。完整配置会生成 100M 条 warm、100M 条 YCSB A、100M 条 YCSB C 和 20M 条 YCSB E 请求，需要预留足够的磁盘空间和生成时间。`YCSB_DB_DIR` 必须是一个尚不存在、允许脚本创建并删除的临时 RocksDB 目录。

## 4. 将实验 SSD 绑定到 SPDK

先确认 PCI 地址对应的是专用实验盘，并卸载该 SSD 上的所有文件系统，然后执行：

```bash
sudo env \
  PCI_ALLOWED=0000:5c:00.0 \
  DRIVER_OVERRIDE=uio_pci_generic \
  "$SPDK_ROOT/scripts/setup.sh" config

lspci -s 0000:5c:00.0 -nnk
```

`Kernel driver in use` 应为 `uio_pci_generic` 或 `vfio-pci`。请把示例 PCI 地址替换为本机实验 SSD 的地址。

## 5. 快速 smoke test

该测试使用 1 KiB KV 和 1,000 条请求，并在运行前后清理自己的 PMEM 子目录：

```bash
sudo env \
  LIBDB_ALLOW_DESTRUCTIVE=1 \
  PCI_ADDR=0000:5c:00.0 \
  PMEM_BASE=/mnt/pmem0/libdb \
  ./run_smoke_1k.sh
```

日志和结果默认写入：

```text
/tmp/libdb-clean-smoke-1k.log
/tmp/libdb-clean-smoke-1k-result.txt
```

## 6. YCSB A/C + recovery 实验

下面的脚本依次运行：fresh database + YCSB A、fresh database + YCSB C、恢复 C 阶段的数据并再次运行 YCSB C。默认配置为 16 个 workers、8 个 injectors、20,000 MiB cache、1 KiB KV 和 100M KV。

```bash
sudo env \
  LIBDB_ALLOW_DESTRUCTIVE=1 \
  WORKLOAD_DIR="$PWD/artifact/ycsb/generated" \
  PCI_ADDR=0000:5c:00.0 \
  PMEM_BASE=/mnt/pmem0/libdb \
  ./run_heavy_ac_recovery.sh
```

结果和日志保存在：

```text
results/heavy_ac_<timestamp>/
├── ycsba.log
├── ycsba.result.txt
├── ycsbc.log
├── ycsbc.result.txt
├── recovery_ycsbc.log
└── recovery_ycsbc.result.txt
```

如需限制每个阶段的运行时间，可在命令中加入 `TIMEOUT_SECONDS=21600`。查看所有可覆盖的路径和参数：

```bash
./run_smoke_1k.sh --help
./run_heavy_ac_recovery.sh --help
./artifact/ycsb/generate_workloads.sh --help
```

测试脚本只清理各自的 `PMEM_DIR`，不会删除 workload；无论正常结束、失败、超时或收到 Ctrl-C，都会尝试执行清理。

## 7. 删除触发的 range-group merge

每次 `db_delete` 完成后（包括键不存在的情况），检查所在 group 的左右邻居。
存活键数为 GWB 与 SSD item index 中键的去重并集，不把同一键的多个版本重复计数。
至少一侧低于 `LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD`，且总数不超过
`LIBDB_SUBSPACE_MAX_KEYS` 时可以合并。下限默认是上限的一半：当前为 **64 / 128**，
比较条件分别为严格 `< 64` 和 `<= 128`。下限在 `config.h` 定义，也可通过
编译器的 `-DLIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD=...` 覆盖。

合并只在同一 worker 的同一初始固定键区间内进行，不跨越 worker 的静态路由边界。
左右均符合条件时选择总键数较少的一侧，相同则选左侧；保留左侧 lower fence。
每次删除最多触发一次合并。邻居正忙或没有空闲 I/O task 时，由 worker 的维护队列重试。
删除遇到本组的 flush、split、GC 或 merge 时，等待 I/O 完成并重新定位 group。

逻辑合并先拼接 buffer/cache 索引，压紧 DRAM item index，再移除共享 fence。
此时保留原 PMEM 索引，读取仍使用旧 SSD 地址。迁移期间的新写入进入合并后的 GWB，
暂停该组 flush、GC 和其他结构变更。目标块使用 FUA 写入；全部完成后更新 PMEM slots，
再回收旧 slots 和源块。未完成的 get/scan 会保留旧 group 和源块的引用，避免提前复用。
完成后按 DMA 容量和 slot 容量分批处理积压 GWB，并继续使用现有 split/GC 流程。

本实现不提供 **merge 中途崩溃恢复协议**。正常完成后的持久索引可由现有 recovery
代码恢复。发布逻辑合并后的 I/O 或持久化错误会终止进程并保留源块，不保证此时可恢复。
输出新增 `Merge completed`、`read blocks`、`write blocks` 统计。

### 无设备自动化测试

测试模拟 NVMe 提交和回调，PMDK 事务使用 `/tmp` 下的普通临时文件；不打开真实 NVMe
设备，不需要 root，也不运行上述会覆盖实验盘的脚本。仍需编译依赖 SPDK headers 和 PMDK。

```bash
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

为避免与 CTest 的 `test` 目标重名，主程序 CMake 目标名改为 `libdb`，可执行文件仍为
`build/test`，原运行脚本不变。可用 `-DBUILD_TESTING=OFF` 关闭测试构建。

启用 AddressSanitizer / UndefinedBehaviorSanitizer：

```bash
cmake -S . -B /tmp/libdb-merge-asan -DSPDK_ROOT="$SPDK_ROOT" \
  -DLIBDB_TEST_SANITIZERS=ON
cmake --build /tmp/libdb-merge-asan --target merge_test allocator_test --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/libdb-merge-asan --output-on-failure
```

测试以独立子进程隔离场景，覆盖阈值、重复键、空组、跨叶节点 fence 删除、cache/LRU、
延迟 get/scan、迁移期间写入、删除等待、资源重试、I/O/提交失败、变长记录、积压 flush、
后续 split/GC，以及关闭并重新打开 PMDK pool 后调用现有 recovery 重建索引。

### 动态 GC 阈值

初始 load 成功完成后，固定插入记录的 value 长度与记录头之和作为数据库大小（重复插入按输入计数）。main 线程立即采样并每 100ms 读取 SSD 被引用数据块的原子计数；空间放大为被引用块字节数除以初始 load 大小，不计 PMEM、缓存或无引用预分配块，也不强制刷盘。空间放大 ≤1.30 时，冗余阈值放宽为 0.55，对应垃圾占用比例阈值 0.55/1.55；否则使用 0.30，对应比例 0.30/1.30；GC 触发与块筛选使用同一次阈值快照。首次采样及阈值切换不输出日志。load 前、统计无效及跳过 load 的恢复模式保持 0.3/1.3。

### Uniform / Zipfian YCSB A、F 测试

`run_ycsba_uniform_zipfian.sh` 保留原文件名，固定三轮，每轮按 **uniform A → uniform F → zipfian A → zipfian F** 的顺序运行，共十二次。每次使用全新数据库，完成对应分布的 load、缓存预热后执行正式 workload。`REPEATS` 环境变量不改变次数。

脚本使用当前已编译程序的 GC 策略，不自动编译、生成数据、修改 CPU/NUMA 或切换 NVMe 驱动。默认 `LIBDB_ALLOW_DESTRUCTIVE=1`、`PCI_ADDR=0000:5c:00.0`、`PMEM_BASE=/mnt/whypmem`，直接运行：

```bash
sudo ./run_ycsba_uniform_zipfian.sh
```

设备必须已绑定 SPDK 驱动，PMEM 父目录必须存在。默认值可通过 `sudo env PCI_ADDR=... PMEM_BASE=... ./run_ycsba_uniform_zipfian.sh` 覆盖；显式设置 `LIBDB_ALLOW_DESTRUCTIVE=0` 阻止运行。脚本仅清理本次创建的专属 PMEM 子目录，不删除父目录及已有内容。

| 环境变量 | 默认值 / 含义 |
| --- | --- |
| `WORKLOAD_ROOT` | `/mnt/nvme0n1/why/ycsb_workloads_one_warm` |
| `UNIFORM_WARM_FILE`、`UNIFORM_YCSBA_FILE`、`UNIFORM_YCSBF_FILE` | 根目录下 `uniform/warm.txt`、`uniform/ycsba.txt`、`uniform/ycsbf.txt` |
| `ZIPFIAN_WARM_FILE`、`ZIPFIAN_YCSBA_FILE`、`ZIPFIAN_YCSBF_FILE` | 根目录下 `zipfian/warm.txt`、`zipfian/ycsba.txt`、`zipfian/ycsbf.txt` |
| `BINARY` | 项目目录下 `build/test` |
| `THREADS`、`LOAD_INJECTORS` | 16、8 |
| `KV_SIZE`、`KV_COUNT` | 1024 字节、100000000；KV_COUNT 不截断输入 |
| `CACHE_SIZE_MB` | 20000 MiB |
| `TASKS_PER_THREAD`、`BOUNDARY_SIZE` | 16、1000 |
| `RESULT_BASE` | 项目目录下 `results` |
| `USE_SUDO` | 1，非 root 用户通过 sudo 运行数据库和准备 PMEM |
| `TIMEOUT_SECONDS`、`KILL_AFTER_SECONDS` | 0（不限时）、30（中断后的退出等待秒数） |

开始前检查全部六个输入文件并统计请求数。warm 和 A 必须为至少 1000 的千条整数倍。F 沿用数据库读取器向下截取整千条的行为，在配置和汇总中记录 `raw_requests`（原始条数）、`requests`（执行条数）、`tail_requests`（舍弃条数）。尾部 READ/UPDATE 仍会进入程序打印的操作计数，汇总工具会按尾部内容扣除，再校验实际执行数；不修改原始输入或数据库代码。

**F 吞吐量按实际执行的 READ/UPDATE 请求数计算，不是逻辑 read-modify-write 事务吞吐量。** load 和缓存预热不计入正式吞吐量。

每次调用创建唯一结果目录，保存 `config.json`、每阶段命令、完整日志、程序结果、`summary.csv` 和 `aggregate.csv`。文件名包含轮次、分布、workload，例如 `01_uniform_A.log`、`01_uniform_F.result.txt`。汇总按 `distribution + workload` 分为四组，各自统计成功次数、平均吞吐量、样本标准差、最小值和最大值；只有一次成功时标准差为 0，无成功记录时统计值留空。程序产生的 GC 日志原样保留。

程序、日志写入、解析失败或超时、中断时停止后续运行，返回非零状态并保留结果。清理前终止当前数据库；若无法终止则保留 PMEM 目录并报错。汇总仅纳入成功运行。旧的无 workload 列的 summary 文件重新汇总时按 A 处理。

```bash
# 重新生成分布 + workload 汇总
python3 tools/summarize_ycsba.py results/<本次运行目录>/summary.csv
# 帮助与无设备模拟测试
./run_ycsba_uniform_zipfian.sh --help
python3 tests/test_ycsba_runner.py -v
```

Bash 入口调用 Python 3 标准库工具，无第三方 Python 依赖。`SKIP_DRIVER_CHECK=1` 仅供模拟程序测试；真实实验保持默认驱动检查。

### Uniform YCSB A：动态 Flush 关闭 / 开启对比

```bash
sudo ./run_ycsba_dynamic_flush.sh
```

脚本先从当前源码独立构建动态 Flush **关闭（0）** 和 **开启（1）** 两个版本，两个版本
均构建成功后，按 **OFF → ON** 顺序各运行一次 uniform YCSB A。每次创建全新数据库，
依次执行 load、缓存预热和正式 A；两次使用相同输入和参数。编译时仅
`LIBDB_DYNAMIC_FLUSH_ENABLED` 不同，其他配置来自当前源码，包括 GC 策略及当前默认的
16GiB / 100ms NVM 控制参数。开启版本仍只在正式 workload 使用动态 Flush。

**编译输出、数据库 stdout 和 stderr 都会实时出现在运行脚本的终端中，同时保存日志。**
数据库使用 `stdbuf -oL -eL` 启动，避免 C 标准输出接入管道后变成整块缓冲；无需另开终端
执行 `tail`。原程序通过 `--output` 写出的结果文件单独保留，结束后在终端打印对比汇总。

默认沿用 `PCI_ADDR=0000:5c:00.0`、`PMEM_BASE=/mnt/whypmem`、
`LIBDB_ALLOW_DESTRUCTIVE=1`，16 workers、8 injectors、1024 字节 KV、100M KV、
20000MiB cache、16 tasks/worker、boundary size 1000。设备需已绑定 SPDK 驱动。
只要求 uniform 的 `warm.txt` 和 `ycsba.txt`，不读取 F、E 或 zipfian 输入。

| 环境变量 | 默认值 / 含义 |
| --- | --- |
| `WORKLOAD_ROOT` | `/mnt/nvme0n1/why/ycsb_workloads_one_warm` |
| `UNIFORM_WARM_FILE`、`UNIFORM_YCSBA_FILE` | 根目录下 `uniform/warm.txt`、`uniform/ycsba.txt` |
| `BUILD_JOBS`、`SPDK_ROOT` | 6；项目同级 `spdk` 目录 |
| `RESULT_BASE` | 项目下 `results` |
| `TIMEOUT_SECONDS` | 0（不限时）；非零时分别限制每个配置、构建或数据库进程 |
| `KILL_AFTER_SECONDS`、`USE_SUDO` | 30；1（非 root 用户通过 sudo 准备 PMEM、运行数据库） |

设备、线程、KV/cache 等环境变量与上方 A/F 脚本一致。`REPEATS` 和 `BINARY` 被忽略：
固定各跑一次，并使用本次构建的两个二进制。需要 `cmake`、`stdbuf` 及正常编译依赖。
例如修改编译并行度和 worker 数：

```bash
sudo env BUILD_JOBS=8 THREADS=16 ./run_ycsba_dynamic_flush.sh
```

每次调用生成 `results/ycsba_dynamic_flush_<时间>_<唯一标识>/`，保留：

- `build/off/test`、`build/on/test` 以及各自构建目录；不覆盖现有 `build/test`，不修改源码配置。
- `config.json`、配置/构建/运行命令 JSON、构建日志、`off_uniform_A.log`、`on_uniform_A.log` 和对应 `.result.txt`。
- `summary.csv`：每种模式的正式耗时、吞吐量、NVM 最终槽位占用、采样峰值、压力切换次数及状态。
- `comparison.txt`：OFF、ON 吞吐量及 `(ON / OFF - 1) * 100%`；`status.json`：执行状态和失败阶段。

仅从正式阶段提取吞吐量，不计 load、预热。脚本核对请求数、日志与结果文件的 NVM
开关和统计，并检查两次预算和采样周期一致。关闭模式未启用采样，其采样峰值、切换次数
通常为 0，不应当作实际占用峰值；最终槽位占用仍可用于比较。

输入或构建失败时不启动数据库；运行失败、统计不符、超时或中断时停止后续测试并保留
结果。OFF 结束后释放其专属 PMEM 目录，再运行 ON。清理只涉及本次新建的私有子目录，
不删除 PMEM 父目录或已有数据；无法终止数据库时保留对应目录。脚本不切换驱动、不进行
CPU/NUMA 调优，也不生成 workload。

```bash
./run_ycsba_dynamic_flush.sh --help
# 模拟 CMake、数据库和临时 PMEM 目录，不访问实验设备
python3 tests/test_ycsba_dynamic_flush.py -v
```

### Workload 完成后的 SSD 空间放大

每次正式 workload 完成后，worker 排空已提交 I/O 和已有维护任务，main 等待全部线程退出，再输出一次最终 SSD 空间统计到终端和 `--output` 文件（两者相同时只打印一次）。收尾和统计位于正式吞吐量计时之后，不强制刷出未达到正常刷盘条件的 PMEM 缓冲数据；运行中的 GC 阈值调整继续保持静默。

输出 `initial_load_record_bytes`、`ssd_data_used_blocks`、`ssd_data_used_bytes`、九位小数的 `ssd_space_amplification`，以及 `ssd_online_used_blocks`、`online_counter_matches_snapshot`。分母是初始 load 插入记录总字节数，分子是引用表中非零数据块数乘以块大小；多引用块只计一次，无引用预分配块不计。PMEM 和缓存不计入该 SSD 指标。load 未完成、恢复模式跳过 load、分母为零或引用表状态无效时，空间放大显示 `N/A` 并说明原因。

### Uniform YCSB E 延迟（TASK=2）

直接运行独立脚本：

```bash
sudo ./run_ycsbe_latency_task2.sh
```

固定进行三次独立 uniform E 实验，每次创建全新数据库并依次执行 load、缓存预热、E。固定 `--tasks-per-thread=2`（每个 worker 两个 task），忽略环境中的 `TASKS_PER_THREAD` 和 `REPEATS`。不执行 YCSB A 预处理，不要求 A、F 或 zipfian 工作负载，不改变已有 A/F 脚本。

默认使用 `build/test`、16 个 worker、8 个 injector、1024 字节 KV、100000000 KV 配置、20000 MiB cache、boundary size 1000；PCI 为 `0000:5c:00.0`，PMEM 父目录为 `/mnt/whypmem`，`LIBDB_ALLOW_DESTRUCTIVE=1`。使用当前二进制的 GC 策略，不自动编译或进行系统调优。

默认读取 `/mnt/nvme0n1/why/ycsb_workloads_one_warm/uniform/warm.txt` 和 `ycsbe.txt`。可通过 `WORKLOAD_ROOT` 或 `UNIFORM_WARM_FILE`、`UNIFORM_YCSBE_FILE` 覆盖路径；输入必须至少 1000 条且为千条整数倍，避免数据库读取器截断。其余设备、线程、KV/cache、二进制、结果目录和超时参数与现有脚本一致，详见 `--help`。

只提取正式 E 阶段的所有请求延迟（scan 与插入混合，每个 scan 算一个请求）：平均值、P50、P95、P99、P99.9、最大值，单位均为微秒，同时记录吞吐量。计时从 worker 开始处理至生成结果，不包含此前的请求排队时间；平均值是直方图区间中点估计，分位数和最大值使用桶上界。load 和缓存预热统计不计入结果。正式阶段数据缺失、重复或无效会报错，不回退到其他阶段。

结果在 `results/ycsbe_latency_task2_<时间>_<唯一标识>/`：`config.json`、每轮命令、完整日志及程序结果、`summary.csv` 三轮明细、`aggregate.csv` 各指标跨轮均值与样本标准差。**跨轮 P99 等分位数的均值不是合并所有请求后的总体分位数。** 失败轮次不参与聚合，成功次数单独列出，只有一轮成功时样本标准差字段按既有约定记为 0。

沿用设备检查、超时和中断处理；只清理本次新建的专属 PMEM 子目录。失败后停止后续轮次并保留日志，进程无法停止时保留其 PMEM 目录。

```bash
# 无设备模拟测试
python3 tests/test_ycsbe_latency.py -v
# 重新生成跨轮汇总
python3 tools/summarize_ycsbe_latency.py results/<本次运行目录>/summary.csv
```

### 动态 NVM Flush

动态 Flush 只在正式 workload 请求注入前启用；初始 load、cache 预热和测试请求文件读取期间
使用原有固定局部阈值。main 在 workload 完成轮询中每 100ms 采样 WBF 的未回收槽位字节数：
小于 16GiB 时发布压力标志 0，延后 Flush；大于等于 16GiB 时发布 1，允许所属 worker
刷出 `buffered_size > BLOCK_SIZE * Flush_Blcok_Num` 的组（默认严格超过 16KiB）。

`config.h` 中的以下宏均支持编译时覆盖，修改后需重新编译：

| 宏 | 默认值 |
| --- | --- |
| `LIBDB_DYNAMIC_FLUSH_ENABLED` | `1`，设为 `0` 恢复固定局部阈值行为 |
| `LIBDB_NVM_FLUSH_LIMIT_BYTES` | `16 * 1024 * 1024 * 1024` 字节 |
| `LIBDB_NVM_FLUSH_POLL_NS` | `100000000` 纳秒 |

占用按记录实际申请的 2 的幂槽位大小统计，包括记录头和内部对齐浪费。覆盖写、删除和
Flush 完成后实际归还槽位才扣减；正在写 SSD 的冻结批次仍计入占用。恢复启动时扫描
所有 WBF 有效槽位重建计数，尚未回收的旧版本也计入，且不依赖初始 load 是否执行。
分裂、合并只移动索引，不改变占用。回收失败不会把无法复用的空间当成已释放空间。

每个 worker 用按路由 key 去重的延后队列保存候选组；压力为 1 时每次维护轮询最多处理
16 个条目，即使没有后续写入也能启动 Flush。压力解除后停止新的压力 Flush，已提交的
I/O 正常完成。main 收到全部正式请求结果后立即停止新的压力 Flush；退出时排空已提交
I/O 和必要维护，不强制刷空延后组，也不会因延后队列非空而等待。

16GiB 是软阈值，采样与 I/O 延迟可能导致超出，且不保证所有工作负载都会填满预算。
该口径不包含空闲槽位、WBF 文件预分配、PMDK 元数据和 NVM 索引，不是物理文件大小上限。
文件不会因 Flush 缩小，本改动不提供文件回收、硬分配限额或写入背压。

启动和结束日志输出配置；结束统计包括 `nvm_used_slot_bytes`、`nvm_sampled_peak_bytes`
和 `nvm_pressure_changes`。峰值是正式 workload 的采样峰值，不是逐次分配的瞬时峰值。
新增测试以 64KiB 小预算验证完整压力循环、忙组/资源重试、分裂合并和停机；真实 PMDK
临时文件测试覆盖分配回收、并发计数及恢复。新建 segment 使用 PMDK 返回的准确地址并
清零，修复旧分配器向下对齐可能覆盖对象前元数据的问题；持久化结构格式保持不变。
占用重建遇到非空但无法解析的槽位会报错退出，不尝试修复已有损坏数据。
