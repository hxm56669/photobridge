# PhotoBridge V6-A：Dedicated DB Writer 代码改造与设计说明

> 项目：PhotoBridge  
> 阶段：V6-A  
> 主题：SQLite 写路径收敛 / Dedicated DB Writer  
> 文档类型：代码改造记录  
> 审核基准：2026-09-21 GitHub `main` 最新代码  
> 本文重点：记录 **V6-A 为什么要做、改造前是什么、改造后是什么、具体改了哪些代码、为什么这样改，以及哪些语义刻意保持不变**。  
>
> 本文不提前写 V6-A 的性能收益结论。性能收益需要后续通过：
>
> ```text
> Worker 1 / 2 / 4 / 8
> +
> E2E 10 reps
> +
> perf
> +
> strace
> +
> clock_nanosleep stack
> ```
>
> 单独验证。

---

# 1. V6-A 的定位

V6-A 的核心不是：

```text
让 SQLite 可以同时多 Writer 并发写
```

而是：

```text
承认 SQLite 最终只有一个 Writer
↓
把多个线程在 SQLite 内部被动抢锁
↓
改成应用层主动排队
↓
由一个 Dedicated DB Writer 顺序执行
```

一句话概括：

> **把“多个线程竞争 SQLite 单 Writer Lock”改成“多个线程提交数据库命令，由一个 Writer Thread 主动串行执行”。**

V6-A 解决的是：

```text
谁来写数据库
```

而不是：

```text
一次事务里写多少内容
```

后者属于 V6-B。

---

# 2. V6-A 的来源：V5 之后瓶颈发生迁移

V5 已经对 Copy 数据面做了 `io_uring` 优化。

局部 Copy + Hash 已经继续加速，但完整 E2E 的提升明显小于局部 Microbenchmark。

此前 profiling 中观察到：

```text
clock_nanosleep
↓
unixSleep
↓
sqliteDefaultBusyCallback
↓
btreeBeginTrans
↓
sqlite3_step / sqlite3_exec
↓
SqliteConnection::Execute
↓
TaskRuntimeRepository
```

同时命中的业务状态写包括：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
```

说明完整迁移链路的新问题已经不只是：

```text
文件 Copy 速度
```

而是开始受到：

```text
SQLite 多连接 Writer Lock 竞争
```

影响。

因此 V6-A 是一次典型的：

```text
V5 优化
↓
重新 profiling
↓
瓶颈迁移
↓
针对新瓶颈调整架构
```

---

# 3. V6-A 改造前：V5 的 SQLite 并发模型

## 3.1 旧架构

V6-A 之前，多 Worker migrate 的数据库结构大致是：

```text
Producer
│
└── Main SQLite Connection
    └── TaskRuntimeRepository
        └── ClaimNextReady()

Worker 1
│
└── SQLite Connection 1
    └── TaskRuntimeRepository 1
        ├── MarkCommitIntent()
        ├── MarkTempWritten()
        ├── PersistVerifiedReceipt()
        └── MarkRetryable()

Worker 2
│
└── SQLite Connection 2
    └── TaskRuntimeRepository 2

Worker 3
│
└── SQLite Connection 3
    └── TaskRuntimeRepository 3

Worker 4
│
└── SQLite Connection 4
    └── TaskRuntimeRepository 4
```

如果：

```text
workers = 4
```

那么并发阶段可能同时存在：

```text
1 条 Producer 写连接
+
4 条 Worker 写连接
```

即：

```text
5 个潜在 SQLite Writer
```

---

# 4. 为什么旧实现会产生竞争

PhotoBridge 当前 SQLite 配置仍然是：

```text
journal_mode = WAL
synchronous = FULL
foreign_keys = ON
busy_timeout = 5000 ms
```

WAL 的主要好处是：

```text
Reader 和 Writer 可以更好并发
```

但它不意味着：

```text
多个 Writer 可以真正同时修改数据库
```

SQLite 最终仍然只有一个 Writer。

而 PhotoBridge 的关键状态推进内部大量采用：

```sql
BEGIN IMMEDIATE;
...
COMMIT;
```

所以旧模型在多 Worker 下会形成：

```text
Producer / Worker 1 / Worker 2 / Worker 3 / Worker 4
                ↓
        同时开始写事务
                ↓
         抢 SQLite Writer Lock
                ↓
        只有一个连接成功进入
                ↓
       其他连接遇到 SQLITE_BUSY
                ↓
          busy handler 等待
                ↓
         clock_nanosleep
                ↓
            醒来重试
```

这里有一个非常重要的结论：

> 多个 Connection 并没有让 SQLite 的写事务真正并行，只是让多个线程在数据库内部竞争同一个不可并行的资源。

因此旧实现的主要问题不是：

```text
SQLite 没有加锁
```

而恰恰是：

```text
多个连接都在争抢同一把写锁
```

---

# 5. 为什么不能简单调 busy_timeout

一个最容易想到的方案是：

```text
busy_timeout = 5000
↓
改小
```

甚至：

```text
busy_timeout = 0
```

但这不会解决根因。

因为：

```text
Writer Lock 竞争仍然存在
```

区别只是：

```text
以前：
竞争失败
→ sleep
→ retry

改成 0 后：
竞争失败
→ 立即 SQLITE_BUSY
```

所以：

> `busy_timeout` 只能决定“竞争失败以后怎么等”，不能消除“为什么会有多个 Writer 同时竞争”。

V6-A 因此没有把：

```text
busy_timeout
```

作为优化变量。

---

# 6. 为什么不能直接改 synchronous=FULL

另一个看起来可能明显提高速度的方案是：

```text
synchronous = FULL
↓
NORMAL
```

但这会同时改变：

```text
掉电语义
WAL 持久化保证
Crash Recovery 的 durability 边界
```

这样即使性能变快，也无法判断：

```text
到底是 Writer 拓扑优化有效
还是 durability 被削弱以后变快
```

所以 V6-A 明确保持：

```text
WAL 不变
FULL 不变
busy_timeout 不变
SQL 状态机不变
事务边界不变
```

只改变：

```text
并发 migrate 阶段的 Writer 拓扑
```

这样后面的性能对比才有清晰因果关系。

---

# 7. V6-A 最终目标架构

改造后，并发 migrate 阶段变成：

```text
                         ┌── Producer
                         │
                         │   ClaimNextReady
                         │
                         ↓
