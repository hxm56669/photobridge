# PhotoBridge 第二项性能优化：Worker Pool 多 Task 并行迁移代码改造记录

> 项目：PhotoBridge  
> 优化项：第二项性能优化 —— Worker Pool，多 Task 并行迁移  
> 文档目标：记录这次优化**改了什么代码、为什么这样改、改造前是什么、改造后是什么，以及整个并发执行逻辑如何闭环**。  
> 本文只讨论代码设计与执行逻辑，不记录 Benchmark、测试命令和性能结果。

---

# 一、这次优化到底在解决什么问题

PhotoBridge 原来的迁移主链本质上是一个 **单 Task 执行模型**。

即使 Frozen Plan 中存在很多可以执行的文件任务，一次 `migrate` 也只会：

```text
找到一个 READY Task
↓
Claim
↓
执行 Copy / Hash / Verify
↓
生成 VerifiedReceipt
↓
return
```

因此多文件计划实际执行方式是：

```text
Task A
↓
migrate
↓
resume

Task B
↓
migrate
↓
resume

Task C
↓
migrate
↓
resume
```

这套模型的优点是简单，状态容易推理；但它有一个明显问题：

> 一次只能处理一个文件，CPU、文件 I/O 和 Hash 能力无法被多个独立 Task 同时利用。

对于照片迁移这种天然存在大量独立文件的场景，更合理的并行粒度是：

```text
File A → Worker 1
File B → Worker 2
File C → Worker 3
File D → Worker 4
```

所以第二项优化的目标不是简单“加几个 `std::thread`”，而是把整个迁移执行模型从：

```text
单 Task 串行执行
```

改成：

```text
Producer
↓
原子 Claim READY Task
↓
Bounded Queue
↓
多个 Worker 并行执行 Task
```

同时不能破坏 PhotoBridge 原来的核心约束：

```text
Frozen Plan
Task / Attempt
Execution Epoch
VerifiedReceipt
Crash Recovery
SQLite Runtime
```

因此这次优化本质上是一次 **执行模型升级**，不是局部线程化。

---

# 二、优化前的整体执行模型

优化前 `MigrationService` 同时承担了太多职责：

```text
读取 Plan
↓
Materialize Task
↓
扫描全部 Task Runtime
↓
找到一个 READY / RETRYABLE Task
↓
确定 attempt_count
↓
生成 attempt_id
↓
Acquire execution epoch
↓
Claim Task
↓
找到对应 Plan Asset
↓
打开 Source
↓
MutationGuard
↓
打开 Target
↓
创建 Temp
↓
CopyAndHash
↓
fdatasync
↓
Verify
↓
Persist VerifiedReceipt
↓
输出结果
↓
return
```

也就是说：

> “挑任务”和“执行任务”完全耦合在 `MigrationService::Execute()` 中。

旧结构可以简化成：

```text
MigrationService
│
├─ 找 READY Task
├─ 生成 attempt_id
├─ Claim
├─ 打开 source
├─ MutationGuard
├─ 打开 target
├─ CopyAndHash
├─ Verify
├─ Persist Receipt
└─ return
```

它天然只能处理一个 Task。

---

# 三、优化前为什么不能直接 `for` 循环多执行几个 Task

最直接的想法可能是：

```cpp
for (...) {
    ClaimTask();
    ExecuteTask();
}
```

这样虽然一次 `migrate` 可以顺序处理多个 Task，但仍然是：

```text
Claim A
→ Execute A
→ Claim B
→ Execute B
→ Claim C
→ Execute C
```

本质上依旧没有并行。

另一种更危险的想法是：

```cpp
for (...) {
    std::thread(...);
}
```

直接为每个 Task 创建一个线程。

这又会产生几个问题。

## 1. 线程数量失控

假设有：

```text
10 万个 Task
```

如果按 Task 建线程，就可能造成：

```text
大量线程
↓
大量栈内存
↓
频繁上下文切换
↓
调度压力增加
```

因此线程数量必须是**固定上限**。

---

## 2. Task 领取和 Task 执行没有分层

如果每个 Worker 都自己：

```text
扫描 Plan
↓
查 READY
↓
生成 Attempt
↓
Claim
↓
执行
```

会造成大量重复逻辑。

更重要的是：

> Claim 是 SQLite 运行时状态操作，而 Copy / Hash 是文件数据面操作，两者职责不同。

所以需要拆成：

```text
Producer：负责领取任务
Worker：负责执行任务
```

---

## 3. 无界提前 Claim 会扩大 RUNNING 集合

即使只开 4 个 Worker，如果 Producer 一口气把 10 万个 Task 全部 Claim：

```text
10 万个 Task
READY
↓
RUNNING
```

但实际上真正同时执行的只有几个 Worker。

这会造成：

```text
SQLite 中大量 Task 提前进入 RUNNING
+
内存 Queue 可能持续增长
+
Crash Recovery 需要处理大量实际上还没真正开始执行的 RUNNING Task
```

因此队列必须是**有界队列**。

---

# 四、优化后的整体架构

