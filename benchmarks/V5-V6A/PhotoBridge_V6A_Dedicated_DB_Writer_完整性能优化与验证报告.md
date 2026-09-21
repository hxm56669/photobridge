# PhotoBridge V6A Dedicated DB Writer 完整性能优化与验证报告

> 项目：PhotoBridge  
> 阶段：V6A  
> 优化主题：SQLite 写路径收敛 / Dedicated DB Writer  
> 测试日期：2026-09-21  
> 测试提交：`8e3a1561d3f3edd1c781c37ae3b64771e090aeb3`  
> 核心目标：解决 Multi-worker 场景下多个 SQLite Connection 竞争单 Writer Lock，减少 `SQLITE_BUSY → busy handler → clock_nanosleep` 退避等待。  
> 核心原则：**先减少“谁在写”，再在 V6B 讨论“写多少次”。**

---

# 1. 结论先行

V6A 的核心优化目标已经完成，并且有完整证据链支撑。

优化前 V5 的问题不是猜测，而是已经通过 `strace -f -k` 定位：

```text
多个 Worker / Producer
→ 多个 SQLite Connection
→ 同时执行 BEGIN IMMEDIATE
→ 竞争 SQLite 唯一 Writer Lock
→ sqliteDefaultBusyCallback
→ unixSleep
→ clock_nanosleep
→ 睡眠后再次重试
```

V6A 改为：

```text
Producer / Worker
→ RuntimeDbWriter::Enqueue()
→ Command Queue
→ 单一 RuntimeDbWriter Thread
→ 单一 SQLite Writer Connection
→ TaskRuntimeRepository
→ COMMIT 后再完成 ACK / future
```

最终验证结果：

```text
V5：
clock_nanosleep 在 E2E strace 中约占 38%
并能抓到 sqliteDefaultBusyCallback 调用栈

V6A：
clock_nanosleep = 0
sqliteDefaultBusyCallback = 0
unixSleep = 0

F_SETLKW = 0
failed fcntl = 0

futex：
主要来自 RuntimeDbWriter 队列 condition_variable 的正常 WAIT / WAKE
无 ETIMEDOUT

CPU 热点：
blake3_hash_many_avx2 = 85.55%
sqlite3VdbeExec = 0.05%

正确性：
Debug 228 / 228 passed
Sanitizer 228 / 228 passed
```

4 Worker、1 GiB 正式 E2E 对比：

| 指标 | V5 Multi-Writer | V6A Dedicated Writer | 变化 |
|---|---:|---:|---:|
| Mean wall | 8.568 s | **8.380 s** | **-2.20%** |
| Median wall | 8.577 s | **8.390 s** | **-2.17%** |
| P95 / P99 | 8.767 s | **8.621 s** | **-1.67%** |
| 吞吐 | 119.51 MiB/s | **122.28 MiB/s** | **+2.32%** |
| CPU utilization | 144.35% | 176.35% | +22.17% 相对增长 |
| Peak RSS | 13.14 MiB | 29.36 MiB | 增加约 16.2 MiB |

因此 V6A 的价值不是“E2E 暴涨”，而是：

> **把 V5 已经被 profiling 证明存在的 SQLite 内部多 Writer 竞争，收敛成应用层可控的单 Writer 队列；SQLite busy sleep 在本次 V6A benchmark 中完全没有再被观察到。**

E2E 只提升约 2.2%，说明瓶颈已经继续迁移到：

```text
BLAKE3 SIMD Hash
+
fdatasync / fsync
+
Worker ↔ Dedicated Writer 正常同步成本
```

这正是一次完整的性能工程闭环：

```text
Benchmark
→ 找瓶颈
→ 设计单变量优化
→ 正确性回归
→ 再 Benchmark
→ 再 Profiling
→ 证明瓶颈迁移
```

---

# 2. V6A 为什么要做

## 2.1 V5 已经优化过数据面

V5 完成 `io_uring` 后，局部 Copy + Hash 已经明显改善。

此前结论：

```text
64 MiB Copy + Hash Microbenchmark
QD4 相比同步吞吐约 +26.5%

但 1 GiB / 4 Worker 完整 E2E
吞吐只提升约 +2.5%
```

这说明：

```text
Copy 数据面已经变快
↓
但完整 scan → plan → migrate → resume → verify
没有同步获得同等幅度提升
↓
主链存在新的限制因素
```

继续 profiling 后发现：

```text
clock_nanosleep ≈ 38%
```

调用栈可以落到：

```text
clock_nanosleep
↓
__nanosleep
↓
unixSleep
↓
sqliteDefaultBusyCallback
↓
btreeBeginTrans
↓
sqlite3VdbeExec
↓
sqlite3_step / sqlite3_exec
↓
SqliteConnection::Execute
↓
TaskRuntimeRepository
```

真实业务路径包括：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
```

所以 V6 的出发点很明确：

> **SQLite Writer Lock 竞争已经成为 V5 完整 E2E 的重要等待来源。**

---

# 3. V5 的旧写模型

V5 多 Worker 模型可以简化成：

```text
Producer
├─ SQLite Connection
└─ ClaimNextReady()

Worker 1
├─ SQLite Connection 1
└─ TaskRuntimeRepository

Worker 2
├─ SQLite Connection 2
└─ TaskRuntimeRepository

Worker 3
├─ SQLite Connection 3
└─ TaskRuntimeRepository

Worker 4
├─ SQLite Connection 4
└─ TaskRuntimeRepository
```

这些线程都会推进 Runtime 状态，例如：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
...
```

SQLite 配置保持：

```text
journal_mode = WAL
synchronous = FULL
busy_timeout = 5000 ms
```

WAL 可以提升 Reader / Writer 并发，但并不会把 SQLite 变成多 Writer 数据库。

关键约束仍然是：

```text
SQLite 同一时刻只有一个 Writer
```

因此多个连接同时写时：

```text
Connection A → BEGIN IMMEDIATE → 获得 Writer Lock

Connection B → BEGIN IMMEDIATE
Connection C → BEGIN IMMEDIATE
Connection D → BEGIN IMMEDIATE
                  ↓
              SQLITE_BUSY
                  ↓
       sqliteDefaultBusyCallback
                  ↓
           clock_nanosleep
                  ↓
                重试
```

问题不是数据库“不够快”，而是应用层制造了不必要的内部 Writer 竞争。

---

# 4. V6A 的新写模型

V6A 的核心不是改 SQLite 参数，而是改写路径所有权。

```text
Producer
   \
Worker 1 \
Worker 2  → RuntimeDbWriter::Enqueue()
Worker 3 /          ↓
Worker 4/       Command Queue
                      ↓
             Dedicated Writer Thread
                      ↓
            单一 SQLite Writer Connection
                      ↓
              TaskRuntimeRepository
                      ↓
                    COMMIT
                      ↓
                promise / future ACK
```

核心变化：

```text
过去：
多个执行线程
→ 多个 SQLite Writer Connection
→ 让 SQLite 自己解决竞争

现在：
多个执行线程
→ 应用层 Queue
→ 一个 RuntimeDbWriter
→ 一个 SQLite Writer Connection
→ SQLite 内部不再承受本进程的多 Writer 竞争
```

这里非常重要的一点是：

```text
ACK 不能在 command 入队时完成
```

正确语义必须是：

```text
Command 入队
↓
Writer Thread 真正执行 Repository 操作
↓
SQLite COMMIT 成功
↓
promise / future 才 ready
```

否则会破坏原有 crash-safe 状态机的持久化边界。

---

# 5. V6A 明确没有改什么

为了保证性能变化具有清晰因果，V6A 没有通过降低可靠性换性能。

保持不变：

```text
busy_timeout = 5000 ms
journal_mode = WAL
synchronous = FULL

Crash-safe 顺序
VerifiedReceipt 顺序
Recovery 语义
Frozen Plan 语义
fdatasync / fsync 持久化要求
```

所以：

```text
V6A 的变量只有：
多 Writer
→ Dedicated Single Writer
```

这让结果具有可解释性。

`busy_timeout` 继续保留也有意义：