Worker 1 ────────────────┐
Worker 2 ────────────────┤
Worker 3 ────────────────┼──→ RuntimeDbWriter
Worker 4 ────────────────┤       │
                         │       ↓
Producer ────────────────┘   DB Command Queue
                                 │
                                 ↓
                         Dedicated Writer Thread
                                 │
                                 ↓
                         TaskRuntimeRepository
                                 │
                                 ↓
                         1 SQLite Connection
                                 │
                                 ↓
                              SQLite
```

文件数据面仍然是：

```text
Worker 1
Worker 2
Worker 3
Worker 4
```

并发执行：

```text
Open
Copy
BLAKE3
fdatasync
Verify
```

只有：

```text
SQLite Runtime 写事务
```

被主动串行化。

因此：

> V6-A 不是把整个迁移程序改成单线程，而是只把 SQLite 写路径收敛为单 Writer。

---

# 8. 本次代码改动涉及的主要文件

V6-A 当前主要落点：

```text
include/photobridge/app/
├── migration_runtime_store.h        新增
├── runtime_db_writer.h              新增
├── task_runtime_repository.h        修改
└── migration_attempt_preparer.h     修改

src/app/
├── runtime_db_writer.cpp            新增
├── task_runtime_repository.cpp      保留事务实现
└── migration_attempt_preparer.cpp   修改依赖

src/pipeline/
└── migration_service.cpp            核心主链改造

tests/integration/sqlite/
└── runtime_db_writer_test.cpp       新增

CMakeLists.txt                        增加 Writer 编译单元
tests/CMakeLists.txt                  增加 Writer 集成测试
```

其中最关键的是：

```text
migration_runtime_store.h
runtime_db_writer.*
migration_service.cpp
```

---

# 9. 改动一：抽出 MigrationRuntimeStore

## 9.1 过去的问题

过去迁移执行代码直接依赖：

```text
TaskRuntimeRepository
```

例如 Worker 的执行链、`MigrationAttemptPreparer` 都知道：

```text
底层就是 TaskRuntimeRepository
```

而 `TaskRuntimeRepository` 又直接绑定：

```text
SqliteConnection
```

形成：

```text
Migration 业务代码
↓
TaskRuntimeRepository
↓
SqliteConnection
↓
SQLite
```

这样的问题是：

> 如果想在 Migration 和 Repository 中间增加“单 Writer 调度层”，上层类型已经直接绑定 Repository，很难无侵入地替换。

---

## 9.2 现在的实现

新增：

```text
include/photobridge/app/migration_runtime_store.h
```

定义最小接口：

```text
MigrationRuntimeStore
```

只暴露并发 migrate 阶段真正需要的操作：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
```

同时 `ClaimedTask` 也移动到这一层，成为 migrate runtime 接口共同使用的数据结构。

现在关系变成：

```text
                       MigrationRuntimeStore
                         ↑             ↑
                         │             │
            TaskRuntimeRepository   RuntimeDbWriter
```

---

# 10. 为什么 MigrationRuntimeStore 只放这几个接口

这里没有把整个 `TaskRuntimeRepository` 的所有方法复制进去。

例如下面这些能力没有全部塞进接口：

```text
AddTask
AddDependency
ReadRuntime
AcquireNextExecutionEpoch
ReadVerifiedReceipt
RecoverSucceeded
RecoverRetryable
RecoverInconsistent
...
```

原因是：

> V6-A 只解决“并发 migrate 阶段的数据库写路径”。

因此接口应该保持最小化：

```text
只抽出并发阶段真正会被 Producer / Worker 调用的状态写操作
```

而不是制造：

```text
一个巨大 Repository Interface
```

这是一次有明确边界的依赖倒置。

---

# 11. TaskRuntimeRepository 的变化

现在：

```cpp
class TaskRuntimeRepository final : public MigrationRuntimeStore
```

也就是说原 Repository：

```text
没有被删除
没有被重写
没有改变原来的 SQL 语义
```

它只是：

```text
实现 MigrationRuntimeStore
```

这样做的好处是：

```text
原有 Repository
→ 继续负责真正的 SQL / Transaction / State Machine

RuntimeDbWriter
→ 只负责调度和串行化
```

职责划分变成：

```text
RuntimeDbWriter
负责：
谁先执行
在哪个线程执行
用哪一条 Connection 执行

TaskRuntimeRepository
负责：
执行什么 SQL
状态是否合法
epoch / ownership 是否正确
事务怎么 COMMIT / ROLLBACK
```

这是 V6-A 很重要的设计点：

> **没有为了做 Dedicated Writer 重写已有且成熟的状态机代码。**

---

# 12. 改动二：新增 RuntimeDbWriter

新增：

```text
include/photobridge/app/runtime_db_writer.h
src/app/runtime_db_writer.cpp
```

它是 V6-A 的核心组件。

内部拥有：

```text
1 SqliteConnection
1 TaskRuntimeRepository
1 Writer Thread
1 mutex
1 condition_variable
1 DB Command Queue
```

当前成员关系本质上是：

```text
RuntimeDbWriter
├── connection_
├── repository_
├── thread_
├── mutex_
├── changed_
├── commands_
├── stopping_
└── fatal_
```

---

# 13. RuntimeDbWriter 为什么自己持有 SQLite Connection

V6-A 需要保证：

```text
并发 migrate 阶段真正执行 Runtime 写事务的只有一个执行上下文
```

因此 Writer 自己创建并拥有：

```text
SqliteConnection
```

然后：

```text
TaskRuntimeRepository repository_(connection_)
```

这样 Repository 的所有写事务最终都进入：

```text
同一条 Writer Connection
```

Worker 和 Producer 不直接持有这条 Connection。

因此并发关系从：

```text
多个线程
→ 多个 SQLite Connection
→ 争锁
```

变成：

```text
多个线程
→ Queue
→ 一个 Writer Thread
→ 一个 SQLite Connection
```

---

# 14. RuntimeDbWriter::Start

Writer 的启动逻辑可以概括成：