优化后主链变成：

```text
Frozen Plan
↓
MigrationService
↓
MaterializePlan
↓
AcquireNextExecutionEpoch()   ← 一轮 migrate 只获取一次
↓
Producer
↓
TaskRuntimeRepository::ClaimNextReady()
↓
Bounded Queue
↓
┌────────────┬────────────┬────────────┐
│ Worker 1   │ Worker 2   │ Worker 3   │ ...
└────────────┴────────────┴────────────┘
      ↓             ↓             ↓
ExecuteClaimedTask()
      ↓
MutationGuard
      ↓
MigrationAttemptPreparer
      ↓
temp → Copy/Hash → fdatasync → Verify → Receipt
```

核心变化可以概括成：

```text
旧：
MigrationService = 选任务 + Claim + 执行

新：
MigrationService = 调度器

Producer = Claim

Worker = ExecuteClaimedTask
```

---

# 五、第一处改动：CLI 增加 Worker 数配置

## 优化前

原来的 `migrate` 只需要：

```text
workspace
plan
```

一次固定执行一个 Task，没有 Worker 数这个概念。

---

## 优化后

CLI 增加：

```text
--workers
```

当前约束：

```text
1 ≤ workers ≤ 8
默认 workers = 4
```

数据传递链变成：

```text
cli_app.cpp
↓
PipelineCommand
↓
RunPipelineStage
↓
RunMigrationService
↓
MigrationService::Execute
```

也就是说 Worker 数不是写死在 `MigrationService` 内，而是作为运行参数从 CLI 一直传到迁移执行层。

相关代码文件：

```text
src/cli/cli_app.cpp
include/photobridge/cli/pipeline_command.h
src/cli/pipeline_command.cpp
include/photobridge/cli/pipeline_services.h
src/pipeline/pipeline_router.cpp
src/pipeline/migration_service.cpp
```

---

## 为什么这样改

因为 Worker 数属于：

```text
执行策略参数
```

而不是：

```text
业务语义
```

Frozen Plan 决定的是：

```text
哪些文件应该迁移到哪里
```

Worker 数决定的是：

```text
这次用多少个执行线程完成这些 Task
```

因此：

```text
Plan 不需要知道 workers
Runtime 执行层才需要知道 workers
```

这保持了 Frozen Plan 的稳定性。

---

# 六、第二处改动：把“执行一个 Task”抽成 `ExecuteClaimedTask`

这是本次改造最重要的结构性变化之一。

---

## 优化前

一个 Task 的完整执行代码全部写在：

```text
MigrationService::Execute()
```

内部。

包括：

```text
Open Source Root
↓
MutationGuard
↓
Open Target Root
↓
TempNameFor
↓
1 MiB Buffer
↓
MigrationAttemptPreparer::Prepare
↓
失败则 MarkRetryable
↓
成功则返回 receipt
```

因此 `MigrationService::Execute()` 既负责调度，又负责真正的数据迁移。

---

## 优化后

把“一条已经 Claim 的 Task 怎么执行”抽成：

```cpp
ExecuteClaimedTask(...)
```

它接收：

```text
TaskRuntimeRepository&
FrozenPlanFile
MinimalPlanAsset
ClaimedTask
ExecutionEpoch
source_root
task_count
```

然后独立完成：

```text
读取 attempt_id
↓
打开 source root
↓
MutationGuard
↓
打开 target root
↓
生成 temp_name
↓
准备 1 MiB buffer
↓
MigrationAttemptPreparer::Prepare()
↓
失败 → MarkRetryable
↓
成功 → 返回输出信息
```

---

## 为什么必须抽这个函数

因为 Worker Pool 需要明确的 Worker 工作单元：

```text
Worker 拿到 QueuedTask
↓
只负责 ExecuteClaimedTask
```

如果不抽：

```text
线程函数
```

内部就必须复制大段 MigrationService 逻辑，最终会出现：

```text
调度逻辑
+
任务执行逻辑
+
线程同步逻辑
```

全部混在一个巨大函数中。

抽出来以后职责变成：

```text
MigrationService：
负责调度生命周期

ExecuteClaimedTask：
负责执行一条已经取得所有权的 Task
```

这也是从单线程迁移到并行模型最关键的代码解耦。

---

# 七、第三处改动：增加 `QueuedTask`

新的队列元素不是只有 `task_id`。

现在使用类似：

```cpp
struct QueuedTask {
    ClaimedTask claim;
    std::size_t asset_index;
};
```

它携带两个东西：

```text
ClaimedTask
+
Frozen Plan 中的 asset_index
```

---

## 为什么需要 `ClaimedTask`

因为 Worker 执行时必须知道当前 Task 的运行时所有权信息：

```text
task_id
attempt_id
owner epoch
```

这些信息是在 Claim 时生成并持久化的。

Worker 不能再重新猜。

---

## 为什么还需要 `asset_index`

SQLite Runtime 保存的是：

```text
Task 状态
```

但真正执行迁移还需要 Frozen Plan 中对应的：