```text
Dedicated Writer
只消除当前进程内部不必要的多 Writer 竞争

仍然可能存在：
其他进程
其他命令
异常数据库占用
```

因此不能把 `busy_timeout` 删除后声称性能变好了。

---

# 6. 测试环境

环境记录文件：

```text
benchmark-results/v6a-dedicated-db-writer/environment.txt
```

核心环境：

| 项目 | 值 |
|---|---|
| Git commit | `8e3a1561d3f3edd1c781c37ae3b64771e090aeb3` |
| OS | Ubuntu / Linux 5.15.0-185-generic |
| 虚拟化 | VMware |
| CPU | 13th Gen Intel Core i7-13700F |
| VM CPU | 8 vCPU |
| 内存 | 7.7 GiB |
| Swap | 4 GiB |
| 文件系统 | ext4 |
| 根磁盘 | 100 GiB |
| GCC | 11.4.0 |
| CMake | 3.22.1 |

环境记录时使用：

```bash
mkdir -p benchmark-results/v6a-dedicated-db-writer

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
} | tee benchmark-results/v6a-dedicated-db-writer/environment.txt
```

## 6.1 关键命令解释

| 命令/关键字 | 作用 |
|---|---|
| `mkdir -p` | 创建目录；目录已存在时不报错 |
| `{ ... }` | 把多条 Shell 命令组合成一个命令组 |
| `git rev-parse HEAD` | 输出当前提交完整 SHA，保证结果可追溯 |
| `git status --short` | 以短格式记录工作区是否有未提交修改 |
| `uname -a` | 记录内核和系统信息 |
| `lscpu` | 记录 CPU、核心、虚拟化、指令集 |
| `free -h` | 记录内存和 Swap，`-h` 表示人类可读 |
| `lsblk` | 记录块设备和磁盘布局 |
| `df -T` | 记录挂载点、容量和文件系统类型 |
| `tee file` | 一边在终端显示，一边写入文件，避免测试记录丢失 |

环境记录不是“形式工作”。

它解决的是：

> 后续如果 V6B 数字变化，要先确认是不是代码变化，而不是 VM CPU、内存、文件系统、编译器或环境变化。

---

# 7. Benchmark 构建与入口确认

构建：

```bash
cmake --preset bench-release
cmake --build --preset bench-release -j2
```

确认 V6A benchmark 参数：

```bash
./build/bench-release/photobridge_bench --help | \
  grep -E 'e2e-v6a|warmup-repetitions|e2e-workers'
```

确认存在：

```text
e2e-v6a
e2e-v6a-matrix
--warmup-repetitions
--e2e-workers
```

## 7.1 关键字解释

| 关键字 | 作用 |
|---|---|
| `cmake --preset bench-release` | 使用仓库定义好的 Release Benchmark 配置生成构建目录 |
| `cmake --build` | 执行真正的编译 |
| `--preset bench-release` | 使用与 Configure 对应的构建 preset |
| `-j2` | 最多并行两个编译任务；这是编译并发，不是 benchmark Worker |
| `--help` | 输出 benchmark 支持的 workload 和参数 |
| `grep -E` | 使用扩展正则筛选关键参数 |
| `|` | 把前一个命令输出传给下一个命令 |

Benchmark 必须使用 Release。

原因：

```text
Debug / Sanitizer
→ 有额外检查和编译差异
→ 适合正确性
→ 不适合作为正式性能成绩
```

---

# 8. Benchmark 数据规模

V6A 延续 V5 大规模 E2E 基准：

```text
64 files
× 16 MiB / file
= 1024 MiB
= 1 GiB
```

完整主链：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

核心正式配置：

```text
Worker = 4
Warmup = 1
Formal repetitions = 10
```

Worker 扩展性：

```text
1
2
4
8
```

这是非常关键的测试设计。

因为 V6A 假设本身就是：

```text
1 Worker：
原本没有明显多 Writer 竞争
Dedicated Writer 可能收益较小

2 / 4 / 8 Worker：
并发提高后
V5 多连接 Writer 竞争更明显
V6A 应该更稳定
```

---

# 9. Smoke Test

正式跑 10 次之前先执行单次 Smoke：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4 \
  --output benchmark-results/v6a-dedicated-db-writer/smoke-worker4.json
```

结果：

```text
Wall time ≈ 11.024 s
吞吐 ≈ 92.9 MiB/s
CPU ≈ 187.1%
Peak RSS ≈ 29.1 MiB
```

Smoke 的用途不是拿性能结论，而是先确认：

```text
V6A 主链能跑通
SQLite 没有直接报错
1 GiB 数据规模正确
4 Worker 参数正确
JSON 输出正常
```

## 9.1 参数解释

| 参数 | 作用 |
|---|---|
| `--workload e2e-v6a` | 选择 V6A Dedicated DB Writer 的完整 E2E workload |
| `--repetitions 1` | 只执行一次，作为 Smoke |
| `--e2e-workers 4` | migrate 阶段使用 4 Worker |
| `--output ...json` | 把结构化结果写入 JSON，方便后续统计与归档 |

---

# 10. 4 Worker 正式 10 次 E2E

正式命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --warmup-repetitions 1 \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v6a-dedicated-db-writer/e2e-v6a-4w.json
```

## 10.1 为什么需要 warmup

`--warmup-repetitions 1` 表示：

```text
先完整跑 1 次
但不进入正式统计
```

主要目的：

```text
让程序代码页、动态库、文件系统 page cache 等进入较稳定状态
↓
减少第一轮冷启动对 10 次正式样本的污染
```

这不是“隐藏慢数据”。

因为：

```text
Warmup 次数固定
V5 / V6A 比较口径要提前定义
正式样本仍完整保留
```

---

# 11. V6A 4 Worker 十次原始样本

| 样本 | Wall | 吞吐 | CPU utilization |
|---:|---:|---:|---:|
| 1 | 8.550 s | 119.77 MiB/s | 169.94% |
| 2 | 8.136 s | 125.86 MiB/s | 177.40% |
| 3 | 8.390 s | 122.05 MiB/s | 174.82% |
| 4 | 8.140 s | 125.80 MiB/s | 175.03% |
| 5 | 8.509 s | 120.34 MiB/s | 175.20% |
| 6 | 8.600 s | 119.07 MiB/s | 177.16% |
| 7 | 8.621 s | 118.79 MiB/s | 178.68% |
| 8 | 8.073 s | 126.85 MiB/s | 180.49% |
| 9 | 8.171 s | 125.33 MiB/s | 174.04% |
| 10 | 8.608 s | 118.96 MiB/s | 180.78% |

汇总：

| 指标 | V6A 4W |
|---|---:|
| Min wall | 8.073 s |
| Mean wall | **8.380 s** |
| Median wall | **8.390 s** |
| P95 | **8.621 s** |
| P99 | **8.621 s** |
| Max wall | 8.621 s |
| Mean throughput | **122.28 MiB/s** |
| Mean CPU | **176.35%** |
| CPU P95/P99 | 180.78% |
| Peak RSS | **30,064 KiB = 29.36 MiB** |
| 10 次 kernel write | 10.101 GiB |
| 平均每次 kernel write | 约 1.010 GiB |
| 平均 read syscall | 5,784 / run |
| 平均 write syscall | 4,416 / run |

10 次 Wall 范围：

```text
8.073 s
~
8.621 s
```

跨度约：

```text
0.548 s
```

因此这组数据没有明显离群的异常长尾。

---

# 12. Mean / Median / P95 / P99 分别表示什么

## Mean

```text
所有样本耗时相加
÷
样本数量
```

适合表示整体平均性能。

## Median

排序后中间位置的值。

它比 Mean 更不容易被少量极端值影响。

## P95

表示：

> 约 95% 的样本不高于这个延迟。

## P99

表示：

> 约 99% 的样本不高于这个延迟。

本次只有 10 个正式样本，benchmark 的 P95/P99 最终都落到最大附近。

所以：

> 这里的 P95/P99 主要作为版本间统一口径，不应该把 10 样本 P99 解释成大规模线上服务那种高统计置信度的 P99。

---

