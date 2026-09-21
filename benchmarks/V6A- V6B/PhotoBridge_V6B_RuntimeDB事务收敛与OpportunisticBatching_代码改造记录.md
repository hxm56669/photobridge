# PhotoBridge V6B：Runtime DB Transaction Convergence / Opportunistic Batching 代码改造记录

> 项目：PhotoBridge  
> 阶段：V6B  
> 主题：SQLite Runtime 写事务收敛与 Opportunistic Batching  
> 基线：V6A Dedicated DB Writer 已完成  
> 当前文档定位：**代码设计与改造记录，不包含最终性能测试结论**  
>
> 核心目标：
>
> ```text
> V6A：
> 多个 SQLite Writer
> → 单 Dedicated DB Writer
> → 解决“谁在写”
>
> V6B：
> 单 Writer 下每个 Command 一个事务
> → 多个安全 Command 共享一个事务
> → 解决“写多少次”
> ```
>
> V6B 的核心原则仍然是：
>
> ```text
> 不修改 WAL
> 不修改 synchronous=FULL
> 不修改 busy_timeout
> 不删除 crash-safe checkpoint
> 不改变 VerifiedReceipt 的业务含义
> 不允许 COMMIT 前 ACK
> ```
>
> 所以这次优化不是降低持久化等级，而是减少重复事务边界和重复事务固定成本。

---

# 1. V6B 为什么要做

V6A 已经完成了 Dedicated DB Writer。

V6A 之前是：

```text
Producer SQLite Connection
Worker 1 SQLite Connection
Worker 2 SQLite Connection
Worker 3 SQLite Connection
Worker 4 SQLite Connection
        ↓
多个连接争抢 SQLite 唯一 Writer Lock
```

V6A 改成：

```text
Producer / Worker
        ↓
DB Command Queue
        ↓
RuntimeDbWriter
        ↓
1 Writer Thread
        ↓
1 SQLite Connection
```

这样已经解决：

```text
多个应用线程
→ 同时 BEGIN IMMEDIATE
→ SQLite Writer Lock contention
→ busy handler
→ sleep / wakeup
```

的问题。

但是 V6A 之后仍然存在第二层开销：

```text
Writer 线程虽然只有一个
但：

Command A
BEGIN IMMEDIATE
SQL...
COMMIT

Command B
BEGIN IMMEDIATE
SQL...
COMMIT

Command C
BEGIN IMMEDIATE
SQL...
COMMIT
```

也就是说：

> **V6A 消除了“多个 Writer 互相竞争”，但没有减少“事务本身的数量”。**

在 `synchronous=FULL + WAL` 下，事务提交仍然不是免费操作。

Runtime 状态推进又比较频繁，例如：

```text
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
```

如果每个状态操作都独占一个事务，那么：

```text
BEGIN / COMMIT 次数
WAL transaction boundary
同步与事务固定成本
current epoch 重复读取
```

仍然会随着 Command 数量增长。

因此 V6B 的目标是：

> **在不改变 crash-safe 状态语义的前提下，让多个可以安全共享 durability boundary 的 Runtime Command 共用一次事务。**

---

# 2. V6A 优化前基线

V6B 的直接基线不是最早的 Multi-writer 版本，而是 **V6A Dedicated DB Writer**。

V6A 已经有：

```text
MigrationRuntimeStore
        ↑
RuntimeDbWriter
        ↓
TaskRuntimeRepository
        ↓
SQLite
```

并且 `RuntimeDbWriter` 已经负责：

```text
ClaimNextReady
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
MarkRetryable
```

每次调用的同步过程是：

```text
调用线程
↓
创建 promise / future
↓
Enqueue(Command)
↓
Writer Thread
↓
Execute(Command)
↓
TaskRuntimeRepository
↓
BEGIN IMMEDIATE
↓
SQL
↓
COMMIT
↓
promise.set_value(...)
↓
future.get() 返回
```

这里已经满足：

```text
ACK-after-COMMIT
```

但是问题在于：

```text
一个 Command
=
一个 Repository 调用
=
一个事务
```

---

# 3. V6A 的旧 RuntimeDbWriter 是什么样

V6A 中 Writer 主循环的逻辑可以抽象成：

```text
wait queue
↓
pop 1 command
↓
Execute(command)
↓
repository.xxx()
↓
COMMIT
↓
完成 promise
↓
再处理下一个 command
```

也就是：

```text
Queue:
[A][B][C][D]

Writer:

pop A
BEGIN
A
COMMIT
ACK A

pop B
BEGIN
B
COMMIT
ACK B

pop C
BEGIN
C
COMMIT
ACK C

pop D
BEGIN
D
COMMIT
ACK D
```

假设四个 Command 都可以安全共享事务，V6A 仍然需要：

```text
4 × BEGIN
4 × COMMIT
```

---

# 4. V6B 的核心改造

V6B 把 Writer 从：

```text
一次取 1 个 Command
```

改成：

```text
先取第 1 个 Command
↓
查看队列中当前已经存在的后续 Command
↓
如果满足安全条件
→ 顺便取入同一 batch
↓
达到 max_batch_size
或遇到第一个不兼容 Command
→ 停止收集
```

然后：

```text
BEGIN IMMEDIATE
↓
Command A SQL
↓
Command B SQL
↓
Command C SQL
↓
Command D SQL
↓
COMMIT
↓
ACK A / B / C / D
```

因此理想情况下：

```text
4 Commands

V6A：
4 transactions

V6B：
1 transaction
```

这就是：

> **Transaction Convergence：把多个独立 Runtime 状态写收敛到更少的 SQLite transaction group。**

---

# 5. 为什么叫 Opportunistic Batching

这次没有设计成：

```text
Writer 收到 1 个 Command
↓
sleep 5ms / 10ms
↓
等待更多 Command
↓
强行凑满 batch
```

而是：

```text
Writer 被唤醒
↓
立即拿队首 Command
↓
只顺便消费此刻已经在 Queue 里的兼容 Command
↓
不主动等待
```

所以它叫：

```text
Opportunistic Batching
```

可以理解为：

> **有现成的就一起做，没有就立即做，不为了攒批次人为增加等待时间。**

这样主要避免一个问题：

```text
吞吐提高
但每个 Worker 为了等 batch
额外增加固定 latency
```

V6B 希望利用 Multi-worker 自然产生的并发 Command，而不是人为制造 batch 等待窗口。

---

# 6. RuntimeDbWriter::Start 增加 max_batch_size

新的启动接口：