```text
MinimalPlanAsset
```

其中包含：

```text
source_path
source_identity
target_path
```

所以 Producer Claim 出 Task 之后，需要把：

```text
task_id
→ Frozen Plan asset
```

建立映射。

当前实现提前建立：

```text
unordered_map<task_id, asset_index>
```

Producer Claim 到 Task 后直接查表：

```text
task_id
↓
asset_index
↓
QueuedTask
```

Worker 就不需要再次线性扫描整个 Plan。

---

# 八、第四处改动：一轮 migrate 只获取一次 execution epoch

## 优化前

一次 `migrate` 本来只执行一个 Task，因此：

```text
migrate
↓
AcquireNextExecutionEpoch
↓
Claim Task
↓
Execute Task
```

没有冲突。

---

## 多 Worker 后容易犯的错误

错误设计是：

```text
Worker 1 → Acquire epoch = 10
Worker 2 → Acquire epoch = 11
Worker 3 → Acquire epoch = 12
```

这样 Worker 3 获取新的 epoch 后：

```text
current epoch = 12
```

Worker 1 和 Worker 2 就会立刻成为：

```text
stale executor
```

后续状态更新可能被 fencing 拒绝。

---

## 现在的实现

正确流程是：

```text
一轮 MigrationService::Execute()
↓
AcquireNextExecutionEpoch()
↓
epoch = N
↓
Producer Claim A → epoch N
Producer Claim B → epoch N
Producer Claim C → epoch N
↓
Worker 1 / 2 / 3 全部使用 epoch N
```

也就是说：

> epoch 表示“一轮 migrate 执行器所有权”，而不是“一条 Worker 线程的身份”。

---

## 为什么这样设计

PhotoBridge 中 epoch 本质是：

```text
execution ownership generation
```

它用于 fencing：

```text
旧执行轮次
不能继续修改
新执行轮次拥有的 Task
```

因此：

```text
一轮 migrate
=
一个 executor generation
=
一个 epoch
```

Worker 只是这一轮 executor 内部的并行执行单元。

---

# 九、第五处改动：重构 `ClaimNextReady`

这是第二项优化的另一个核心点。

---

## 优化前

旧 `MigrationService` 会自己做很多 Claim 前置工作：

```text
遍历全部 Task
↓
寻找 READY / RETRYABLE
↓
选 task_id
↓
读取 attempt_count
↓
next_attempt_count = old + 1
↓
拼 attempt_id
↓
调用 ClaimNextReady(plan_id, epoch, attempt_id)
```

也就是说：

```text
MigrationService
```

知道太多 SQLite Runtime 内部细节。

---

## 优化后的目标

Producer 应该只表达：

```text
“请原子领取下一条可执行 READY Task。”
```

即：

```cpp
repository.ClaimNextReady(plan_id, epoch);
```

至于：

```text
选哪一个 READY Task
attempt_count 是多少
attempt_id 怎么生成
READY → RUNNING
attempt 怎么写
event 怎么写
```

应该全部由 Repository 在一个事务里完成。

---

# 十、现在 `ClaimNextReady` 的事务语义

当前 Claim 逻辑核心是：

```text
BEGIN IMMEDIATE
↓
检查当前 plan epoch
↓
确认调用者不是 stale executor
↓
SELECT 下一条可执行 READY Task
↓
读取 task_id + attempt_count
↓
生成本次 attempt_id
↓
UPDATE plan_task
READY → RUNNING
同时写 owner_epoch / active_attempt_id
↓
INSERT task_attempt
↓
INSERT RUNNING event
↓
重新读取 runtime
↓
COMMIT
↓
返回 ClaimedTask
```

这一步完成后 Worker 才真正得到：

```text
这条 Task 的执行所有权
```

---

# 十一、为什么 Claim 必须放在 SQLite 事务里

如果改成：

```text
SELECT READY Task
↓
退出事务
↓
再 UPDATE RUNNING
```

多个执行者可能出现：

```text
Producer A 看见 Task X = READY
Producer B 看见 Task X = READY

A 决定执行 X
B 也决定执行 X
```

因此必须把：

```text
选择
+
状态转换
+
Attempt 创建
```

组成同一个原子操作。

使用：

```text
BEGIN IMMEDIATE
```

使 SQLite 写事务在 Claim 阶段串行化。

所以多 Worker 并不意味着：

```text
多个线程随意抢同一条 Task
```

而是：

```text
Claim 在数据库事务层串行
+
真正的文件 Copy / Hash 并行
```

这是非常关键的设计边界。

---

# 十二、第六处改动：增加 Producer

优化前不存在 Producer。

因为一次只执行一个任务：

```text
选一个
→ 执行一个
→ return
```

即可。

---

## 优化后 Producer 的职责

Producer 主循环负责：

```text
等待 Queue 有空间
↓
ClaimNextReady()
↓
得到 ClaimedTask
↓
查 asset_index
↓
Push QueuedTask
↓
继续 Claim
```

直到：

```text
没有可执行 READY Task
```

或者：

