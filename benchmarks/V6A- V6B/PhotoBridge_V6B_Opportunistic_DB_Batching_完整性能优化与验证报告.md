# PhotoBridge V6B：SQLite Opportunistic DB Batching 性能优化与验证报告

> 对应版本：`main @ 7fe956f (V6B)`  
> 优化阶段：V6A Dedicated DB Writer → V6B Opportunistic DB Batching  
> 测试目录：`benchmark-results/v6b-opportunistic-batching/`

---

## 1. 结论摘要

V6A 已经把多个 Worker 对 SQLite 的高频状态写收敛到单独的 Dedicated DB Writer，因此主要矛盾不再是“多个 SQLite 写连接竞争 Writer Lock”，而变成：

```text
Worker 提交 1 条状态写
→ DB Writer 执行 1 次独立事务
→ WAL / pwrite64 / fsync / fcntl
→ commit
→ promise 完成
→ Worker future.get() 返回
```

V6B 的核心改造不是取消同步 ACK，也不是主动等待攒批，而是：

```text
Writer 被唤醒
→ 取出当前已经在队列中的多条 command
→ 在同一个 SQLite 写事务中顺序执行
→ 统一 commit
→ commit 成功后逐个 Complete()
→ 唤醒对应 Worker
```

因此 V6B 优化的是**事务固定成本的摊销**。

正式测试得到的核心结论：

1. 在 Runtime DB 状态写这一目标工作负载中，V6B 的收益非常明显。
   - 8 Worker 下，batch=8 相对 batch=1：
     - 吞吐约提升 **3.94 倍**
     - ACK 平均延迟下降约 **74.4%**
     - transaction groups 下降约 **80.3%**
     - write syscalls 下降约 **76.6%**
     - kernel write bytes 下降约 **76.6%**
   - batch=16 没有继续明显提升，因为 8 个同步提交 Worker 最多只能同时形成约 8 个 in-flight command。

2. V6B 的实际 batch 深度由并发提交者数量限制。
   - 1 Worker → 无法形成批次
   - 2 Worker → largest batch = 2
   - 4 Worker → largest batch = 4
   - 8 Worker → largest batch = 8

3. 1 GiB 完整迁移链路中，V6B **没有带来端到端吞吐提升**。
   - batch=4 / 16 与 batch=1 基本接近，均值甚至约慢 1%～2%。
   - batch=8 出现两次明显长尾。
   - 但 write syscall 数仍下降约 11%～13%。
   - 原因是完整链路的主要成本已经变成 copy、BLAKE3、verify、`fdatasync` 和文件系统 I/O，SQLite 状态写只占总成本的一小部分。

4. perf / strace 证据与设计目标完全一致。
   - `task-clock`、system CPU、context switch、`fsync`、`pwrite64`、`fcntl` 均显著下降。
   - CPU hotspot 中 `fsync` 仍是剩余主要热点，但绝对总工作量下降。
   - 完整 `strace` 未观察到 SQLite `busy_timeout` 导致的 `nanosleep` / `clock_nanosleep`。
   - `fcntl` 锁相关操作显著减少，但没有观察到 `EAGAIN/EACCES` 文件锁获取失败。
   - `futex` 调用栈证明 Worker 仍然同步等待 ACK，只是 batch 后每次等待显著缩短。

5. 正确性验证完整。
   - Debug：230 / 230 全部通过。
   - Sanitizer：230 / 230 全部通过。
   - 合批、事务回滚、多线程提交、停止排空、ACK-after-commit、崩溃恢复均覆盖。

---

# 2. 版本与测试基线

## 2.1 Git 状态

测试对应：

```text
branch: main
commit: 7fe956f (HEAD -> main, origin/main) V6B
```

当时 tag：

```text
v6a-dedicated-db-writer
```

尚未存在 V6B tag。

测试结果目录当时仍为未跟踪文件：

```text
?? benchmark-results/v6b-opportunistic-batching/
```

因此建议在报告与结果文件确认完成后，再统一提交并打 V6B tag。

---

## 2.2 测试环境

正式环境记录在：

```text
benchmark-results/v6b-opportunistic-batching/environment.txt
```

已知主要环境：

```text
OS / Kernel : Linux 5.15.0-185-generic
虚拟化      : VMware
CPU         : Intel Core i7-13700F
逻辑 CPU    : 8
内存        : 7.7 GiB
文件系统    : ext4
Compiler    : g++ 11.4
CMake       : 3.22.1
SQLite      : 3.53.4
liburing    : 2.15
BLAKE3      : 1.8.5
```

Benchmark 二进制：

```text
build/bench-release/photobridge_bench
```

---

# 3. V6A 的实现与剩余问题

## 3.1 V6A：Dedicated DB Writer

V6A 已经实现：

```text
多个 Worker
    ↓
MigrationRuntimeStore
    ↓
RuntimeDbWriter
    ↓
单条 SqliteConnection
    ↓
TaskRuntimeRepository
    ↓
SQLite
```

`RuntimeDbWriter` 持有：

```text
1 条 SqliteConnection
1 个 TaskRuntimeRepository
1 个 writer thread
mutex + condition_variable
std::deque<Command>
```

Command 主要包括：

```text
Claim
CommitIntent
TempWritten
Receipt
Retryable
```

每个 public DB 操作的同步流程：