```cpp
RuntimeDbWriter::Start(
    database_path,
    max_batch_size);
```

默认：

```text
max_batch_size = 8
```

合法范围：

```text
1 ～ 16
```

其中：

```text
batch_size = 1
```

具有非常重要的意义。

它等价于：

```text
关闭 batching
保留 Dedicated DB Writer
```

也就是可以作为：

```text
V6A Control
```

因此后续 Benchmark 可以在同一套代码中直接比较：

```text
batch = 1
batch = 4
batch = 8
batch = 16
```

而不用切换不同分支或不同二进制。

---

# 7. 新增 RuntimeDbWriterStats

V6B 新增 Writer 内部统计：

```text
accepted_commands
commands
transaction_groups
batched_commands
largest_batch
```

它们分别回答不同问题。

## accepted_commands

表示：

```text
成功进入 DB Command Queue 的 Command 数量
```

## commands

表示：

```text
Writer 实际处理的 Command 数量
```

## transaction_groups

表示：

```text
Writer 把这些 Command 分成了多少个事务执行组
```

这是 V6B 最关键的结构性指标之一。

例如：

```text
commands = 1000
transaction_groups = 1000
```

说明基本没有 batch。

如果：

```text
commands = 1000
transaction_groups = 300
```

说明事务数量已经明显收敛。

## batched_commands

表示：

```text
进入多 Command batch 的 Command 数量
```

## largest_batch

表示：

```text
实际观察到的最大 batch 大小
```

这些指标使 V6B 不再只能靠：

```text
“wall time 好像变快了”
```

来判断 batching 是否发生。

可以直接证明：

```text
Command 有没有真的聚合
事务组有没有真的减少
```

---

# 8. V6B 的 Batch 不是任意合并

这是整次改造最重要的安全设计之一。

Writer **不会扫描整个 Queue 找任意可合并 Command**。

它只从：

```text
队首
```

开始，看：

```text
连续的后续 Command
```

只要遇到第一个不兼容 Command：

```text
立即停止当前 batch
```

所以：

```text
[A兼容][A兼容][B不兼容][A兼容]
```

不会变成：

```text
[A][A][A] 一个 batch
[B] 单独
```

而是：

```text
[A][A] batch
[B] 单独/下一组
[A] 后续再处理
```

这样做的好处是：

```text
保持 Queue FIFO 顺序
不为了 batching 重排数据库状态操作
```

这对状态机型 Runtime 写入非常重要。

---

# 9. 一个 Command 想进入同一 Batch，需要满足什么

当前代码通过 Command key 判断兼容性。

核心约束是：

```text
1. 不能是 ClaimNextReady
2. Command 类型必须相同
3. plan_id 必须相同
4. execution epoch 必须相同
5. task_id 必须不同
6. 只能取 Queue 中连续出现的兼容 Command
7. batch.size < max_batch_size
```

可以表示为：

```text
same command type
+
same plan
+
same epoch
+
different task
+
FIFO contiguous
=
允许同 Batch
```

---

# 10. 为什么必须是相同 Command 类型

例如：

```text
MarkCommitIntent(task A)
MarkCommitIntent(task B)
MarkCommitIntent(task C)
```

它们是同一个状态阶段、不同 Task。

它们之间没有：

```text
A 必须依赖 B 的结果
```

这样的任务内状态依赖。

因此更容易证明：

```text
可以共享 transaction boundary
```

但如果直接允许：

```text
MarkCommitIntent(A)
MarkTempWritten(B)
PersistVerifiedReceipt(C)
MarkRetryable(D)
```

全部混合 batching，

虽然理论上某些组合也可能安全，但：

```text
状态语义更复杂
错误传播更复杂
crash 边界更复杂
测试矩阵更复杂
```

所以当前实现采用更保守的：

> **同类型 Command 才能共享 Batch。**

这是一个有意的工程取舍。

---

# 11. 为什么同一个 Task 不能在一个 Batch 中出现两次

代码会记录当前 batch 已经包含的：

```text
task_id
```

如果下一个 Command 的 task 已经出现：

```text
停止继续合并
```

原因是同一个 Task 的状态通常存在严格顺序，例如：

```text
RUNNING
↓
COMMIT_INTENT
↓
TEMP_WRITTEN
↓
VERIFIED_DURABLE
```

即使 Command 类型看起来相同，也不应该轻易把：

```text
同一 Task 的多个状态行为
```

视为彼此独立。

所以 V6B 只 batch：

> **不同 Task 的独立状态操作。**

---

# 12. 为什么 ClaimNextReady 不进入 Batch

`ClaimNextReady` 明确返回：

```text
不可参与 runtime write batch
```

原因是 Claim 不只是普通 UPDATE。

它承担：

```text
选择一个 READY Task
↓
取得执行权
↓
写 owner_epoch
↓
生成 / 绑定 attempt
↓
返回 ClaimedTask
```

而且：

```text
下一次 Claim 的选择结果
```

可能依赖前一次 Claim 已经修改后的数据库状态。

因此 Claim 与：

```text
多个独立 Worker 的状态 checkpoint
```

不是同一类操作。

当前实现保守地保持：

```text
Claim
=
单 Command
=
单独事务
```

这使 V6B 的优化边界更清楚。

---

# 13. V6B 新的 Writer Run 主循环

新的主循环可以概括为：

```text
while (true)
{
    wait

    batch = []

    pop queue.front()
    batch.push(first)

    if first 可 batch:
        while:
            next 满足兼容条件
            &&
            batch 未达到 max_batch_size

            → pop next
            → push batch

    release queue mutex

    if batch.size == 1:
        Execute(single)
    else:
        ExecuteBatch(batch)
}
```

这里还有一个很重要的并发设计：

```text
收集 batch 时持有 DB Queue mutex

真正执行 SQLite 时：
释放 DB Queue mutex
```

所以其他 Worker 在 Writer 执行事务期间仍然可以：

```text
继续 Enqueue 新 Command
```

不会因为 SQLite transaction 本身长期占住 Queue 锁。

---

# 14. 单 Command 路径仍然保留

V6B 没有把旧路径完全删除。

如果：

```text
batch.size == 1
```

仍然走：

```text
Execute(command)
```

也就是调用原来的：

```text
repository.MarkCommitIntent(...)
repository.MarkTempWritten(...)
repository.PersistVerifiedReceipt(...)
repository.MarkRetryable(...)
```

这些操作仍然拥有自己的事务。

因此：

```text
batch=1
```

或者：