```text
打开数据库
↓
构造 RuntimeDbWriter
↓
构造绑定该 Connection 的 TaskRuntimeRepository
↓
启动 Writer Thread
↓
Writer Thread 进入 Run()
```

核心目标是：

> 从进入并发 migrate 阶段开始，所有 Runtime 写命令有唯一执行入口。

---

# 15. 改动三：显式 DB Command 类型

V6-A 没有采用：

```text
std::function<void()>
```

或者复杂的通用 RPC 框架。

而是为当前 5 类操作定义显式 Command：

```text
Claim
CommitIntent
TempWritten
Receipt
Retryable
```

再组合成：

```text
std::variant<...>
```

Queue 中保存的是：

```text
Command
```

Writer Thread 用：

```text
std::visit
```

分发到真正的：

```text
TaskRuntimeRepository
```

对应方法。

---

# 16. 为什么使用显式 Command，而不是任意 std::function

显式 Command 的优点是：

```text
命令集合是封闭的
参数类型明确
返回值类型明确
更容易审计哪些写操作进入 Writer
更容易测试
不容易把任意数据库操作偷偷塞进 Writer
```

如果一开始直接使用：

```text
std::function
```

虽然看起来通用，但会让 V6-A 变成：

```text
一个通用异步执行器
```

反而模糊：

```text
哪些数据库写是允许的
哪些数据库写已经完成收敛
```

所以当前 `variant + 明确 command struct` 更符合项目范围。

---

# 17. 改动四：Command Queue

Producer 和 Worker 调用：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
```

时，不再自己访问 SQLite。

而是：

```text
构造 Command
↓
获取 future
↓
Enqueue
↓
notify Writer Thread
↓
等待 future
```

Writer：

```text
wait condition_variable
↓
pop queue.front()
↓
Execute(command)
↓
repository_.XXX(...)
↓
promise.set_value(...)
```

这一步把 SQLite 的被动锁竞争替换成：

```text
应用层 FIFO Queue
```

---

# 18. V6-A 的真正性能逻辑

旧模型：

```text
线程 A
线程 B
线程 C
线程 D
↓
一起尝试 BEGIN IMMEDIATE
↓
SQLite 内部决定谁拿 Writer Lock
↓
其他连接 busy
↓
sleep / wake / retry
```

新模型：

```text
A → Queue
B → Queue
C → Queue
D → Queue
↓
Writer Thread
↓
A
↓
B
↓
C
↓
D
```

SQLite 本来就不能同时执行这些写事务。

因此 V6-A 并不是：

```text
牺牲 SQLite 写并行度
```

因为原本并不存在真正的多 Writer 并行。

它做的是：

```text
把数据库内部的隐式竞争
↓
搬到应用层显式调度
```

理论上减少：

```text
SQLITE_BUSY
busy handler
clock_nanosleep
重复抢锁
无效 wakeup
context switch
多 Connection 写协调成本
```

---

# 19. 改动五：ACK 不允许在“入队”时返回

这是 V6-A 最重要的正确性约束之一。

错误设计是：

```text
Worker
↓
Command 入队成功
↓
立即认为数据库状态成功
↓
继续后面的业务步骤
```

这种写法会改变 PhotoBridge 的 crash-safe 语义。

例如：

```text
fdatasync(temp)
↓
MarkTempWritten 入队
↓
还没 COMMIT
↓
Worker 已继续 Verify
```

如果这时进程崩溃：

```text
文件系统已经进入下一阶段
但数据库 checkpoint 可能还没有真正持久化
```

因此当前实现采用：

```text
promise / future
```

调用者流程是：

```text
创建 command
↓
command.completion.get_future()
↓
Enqueue
↓
future.get()
↓
等待 Writer 真正执行 Repository
↓
Repository transaction COMMIT
↓
Repository 返回 Status
↓
promise.set_value(Status)
↓
future ready
↓
调用者继续
```

所以：

> **ACK 的含义仍然是“Repository 操作已经真正完成”，而不是“Command 已经排队”。**

---

# 20. 为什么可以认为 ACK 在 COMMIT 之后

`RuntimeDbWriter` 的 `Execute()` 最终调用：

```text
TaskRuntimeRepository::ClaimNextReady
TaskRuntimeRepository::MarkCommitIntent
TaskRuntimeRepository::MarkTempWritten
TaskRuntimeRepository::PersistVerifiedReceipt
TaskRuntimeRepository::MarkRetryable
```

而 Repository 内部仍然自己管理：

```text
BEGIN IMMEDIATE
↓
ownership / epoch 校验
↓
UPDATE / INSERT
↓
event
↓
COMMIT
↓
return Status
```

例如 `AdvanceAttempt()`：

```text
BEGIN IMMEDIATE
↓
检查 epoch
↓
检查 ownership
↓
读取当前 attempt state
↓
更新 attempt state
↓
写 task_event
↓
COMMIT
↓
return
```

`PersistVerifiedReceipt()` 也是：

```text
BEGIN IMMEDIATE
↓
校验 epoch / ownership
↓
INSERT verified_receipt
↓
attempt → VERIFIED_DURABLE
↓
写 VERIFIED_DURABLE event
↓
COMMIT
↓
return
```

因此：

```text
Repository 返回
```

发生在：

```text
COMMIT 成功
```

之后。

Writer 又是在 Repository 返回之后才：

```text
promise.set_value(...)
```

所以：

```text
future ready
```

自然发生在：

```text
transaction COMMIT
```

之后。

---

# 21. 改动六：MigrationAttemptPreparer 从具体 Repository 解耦

改造前它直接依赖：

```text
TaskRuntimeRepository&
```

改造后变成：

```text
MigrationRuntimeStore&
```

现在准备一次迁移 attempt 的状态链仍然是：

```text
MarkCommitIntent
↓
CreateTempNoReplace
↓
Copy / Hash
↓
fdatasync(temp)
↓
MarkTempWritten
↓
重新打开 temp
↓
VerifyBinary
↓
PersistVerifiedReceipt
```

变化只有：

```text
这些数据库操作由谁执行
```

没有变化的是：

```text
这些 checkpoint 在业务流程中的顺序
```

这意味着 V6-A 没有破坏原有：

```text
Crash-safe Commit 前半段状态机
```

---

# 22. 为什么要改 MigrationAttemptPreparer 的参数类型

如果它继续接受：

```text
TaskRuntimeRepository&
```

那么 Worker 最终还是必须拿到：

```text
TaskRuntimeRepository
```

这会迫使：

```text
Worker
→ Repository
→ SQLite
```

旧依赖继续存在。

改成：

```text
MigrationRuntimeStore&
```

以后：

```text
MigrationAttemptPreparer
```

只知道：

```text
我需要推进 Runtime 状态
```

而不知道：

```text
这个状态操作是直接 Repository 执行
还是经过 Dedicated Writer 执行
```

这正是抽象层存在的意义。

---

# 23. 改动七：ExecuteClaimedTask 也改为依赖 MigrationRuntimeStore

`MigrationService` 内部的：

```text
ExecuteClaimedTask(...)
```

当前第一个 Runtime 参数已经是：

```text
MigrationRuntimeStore&
```

因此错误路径中的：

```text
MarkRetryable
```

也通过同一个 Store。

这一步很重要。

否则很容易出现一种“半收敛”架构：

```text
正常状态写
→ Dedicated Writer

