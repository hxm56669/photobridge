# PhotoBridge benchmark baseline

本基线由 Linux 虚拟机上的 Release 构建产生，命令为：

```bash
./build/bench-release/photobridge_bench \
  --repetitions 5 \
  --scanner-files 100000 \
  --copy-bytes 67108864 \
  --sqlite-commands 5000 \
  --output /tmp/photobridge-benchmark-release.json
```

数据目录为 `/tmp/photobridge_bench_data`。每个 workload 的计时区间不包含 fixture 创建；scanner 包含扫描、分类、Manifest 批量写入和 Freeze，copy 包含 CopyAndHash、BLAKE3 与目标 `fdatasync`，SQLite 包含固定数量 INSERT 命令提交到单写者队列并等待全部 ACK。

## Release baseline

| Workload | 配置 | wall mean | wall P95 | wall P99 | 吞吐 mean | CPU mean | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| scanner_manifest | 100,000 files | 2007.64 ms | 2031.86 ms | 2031.86 ms | 49,814 files/s | 81.03% | 13,084 KiB |
| copy_hash_fdatasync | 64 MiB | 278.69 ms | 292.61 ms | 292.61 ms | 241.05 MB/s | 99.08% | 13,816 KiB |
| sqlite_writer_ack | 5,000 commands | 3584.04 ms | 3841.93 ms | 3841.93 ms | 1,396.8 commands/s | 46.69% | 14,320 KiB |

本次 5 次重复样本的 P99 与最大样本相同；正式性能比较应使用相同 fixture、参数、构建类型和至少 10 次重复。吞吐率的 P95/P99 也写入 JSON，但应注意它们是逐样本吞吐率的分位数，不是延迟分位数的倒数。

## Resource interpretation

- `/proc/self/io` 的 `rchar/wchar` 和 read/write syscall 计数会记录逻辑 I/O；本次三个 workload 的 `read_bytes` 均为 0，说明数据主要命中 Linux page cache，不代表没有读取。
- 本次累计内核写入字节分别为 scanner `391,249,920`、copy `335,544,320`、SQLite `206,540,800`；copy 的写入量与 5 次 × 64 MiB 一致。
- JSON 报告保留每个样本的 wall/CPU、CPU 利用率、RSS、逻辑/内核 I/O 和 syscall 计数，便于后续对比。

## Local end-to-end baseline

使用独立 workspace/target、4 个 64 KiB 文件，执行 30 次完整 CLI 链路：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e \
  --repetitions 30 \
  --e2e-files 4 \
  --e2e-file-bytes 65536 \
  --output /tmp/photobridge-e2e-local-30.json
```

| Workload | wall mean | wall P95 | wall P99 | 吞吐 mean | CPU mean | CPU utilization | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| e2e_local_pipeline | 91.94 ms | 122.78 ms | 129.27 ms | 11.13 pipelines/s | 45.11 ms | 49.08% | 7,956 KiB |

每条 pipeline 的数据量为 4 × 64 KiB；计时包含 `scan → plan → migrate × 4 → resume × 4 → verify`，不包含源 fixture 创建和 workspace 初始化。30 次均返回成功，最终 verify 均为 `IDENTICAL`。原始 JSON 保存在 Linux VM `/tmp/photobridge-e2e-local-30.json`。

## Large Local E2E 1 GiB profile

正式大压测使用独立 workload `e2e-large`，固定为 64 个 16 MiB 文件，即每条 pipeline 精确 1 GiB（1,073,741,824 bytes），重复 30 次：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 30 \
  --output /tmp/photobridge-e2e-large-local.json
```

| Workload | wall mean | wall P95 | wall P99 | 吞吐 | 等效数据吞吐 | CPU | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| e2e_large_local_pipeline | 10988.20 ms | 11138.38 ms | 16285.56 ms | 0.0915 pipelines/s | 93.19 MiB/s（97.72 MB/s） | 92.60% | 8,256 KiB |

30/30 次成功且 verify 均为 `IDENTICAL`。P99 受第 1 个样本的 16.29 秒长尾影响；原始 JSON 保存在 Linux VM `/tmp/photobridge-e2e-large-local.json`。