```text
当前 Queue 没有可合并 Command
```

时，系统行为会自然退化到 V6A 模式。

这个性质非常重要：

> **V6B 是 V6A 上的渐进增强，而不是完全重新实现一套 Runtime 写路径。**

---

# 15. ExecuteBatch 做了什么

当：

```text
batch.size > 1
```

Writer 进入：

```text
ExecuteBatch()
```

核心流程：

```text
统计 transaction group
↓
确定 batch 的 plan_id + epoch
↓
repository.BeginWriteBatch(plan_id, epoch)
↓
逐条 ExecuteBatchedOperation(command)
↓
全部成功
    → CommitWriteBatch()
    → Complete 所有 Command

任意一条失败
    → RollbackWriteBatch()
    → 给所有等待者返回结果
```

最核心的一点是：

```text
Complete()
```

发生在：

```text
CommitWriteBatch()
```

之后。

---

# 16. TaskRuntimeRepository 新增显式 Batch 生命周期

V6A 时 Repository 方法自己拥有事务：

```text
Repository Method
↓
BEGIN IMMEDIATE
↓
SQL
↓
COMMIT
```

V6B 新增：

```text
BeginWriteBatch()
CommitWriteBatch()
RollbackWriteBatch()
```

并在 Repository 内记录：

```text
write_batch_active_
write_batch_plan_id_
write_batch_epoch_
```

因此 Repository 可以知道：

```text
当前调用
是在普通单事务模式
还是在 RuntimeDbWriter 建立的共享事务中
```

---

# 17. BeginWriteBatch 具体做什么

`BeginWriteBatch(plan_id, epoch)`：

```text
检查当前没有 active batch
↓
检查 plan_id / epoch 参数
↓
BEGIN IMMEDIATE
↓
读取数据库当前 execution epoch
↓
确认 current_epoch == batch epoch
↓
记录：
write_batch_active = true
write_batch_plan_id = plan_id
write_batch_epoch = epoch
```

这里不是简单：

```text
BEGIN;
```

然后盲目执行一批 SQL。

它在 batch 开始时仍然执行一次：

```text
epoch fencing
```

如果数据库当前 epoch 已经变化：

```text
拒绝启动 batch
ROLLBACK
```

所以：

> **V6B 减少了重复 epoch 读取，但没有删除 epoch fencing。**

---

# 18. BatchedEpochFor 的作用

Repository 中新增：

```text
BatchedEpochFor(plan_id)
```

逻辑：

```text
如果：
write_batch_active
&&
当前 plan_id == batch plan_id

返回：
batch epoch

否则：
nullptr
```

后面的：

```text
AdvanceAttempt
FinishTask
PersistVerifiedReceipt
```

根据它判断：

```text
当前是不是处于共享事务中
```

---

# 19. 过去 Repository 的事务结构

以 `MarkCommitIntent` / `MarkTempWritten` 为例。

两者最后都会进入：

```text
AdvanceAttempt(...)
```

V6A：

```text
AdvanceAttempt
↓
BEGIN IMMEDIATE
↓
ReadCurrentEpochForPlan
↓
检查 current_epoch
↓
CheckTaskOwnership
↓
SELECT 当前 file_state
↓
检查状态转换
↓
UPDATE task_attempt
↓
INSERT task_event
↓
COMMIT
```

每个 Task 都重复一遍：

```text
BEGIN
ReadCurrentEpoch
...
COMMIT
```

假设 8 个 Worker 几乎同时到达 `MarkCommitIntent`：

```text
8 Tasks
=
8 transactions
=
8 次 current epoch read
=
8 次 COMMIT
```

---

# 20. 现在 AdvanceAttempt 的事务结构

V6B 为 `AdvanceAttempt` 增加：

```text
const ExecutionEpoch* batched_epoch
```

它先判断：

```text
owns_transaction = batched_epoch == nullptr
```

## 普通单 Command

如果：

```text
batched_epoch == nullptr
```

保持旧逻辑：

```text
BEGIN IMMEDIATE
↓
ReadCurrentEpoch
↓
...
↓
COMMIT
```

## Batch 内 Command

如果：

```text
batched_epoch != nullptr
```

则：

```text
不再 BEGIN
不再单独 ReadCurrentEpoch
使用 batch 已验证的 epoch
↓
继续做 task ownership 检查
↓
继续做 attempt state 检查
↓
UPDATE
↓
INSERT event
↓
不单独 COMMIT
↓
返回 RuntimeDbWriter
```

最终统一：

```text
CommitWriteBatch()
```

---

# 21. V6B 实际减少了什么

以同一批 8 个 `MarkCommitIntent` 为例。

## V6A

```text
Task 1:
BEGIN
ReadCurrentEpoch
CheckOwnership
ReadState
UPDATE
INSERT event
COMMIT

Task 2:
BEGIN
ReadCurrentEpoch
CheckOwnership
ReadState
UPDATE
INSERT event
COMMIT

...

Task 8:
BEGIN
ReadCurrentEpoch
CheckOwnership
ReadState
UPDATE
INSERT event
COMMIT
```

即：

```text
8 BEGIN
8 ReadCurrentEpoch
8 COMMIT
```

## V6B batch=8

```text
BEGIN
ReadCurrentEpoch 一次

Task 1:
CheckOwnership
ReadState
UPDATE
INSERT event

Task 2:
CheckOwnership
ReadState
UPDATE
INSERT event

...

Task 8:
CheckOwnership
ReadState
UPDATE
INSERT event

COMMIT
```

变成：

```text
1 BEGIN
1 ReadCurrentEpoch
1 COMMIT
```

但仍然保留：

```text
8 × task ownership check
8 × task state check
8 × UPDATE
8 × event INSERT
```

这点必须说清楚。

V6B 当前不是：

```text
把所有业务 SQL 都消灭
```

而是主要减少：

```text
事务边界
+
每事务重复 epoch read
```

---

# 22. 为什么 task ownership 检查没有删除

虽然 Batch 已经验证：

```text
plan epoch
```

但不同 Task 仍然有自己的：

```text
owner_epoch
active_attempt_id
TaskState
```

因此：

```text
batch epoch 正确
```

不代表：

```text
batch 中每个 task ownership 都一定正确
```

所以 V6B 仍然逐 Task 执行：

```text
CheckTaskOwnership
```

这是正确性优先的设计。

优化的是：

```text
可以安全共享的 transaction-level 固定成本
```

而不是把 Task 级 fencing 一起删掉。

---