错误状态写
→ Worker 自己的 Repository
```

这样最终仍然存在第二条 SQLite Writer 路径。

当前实现没有留下这个绕行路径。

---

# 24. 改动八：MigrationService 删除 per-worker SQLite Connection

这是 V6-A 主链中最关键的结构变化。

## 改造前

旧逻辑需要：

```text
worker_connections[workers]
```

每个 Worker：

```text
创建自己的 SqliteConnection
↓
创建自己的 TaskRuntimeRepository
↓
执行任务
```

所以：

```text
Worker 数量 ↑
↓
SQLite Writer Connection 数量也 ↑
```

---

## 改造后

当前 `MigrationService` 在进入并发阶段前：

```text
完成 plan / runtime 初始化
↓
AcquireNextExecutionEpoch
↓
读取 source root
↓
RuntimeDbWriter::Start(database)
```

然后创建 Worker Pool。

Worker 不再创建：

```text
SqliteConnection
TaskRuntimeRepository
```

而是共享：

```text
RuntimeDbWriter
```

并调用：

```text
ExecuteClaimedTask(*writer, ...)
```

因此：

```text
Worker 数量
```

和：

```text
SQLite Writer Connection 数量
```

已经解耦。

例如：

```text
workers = 1
→ 1 Dedicated Writer

workers = 4
→ 仍然 1 Dedicated Writer

workers = 8
→ 仍然 1 Dedicated Writer
```

这就是 V6-A 的核心完成标志。

---

# 25. 改动九：Producer ClaimNextReady 也进入 Writer

只把 Worker 状态写放进 Writer 是不够的。

因为 Producer 的：

```text
ClaimNextReady
```

本身也是写事务。

它会：

```text
BEGIN IMMEDIATE
↓
选择 READY Task
↓
设置 RUNNING
↓
写 owner_epoch
↓
创建 attempt
↓
更新 active_attempt_id
↓
COMMIT
```

所以如果改成：

```text
Workers → Dedicated Writer

Producer → Main Repository 直接 Claim
```

那么并发阶段仍然有：

```text
Dedicated Writer Connection
+
Producer Main Connection
```

即：

```text
2 个 Writer
```

只能做到：

```text
5 Writer
→
2 Writer
```

并没有完成 V6-A。

当前代码已经改成：

```text
writer->ClaimNextReady(...)
```

所以 Producer 和 Worker 的并发写入口统一为：

```text
RuntimeDbWriter
```

这一点非常关键。

---

# 26. 为什么初始化阶段没有全部塞进 RuntimeDbWriter

当前代码仍然在启动 Dedicated Writer **之前**使用主连接完成：

```text
EnsureSchema
MaterializePlan
ReadTaskStateCounts
ReadRuntime
SetReady
AcquireNextExecutionEpoch
ReadManifestSourceRoot
```

这不是遗漏。

原因是这些操作发生在：

```text
Worker Pool 启动之前
```

也就是还没有：

```text
Producer + Workers 并发写数据库
```

此时不存在 V6-A 要解决的多 Writer 竞争。

所以当前生命周期是：

```text
阶段 1：单线程初始化
Main Connection
↓
完成准备

阶段 2：并发 migrate
启动 RuntimeDbWriter
↓
Producer + Workers 的 Runtime 写全部收口

阶段 3：并发任务结束
join Workers
↓
Stop Writer
```

这种边界比“所有数据库操作都强行走 Writer”更简单。

---

# 27. 改动十：Task Queue 和 DB Queue 分离

当前 migrate 本来就有一条：

```text
Task Queue
```

结构大致是：

```text
Producer
↓
ClaimedTask
↓
Task Queue
↓
Workers
```

V6-A 新增的是另一条：

```text
DB Command Queue
```

结构是：

```text
Producer / Workers
↓
Runtime DB Command
↓
DB Command Queue
↓
Writer Thread
```

两条 Queue 职责完全不同：

```text
Task Queue
负责：
任务调度和 Worker 并发

DB Queue
负责：
数据库写事务串行化
```

因此当前没有复用：

```text
同一个 queue
同一个 mutex
```

这是正确的。

---

# 28. 为什么两条 Queue 必须分开

如果把两条 Queue 合并，容易产生：

```text
任务调度状态
和
数据库持久化状态
```

互相绑死。

甚至可能形成：

```text
持有 Task Queue mutex
↓
等待 DB ACK
↓
DB Writer 又需要某个 Task Queue 条件
↓
死锁
```

当前实现的 Worker 流程更接近：

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

DB Writer 自己只管理：

```text
DB Queue mutex
```

不存在反向获取：

```text
Task Queue mutex
```

因此锁依赖关系更清晰。

---

# 29. RuntimeDbWriter 的 Queue 为什么当前没有 bounded capacity

当前 DB Queue 使用：

```text
std::deque<Command>
```

本身没有额外：

```text
capacity = 32 / 64
```

限制。

但这在 V6-A 当前实现中不是严重问题。

原因是所有公开 API 都是同步 ACK：

```text
Submit
↓
future.get()
```

调用线程一次提交一个 Command 后就等待结果。

因此每个 Producer / Worker 同时通常只会挂起极少量数据库命令。

队列天然受到：

```text
Producer 数量
+
Worker 数量
```

限制。

所以当前没有必要为了“形式上 bounded”增加新的阻塞条件和复杂度。

---

# 30. 改动十一：Stop / Drain 生命周期

Dedicated Writer 最大的风险之一是：

```text
Worker 在 future.get()
↓
Writer 提前退出
↓
Worker 永久等待
```

当前 `Stop()` 的核心行为是：

```text
stopping_ = true
↓
notify Writer Thread
↓
join Writer Thread
```

而 Writer Thread 的退出条件不是：

```text
stopping_ == true
→ 立即退出
```

而是：

```text
被唤醒
↓
如果 queue 非空
→ 继续 pop + Execute

