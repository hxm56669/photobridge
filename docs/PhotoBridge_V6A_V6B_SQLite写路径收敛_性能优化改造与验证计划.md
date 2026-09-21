# PhotoBridge V6：SQLite 写路径收敛性能优化改造与验证计划

> 项目：PhotoBridge  
> 阶段：V6  
> 主题：SQLite 写路径收敛  
> 目标：解决 Multi-worker 场景下多个 SQLite 连接竞争单 Writer Lock 的问题  
> 规划方式：分为 **V6-A Dedicated DB Writer** 与 **V6-B Transaction Convergence / Batching** 两阶段  
>
> 核心原则：
>
> ```text
> 先减少“谁在写”
> 再减少“写多少次”
> ```
>
> 不同时修改：
>
> ```text
> busy_timeout
> synchronous=FULL
> WAL
> crash-safe 状态机
> receipt 顺序
> recovery 语义
> ```
>
> 这样才能保证每一阶段的性能变化都有清晰因果。

---

# 1. V6 的来源

V5 完成 io_uring 优化后，PhotoBridge 的局部 Copy+Hash 数据面已经明显加速。

V5 的主要结论：

```text
64 MiB Copy+Hash Microbenchmark
QD4 相比同步：
吞吐约 +26.5%

完整 1 GiB / 4 Worker E2E：
吞吐约 +2.5%
```

这说明：

```text
Copy 已经被优化
但完整主链仍然受其他步骤限制
```

继续用 `perf`、`strace` 和调用栈分析后，发现完整 E2E 中：

```text
clock_nanosleep ≈ 38%
```

通过：

```bash
strace -f -k -e trace=clock_nanosleep
```

确认调用栈：

```text
clock_nanosleep
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

实际命中的业务路径包括：

```text
ClaimNextReadyImpl
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
```

因此 V6 的出发点不是猜测，而是：

> **V5 profiling 已经证明 SQLite Writer Lock 竞争成为新的重要瓶颈。**

---

# 2. 当前 SQLite 并发模型

当前 MigrationService 的并发结构大致是：

```text
Producer
├─ 主 SQLite Connection
└─ ClaimNextReady()

Worker 1
├─ SQLite Connection 1
└─ TaskRuntimeRepository 1

Worker 2
├─ SQLite Connection 2
└─ TaskRuntimeRepository 2

Worker 3
├─ SQLite Connection 3
└─ TaskRuntimeRepository 3

Worker 4
├─ SQLite Connection 4
└─ TaskRuntimeRepository 4
```

并发阶段中：

```text
Producer
→ ClaimNextReady

Workers
→ MarkCommitIntent
→ MarkTempWritten
→ PersistVerifiedReceipt
→ MarkRetryable
→ 其他 attempt/task 状态推进
```

这些操作内部大量采用：

```sql
BEGIN IMMEDIATE;
...
COMMIT;
```

SQLite 当前配置：

```text
journal_mode = WAL
synchronous = FULL
foreign_keys = ON
busy_timeout = 5000 ms
```

WAL 允许 Reader 与 Writer 更好并发，但：

```text
SQLite 同一时刻仍然只有一个 Writer
```

所以真实运行模型是：

```text
多个 Connection
↓
同时 BEGIN IMMEDIATE
↓
争抢唯一 Writer Lock
↓
只有一个成功
↓
其他连接进入 busy handler
↓
clock_nanosleep
↓
醒来重试
```

---

# 3. V6 总体目标

V6 不直接修改 SQLite 配置。

不做：

```text
busy_timeout 5000 → 0
FULL → NORMAL
WAL → 其他模式
```

也不通过删除 crash-safe checkpoint 获得假性能。

V6 目标分为两个阶段：

```text
V6-A
Dedicated DB Writer
→ 先减少“谁在写”

V6-B
Transaction Convergence / Batching
→ 再减少“写多少次”
```

---

# 4. V6-A：Dedicated DB Writer

## 4.1 核心思想

当前：

```text
Worker 1 → SQLite
Worker 2 → SQLite
Worker 3 → SQLite
Worker 4 → SQLite
Producer → SQLite
```

改成：

```text
                   Producer
                      │
             ┌────────┴────────┐
             │                 │
          Worker 1          Worker 2
          Worker 3          Worker 4
             │                 │
             └────────┬────────┘
                      ↓
               DB Command Queue
                      ↓
            Dedicated DB Writer
                      ↓
              1 SQLite Connection
                      ↓
                   SQLite