# 13. V5 vs V6A 正式核心对比

V5 正式结果：

```text
4 Worker
1 GiB
10 repetitions
```

V5：

```text
Mean wall = 8568.214 ms
Median = 8576.541 ms
P95/P99 = 8766.896 ms
CPU utilization = 144.354%
Peak RSS = 13460 KiB
```

V6A：

```text
Mean wall = 8379.638 ms
Median = 8390.209 ms
P95/P99 = 8620.594 ms
CPU utilization = 176.352%
Peak RSS = 30064 KiB
```

对比：

| 指标 | V5 | V6A | 结论 |
|---|---:|---:|---|
| Mean wall | 8.568 s | **8.380 s** | **-2.20%** |
| Median | 8.577 s | **8.390 s** | **-2.17%** |
| P95/P99 | 8.767 s | **8.621 s** | **-1.67%** |
| Throughput | 119.51 MiB/s | **122.28 MiB/s** | **+2.32%** |
| CPU | 144.35% | 176.35% | 更高 |
| RSS | 13.14 MiB | 29.36 MiB | 更高 |

## 13.1 怎么理解“只提升约 2%”

不能得出：

```text
V6A 没价值
```

因为 V6A 的首要验收目标不是：

```text
必须 +20% E2E
```

而是：

```text
SQLite busy backoff 显著下降
+
架构把 Writer 所有权收敛
+
正确性不退化
```

如果竞争被消除，而 E2E 只小幅变快，说明：

```text
SQLite Writer contention
从主要等待因素中移除
↓
新的瓶颈接管总耗时
```

后续 `strace` 和 `perf` 确实证明了这一点。

---

# 14. CPU 利用率为什么反而升高

V5：

```text
144.35%
```

V6A：

```text
176.35%
```

这里的 Linux 多线程进程 CPU utilization：

```text
100% ≈ 持续占满 1 个逻辑 CPU
200% ≈ 平均约占满 2 个逻辑 CPU
```

V6A CPU 提高不能简单评价为好或坏。

结合其他证据：

```text
V5：
线程在 SQLite busy handler 里 sleep

V6A：
busy sleep 消失
Worker 更持续推进数据处理
```

因此更合理的解释是：

> **V6A 减少了 SQLite busy sleep，线程把更多时间用于真实工作，所以 CPU 活跃度提高。**

同时必须承认代价：

```text
Dedicated Writer
Queue
future / condition_variable
更多并发活跃状态
```

也增加了内存和同步成本。

---

# 15. RSS 为什么增加

V5 Peak RSS：

```text
13.14 MiB
```

V6A：

```text
29.36 MiB
```

增量约：

```text
16.2 MiB
```

这不是本轮的主要失败信号，因为绝对量仍然很小。

合理来源包括：

```text
更多 Worker 同时活跃
每 Worker Copy / Hash buffer
io_uring 相关内存
RuntimeDbWriter Queue / Command / future
线程栈和运行时对象
```

但本报告不把 RSS 增量归因到某一个具体对象，因为本轮没有做 heap profiler。

严谨表达：

> V6A 的内存开销明显增加，但绝对峰值仍只有约 29 MiB；若后续任务规模扩大或 Queue 改成更大批量，需要再次观察内存。

---

# 16. 1 / 2 / 4 / 8 Worker 扩展性矩阵

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-v6a-matrix \
  --warmup-repetitions 1 \
  --repetitions 3 \
  --output benchmark-results/v6a-dedicated-db-writer/e2e-v6a-matrix.json
```

`e2e-v6a-matrix` 自动执行：

```text
1 Worker
2 Worker
4 Worker
8 Worker
```

每档：

```text
1 warmup
+
3 formal repetitions
```

结果：

| Worker | Mean Wall | Mean Throughput | CPU | Peak RSS |
|---:|---:|---:|---:|---:|
| 1 | 11.276 s | 90.82 MiB/s | 104.61% | 14.29 MiB |
| 2 | 9.180 s | 111.76 MiB/s | 140.17% | 19.38 MiB |
| 4 | 8.382 s | 122.18 MiB/s | 175.97% | 29.48 MiB |
| 8 | **8.005 s** | **127.93 MiB/s** | 198.32% | 49.63 MiB |

---

# 17. Worker 扩展性分析

Wall time：

```text
1W → 2W
11.276 s → 9.180 s
下降 18.59%

2W → 4W
9.180 s → 8.382 s
再下降 8.70%

4W → 8W
8.382 s → 8.005 s
再下降 4.49%
```

吞吐：

```text
1W → 2W
+23.05%

2W → 4W
+9.33%

4W → 8W
+4.71%
```

1W → 8W 总体：

```text
Wall time：下降 29.01%
Throughput：提升 40.86%
```

趋势非常明确：

```text
Worker 增加
→ 性能继续提高
→ 但边际收益不断下降
```

即：

```text
1 → 2：收益明显
2 → 4：仍有价值
4 → 8：只剩小幅收益
```

因此当前 VM / workload 下：

> **4 Worker 是性能与资源开销之间更合理的默认点；8 Worker 可以继续提升，但已经进入明显的收益递减区。**

不能仅根据这组数据说：

```text
4 Worker 永远最优
```

因为最佳 Worker 数会受：

```text
CPU 核数
存储速度
文件大小
Hash 开销
SMB / 本地盘
page cache
```

影响。

---

# 18. perf stat：系统级运行画像

命令：

```bash
mkdir -p benchmark-results/v6a-dedicated-db-writer/perf

perf stat \
  -o benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 3 \
  --e2e-workers 4 \
  --output benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w.json
```

结果：

```text
task-clock       50,878.13 ms
CPUs utilized    1.597

context-switches 18,602
                 365.619 / sec

cpu-migrations   1,301
                 25.571 / sec

page-faults      194,750
                 3.828 K / sec

elapsed          31.8566 s
user             22.3196 s
sys              28.6854 s
```

VM 环境：

```text
cycles        <not supported>
instructions  <not supported>
branches      <not supported>
branch-misses <not supported>
```

## 18.1 `perf stat` 关键字解释

| 关键字 | 含义 |
|---|---|
| `perf stat` | 对一个程序执行期间的性能计数器做汇总统计 |
| `-o file` | 把 perf 输出写到指定文件 |
| `task-clock` | 所有线程累计获得的 CPU 执行时间 |
| `CPUs utilized` | task-clock / wall time，可理解为平均同时用了多少 CPU |
| `context-switches` | 操作系统发生线程/进程上下文切换的次数 |
| `cpu-migrations` | 线程从一个 CPU 调度到另一个 CPU 的次数 |
| `page-faults` | 页缺失次数，不等于磁盘故障 |
| `cycles` | CPU 时钟周期数；本 VM PMU 不支持 |
| `instructions` | CPU 指令退休数量；本 VM 不支持 |

## 18.2 为什么 user + sys 可以大于 elapsed

因为程序是多线程的。

```text
elapsed
= 现实世界经过的墙钟时间

user / sys
= 多个线程累计的 CPU 时间
```

多个线程并行时：

```text
user + sys > wall
```

完全正常。

---

# 19. 为什么不能因为 perf stat 就说 CPU 已经打满

结果：

```text
1.597 CPUs utilized
```

说明 3 次 `perf stat` 整体平均同时大约使用 1.6 个 CPU。

不能说：

```text
4 Worker 就用了 4 个满核
```

因为 E2E 中存在：

```text
I/O 等待
fdatasync / fsync
阶段性串行流程
Verify
SQLite 持久化
condition_variable 等待
```

Worker 数不是“持续占满的核心数”。

---

# 20. 完整 strace -f -c

命令：

```bash
mkdir -p benchmark-results/v6a-dedicated-db-writer/strace

strace -c -f \
  -o benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-full-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4
