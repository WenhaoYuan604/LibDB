# LibDB

LibDB 是一个面向分层存储的研究型键值数据库原型。系统使用 DRAM 与持久内存维护缓存、写缓冲和混合索引，并通过 SPDK 直接访问 NVMe SSD；代码包含 subspace split、垃圾回收、scan 和恢复路径。

本仓库只保留当前构建使用的架构代码和可复用测试脚本，不包含实验结果或大型 workload。

> [!CAUTION]
> LibDB 会直接访问由 SPDK 接管的 NVMe 设备，并在指定的 PMEM 目录中创建或更新持久化文件。运行测试可能覆盖目标设备上的数据。只能使用专门用于实验、允许清空的 NVMe 和持久内存设备，绝不能使用系统盘或含有重要数据的设备。

## Architecture

```text
YCSB trace
    |
client -> per-thread request/result rings -> server workers
                                             |
                  +--------------------------+--------------------------+
                  |                          |                          |
               KV cache                write buffer              hybrid index
                                                                       |
                                                        DRAM index + NVM index
                                                                       |
                                                        SPDK -> NVMe SSD
```

主要模块：

| 模块 | 文件 | 作用 |
| --- | --- | --- |
| 配置 | `config.h`, `config.c` | 编译期布局参数和运行时设备/路径配置 |
| 程序入口 | `main.c` | 参数解析、配置校验、初始化和 worker 启动 |
| 请求执行 | `client.c`, `server.c`, `task.c` | workload 读取、请求提交与完成轮询 |
| 混合索引 | `index.c`, `dram_index.c`, `nvm_index.c` | DRAM/NVM 分层索引 |
| 缓冲与缓存 | `kvbuffer.c`, `kvcache.c`, `write_buffer_index.c` | 写缓冲、KV cache 和缓冲索引 |
| 数据组织 | `subspace.c`, `leaf_node_file.c`, `merge_flush.c` | subspace、叶节点文件和 flush/merge |
| 存储管理 | `spdk_struct.c`, `nvme_pipe.c`, `pmdk.c` | NVMe/SPDK 与持久内存访问 |
| 维护路径 | `garbadge_c.c`, `recovery.c` | GC、空间回收和恢复 |

## Configuration

配置分为运行时配置和编译期布局配置。设备地址、目录和 workload 不再硬编码在源码中。

### Runtime configuration

以下参数可在每次运行时传入。其中 PCI 地址、PMEM 目录和两个 workload 文件为必填项。

| 命令行参数 | 程序环境变量 | `run_example.sh` 变量 | 说明 |
| --- | --- | --- | --- |
| `--pci-addr=DDDD:BB:DD.F` | `LIBDB_PCI_ADDR` | `PCI_ADDR` | SPDK 使用的 NVMe 控制器 PCI BDF |
| `--pmem-dir=PATH` | `LIBDB_PMEM_DIR` | `PMEM_DIR` | 已存在且可写的 PMEM/DAX 目录 |
| `--warm=FILE` | `LIBDB_WARM_FILE` | `WARM_FILE` 或 `WORKLOAD_DIR/warm.txt` | load/warmup trace |
| `--test=FILE` | `LIBDB_TEST_FILE` | `TEST_FILE` 或 `WORKLOAD_DIR/ycsbc.txt` | test trace |
| `--thread=N` | — | `THREADS`，默认 `16` | server worker 数量 |
| `--kvsize=BYTES` | — | `KV_SIZE`，默认 `1024` | 运行时逻辑 record 总大小，范围 52～32768；逻辑 value 大小为 `BYTES - 20` |
| `--kvs=N` | — | `KV_COUNT`，默认 `100000000` | 队列和实验配置使用的 KV 数量 |
| `--cachesize=MB` | — | `CACHE_SIZE_MB`，默认 `20000` | KV cache 容量（MiB） |
| `--load-injectors=N` | — | `LOAD_INJECTORS`，默认 `8` | client injector 数量 |
| `--tasks-per-thread=N` | — | `TASKS_PER_THREAD`，默认 `16` | 每个 worker 的并行任务数 |
| `--boundary-size=N` | — | `BOUNDARY_SIZE`，默认 `1000` | 初始 key boundary 样本数 |
| `--output=FILE` | — | 自动生成 | 追加写入统计结果 |
| `--recovery` | — | `RECOVERY=1` | 从持久化数据恢复 |
| `--recovery-dupcheck=1` | — | `RECOVERY_DUPCHECK=1` | 恢复后执行全量重复键检查 |