```

核心不是让 SQLite 多 Writer 并发。

恰恰相反：

> **既然 SQLite 最终只能有一个 Writer，就让应用层主动只有一个 Writer。**

---

## 4.2 为什么可能更快

当前多连接模型：

```text
A / B / C / D 同时抢 Writer Lock
↓
只有 A 成功
↓
B/C/D SQLITE_BUSY
↓
busy handler
↓
sleep
↓
wake
↓
再次竞争
```

Dedicated Writer：

```text
A → Queue
B → Queue
C → Queue
D → Queue

Writer：
A → COMMIT
B → COMMIT
C → COMMIT
D → COMMIT
```

SQLite 本来就必须串行写。

V6-A 将：

```text
数据库内部锁竞争
```

替换为：

```text
应用层显式队列
```

理论收益：

```text
busy handler ↓
clock_nanosleep ↓
无效锁竞争 ↓
线程反复 sleep/wakeup ↓
context switch ↓
SQLite 多连接协调开销 ↓
```

---

## 4.3 V6-A 不做什么

必须明确：

```text
不改变 SQL
不删除 event
不减少状态 checkpoint
不改变 BEGIN IMMEDIATE
不修改 FULL
不修改 WAL
不修改 busy_timeout
不进行事务 batching
```

唯一核心变量：

```text
多 Writer Connection
→ 单 Dedicated Writer Connection
```

---

# 5. V6-A 推荐代码结构

建议新增：

```text
include/photobridge/app/migration_runtime_store.h
include/photobridge/app/runtime_db_writer.h
src/app/runtime_db_writer.cpp
```

---

# 6. MigrationRuntimeStore

当前迁移主链直接依赖：

```text
TaskRuntimeRepository
```

建议增加只包含迁移阶段必要接口的最小抽象：

```cpp
class MigrationRuntimeStore {
public:
    virtual StatusOr<ClaimedTask> ClaimNextReady(...) = 0;
    virtual Status MarkCommitIntent(...) = 0;
    virtual Status MarkTempWritten(...) = 0;
    virtual Status PersistVerifiedReceipt(...) = 0;
    virtual Status MarkRetryable(...) = 0;
    virtual ~MigrationRuntimeStore() = default;
};
```

意义：

```text
MigrationAttemptPreparer / MigrationService
不再关心：
“背后是不是 SQLite”

只关心：
“状态操作成功了吗”
```

---

# 7. TaskRuntimeRepository 的处理

不要重写整个 Repository。

建议：

```text
TaskRuntimeRepository
→ 实现 MigrationRuntimeStore
```

原有：

```text
Recovery
ReadRuntime
AddTask
Materialize
各种查询
```

继续保留。

V6-A 只把迁移阶段并发写路径抽出来。

---

# 8. RuntimeDbWriter

新增：

```text
RuntimeDbWriter
```

内部拥有：

```text
1 SqliteConnection
1 TaskRuntimeRepository
1 Writer Thread
1 DB Command Queue
1 mutex
1 condition_variable
```

逻辑：

```text
Submit Command
↓
push queue
↓
notify writer
↓
writer pop
↓
TaskRuntimeRepository 执行
↓
COMMIT
↓
返回结果
```

---

# 9. Command 类型

建议显式定义：

```text
ClaimNextReadyCommand
MarkCommitIntentCommand
MarkTempWrittenCommand
PersistVerifiedReceiptCommand
MarkRetryableCommand
```

可以用：

```cpp
std::variant<
    ClaimNextReadyCommand,
    MarkCommitIntentCommand,
    MarkTempWrittenCommand,
    PersistVerifiedReceiptCommand,
    MarkRetryableCommand