```text
Worker
→ 创建 promise / future
→ Enqueue(command)
→ Writer 执行 repository 方法
→ repository 内部事务完成
→ promise.set_value(...)
→ Worker future.get() 返回
```

这保证了：

```text
ACK 一定发生在 durable transaction commit 之后
```

这是 PhotoBridge 恢复语义的重要约束。

---

## 3.2 V6A 的剩余性能问题

V6A 虽然消除了“多个写连接竞争 SQLite writer lock”的问题，但 Writer 每次只取一条 command：

```text
pop 1 command
→ BEGIN
→ SQL
→ WAL write
→ fsync
→ COMMIT
→ ACK
```

如果有 N 条 command：

```text
N 条 command
≈ N 次独立事务
≈ N 轮 transaction / WAL / sync / lock 固定成本
```

所以瓶颈由：

```text
多连接写锁竞争
```

转化为：

```text
单 Writer 串行执行过多小事务
```

---

# 4. V6B 的设计

## 4.1 Opportunistic Batching

V6B Writer 不再严格一次只处理 1 条 command。

Writer 被唤醒后：

```text
1. 先取得队首 command
2. 查看此时队列中是否已经存在更多 command
3. 最多取到配置的 batch size
4. 在一个共享 SQLite 写事务中执行这一批 command
5. commit 成功后，再完成每条 command 的 promise
```

关键点：

```text
不 sleep
不主动等待
不人为延迟第一条 command
```

也就是说：

> V6B 只消费“已经自然进入队列”的并发机会。

这避免了为了提高 batch size 而人为增加 ACK 延迟。

---

## 4.2 ACK 语义没有改变

V6B 仍然保持：

```text
SQL 执行
→ transaction commit
→ Complete(command)
→ promise set_value
→ future.get() 返回
```

而不是：

```text
Enqueue
→ 立即 ACK
→ 后台慢慢写 SQLite
```

所以 durable ACK 语义仍然成立。

---

## 4.3 Batch 失败语义

如果 batch 内任意操作失败：

```text
batch transaction rollback
```

不能让部分 command 已提交、部分 command 未提交。

对应测试：

```text
RuntimeDbWriterTest.FailedCommandRollsBackItsEntireBatch
```

已经在 Debug 和 Sanitizer 下通过。

---

# 5. 为什么 batch size 会被 Worker 数限制

每个 Worker 的调用模式是：

```text
提交 1 条 command
→ future.get()
→ 等 ACK
→ ACK 返回后才提交下一条
```

因此一个 Worker 同时最多只有 1 条 command 正在等待。

假设：

```text
workers = 4
```

最多同时有：

```text
4 条 in-flight command
```

即使配置：

```text
batch size = 16
```

Writer 实际也不可能一次拿到 16 条，因为只有 4 个同步提交者。

因此：

```text
实际 batch depth
≈ min(configured batch size, concurrent submitters)
```

正式测试验证：

```text
1 Worker → largest_batch = 0
2 Worker → largest_batch = 2
4 Worker → largest_batch = 4
8 Worker → largest_batch = 8
```

这里 1 Worker 的 `largest_batch=0` 表示没有形成“真正多 command batch”。

---

# 6. Runtime DB 状态写 Benchmark

## 6.1 Smoke Test

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload runtime-state-compare \
  --repetitions 1 \
  --output benchmark-results/v6b-opportunistic-batching/runtime-state-smoke.json