```

关键结果：

| syscall | `% time` | seconds | calls | errors |
|---|---:|---:|---:|---:|
| `futex` | **50.30%** | 19.502 | 966 | 17 |
| `fdatasync` | **13.14%** | 5.094 | 129 | 0 |
| `fsync` | **12.24%** | 4.744 | 480 | 0 |
| `read` | 7.46% | 2.891 | 5465 | 0 |
| `io_uring_enter` | 5.39% | 2.088 | 1943 | 0 |
| `pwrite64` | 3.83% | 1.486 | 4515 | 0 |
| `write` | 2.77% | 1.073 | 1034 | 0 |
| `fcntl` | 1.88% | 0.729 | 3478 | 0 |
| `unlinkat` | 0.88% | 0.340 | 137 | 0 |
| `madvise` | 0.45% | 0.175 | 67 | 0 |

总计：

```text
22,909 syscalls
183 error-return syscalls
```

这里最重要的结果不是谁排第一，而是：

```text
完整 syscall 排名中
没有 clock_nanosleep
```

---

# 21. `strace -c -f` 关键字解释

| 关键字 | 含义 |
|---|---|
| `strace` | 跟踪 Linux 系统调用 |
| `-f` | 跟踪 fork / clone 出来的线程或子进程；多线程 benchmark 必须用 |
| `-c` | 不打印每一行 syscall，而是汇总次数、耗时、错误 |
| `-o file` | 写入文件 |
| `% time` | strace 统计的 syscall 累计耗时占比 |
| `usecs/call` | 平均每次 syscall 的耗时 |
| `calls` | 调用次数 |
| `errors` | 返回错误码的调用数量 |

注意：

> `strace` 本身会显著影响程序时间，因此 `strace` 下的 wall time 不作为正式性能成绩。

它的用途是：

```text
看系统调用结构
看等待发生在哪里
看版本优化后调用行为有没有迁移
```

---

# 22. 为什么不能说 futex 让程序“卡了 19.5 秒”

这是报告中最容易说错的一点。

`strace -f -c` 对多线程程序会累计多个线程的 syscall 时间。

例如：

```text
Worker 1 等待 5 s
Worker 2 等待 5 s
Writer 等待 5 s
```

这些等待可能是重叠发生的。

`strace -c` 可以累计成：

```text
15 s
```

但现实 wall time 并不一定多了 15 s。

所以正确表达：

> `futex` 是当前系统调用累计等待的最大组成部分，需要进一步看调用栈判断它是什么同步。

错误表达：

> V6A 有 19.5 秒 futex 瓶颈。

后续 futex stack 已经证明它主要是 Dedicated Writer 队列的正常条件变量同步。

---

# 23. fcntl 锁定向统计

先做限定 syscall 的汇总：

```bash
strace -f -c \
  -e trace=fcntl,futex,fdatasync,fsync,pwrite64,write \
  -o benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-lock-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4
```

结果：

```text
futex       57.97%
fdatasync   18.60%
fsync       14.41%
pwrite64     4.10%
write        3.06%
fcntl        1.87%
```

`fcntl`：

```text
3478 calls
累计约 0.600 s
约 1.87%
```

这说明：

> 文件锁 syscall 本身已经不是这个 trace 中的大头。

但仅凭汇总表仍不能证明：

```text
没有锁竞争
```

所以继续抓详细 `fcntl`。

---

# 24. fcntl 详细锁轨迹

命令：

```bash
strace -f -tt -T \
  -e trace=fcntl \
  -o benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-fcntl-trace.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4
```

## 24.1 关键参数解释

| 参数 | 作用 |
|---|---|
| `-f` | 跟踪所有相关线程 |
| `-tt` | 每条 syscall 输出高精度时间戳 |
| `-T` | 在每条 syscall 后显示该调用耗时 |
| `-e trace=fcntl` | 只抓 `fcntl`，降低噪声 |

汇总命令：

```bash
TRACE=benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-fcntl-trace.txt

{
  echo "=== total fcntl ==="
  grep -c 'fcntl(' "$TRACE"

  echo
  echo "=== F_SETLK / F_SETLKW ==="
  grep -E 'F_SETLK|F_SETLKW' "$TRACE" | wc -l

  echo
  echo "=== lock types ==="
  grep -oE 'l_type=F_(RDLCK|WRLCK|UNLCK)' "$TRACE" \
    | sort \
    | uniq -c

  echo
  echo "=== blocking F_SETLKW ==="
  grep -c 'F_SETLKW' "$TRACE"

  echo
  echo "=== failed fcntl ==="
  grep 'fcntl(' "$TRACE" \
    | grep -E '= -1' \
    || true
}
```

## 24.2 grep / wc / sort / uniq 解释

| 命令 | 作用 |
|---|---|
| `grep -c` | 只输出匹配行数量 |
| `grep -E` | 使用扩展正则 |
| `grep -o` | 只输出匹配到的那一部分 |
| `wc -l` | 统计行数 |
| `sort` | 排序，让相同内容相邻 |
| `uniq -c` | 对相邻相同内容计数 |
| `|| true` | 即使 grep 没找到内容返回非 0，也不让命令组中断 |

---

# 25. fcntl 实测结果

```text
total fcntl = 3478

F_SETLK / F_SETLKW 相关行 = 2925

lock type token:
F_RDLCK = 756
F_UNLCK = 1467
F_WRLCK = 714

blocking F_SETLKW = 0

failed fcntl = 0
```

注意：

`lock types` 的 grep 是对整个 trace 搜索 `l_type=...`，可能包含不只是 `F_SETLK` 的行，因此三个 lock type 数量不应该直接和 2925 强行做总和等式。

真正最有价值的两个证据：

```text
F_SETLKW = 0
failed fcntl = 0
```

含义：

```text
没有观察到阻塞式 fcntl 锁等待
没有观察到 fcntl 锁操作失败
```

严谨表述应该是：

> **在本次 V6A / 4 Worker / 1 GiB trace 中，没有观察到内核 `fcntl` 层面的阻塞式锁等待或锁获取失败。**

不要扩大成：

```text
SQLite 在任何场景永远不会锁等待
```

因为：

```text
外部进程
不同 workload
不同数据库访问模式
```

仍可能造成竞争。

---

# 26. SQLite busy sleep 专项验证

这是 V6A 最核心的一项证据。

命令：

```bash
strace -f -k \
  -e trace=clock_nanosleep \
  -o benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-nanosleep-stack.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4
```

## 26.1 参数解释

| 参数 | 作用 |
|---|---|
| `-f` | 跟踪所有线程 |
| `-k` | 为 syscall 输出用户态调用栈 |
| `-e trace=clock_nanosleep` | 只抓 sleep syscall，专门验证 SQLite busy callback |
| `-o` | 保存 trace |

为什么是 `-k`：

仅知道：

```text
clock_nanosleep()
```

还不够。

程序中可能有很多主动 sleep。

真正要证明 SQLite Writer Lock contention，需要看到：

```text
clock_nanosleep
↓
unixSleep
↓
sqliteDefaultBusyCallback
↓
SQLite transaction
↓
TaskRuntimeRepository
```

所以必须看调用栈来源。

---

# 27. V5 与 V6A busy sleep 证据

V5 曾明确抓到：

```text
clock_nanosleep
↓
__nanosleep
↓
unixSleep
↓
sqliteDefaultBusyCallback
↓
btreeBeginTrans
↓
sqlite3VdbeExec
↓
sqlite3_step / sqlite3_exec
↓
SqliteConnection::Execute
↓
TaskRuntimeRepository::PersistVerifiedReceipt
...
```

而 V6A 汇总：

```text
total clock_nanosleep = 0

sqliteDefaultBusyCallback = 0

unixSleep = 0

