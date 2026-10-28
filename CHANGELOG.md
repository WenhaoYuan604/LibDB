# 版本更新记录

## 0.2.0

本版本在 `v0.1.0` 基础上增加 WBF 文件容量硬上限，并采用提前 Flush 的高低水位控制。
解决按未回收槽位统计的软阈值无法限制实际 WBF 文件总容量的问题。

### 新增与改进

- **WBF 文件硬上限**：所有 WBF 文件容量总和默认不超过 **16GiB**，当前每文件 1GiB，
  最多创建 16 个。初始化、load、预热、正式 workload 和 recovery 全程有效，关闭动态
  Flush 也不能绕过。创建前在扩容互斥锁内检查预算；超限不调用文件创建接口，记录容量
  错误并沿用分配失败退出路径。
- **14/12GiB 高低水位**：压力占用改为“已创建 WBF 文件总容量减可立即复用空闲槽位
  容量”，包含文件开销及尚未划分槽位的预留空间。达到 14GiB 置压力标志为 1，降到
  12GiB 或以下置 0，中间保持状态。main 每 100ms 采样，扩容慢路径达到高水位时同步
  置 1，无需等待下次采样；两条路径通过同一扩容锁协调。
- **可复用空间统计**：新 segment 的槽位成功加入空闲链表后增加空闲量，取出槽位时
  扣减；记录持久化失效且成功归还链表后才增加。未完成 Flush、回收失败均不提前增加
  空闲量。保留未回收槽位占用计数作为独立诊断指标。
- **恢复容量检查**：启动 worker 前统计已有 WBF 文件实际容量；超过预算时拒绝启动，
  不删除、截断或迁移已有文件。恢复中未重建到空闲链表的槽位不计为可立即复用空间。
- **日志与实验工具**：新增文件数量、文件总容量及峰值、可复用槽位容量、压力占用及
  采样峰值、硬上限拒绝次数。汇总工具兼容旧软阈值和新硬上限日志，区分统计口径；
  即使程序在最终统计前因上限退出，也报告触及硬上限。旧日志不作为硬上限验证依据。
- 动态 Flush 实验脚本默认独立构建并依次运行 **OFF → ON**，可通过
  `LIBDB_DYNAMIC_FLUSH_MODES=on` 或 `off` 仅运行一种模式。

### 配置与兼容性

以下宏可在编译时覆盖；编译检查要求 `低水位 < 高水位 < 硬上限`。

| 配置 | 默认值 | 含义 |
| --- | --- | --- |
| `LIBDB_WBF_CAPACITY_LIMIT_BYTES` | `17179869184` | 16GiB WBF 文件总容量硬上限 |
| `LIBDB_NVM_FLUSH_HIGH_BYTES` | `15032385536` | 14GiB 压力高水位 |
| `LIBDB_NVM_FLUSH_LOW_BYTES` | `12884901888` | 12GiB 压力低水位 |
| `LIBDB_NVM_FLUSH_POLL_NS` | `100000000` | 100ms main 采样周期 |
| `LIBDB_DYNAMIC_FLUSH_ENABLED` | `1` | 正式 workload 动态 Flush 开关 |
| `LIBDB_WBF_FILE_SIZE` | `1073741824` | 单个 WBF 默认容量，支持小预算测试覆盖 |

- 旧 `LIBDB_NVM_FLUSH_LIMIT_BYTES` 不再使用；自定义构建需改用硬上限、高水位和低水位宏。
- 持久化布局不变。`nvm_used_slot_bytes` 和 `nvm_sampled_peak_bytes` 仍分别表示未回收
  槽位占用及其采样峰值；新压力占用及峰值另行记录，不能互换比较。
- 动态策略仍仅在正式 workload 生效，load 和预热使用固定局部阈值。所有模式保留单组
  **严格超过 16KiB** 才 Flush 的默认条件，不增加紧急小组 Flush。
- Flush 不缩小 WBF 文件；成功回收增加可复用槽位，降低压力占用，从而恢复延后。
  本版本不实现等待、写入背压、跨 worker 空闲槽位共享或文件缩容；达到硬上限且没有
  可用槽位时仍会退出，不保证所有负载均能持续写入。

### 单次真实设备实验

动态 Flush 开启，16GiB 硬上限、14/12GiB 水位；uniform YCSB A，16 workers、
8 injectors、1KiB KV、20000MiB cache、16 tasks/worker。正式阶段共 100,000,000
个请求，其中 UPDATE 50,011,770 个、GET 49,988,230 个。

| 指标 | 结果 |
| --- | --- |
| 吞吐量 | **865,899.50 ops/s** |
| 正式阶段耗时 | **115.486843 秒** |
| 重新计算的 WA | **1.1768×** |
| WBF 文件容量峰值 | **15GiB** |
| 硬上限拒绝次数 | **0** |