```

主要结果：

| Batch | Throughput | ACK | Transaction Groups | Largest Batch |
|---|---:|---:|---:|---:|
| 1 | 622.49 cmd/s | 12.80 ms | 1000 | 0 |
| 4 | 2392.06 cmd/s | 3.33 ms | 251 | 4 |
| 8 | 2724.89 cmd/s | 2.93 ms | 201 | 8 |
| 16 | 2795.05 cmd/s | 2.85 ms | 202 | 8 |

Smoke Test 已经显示：

```text
batch=1 → batch=4/8
```

有非常明显的吞吐和 ACK 改善。

batch=16 的实际最大批次仍然只有 8。

---

# 7. 8 Worker 正式 Runtime 测试

配置：

```text
runtime_commands = 1000
runtime_workers  = 8
repetitions      = 30
```

结果文件：

```text
runtime-state-30.json
```

## 7.1 结果

| 指标 | batch=1 | batch=4 | batch=8 | batch=16 |
|---|---:|---:|---:|---:|
| Throughput | 565.58 | 1948.47 | 2228.95 | 2255.84 cmd/s |
| ACK mean | 14.240 | 4.161 | 3.645 | 3.586 ms |
| Wall mean | 1786.99 | 522.21 | 458.33 | 450.84 ms |
| CPU mean | 696.63 | 285.23 | 254.03 | 251.43 ms |
| CPU util | 38.80% | 54.71% | 55.31% | 55.71% |
| Commands | 30000 | 30000 | 30000 | 30000 |
| Transaction Groups | 30000 | 7528 | 5917 | 5925 |
| Batched Commands | 0 | 29978 | 29950 | 29962 |
| Largest Batch | 0 | 4 | 8 | 8 |
| Kernel Write Bytes | 635,195,392 | 184,586,240 | 148,447,232 | 148,168,704 |
| Write Syscalls | 248,640 | 72,014 | 58,308 | 58,255 |

---

## 7.2 batch=8 相对 batch=1

吞吐：

```text
2228.95 / 565.58 ≈ 3.94×
```

ACK：

```text
14.240 ms → 3.645 ms
下降约 74.4%
```

Transaction Groups：

```text
30000 → 5917
下降约 80.3%
```

Write Syscalls：

```text
248640 → 58308
下降约 76.6%
```

Kernel Write Bytes：

```text
635 MB → 148 MB
下降约 76.6%
```

这组结果是 V6B 最核心的性能证据。

---

## 7.3 为什么不能说 batch=16 明显优于 batch=8

batch=8：

```text
2228.95 cmd/s
```

batch=16：

```text
2255.84 cmd/s
```

只差约：

```text
1.2%
```

而两者实际：

```text
largest_batch = 8
```

所以 batch=16 没有真正形成 16 command transaction。

因此正确结论是：

> 在 8 个同步提交 Worker 下，batch size 增大到 8 后已经达到实际并发上限；batch=16 没有进一步形成更大的实际事务批次，吞吐差异主要属于运行波动。

---

# 8. Worker 扩展性

## 8.1 1 Worker

| Batch | Throughput | ACK | Largest Batch |
|---|---:|---:|---:|
| 1 | 508.58 | 2.002 ms | 0 |
| 4 | 488.52 | 2.051 ms | 0 |
| 8 | 502.48 | 2.003 ms | 0 |
| 16 | 508.29 | 1.974 ms | 0 |

结论：

```text
1 submitter
→ enqueue 1
→ wait ACK
→ enqueue next
```

Writer 永远看不到第二条同时排队的 command。

因此没有 batching opportunity。

---

## 8.2 2 Worker

| Batch | Throughput | ACK | Transaction Groups | Largest Batch |
|---|---:|---:|---:|---:|
| 1 | 621.55 | 3.235 ms | 30000 | 0 |
| 4 | 1278.37 | 1.582 ms | 23843 | 2 |
| 8 | 1278.38 | 1.571 ms | 24503 | 2 |
| 16 | 1332.20 | 1.513 ms | 23982 | 2 |

最大实际批次始终：

```text
2
```

因此 batch=4 / 8 / 16 的容量实际上等价。

不能把这几组之间的小差别解释成“大 batch 更好”。

---

## 8.3 4 Worker

| Batch | Throughput | ACK | Transaction Groups | Largest Batch |
|---|---:|---:|---:|---:|
| 1 | 602.15 | 6.730 ms | 30000 | 0 |
| 4 | 1949.90 | 2.084 ms | 12162 | 4 |
| 8 | 1742.40 | 2.340 ms | 12234 | 4 |
| 16 | 1828.48 | 2.225 ms | 12073 | 4 |

batch=4 相对 batch=1：

```text
Throughput ≈ 3.24×
ACK ≈ -69%
Transaction Groups ≈ -59.5%
Write Syscalls ≈ -56.3%
Kernel Write Bytes ≈ -56.1%
```

但 batch=4 / 8 / 16 的：

```text
largest_batch 都是 4
```

所以不能根据某一次均值就声称 batch=4 是算法意义上的最优值。

---

## 8.4 固定 batch=8 的 Worker 扩展曲线

为了避免把 Worker 数和 batch size 两个变量混在一起，可以固定：

```text
batch size = 8
```

结果：

| Worker | Throughput | Largest Batch |
|---:|---:|---:|
| 1 | 502.48 cmd/s | 0 |
| 2 | 1278.38 cmd/s | 2 |
| 4 | 1742.40 cmd/s | 4 |
| 8 | 2228.95 cmd/s | 8 |

这是 V6B 最干净的并发扩展性证据。

---

## 8.5 batch=1 的 Worker 扩展曲线

控制组：

| Worker | Throughput |
|---:|---:|
| 1 | 508.58 cmd/s |
| 2 | 621.55 cmd/s |
| 4 | 602.15 cmd/s |
| 8 | 565.58 cmd/s |

说明：

```text
V6A Dedicated Writer
```

虽然允许多个 Worker 提交，但最终仍然：

```text
每条 command 独立 durable transaction
```

所以增加 submitter 数并不能线性提升数据库写吞吐，反而会增加 ACK 排队等待。

---

# 9. 1 GiB E2E 正式测试

## 9.1 测试规模

正式 E2E：

```text
64 files
× 16 MiB
= 1 GiB
```

Workers：

```text
4
```

配置：

```text
1 warmup
10 measured repetitions
```

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-v6b-compare \
  --warmup-repetitions 1 \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v6b-opportunistic-batching/e2e-v6b-compare-10.json
```

完整链路覆盖：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

---

## 9.2 正式结果

| 指标 | batch=1 | batch=4 | batch=8 | batch=16 |
|---|---:|---:|---:|---:|
| Wall mean | 8605.41 | 8746.31 | 9253.51 | 8699.98 ms |
| Median | 8603.30 | 8758.60 | 8813.38 | 8716.75 ms |
| Max / P95 | 8790.53 | 8829.26 | 11553.56 | 8923.02 ms |
| Throughput | 119.01 | 117.08 | 111.77 | 117.73 MiB/s |
| CPU mean | 15033.04 | 15481.56 | 15739.43 | 15610.00 ms |
| CPU util | 174.69% | 177.01% | 171.65% | 179.44% |
| Write Syscalls | 44,159 | 39,184 | 38,586 | 39,023 |