TaskRuntimeRepository = 0
```

完整 `strace -f -c` 中也没有 `clock_nanosleep`。

因此这不是：

```text
过滤命令漏抓
```

而是两种独立观察方式一致：

```text
定向 clock_nanosleep trace = 0
+
完整 syscall summary 中不存在 clock_nanosleep
```

核心结论：

> **V5 中已经确认的 `SQLite busy → clock_nanosleep` 路径，在本次 V6A 4 Worker E2E 中已经完全没有被观察到。**

这就是 V6A 是否成功的最关键证据。

---

# 28. futex 为什么反而成为 syscall 第一名

完整 strace：

```text
futex ≈ 50.30%
```

这其实符合 V6A 设计前的预测。

旧模型：

```text
SQLite busy
→ SQLite busy handler
→ clock_nanosleep
```

新模型：

```text
Worker
→ Queue
→ condition_variable / future
→ Dedicated Writer
```

也就是说等待机制可能从：

```text
SQLite 内部不可控的 busy sleep
```

转移成：

```text
应用层明确设计的线程同步
```

所以不能要求：

```text
V6A 后 futex 必须下降
```

真正要验证的是：

```text
futex 是不是正常 Queue 同步
还是出现死锁 / 超时 / 新竞争
```

---

# 29. futex 调用栈验证

命令：

```bash
strace -f -k \
  -e trace=futex \
  -o benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-futex-stack.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 1 \
  --e2e-workers 4
```

汇总：

```bash
TRACE=benchmark-results/v6a-dedicated-db-writer/strace/e2e-v6a-4w-futex-stack.txt

{
  echo "=== total futex ==="
  grep -c 'futex(' "$TRACE"

  echo
  echo "=== FUTEX_WAIT ==="
  grep -c 'FUTEX_WAIT' "$TRACE"

  echo
  echo "=== FUTEX_WAKE ==="
  grep -c 'FUTEX_WAKE' "$TRACE"

  echo
  echo "=== timed out ==="
  grep 'futex(' "$TRACE" | grep -c 'ETIMEDOUT' || true

  echo
  echo "=== failed futex ==="
  grep 'futex(' "$TRACE" | grep '= -1' | head -30 || true

  echo
  echo "=== synchronization stack keywords ==="
  grep -E 'condition_variable|pthread_cond|pthread_join|Writer|writer|TaskRuntimeRepository' "$TRACE" \
    | head -80 || true
}
```

---

# 30. futex 实测结果

```text
total futex = 891

FUTEX_WAIT = 301
FUTEX_WAKE = 590

ETIMEDOUT = 0

EAGAIN = 1
```

调用栈重点：

```text
pthread_cond_signal
↓
RuntimeDbWriter::Enqueue()
↓
RuntimeDbWriter::ClaimNextReady()
```

Writer 线程：

```text
pthread_cond_wait
↓
RuntimeDbWriter::Run()
```

执行 Command：

```text
RuntimeDbWriter::Run()
↓
RuntimeDbWriter::Execute(variant<...>)
```

还可以看到：

```text
RuntimeDbWriter::MarkCommitIntent
RuntimeDbWriter::ClaimNextReady
RuntimeDbWriter::Execute
```

因此：

> **futex 的主要来源与 V6A 新引入的 Dedicated Writer Queue / condition_variable 设计一致。**

---

# 31. `FUTEX_WAIT ... EAGAIN` 是不是错误

不是这里意义上的程序故障。

`futex WAIT` 的核心逻辑类似：

```text
只有内存中的值仍等于 expected
才真正睡眠
```

如果线程准备睡眠时：

```text
另一个线程已经修改了值
```

内核会直接返回：

```text
EAGAIN
```

意思更接近：

> 条件已经变了，不需要睡。

所以本次：

```text
1 次 EAGAIN
0 次 ETIMEDOUT
```

不能解释成：

```text
RuntimeDbWriter 锁失败
```

反而最重要的是：

```text
没有 futex timeout
Debug / Sanitizer 全部通过
Stop/Drain 测试通过
```

没有看到死锁迹象。

---

# 32. perf record：用户态 CPU 热点

因为 VM 不支持可靠的硬件 PMU：

```text
cycles
instructions
branches
branch-misses
```

所以使用软件事件：

```bash
perf record -e cpu-clock:u -g \
  -o benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w-user.data \
  ./build/bench-release/photobridge_bench \
  --workload e2e-v6a \
  --repetitions 3 \
  --e2e-workers 4
```

导出：

```bash
perf report --stdio \
  -i benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w-user.data \
  > benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w-user-report.txt
```

查看：

```bash
head -80 \
  benchmark-results/v6a-dedicated-db-writer/perf/e2e-v6a-4w-user-report.txt
```

---

# 33. perf record 参数解释

| 参数 | 含义 |
|---|---|
| `perf record` | 采样并生成原始 `perf.data` 类文件 |
| `-e cpu-clock:u` | 使用 CPU clock 软件采样事件；`:u` 只采用户态 |
| `-g` | 保存调用栈信息 |
| `-o file.data` | 指定原始采样文件 |
| `perf report` | 读取 record 文件并聚合热点 |
| `--stdio` | 输出纯文本，适合归档和 Git |
| `-i file.data` | 指定输入 perf 数据文件 |
| `>` | Shell 重定向，把文本报告写入文件 |
| `head -80` | 只看前 80 行，快速定位主要热点 |

这里的 `cpu-clock:u` 很重要。

它回答的是：

> 用户态真正消耗 CPU 的代码在哪里？

它不会完整体现：

```text
fdatasync 等待
fsync 等待
内核 I/O 时间
```

这些要结合 `strace` / `perf stat` 一起看。

---

# 34. V6A CPU 热点结果

`perf report`：

```text
blake3_hash_many_avx2               85.55%

WriteDeterministicFile              11.61%

sqlite3VdbeExec                      0.05%

TryIoUringCopyAndHash                0.04%

MigrationService worker lambda       0.18%

RuntimeDbWriter::PersistVerifiedReceipt
约 0.01% 量级
```

## 34.1 第一结论：SQLite 不再是用户态 CPU 热点

`sqlite3VdbeExec`：

```text
0.05%
```

说明：

> V6A 当前用户态 CPU 主要不花在 SQLite 指令执行上。

同时：

```text
RuntimeDbWriter
也只出现在极低比例样本中
```

所以 Dedicated Writer 本身没有形成明显用户态计算热点。

---

# 35. 第二结论：新的 CPU 热点已经迁移到 BLAKE3

```text
blake3_hash_many_avx2 = 85.55%
```

这是非常强的热点集中度。

说明在当前 VM / 热缓存 / workload 下：

```text
真正执行 CPU 指令时
绝大多数时间在做 BLAKE3 AVX2 Hash
```

这和之前 V4 BLAKE3 SIMD 优化并不矛盾。

SIMD 已经让单字节 Hash 更快，但 E2E 中仍然需要大量：

```text
Copy-time Hash
Independent Verify
Source / Target Hash
```

当 SQLite busy sleep 被消除后：

```text
BLAKE3 自然成为更明显的 CPU 热点
```

这就是“瓶颈迁移”。

---

# 36. `WriteDeterministicFile` 11.61% 怎么理解

这个函数是 benchmark 构造确定性测试输入时使用的。

因此它主要属于：

```text
Benchmark fixture / 数据准备成本
```

而不是用户真实照片迁移时必须存在的业务热点。

所以面试时不能说：

```text
项目第二大业务瓶颈是 WriteDeterministicFile
```

正确说法：

> perf 用户态报告中约 11.6% 落在 benchmark 输入生成函数；真实迁移主链的主要 CPU 热点仍是 BLAKE3。

---

# 37. 系统调用瓶颈迁移

把 CPU 和 syscall 两类证据合起来：

用户态：

```text
BLAKE3 AVX2
≈ 85.55%
```

系统调用累计时间：

```text
futex
fdatasync
fsync
read
io_uring_enter
pwrite64
```

因此 V6A 后的下一层限制可以概括为：

```text
CPU 侧：
BLAKE3 Hash

I/O / durability 侧：
fdatasync
fsync