```text
某个 Worker 已经发生错误
```

---

## Producer 不负责什么

Producer 不负责：

```text
Copy
Hash
MutationGuard
Verify
Receipt
```

这些全部交给 Worker。

所以现在形成非常清晰的职责：

```text
Producer
=
控制面
=
任务领取 / 排队

Worker
=
数据面
=
真正文件迁移
```

---

# 十三、第七处改动：增加有界阻塞队列

当前队列：

```text
std::deque<QueuedTask>
```

配合：

```text
std::mutex
std::condition_variable
```

队列容量：

```text
queue_capacity = 8
```

---

## 为什么不能使用无界队列

假设：

```text
Worker = 4
Task = 100000
```

如果 Producer 不限制 Claim：

```text
READY
↓
全部 Claim
↓
100000 个 RUNNING
↓
全部进入内存 Queue
```

真正执行能力仍然只有：

```text
4 Worker
```

这没有提高吞吐，只会造成：

```text
内存增加
+
RUNNING Task 数暴涨
+
恢复状态复杂度增加
```

---

# 十四、Backpressure 的实现逻辑

Producer 在 Claim 前先检查：

```text
queue.size() < queue_capacity
```

如果队列满：

```text
Producer
↓
condition_variable.wait()
↓
暂停 Claim
```

Worker 取出一个任务：

```text
queue.pop_front()
↓
队列腾出空间
↓
notify_all()
↓
Producer 恢复
```

所以数据流是：

```text
Producer 太快
↓
Queue = 8，满
↓
Producer 阻塞
↓
Worker 完成/取走任务
↓
Queue 出现空间
↓
Producer 再继续 Claim
```

这就是 Backpressure。

---

## Backpressure 的意义

它不是直接“加速”。

它解决的是：

```text
生产速度
>
消费速度
```

时系统不能无限堆积的问题。

因此 Worker Pool 的正确模型不是：

```text
尽可能多 Claim
```

而是：

```text
只维持有限数量的待执行 Task
```

---

# 十五、第八处改动：固定数量 Worker，而不是按 Task 创建线程

当前根据：

```text
workers
```

创建固定数量：

```cpp
std::vector<std::thread> pool;
```

例如：

```text
workers = 4
```

就只创建：

```text
Worker 0
Worker 1
Worker 2
Worker 3
```

无论 Plan 里是：

```text
10 个 Task
1000 个 Task
100000 个 Task
```

Worker 数都不会继续增长。

---

# 十六、Worker 的执行循环

每个 Worker 的逻辑可以简化成：

```text
while (true)
    ↓
等待
    ↓
producer_done || queue 非空
    ↓
queue 非空
    ↓
pop_front()
    ↓
ExecuteClaimedTask()
    ↓
保存结果 / 第一个错误
    ↓
继续等待下一条
```

Producer 完成后：

```text
producer_done = true
↓
notify_all()
```

Worker 如果发现：

```text
queue.empty()
+
producer_done
```

就退出线程。

最后主线程：

```text
join 所有 Worker
```

确保所有已经入队的任务结束以后，`MigrationService::Execute()` 才返回。

---

# 十七、第九处改动：每个 Worker 独立 SQLite Connection

这是多线程化以后必须解决的问题。

---

## 不推荐的方案

```text
Worker 1
Worker 2
Worker 3
Worker 4
↓
共用同一个 SqliteConnection
```

虽然 SQLite 可以工作在线程安全模式下，但项目中的：

```text
TaskRuntimeRepository
SqliteStatement 缓存
事务状态
```

都更适合和一个明确的 Connection 生命周期绑定。

共享连接会使：

```text
事务边界
Statement 使用
线程所有权
```

变得难以推理。

---

## 当前方案

启动 Worker 前：

```text
打开 N 个 SqliteConnection
```

例如：

```text
Worker 0 → Connection 0
Worker 1 → Connection 1
Worker 2 → Connection 2
Worker 3 → Connection 3
```

每个 Worker 内再构造：

```cpp
TaskRuntimeRepository worker_repository(worker_connections[index]);
```

因此每个 Worker 的状态写入路径是独立的。

---

## 为什么连接要在线程启动前打开

当前实现先：

```text
Open worker connections
```

然后再：

```text
start worker threads
```

这样 SQLite Connection 的：

```text
WAL
synchronous
foreign_keys
busy_timeout
```

等初始化配置在并发写事务开始之前完成。

可以减少：

```text
多个 Worker 启动时同时初始化 SQLite 配置
```

带来的额外竞争。

---

# 十八、SQLite 多 Worker 的真实并发关系

这里要特别注意：

```text
4 个 Worker
≠
4 个 SQLite Writer 真正同时提交
```

SQLite WAL 可以很好支持：

```text
多个 Reader
```

但是写事务仍然基本是：

```text
Single Writer
```

因此现在的设计是：

```text
文件 Copy / Hash
→ 多 Worker 并行

SQLite 状态更新
→ 多 Connection
→ 短事务
→ SQLite 自己协调写竞争
```