---

## 9.3 相对 batch=1

batch=4：

```text
Throughput -1.62%
Wall +1.64%
Write Syscalls -11.27%
```

batch=8：

```text
Throughput -6.09%
Wall +7.53%
Write Syscalls -12.62%
```

batch=16：

```text
Throughput -1.08%
Wall +1.10%
Write Syscalls -11.63%
```

---

## 9.4 为什么 Runtime 快 3～4 倍，但 E2E 没变快

Runtime benchmark 测的是：

```text
SQLite runtime-state write path
```

而完整 E2E 包含：

```text
scan
+ source read
+ io_uring / copy
+ BLAKE3
+ target write
+ fdatasync
+ reopen
+ verify read
+ verify hash
+ rename / directory operations
+ SQLite state transition
```

V6B 只明显加速其中：

```text
SQLite runtime-state write
```

如果 SQLite 状态写只占总时间很小一部分，那么即使它本身快 4 倍：

```text
整体耗时也不会快 4 倍
```

这就是 Amdahl 定律。

因此：

> V6B 是 DB 写路径优化，不是整个 1 GiB 文件迁移数据面的 4 倍优化。

---

## 9.5 E2E 的正确结论

不能说：

```text
V6B 让整个迁移快了 4 倍
```

也不能因为 E2E 没变快就说：

```text
V6B 优化失败
```

正确表述：

> V6B 在高并发 runtime-state 写入中将吞吐提升约 3～4 倍，并显著降低事务组、系统调用和 WAL 写入成本；但在 1 GiB 完整迁移链路中，copy、hash、verify 与持久化文件 I/O 占据主要成本，因此 DB batching 对总耗时影响有限。

---

# 10. perf stat

测试对比：

```text
4 Worker
1000 commands
batch=1 vs batch=4
```

## 10.1 batch=1

```text
task-clock       520.59 ms
CPUs utilized      0.332
context-switches    3997
cpu-migrations        45
page-faults          442
elapsed             1.5687 s
user                0.0544 s
sys                 0.5462 s
```

---

## 10.2 batch=4

```text
task-clock       218.94 ms
CPUs utilized      0.423
context-switches    2012
cpu-migrations        53
page-faults          440
elapsed             0.5178 s
user                0.0472 s
sys                 0.2047 s
```

---

## 10.3 对比

| 指标 | batch=1 | batch=4 | 变化 |
|---|---:|---:|---:|
| Elapsed | 1.569 s | 0.518 s | -67.0% |
| Task-clock | 520.59 ms | 218.94 ms | -58.0% |
| Context switches | 3997 | 2012 | -49.7% |
| System CPU | 0.546 s | 0.205 s | -62.5% |
| User CPU | 0.054 s | 0.047 s | -13.2% |
| Page faults | 442 | 440 | 基本不变 |

这里非常关键：

```text
user CPU 变化不大
system CPU 大幅下降
```

说明 V6B 主要减少的是：

```text
SQLite transaction
filesystem / WAL
fsync
lock
thread synchronization
系统调用
```

而不是减少应用层计算。

---

## 10.4 硬件 PMU 限制

当前 VMware 环境无法正常提供：

```text
cycles
instructions
branches
cache-misses
```

因此不能可靠计算：

```text
IPC
branch miss rate
cache miss rate
```

报告应如实记录：

> 当前虚拟机未暴露可用硬件 PMU，因此本轮 perf 分析使用 software `cpu-clock` 与系统调用/调度指标，不对 IPC 和缓存事件作结论。

---

# 11. perf CPU Hotspot

## 11.1 batch=1

主要调用链：

```text
RuntimeDbWriter::MarkCommitIntent
→ TaskRuntimeRepository
→ SQLite VDBE / BTree
→ WAL frames
→ unixWrite / pwrite
→ commit
→ unixSync
→ fsync
→ kernel storage path
```

`fsync`：

```text
Children ≈ 72.49%
```

正确理解是：

> 约 72% 的采样调用链落在 `fsync` 路径之下。

不能说：

> `fsync` 精确消耗了 72% 的全部 CPU 时间。

---

## 11.2 batch=4

可以直接看到 V6B 新路径：

```text
RuntimeDbWriter::ExecuteBatchedOperation
TaskRuntimeRepository::BeginWriteBatch
```

主要热点：

```text
fsync Children ≈ 62.43%
syscall Children ≈ 21.91%
__libc_pwrite ≈ 2.71%
```

总 `cpu-clock` 事件数量相对 batch=1 大约下降一半。

---

## 11.3 CPU Hotspot 结论

V6B 后：

```text
fsync 仍然是主要剩余热点
```

但：

```text
总体 task-clock
system CPU
wall time
transaction 数
fsync 次数
```

都已经显著下降。

因此正确结论：

> V6B 没有“消灭 fsync”，而是通过共享事务减少触发 durable commit / fsync 的频率。

---

# 12. strace 系统调用分析

## 12.1 batch=1

`strace -f -c`：

```text
futex       3057 calls    11.690883 s cumulative
fsync       1011 calls     0.412707 s
pwrite64    8454 calls     0.406407 s
fcntl       6062 calls     0.307770 s

total      18931 calls
```

---

## 12.2 batch=4

```text
futex       3032 calls     4.878331 s cumulative
pwrite64    2700 calls     0.158111 s
fsync        281 calls     0.117028 s
fcntl       1672 calls     0.079930 s

total       8119 calls
```