# 23. FinishTask 也支持共享事务

`MarkRetryable` 最终进入：

```text
FinishTask(...)
```

V6A：

```text
BEGIN IMMEDIATE
↓
ReadCurrentEpoch
↓
UPDATE plan_task
↓
UPDATE task_attempt
↓
INSERT task_event
↓
COMMIT
```

V6B：

```text
Batch 外：
保持原逻辑

Batch 内：
复用 Batch epoch
不独立 BEGIN
不独立 COMMIT
↓
UPDATE plan_task
UPDATE task_attempt
INSERT event
↓
由外层统一 COMMIT
```

所以：

```text
不同 Task 的 MarkRetryable
```

在安全条件满足时也可以共享事务。

---

# 24. PersistVerifiedReceipt 也支持共享事务

`PersistVerifiedReceipt` 是 crash-safe 主链里非常敏感的一步。

原流程：

```text
验证 receipt 参数
↓
BEGIN IMMEDIATE
↓
ReadCurrentEpoch
↓
CheckTaskOwnership
↓
INSERT verified_receipt
↓
如果已存在：
    读取已有 receipt
    检查内容一致
↓
UPDATE attempt = VERIFIED_DURABLE
↓
INSERT VERIFIED_DURABLE event
↓
COMMIT
```

V6B 没有删除这些业务校验。

Batch 内只是：

```text
BEGIN
ReadCurrentEpoch
```

从每个 Receipt Command 自己执行，提升为：

```text
整个 batch 只执行一次
```

每个 receipt 仍然：

```text
CheckTaskOwnership
INSERT / conflict check
UpdateAttemptState
AppendTaskEvent
```

最后：

```text
一个统一 COMMIT
```

---

# 25. 最重要的正确性约束：ACK-after-COMMIT

这是 V6B 最容易做错的地方。

错误实现：

```text
BEGIN

执行 A
↓
ACK A

执行 B
↓
ACK B

执行 C
↓
ACK C

COMMIT
```

如果：

```text
ACK A 已返回 Worker
↓
进程 crash
↓
事务还没真正 COMMIT
```

Worker 曾经观察到：

```text
“MarkTempWritten 成功”
```

但数据库恢复后：

```text
根本没有这个状态
```

这会破坏 PhotoBridge 的 crash-safe checkpoint 含义。

---

# 26. V6B 当前正确的 ACK 顺序

现在实现是：

```text
BeginWriteBatch
↓
Execute A
↓
Execute B
↓
Execute C
↓
CommitWriteBatch
↓
Complete A
Complete B
Complete C
↓
future.get() 返回
```

所以：

```text
Worker 收到 Status::Ok()
```

仍然表示：

> **包含该 Command 的 SQLite transaction 已经执行 COMMIT。**

这保留了 V6A 原来的同步契约。

---

# 27. Batch 的 durability boundary 发生了什么变化

这里需要准确理解。

V6A：

```text
A COMMIT
↓
ACK A

B COMMIT
↓
ACK B
```

A、B 是两个 durability boundary。

V6B：

```text
A
B
C
↓
同一个 COMMIT
↓
ACK A/B/C
```

也就是说：

> **多个不同 Task 的同阶段状态写共享 durability boundary。**

但并没有变成：

```text
先 ACK
以后再异步持久化
```

因此：

```text
ACK 的含义没有弱化
```

只是：

```text
多个 ACK 对应同一次事务提交
```

---

# 28. Batch 中单个 Command 失败怎么办

例如：

```text
Batch:
A
B
C
D
```

执行到：

```text
A 成功
B 成功
C 失败
```

不能：

```text
只丢 C
然后把 A/B COMMIT
```

因为当前 Batch 已经被定义为：

```text
共享一个 transaction
```

所以当前实现采用：

```text
ROLLBACK 整个 Batch
```

即：

```text
A SQL
B SQL
C 失败
↓
ROLLBACK
↓
A/B 的修改也撤销
```

---

# 29. Batch 失败时如何通知等待者

当前逻辑区分：

## 真正触发失败的 Command

返回：

```text
原始 operation_error
```

## 其他因为共享事务而被回滚的 Command

返回：

```text
runtime DB batch rolled back: ...
```

这样不会出现：

```text
A 实际被 rollback
却返回 Status::Ok()
```

这是必须保证的。

---

# 30. 为什么 Batch 失败策略选择整批回滚

还有一种更复杂的实现方式：

```text
Batch 中 C 失败
↓
rollback
↓
把 A/B 重新单独执行
↓
C 返回失败
↓
D 再尝试
```

这样可能提高局部成功率。

但是会明显增加：

```text
重试语义
Command 顺序推理
重复执行
错误传播
ACK 边界
测试复杂度
```

V6B 当前选择：

```text
整批 rollback
```

优点是：

```text
事务语义简单
失败结果明确
更容易证明 crash consistency
```

对于求职项目，这个取舍是合理的。

---

# 31. RuntimeDbWriter 异常处理仍然保留

如果 Writer 内部出现 C++ 异常：

```text
catch (...)
```

当前会：

```text
尝试 RollbackWriteBatch
↓
构造 fatal Status
↓
Fail 当前 batch 所有 Command
↓
设置 fatal_
↓
设置 stopping_
↓
取出 pending queue
↓
Fail 所有 pending Command
↓
Writer Thread 退出
```

因此不会出现：

```text
Writer 已经死了
但 Worker 的 future 永久等待
```

V6A 的 shutdown / fatal 基本原则在 V6B 中仍然保留。

---

# 32. Stop 行为没有因为 Batching 改坏

`Stop()`：

```text
stopping_ = true
↓
notify writer
↓
join
```

Writer 主循环仍然：

```text
只要 Queue 中有已接受 Command
就继续处理
```

直到：

```text
Queue empty
```

才退出。

所以：

> **Stop 表示停止接受未来工作并 drain 已接受工作，而不是直接丢弃队列。**

Batching 不改变这一生命周期原则。

---

# 33. 正式 migrate 主链增加 db_batch_size

V6B 不只存在于 Benchmark。

CLI 的 `migrate` 新增：

```bash
--db-batch-size N
```

合法范围：

```text
1 ～ 16
```

默认：

```text
8
```

参数链：

```text
CLI
↓
PipelineCommand
↓
RunPipelineStage
↓
RunMigrationService
↓
MigrationService::Execute
↓
RuntimeDbWriter::Start(database, db_batch_size)
```

这说明：

> **V6B 已经接入正式 migrate 主链，不是 Benchmark 私有实验代码。**

