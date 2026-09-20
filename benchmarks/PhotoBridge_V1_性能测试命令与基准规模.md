# PhotoBridge V1 性能测试命令与基准规模记录

> 目的：只记录 V1 基线阶段实际使用的测试规模与命令，后续 V2 / V3 / V4 优化后保持相同口径复测。  
> 本文档不整理性能结果，只记录“测什么、测多大、怎么跑”。

---

## 1. 构建 V1 Benchmark

```bash
cmake --preset bench-release
cmake --build --preset bench-release -j2
```

---

## 2. 保存 V1 测试环境

建立结果目录：

```bash
mkdir -p benchmark-results/v1-baseline
```

保存环境信息：

```bash
{
  echo "=== Git ==="
  git rev-parse HEAD
  git status --short

  echo "=== System ==="
  uname -a

  echo "=== CPU ==="
  lscpu

  echo "=== Memory ==="
  free -h

  echo "=== Disk ==="
  lsblk
  df -T

  echo "=== Compiler ==="
  g++ --version

  echo "=== CMake ==="
  cmake --version
} > benchmark-results/v1-baseline/environment.txt
```

---

# 3. Microbenchmark

## 3.1 基准规模

一次命令同时测试三组 workload：

```text
scanner_manifest
copy_hash_fdatasync
sqlite_batch_insert
```

固定规模：

```text
重复次数：30

Scanner：
100000 files

Copy：
67108864 bytes
= 64 MiB

SQLite：
5000 commands
```

## 3.2 命令

```bash
./build/bench-release/photobridge_bench \
  --repetitions 30 \
  --scanner-files 100000 \
  --copy-bytes 67108864 \
  --sqlite-commands 5000 \
  --output benchmark-results/v1-baseline/micro.json
```

输出文件：

```text
benchmark-results/v1-baseline/micro.json
```

---

# 4. 小规模 E2E Benchmark

## 4.1 基准规模

完整执行：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

固定规模：

```text
重复次数：30

文件数量：
4 files

单文件大小：
65536 bytes
= 64 KiB

总输入数据：
4 × 64 KiB
= 256 KiB
```

这一组主要用于观察：

```text
完整主链固定开销
SQLite / 状态切换
文件创建
fsync
规划与验证开销
```

## 4.2 命令

```bash
./build/bench-release/photobridge_bench \
  --workload e2e \
  --repetitions 30 \
  --e2e-files 4 \
  --e2e-file-bytes 65536 \
  --output benchmark-results/v1-baseline/e2e.json
```

输出文件：

```text
benchmark-results/v1-baseline/e2e.json
```

---

# 5. 大规模 E2E Benchmark

## 5.1 基准规模

固定 workload：

```text
e2e-large
```

实际数据规模：

```text
64 files
×
16 MiB / file
=
1 GiB
```

正式记录前先单独预热 1 次。

## 5.2 预热命令

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1
```

预热结果不保存为正式基线。

## 5.3 正式命令

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --output benchmark-results/v1-baseline/e2e-large.json
```

输出文件：

```text
benchmark-results/v1-baseline/e2e-large.json
```

后续 V2 / V3 / V4 复测时保持：

```text
1 次预热
+
10 次正式记录
```

---

# 6. perf 环境准备

安装：

```bash
sudo apt update
sudo apt install linux-tools-common linux-tools-$(uname -r)
```

如果对应内核工具包不可用：

```bash
sudo apt install linux-tools-generic
```

检查：

```bash
perf --version
```

如果出现：

```text
perf_event_paranoid = 4
```

临时降低限制：

```bash
sudo sysctl -w kernel.perf_event_paranoid=1
```

检查：

```bash
cat /proc/sys/kernel/perf_event_paranoid
```

---

# 7. Copy perf stat

## 7.1 基准规模

```text
workload：
copy

重复次数：
10

单次 Copy：
64 MiB
```

## 7.2 命令

先建立目录：

```bash
mkdir -p benchmark-results/v1-baseline/perf
```

执行：

```bash
perf stat \
  -o benchmark-results/v1-baseline/perf/copy-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --repetitions 10 \
  --copy-bytes 67108864
```

输出：

```text
benchmark-results/v1-baseline/perf/copy-stat.txt
```

---

# 8. Copy perf record

## 8.1 整体 CPU 采样

基准规模：

```text
workload：
copy

重复次数：
10

单次 Copy：
64 MiB
```

命令：

```bash
perf record -e cpu-clock -g \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --repetitions 10 \
  --copy-bytes 67108864
```

保存原始 perf.data：