---

## 12.3 batch=1 → batch=4

| Syscall | batch=1 | batch=4 | 下降 |
|---|---:|---:|---:|
| fsync | 1011 | 281 | 72.2% |
| pwrite64 | 8454 | 2700 | 68.1% |
| fcntl | 6062 | 1672 | 72.4% |
| total syscalls | 18931 | 8119 | 57.1% |
| futex calls | 3057 | 3032 | 基本不变 |
| futex cumulative time | 11.69 s | 4.88 s | 58.3% |

这是 V6B 最直接的因果证据之一。

---

## 12.4 为什么 futex 次数没有明显减少

Worker 的同步模型没有改变：

```text
提交 command
→ future.get()
→ 等 ACK
```

所以每条 command 仍然会产生等待/唤醒。

V6B 优化的是：

```text
一次 ACK 要等多久
```

而不是：

```text
每条 command 是否需要 ACK
```

因此出现：

```text
futex call count ≈ 不变
futex cumulative waiting time ↓
```

非常符合设计预期。

---

## 12.5 关于 strace 时间的注意事项

`strace -f -c` 中多线程 syscall time 是：

```text
所有线程 syscall 时间累计
```

不同线程可能同时等待。

所以：

```text
11.69 s futex
```

不能理解为：

```text
程序 wall time = 11.69 s
```

系统调用**次数**比 strace 中的绝对时间更适合作为本轮比较证据。

---

# 13. fcntl / SQLite WAL 锁证据

完整 trace 中可以看到：

```text
F_SETLK
F_WRLCK
F_RDLCK
F_UNLCK
```

例如针对 SQLite WAL shared-memory 锁区的加锁与释放。

独立提取结果：

```text
batch=1:
约 6060 条 F_SETLK/F_GETLK

batch=4:
约 1712 条 F_SETLK/F_GETLK
```

下降约：

```text
71.7%
```

batch=4 中：

```text
F_SETLK = 1711
F_GETLK = 1
```

没有观察到：

```text
fcntl(...)= -1 EAGAIN
fcntl(...)= -1 EACCES
```

因此正确结论不是：

```text
V6B 解决了大量锁冲突
```

而是：

> V6A 已经通过单 Dedicated Writer 消除了主要的多写者锁竞争；V6B 进一步通过事务合批减少 WAL 锁获取/释放操作频率。

---

# 14. SQLite busy sleep 证据

对：

```text
runtime-batch1-full.txt
runtime-batch4-full.txt
```

检索：

```text
nanosleep
clock_nanosleep
```

结果：

```text
0 次
```

因此在本次 isolated Runtime Writer benchmark 中：

> 没有观察到 SQLite busy timeout 触发 sleep 重试。

这说明 V6B 的收益不是：

```text
减少 SQLITE_BUSY 后的睡眠
```

而是：

```text
减少 transaction / WAL / fsync / fcntl 固定成本
```

注意不能扩大为：

```text
PhotoBridge 永远不会发生 SQLITE_BUSY
```

这里只能描述本次测试观测。

---

# 15. futex 调用栈

由于当前系统没有暴露可用的 futex perf tracepoint，因此使用：

```bash
strace -f -k -tt -T -e trace=futex
```

获取用户态调用栈。

---

## 15.1 Writer 等待队列

可以看到：

```text
pthread_cond_wait
→ RuntimeDbWriter::Run()
```

说明：

```text
Writer 无任务
→ condition_variable 睡眠
→ Worker Enqueue
→ condition_variable signal
→ Writer 被唤醒
```

---

## 15.2 Worker 等待 durable ACK

关键调用栈：

```text
futex(FUTEX_WAIT...)
→ std::__atomic_futex_unsigned_base::_M_futex_wait_until
→ RuntimeDbWriter::MarkCommitIntent()
→ benchmark worker
```

说明：

```text
Worker
→ Enqueue CommitIntent
→ future.get()
→ futex wait
```

然后 Writer：

```text
Execute / ExecuteBatch
→ Complete
→ promise set_value
→ FUTEX_WAKE
→ Worker 返回
```

这直接证明：

> V6B 没有取消同步 durable ACK。

---

## 15.3 batch=4 特有证据

batch=4 trace 中可看到：

```text
RuntimeDbWriter::ExecuteBatch
→ RuntimeDbWriter::Complete
→ FUTEX_WAKE
```

这是 V6B 实际执行 batch 路径的直接调用栈证据。

---

## 15.4 MarkCommitIntent 等待时间

两份 `strace -k` 在相同 4 Worker / 1000 commands 条件下：

| 等待指标 | batch=1 | batch=4 | 变化 |
|---|---:|---:|---:|
| Mean | 63.07 ms | 43.04 ms | -31.8% |
| Median | 62.98 ms | 42.07 ms | -33.2% |
| P95 | 83.33 ms | 57.72 ms | -30.7% |
| P99 | 95.74 ms | 76.37 ms | -20.2% |
| Max | 109.35 ms | 95.74 ms | -12.4% |

注意：

```text
strace -k
```

本身有非常明显的 tracing overhead。

因此这些值不能当作正常 Release ACK 延迟。

它们最适合用于：

```text
相同 tracing 条件下的 batch=1 / batch=4 相对比较
```

---

# 16. Debug 正确性验证