---

# 34. 为什么把 batch size 暴露成参数

如果直接写死：

```text
batch = 8
```

后续很难做严谨 A/B。

现在可以：

```text
--db-batch-size 1
```

得到：

```text
V6A control
```

再和：

```text
--db-batch-size 4
--db-batch-size 8
--db-batch-size 16
```

比较。

因此这个参数首先是：

```text
性能实验控制变量
```

其次才是：

```text
运行时调优参数
```

---

# 35. 为什么限制最大值 16

当前 V6B 不追求超大 batch。

原因是 batch 越大：

```text
潜在吞吐 ↑
```

但同时：

```text
事务持有时间 ↑
单个失败影响范围 ↑
ACK 等待时间可能 ↑
一个 transaction 内关联的 Task 数量 ↑
```

PhotoBridge 的 Runtime 状态写属于：

```text
crash-safe checkpoint
```

不是普通日志聚合。

因此当前只允许：

```text
1 ～ 16
```

属于比较保守的小批量设计。

---

# 36. 为什么没有做固定 batch=100 / 1000

普通数据库吞吐 Benchmark 常见：

```text
100
1000
5000
```

一批。

但这里不合适直接照搬。

因为 Runtime Command 与普通 INSERT 不同。

Worker 会等待：

```text
ACK
```

所以大 batch 会影响：

```text
每个状态 checkpoint 的可见延迟
```

而且：

```text
任意一个 Command 失败
可能使整批回滚
```

所以 V6B 的目标不是：

```text
追求最大 SQLite 吞吐
```

而是：

> **在状态机正确性与延迟可接受的范围内，减少重复事务成本。**

---

# 37. 新增 Runtime State Microbenchmark

旧 SQLite benchmark：

```text
sqlite-v1
sqlite-v2
sqlite-prepared
sqlite-batch-*
```

主要测：

```text
通用 INSERT 吞吐
```

它不能直接证明：

```text
RuntimeDbWriter batching
```

因为真实 Runtime 写还包含：

```text
epoch
ownership
attempt state
event
promise/future ACK
Writer Queue
```

因此 V6B 新增：

```text
runtime-state-write
runtime-state-compare
```

专门走：

```text
RuntimeDbWriter
+
TaskRuntimeRepository
```

真实状态写路径。

---

# 38. Runtime State Benchmark 怎么工作

Benchmark 先构造：

```text
migration
migration_plan
plan_task
task_attempt
```

使 Task 已处于：

```text
RUNNING
```

状态。

然后启动：

```text
RuntimeDbWriter::Start(database, batch_size)
```

多个 runtime submitter 并发执行：

```text
MarkCommitIntent(task-N)
```

这样测试的不是简单 SQL，而是：

```text
Worker submit
↓
Queue
↓
Batch selection
↓
Repository
↓
Transaction
↓
ACK
```

整条 Runtime DB 写链。

---

# 39. Benchmark 新增 ACK Latency

V6B 不能只看：

```text
commands/s
```

因为 batching 可能出现：

```text
吞吐 ↑
但 Worker 等 ACK 更久
```

因此 Runtime Benchmark 会记录每条 Command：

```text
start time
↓
writer.MarkCommitIntent(...)
↓
return time
```

得到：

```text
ACK latency
```

后续报告可以统计：

```text
Mean
Median
P95
P99
Max
```

这使 V6B 的评价不再只是吞吐单指标。

---

# 40. Benchmark 新增 Batch 结构证据

每个 Sample 还会记录：

```text
accepted_commands
commands
transaction_groups
batched_commands
largest_batch
```

所以性能测试可以同时回答两层问题：

## 第一层：机制真的发生了吗？

```text
transaction_groups 是否下降
largest_batch 是否 > 1
batched_commands 是否 > 0
```

## 第二层：发生以后值得吗？

```text
wall time 是否下降
commands/s 是否上升
ACK P95/P99 是否可接受
CPU / IO 是否变化
```

这是比较完整的性能验证方式。

---

# 41. 新增 V6A / V6B 对照模式

Runtime Microbenchmark：

```text
runtime-state-compare
```

自动比较：

```text
batch = 1
batch = 4
batch = 8
batch = 16
```

其中：

```text
batch=1
=
v6a_dedicated_db_writer_control
```

而：

```text
batch>1
=
v6b_opportunistic_db_batching
```

这样因果关系很清楚：

```text
Dedicated Writer 架构相同
SQL 语义相同
SQLite PRAGMA 相同
唯一主要变量：
batch size
```

---

# 42. E2E Benchmark 也接入 V6B

V6B 还新增：

```text
e2e-v6b
e2e-v6b-compare
```

E2E 会真正执行：

```text
init
↓
scan
↓
plan
↓
migrate --workers N --db-batch-size N
↓
resume
↓
verify
```

因此不是只测：

```text
DB microbenchmark
```

还可以观察：

```text
完整 PhotoBridge pipeline
```

是否因为减少 Runtime transaction 而真正变快。

---

# 43. E2E 为什么必须保留 batch=1

V6B E2E compare：

```text
batch=1
batch=4
batch=8
batch=16
```

其中 `batch=1` 非常重要。

因为如果只比较：

```text
V5 老版本
vs
V6B
```

中间同时变化了：

```text
Dedicated Writer
Batching
```

无法判断收益来自哪里。

现在可以：

```text
V6A batch=1
vs
V6B batch=4/8/16
```

直接隔离：

```text
事务 batching 本身
```

的贡献。

---

# 44. 新增 Batching 集成测试

V6B 增加：

```text
OpportunisticallyBatchesQueuedIndependentTasks
```

测试核心不是只看：

```text
16 个调用都成功
```

还明确检查：

```text
stats.commands == task 数
stats.batched_commands > 0
stats.largest_batch > 1
stats.transaction_groups < stats.commands
```

也就是说，它验证：

> **系统真的形成了 multi-command transaction，而不是表面上增加 batch 参数。**

---

# 45. 如何稳定制造 Batch 进行测试

测试中有一个很有价值的技巧：

先用另一条 SQLite Connection：

```text
BEGIN IMMEDIATE
```

暂时占住 Writer。

RuntimeDbWriter 收到 Command 后：

```text
无法马上拿到事务
```

此时多个线程继续：

```text
Enqueue
```

于是 DB Command Queue 中自然积累多个请求。

之后测试释放外部事务：

```text
COMMIT
```

Writer 继续执行。

这样就能比较稳定地让：