命令行参数优先于对应的 `LIBDB_*` 环境变量。程序会在初始化 SPDK 前检查 PCI 地址格式、PMEM 目录和 workload 可读性。

### Persistent KV record format

全局原子分配器以实时时钟纳秒值初始化。每个 server worker 每次从全局分配器
原子申请 65,536 个连续的 64-bit version，之后在该区间内使用线程私有变量递增；
只有区间耗尽时才再次访问全局原子。这样进程内的 version 全局唯一，并且同一个 key
的 version 严格递增。记录按以下顺序持久化：

```text
uint16 keysize | key_type key | uint64 version | uint16 valuesize
| 32-byte valid value prefix | runtime-sized invalid tail
```

当前 `key_type` 为 8 字节，因此记录头共 20 字节。逻辑 record 大小由
`--kvsize` 在启动时指定，`valuesize = kvsize - 20`。为模拟 PRISM 的固定宽度
value，只有 value 的前32字节被视为有效；其余 `kvsize - 52` 字节不写入、不补零。
读取接口仍返回原始逻辑 `valuesize`，但调用方不得使用第33字节之后的内容。例如
`--kvsize=1024` 对应1004字节逻辑value和972字节无效尾部。get、scan、GC和recovery
均通过同一解码器读取该格式并校验记录中的key。
recovery 重建 write-buffer index 时会比较同一个 key 的 version，只保留 version 最大的记录；恢复日志中的 `skipped_older` 给出被跳过的旧版本/同版本条目数。

此格式与旧的 12 字节记录头不兼容；首次使用新版本前必须清理旧 PMEM write-buffer 数据和旧 NVMe 数据。

### PRISM-style partial PMEM persistence

PMEM write-buffer 按运行时 `--kvsize` 分配和推进，索引、flush和recovery都保留
该逻辑record size。写入时分别赋值20字节header，并用一次结构体赋值写入32字节
value前缀，随后只对这52字节调用 `pmem_persist`。变长无效tail不会被写入或补零。

查看完整参数：

```bash
./build/test --help
```

PMEM 文件会自动创建在指定目录中，例如：

```text
<pmem-dir>/write_buffer_file0
<pmem-dir>/leaf_node_file0
<pmem-dir>/leaf_node_file1
...
```

### NVM index slot format

每个 leaf slot 固定为 256 字节，持久化布局为：

```text
uint64 group_boundary | slot_element entries[10] | uint8 reserved[8]
```

`group_boundary` 是该 slot 所属 group/subspace 在 DRAM index 中的边界；同一个 group 的所有 slots 写入相同 boundary。
`slot_element` 由 8-byte key 和 16-byte SSD address 组成，因此boundary 后的 248 字节可容纳 10 个 entries，并保留 8 字节 padding。运行时的 flush、split、search、scan、GC 和 delete 均按此布局访问。

recovery 会从 slot 首部的 `group_boundary` 和后续 entries 重建 group，并处理
split marker 与未完成的左右覆盖关系。旧 entry-array-only leaf slot 数据与此布局不兼容。

### Compile-time layout configuration

所有影响持久化布局、固定数组或底层 I/O 几何的参数都集中在 [`config.h`](config.h)。常用参数包括：