也就是说第二项优化主要并行的是：

```text
文件级数据面
```

不是把 SQLite 变成多写者数据库。

---

# 十九、第十处改动：统一错误收集

多 Worker 后不能再像旧代码一样：

```cpp
if (!status.ok()) {
    return status;
}
```

因为：

```text
Worker 1
Worker 2
Worker 3
```

可能同时已经在执行。

一个 Worker 出错时，不能直接从主线程函数返回，否则其他线程还没有正确 join。

所以当前增加：

```text
first_error
producer_error
```

---

## Worker 错误

Worker 执行：

```text
ExecuteClaimedTask
```

失败后：

```text
记录 first_error
```

只保留第一个错误用于最终返回。

Producer 观察到：

```text
first_error != OK
```

后停止继续 Claim 新任务。

但已经入队、已经运行的线程仍按线程生命周期收尾。

---

## Producer 错误

例如：

```text
ClaimNextReady()
```

返回真正的数据库错误，而不是正常的：

```text
kNotFound
```

则记录：

```text
producer_error
```

最后：

```text
join workers
↓
优先返回 producer_error / first_error
```

这保证线程生命周期不会因为中途 `return` 被破坏。

---

# 二十、第十一处改动：输出消息也需要线程安全收集

优化前只有一个 Task：

```text
context.out << ...
```

不存在并发输出竞争。

多 Worker 后如果每个 Worker 都直接：

```cpp
context.out << ...
```

可能产生交叉输出：

```text
Worker1: migrate claiWorker2: migrate ...
```

因此现在：

```text
ExecuteClaimedTask
↓
返回 std::string
```

Worker 再在锁保护下：

```text
messages.push_back(...)
```

所有线程结束后由主线程统一：

```text
context.out << message
```

这样避免多个 Worker 同时写同一个输出流。

---

# 二十一、第十二处改动：Resume 从单 RUNNING 改成多 RUNNING

这是 Worker Pool 不能绕开的状态模型变化。

---

## 优化前

由于一次 migrate 只会 Claim 一个 Task：

```text
RUNNING Task
最多 1 个
```

所以旧 Resume 明确要求：

```text
running_count == 1
```

旧逻辑大致是：

```text
扫描 Plan
↓
找到 RUNNING
↓
如果 0 个 → NotFound
↓
如果不是 1 个 → Error
↓
恢复这一条 Task
↓
return
```

也就是说：

```text
Resume
=
单 Task Recovery
```

---

# 二十二、为什么 Worker Pool 会让旧 Resume 失效

假设：

```text
workers = 4
```

Producer 可以连续 Claim：

```text
Task A → RUNNING
Task B → RUNNING
Task C → RUNNING
Task D → RUNNING
```

如果此时程序退出或者 migrate 正常 Prepare 完成：

SQLite 中可能存在：

```text
4 个 RUNNING
```

旧 Resume 会直接：

```text
running_count != 1
→ error
```

因此只修改 migrate，而不修改 resume，Worker Pool 是不完整的。

---

# 二十三、现在 Resume 怎么改

现在先收集：

```cpp
std::vector<std::size_t> running_indices;
```

即：

```text
扫描 Frozen Plan
↓
找到所有 Runtime == RUNNING 的 Task
↓
保存对应 asset index
```

然后：

```text
running_indices.empty()
→ 没有任务可恢复
```

否则：

```text
AcquireNextExecutionEpoch()
```

注意这里同样是：

> 一轮 resume 只获得一个 recovery epoch。

---

# 二十四、把单 Task Recovery 抽成 `recover_one`

原来 Resume 整个函数默认只有一个 Task。

现在把原来的单 Task 恢复逻辑包进：

```text
recover_one(selected_index)
```

内部恢复语义基本不变：

```text
读取 TaskSpec
↓
读取 Runtime
↓
检查 attempt / owner_epoch
↓
构造 CommitIntent
↓
读取 VerifiedReceipt
↓
重新观察 source
↓
重新观察 temp
↓
重新观察 final
↓
Reconciler::Decide
↓
根据 Decision 执行恢复
```

最后外层：

```text
for each running_index
    recover_one(index)
```

因此第二项优化没有重写原来的 Crash Recovery 规则。

它只是把：

```text
恢复 1 条 RUNNING
```

扩展成：

```text
依次恢复所有 RUNNING
```

---

# 二十五、为什么 Resume 第一版仍然串行

Migrate 做并行，不代表 Resume 也必须立即并行。

当前设计是：

```text
migrate
→ 并行 Prepare 多 Task

resume
→ 串行 Reconcile 多 Task
```

原因是 Resume 的主要目标是：

```text
正确恢复
```

而不是追求最高吞吐。

恢复过程涉及：

```text
receipt
temp
final
rename
directory fsync
状态收敛
```

这里如果同时再引入并行恢复，会显著增加：

```text
并发状态
错误处理
Crash Window
```

的复杂度。

因此第一版合理策略是：

```text
先把正常数据迁移并行化
↓
保留恢复路径简单、确定
```

---