```text
多个 Command 同时存在于 Queue
```

从而验证 opportunistic batching。

---

# 46. 新增整批回滚测试

新增：

```text
FailedCommandRollsBackItsEntireBatch
```

用于验证：

```text
一个 batch 中
某个 Command 使用错误 attempt
↓
业务校验失败
↓
整个 batch rollback
```

并检查：

```text
失败 Command 返回错误
其他同事务 Command 不会错误地持久化
```

这对 V6B 很关键。

因为只有成功路径测试是不够的。

Batching 最大风险恰恰在：

```text
部分执行成功
+
中途失败
```

时如何保持原子性。

---

# 47. 旧实现与新实现对照——架构层

## V6A

```text
Workers
↓
RuntimeDbWriter Queue
↓
1 Writer Thread
↓
1 SQLite Connection

每次：
pop 1
→ transaction
→ ACK
```

## V6B

```text
Workers
↓
RuntimeDbWriter Queue
↓
1 Writer Thread
↓
收集当前可用兼容 Command
↓
1 SQLite Connection

一次：
pop N
→ shared transaction
→ commit
→ ACK N
```

不变的是：

```text
仍然只有 1 SQLite Writer
```

变化的是：

```text
1 transaction / command
→
1 transaction / command group
```

---

# 48. 旧实现与新实现对照——事务层

## V6A

```text
A:
BEGIN
epoch read
A SQL
COMMIT

B:
BEGIN
epoch read
B SQL
COMMIT

C:
BEGIN
epoch read
C SQL
COMMIT
```

## V6B

```text
BEGIN
epoch read

A SQL
B SQL
C SQL

COMMIT
```

---

# 49. 旧实现与新实现对照——ACK 层

## V6A

```text
A COMMIT → ACK A
B COMMIT → ACK B
C COMMIT → ACK C
```

## V6B

```text
A SQL
B SQL
C SQL
↓
COMMIT
↓
ACK A
ACK B
ACK C
```

注意：

```text
ACK 次序模型发生聚合
```

但：

```text
ACK-after-COMMIT
```

契约没有改变。

---

# 50. 旧实现与新实现对照——Epoch 校验

## V6A

```text
每个 Command：
ReadCurrentEpoch
↓
compare
```

## V6B

Batch 启动时：

```text
ReadCurrentEpoch
↓
compare batch epoch
↓
缓存为 write_batch_epoch
```

每个 Batch Command：

```text
复用 batch epoch
```

但是每 Task 的：

```text
ownership / attempt
```

仍然独立检查。

---

# 51. 旧实现与新实现对照——失败范围

## V6A

一个事务只有一个 Command：

```text
Command A 失败
→ rollback A
```

## V6B

一个事务可能有多个 Command：

```text
A
B
C 失败
D
↓
rollback 整个 batch
```

所以 V6B 用：

```text
更少 transaction
```

换来了：

```text
单次 transaction 失败影响范围扩大
```

这就是 batching 的核心工程取舍之一。

---

# 52. 旧实现与新实现对照表

| 维度 | V6A | V6B |
|---|---|---|
| SQLite Writer 数 | 1 | 1 |
| DB Writer Thread | 1 | 1 |
| Command Queue | 有 | 有 |
| 单次 dequeue | 1 Command | 1～N Command |
| 最大 batch | 无 | 1～16 |
| 默认 batch | 1 的等价行为 | 8 |
| batching 方式 | 无 | Opportunistic |
| 主动 sleep 攒 batch | 无 | 无 |
| Claim batching | 无 | 禁止 |
| 跨 Command 类型 batch | 无 | 不允许 |
| 同 Task 多 Command batch | 无 | 不允许 |
| 事务数 | 约等于 Command 数 | 小于等于 Command 数 |
| current epoch read | 每事务一次 | 每 batch 一次 |
| Task ownership 检查 | 每 Command | 每 Command |
| 状态转换检查 | 每 Command | 每 Command |
| ACK | 每 Command COMMIT 后 | 整批 COMMIT 后 |
| Batch 失败 | 不存在 | 整批 rollback |
| V6A Control | 原实现 | batch=1 |
| Runtime stats | 较少 | transaction_groups 等新增 |
| ACK latency Benchmark | 无专项 | 有 |
| Runtime state Microbenchmark | 无专项 | 有 |
| E2E batch 对照 | 无 | 1/4/8/16 |

---

# 53. V6B 没有修改什么

为了保证优化因果关系，当前代码没有通过下面方式“做快”：

```text
没有把 synchronous=FULL 改成 NORMAL
没有关闭 WAL
没有删除 busy_timeout
没有减少 VerifiedReceipt
没有删除 MarkTempWritten
没有删除 Task event
没有取消 epoch fencing
没有取消 ownership 检查
没有改成 fire-and-forget
没有让 Worker 在 COMMIT 前继续
```

所以如果后续性能提升：

> 可以主要归因于事务收敛和 batching，而不是 durability 降级。

---

# 54. V6B 与 Prepared Statement 的关系

当前 Repository 原本已经存在：

```text
SqliteStatement
ReusableStatement
```

Prepared Statement 会：

```text
首次 Prepare
后续 Reset / Rebind / Reuse
```

因此 V6B 不能描述为：

```text
“这次加入 Prepared Statement，所以数据库变快”
```

这是不准确的。

更准确的是：

```text
Prepared Statement
早已解决 SQL prepare 重复开销

V6B
进一步解决 transaction boundary 重复开销
```

---

# 55. 当前 V6B 更准确的技术定位

原计划里 V6B 可以拆成：

```text
V6-B1：
缩短事务
+
减少重复 SQL

V6-B2：
Opportunistic Batching
```

当前代码最明确完成的是：

```text
1. transaction boundary convergence
2. batch 内 current epoch read 收敛
3. opportunistic batching
4. ACK-after-COMMIT
5. batch rollback
6. benchmark / stats
```

但是当前代码**没有大规模删除每 Task 的业务查询**。

例如仍然保留：

```text
CheckTaskOwnership
attempt state read
状态转换检查
receipt conflict check
```

因此面试和报告里更准确的说法应该是：

> **V6B 主要通过共享事务和 batch 级 epoch 校验减少事务固定成本，没有为了性能删除 Task 级正确性校验。**

---

# 56. 为什么这种改法比直接改 synchronous 更有价值

一个更简单的性能办法可能是：

```text
synchronous=FULL
→
synchronous=NORMAL
```

但这样同时改变：

```text
durability 语义
掉电行为
数据丢失窗口
```