>
```

Writer 使用：

```cpp
std::visit(...);
```

不建议一开始设计：

```text
任意 std::function
通用 RPC framework
复杂泛型执行器
```

因为 V6 的重点是数据库写路径，不是造框架。

---

# 10. Command Result

Worker 提交 command 后不能直接继续。

必须：

```text
Submit
↓
等待 DB Writer 执行
↓
等待 COMMIT
↓
收到 Status / StatusOr
↓
继续业务流程
```

可以使用：

```text
std::promise
std::future
```

或者项目内部自己的 completion object。

---

# 11. 为什么不能 fire-and-forget

例如现在：

```text
Copy
↓
fdatasync(temp)
↓
MarkTempWritten
↓
Verify
```

V6-A 不能改成：

```text
fdatasync
↓
把 MarkTempWritten 丢队列
↓
不等
↓
直接 Verify
```

必须保持：

```text
fdatasync
↓
Submit MarkTempWritten
↓
Writer COMMIT
↓
ACK
↓
Verify
```

否则：

```text
文件系统状态
与
数据库状态
```

的 crash-safe 顺序会改变。

---

# 12. 哪些操作必须进入 Dedicated Writer

并发 migrate 开始以后，只要是写数据库，都必须通过 Dedicated Writer。

至少：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
任务成功状态推进
错误状态推进
```

`ClaimNextReady` 也必须进入 Writer。

否则：

```text
Worker → Dedicated Writer
Producer → 原 main Connection
```

最终仍然有：

```text
2 个并发 Writer
```

只能把：

```text
5 Writer
→ 2 Writer
```

没有彻底解决问题。

---

# 13. 哪些暂时不用改

迁移并发真正启动前的初始化：

```text
EnsureSchema
MaterializePlan
ReadTaskStateCounts
SetReady
AcquireNextExecutionEpoch
ReadManifestSourceRoot
```

可以继续由主连接执行。

结构：

```text
初始化阶段
↓
Main Connection

进入并发 migrate
↓
启动 DB Writer

并发阶段写操作
↓
全部 Dedicated Writer
```

RecoveryService 这轮也不要一起重构。

---

# 14. MigrationService 的主要变化

当前：

```text
main connection
+
worker connection 1
+
worker connection 2
+
worker connection 3
+
worker connection 4
```

V6-A 后：

```text
bootstrap/main connection
+
dedicated writer connection
```

并发写：

```text
只有 writer connection
```

原来的：

```text
std::vector<SqliteConnection> worker_connections
```

应该逐步删除。

Worker 不再创建自己的：

```text
TaskRuntimeRepository
```

而是共享：

```text
RuntimeDbWriter
```

---

# 15. 两条 Queue 必须分离

当前已有：

```text
Task Queue
```

负责：

```text
Producer
→ QueuedTask
→ Worker
```

V6-A 新增：

```text
DB Command Queue
```

负责：

```text
Producer / Worker
→ DB Command
→ DB Writer
```

所以：

```text
Task Queue
≠
DB Queue
```

不要复用同一个 queue/mutex。

---

# 16. Queue 是否需要 bounded

建议初版可以 bounded，例如：

```text
capacity = 32 / 64
```

作用：

```text
防止异常情况下 DB command 无限堆积
```

但由于 Worker 每次提交后都等待 ACK：

```text
每个 Worker 同时最多通常只有少量未完成 command
```

正常情况下 queue 不会无限增长。

因此：

> **V6-A 不要把时间浪费在调 DB Queue capacity 上。**

---

# 17. Writer 生命周期

推荐：

```text
初始化数据库
↓
Start RuntimeDbWriter
↓
Start Producer / Worker
↓
停止产生新任务
↓
等待 Worker 全部完成
↓
DB Queue drain
↓
Stop Writer
↓
Join Writer Thread
```

---

# 18. Error / Shutdown 规则

必须防止：

```text
Worker future.get()
↓
Writer 已提前退出
↓
永久阻塞
```

Writer Stop 应满足：

```text
停止接收新 command
但处理已有 command
↓
所有 promise 都必须完成
↓
再退出线程
```

如果 Writer 本身出现 fatal error：

```text
记录 first_error
↓
后续 command 返回 fatal status
↓
唤醒所有等待者
```

不能悄悄丢 command。

---

# 19. 锁顺序要求

绝对不要：

```text
持有 Task Queue mutex
↓
等待 DB Future
```