# 二十六、优化前后 `MigrationService` 职责对比

## 优化前

```text
MigrationService
│
├─ 读取 Plan
├─ Materialize
├─ 找一个 READY
├─ 计算 attempt_count
├─ 生成 attempt_id
├─ Acquire epoch
├─ Claim
├─ 找 Plan Asset
├─ Open Source
├─ MutationGuard
├─ Open Target
├─ Copy / Hash
├─ Verify
├─ Receipt
└─ return
```

特点：

```text
调度 + 执行完全耦合
一次只处理一个 Task
```

---

## 优化后

```text
MigrationService
│
├─ 读取 Plan
├─ Materialize
├─ 建立 task_id → asset_index
├─ Acquire epoch once
├─ 创建 Worker Connections
├─ 创建 Bounded Queue
├─ 启动 Worker Pool
├─ Producer 循环 Claim
├─ Push Queue
├─ 等待 Workers
└─ 汇总结果
```

真正单 Task 执行：

```text
ExecuteClaimedTask
│
├─ Open Source
├─ MutationGuard
├─ Open Target
├─ MigrationAttemptPreparer
└─ 返回结果
```

特点：

```text
调度与执行解耦
固定 Worker 数
多个 Task 并行
```

---

# 二十七、优化前后 Claim 职责对比

## 优化前

```text
MigrationService
↓
自己遍历 Task
↓
自己判断 READY / RETRYABLE
↓
自己计算 attempt_count
↓
自己拼 attempt_id
↓
ClaimNextReady(plan, epoch, attempt_id)
```

问题：

```text
Service 知道太多 Repository 内部状态
```

---

## 优化后

```text
Producer
↓
ClaimNextReady(plan, epoch)
```

Repository 内部：

```text
BEGIN IMMEDIATE
↓
SELECT READY
↓
读取 attempt_count
↓
生成 attempt_id
↓
READY → RUNNING
↓
INSERT attempt
↓
INSERT event
↓
COMMIT
```

收益：

```text
任务领取成为真正的原子 Repository 操作
```

---

# 二十八、优化前后执行并行度对比

## 优化前

```text
时间 →

Task A：████████████
Task B：            ████████████
Task C：                        ████████████
Task D：                                    ████████████
```

本质：

```text
A 完成
才能开始 B
```

---

## 优化后，以 4 Worker 为例

```text
时间 →

Worker1：Task A ████████████ → Task E ...
Worker2：Task B ████████████ → Task F ...
Worker3：Task C ████████████ → Task G ...
Worker4：Task D ████████████ → Task H ...
```

本质：

```text
不同文件之间并行
```

而每个文件内部仍保持原来的：

```text
Read
↓
Hash
↓
Write
↓
fdatasync
↓
Verify
```

第二项优化没有修改单文件内部的 I/O 算法。

这也是它和后面的：

```text
BLAKE3 优化
io_uring 优化
```

之间的重要区别。

---

# 二十九、优化前后 Resume 模型对比

## 优化前

```text
Migrate
↓
1 Task RUNNING
↓
Resume
↓
恢复这 1 个
```

约束：

```text
RUNNING count 必须等于 1
```

---

## 优化后

```text
Migrate --workers N
↓
多个 Task RUNNING
↓
Resume
↓
收集全部 RUNNING
↓
Acquire recovery epoch once
↓
recover_one(A)
↓
recover_one(B)
↓
recover_one(C)
...
```

因此状态模型正式从：

```text
single RUNNING task
```

升级为：

```text
multiple RUNNING tasks
```

这是第二项优化在 Crash Recovery 层最重要的变化。

---

# 三十、为什么 Queue Capacity 和 Worker Count 是两个概念

例如：

```text
workers = 4
queue_capacity = 8
```

表示：

```text
最多 4 个 Task 正在 Worker 中执行
+
最多 8 个 Task 已 Claim、等待 Worker
```

所以 Worker 数控制的是：

```text
并行执行数量
```

Queue Capacity 控制的是：

```text
提前准备 / 排队数量
```

二者不能混为一谈。

---

# 三十一、为什么不用一个通用 ThreadPool 类

当前实现直接在 `MigrationService` 中使用：

```text
std::thread
std::deque
std::mutex
std::condition_variable
```

而没有为了这个优化额外设计一个大型通用：

```text
ThreadPool
TaskExecutor
Scheduler
Future
Promise
```

框架。

这符合 PhotoBridge 当前代码收敛原则：

```text
只实现主链真正需要的并发能力
```

当前 Worker Pool 的需求非常明确：

```text
固定 Worker
+
单 Producer
+
FIFO 有界 Queue
+
错误停止
+
join
```

因此直接实现比增加一个通用并发框架更容易审计。

---

# 三十二、这次优化没有改变哪些东西

第二项优化只改变：

```text
Task 级执行并发模型
```

没有改变单 Task 的 Crash-safe Commit 协议。

仍然是：