性能变化就不能单纯解释为：

```text
架构优化
```

而 V6B：

```text
FULL 不变
WAL 不变
状态机不变
```

只是：

```text
更多安全 Command
共享同一次 FULL transaction commit
```

所以它更适合作为：

```text
性能工程项目
```

来讲。

---

# 57. 为什么 V6B 只有 Multi-worker 才更容易生效

Opportunistic batching 的前提是：

```text
Writer 取第一个 Command 时
Queue 里已经存在更多兼容 Command
```

如果：

```text
Worker = 1
```

常见情况是：

```text
submit
↓
future.get 等待
↓
Writer 执行
↓
ACK
↓
同一个 Worker 才提交下一个
```

Queue 不容易积累多个同类 Command。

因此：

```text
batch=8
```

不代表实际：

```text
largest_batch = 8
```

Multi-worker 越多：

```text
并发提交机会越多
```

越可能形成 batch。

这也是为什么必须记录：

```text
largest_batch
transaction_groups
batched_commands
```

而不能只看配置值。

---

# 58. V6B 的潜在瓶颈迁移

如果 V6B 成功：

```text
事务组数量下降
COMMIT 次数下降
```

那么新的热点可能转向：

```text
fdatasync(temp)
文件 Copy / Hash
Verify
SQLite 每 Task 必需 SQL
futex / future 等待
目录 fsync
```

所以 V6B 做完以后仍然需要：

```text
perf
strace
```

重新 profiling。

性能工程不是：

```text
一次优化后结束
```

而是：

```text
发现瓶颈
↓
只改一个主要变量
↓
验证
↓
重新找瓶颈
```

---

# 59. 这次代码改动涉及的主要文件

核心文件：

```text
include/photobridge/app/runtime_db_writer.h
src/app/runtime_db_writer.cpp

include/photobridge/app/task_runtime_repository.h
src/app/task_runtime_repository.cpp

include/photobridge/cli/pipeline_command.h
src/cli/pipeline_command.cpp
src/cli/cli_app.cpp

src/pipeline/migration_service.cpp

tests/integration/sqlite/runtime_db_writer_test.cpp

benchmarks/photobridge_bench.cpp
```

---

# 60. 各文件职责变化

## runtime_db_writer.h / .cpp

V6A：

```text
DB Command Queue
+ single Writer
+ single command execute
```

V6B：

```text
增加 max_batch_size
增加 RuntimeDbWriterStats
增加 batch 收集
增加 batch compatibility 判断
增加 ExecuteBatch
增加 ExecuteBatchedOperation
增加 batch completion / rollback
```

---

## task_runtime_repository.h / .cpp

V6A：

```text
每个 Runtime mutation 自己管理 transaction
```

V6B：

```text
增加：
BeginWriteBatch
CommitWriteBatch
RollbackWriteBatch
BatchedEpochFor

AdvanceAttempt / FinishTask / PersistVerifiedReceipt
同时支持：
self-owned transaction
与
external shared transaction
```

---

## cli_app / PipelineCommand / MigrationService

V6A：

```text
migrate --workers
```

V6B：

```text
migrate --workers
        --db-batch-size
```

并将参数一直传递到：

```text
RuntimeDbWriter::Start
```

---

## runtime_db_writer_test.cpp

新增重点：

```text
真正形成 batch
整批 rollback
事务组数量下降
largest_batch > 1
```

---

## photobridge_bench.cpp

新增：

```text
runtime-state-write
runtime-state-compare

e2e-v6b
e2e-v6b-compare

runtime-db-batch-size
e2e-db-batch-size

ACK latency

RuntimeDbWriterStats 输出
```

---

# 61. V6B 的完整新数据流

正式 migrate：

```text
Producer
↓
ClaimNextReady
↓
RuntimeDbWriter
↓
Claim 单独 transaction
↓
QueuedTask
↓
Worker Pool

Worker 1 / 2 / 3 / 4
↓
文件 I/O 并行
↓
状态 checkpoint
↓
RuntimeDbWriter::Enqueue
↓
DB Command Queue

Writer Thread
↓
取 Queue head
↓
检查连续兼容 Command
↓
形成 batch
↓
BeginWriteBatch
↓
执行 N 个同类不同 Task Command
↓
CommitWriteBatch
↓
统一 ACK
↓
Worker 继续
```

---

# 62. V6A 到 V6B 的本质变化

可以把整个变化压缩为一句：

```text
V6A：
用应用层单 Writer
替代 SQLite 内部多 Writer 竞争。

V6B：
在这个单 Writer 内，
再把多个独立状态写的 durability boundary 合并。
```

更完整一点：

```text
Multi-worker 文件处理仍然并行
SQLite Writer 仍然单线程
但 SQLite Transaction 不再严格一条 Command 一个
而是：
多个同阶段、同 plan、同 epoch、不同 Task 的 Command
可以共享一个事务。
```

---

# 63. 为什么不是“数据库异步化”

V6B 很容易被误讲成：

```text
“把数据库写异步化了”
```

这不准确。

Worker 调用：

```text
MarkCommitIntent(...)
```

仍然会：

```text
future.get()
```

等待数据库结果。

因此从 Worker 业务语义看：

```text
DB checkpoint 仍然是同步 barrier
```

V6B 的变化是：

```text
多个 Worker 的同步 barrier
可以由同一次 transaction COMMIT 同时释放
```

而不是：

```text
Worker 不等数据库
```

---

# 64. 为什么不是“SQLite 多事务并行”

也不能说：

```text
“V6B 让 SQLite 写事务并行了”
```

完全相反。

仍然是：

```text
1 Writer Thread
1 SQLite Writer Connection
1 active write transaction
```

V6B 是：

```text
更少的串行事务
```

不是：

```text
更多的并行事务
```

---

# 65. 为什么 V6B 有面试价值

这次优化比较适合面试的原因不是：

```text
用了 batching 这个词
```

而是它有完整性能工程链：

```text
V5 io_uring 后重新 profiling
↓
发现 SQLite contention
↓
V6A Dedicated Writer
↓
消除多连接 Writer Lock 竞争
↓
发现单 Writer 下 transaction 次数仍高
↓
V6B Transaction Convergence
↓
只 batch 可证明安全的独立状态写
↓
保持 ACK-after-COMMIT
↓
补 batch rollback
↓
增加 runtime microbenchmark + ACK latency
↓
再做 E2E A/B
```

这是完整的：

```text
测量
→ 定位
→ 改造
→ 正确性证明
→ A/B 验证
```