正确：

```text
从 Task Queue 取任务
↓
释放 Task Queue mutex
↓
执行文件操作
↓
提交 DB Command
↓
等待 ACK
```

DB Writer 内也不要反向获取：

```text
Task Queue mutex
```

目标：

```text
Task Queue Lock
与
DB Queue Lock
无交叉依赖
```

---

# 20. V6-A 测试要求

V6-A 改完首先跑：

```text
Unit Tests
Integration Tests
Sanitizer Tests
Crash / Recovery Tests
```

重点验证：

```text
状态顺序没有改变
每个 ACK 都发生在 COMMIT 之后
错误能返回原 Worker
Writer shutdown 不死锁
多 Worker 不丢 Command
```

---

# 21. V6-A 必须增加的单元测试

建议至少：

```text
1. SingleCommand
2. MultiThreadSubmit
3. ResultPropagation
4. RepositoryErrorPropagation
5. OrderedAck
6. StopAfterDrain
7. StopWithPendingCommands
8. WriterFatalErrorWakesAllWaiters
```

---

# 22. MultiThreadSubmit 测试

例如：

```text
8 threads
×
100 commands
```

同时提交。

验证：

```text
所有 command 都完成
没有 future 永久等待
最终数据库状态一致
Writer Thread 没有数据竞争
```

---

# 23. OrderedAck 测试

验证：

```text
Submit MarkTempWritten
↓
只有真正 COMMIT 后
↓
future 才 ready
```

避免实现成：

```text
Command 入队
↓
立即 promise.set_value()
```

这是错误的。

---

# 24. V6-A Benchmark 核心假设

## 假设 1

```text
Worker = 1
```

Dedicated Writer：

```text
收益可能很小
甚至略慢
```

因为：

```text
没有多 Writer 竞争
却增加 Queue + Future
```

这是合理结果。

## 假设 2

```text
Worker = 2 / 4 / 8
```

随着并发增加：

```text
V5 多连接 busy contention
```

越来越严重。

V6-A 应该开始占优。

## 假设 3

V6-A 后：

```text
sqliteDefaultBusyCallback
clock_nanosleep
```

应该显著下降。

## 假设 4

`futex` 不一定下降。

因为等待机制可能从：

```text
SQLite busy sleep
```

变成：

```text
condition_variable / future
```

真正要看：

```text
总 wall time
CPU
context switch
E2E throughput
```

---

# 25. V6-A Benchmark 基准

保持和 V5 一致：

```text
workload：e2e-large
64 files
16 MiB / file
total：1 GiB
```

Workers：

```text
1
2
4
8
```

---

# 26. 正式核心对照

最重要：

```text
Worker = 4
```

流程：

```text
1 次 warmup
+
10 次正式
```

对照：

```text
V5
Multi-Connection Writer

vs

V6-A
Dedicated DB Writer
```

必须保持：

```text
同机器
同文件系统
同 Release
同 workload
同文件规模
同 Worker
同 io_uring QD
同 SQLite PRAGMA
```

---

# 27. V6-A 需要记录的指标

每组：

```text
Mean wall
Median wall
P95 / P99
Throughput
CPU time
CPU utilization
RSS
read/write bytes
syscall count
```

以及：

```text
perf stat
strace -f -c
strace -f -k clock_nanosleep
```

---

# 28. V6-A perf stat

命令：

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v6-a-db-writer/perf/e2e-large-4w-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 4
```

重点：

```text
task-clock
context-switches
cpu-migrations
page-faults
```

---

# 29. V6-A strace

```bash
strace -f -c \
  -o benchmark-results/v6-a-db-writer/strace/e2e-large-4w-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

重点比较 V5：

```text
clock_nanosleep
futex
fsync
fdatasync
pread64
pwrite64
write
read
```

---

# 30. SQLite busy stack 验证

V6-A 必须重新执行：

```bash
strace -f -k \
  -e trace=clock_nanosleep \
  -o benchmark-results/v6-a-db-writer/strace/nanosleep-stack.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

核心目标：

```text
V5：
大量
sqliteDefaultBusyCallback
→ clock_nanosleep

