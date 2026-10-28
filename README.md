# LibDB

当前版本：**0.4.0**。版本更新和兼容性说明见 [CHANGELOG.md](CHANGELOG.md)。

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
合并完成后不主动 Flush；后续写入该组时，按 DMA 容量和 slot 容量启动有界批次，
并继续使用现有 split/GC 流程。

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
cmake --build /tmp/libdb-merge-asan --target merge_test merge_off_test gc_tail_wbf_test allocator_test --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/libdb-merge-asan --output-on-failure -R '^(merge|merge_off|gc_tail_wbf|allocator)$'
```

测试以独立子进程隔离场景，覆盖阈值、重复键、空组、跨叶节点 fence 删除、cache/LRU、
延迟 get/scan、迁移期间写入、删除等待、资源重试、I/O/提交失败、变长记录、
写入驱动的有界 Flush、后续 split/GC，以及关闭并重新打开 PMDK pool 后调用现有 recovery
重建索引。`merge` 和 `merge_off` 分别验证动态 ON/OFF：高压力下维护与读取不启动 Flush、
只刷本次写入组、忙碌时写入仍生效、task 等待与取消、轮询错误退出和完成回调不续刷。

### 动态 GC 阈值

初始 load 成功完成后，固定插入记录的 value 长度与记录头之和作为数据库大小（重复插入按输入计数）。main 线程立即采样并每 100ms 读取 SSD 被引用数据块的原子计数；空间放大为被引用块字节数除以初始 load 大小，不计 PMEM、缓存或无引用预分配块，也不强制刷盘。空间放大 ≤1.30 时，冗余阈值放宽为 0.55，对应垃圾占用比例阈值 0.55/1.55；否则使用 0.30，对应比例 0.30/1.30；GC 触发与块筛选使用同一次阈值快照。首次采样及阈值切换不输出日志。load 前、统计无效及跳过 load 的恢复模式保持 0.3/1.3。

### GC 尾块补齐与回迁

GC 整理 SSD 有效记录后，如果输出有多个块且最后一块不足 4KiB，会按 key 升序尝试
使用本组活动 PMEM 写缓冲的前缀补齐。只有记录恰好填满剩余空间，且最终 SSD 索引不超过
key/slot 容量时才补齐；遇到下一条放不下的记录就停止，不跳过记录。补齐批次会冻结并保持
可读，期间的新写入进入活动缓冲。单个不足 4KiB 的输出块以及完整尾块保持正常 SSD 写入。

无法精确补齐时，候选缓冲记录保持不变，GC 把 SSD 尾块记录回迁 PMEM，少写一个 SSD 块。
回迁保留原始版本、逻辑 value 长度和有效的 32 字节前缀，已有相同或更新的 PMEM 版本不会
被覆盖。回迁沿用 WBF 分配、回收和空间计数接口：压力标志不禁止回迁，但仍遵守现有 16GiB
文件容量硬上限；没有可用槽位且无法扩容时沿用失败终止策略。回迁过程和该分支的完成回调
不新增 Flush 请求，只有后续前台写入该组才会再次检查 Flush 条件。

GC 目标 SSD 块的写入标志为 `0`；全部写入完成后仅提交受影响的索引槽位，再更新内存元数据、
回收冻结记录和无引用源块。尚未完成的读取会推迟物理块复用。补齐按 key 合并映射，避免
旧 SSD 地址与补齐记录重复；不再引用的输出块不会分配或写入。持久化记录格式不变，
沿用现有逐槽位提交及恢复协议，不提供跨槽位的原子提交保证。
GC 保留原有槽位布局，只更新搬迁记录的地址；回迁只在原槽位内删除并压紧条目，回收变空的
槽位。补齐新增 key 沿用槽内有序插入及必要的局部槽位分裂，不排序、重排或提交整个组。
补齐前按各槽位实际需要的容量检查上限，无法容纳时采用回迁。
普通搬迁和尾部回迁保留现有缓存；仅在补齐批次解除冻结前删除该批次 key 的旧缓存。
块引用只更新被搬迁、回迁或补齐替换的映射，未受影响映射保持不变。打包暂存区按本次
选中块数乘以 4KiB 分配，检查实际容量边界，仅清零输出块未使用的部分。变长记录按 key
重排后若超出该容量，沿用失败终止策略并保留源块，不自动扩大暂存区。

`merge` 测试包含尾块分支、覆盖写、延迟 get/scan、计数、维护队列及故障注入；
`gc_tail_wbf` 使用模拟 NVMe 和真实临时 PMDK/WBF 文件，覆盖补齐、回迁、重启恢复及小预算
硬上限耗尽。两者均不访问真实 SSD，可单独运行：

```bash
cmake --build build --target merge_test gc_tail_wbf_test --parallel
ctest --test-dir build --output-on-failure -R '^(merge|gc_tail_wbf)$'
```

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

脚本默认从当前源码独立构建动态 Flush **ON、OFF** 两个版本，均构建成功后
按 **ON → OFF** 各运行一次 uniform YCSB A。设置 `LIBDB_DYNAMIC_FLUSH_MODES=on`
或 `off` 可仅运行对应模式。每次创建全新数据库，依次执行 load、缓存预热和正式 A；
两种模式使用相同输入和参数，编译时仅 `LIBDB_DYNAMIC_FLUSH_ENABLED` 不同。
默认采用 16GiB WBF 文件硬上限、14GiB/12GiB 高低水位及 100ms 采样周期。
文件硬上限两种模式全程有效；开启版本仍只在正式 workload 使用动态 Flush。

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
| `LIBDB_DYNAMIC_FLUSH_MODES` | 当前源码默认 `on,off`；可选 `on`、`off` 或 `off,on` |
| `TIMEOUT_SECONDS` | 0（不限时）；非零时分别限制每个配置、构建或数据库进程 |
| `KILL_AFTER_SECONDS`、`USE_SUDO` | 30；1（非 root 用户通过 sudo 准备 PMEM、运行数据库） |

设备、线程、KV/cache 等环境变量与上方 A/F 脚本一致。`REPEATS` 和 `BINARY` 被忽略：
所选模式各跑一次，并使用本次构建的二进制。需要 `cmake`、`stdbuf` 及正常编译依赖。
例如修改编译并行度和 worker 数：

```bash
sudo env BUILD_JOBS=8 THREADS=16 ./run_ycsba_dynamic_flush.sh
```

每次调用生成 `results/ycsba_dynamic_flush_<时间>_<唯一标识>/`，保留：

- 所选模式对应的 `build/off/test`、`build/on/test` 及构建目录；不覆盖现有 `build/test`，不修改源码配置。
- `config.json`、配置/构建/运行命令 JSON、构建日志、`off_uniform_A.log`、`on_uniform_A.log` 和对应 `.result.txt`。
- `summary.csv`：每种模式的正式耗时、吞吐量、NVM 最终槽位占用、采样峰值、压力切换次数及状态。
- `comparison.txt`：双模式时包含 OFF、ON 吞吐量及 `(ON / OFF - 1) * 100%`，单模式时只报告该模式吞吐量；`status.json`：执行状态和失败阶段。

仅从正式阶段提取吞吐量，不计 load、预热。脚本核对请求数、日志与结果文件的 NVM
开关和统计，并检查两次预算和采样周期一致。关闭模式未启用采样，其采样峰值、切换次数
通常为 0，不应当作实际占用峰值；最终槽位占用仍可用于比较。

输入或构建失败时不启动数据库；运行失败、统计不符、超时或中断时停止后续测试并保留
结果。ON 结束后释放其专属 PMEM 目录，再运行 OFF。清理只涉及本次新建的私有子目录，
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

### 动态 NVM Flush 与 WBF 文件硬上限

WBF 文件容量总和在所有阶段严格限制为 16GiB；默认每个文件 1GiB，最多创建 16 个。
初始化、load、预热、正式 workload 和 recovery 均受此限制，关闭动态 Flush 也不解除
文件限额。扩容在创建文件前持有统一互斥锁检查预算，并发请求不能各自绕过检查。
恢复先统计全部 WBF 文件；总容量超过预算时拒绝启动，不删除、截断或迁移原文件。

动态 Flush 仅在正式 workload 生效。main 每 100ms 按以下口径判断压力：

`压力占用 = 已创建 WBF 文件总容量 − 可立即复用的空闲槽位容量`

达到 14GiB 时置压力标志为 1，降到 12GiB 或以下时置 0，中间区间保持原状态。
扩容慢路径达到高水位时也立即置 1，不等待 main 的下一次采样。文件总量不会因 Flush
下降，但实际归还的空闲槽位增加，可以使压力占用下降并恢复延后。未分配成槽位的预留
空间和 PMDK 文件开销均计入压力；原有未回收槽位字节数保留为独立诊断数据。

`config.h` 支持以下编译配置，修改后需重新编译：

| 宏 | 默认值 |
| --- | --- |
| `LIBDB_DYNAMIC_FLUSH_ENABLED` | `1`；`0` 恢复固定局部 Flush 策略，文件硬上限仍有效 |
| `LIBDB_WBF_CAPACITY_LIMIT_BYTES` | `16GiB` 文件硬上限 |
| `LIBDB_NVM_FLUSH_HIGH_BYTES` | `14GiB` 高水位 |
| `LIBDB_NVM_FLUSH_LOW_BYTES` | `12GiB` 低水位 |
| `LIBDB_NVM_FLUSH_POLL_NS` | `100000000` 纳秒 |

编译检查要求 `低水位 < 高水位 < 硬上限`。旧 `LIBDB_NVM_FLUSH_LIMIT_BYTES` 已由高低水位
配置替代；实验脚本仅覆盖动态开关，其他数值仍来自源码配置。

空闲量按实际槽位大小统计：新 segment 成功加入 worker 空闲链表后增加，取出槽位时
减少，覆盖写、删除、Flush 完成后成功归还才增加。Flush 提交和回收失败不增加空闲量。
recovery 仍会重建未回收记录计数，但未恢复到可用空闲链表的空槽位不计入可复用空间。

单组 Flush 条件始终为 `buffered_size > BLOCK_SIZE * Flush_Blcok_Num`，默认严格超过
16KiB。所有阶段（load、动态 ON/OFF、恢复后的写入）都仅由前台 insert/update
检查本次写入的组；不登记待 Flush 组，不在读请求、空闲轮询或完成回调中续刷。
高水位只开启 Flush 许可，不主动排空积压；没有后续写入的冷组可以长期保留缓冲，
因此压力不保证自动回到低水位。动态 OFF 和 load 沿用局部阈值，不受压力门控。

写入先正常进入缓存并更新索引，再检查是否触发 Flush。
组正在 Flush、split、GC 或 merge 时，只跳过本次 Flush，等待下一次写入该组。
组空闲但无可用 task 时，当前写入轮询自己的 qpair 等待已有 I/O 释放 task；
每次轮询后重新检查组状态、阈值和 Flush 许可，许可关闭、阶段结束或组变忙就放弃本次 Flush。
轮询错误会终止程序。等待期间不拆分缓冲批次、不分配 SSD 块、不调用维护调度、不扫描其他组。
一次写入最多提交一批（沿用最多64条记录及现有 DMA 上限），剩余缓冲等待新写入。
已提交 I/O 正常完成，GC、分裂和删除触发的合并规则保留，但完成后不额外续刷。
正式 workload 完成后排空已提交 I/O 和必要的合并维护，不强制刷空写缓冲。

本版本的 task 等待不等于 WBF 槽位分配等待；不增加 WBF 分配等待、背压、
跨 worker 空闲共享或不足 16KiB 的紧急 Flush。如果局部
槽位不足且扩容将突破预算，打印 `WBF hard limit` 并沿用分配失败终止路径，绝不创建
超预算文件。提前水位用于避免触碰限额，但不保证所有负载都能持续写入。16GiB 只约束
WBF 文件容量，不包含 leaf-node 索引或其他文件系统开销。

日志输出文件数、文件容量及峰值、可复用槽位、压力占用及采样峰值、高低水位、硬上限、
上限拒绝次数。`nvm_used_slot_bytes` 和 `nvm_sampled_peak_bytes` 仍表示原来的未回收槽位
统计；新字段 `nvm_pressure_sampled_peak_bytes` 表示压力口径的采样峰值。文件峰值在每次
创建时记录，不依赖周期采样。实验汇总同时识别新旧日志，旧日志不会被标记为硬上限已验证；
出现容量拒绝时明确报告 `capacity_limit`，即使进程已在最终统计前退出。

实验脚本默认按 ON → OFF 对比；可用 `LIBDB_DYNAMIC_FLUSH_MODES=on` 仅运行 ON，
或设为 `off,on` 调整顺序。新实验验收要求文件容量峰值不超过 16GiB、正常完成、上限拒绝次数为 0。
无设备测试以小预算临时 PMDK 文件覆盖并发扩容、回收复用及恢复拒绝；不触碰既有实验文件。

### 不同 KVsize：Uniform / Zipfian YCSB A 吞吐量（当前仅测试 A）

```bash
sudo ./run_ycsb_ae_kvsize.sh
# 可选：每个组合重复三次
sudo env REPEATS=3 ./run_ycsb_ae_kvsize.sh
```

脚本内 `REPEATS` 初始为 **1**，支持环境变量覆盖。每轮按下表从上到下执行，
每种 KVsize 依次运行 **uniform A → zipfian A**，
共 8 次独立数据库测试。固定 **TASKS_PER_THREAD=16**，默认 16 workers、8 injectors。

| KVsize（字节） | Cache（GiB / 传入的 MiB） | WBF 文件容量硬上限（GiB） | Flush 高 / 低水位（GiB） | 局部 Flush 阈值（Block / KiB） |
| --- | --- | --- | --- | --- |
| 1024 | 20 / 20480 | 16 | 14 / 12 | 4 / 16 |
| 512 | 10 / 10240 | 8 | 7 / 6 | 2 / 8 |
| 256 | 5 / 5120 | 4 | 3 / 2 | 1 / 4 |
| 128 | 1 / 1024 | 4 | 3 / 2 | 1 / 4 |

脚本使用当前工作区的最新源码（包括未提交修改），在本次结果目录中重新独立构建四份程序，显式启用动态 Flush 并设置各组硬上限和
高低水位，采样间隔为 100ms；全部构建成功后才启动数据库。现有动态 GC 策略保持启用。
WBF 硬上限在全部阶段有效，动态 Flush 在正式 A 阶段启用。

每个 Block 为 4096 字节，各组编译时显式设置 `LIBDB_FLUSH_BLOCK_COUNT=4/2/1/1`。
该宏默认值仍为 4，允许编译参数覆盖且必须为正整数。沿用最新系统规则：只有前台写入后，
组缓冲大小**严格大于**局部阈值且动态策略允许时才尝试 Flush；忙组与任务资源等待逻辑保持现有实现。
这项局部阈值与 WBF 全局容量高低水位分别记录。

本实验所称的 WAL 开启指**保留现有 PMEM 持久化写缓冲路径**，没有新增独立 WAL。
当前代码按 KVsize 分配完整逻辑记录，但只赋值并持久化记录头与 **32 字节 value 前缀**。

每次独立测试使用全新数据库，流程为：**对应分布 load → 缓存 warmup → 对应分布 A**。
脚本名称和运行命令保留不变，当前只运行 A，不读取或要求 E 的输入文件，也不传入
`--precondition`。四种 KVsize 都按相同流程执行。

数据库的可选接口 `--precondition=FILE` 和汇总器的 E 解析能力保留，供后续恢复 E 测试。
该接口会在 warmup 后、正式 test 前执行 YCSB A trace，保留同一数据库状态；前置 trace
必须包含整千条 READ/UPDATE 请求。当前矩阵脚本及旧的 `run_ycsbe_latency_task2.sh`
均不传此参数。

| 环境变量 | 默认值 / 含义 |
| --- | --- |
| `REPEATS` | 1，正整数，每轮包含完整的 8 个组合 |
| `WORKLOAD_ROOT` | `/mnt/nvme0n1/why/ycsb_workloads_one_warm` |
| `UNIFORM_WARM_FILE`、`UNIFORM_YCSBA_FILE` | 根目录下 `uniform/warm.txt`、`uniform/ycsba.txt` |
| `ZIPFIAN_WARM_FILE`、`ZIPFIAN_YCSBA_FILE` | 根目录下 `zipfian/` 对应文件 |
| `THREADS`、`LOAD_INJECTORS`、`KV_COUNT`、`BOUNDARY_SIZE` | 16、8、100000000、1000；KV_COUNT 仅用于队列配置，不截断 trace |
| `PCI_ADDR`、`PMEM_BASE`、`LIBDB_ALLOW_DESTRUCTIVE` | `0000:5c:00.0`、`/mnt/whypmem`、1；显式设为 0 禁止实验 |
| `SPDK_ROOT`、`BUILD_JOBS` | 项目同级 `spdk` 目录、6 |
| `RESULT_BASE`、`USE_SUDO` | 项目 `results` 目录、1 |
| `TIMEOUT_SECONDS`、`KILL_AFTER_SECONDS` | 0（不限时）、30；超时分别作用于每个配置、构建或数据库进程，数据库超时包含全部阶段 |

`KV_SIZE`、`CACHE_SIZE_MB`、`TASKS_PER_THREAD`、`BINARY` 和 `LIBDB_DYNAMIC_FLUSH_MODES`
环境变量不会改变此矩阵。输入必须至少 1000 条且为千条整数倍；全部四个文件在构建前
检查并统计条数。不同 KVsize 使用相同 trace，不自动生成新数据或调整记录数。
脚本沿用设备绑定检查，不修改 CPU/NUMA 或 NVMe 驱动；实验 SSD 必须允许覆盖。
只清理本次新建的私有 PMEM 子目录，不删除父目录已有内容或 workload。

结果保存于 `results/ycsb_ae_kvsize_<时间>_<唯一标识>/`：四组构建产物、`config.json`、
每阶段命令、实时输出的完整日志、程序结果、`status.json`、`summary.csv` 和 `aggregate.csv`。
文件名例如 `01_kv1024_zipfian_A.log`。日志与结果文件中使用
`Benchmark phase: begin/end load/warmup/measured` 标记阶段。

汇总器校验 load、warmup、正式 A 三个阶段，请求数、操作类型、耗时、动态 Flush 配置及
采样统计必须一致；程序结果中 `LIBDB_FLUSH_BLOCK_COUNT` 必须存在且与本组一致，缺失、重复或不符则判定结果无效。正式吞吐量单位为 ops/s，不包含 load 和 warmup。
`summary.csv` 保留各次配置、请求数、正式吞吐量及状态；前置文件列为空、前置请求数为 0。
`config.json` 的 `FLUSH_THRESHOLDS`、明细及汇总 CSV 均记录 `flush_block_count` 和
`flush_threshold_bytes`。旧 CSV 缺少这两列时保留为空，不推测历史阈值。
`aggregate.csv` 按 KVsize、分布、workload 及局部 Flush 阈值分组，统计成功次数、均值、样本标准差、最小值和最大值。
仅成功测试参与汇总，一次成功的标准差为 0，没有成功时统计值留空。
`nvm_sampled_peak_bytes`、压力采样峰值和切换次数对应正式阶段；
`wbf_file_peak_bytes` 是**整次运行**的文件容量峰值（`wbf_file_peak_scope=whole_run`）。

输入、构建、运行、解析失败或超时、中断时停止后续组合并保留已有结果。
清理前先终止数据库；无法终止时保留 PMEM 目录。只需编写/验证脚本时不应执行真实硬件实验。

```bash
./run_ycsb_ae_kvsize.sh --help
python3 tests/test_ycsb_ae_kvsize.py -v
cmake --build build --target client_workload_test --parallel
ctest --test-dir build --output-on-failure -R '^client_workload$'
python3 tools/summarize_ycsb_ae_kvsize.py results/<本次运行目录>/summary.csv
```

以上 Python 测试使用模拟 CMake 和数据库，验证仅 A 的 8 个组合、缺少 E 文件、重复轮数、
失败退出及保留的 E 解析能力；C 测试使用真实阶段编排与动态 Flush 控制器、
模拟请求读取和执行，覆盖无前置阶段、A→E、阶段计数/采样重置、状态保留和前置失败。
这些测试均不打开真实实验设备。

## GC-heavy uniform YCSB A：最后 100M 操作

```bash
./run_ycsba_gc_heavy.sh
```

脚本在独立结果目录编译当前源码，然后在同一全新数据库中依次执行：
`warm.txt` 加载 100M KV → 现有缓存预热 → `ycsba1.txt` 前置运行 150M 操作 →
`ycsba2.txt` 测量 100M 操作。默认 trace 位于 `/home/why/LibDB/workloads_unif`。
三个文件必须分别为 100M、150M、100M 条合法请求；加载只接受 INSERT，两个 A 阶段只接受
READ/UPDATE，并按 trace 的实际读写数计算结果，不假设恰好 50M 次 UPDATE。

默认配置：1024 字节完整逻辑 KV、16 workers、8 injectors、**20GiB cache
（`CACHE_SIZE_MB=20480`，单位 MiB）**、每 worker 16 tasks、boundary size 1000。
运行一轮，保留当前源码的动态 GC、动态 Flush、WBF 容量及局部 Flush 阈值。
`BINARY`、`REPEATS` 不改变此流程。默认实验 SSD 为 `0000:5c:00.0`，PMEM 父目录为
`/mnt/whypmem`，`LIBDB_ALLOW_DESTRUCTIVE=1`；显式设为 0 禁止实验。
实验盘须允许覆盖并已绑定 SPDK 驱动。脚本不修改设备绑定，只清理本次创建的私有 PMEM 子目录。

可覆盖 `WORKLOAD_ROOT`、`WARM_FILE`、`PRECONDITION_FILE`、`TEST_FILE`、`THREADS`、
`LOAD_INJECTORS`、`KV_SIZE`、`CACHE_SIZE_MB`、`TASKS_PER_THREAD`、`BOUNDARY_SIZE`、
`PCI_ADDR`、`PMEM_BASE`、`SPDK_ROOT`、`BUILD_JOBS`、`RESULT_BASE`、`USE_SUDO`、
`TIMEOUT_SECONDS`、`KILL_AFTER_SECONDS`。`TIMEOUT_SECONDS=0` 默认不限时；非零时分别限制
configure、build、整个数据库进程（包括加载及前置阶段）。`SKIP_DRIVER_CHECK=1` 仅供模拟测试。

统计窗口在释放请求线程之前开始，全部前台请求完成后结束。读取 trace、构建和打印报告不计时。
每个 worker 通过独立原子计数器记录成功完成的 Flush、GC、Split、Merge SSD 写入字节数；
计数器在数据库生命周期内不清零，窗口采用起止快照差值。各线程、各类别依次采样，存在很小的
采样边界偏差；不暂停后台工作，也不在窗口结束强制刷空 PMEM。后台写完成按完成时间归入窗口，
因此可能包括前置工作延续到窗口中的写入，而不包括窗口结束后才完成的写入。

- 吞吐量：`100000000 / seconds`，同时输出 ops/s 和 Mops/s。
- SSD 写放大：`(flush_bytes + gc_bytes + split_bytes + merge_bytes) / (updates × KV_SIZE)`。
  分母为完整逻辑 KV 字节数（含记录头），不是 PMEM 实际持久化的 value 前缀长度。
  分子不含 PMEM 写入，也不表示 SSD 内部 NAND 写放大。
- `gc_observed` 表示最后 100M 窗口中是否有 GC 写完成；为 false 时明确报告 GC-heavy 未验证。
  该标志只证明观察到 GC，不量化 GC 压力等级。

结果位于 `results/ycsba_gc_heavy_<时间>_<唯一标识>/`，包括构建产物、构建日志、命令 JSON、
`config.json`、`uniform_A.log`、`uniform_A.result.txt`、`status.json` 和 `summary.csv`。
`SSD window: {...}` 为新增结构化窗口统计，日志和结果文件必须一致；脚本不使用旧 `WA is` 行。
汇总记录各类写入量、实际读写数、逻辑更新字节数、耗时、吞吐量、写放大及 GC 标志。
缺失/重复阶段、计数不一致、无效统计、非零退出及超时都导致失败，不打印成功指标。

已成功运行的结果可独立重新校验并输出 JSON：

```bash
python3 tools/summarize_ycsba_gc_heavy.py results/ycsba_gc_heavy_<时间>_<唯一标识>
```

无设备模拟测试：`python3 tests/test_ycsba_gc_heavy.py`。启用 `BUILD_TESTING` 后，CTest
同时包含该测试和 `ssd_window`；现有 Merge/GC/Flush 模拟 SSD 测试还会校验成功与失败写完成计数。