```text
Claim
↓
MutationGuard
↓
Temp
↓
CopyAndHash
↓
fdatasync
↓
Independent Verify
↓
VerifiedReceipt
↓
RUNNING 等待 Resume
↓
rename-no-replace
↓
fsync(directory)
↓
SUCCEEDED
```

也没有改变：

```text
Frozen Plan
FileIdentity
MutationGuard
VerifiedReceipt
Reconciler
RENAME_NOREPLACE
```

因此这次优化的原则是：

> **改变调度方式，不改变单 Task 的正确性协议。**

---

# 三十三、为什么 Worker Pool 放在 `MigrationService`

Worker Pool 没有放进：

```text
LinuxFileOps
MigrationAttemptPreparer
CopyAndHash
TaskRuntimeRepository
```

原因是这些模块的职责都是：

```text
完成一条 Task 中的某一个能力
```

例如：

```text
LinuxFileOps
→ 文件系统操作

MigrationAttemptPreparer
→ 准备一个 attempt

TaskRuntimeRepository
→ Runtime 持久化

CopyAndHash
→ 单文件 copy + hash
```

只有：

```text
MigrationService
```

拥有：

```text
“一轮 migrate 要调度多少 Task”
```

这一业务视角。

所以 Worker Pool 应该属于：

```text
业务编排层
```

而不是底层 I/O 层。

---

# 三十四、第二项优化最终形成的职责边界

## CLI

```text
决定 Worker 数
```

---

## PipelineCommand / Router

```text
把 Worker 参数传到迁移主链
```

---

## MigrationService

```text
一轮 migrate 的总调度
+
epoch 生命周期
+
Producer
+
Bounded Queue
+
Worker Pool
```

---

## TaskRuntimeRepository

```text
原子 Claim
+
Task / Attempt / Epoch Runtime
```

---

## ExecuteClaimedTask

```text
执行一条已经取得所有权的 Task
```

---

## MigrationAttemptPreparer

```text
temp
→ copy/hash
→ fdatasync
→ independent verify
→ receipt
```

---

## RecoveryService

```text
恢复一轮 migrate 留下的全部 RUNNING Task
```

---

# 三十五、第二项优化的完整新流程

最终可以把当前代码理解成：

```text
用户：

photobridge migrate --workers 4
        │
        ↓
CLI 解析 workers
        │
        ↓
PipelineCommand
        │
        ↓
PipelineRouter
        │
        ↓
MigrationService
        │
        ├─ Read Frozen Plan
        │
        ├─ MaterializePlan
        │
        ├─ RETRYABLE → READY
        │
        ├─ 建立 task_id → asset_index
        │
        ├─ Acquire execution epoch once
        │
        ├─ Open 4 Worker SQLite Connections
        │
        ├─ 创建 capacity=8 的 Queue
        │
        └─ 启动 4 Worker
                │
                ↓
             Producer
                │
                ↓
       ClaimNextReady(plan, epoch)
                │
                ↓
          BEGIN IMMEDIATE
                │
                ├─ SELECT READY
                ├─ 生成 attempt
                ├─ READY → RUNNING
                ├─ INSERT attempt
                ├─ INSERT event
                └─ COMMIT
                │
                ↓
          QueuedTask 入队
                │
     ┌──────────┼──────────┐
     ↓          ↓          ↓
 Worker 1   Worker 2   Worker 3 ...
     │          │          │
     ↓          ↓          ↓
 ExecuteClaimedTask
     │
     ↓
 MutationGuard
     │
     ↓
 MigrationAttemptPreparer
     │
     ↓
 temp → copy/hash
     │
     ↓
 fdatasync
     │
     ↓
 verify
     │
     ↓
 VerifiedReceipt
     │
     ↓
 Task 保持 RUNNING
```

然后：

```text
photobridge resume
        │
        ↓
找到全部 RUNNING
        │
        ↓
Acquire recovery epoch once
        │
        ↓
recover_one(Task A)
        │
        ↓
recover_one(Task B)
        │
        ↓
recover_one(Task C)
        │
        ↓
...
```

---

# 三十六、优化前后核心差异总表

| 维度 | 优化前 | 优化后 |
|---|---|---|
| 一次 migrate | 1 个 Task | 多个 READY Task |
| 执行模型 | 单 Task 串行 | Producer + Worker Pool |
| Worker 数 | 固定 1 | CLI 可配置 1～8 |
| Task 执行代码 | 混在 MigrationService | 抽成 ExecuteClaimedTask |
| Task Claim | Service 参与挑选和 attempt 生成 | Repository 原子 Claim |
| execution epoch | 一个 migrate 对一个 Task | 一轮 migrate 多 Task 共用一个 epoch |
| Queue | 无 | 有界 Queue |
| Backpressure | 无 | Queue 满后 Producer 阻塞 |
| SQLite Connection | 单 Connection | Producer + 每 Worker 独立 Connection |
| RUNNING 数 | 最多 1 个 | 可以多个 |
| Resume | 恢复单 RUNNING | 串行恢复全部 RUNNING |
| 输出 | 单线程直接输出 | Worker 收集，主线程统一输出 |
| 错误返回 | 发现错误直接 return | 收集错误，先正确结束 Worker 生命周期 |
| 并行粒度 | 无 | 文件 / Task 级并行 |

