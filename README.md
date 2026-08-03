# LibDB

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