V6-A：
明显减少
或基本消失
```

---

# 31. V6-A 验收标准

## 正确性

```text
全部原有测试通过
Crash-safe 顺序不变
Recovery 行为不变
Receipt 顺序不变
Worker 无死锁
Writer 无丢 Command
```

## 架构

```text
并发 migrate 阶段只有 1 条 SQLite Writer Connection
Worker 不再直接执行 SQLite 写事务
Producer Claim 也通过 Writer
```

## 性能证据

至少证明：

```text
SQLite busy backoff 显著下降
```

最好同时：

```text
E2E wall time 改善
或
高 Worker scalability 改善
```

---

# 32. 什么情况下 V6-A 也算成功

即使：

```text
4 Worker E2E 只提升很小
```

但如果：

```text
clock_nanosleep / SQLite busy 明显下降
```

仍说明架构目标完成。

这可能意味着新的瓶颈已经转移到：

```text
fdatasync
fsync
Verify
SQLite COMMIT 本身
```

这仍然是有价值的性能工程结果。

---

# 33. 什么情况下应该回滚 V6-A

如果：

```text
Worker 1/2/4/8 全部稳定退化
```

而且：

```text
busy backoff 减少
但 Queue/Future 开销更大
```

说明 Dedicated Writer 在当前 workload 不值得。

这时：

```text
保留实验分支
不进入主线
```

不能因为架构看起来高级就强行保留。

---

# 34. V6-B：Transaction Convergence / Batching

V6-A 完成后：

```text
多 Writer Lock 竞争
```

应该显著减少。

此时重新 profiling。

如果剩余热点集中在：

```text
BEGIN / COMMIT
fsync
WAL sync
SQLite transaction
```

才进入 V6-B。

---

# 35. V6-B 核心目标

V6-A：

```text
减少写者数量
```

V6-B：

```text
减少事务数量
缩短事务持有时间
减少重复 SQL
减少 COMMIT 次数
```

---

# 36. V6-B1：缩短事务

例如当前某些事务：

```text
BEGIN IMMEDIATE
↓
Read epoch
↓
Check ownership
↓
Read state
↓
UPDATE
↓
INSERT event
↓
COMMIT
```

需要审查：

```text
哪些查询必须在 Writer Lock 持有期间？
```

如果某些只读信息：

```text
可以安全移到 BEGIN 前
```

则可以缩短：

```text
BEGIN IMMEDIATE
到
COMMIT
```

之间的窗口。

但必须确认：

```text
移出去以后数据是否可能失效
```

不能破坏原子性。

---

# 37. V6-B1：减少重复查询

Dedicated Writer 后：

```text
所有 command 已经单线程顺序执行
```

这可能允许更安全地复用：

```text
Prepared Statement
当前 epoch
部分只读元信息
```

优先：

```text
Prepared Statement 重用
减少重复 SELECT
```

再考虑：

```text
状态缓存
```

不建议一开始做完整内存状态镜像。

---

# 38. V6-B2：Writer Batching

Writer 从：

```text
一次 pop 1 个 command
```

演进到：

```text
一次取多个已经排队的安全 command
```

例如：

```text
A
B
C
D
```

如果这些 command：

```text
属于不同 Task
彼此没有依赖
允许共享事务
```

则可能：

```text
BEGIN
→ A
→ B
→ C
→ D
COMMIT
```

减少：

```text
COMMIT 次数
WAL sync 次数
事务切换开销
```

---

# 39. Batching 最大风险

PhotoBridge 的状态写不是普通日志。

很多操作是 crash-safe checkpoint。

例如：

```text
MarkCommitIntent ACK
```

意味着：

```text
数据库状态已经成功持久化到该 checkpoint
```

所以如果：

```text
A / B / C 共用一个 transaction
```

不能：

```text
A 执行完
→ 立即 ACK
→ transaction 还没 COMMIT
```

正确：

```text
A
B
C
↓
同一 transaction
↓
COMMIT
↓
A/B/C 一起 ACK
```

否则 crash 后可能出现：

```text
Worker 以为状态成功
但数据库里没有
```

---

# 40. V6-B2 建议只做小批量

不要：

```text
batch 1000
```

推荐测试：

```text
batch = 4
batch = 8
batch = 16
```

并采用：

> **Opportunistic Batching**

逻辑：

```text
Writer 被唤醒
↓
立即取第一个 command
↓
顺便取当前 queue 中已经存在的安全 command
↓
不主动 sleep 等更多 command
```

这样避免：

```text
为了攒 batch 人为增加 latency
```

---

# 41. 哪些 command 可能适合 batch

优先：

```text
不同 Task
同类状态写
彼此无依赖
```

但：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
```