直到：
queue 为空
→ return
```

因此已经进入 Queue 的 Command 会：

```text
drain
```

完成以后 Writer 才真正退出。

---

# 31. Stop 以后为什么不能再接受新命令

`Enqueue()` 会检查：

```text
stopping_
```

如果已经停止：

```text
返回 runtime DB writer is stopped
```

而不是：

```text
继续 push 到已经没有消费者的 Queue
```

这样可以避免：

```text
Stop 已开始
↓
新 Command 入队
↓
Writer 已退出
↓
future 永久不 ready
```

---

# 32. fatal error 的处理

Writer Thread 的 `Run()` 对未捕获异常设置：

```text
fatal_
stopping_
```

同时：

```text
当前 Command
→ 返回 failure

Queue 中 pending Commands
→ 全部 Fail

后续 Enqueue
→ 直接返回 fatal_
```

这个设计的目标是：

> **任何等待者都必须得到结果，不能因为 Writer Thread 异常退出而永久阻塞。**

需要注意：

```text
普通 Repository 返回 Status error
```

和：

```text
Writer Thread 自身抛出异常
```

不是一回事。

普通业务错误只返回给当前调用者；

Writer 本身出现 fatal exception 才终止整条 Writer。

---

# 33. V6-A 没有改变 TaskRuntimeRepository 的状态机

这是本次改造一个非常重要的优点。

V6-A 没有重写：

```text
Claim 状态机
Attempt 状态机
Epoch fencing
Ownership check
VerifiedReceipt 语义
Retryable 语义
Recovery 状态机
```

原来 Repository 里的：

```text
BEGIN IMMEDIATE
epoch check
ownership check
state transition validation
event append
COMMIT / ROLLBACK
```

全部继续存在。

改变的是：

```text
谁调用 Repository
```

而不是：

```text
Repository 内部状态机怎么工作
```

因此 V6-A 的正确性风险比“同时重写 SQL 状态机 + 并发模型”低得多。

---

# 34. V6-A 没有改变的配置

当前 `SqliteConnection` 仍然保持：

```text
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
PRAGMA foreign_keys=ON;
sqlite3_busy_timeout(..., 5000);
```

同时打开方式仍包含：

```text
SQLITE_OPEN_FULLMUTEX
```

所以 V6-A 没有偷偷通过：

```text
降低 durability
关闭 busy protection
修改 journal mode
```

换性能。

这对后面的 V5 / V6-A 对比非常重要。

---

# 35. 为什么 busy_timeout 仍然保留

Dedicated Writer 解决的是：

```text
当前 PhotoBridge migrate 进程内部
多个 Connection 竞争 Writer Lock
```

但理论上数据库还可能受到：

```text
其他进程
其他命令
外部异常持锁
```

影响。

所以：

```text
busy_timeout = 5000
```

仍然可以作为防御性配置保留。

V6-A 的目标不是：

```text
让 busy_timeout 永远失效
```

而是：

```text
让正常 migrate 主链不再依赖 busy handler 来解决内部 Writer 竞争
```

---

# 36. CMake 构建接入

V6-A 新增的：

```text
src/app/runtime_db_writer.cpp
```

已经加入：

```text
photobridge_core
```

所以 Dedicated Writer 不是孤立代码，而是正式构建目标的一部分。

测试侧：

```text
integration/sqlite/runtime_db_writer_test.cpp
```

也已经加入：

```text
photobridge_tests
```

说明 V6-A 已经正式进入项目构建与测试体系。

---

# 37. V6-A 新增测试：AckFollowsCommitAndRepositoryErrorsPropagate

这个测试验证两件重要事情。

第一：

```text
Writer 调用成功返回后
↓
数据库状态已经真的变化
```

例如：

```text
Claim
→ RUNNING

MarkCommitIntent
→ COMMIT_INTENT