WA 按正式阶段 SSD 写入字节数（Flush、GC、Split、Merge）除以实际更新字节数计算：
`60,266,283,008 / (50,011,770 × 1024) = 1.1767988…`。
数据来自本地实验 `ycsba_dynamic_flush_20261028_145155_ju3f2f1l`，原始日志不随源码发布。
以上为单次实验观测，不代表多轮均值或所有负载的性能保证；发布验证不重跑真实设备实验。

### 已知问题

现有 `WA is ...` 输出的比值表达式错误地在除法后乘以 1024，且分母写死为 50,000,000
个 1KiB 写请求，因此不能直接使用该打印数值。**本版本保留当前代码，不修复此问题**；
分析结果应根据各阶段实际写请求字节数和 SSD 写入块数重新计算，不能将 SSD 空间放大
当作写放大。

### 发布验证

- 完整 CMake 配置和构建通过，主程序输出为 `build/test`。
- CTest：**10/10 通过**，包含动态策略开关、假时钟高低水位、WBF 并发扩容硬上限、
  拒绝后无额外文件、槽位复用、恢复检查、正常退出以及 Flush/merge/split 回归。
- Python 无设备测试：**49/49 通过**，覆盖 YCSB A/F、E 延迟、动态 Flush 默认 OFF→ON
  与单 ON 模式、帮助说明、新旧统计解析及容量拒绝报告。
- ASan/UBSan：WBF 容量（开/关）、槽位占用、动态控制（开/关）及 merge 共 **6/6 通过**。
  编译启用 `-fsanitize=address,undefined -fno-omit-frame-pointer`，运行使用
  `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`；未进行泄漏检查。
- `git diff --check` 通过。

常规回归命令：

```bash
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_ycsb*.py' -v
```

## 0.1.0

本版本将原 GitHub main 基线（`v0.0.0`，提交
`f68423ca2fae9a40b5845b20a246f428e1a15732`）之后的源码、实验脚本、测试和文档改动统一发布。
重点改进 NVM 空间利用率、group 合并、空间统计和实验复现能力。

### 新增与改进

- **动态 NVM Flush**：仅在正式 workload 启用；初始 load 和缓存预热继续使用固定局部阈值。
  main 每 100ms 读取未回收 WBF 槽位字节数；占用低于 16GiB 时延后 Flush，大于等于
  16GiB 时允许 worker Flush 严格超过局部阈值的组。默认局部阈值为 16KiB。
- **实际槽位占用统计与恢复**：按分配器实际分配的 2 的幂槽位大小统计，包含记录头和
  内部浪费；正在写 SSD 的冻结批次仍计入占用。覆盖写、删除和 Flush 完成后实际归还
  槽位才扣减。恢复启动扫描有效槽位重建计数，尚未回收的旧版本也计入。
- **延后 Flush 调度**：worker 按路由 key 去重保存候选组，压力出现时每次维护轮询最多
  处理 16 个条目，没有新写入的组也能被刷出。组忙或 I/O task 不足时重试，结构变更后
  重新定位组。停止正式 workload 后不再发起新的压力 Flush，排空已提交 I/O 和必要维护。
- **删除触发的 range-group merge**：在同一 worker 的同一初始固定键区间内选择相邻组，
  至少一侧存活键数低于 64、合计不超过 128 时可合并。存活键数按 GWB 与 SSD 的键并集
  计算。合并维护 buffer/cache、DRAM 和 PMEM 索引，迁移期间保留旧地址并保护未完成读取。
  相应完善读取、scan、删除等待、后续 split/GC 和维护重试路径。
- **动态 GC**：初始 load 完成后，每 100ms 计算 SSD 空间放大。空间放大不超过 1.30 时，
  冗余阈值放宽为 0.55（垃圾占用比例 0.55/1.55），否则使用 0.30（比例 0.30/1.30）。
  阈值判断和块筛选使用一致快照；load 前、统计无效或恢复模式跳过 load 时采用默认阈值。
- **SSD 空间统计**：正式 workload 结束且 worker 退出后，按引用表非零数据块计算占用，
  输出初始 load 字节数、SSD 使用量、空间放大及在线计数一致性。该指标不包含 WAL、
  PMEM 或缓存；缺少有效分母时输出 `N/A` 及原因。

### 修复与接口变化

- WBF 新 segment 使用 PMDK 返回的准确分配地址并清零，修复向下对齐可能覆盖对象前
  元数据的问题；修正 segment 元数据持久化长度和并发记录文件序号的时机。
- subspace 的 `buffered_size` 改为 64 位，避免延后 Flush 后大缓存发生 32 位溢出。
- 内部 `write_buffer_free()` 增加所属 `write_buffer` 参数，统一回收计数；回收失败或
  重复回收不会被当作成功释放空间。