命令：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

结果：

```text
100% tests passed
0 tests failed out of 230
Total Test time = 26.72 sec
```

V6B 关键测试包括：

```text
RuntimeDbWriterTest.AckFollowsCommitAndRepositoryErrorsPropagate
RuntimeDbWriterTest.MultiThreadSubmitDrainsEveryCommand
RuntimeDbWriterTest.ReceiptAndRetryUseTheWriterConnection
RuntimeDbWriterTest.StopDrainsAcceptedCommandsAndWakesAllCallers
RuntimeDbWriterTest.OpportunisticallyBatchesQueuedIndependentTasks
RuntimeDbWriterTest.FailedCommandRollsBackItsEntireBatch
```

---

# 17. Sanitizer 验证

配置：

```text
CMAKE_BUILD_TYPE=Debug
PHOTOBRIDGE_ENABLE_SANITIZERS=ON
```

命令：

```bash
cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized
```

结果：

```text
100% tests passed
0 tests failed out of 230
Total Test time = 91.89 sec
```

没有观察到 sanitizer 报错。

Sanitizer 时间明显高于普通 Debug：

```text
Debug      26.72 s
Sanitized  91.89 s
```

这是 instrumentation 开销，不能作为性能对比。

---

# 18. 关键 Benchmark 命令与参数解释

## 18.1 `runtime-state-compare`

```bash
./build/bench-release/photobridge_bench \
  --workload runtime-state-compare \
  --repetitions 30 \
  --runtime-commands 1000 \
  --runtime-workers 8 \
  --output result.json
```

参数：

### `--workload runtime-state-compare`

运行 Runtime DB Writer 对比测试。

通常比较：

```text
batch=1
batch=4
batch=8
batch=16
```

其中：

```text
batch=1
```

作为 V6A-like control。

---

### `--repetitions 30`

同一配置重复 30 次。

作用：

```text
降低一次运行调度波动
提高平均值 / percentile 的可靠性
```

Microbenchmark 比 E2E 成本低，因此使用更大的 repetitions。

---

### `--runtime-commands 1000`

每次 Runtime benchmark 提交的 command 总数。

---

### `--runtime-workers N`

并发提交线程数。

它不等于 SQLite writer 数。

V6A/V6B 中数据库真正写入仍由：

```text
1 个 RuntimeDbWriter thread
```

完成。

---

### `--runtime-db-batch-size N`

设置 Writer 一次最多合并多少已经排队的 command。

它是：

```text
maximum batch capacity
```

不是：

```text
保证每次都能形成 N 条 batch
```

实际 batch 受队列积累和 submitter 数限制。

---

### `--output FILE`

把 benchmark 原始结果保存成 JSON。

正式分析应优先依赖原始 JSON，而不是手工抄终端输出。

---

# 19. E2E 命令解释

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-v6b-compare \
  --warmup-repetitions 1 \
  --repetitions 10 \
  --e2e-workers 4 \
  --output e2e-v6b-compare-10.json
```

## `e2e-v6b-compare`

自动比较：

```text
batch=1
batch=4
batch=8
batch=16
```

并使用正式 V6B E2E workload。

当前正式固定规模：

```text
64 × 16 MiB = 1 GiB
```

---

## `--warmup-repetitions 1`

正式测量前先运行一次 warmup。

主要目的是减少：

```text
首次启动
程序初始化
动态库首次加载
page cache 状态差异
```

带来的偏差。

---

## `--e2e-workers 4`

完整迁移阶段使用 4 个 Worker。

---

# 20. perf 命令解释

## 20.1 `perf stat`

```bash
perf stat -d \
  -o perf-runtime-batch1.txt \
  -- \
  ./build/bench-release/photobridge_bench ...
```

### `perf stat`

统计程序运行过程中的整体性能计数。

### `-d`

请求更详细的一组默认性能指标。

但虚拟机不一定支持所有 hardware PMU counter。

### `-o FILE`

把 perf 统计写入文件。

### `--`

表示：

```text
perf 参数结束
后面的内容是被测程序
```

---

# 21. perf record / report 命令解释

```bash
perf record \
  -e cpu-clock \
  -g \
  -o perf-runtime-batch1.data \
  -- \
  ./build/bench-release/photobridge_bench ...
```

### `perf record`

对程序进行 sampling profiling。

### `-e cpu-clock`

使用软件 CPU clock 事件采样。

由于 VMware 中 hardware cycles 不可用，本轮选择它作为 CPU hotspot 事件。

### `-g`

记录调用栈。

没有 `-g` 时只能看到当前函数，很难解释完整路径。

### `.data`

保存原始 perf sample。

---

生成文本报告：

```bash
perf report \
  --stdio \
  -i perf-runtime-batch1.data \
  > perf-runtime-batch1-report.txt
```

### `--stdio`

用纯文本输出，而不是交互式 TUI。

适合：

```text
存档
Git diff
报告引用
```

### `-i`

指定 perf data 输入文件。

---

# 22. strace 命令解释

## 22.1 完整 trace

```bash
strace -f -tt -T \
  -o runtime-batch1-full.txt \
  ./build/bench-release/photobridge_bench ...