| 宏 | 默认值 | 含义 |
| --- | ---: | --- |
| `LIBDB_BLOCK_SIZE` | `4096` | NVMe 数据块大小 |
| `LIBDB_LNF_SLOT_SIZE` | `256` | leaf-node slot 大小 |
| `LIBDB_FREE_SPACE_GROUP_BLOCKS` | `1024` | free-space group/segment 包含的块数 |
| `LIBDB_FLUSH_BLOCK_COUNT` | `4` | flush 触发阈值；实际写入块数按有效数据动态计算 |
| `LIBDB_SUBSPACE_MAX_KEYS` | `128` | 单个 subspace 的最大 key 数 |
| `LIBDB_LNF_SLOT_ENTRY_CAPACITY` | 派生值（当前为 `10`） | 扣除 8-byte group boundary 后可容纳的 `key + SSD address` entry 数 |
| `LIBDB_SUBSPACE_MAX_SLOTS` | 派生值（当前为 `26`） | `ceil((2 * MAX_KEYS) / slot entry capacity)` |
| `LIBDB_SUBSPACE_REDUNDANT_THRESHOLD` | `0.3` | subspace GC 冗余阈值 |
| `LIBDB_WBF_FILE_SIZE` | `1 GiB` | write-buffer PMEM pool 文件大小 |
| `LIBDB_PMEM_VALUE_PREFIX_SIZE` | `32` | 实际赋值并持久化的有效 value 前缀大小 |
| `LIBDB_WBF_SEGMENT_SIZE` | `4 MiB` | write-buffer segment 大小 |
| `LIBDB_WBF_MIN_SLOT_SIZE` | `32` | write-buffer 最小分配粒度 |
| `LIBDB_WBF_MAX_RECORD_SIZE` | `32768` | 受uint16_t分配粒度元数据约束的最大逻辑record |
| `LIBDB_LNF_FILE_SIZE` | `512 MiB` | 每个 leaf-node PMDK pool 的大小；不足时自动创建下一个 pool |
| `LIBDB_LNF_SEGMENT_SIZE` | `4 MiB` | leaf-node segment 的可用数据大小 |
| `LIBDB_MAX_NVME_QPAIRS` | `31` | 单 namespace 最大 I/O qpair 数 |
| `LIBDB_SPDK_IO_QUEUE_SIZE` | `1024` | SPDK I/O queue 深度 |

> [!WARNING]
> 修改 slot、group、subspace、PMEM 文件/segment 等布局参数后，必须完整重新编译，并使用全新的 PMEM/NVMe 实验数据。不同布局参数生成的持久化数据不能保证兼容，尤其不能直接用于 recovery。

源码中的旧名称（例如拼写保留的 `Flush_Blcok_Num`、`alloc_grain`）只作为兼容别名存在，定义也统一位于 `config.h`。新代码应使用 `LIBDB_*` 名称。

### Leaf-slot updates

flush、GC、split 和 delete 中的 leaf-slot 更新会先在 DRAM 中构造完整的
256-byte 后镜像，然后在该 slot 所属的 PMDK pool 内执行一个独立事务。
事务对整个 slot 调用 `pmemobj_tx_add_range_direct()` 建立 undo snapshot，
因此 crash 后单个 slot 只会是完整旧版本或完整新版本，不会留下撕裂内容。
SSD 写入仍然必须先完成，之后才提交 slot 事务；全部 slots 更新完成后
才释放旧 SSD 块。

> [!IMPORTANT]
> 事务粒度仍是单个 slot。flush、GC 和 delete 在一组 slots 提交过程中崩溃时，
> 持久化 NVM index 仍可能包含部分新 slot 和部分旧 slot。split 的正常更新路径
> 额外采用可恢复顺序：若分界键位于 slot `s` 内部，先将 `s` 的 lower fence
> 原子标记为 `UINT64_MAX`，再按 smallest key 递增更新其余旧 slots，持久化新的
> 右侧 slot，最后原子裁剪 `s` 并恢复其原 lower fence；若分界键恰好是 `s` 的
> smallest key，同样先标记 `s`，但不创建新 slot，并在最后将 `s` 的
> lower-fence 原子更新为右孩子的 fence。recovery 会根据 marker、左右 group
> 的覆盖/重叠关系回滚未完成的左右索引，或向前完成内部/对齐切分；对齐切分
> 完成态只更新 `s` 的 lower fence，不移动或删除其中的 entries。

每个 subspace 最多保留一个在途 flush。提交 flush 前会按当前 NVM index 和
active write buffer 精确计算合并后的 slot 数；若会超过
`LIBDB_SUBSPACE_MAX_SLOTS`，则先启动 split，并由现有 split 逻辑拆分 write
buffer 与 KV cache。leaf slots 使用连续编号的 512 MiB
`leaf_node_file0`、`leaf_node_file1` …… pool；当前 pool 无法再分配一个完整
4 MiB segment 时会自动创建下一个文件，recovery 会按编号打开所有连续 pool。