---

# 66. 面试中推荐怎么回答这次 V6B

可以这样讲：

> V6A 之后，并发迁移阶段已经只有一个 SQLite Writer，所以多连接争抢 Writer Lock 的问题基本收敛了。但我继续看写路径后发现，每个 Runtime Command 还是自己做一次 `BEGIN IMMEDIATE → COMMIT`，像 `MarkCommitIntent`、`MarkTempWritten`、`PersistVerifiedReceipt` 都是独立事务。
>
> 所以 V6B 我没有去降低 `synchronous=FULL`，而是在 Dedicated Writer 内做 opportunistic batching。Writer 拿到队首命令后，不主动 sleep 攒批次，只把当前已经排队、并且属于同一 Command 类型、同一 plan、同一 epoch、不同 task 的连续命令收进一个小 batch，最大支持 1～16。
>
> Repository 增加了显式的 `BeginWriteBatch / CommitWriteBatch / RollbackWriteBatch`。Batch 开始时统一做一次 epoch fencing，后面的每个 Task 仍然保留 ownership 和状态转换校验，只是不再每条命令重复 BEGIN、重复读取 current epoch 和单独 COMMIT。
>
> 最关键的是 ACK 语义没有改变。Batch 里的所有 SQL 都执行成功后先统一 COMMIT，只有 COMMIT 成功以后才完成每个 promise。如果其中一个 Command 失败，就 rollback 整个 batch，不能让已经被回滚的命令返回成功。
>
> 为了验证它，我还增加了 Runtime state microbenchmark，记录 transaction group、largest batch 和每条 Command 的 ACK P95/P99，并用 batch=1 作为 V6A control，再和 4、8、16 做 A/B。

---

# 67. 面试官可能追问

## 追问 1：为什么只 batch 同类型 Command？

回答重点：

```text
先降低正确性证明复杂度
不同 Task 的同阶段状态写最容易证明彼此独立
不为了吞吐过早做跨类型事务
```

## 追问 2：为什么不主动等 1ms 攒更多请求？

回答重点：

```text
会主动增加 ACK latency
当前选择 opportunistic
只吃已经在 Queue 中的请求
```

## 追问 3：Batch 中一个失败怎么办？

回答重点：

```text
整个 transaction rollback
失败 Command 返回原错误
其他 Command 返回 batch rollback
不能返回假成功
```

## 追问 4：为什么 Batch 后还能保证 crash-safe？

回答重点：

```text
仍然 COMMIT 后 ACK
只是多个不同 Task 共享 durability boundary
没有 fire-and-forget
没有删除 checkpoint
```

## 追问 5：为什么 Claim 不 batch？

回答重点：

```text
Claim 会改变 READY 选择和任务所有权
下一次 Claim 依赖之前数据库状态
不属于彼此独立的普通 checkpoint
```

## 追问 6：为什么 batch=16，不做 1000？

回答重点：

```text
状态写不是纯日志
batch 越大：
ACK latency ↑
事务持有时间 ↑
失败影响范围 ↑
所以先做小 batch
```

---

# 68. 当前代码阶段应该怎么定义

现在可以说：

```text
V6B 代码改造：
完成
```

具体包括：

```text
[✓] Opportunistic batching
[✓] batch size 1～16
[✓] batch=1 V6A control
[✓] 同类型 / 同 plan / 同 epoch
[✓] 不同 task
[✓] Claim 排除
[✓] FIFO 连续收集
[✓] 共享 BEGIN / COMMIT
[✓] batch 级 epoch read
[✓] Task ownership 保留
[✓] ACK-after-COMMIT
[✓] 整批 rollback
[✓] 正式 migrate 参数接入
[✓] Runtime stats
[✓] Runtime state microbenchmark
[✓] ACK latency
[✓] E2E V6B compare
[✓] batching / rollback 测试
```

但是暂时不能说：

```text
“V6B 最终性能优化已经成功”
```

因为还缺：

```text
正式 Release Benchmark 数据
1 / 4 / 8 / 16 对照数据
1 / 2 / 4 / 8 Worker 扩展性
perf
strace
COMMIT / fsync 证据
ACK P95/P99 结论
Debug + Sanitizer 实际结果
最终收益 / 复杂度判断
```

所以当前更准确的状态：

> **V6B 代码实现完成，进入验证阶段。**

---

# 69. 后续性能验证最重要的问题

后续 Benchmark 不只是问：

```text
“batch=8 快不快？”
```

而应该依次回答：

## 问题 1

```text
Batch 是否真的形成？
```

看：

```text
batched_commands
largest_batch
transaction_groups
```

## 问题 2

```text
事务数量是否真的下降？
```

看：

```text
commands
vs
transaction_groups
```

## 问题 3

```text
Runtime DB 吞吐是否提高？
```

看：

```text
commands/s
wall time
```

## 问题 4

```text
ACK latency 有没有恶化？
```

看：

```text
mean
median
P95
P99
max
```

## 问题 5

```text
E2E 是否真正收益？
```

看：

```text
1 GiB
4 Worker
batch 1/4/8/16
正式 10 次
```

## 问题 6

```text
系统调用层是否支持结论？
```

看：

```text
perf
strace
SQLite transaction / sync 路径
```

---

# 70. 一句话最终总结

```text
V6A：
把多个线程竞争 SQLite 单 Writer
改成
多个线程提交 Command，由一个 Dedicated DB Writer 串行执行。

V6B：
在 Dedicated DB Writer 内，
把当前 Queue 中已经存在、同类型、同 plan、同 epoch、不同 Task 的安全 Command
做 opportunistic batching，
让多个 Runtime 状态写共享一次 BEGIN / COMMIT 和一次 batch-level epoch 校验。

同时坚持：
COMMIT 后才 ACK，
任一 Command 失败则整批回滚，
Task ownership、状态转换、VerifiedReceipt 和 crash-safe 语义不删除。
```

---

# 71. 最终改造链路

```text
V5
io_uring 优化
↓
profiling
↓
SQLite busy / Writer contention

V6A
Dedicated DB Writer
↓
多 Writer
→ 单 Writer
↓
解决“谁在写”

V6B
Transaction Convergence
+
Opportunistic Batching
↓
一 Command 一事务
→
多个安全 Command 一事务
↓
解决“写多少次”

下一步
正式 Benchmark
↓
perf / strace
↓
验证 transaction group / ACK latency / E2E
↓
决定最终 batch size 和是否进入最终主线结论
```