---

# 三十七、为什么这项优化不是“简单线程池”

如果面试时只说：

> “我把 migrate 改成了 4 个线程并行复制。”

会丢掉这项优化最重要的工程价值。

真正的改造链是：

```text
单 Task 状态模型
↓
多 Task RUNNING
↓
Claim 必须原子化
↓
Attempt 生成下沉到 Repository
↓
一轮 migrate 共用 epoch
↓
Producer / Worker 职责分离
↓
有界 Queue
↓
Backpressure
↓
每 Worker 独立 DB Connection
↓
Resume 支持多个 RUNNING
```

所以正确理解应该是：

> **为了实现 Task 级并行，我不仅增加了 Worker，还同步重构了任务领取、执行所有权、数据库连接、队列背压和恢复模型。**

---

# 三十八、为什么这种设计适合 PhotoBridge

照片迁移的任务具有一个天然特点：

```text
不同照片文件之间通常互相独立
```

因此最合适的并行方式是：

```text
文件级并行
```

而不是立刻把一个文件内部拆成很多线程。

例如：

```text
Worker 1 → IMG001.jpg
Worker 2 → IMG002.jpg
Worker 3 → IMG003.jpg
Worker 4 → IMG004.jpg
```

每个 Worker 内仍然保持：

```text
单文件顺序 Copy / Hash / Verify
```

这样既能利用多核和 I/O 并发能力，又不会立刻把单文件 Crash-safe 逻辑变得复杂。

后面的：

```text
BLAKE3 SIMD / TBB
io_uring
```

才分别处理：

```text
单文件 Hash CPU
单文件 I/O Pipeline
```

---

# 三十九、这次优化与第一项 SQLite 优化的关系

两项优化代码模块相对独立，但性能上存在明显关系。

第一项优化解决：

```text
SQLite 高频 SQL
+
事务提交成本
```

第二项优化带来：

```text
多个 Worker
↓
更多 Task 同时推进
↓
更多 Runtime 状态更新
↓
SQLite 写竞争更明显
```

因此实施顺序是合理的：

```text
先优化 SQLite
↓
再增加 Worker
```

这样多 Worker 后不至于过早被 SQLite 状态写入拖住。

---

# 四十、这次优化与第三、第四项优化的关系

## 与 BLAKE3

Worker Pool：

```text
文件级并行
```

BLAKE3 SIMD：

```text
单线程内部向量化
```

BLAKE3 TBB：

```text
单文件 Hash 内部再并行
```

后续如果同时使用：

```text
4 Worker
×
每个 Worker 多线程 Hash
```

可能出现 CPU oversubscription。

---

## 与 io_uring

Worker Pool：

```text
多个文件同时执行
```

io_uring：

```text
单文件内部多个 I/O request 同时 in-flight
```

以后如果：

```text
Worker = 4
QD = 8
```

理论上最多可能形成大量并发 I/O。

因此最终需要联合调参，而不是所有并行参数都开到最大。

---

# 四十一、这项优化最重要的设计原则

整个第二项优化可以收敛成四句话。

## 1. Claim 和 Execute 分离

```text
Repository 原子领取
Worker 执行
```

---

## 2. 并行是有界的

```text
固定 Worker
+
固定 Queue Capacity
```

不是无限创建线程，也不是无限 Claim。

---

## 3. 一轮 migrate 只有一个执行所有权代次

```text
one migrate
→ one epoch
→ multiple workers
```

---

## 4. 并发调度不能破坏恢复协议

```text
单 Task Crash-safe Commit 不变
+
Resume 从单 RUNNING 扩展到多 RUNNING
```

---

# 四十二、最终总结

第二项优化前，PhotoBridge 的 migrate 本质是：

```text
一个命令
→ 找一个 READY Task
→ Claim
→ Copy / Hash / Verify
→ 保存 Receipt
→ return
```

它的优势是简单，但只能串行处理文件。

第二项优化后，主链变成：

```text
一个 migrate
→ 获取一个 execution epoch
→ Producer 持续原子 Claim READY Task
→ 有界 Queue 控制提前领取数量
→ 固定数量 Worker 并行执行
→ 每个 Worker 独立 SQLite Connection
→ 单 Task 原有 Crash-safe Prepare 语义保持不变
→ Resume 再串行恢复这一轮产生的全部 RUNNING Task
```

因此这次优化真正完成的是：

```text
单 Task 执行器
↓
有界、多 Worker、可恢复的 Task 调度器
```

而不是简单：

```text
加了几个 std::thread
```

从代码设计角度，这次优化最有价值的地方是：

```text
调度与执行解耦
+
Claim 原子化
+
Backpressure
+
Worker 连接隔离
+
Execution Epoch 语义保持
+
多 RUNNING Recovery
```

这使 PhotoBridge 在保留原有 Crash-safe 语义的前提下，具备了文件级并行迁移能力。