```

### `-f`

跟踪程序创建的线程 / 子进程。

Runtime benchmark 是多线程的，如果不加 `-f` 会漏掉 Writer 和 Worker 的 syscall。

### `-tt`

输出更高精度的绝对时间戳。

### `-T`

记录每个 syscall 的耗时。

### `-o`

输出到文件。

---

## 22.2 汇总 syscall 次数

```bash
strace -f -c \
  -o runtime-batch1-summary.txt \
  ./build/bench-release/photobridge_bench ...
```

### `-c`

不输出每条调用，而是统计：

```text
calls
errors
time
usecs/call
```

特别适合比较：

```text
fsync
pwrite64
fcntl
futex
```

调用数量。

---

# 23. `strace -k` 命令解释

```bash
strace -f -k -tt -T \
  -e trace=futex \
  -o runtime-batch4-futex-stack.txt \
  ./build/bench-release/photobridge_bench ...
```

### `-k`

为 syscall 记录用户态 stack trace。

### `-e trace=futex`

只跟踪 futex。

否则完整 syscall + stack 数据量会非常大。

这条命令用于回答：

> Worker 为什么在 futex 上等待？

最终能把：

```text
futex
→ std::future 内部
→ RuntimeDbWriter::MarkCommitIntent
```

关联起来。

---

# 24. grep 锁证据命令解释

```bash
grep -E 'fcntl\(.*(F_SETLK|F_GETLK)' runtime-batch1-full.txt
```

### `grep -E`

启用扩展正则表达式。

### `F_SETLK`

设置 advisory record lock。

SQLite WAL 使用文件锁协调访问。

### `F_GETLK`

查询冲突锁状态。

### `F_WRLCK`

写锁。

### `F_RDLCK`

读锁。

### `F_UNLCK`

释放锁。

---

# 25. SQLite busy sleep 检查

```bash
grep -En 'nanosleep|clock_nanosleep' \
  runtime-batch1-full.txt \
  runtime-batch4-full.txt
```

### `nanosleep / clock_nanosleep`

如果 SQLite busy handler / busy timeout 进入 sleep-retry 路径，通常可以在 syscall trace 中观察到这类睡眠系统调用。

本轮：

```text
0 hits
```

---

# 26. Debug / Sanitizer 命令解释

## Debug

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

### `cmake --preset debug`

按 `CMakePresets.json` 中 debug preset 配置工程。

### `cmake --build --preset debug`

使用对应 build preset 编译。

### `ctest --preset debug`

执行 Debug 测试集合。

---

## Sanitizer

```bash
cmake --preset sanitized
cmake --build --preset sanitized
ctest --preset sanitized
```

该 preset：

```text
CMAKE_BUILD_TYPE=Debug
PHOTOBRIDGE_ENABLE_SANITIZERS=ON
```

用于发现普通功能测试不容易发现的内存 / 未定义行为问题。

---

# 27. 面试时怎么讲 V6B

## 27.1 30 秒版本

> V6A 我先把多个 Worker 的 SQLite 状态写收敛到了一个 Dedicated DB Writer，解决多连接争抢 SQLite writer lock 的问题。但 profiling 发现 Writer 仍然是一条 command 一个事务，导致大量 WAL 写、fsync 和 fcntl。V6B 就是在不改变 durable ACK 语义的前提下，把 Writer 唤醒时已经排队的 command opportunistically 合并到一个事务里。8 Worker 下 Runtime 状态写吞吐大约提高到 3.9 倍，transaction groups 减少约 80%，write syscall 减少约 76%。不过 1 GiB 完整迁移没有明显加速，因为总瓶颈已经主要在 copy、hash、verify 和文件持久化 I/O。

---

## 27.2 为什么不直接异步 ACK？

回答：

> PhotoBridge 的状态写参与崩溃恢复语义。如果 Worker 在数据库事务真正持久化之前就拿到成功 ACK，进程此时崩溃，就可能出现业务已经继续执行但恢复状态没有落盘的问题。所以我保留 commit 后 ACK，只优化事务固定成本。

---

## 27.3 为什么不 sleep 1～5 ms 主动攒 batch？

回答：

> 主动等待确实可能提高平均 batch depth，但它会人为增加低负载时的 ACK 延迟。我希望优化对轻负载基本无额外成本，所以选择 opportunistic batching，只利用已经自然排队的并发请求。

---

## 27.4 为什么 batch=16 没有比 batch=8 快很多？

回答：

> 因为 Worker 是同步 submit + wait ACK，一个 Worker 同时最多贡献一条 in-flight command。8 Worker 下实际最大 batch 就是 8，所以把配置从 8 调到 16 并不会真的得到 16 条事务批次。

---

## 27.5 为什么 E2E 没变快？

回答：

> Runtime microbenchmark 只测试 DB 状态写，所以 V6B 能体现出 3～4 倍收益。但完整迁移还有 copy、BLAKE3、verify、fdatasync 等重 I/O 工作。SQLite 状态写占整体比例很小，所以根据 Amdahl 定律，优化局部热点不会自动转化成相同比例的端到端提升。E2E 中虽然总耗时基本不变，但 write syscall 仍下降约 11%～13%，证明 DB 写路径自身确实减少了系统工作。

---

## 27.6 perf 如何证明不是“偶然快了”？

回答：

> perf stat 看到 batch=4 相比 batch=1，task-clock 下降约 58%，system CPU 下降约 62%，context switch 减少约 50%；strace 又看到 fsync 从 1011 次下降到 281 次，pwrite64 从 8454 次下降到 2700 次，fcntl 从 6062 次下降到 1672 次。也就是说吞吐提升和事务、同步、WAL 写系统调用减少是同时发生的，因果链比较完整。

---

# 28. 本轮不能过度宣称的内容

不能说：

```text
V6B 让 PhotoBridge 整体快 4 倍
```

应该说：

```text
Runtime DB state-write workload 提升约 3～4 倍
```

不能说：

```text
batch=16 是最佳 batch size
```

应该说：

```text
8 Worker 下实际 batch depth 被并发提交者限制在 8 左右
```

不能说：

```text
V6B 解决了 SQLite busy 问题
```

应该说：

```text
本轮测试未观察到 busy sleep；V6B 主要减少事务/WAL/sync 固定成本
```

不能说：

```text
所有 fcntl 都是锁竞争
```

应该说：

```text
完整 trace 中确认存在 SQLite WAL F_SETLK/F_WRLCK/RDLCK/UNLCK 操作，并且 V6B 后锁相关调用数量明显下降
```

不能说：

```text
fsync 占精确 72% CPU
```

应该说：

```text
约 72% 的 perf sample 调用链落在 fsync 路径下
```

---

# 29. 本轮证据链

V6B 的证据不是只有一个吞吐数字，而是：

```text
代码设计
  ↓