```bash
mv perf.data benchmark-results/v1-baseline/perf/copy-record.data
```

导出文本报告：

```bash
perf report --stdio \
  -i benchmark-results/v1-baseline/perf/copy-record.data \
  > benchmark-results/v1-baseline/perf/copy-report.txt
```

---

## 8.2 只看用户态热点

命令：

```bash
perf record -e cpu-clock:u -g \
  -o benchmark-results/v1-baseline/perf/copy-user.data \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --repetitions 10 \
  --copy-bytes 67108864
```

导出：

```bash
perf report --stdio \
  -i benchmark-results/v1-baseline/perf/copy-user.data \
  > benchmark-results/v1-baseline/perf/copy-user-report.txt
```

查看前 50 行：

```bash
head -50 benchmark-results/v1-baseline/perf/copy-user-report.txt
```

---

# 9. SQLite strace

## 9.1 基准规模

```text
workload：
sqlite

重复次数：
10

每次：
5000 commands

总命令数：
50000 commands
```

## 9.2 命令

先建立目录：

```bash
mkdir -p benchmark-results/v1-baseline/strace
```

执行：

```bash
strace -c -f \
  -o benchmark-results/v1-baseline/strace/sqlite-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload sqlite \
  --repetitions 10 \
  --sqlite-commands 5000
```

输出：

```text
benchmark-results/v1-baseline/strace/sqlite-summary.txt
```

注意：

```text
strace 会显著影响运行时间
→ strace 下的 wall time 不作为正式 benchmark 性能成绩
→ 只用于分析系统调用次数和耗时分布
```

---

# 10. Copy strace

## 10.1 基准规模

```text
workload：
copy

重复次数：
10

单次 Copy：
64 MiB
```

## 10.2 命令

```bash
strace -c -f \
  -o benchmark-results/v1-baseline/strace/copy-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --repetitions 10 \
  --copy-bytes 67108864
```

输出：

```text
benchmark-results/v1-baseline/strace/copy-summary.txt
```

主要用于后续比较：

```text
read
write
fdatasync
fsync
openat
close
```

尤其用于后续：

```text
传统 read/write
vs
io_uring
```

的系统调用层面对比。

---

# 11. SQLite perf stat

## 11.1 基准规模

```text
workload：
sqlite

重复次数：
10

每次：
5000 commands
```

## 11.2 命令

```bash
perf stat \
  -o benchmark-results/v1-baseline/perf/sqlite-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload sqlite \
  --repetitions 10 \
  --sqlite-commands 5000
```

输出：

```text
benchmark-results/v1-baseline/perf/sqlite-stat.txt
```

---

# 12. E2E-large perf stat

## 12.1 基准规模

```text
workload：
e2e-large

重复次数：
3

单次数据量：
1 GiB
```

这里不是正式 benchmark，只是补充完整链路 CPU / 系统行为分析，因此使用 3 次。

## 12.2 命令

```bash
perf stat \
  -o benchmark-results/v1-baseline/perf/e2e-large-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3
```

输出：

```text
benchmark-results/v1-baseline/perf/e2e-large-stat.txt
```

---

# 13. V1 固定测试口径总表

| 测试 | Workload | 单次规模 | 重复次数 |
|---|---|---:|---:|
| Micro Scanner | scanner_manifest | 100000 files | 30 |
| Micro Copy | copy_hash_fdatasync | 64 MiB | 30 |
| Micro SQLite | sqlite_batch_insert | 5000 commands | 30 |
| Small E2E | e2e | 4 × 64 KiB = 256 KiB | 30 |
| Large E2E | e2e-large | 1 GiB | 10 |
| Copy perf stat | copy | 64 MiB | 10 |
| Copy perf record | copy | 64 MiB | 10 |
| Copy perf user | copy | 64 MiB | 10 |
| SQLite strace | sqlite | 5000 commands | 10 |
| Copy strace | copy | 64 MiB | 10 |
| SQLite perf stat | sqlite | 5000 commands | 10 |
| E2E-large perf stat | e2e-large | 1 GiB | 3 |

---

# 14. 后续优化版本统一规则

后续 V2 / V3 / V4 测试时：

```text
不随意改 benchmark 数据规模
不随意改 repetitions
不随意改构建模式
不随意改 VM CPU / RAM
不随意改文件系统
```

核心原则：

```text
V1 和优化版本
→ 使用相同 workload
→ 使用相同数据规模
→ 使用相同重复次数
→ 使用相同 Release 构建
→ 使用相同机器环境
```

这样最终对比结果才有效。