并发协调侧：
RuntimeDbWriter Queue
condition_variable / futex
```

而不是 V5 的：

```text
SQLite busy sleep
```

---

# 38. fdatasync / fsync 为什么不能为了性能直接删

完整 strace 中：

```text
fdatasync ≈ 13.14%
fsync ≈ 12.24%
```

看起来很贵。

但 PhotoBridge 的目标不是单纯跑分。

Crash-safe commit 依赖：

```text
write temp
↓
fdatasync(temp)
↓
Verify
↓
VerifiedReceipt
↓
rename-no-replace
↓
fsync(parent directory)
```

直接删掉：

```text
fdatasync
fsync directory
```

确实可能让 benchmark 更快。

但这等于：

```text
修改持久化语义
↓
扩大断电 / crash 数据丢失窗口
↓
破坏原设计保证
```

所以它不能混在 V6A 中作为“性能优化”。

如果未来实验：

```text
FULL vs NORMAL
或
不同 durability policy
```

必须作为独立实验，并明确可靠性语义变化。

---

# 39. io_uring 在完整 strace 中的位置

可以观察到：

```text
io_uring_enter
5.39%
1943 calls
```

这说明 V5 引入的 `io_uring` 数据路径确实仍在 V6A E2E 中工作。

因此 V6A 不是：

```text
为了改 SQLite
不小心回退掉 io_uring
```

CPU report 也出现：

```text
TryIoUringCopyAndHash
io_uring_submit
_io_uring_get_cqe
```

说明优化链是累积保留的。

---

# 40. Debug 全量回归

命令：

```bash
cmake --preset debug

cmake --build --preset debug -j2

ctest --preset debug \
  --output-on-failure \
  | tee benchmark-results/v6a-dedicated-db-writer/ctest-debug.txt
```

结果：

```text
100% tests passed
0 tests failed out of 228

Total Test time = 26.47 s
```

## 40.1 命令解释

| 参数 | 作用 |
|---|---|
| `cmake --preset debug` | 使用 Debug 配置 |
| `ctest --preset debug` | 按项目 Debug test preset 跑全部测试 |
| `--output-on-failure` | 只有测试失败时完整打印失败输出，方便排错 |
| `tee` | 终端显示同时保存测试记录 |

---

# 41. V6A 新相关 Debug 测试

特别关键：

```text
RuntimeDbWriterTest.AckFollowsCommitAndRepositoryErrorsPropagate
RuntimeDbWriterTest.MultiThreadSubmitDrainsEveryCommand
RuntimeDbWriterTest.ReceiptAndRetryUseTheWriterConnection
RuntimeDbWriterTest.StopDrainsAcceptedCommandsAndWakesAllCallers
```

全部通过。

它们对应验证：

```text
ACK 在真正持久化后返回
Repository 错误可以传播
多线程提交不会丢 Command
Stop 会 drain 已接受 Command
等待线程会被唤醒
Receipt / Retry 走 Dedicated Writer
```

同时：

```text
RunCliTest.MigratesAndResumesMoreTasksThanQueueCapacity
```

通过。

说明：

```text
任务数 > Queue Capacity
```

时不会因为有界队列直接死锁。

---

# 42. Crash Recovery 回归

最重要的 E2E crash 测试也通过：

```text
ProcessCrashE2ETest.SigkillResumeVerifyCoversAllCommitWindows
ProcessCrashE2ETest.MultipleWorkersRecoverReceiptsAndPartialTemps
```

这对 V6A 很关键。

因为性能优化最怕：

```text
吞吐变快
↓
但 crash-safe 状态机被破坏
```

当前测试支持：

```text
Dedicated Writer 引入后
原有 crash window / receipt / resume / verify 行为仍通过自动化验证
```

---

# 43. Sanitizer 全量回归

命令：

```bash
cmake --preset sanitized

cmake --build --preset sanitized -j2

ctest --preset sanitized \
  --output-on-failure \
  | tee benchmark-results/v6a-dedicated-db-writer/ctest-sanitized.txt
```

结果：

```text
100% tests passed
0 tests failed out of 228

Total Test time = 88.01 s
```

当前输出没有出现：

```text
AddressSanitizer error
heap-use-after-free
buffer overflow
UndefinedBehaviorSanitizer runtime error
```

所以在当前 sanitizer 配置覆盖范围内，没有发现对应的内存安全 / 未定义行为问题。

---

# 44. Sanitizer 为什么比 Debug 慢很多

Debug：

```text
26.47 s
```

Sanitized：

```text
88.01 s
```

这是正常现象。

Sanitizer 会插入额外运行时检查：

```text
内存访问合法性
对象生命周期
部分未定义行为检查
```

所以：

```text
Sanitizer 时间
不能拿去和 Release Benchmark 比性能
```

它只回答：

> 优化后有没有暴露明显的内存安全和未定义行为问题。

---

# 45. 正确性验收表

| 验收项 | 结果 | 证据 |
|---|---|---|
| 原有测试全部通过 | ✅ | Debug 228/228 |
| Sanitizer 全部通过 | ✅ | Sanitized 228/228 |
| RuntimeDbWriter ACK 语义 | ✅ | AckFollowsCommit... |
| 多线程提交 drain | ✅ | MultiThreadSubmit... |
| Stop 后唤醒等待者 | ✅ | StopDrains... |
| Receipt / Retry 经 Writer | ✅ | ReceiptAndRetry... |
| Queue 超容量场景 | ✅ | RunCli 多任务测试 |
| Crash-safe E2E | ✅ | ProcessCrashE2E |
| 多 Worker Recovery | ✅ | MultipleWorkersRecover... |
| busy sleep 消失 | ✅ | clock_nanosleep=0 |
| 阻塞 fcntl lock | ✅ 未观察到 | F_SETLKW=0 |
| failed fcntl | ✅ 未观察到 | 0 |
| futex timeout | ✅ 未观察到 | ETIMEDOUT=0 |

---

# 46. 性能验收表

| 验收项 | 结果 |
|---|---|
| 4W Mean wall 改善 | ✅ 8.568 s → 8.380 s |
| 4W Throughput 改善 | ✅ +2.32% |
| Worker 扩展到 8W 无性能倒退 | ✅ |
| 1→8W Throughput | ✅ +40.86% |
| SQLite busy backoff | ✅ 本次为 0 |
| CPU 热点重新 profiling | ✅ |
| 新热点明确 | ✅ BLAKE3 / durability / futex |
| Perf / strace / lock stack 证据完整 | ✅ |

---

# 47. V6A 的真正收益

如果只看：

```text
E2E +2.32%
```

会低估这次优化。

真正收益分三层。

## 第一层：性能

```text
4W Mean wall -2.20%
Throughput +2.32%
P95/P99 改善
```

## 第二层：扩展性

```text
1W → 8W
吞吐 +40.86%

4W → 8W
仍能继续增加约 4.71%
```

没有出现 Worker 越加越慢的明显 Writer contention 崩塌。

## 第三层：架构

从：

```text
N 个 Worker
→ N 个 SQLite Writer Connection
→ SQLite 内部抢锁
```

收敛为：

```text
N 个 Worker
→ 应用层显式 Queue
→ 1 个 Writer
```

把不可控等待变成可观测、可测试、可继续 batching 的结构。

这一点也是 V6B 能继续做 Transaction Convergence / Batching 的架构基础。

---

# 48. V6A 的代价

没有免费优化。

V6A 明确付出：

```text
1. 新增 RuntimeDbWriter abstraction
2. Queue / Command variant / promise / future
3. condition_variable / futex 同步
4. CPU utilization 提升
5. RSS 提升
6. 生命周期和 Stop/Drain 逻辑更复杂
```

所以必须有：

```text
MultiThreadSubmit
Ordered ACK
Stop After Drain
Pending caller wakeup
Sanitizer
Crash Recovery
```

这些测试兜底。

工程取舍不是：

```text
“Single Writer 一定更高级”
```

而是：

> 当前 profiling 已证明 Multi-Writer contention 存在，因此增加这层复杂度是有证据支持的。

---

# 49. 目前能说什么，不能说什么

## 可以说

> V5 中通过 `strace -k` 已经确认 SQLite busy callback 会触发 `clock_nanosleep`；V6A 把 runtime 写操作收敛到一个 Dedicated Writer 后，本次 4 Worker / 1 GiB E2E 中 `clock_nanosleep`、`sqliteDefaultBusyCallback` 都降到 0，`F_SETLKW` 和 failed `fcntl` 也都是 0。

可以说：

> V6A 4 Worker 平均 E2E 从约 8.57 s 降到 8.38 s，吞吐提升约 2.3%，但更重要的是 Writer contention 的证据消失，瓶颈迁移到 BLAKE3、持久化同步和应用层线程协调。

可以说：

> 1/2/4/8 Worker 下性能持续提升，但 4 Worker 后收益明显递减。

## 不能说

不能说：

```text
SQLite 锁已经永远不存在
```

只能说：

```text
本次 V6A benchmark 中
没有观察到内部 busy backoff 和阻塞式 fcntl 锁等待
```

不能说：

```text
futex 50% 表示程序被锁卡住一半时间
```

因为多线程 strace 是累计时间。

不能说：

```text
V6A 让 E2E 提升几十个百分点
```

正式 4W 对比只有约：

```text
+2.32% throughput
```

不能说：

```text
V6A 优化了 SQLite COMMIT 次数
```

那是 V6B Transaction Convergence / Batching 的目标。

---

# 50. 为什么 V6A 可以进入 V6B

原计划要求：

```text
V6A 正确性全部通过
并发 migrate 确认只有一个 Writer
busy handler 明显下降
没有死锁
没有 pending future
```

本轮证据：

```text
Debug 228/228
Sanitizer 228/228