- 完善正常退出条件：等待已提交 I/O 和必要维护，延后队列不阻塞退出，不强制刷空缓存。
- CMake 主程序目标由 `test` 改为 `libdb`，可执行文件仍为 `build/test`，避免与 CTest
  的 `test` 目标重名。新增无设备 CTest 测试和可选 ASan/UBSan 检查。

### 配置

动态 Flush 的以下宏位于 `config.h`，可通过编译参数覆盖，修改后需重新编译：

| 配置 | 默认值 | 含义 |
| --- | --- | --- |
| `LIBDB_DYNAMIC_FLUSH_ENABLED` | `1` | 设为 `0` 恢复固定局部 Flush 阈值行为 |
| `LIBDB_NVM_FLUSH_LIMIT_BYTES` | `17179869184` | 16GiB 未回收槽位占用软阈值 |
| `LIBDB_NVM_FLUSH_POLL_NS` | `100000000` | 100ms main 采样周期 |

新增 `LIBDB_SUBSPACE_MERGE_LOWER_THRESHOLD`，默认是 `LIBDB_SUBSPACE_MAX_KEYS` 的一半；
两者支持编译时覆盖。动态 GC 当前配置为 `LIBDB_GC_RELAXED_THRESHOLD=0.55`、
`LIBDB_GC_SPACE_LIMIT=1.30`、`LIBDB_GC_POLL_NS=100000000`。

日志新增 `nvm_used_slot_bytes`、`nvm_sampled_peak_bytes`、`nvm_pressure_changes` 等字段；
NVM 峰值为正式 workload 的采样峰值，并非逐次分配的瞬时峰值。

### 实验工具

- `run_ycsba_uniform_zipfian.sh`：固定三轮 Uniform/Zipfian YCSB A/F 实验，输出每轮结果
  和按分布、workload 分组的吞吐量汇总。
- `run_ycsbe_latency_task2.sh`：固定每个 worker 两个 task，进行三轮独立 Uniform YCSB E，
  提取正式阶段平均延迟、P50/P95/P99/P99.9、最大延迟及吞吐量。
- `run_ycsba_dynamic_flush.sh`：分别构建动态 Flush OFF/ON 两个二进制，依次执行一次
  fresh Uniform YCSB A，对比吞吐量及 NVM 占用统计，保存构建和运行日志。
- 新增 Python 汇总工具及无设备模拟测试，覆盖成功、输入校验、统计解析、失败、中断和
  超时路径。具体参数、运行前提和统计口径见 README。

### 发布验证

本次发布验证通过：

- 完整 CMake 配置和构建（主程序输出 `build/test`）。
- CTest：**8/8 通过**，覆盖动态 Flush 开关和阈值、WBF 分配/回收/恢复及并发计数、
  main 轮询、正常退出、SSD 空间统计、分配器以及 Flush/merge/split 集成场景。
- Python 无设备测试：**40/40 通过**，覆盖 A/F runner、E 延迟和动态 Flush OFF/ON 对比。
- `git diff --check` 通过。

复现命令：

```bash
cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_ycsb*.py' -v
```

### 兼容性与限制

- 动态 Flush 不改变持久化结构格式；16GiB 是未回收槽位的软阈值，采样和 I/O 延迟可能
  导致暂时超出。空闲槽位、文件预分配、PMDK 元数据和 NVM 索引不计入该阈值。
  WBF 文件不会因 Flush 缩小，本版本不提供文件回收、物理容量硬上限或写入背压。
- 新 segment 地址修复不负责修复已有损坏数据；恢复占用扫描遇到非空但无法解析的槽位
  会报错退出。改变影响持久化布局的编译配置仍须按 README/config.h 约定使用新数据。
- group merge 尚不提供中途崩溃恢复协议。正常完成的持久索引支持现有恢复流程；迁移中
  I/O 或持久化错误会终止进程并保留源块，不保证该状态可恢复。
- 无设备测试及内存检查不等同于真实 NVMe/PMEM 的性能验证。本次发布不宣称已完成真实
  设备 16GiB 压测，也不承诺所有工作负载都能填满该预算。
- F 的吞吐量按实际 READ/UPDATE 请求统计；跨轮 E 延迟分位数均值不等同于总体合并分位数。

## 0.0.0

原 GitHub `main` 的基线快照，标签 `v0.0.0` 指向提交
`f68423ca2fae9a40b5845b20a246f428e1a15732`
（`Exclude validation from recovery timing`）。

此版本用于保留 0.1.0 之前的代码和建立版本比较起点，仅补建标签与 Release，不修改该历史
提交。该基线包含原有 LibDB、YCSB A/C 与 recovery 实验流程。