MarkTempWritten
→ TEMP_WRITTEN
```

也就是说 ACK 不是：

```text
“已经入队”
```

而是：

```text
“Repository 状态操作已经完成”
```

第二：

```text
Repository 自己返回的非法状态转换错误
```

能够原样返回给调用者。

因此 Dedicated Writer 没有吞掉业务错误。

---

# 38. V6-A 新增测试：MultiThreadSubmitDrainsEveryCommand

当前测试构造：

```text
64 Tasks
8 Threads
```

多个线程同时通过：

```text
RuntimeDbWriter
```

执行：

```text
Claim
MarkCommitIntent
MarkTempWritten
```

最终检查：

```text
所有 64 个 Task 都完成到 TEMP_WRITTEN
failure = 0
```

这个测试主要验证：

```text
多线程 Submit
↓
单 Writer 串行执行
↓
没有丢 Command
↓
没有重复 Claim
↓
状态最终一致
```

---

# 39. V6-A 新增测试：ReceiptAndRetryUseTheWriterConnection

这个测试专门覆盖：

```text
PersistVerifiedReceipt
MarkRetryable
```

避免出现：

```text
Claim / TempWritten 已经走 Writer
但 Receipt / Error path 仍然偷偷直写 SQLite
```

测试验证：

```text
Receipt 可以通过 Writer 持久化
Attempt 进入 VERIFIED_DURABLE
```

以及：

```text
另一个 Task 可以通过 Writer 标记 RETRYABLE
```

所以：

```text
成功路径
+
失败路径
```

都已经纳入 Dedicated Writer。

---

# 40. V6-A 新增测试：StopDrainsAcceptedCommandsAndWakesAllCallers

该测试让：

```text
多个 Caller
```

同时尝试通过 Writer Claim。

然后调用：

```text
Stop()
```

最后要求：

```text
每个 Caller 要么成功 Claim
要么明确收到 stopped error
```

不能出现：

```text
永久阻塞
未知结果
Command 已执行但 Caller 永远不知道
```

同时再读取数据库确认：

```text
真正持久化为 RUNNING 的 Task 数
==
成功收到 Claim 的 Caller 数
```

这验证了：

```text
Stop
Queue Drain
Caller Wakeup
Database Result
```

之间的一致性。

---

# 41. 改造前后的直接对比

| 对比项 | V5 / V6-A 前 | V6-A 后 |
|---|---|---|
| Producer Claim | Main Repository 直接写 SQLite | 通过 RuntimeDbWriter |
| Worker DB Connection | 每 Worker 一条 | Worker 不持有写连接 |
| Worker Repository | 每 Worker 一个 | Worker 不直接创建 Repository |
| SQLite Writer 数 | Main + N Workers | 并发阶段 1 个 Dedicated Writer |
| 写调度位置 | SQLite 内部锁竞争 | 应用层 Command Queue |
| busy 竞争 | 正常高并发路径可能发生 | 理论上显著减少 |
| DB 状态 API | 直接依赖 TaskRuntimeRepository | 依赖 MigrationRuntimeStore |
| SQL / Transaction | 原 Repository | 保持原 Repository |
| WAL | WAL | WAL |
| synchronous | FULL | FULL |
| busy_timeout | 5000 ms | 5000 ms |
| ACK | Repository 返回 | Writer 等 Repository 返回后 ACK |
| 文件 Copy | Worker 并发 | 仍然 Worker 并发 |
| Batch | 无 | 仍无 |
| Recovery | 原实现 | 不改 |
| Receipt 顺序 | 原顺序 | 不改 |

---

# 42. 调用链前后对比

## V6-A 前：Producer

```text
MigrationService
↓
main TaskRuntimeRepository
↓
ClaimNextReady
↓
BEGIN IMMEDIATE
↓
SQLite
```

## V6-A 后：Producer

```text
MigrationService
↓
RuntimeDbWriter::ClaimNextReady
↓
DB Command Queue
↓
Writer Thread
↓
TaskRuntimeRepository::ClaimNextReady
↓
BEGIN IMMEDIATE
↓
SQLite
↓
COMMIT
↓
promise
↓
Producer
```

---

# 43. 调用链前后对比：Worker

## V6-A 前

```text
Worker N
↓
Worker SqliteConnection N
↓
Worker TaskRuntimeRepository N
↓
MarkCommitIntent / MarkTempWritten / Receipt / Retryable
↓
SQLite
```

多个 Worker 同时进入：

```text
BEGIN IMMEDIATE
```

产生竞争。

## V6-A 后

```text
Worker N
↓
MigrationRuntimeStore
↓
RuntimeDbWriter
↓
DB Command Queue
↓
Writer Thread
↓
单 TaskRuntimeRepository
↓
单 SQLite Connection
↓
SQLite
```

所有 Worker 的 Runtime 状态写在：

```text
应用层
```

已经天然串行。

---

# 44. 文件数据路径没有被串行化

V6-A 不能理解成：

```text
Worker 1 Copy 完
↓
Worker 2 才能 Copy
```

真实结构仍然是：

```text
Worker 1：Copy / Hash / Verify ─────┐
Worker 2：Copy / Hash / Verify ─────┤
Worker 3：Copy / Hash / Verify ─────┼── 并发
Worker 4：Copy / Hash / Verify ─────┘
```

只是在遇到 checkpoint 时：

```text
Worker 1 ─┐
Worker 2 ─┤
Worker 3 ─┼→ DB Writer → SQLite
Worker 4 ─┘
```

所以：

```text
数据面并发
+
控制面数据库写串行
```

是当前 V6-A 的准确描述。

---

# 45. 为什么这种结构符合 SQLite 的特性

SQLite 的能力更接近：

```text
多个 Reader
+
单 Writer
```

所以应用层硬做：

```text
多个 Writer Connection
```

并不会自动获得：

```text
真正的多 Writer 并行
```

对于 PhotoBridge 这种场景：

```text
大部分时间：
Copy / Hash / Verify 属于文件 I/O / CPU 工作