RuntimeDbWriter 多线程 / Stop / Ack 测试通过

clock_nanosleep = 0
sqliteDefaultBusyCallback = 0

futex timeout = 0

Crash Recovery tests = passed
```

因此从本轮验证看：

> **V6A 已达到进入 V6B 的条件。**

---

# 51. 但 V6B 不应该直接追求“大 batch”

V6A 解决：

```text
谁在写
```

V6B 才解决：

```text
写多少次
```

正确顺序：

```text
V6A
多个 Writer
→ 一个 Writer

V6B1
先收敛 transaction 边界

V6B2
再试小批量 batch
例如 4 / 8 / 16
```

不能直接：

```text
一次把几十个状态变化随便合成一个大事务
```

因为 PhotoBridge 的状态变化与 crash-safe durable evidence 有严格顺序。

Batching 必须继续保证：

```text
真正 COMMIT 后才 ACK
Receipt 顺序不变
Recovery 语义不变
Crash Window 不被错误合并
```

---

# 52. 面试：30 秒版本

```text
我在 PhotoBridge 做完 io_uring 后发现一个现象：
局部 Copy+Hash 提升比较明显，但完整 E2E 提升很小。

我继续用 strace -k 做 profiling，发现 E2E 里大量时间在
clock_nanosleep，而且调用栈能一直追到
sqliteDefaultBusyCallback 和 TaskRuntimeRepository。

原因是多 Worker 每个都有 SQLite Connection，
虽然用了 WAL，但 SQLite 同一时刻仍然只有一个 Writer，
所以多个 BEGIN IMMEDIATE 会在内部抢 Writer Lock。

V6A 我没有改 synchronous=FULL、busy_timeout 或 crash-safe 语义，
而是增加 Dedicated DB Writer，
让 Worker 把 runtime command 放到队列，
由一个 Writer Connection 串行提交，COMMIT 后再 ACK。

结果 4 Worker E2E 平均从 8.57 秒降到 8.38 秒，
吞吐提升约 2.3%。
更关键的是 clock_nanosleep 和 sqliteDefaultBusyCallback 都降到 0，
F_SETLKW 和 failed fcntl 也是 0。
重新 perf 后 CPU 热点已经迁移到 BLAKE3 AVX2。
```

---

# 53. 面试：2～3 分钟完整版本

```text
这次 V6A 优化不是先拍脑袋改 SQLite 参数，而是从 profiling 出发。

前一个版本我已经做了 io_uring。
在 64 MiB Copy+Hash microbenchmark 上提升比较明显，
但是完整 1 GiB、4 Worker E2E 只提升很小，
所以我判断数据面已经不是唯一瓶颈。

我用 strace -f -c 继续看系统调用，
发现 clock_nanosleep 的累计占比很高。
然后用 strace -f -k 只跟踪 clock_nanosleep，
调用栈能看到：
clock_nanosleep
→ unixSleep
→ sqliteDefaultBusyCallback
→ btreeBeginTrans
→ sqlite3VdbeExec
→ TaskRuntimeRepository。

这说明多 Worker 下多个 SQLite Connection
会同时尝试写 runtime 状态。
SQLite 虽然是 WAL，但仍然只有一个 Writer，
所以其他连接会进入 busy handler，睡眠后重试。

V6A 我做的事情叫 Dedicated DB Writer。
Producer 和 Worker 不再各自直接做 runtime SQLite 写事务，
而是把 Claim、CommitIntent、TempWritten、Receipt、Retryable
这些 command 放进 RuntimeDbWriter 队列，
由一个专用 Writer Thread 和一个 Writer Connection 串行执行。
同时 COMMIT 成功后才完成 promise/future ACK，
这样不会破坏原来的 crash-safe 持久化边界。

我没有同时调 busy_timeout，也没有把 FULL 改成 NORMAL，
fdatasync、fsync、Receipt 顺序和 Recovery 语义都保持不变，
这样性能变化的因果比较清楚。

最终 4 Worker、1 GiB、1 次 warmup 加 10 次正式测试，
平均 wall time 从 V5 的 8.568 秒降到 8.380 秒，
吞吐从约 119.5 MiB/s 提高到 122.3 MiB/s，
提升只有约 2.3%。

但这个优化最重要的证据不是 2.3%：
V6A 重新抓 clock_nanosleep 后是 0，
sqliteDefaultBusyCallback 是 0；
fcntl trace 里 F_SETLKW 是 0，failed fcntl 也是 0。

完整 strace 里新的主要累计等待变成
futex、fdatasync 和 fsync。
我又继续抓 futex 调用栈，
发现主要是 RuntimeDbWriter::Enqueue 和 Run
对应的 condition_variable wait/wake，
没有 ETIMEDOUT，
所以这是应用层显式 Queue 的同步成本，
不是 SQLite busy sleep 又换了一个名字。

最后 perf record 看到用户态 CPU 有 85% 左右在
blake3_hash_many_avx2，
SQLite 执行只有很小比例。
这说明瓶颈已经真正迁移。

正确性方面 Debug 和 Sanitizer 都是 228/228 通过，
包括 RuntimeDbWriter 多线程提交、Stop Drain 和多 Worker Crash Recovery。

所以我对 V6A 的结论是：
E2E 提升不大，但成功消除了当前进程内部已经被证明存在的
SQLite Multi-Writer busy backoff，
把写路径收敛成了可控的 Single Writer，
也为下一阶段 transaction convergence / batching 打下基础。
```

---

# 54. 面试追问 1：为什么不用 mutex 包住所有 SQLite 写

可以回答：

```text
mutex 确实也能把本进程写操作串行化，
但我最终使用 Dedicated Writer，
是因为它不仅解决互斥，还明确了 SQLite Writer 的所有权。

Worker：
只生产 Command

Writer：
统一维护 Connection
统一执行 Repository 操作
统一决定 COMMIT 后 ACK
统一处理 Stop / Drain / 错误传播

这样后续做 Transaction Convergence 或 Batching 时，
所有 command 本来就在同一个线程里，
比在多个 Worker 外面套一把大锁更容易演进。
```

---

# 55. 面试追问 2：Single Writer 不会变成新瓶颈吗

回答：

```text
会有这个可能，所以我没有把架构正确当成性能正确，
而是专门测了 1 / 2 / 4 / 8 Worker。

实际结果是：
1W 约 11.28 s
2W 约 9.18 s
4W 约 8.38 s
8W 约 8.01 s

说明在当前 workload 下，
Writer Thread 还没有导致 Worker 增加后性能反向下降。

但 4W 到 8W 只改善约 4.5%，已经明显收益递减。
所以后面如果继续优化，
不是继续盲目加 Worker，
而是考虑 V6B 减少 Writer 事务次数。
```

---

# 56. 面试追问 3：为什么 CPU 变高还说优化成功

回答：

```text
因为优化前线程会在 SQLite busy handler 中 sleep，
这种等待 CPU 利用率本来就低。