能否混在一个事务中必须逐个证明。

原则：

> **不能因为都是数据库写，就默认可以一起 batch。**

---

# 42. V6-B 推荐拆成两个子阶段

```text
V6-B1
缩短事务
+
减少重复 SQL

↓

Benchmark

↓

V6-B2
Opportunistic Batching

↓

Crash Test
+
Benchmark
```

必要时才考虑：

```text
V6-B3
跨类型 batch
```

---

# 43. V6-B Benchmark

固定核心：

```text
e2e-large
1 GiB
4 Worker
```

比较：

```text
V6-A
vs
V6-B1
vs
V6-B2 batch4
vs
V6-B2 batch8
vs
V6-B2 batch16
```

不要直接只比较：

```text
V5 vs V6-B
```

否则无法分辨每一阶段贡献。

---

# 44. V6-B 建议增加 SQLite Microbenchmark

新增类似：

```text
runtime-state-write
```

模拟：

```text
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
Finish
```

参数：

```text
commands
workers
batch_size
```

记录：

```text
commands/s
Mean latency
P95
P99
COMMIT count
fsync count
```

---

# 45. V6-B 必须记录 ACK Latency

Batching 常见现象：

```text
总吞吐 ↑
单请求等待 ↑
```

所以除 E2E 外，还必须记录：

```text
Mean ACK latency
P95 ACK latency
P99 ACK latency
```

不能只看：

```text
commands/s
```

---

# 46. V6-B perf / strace 重点

V6-A 之后，如果 busy backoff 已经消失：

```text
clock_nanosleep
```

不应再是主角。

V6-B 重点看：

```text
fsync
fdatasync
pwrite64
futex
SQLite COMMIT 相关路径
```

目标：

```text
COMMIT / WAL 持久化次数下降
```

---

# 47. V6-B 正确性测试

只要开始 batching，必须新增 crash test。

至少覆盖：

```text
Batch COMMIT 前 crash
Batch COMMIT 后 crash
Worker 收到 ACK 前 crash
Worker 收到 ACK 后 crash
多个 Task 同 batch
部分 command 业务校验失败
```

必须确保：

```text
要么整个 transaction commit
要么整个 transaction rollback
```

并且等待者都能收到一致结果。

---

# 48. Batch 中单个 command 失败

例如：

```text
A 成功
B 业务校验失败
C 未执行
```

如果共享同一 transaction：

推荐初始策略：

```text
ROLLBACK 整个 batch
```

然后：

```text
B 返回原错误
A/C 返回 batch rollback / retryable 结果
```

但这会增加复杂度。

因此 V6-B2 最好优先 batch：

```text
前置业务校验已完成
执行失败概率低
彼此独立
```

的 command。

---

# 49. busy_timeout 在 V6 中怎么处理

V6-A 和 V6-B 都先保持：

```text
sqlite3_busy_timeout(..., 5000)
```

原因：

```text
Dedicated Writer
只解决当前进程内部多 Writer 竞争
```

仍可能存在：

```text
其他进程
其他命令
异常数据库占用
```

所以 busy_timeout 可以继续作为安全保护。

它不应作为 V6 性能调参变量。

---

# 50. synchronous=FULL 怎么处理

默认：

```text
不改
```

如果未来要测试：

```text
FULL
vs
NORMAL
```

应单独做：

```text
Durability Policy Experiment
```

并重新定义：

```text
掉电语义
数据丢失窗口
恢复保证
```

不能混进 V6。

---

# 51. V6-A → V6-B 的进入条件

只有满足：

```text
V6-A 正确性全部通过
并发 migrate 确认只有一个 Writer
busy handler 明显下降
没有死锁
没有 pending future
```

才进入 V6-B。

否则继续修 V6-A。

---