Transaction Groups 显著下降
  ↓
pwrite64 显著下降
  ↓
fsync 显著下降
  ↓
fcntl 显著下降
  ↓
system CPU 显著下降
  ↓
context switch 显著下降
  ↓
ACK latency 显著下降
  ↓
Runtime throughput 显著提高
```

同时：

```text
futex 调用栈
```

证明同步 durable ACK 语义仍然存在。

再通过：

```text
Debug 230/230
Sanitizer 230/230
```

证明优化没有以牺牲已有正确性测试为代价。

---

# 30. 最终结论

V6B 达到了它真正应该达到的目标：

```text
不是：
把整个照片迁移链路强行提速几倍

而是：
降低 Dedicated DB Writer 中大量小事务的固定成本
```

正式数据说明：

```text
8 Worker / batch=8
Runtime throughput ≈ 3.94×
ACK latency ≈ -74%
Transaction groups ≈ -80%
Write syscalls ≈ -76%
Kernel write bytes ≈ -76%
```

perf / strace 又进一步验证：

```text
fsync ↓
pwrite64 ↓
fcntl ↓
system CPU ↓
context switch ↓
```

而完整 1 GiB E2E 没有明显提速，则说明系统下一阶段若继续做性能优化，应该把注意力重新放回：

```text
copy
hash
verify
fdatasync
storage path
```

而不是继续围绕 SQLite batch size 微调。

因此 V6B 最合适的工程定位是：

> **在保持 commit-after-ACK 崩溃恢复语义不变的前提下，通过 Opportunistic Transaction Batching 将 Dedicated DB Writer 从“一命令一事务”改造成“已排队命令共享事务”，显著降低 SQLite/WAL/持久化系统调用开销，并通过 Runtime benchmark、1/2/4/8 Worker 扩展性、perf、strace、fcntl、futex、Debug 与 Sanitizer 建立完整证据链。**

---

# 31. 结果文件目录

```text
benchmark-results/v6b-opportunistic-batching/
├── artifacts.txt
├── environment.txt
├── git-state.txt
├── runtime-state-smoke.json
├── runtime-state-1w.json
├── runtime-state-2w.json
├── runtime-state-4w.json
├── runtime-state-30.json
├── e2e-smoke.json
├── e2e-v6b-compare-smoke.json
├── e2e-v6b-compare-10.json
├── perf/
│   ├── perf-runtime-batch1.txt
│   ├── perf-runtime-batch1.data
│   ├── perf-runtime-batch1-report.txt
│   ├── perf-runtime-batch4.txt
│   ├── perf-runtime-batch4.data
│   └── perf-runtime-batch4-report.txt
├── strace/
│   ├── runtime-batch1-summary.txt
│   ├── runtime-batch1-full.txt
│   ├── runtime-batch1-fcntl.txt
│   ├── runtime-batch1-futex-stack.txt
│   ├── runtime-batch4-summary.txt
│   ├── runtime-batch4-full.txt
│   ├── runtime-batch4-fcntl.txt
│   ├── runtime-batch4-futex-stack.txt
│   └── sqlite-busy-sleep.txt
└── validation/
    ├── debug.txt
    └── sanitized.txt
```

---

# 32. 建议的 Git 收口

当前 V6B 代码已经在：

```text
7fe956f V6B
```

但测试结果目录未提交，而且没有 V6B tag。

建议最终确认报告后：

```bash
git add benchmark-results/v6b-opportunistic-batching
git add <V6B报告文件>

git commit -m "Add V6B opportunistic batching benchmark report"

git tag -a v6b-opportunistic-db-batching \
  -m "V6B opportunistic DB batching validated"

git push origin main
git push origin v6b-opportunistic-db-batching
```

如果希望 tag 精确标记“代码改完但 benchmark/report 尚未提交”的 `7fe956f`，则应显式：

```bash
git tag -a v6b-opportunistic-db-batching 7fe956f \
  -m "V6B opportunistic DB batching"
```

二者语义不同，应根据你希望 tag 标记“代码版本”还是“代码 + 验证材料版本”来选择。