Dedicated Writer 消除 busy sleep 后，
Worker 能更连续地推进 Copy、Hash、Verify，
所以 CPU 从约 144% 提高到 176%。

同时 wall time 降低、吞吐提高，
而且 perf 显示新增 CPU 主要落在 BLAKE3，
不是 RuntimeDbWriter 自己变成计算热点。

所以这里更高的 CPU 表示等待减少后，
机器在做更多真实工作。
```

---

# 57. 面试追问 4：为什么不直接把 synchronous=FULL 改成 NORMAL

回答：

```text
因为这会同时修改 durability policy。

如果 FULL → NORMAL 后 benchmark 变快，
我无法判断收益来自 Writer 架构，
还是来自减少持久化保证。

PhotoBridge 的核心亮点就是 crash-safe，
所以性能实验不能偷偷降低可靠性。

我把变量拆开：
V6A 只解决 Writer ownership；
如果以后测试 FULL vs NORMAL，
会作为独立 durability policy experiment，
并明确重新定义掉电语义和恢复保证。
```

---

# 58. 面试追问 5：futex 50% 是不是新锁竞争

回答：

```text
不能只看 strace 百分比下结论。

strace -f -c 对多线程会累计所有线程的 syscall 时间，
所以 futex 19 秒不等于 wall time 被阻塞 19 秒。

我继续用了 strace -f -k 抓 futex stack，
看到主要调用来自：
RuntimeDbWriter::Enqueue
pthread_cond_signal
RuntimeDbWriter::Run
pthread_cond_wait。

而且 891 次 futex 中：
301 WAIT
590 WAKE
0 ETIMEDOUT。

所以当前证据更支持它是 Queue / condition_variable
的正常线程协调，不是 SQLite busy contention。
```

---

# 59. 面试追问 6：怎么证明不是只把等待从 SQLite 换到应用层

回答：

```text
我不是只看一个 syscall。

第一：
正式 E2E wall time 有下降。

第二：
1/2/4/8 Worker 继续扩展时性能没有反向恶化。

第三：
clock_nanosleep 和 SQLite busy callback 消失。

第四：
futex stack 能明确落到 RuntimeDbWriter 的条件变量，
而且没有 timeout。

第五：
perf 用户态热点主要转移到 BLAKE3，
SQLite 和 RuntimeDbWriter 都不是明显 CPU hotspot。

所以结果不是简单换一个 syscall 名字，
而是把不可控的 SQLite busy retry
变成了应用层明确的生产者-消费者协调。
```

---

# 60. Benchmark 工具选择逻辑

整个 V6A 不是一个工具解决所有问题。

```text
Benchmark JSON
→ 回答：快了多少？

perf stat
→ 回答：整体 CPU / context switch / page fault 怎么样？

strace -f -c
→ 回答：系统调用时间主要在哪里？

strace -f -k clock_nanosleep
→ 回答：sleep 到底是谁触发的？

fcntl trace
→ 回答：文件锁有没有阻塞式等待 / 失败？

futex stack
→ 回答：新的线程等待来自哪里？

perf record
→ 回答：用户态 CPU 到底花在哪里？

Debug / Sanitizer / Crash E2E
→ 回答：性能优化有没有破坏正确性？
```

这就是性能问题定位时非常重要的原则：

> **不要用单一指标解释整个系统。**

---

# 61. 本轮结果目录建议

```text
benchmark-results/
└── v6a-dedicated-db-writer/
    ├── environment.txt
    ├── smoke-worker4.json
    ├── e2e-v6a-4w.json
    ├── e2e-v6a-matrix.json
    ├── ctest-debug.txt
    ├── ctest-sanitized.txt
    │
    ├── perf/
    │   ├── e2e-v6a-4w.json
    │   ├── e2e-v6a-4w-stat.txt
    │   ├── e2e-v6a-4w-user.data
    │   └── e2e-v6a-4w-user-report.txt
    │
    └── strace/
        ├── e2e-v6a-4w-lock-summary.txt
        ├── e2e-v6a-4w-fcntl-trace.txt
        ├── e2e-v6a-4w-fcntl-summary.txt
        ├── e2e-v6a-nanosleep-stack.txt
        ├── e2e-v6a-nanosleep-summary.txt
        ├── e2e-v6a-4w-full-summary.txt
        ├── e2e-v6a-4w-futex-stack.txt
        └── e2e-v6a-4w-futex-summary.txt
```

---

# 62. 一条完整的证据链怎么讲

最终可以压缩成：

```text
V5 io_uring 后
↓
Micro 提升明显但 E2E 提升有限
↓
strace 发现 clock_nanosleep ≈ 38%
↓
strace -k 证明来自 sqliteDefaultBusyCallback
↓
确认多个 SQLite Connection 竞争唯一 Writer
↓
V6A 引入 RuntimeDbWriter
↓
Worker 只提交 Command
↓
单 Writer Connection 串行 COMMIT
↓
4W E2E +2.32%
↓
clock_nanosleep = 0
↓
F_SETLKW = 0 / failed fcntl = 0
↓
futex stack = condition_variable Queue 协调
↓
perf hotspot = BLAKE3 AVX2
↓
Debug / Sanitizer = 228 / 228
↓
证明 Writer contention 被收敛
并且瓶颈发生迁移
```

---

# 63. 最终工程结论

V6A 的最终评价应该是：

> **成功，但成功的核心不是“大幅提高吞吐”，而是通过 profiling 驱动的架构改造，消除了本进程内部已经被证明存在的 SQLite Multi-Writer busy backoff，并保持了 PhotoBridge 的 crash-safe 持久化语义。**

正式性能：

```text
V5 4W：
8.568 s
119.51 MiB/s

V6A 4W：
8.380 s
122.28 MiB/s

Wall：
-2.20%

Throughput：
+2.32%
```

并发扩展：

```text
1W → 8W
Wall：
-29.01%

Throughput：
+40.86%
```

核心 contention 证据：

```text
clock_nanosleep = 0
sqliteDefaultBusyCallback = 0
F_SETLKW = 0
failed fcntl = 0
```

新的同步行为：

```text
futex
→ RuntimeDbWriter Queue / condition_variable
→ 0 timeout
```

新的 CPU 热点：

```text
blake3_hash_many_avx2
≈ 85.55%
```

正确性：

```text
Debug:
228 / 228

Sanitizer:
228 / 228
```

因此可以进入：

```text
V6B
Transaction Convergence / Batching
```

但 V6B 仍然必须坚持同样原则：

```text
先测
→ 再改
→ 单变量
→ 保持 crash-safe
→ 再测
→ 用 perf / strace 证明收益来源
```

---

# 64. 数据来源

本报告依据以下实际测试产物整理：

```text
environment(3).txt
smoke-worker4.json
e2e-v6a-4w.json
e2e-v6a-matrix.json
e2e-v6a-4w-stat.txt
e2e-v6a-4w-lock-summary.txt
e2e-v6a-4w-fcntl-summary.txt
e2e-v6a-nanosleep-summary.txt
e2e-v6a-4w-full-summary.txt
e2e-v6a-4w-futex-summary.txt
e2e-v6a-4w-user-report.txt
ctest-debug.txt
ctest-sanitized.txt

V5 对照：
e2e-large-4w-final.json
e2e-large-4w-final.txt
e2e-large-nanosleep-stack.txt

设计与验收依据：
PhotoBridge_V6A_V6B_SQLite写路径收敛_性能优化改造与验证计划.md
```

同时参考本次 V6A Benchmark 对话记录：

```text
https://chatgpt.com/share/6ab0af99-0ccc-83ea-a901-a57fb8198243
```

---

# 65. 最终一句话

```text
V6A 没有靠降低 SQLite durability 换分数，
而是用 Dedicated DB Writer 把多 Worker 的数据库写竞争
从 SQLite 内部 busy sleep
收敛成应用层可控的单 Writer Queue；
E2E 吞吐提升约 2.3%，但 busy backoff 在实测中降到 0，
随后热点迁移到 BLAKE3、fdatasync/fsync 和正常线程协调，
并且 Debug / Sanitizer 228 项测试全部通过。
```