# 52. V6-B 的退出条件

如果：

```text
batching 提升很小
但复杂度大幅增加
```

则停止。

例如：

```text
E2E +0.5%
但 crash consistency 复杂度大幅提高
```

不值得。

PhotoBridge 是求职项目，必须考虑：

```text
收益
复杂度
可解释性
```

三者平衡。

---

# 53. 整体 Benchmark 矩阵

## V6-A

```text
Workers:
1
2
4
8

每组：
V5 Multi-Writer
vs
V6-A Dedicated Writer
```

重点：

```text
4 Worker
1 warmup
10 formal repetitions
```

## V6-B

固定：

```text
4 Worker
```

比较：

```text
V6-A
V6-B1
V6-B2 batch4
V6-B2 batch8
V6-B2 batch16
```

如果收益明显，再补：

```text
1 / 2 / 8 Worker
```

---

# 54. 推荐结果目录

```text
benchmark-results/
├── v6-a-db-writer/
│   ├── environment.txt
│   ├── e2e/
│   ├── perf/
│   ├── strace/
│   └── summary.md
│
└── v6-b-db-batching/
    ├── environment.txt
    ├── micro/
    ├── e2e/
    ├── perf/
    ├── strace/
    └── summary.md
```

---

# 55. Git 节点建议

推荐：

```text
V5 final baseline
↓
perf: add dedicated runtime db writer
↓
tag: v6-a-db-writer

↓ Benchmark

perf: shorten runtime db transactions
↓
tag: v6-b1-transaction-convergence

↓ Benchmark

perf: add opportunistic db command batching
↓
tag: v6-b2-db-batching
```

---

# 56. V6-A 最终架构图

```text
              ┌──────── Producer ────────┐
              │                          │
              │                          ↓
              │                  ClaimNextReady
              │                          │
Worker 1 ─────┤                          │
Worker 2 ─────┤                          │
Worker 3 ─────┤──→ RuntimeDbWriter Queue ├──→ DB Writer Thread
Worker 4 ─────┤                          │
              │                          ↓
              │                 TaskRuntimeRepository
              │                          ↓
              └──────────────────── SQLite WAL
```

文件处理仍然：

```text
Worker 1 / 2 / 3 / 4 并发
```

只有：

```text
SQLite write transaction
```

串行。

---

# 57. 常见误解

## Dedicated DB Writer = 整个程序单线程？

不是。

```text
Copy / Hash / Verify
仍然多 Worker

只有 SQLite 写事务
单 Writer
```

## Dedicated Writer = SQLite 写并行？

不是。

它的目的就是：

```text
承认 SQLite 单 Writer 事实
避免多个线程反复抢同一个锁
```

## busy_timeout 很慢，所以删掉？

不对。

删掉后：

```text
竞争仍然存在
只是直接报 SQLITE_BUSY
```

## Batch 越大越快？

不一定。

Batch 增大同时影响：

```text
吞吐
ACK latency
失败影响范围
Crash consistency
```

---

# 58. V6 最终性能故事

完整路线：

```text
V2
SQLite 基础写入优化
↓
V3
Multi-worker
↓
V4
BLAKE3 SIMD
↓
V5
io_uring Copy pipeline
↓
perf / strace
↓
发现 SQLite busy backoff
↓
V6-A
Dedicated DB Writer
↓
减少 Writer Lock 竞争
↓
重新 profiling
↓
如果 COMMIT 成为新瓶颈
↓
V6-B
Transaction Convergence / Batching
```

最终形成：

```text
测量
→ 优化
→ 验证
→ 瓶颈迁移
→ 再优化
```

闭环。

---

# 59. V6-A 面试表达

> V5 完成 io_uring 后，我没有继续盲目增加并发，而是通过 `strace -k` 发现完整主链里大量 `clock_nanosleep` 来自 SQLite 的 `sqliteDefaultBusyCallback`。进一步审查发现 Producer 和多个 Worker 各自持有 SQLite Connection，而状态推进频繁使用 `BEGIN IMMEDIATE`。SQLite 虽然是 WAL，但仍然只有一个 Writer，所以多个连接实际上是在争抢同一个写锁。
>
> V6-A 我不修改 SQL、FULL、WAL 和 busy timeout，而是引入 Dedicated DB Writer。Producer 和 Worker 把数据库状态操作提交到 DB Command Queue，由一个 Writer Thread 和一条 SQLite Connection 顺序执行。Worker 必须等真正 COMMIT 后才能收到 ACK，所以原来的 crash-safe checkpoint 没有改变。
>
> 这一步本质上是把 SQLite 内部的被动锁竞争，改成应用层主动串行调度。