此布局与之前的单个 8 GiB `leaf_node_file0` 不兼容。更新代码后必须删除旧的
LibDB-clean PMEM 数据并从 fresh 模式开始，不能直接对 8 GiB 布局执行 recovery。


## Requirements

LibDB 当前面向 Linux 实验环境，需要：

- C 编译器、CMake 3.13+ 和 `pkg-config`
- 已编译的 [SPDK](https://spdk.io/)，包含 `spdk_nvme`、`spdk_env_dpdk` 和 `spdk_syslibs`
- PMDK：`libpmem` 与 `libpmemobj`
- `libnuma`、POSIX threads、UUID、realtime 和 math 库
- 使用与 `LIBDB_BLOCK_SIZE` 一致的实验 NVMe SSD（默认 4 KiB）
- 已创建并挂载的 PMEM/DAX 文件系统
- 为 SPDK 配置的 hugepages，以及足够的设备访问权限

仓库不会自动格式化或挂载持久内存。

## Build

先构建 SPDK，然后将 `SPDK_ROOT` 指向其源码树：

```bash
export SPDK_ROOT=/path/to/spdk

cmake -S . -B build -DSPDK_ROOT="$SPDK_ROOT"
cmake --build build --parallel
```

默认也会尝试使用与 LibDB 同级的 `../spdk`。成功后可执行文件为 `build/test`。

修改 `config.h` 后应重新执行构建；为避免旧对象残留，布局变更时建议使用新的构建目录。

## Workload format

LibDB 使用精简的 YCSB 文本 trace：

```text
I user1 value
U user1 value
R user1
S user1 10
```

- `I`：insert
- `U`：update
- `R`：read
- `S`：scan，最后一个字段为扫描长度

解析器会把请求数向下取整到 1000 的整数倍，因此 workload 应至少包含 1000 条记录，最好直接生成 1000 的整数倍。

### Generate YCSB A/C/E workloads

仓库不直接保存数 GB 的完整 trace，而是在
[`artifact/ycsb`](artifact/ycsb/README.md) 中提供生成所需的固定 YCSB-tracegen
commit、A/C/E 配置、scan trace 补丁和安全的一键脚本。默认生成：

```text
warm.txt   100M inserts
ycsba.txt  100M operations, 50% read / 50% update
ycsbc.txt  100M reads
ycsbe.txt   20M operations, 95% scan / 5% insert
```

查看生成器的环境、磁盘空间和快速测试说明：

```bash
./artifact/ycsb/generate_workloads.sh --help
```

运行 1 KiB、1000 KV 的 smoke test：

```bash
LIBDB_ALLOW_DESTRUCTIVE=1 \
PCI_ADDR=0000:5c:00.0 \
PMEM_BASE=/mnt/pmem0/libdb \
./run_smoke_1k.sh
```

该脚本会自动生成 `/tmp/libdb-clean-smoke-workload` workload，并在测试前后只清理
`PMEM_DIR`。正常结束、失败、超时或 Ctrl-C 都会执行清理。运行前必须确认
PCI 地址指向专用可丢弃实验盘；可通过 `PCI_ADDR`、`PMEM_BASE` 和其他环境变量
覆盖默认值，详见 `./run_smoke_1k.sh --help`。

## Run the example

查看脚本帮助：

```bash
./run_example.sh --help
```

执行一次测试：

```bash
WORKLOAD_DIR=/path/to/workloads \
SPDK_ROOT=/path/to/spdk \
PCI_ADDR=0000:5c:00.0 \
PMEM_DIR=/mnt/pmem0 \
LIBDB_ALLOW_DESTRUCTIVE=1 \
./run_example.sh
```

`WORKLOAD_DIR` 默认需要包含：

```text
/path/to/workloads/warm.txt
/path/to/workloads/ycsbc.txt
```

脚本会把路径通过 `--warm` 和 `--test` 直接传给程序，不会复制 workload 或修改源码。日志与统计结果默认写入 `results/`。

如果设备尚未绑定给 SPDK，可以显式调用 SPDK setup：

```bash
WORKLOAD_DIR=/path/to/workloads \
SPDK_ROOT=/path/to/spdk \
PCI_ADDR=0000:5c:00.0 \
PMEM_DIR=/mnt/pmem0 \
PREPARE_SPDK=1 \
LIBDB_ALLOW_DESTRUCTIVE=1 \
./run_example.sh
```

`PREPARE_SPDK=1` 只负责 hugepages/SPDK 设备绑定，不会格式化或挂载持久内存。请先确认 PCI 地址对应的是专用实验盘。

### Direct invocation

也可以不使用示例脚本：

```bash
mkdir -p results
sudo ./build/test \
  --thread=16 \
  --kvsize=1024 \
  --kvs=100000000 \
  --cachesize=20000 \
  --load-injectors=8 \
  --tasks-per-thread=16 \
  --boundary-size=1000 \
  --pci-addr=0000:5c:00.0 \
  --pmem-dir=/mnt/pmem0 \
  --warm=/path/to/workloads/warm.txt \
  --test=/path/to/workloads/ycsbc.txt \
  --output=results/result.txt
```

### Heavy YCSB A/C + recovery test

仓库提供了一个完整 heavy 测试脚本。它默认使用
`workload/{warm,ycsba,ycsbc}.txt`，依次运行独立的 YCSB A、独立的
YCSB C，以及基于 C 阶段持久化状态的 recovery + YCSB C 验证：

```bash
sudo env \
  LIBDB_ALLOW_DESTRUCTIVE=1 \
  WORKLOAD_DIR=/path/to/workloads \
  PCI_ADDR=0000:5c:00.0 \
  PMEM_BASE=/mnt/pmem0/libdb \
  ./run_heavy_ac_recovery.sh
```

默认参数为 16 个 server workers、8 个 load injectors、20,000 MiB cache、
1,024-byte KV、100,000,000 个 KV、每 worker 16 个并行任务。恢复阶段默认
启用全量重复键检查。脚本只清理
`<PMEM_BASE>/libdb_clean_heavy_ac`，A 和 C 均从全新 PMEM
状态开始；C 完成后不会清理，直到 recovery 完成或脚本退出。日志和结果写入
`results/heavy_ac_<timestamp>/`。

heavy 阶段默认没有时间限制。如需为每个阶段设置超时：

```bash
sudo env \
  LIBDB_ALLOW_DESTRUCTIVE=1 \
  WORKLOAD_DIR=/path/to/workloads \
  PCI_ADDR=0000:5c:00.0 \
  PMEM_BASE=/mnt/pmem0/libdb \
  TIMEOUT_SECONDS=21600 \
  ./run_heavy_ac_recovery.sh
```

可以通过 `./run_heavy_ac_recovery.sh --help` 查看路径和参数覆盖方式。

## Recovery

恢复时必须使用与写入数据时完全相同的 `config.h` 布局参数、NVMe 设备和 PMEM 目录：

```bash
WORKLOAD_DIR=/path/to/workloads \
SPDK_ROOT=/path/to/spdk \
PCI_ADDR=0000:5c:00.0 \
PMEM_DIR=/mnt/pmem0 \
RECOVERY=1 \
RECOVERY_DUPCHECK=1 \
LIBDB_ALLOW_DESTRUCTIVE=1 \
./run_example.sh
```

## Repository layout

```text
.
├── CMakeLists.txt
├── README.md
├── config.h / config.c
├── artifact/ycsb/              # pinned YCSB A/C/E trace generator
├── run_example.sh
├── main.c / base.c / base.h
├── client.c / server.c / task.c
├── index.c / dram_index.c / nvm_index.c
├── kvbuffer.c / kvcache.c / write_buffer_index.c
├── subspace.c / leaf_node_file.c / merge_flush.c
├── spdk_struct.c / nvme_pipe.c / pmdk.c
└── garbadge_c.c / recovery.c
```

## Notes

- 这是研究原型，不应直接用于生产环境。
- 运行参数和编译期布局参数必须与生成 workload、写入持久化数据时的配置保持一致。
- 仓库目前未附带许可证，因此默认保留全部权利；如需允许第三方使用、修改或再分发，应再选择并添加合适的 `LICENSE`。