数据库操作：
主要是短状态事务
```

更合理的模型是：

```text
文件处理继续并发
数据库状态提交集中串行
```

这也是 Dedicated Writer 最适合当前项目的原因。

---

# 46. V6-A 的代价

Dedicated Writer 不是免费优化。

它新增：

```text
Command 对象
Queue push/pop
mutex
condition_variable
promise/future
线程切换
```

所以：

```text
Worker = 1
```

时完全可能：

```text
收益很小
甚至略慢
```

因为原本：

```text
单 Worker
```

没有严重的 Writer Lock 竞争，却新增了一层调度。

因此 V6-A 真正需要验证的是：

```text
随着 Workers 从 1 → 2 → 4 → 8
```

它能否避免旧架构的：

```text
SQLite contention scalability degradation
```

而不是要求：

```text
1 Worker 必须明显变快
```

---

# 47. V6-A 可能出现的新等待形式

旧实现 profiling 可能主要看到：

```text
clock_nanosleep
→ sqliteDefaultBusyCallback
```

V6-A 后：

```text
SQLite busy sleep
```

理论上应该下降。

但不代表：

```text
所有等待 syscall 都下降
```

因为 Producer / Worker 现在会：

```text
future.get()
```

Writer Thread 也会：

```text
condition_variable.wait()
```

所以等待可能转移为：

```text
futex
condition_variable
```

这是正常现象。

真正需要比较的是：

```text
E2E wall time
Throughput
CPU utilization
context switches
SQLite busy stack
```

而不是只看某一个 syscall 数字。

---

# 48. V6-A 的正确性不变量

V6-A 改完以后，下面这些条件必须继续成立。

## 48.1 Claim 原子性不变

```text
READY
↓
ClaimNextReady transaction
↓
RUNNING + owner_epoch + attempt
```

仍然由 Repository 事务保证。

---

## 48.2 Epoch fencing 不变

旧 executor 不能因为 Dedicated Writer 的存在绕过：

```text
current_epoch
owner_epoch
attempt_id
```

检查。

当前仍然由 Repository 校验。

---

## 48.3 MarkTempWritten 顺序不变

必须：

```text
Copy
↓
fdatasync(temp)
↓
MarkTempWritten COMMIT
↓
Verify
```

不能：

```text
MarkTempWritten 只是排队
↓
直接 Verify
```

当前 promise/future 保证这一点。

---

## 48.4 VerifiedReceipt durability 不变

必须：

```text
Verify temp
↓
PersistVerifiedReceipt
↓
COMMIT
↓
ACK
```

然后才允许后续 publish / recovery 使用这个 durable evidence。

---

## 48.5 Retryable 状态必须真正持久化

Worker 遇到文件错误时：

```text
MarkRetryable
```

也必须通过 Writer 完成数据库事务以后再返回。

---

## 48.6 Stop 不能丢已接受 Command

```text
Command 已 Enqueue
```

就必须：

```text
执行完成
或
明确返回 fatal
```

不能无声消失。

---

# 49. V6-A 刻意没有做的事情

当前阶段没有：

```text
事务 batching
多个 command 合并一个 transaction
跨 Task transaction
减少 checkpoint
删除 event
缩短所有 BEGIN/COMMIT 区间
FULL → NORMAL
busy_timeout 调优
WAL 模式修改
```

原因是：

> V6-A 只验证“减少 Writer 数量”这一件事。

如果同时做 batching：

```text
Writer topology
+
transaction count
```

两个变量一起改变，就无法判断收益来源。

---

# 50. V6-A 与 V6-B 的边界

V6-A：

```text
减少“谁在写”
```

具体是：

```text
N Writer Connections
↓
1 Dedicated Writer
```

V6-B：

```text
减少“写多少次”
```

未来可能包括：

```text
缩短事务
Prepared Statement 重用
减少重复查询
Opportunistic Batching
减少 COMMIT 次数
```

所以正确顺序是：

```text
V6-A
↓
重新 Benchmark / profiling
↓
确认 Writer Lock contention 是否下降
↓
确认新的瓶颈
↓
数据支持时才进入 V6-B
```

---

# 51. 当前 V6-A 代码层完成状态

按照 V6-A 的核心代码目标：

```text
[✓] MigrationRuntimeStore
[✓] TaskRuntimeRepository 实现 MigrationRuntimeStore
[✓] RuntimeDbWriter
[✓] Writer 独占 SqliteConnection
[✓] Writer Thread + Command Queue
[✓] Producer Claim 进入 Writer
[✓] Worker MarkCommitIntent 进入 Writer
[✓] Worker MarkTempWritten 进入 Writer
[✓] PersistVerifiedReceipt 进入 Writer
[✓] MarkRetryable 进入 Writer
[✓] 删除 per-worker SQLite 写连接
[✓] MigrationAttemptPreparer 改依赖抽象
[✓] ACK 等 Repository 返回
[✓] Stop drain
[✓] Repository error 传播
[✓] RuntimeDbWriter 集成测试
[✓] CMake 正式接入
```

从代码改造角度：

> **V6-A 主体已经完成。**

---

# 52. 当前还不能写成“性能优化完成”的原因

代码改造正确：

```text
≠
已经证明性能收益
```

下一阶段仍然必须用数据回答：

```text
SQLite busy backoff 到底下降多少？
4 Worker E2E 是否改善？
8 Worker scalability 是否改善？
context-switches 是否下降？
新的热点变成什么？
```

所以当前更准确的阶段名称是：

```text
V6-A Code Complete
↓
等待 Benchmark / Profiling Verification
```

而不是：

```text
V6-A Performance Verified
```

---

# 53. 后续 Benchmark 要验证的核心假设

## 假设 1：1 Worker

```text
V6-A ≈ V5
```

甚至可能：

```text
V6-A 略慢
```

因为新增：

```text
Queue + future
```

但原本几乎没有 Writer contention。

---

## 假设 2：2 / 4 / 8 Worker

随着 Worker 增加，V5：

```text
多个连接抢 Writer Lock
```

问题会更加明显。

V6-A 应该体现：

```text
更稳定的 scalability
```

---

## 假设 3：SQLite busy stack

V5 中：

```text
sqliteDefaultBusyCallback
→ clock_nanosleep
```

应在 V6-A 中：

```text
显著下降
或基本消失
```

这是验证架构目标最直接的证据之一。

---

# 54. V6-A 后续最关键的性能指标

建议正式记录：

```text
Mean wall time
Median
P95
P99
Throughput
CPU time
CPU utilization
RSS
read bytes
write bytes
context switches
cpu migrations
page faults
```

以及：

```text
strace -f -c
perf stat
strace -f -k -e trace=clock_nanosleep
```

---

# 55. 面试时怎么解释“为什么单 Writer 反而可能更快”

可以这样说：

> SQLite 在 WAL 模式下可以改善读写并发，但写事务本身仍然是单 Writer。原来多 Worker 每人持有一条 SQLite Connection，状态推进又频繁使用 `BEGIN IMMEDIATE`，所以多个线程实际上不是并行写数据库，而是在争抢同一个 Writer Lock。竞争失败后会进入 SQLite busy handler，产生 sleep、wakeup 和重试。
>
> V6-A 我没有降低 `synchronous=FULL`，也没有调 `busy_timeout`，而是把并发 migrate 阶段的数据库状态操作统一提交到 `RuntimeDbWriter`。文件 Copy、Hash、Verify 仍然由多个 Worker 并发执行，只有 SQLite 写事务由一个线程和一条 Connection 顺序执行。
>
> 本质上是把数据库内部的被动锁竞争改成应用层主动串行调度。因为 SQLite 原本也只能有一个 Writer，所以并没有损失真正存在的数据库写并行度，反而有机会减少 busy backoff 和无效上下文切换。

---

# 56. 面试追问：为什么不用 mutex 包住 Repository

一个更简单的做法似乎是：

```text
每个 Worker 仍然有自己的 Connection
↓
写之前抢全局 mutex
↓
拿到 mutex 才执行 SQLite
```

它也可以减少同时写。

但 Dedicated Writer 更清晰：

```text
mutex 方案：
Writer Connection 仍分散在 Worker
SQLite 生命周期仍分散
写路径仍由多个执行线程承担