---

# 60. V6-B 面试表达

> Dedicated Writer 消除多连接 Writer Lock 竞争后，我会重新 profiling。如果新的主要成本转移到事务提交本身，再进入 V6-B。第一步先缩短 `BEGIN IMMEDIATE` 到 `COMMIT` 的持锁区间并减少重复 SQL；第二步只对可以共享 durability boundary 的独立 command 做小批量 opportunistic batching。所有 command 都必须在共享事务真正 COMMIT 后才 ACK，因此不会为了吞吐破坏 crash consistency。

---

# 61. 最终执行顺序

```text
Step 1
冻结 V5 baseline

Step 2
实现 MigrationRuntimeStore

Step 3
实现 RuntimeDbWriter

Step 4
Producer Claim 接入 Writer

Step 5
Worker 状态写全部接入 Writer

Step 6
删除 per-worker SQLite write connections

Step 7
Unit / Integration / Sanitizer

Step 8
V6-A Worker 1/2/4/8 Benchmark

Step 9
perf / strace / nanosleep stack

Step 10
判断 V6-A 是否进入主线

Step 11
重新 profiling

Step 12
如果 transaction / COMMIT 仍是主要瓶颈
进入 V6-B1

Step 13
缩短事务 / 减少重复 SQL

Step 14
Benchmark

Step 15
只有数据继续支持
才进入 V6-B2 opportunistic batching

Step 16
Crash Test + ACK Latency + E2E Benchmark

Step 17
最终收口
```

---

# 62. V6-A 完成定义

```text
[ ] 单 Dedicated Writer
[ ] Producer Claim 通过 Writer
[ ] Worker 写操作全部通过 Writer
[ ] 并发 migrate 无第二个 SQLite Writer
[ ] 原 SQL / Transaction 语义不变
[ ] ACK 发生在 COMMIT 后
[ ] 全部测试通过
[ ] Worker 1/2/4/8 Benchmark
[ ] 4 Worker 正式 10 reps
[ ] perf stat
[ ] strace -f -c
[ ] strace -k 验证 busy stack
[ ] 与 V5 正式对比
[ ] 形成 V6-A 报告
```

---

# 63. V6-B 完成定义

```text
[ ] V6-A 已稳定
[ ] 新瓶颈已重新定位
[ ] 明确哪些事务可以缩短
[ ] 明确哪些 Command 可以 batch
[ ] ACK / crash-safe 语义定义清楚
[ ] V6-B1 transaction convergence
[ ] B1 Benchmark
[ ] V6-B2 opportunistic batching
[ ] batch 4/8/16 对比
[ ] Command latency P95/P99
[ ] Crash Tests
[ ] E2E 10 reps
[ ] perf / strace
[ ] 明确复杂度收益比
[ ] 形成 V6-B 报告
```

---

# 64. 最终原则

V6 最重要的不是：

```text
“用了 Dedicated Writer”
```

或者：

```text
“做了 batching”
```

而是：

```text
V5 profiling
证明 Writer contention

V6-A
只修改 Writer 拓扑
验证竞争是否下降

再次 profiling
确认新的瓶颈

V6-B
只在有证据时减少事务和 commit
```

最终目标：

> **用测量证据驱动数据库并发架构优化，同时保持 PhotoBridge 原有的 crash-safe、epoch fencing、ownership、receipt 与 recovery 语义。**

---

# 65. 一句话总结

```text
V6-A：
把“多个线程竞争 SQLite 单 Writer”
改成
“多个线程提交 Command，由单 DB Writer 主动串行执行”。

V6-B：
在 V6-A 已消除 Writer Lock 竞争后，
进一步减少事务数量、缩短持锁时间，
并在正确性允许时做小批量提交。
```