Dedicated Writer：
写命令统一入口
唯一 Connection
唯一写线程
明确 Queue
明确 ACK
明确 shutdown / drain
```

Dedicated Writer 更容易回答：

```text
“并发 migrate 阶段到底有几个 SQLite Writer？”
```

答案就是：

```text
1
```

而不是：

```text
“多个 Writer，但靠 mutex 保证同时只有一个执行。”
```

---

# 57. 面试追问：为什么不用一个大事务包住所有 Task

因为 PhotoBridge 的状态写是：

```text
crash-safe checkpoint
```

不是普通批处理日志。

Worker 需要依赖：

```text
MarkCommitIntent ACK
MarkTempWritten ACK
VerifiedReceipt ACK
```

来决定是否可以进入下一阶段。

如果把很多 Task 长时间放入同一个大事务：

```text
事务持有时间变长
ACK 延迟增大
失败影响范围增大
Crash Recovery 边界改变
```

所以 V6-A 不做 batching。

Batching 是否值得做，需要留到：

```text
V6-B
```

单独证明。

---

# 58. 面试追问：为什么 Producer Claim 也必须进入 Writer

因为 `ClaimNextReady()` 不是只读操作。

它需要：

```text
BEGIN IMMEDIATE
↓
选择 READY Task
↓
更新 RUNNING
↓
绑定 epoch
↓
生成 attempt
↓
COMMIT
```

如果 Workers 通过 Dedicated Writer，但 Producer 继续自己 Claim：

```text
Dedicated Writer
+
Producer Main Writer
```

仍然存在两个 Writer。

所以完整 V6-A 必须把：

```text
ClaimNextReady
```

一起收口。

---

# 59. 面试追问：future 会不会把 Worker 串行化

不会把整个 Worker 串行化。

Worker 仍然独立并发执行：

```text
文件打开
Copy
Hash
fdatasync
Verify
```

只有遇到数据库 checkpoint 时：

```text
提交 DB Command
↓
等待对应 transaction ACK
```

因此被串行的是：

```text
SQLite write transaction
```

而不是：

```text
整个文件迁移任务
```

---

# 60. 最终前后架构总结

## V6-A 前

```text
多 Worker
↓
每个 Worker 一条 SQLite Connection
↓
每个 Worker 一个 Repository
↓
Producer 还有 Main Connection
↓
多个 BEGIN IMMEDIATE
↓
争 SQLite Writer Lock
↓
SQLITE_BUSY
↓
busy handler
↓
clock_nanosleep
```

## V6-A 后

```text
多 Worker + Producer
↓
MigrationRuntimeStore
↓
RuntimeDbWriter
↓
DB Command Queue
↓
Dedicated Writer Thread
↓
TaskRuntimeRepository
↓
单 SQLite Connection
↓
BEGIN IMMEDIATE
↓
COMMIT
↓
promise ACK
↓
调用者继续
```

---

# 61. V6-A 最核心的三层职责

最终可以把代码结构记成三层。

## 第一层：MigrationService / Worker

负责：

```text
业务编排
文件并发
Task Queue
```

不知道 SQLite 写锁细节。

---

## 第二层：RuntimeDbWriter

负责：

```text
并发请求收口
Command Queue
单线程执行
同步 ACK
Stop / Drain
Fatal wakeup
```

不负责定义业务状态机。

---

## 第三层：TaskRuntimeRepository

负责：

```text
SQL
BEGIN / COMMIT / ROLLBACK
状态转换检查
epoch fencing
ownership
event
receipt
```

不知道上层有多少 Worker。

这种分层是当前 V6-A 最重要的代码设计成果。

---

# 62. 最终结论

V6-A 的代码改造不是：

```text
“给 SQLite 加了一个线程”
```

而是一次明确的并发模型调整：

```text
V5：
Producer + N Workers
各自持有 SQLite 写连接
↓
数据库内部争抢唯一 Writer

V6-A：
Producer + N Workers
共享 RuntimeDbWriter
↓
应用层 DB Command Queue
↓
唯一 Writer Thread
↓
唯一 SQLite 写连接
```

同时刻意保持：

```text
原 SQL
原 BEGIN IMMEDIATE
原 WAL
原 synchronous=FULL
原 busy_timeout
原 epoch fencing
原 ownership
原 attempt 状态机
原 VerifiedReceipt
原 crash-safe checkpoint 顺序
```

因此这次优化的变量非常明确：

> **只改变 SQLite 写者拓扑，不改变 durability 和业务状态机。**

这使后续 V5 与 V6-A 的 Benchmark 可以真正回答：

```text
消除多连接 Writer Lock 竞争
到底能带来多少收益？
```

这也是 V6-A 最有价值的地方：

```text
不是“用了一个高级组件”
而是：

profiling 找到 SQLite busy contention
↓
分析 SQLite 单 Writer 本质
↓
调整应用层并发模型
↓
保持 durability 语义不变
↓
再用 Benchmark 验证
```

形成完整的性能工程闭环。

---

# 63. 后续文档衔接

本文只记录：

```text
V6-A 代码改造
```

下一份建议单独整理：

```text
PhotoBridge V6-A Dedicated DB Writer
性能验证与瓶颈分析报告
```

应包含：

```text
1. 测试环境
2. V5 baseline
3. V6-A Worker 1/2/4/8
4. 4 Worker 10 reps
5. Mean / Median / P95 / P99
6. Throughput
7. perf stat
8. strace -f -c
9. clock_nanosleep stack
10. SQLite busy contention 对比
11. 新瓶颈定位
12. 是否进入 V6-B
13. 面试结论
```

这样最终 V6 可以形成：

```text
代码改造报告
+
性能验证报告
```

两份互相独立、又能完整串起来的证据链。
