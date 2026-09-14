# PhotoBridge 最新代码最终收敛方案

> 审计对象：`https://github.com/hxm56669/photobridge` 当前 `main`  
> 日期：2026-09-14  
> 定位：秋招封版收尾，不再增加业务功能  
> 当前阶段：**新可靠迁移主链已经实现大半，但 CLI 仍实际运行旧 `PipelineCommand`；接下来只做主链切换、Repository 正确性补齐和最后一轮重复/死代码清理。**

---

# 1. 当前最新代码的真实状态

这次修改已经完成了不少正确方向的工作。

已经存在并且值得保留：

```text
pipeline_services.cpp
MigrationService
RecoveryService
VerifyService
MigrationAttemptPreparer
PlanExecutionLock
Reconciler
MutationGuard
DiffEngine
AuditReport
MetadataResolver
Provenance
```

其中新的 `pipeline_services.cpp` 已经形成了新的：

```text
scan
→ plan
→ migrate
→ resume
→ verify
→ status
```

分层结构。

新的 Migration 路径也已经开始使用：

```text
PlanExecutionLock
→ RUNNING 状态检查
→ ExecutionEpoch
→ Task Claim
→ MutationGuard
→ MigrationAttemptPreparer
```

新的 Recovery 路径也已经实现：

```text
Old TaskRuntime
→ VerifiedReceipt
→ Frozen Plan Validation
→ Temp / Final Observation
→ Reconciler
→ RecoverSucceeded / Retryable / Inconsistent
```

Takeout 支线已经实际接入：

```text
TakeoutParser
→ AssociationGraph
→ CanonicalPhoto
→ MetadataResolver
→ Provenance
```

同时旧的：

```text
ScanCommand
TaskGraph
TempCommit
MetadataVerifier
RelationVerifier
BoundedExecutor
DbCommandQueue
```

已经基本从 production CMake 中退出。

因此：

> **现在不再需要重新设计系统。**

当前真正的问题集中在“新代码有没有成为真实生产路径”和“Repository 底层语义是否补完整”。

---

# 2. 当前最大问题：新主链没有真正接到 CLI

现在 `cli_app.cpp` 注册的仍然是：

```text
PipelineCommand
```

实际：

```cpp
std::make_unique<PipelineCommand>(...)
```

处理：

```text
scan
plan
migrate
resume
verify
status
```

但是当前：

```text
PipelineCommand::Execute()
```

仍然保留约 1600 行旧实现。

也就是说当前实际运行关系仍然是：

```text
CLI
 ↓
PipelineCommand
 ↓
旧 scan / plan / migrate / resume / verify
```

而新的：

```text
RunPipelineStage
 ↓
ScanService
PlanService
MigrationService
RecoveryService
VerifyService
StatusService
```

虽然已经写出来，却还没有成为真正运行路径。

---

# 3. 第一目标：先让新主链真正可链接、可运行

不能直接先删除旧 `PipelineCommand`。

因为最新代码还有一个重要问题：

`TaskRuntimeRepository` 头文件已经声明：

```cpp
AcquireNextExecutionEpoch(...)
ReadCurrentEpoch(...)

RecoverSucceeded(...)
RecoverRetryable(...)
RecoverInconsistent(...)
```

新的 `pipeline_services.cpp` 也已经调用这些接口。

但是当前：

```text
src/app/task_runtime_repository.cpp
```

中还没有这些函数的完整定义。

因此当前新服务即使已经编进：

```text
photobridge_core STATIC
```

也没有真正经过 executable 的生产调用。

一旦：

```text
PipelineCommand → RunPipelineStage
```

真正接通，链接阶段就可能直接暴露这些未完成的 Repository API。

所以修改顺序必须是：

```text
先补 Repository
    ↓
确认新 Services 可链接
    ↓
再切 CLI
    ↓
最后删除旧 Pipeline 大实现
```

绝对不要反过来。

---

# 4. P0：补完整 TaskRuntimeRepository

这是下一步最高优先级。

---

## 4.1 实现 ExecutionEpoch

必须实现：

```cpp
StatusOr<ExecutionEpoch> AcquireNextExecutionEpoch(
    const std::string& plan_id);

StatusOr<ExecutionEpoch> ReadCurrentEpoch(
    const std::string& plan_id) const;
```

`AcquireNextExecutionEpoch()` 必须：

```text
BEGIN IMMEDIATE
↓
plan_id → migration_id
↓
读取 migration.current_epoch
↓
检查没有 uint64 / SQLite integer overflow
↓
current_epoch + 1
↓
UPDATE migration
↓
COMMIT
↓
返回新 epoch
```

不能在 CLI 中自己写 SQL。

---

## 4.2 ClaimNextReady 必须检查 current_epoch

当前 `ClaimNextReady()` 只接收：

```text
plan_id
epoch
attempt_id
```

然后直接 Claim READY task。

必须增加：

```text
supplied epoch
==
该 plan 对应 migration.current_epoch
```

检查。

否则一个 stale caller 仍然可能带旧 epoch 领取任务。

最终：

```text
current epoch mismatch
→ reject
```

---

# 5. P0：补完整普通 Finish Fencing

当前普通：

```text
MarkSucceeded
MarkRetryable
```

最终调用 `FinishTask()`。

现在 SQL 主要检查：

```text
task.owner_epoch
task.active_attempt_id
task.state == RUNNING
```

还不够。

必须再检查：

```text
migration.current_epoch == supplied epoch
```

因此最终语义为：

```text
task ownership match
+
attempt match
+
RUNNING
+
current global execution epoch match
↓
才允许 durable finish
```

这样：

```text
旧 Executor epoch=10
↓
新 Resume 推进 epoch=11
↓
旧 Executor 晚到 MarkSucceeded(10)
```

必须：

```text
UPDATE 0 rows
→ stale executor rejected
```

---

# 6. P0：VerifiedReceipt 必须完成真正的 fencing

`VerifiedReceipt` 是恢复链最重要的 durable evidence。

因此：

```text
PersistVerifiedReceipt
```

必须在同一个事务中确认：

```text
migration.current_epoch == receipt.owner_epoch

plan_task.state == RUNNING

plan_task.owner_epoch == receipt.owner_epoch

plan_task.active_attempt_id == receipt.attempt_id
```

全部满足后才能写 Receipt。

否则：

```text
旧 executor
```

即使已经被新 epoch fence 掉，仍有机会写一份新的：

```text
VERIFIED_DURABLE
```

证据，这是不能接受的。

---

# 7. P0：修 VerifiedReceipt 幂等

当前仍然使用：

```sql
ON CONFLICT(plan_id, task_id, attempt_id) DO NOTHING
```

这不能区分：

```text
完全相同的重试
```

和：

```text
同一个 attempt 写入了不同 evidence
```

最终必须改成：

```text
INSERT 成功
→ 写 Receipt
→ 写一次 VERIFIED_DURABLE event

发生 PK conflict
→ SELECT existing Receipt
→ 逐字段比较
```

比较：

```text
task_id
attempt_id
owner_epoch
temp_path
final_path
content_size
source_digest
target_digest
source_identity
```

结果：

```text
完全相同
→ idempotent OK
→ 不重复写 event

任何字段不同
→ INCONSISTENT / Internal Error
```

---

# 8. P0：修 ReadVerifiedReceipt

当前代码：

```cpp
if (sqlite3_step(...) == SQLITE_DONE) {
    return NotFound;
}

// 然后直接读 column
```

必须改成：

```cpp
const int step = sqlite3_step(...);

if (step == SQLITE_DONE) {
    return NotFound;
}

if (step != SQLITE_ROW) {
    return SqliteError(...);
}
```

然后再读 column。

同时检查：

```text
owner_epoch column == INTEGER
content_size column == INTEGER

owner_epoch > 0
content_size >= 0

source_digest exactly 32 bytes
target_digest exactly 32 bytes
source_identity decode success
```

这个是实际 correctness bug，必须修。

---

# 9. P0：实现 Recovery Transition API

头文件现在已经声明：

```cpp
RecoverSucceeded(...)
RecoverRetryable(...)
RecoverInconsistent(...)
```

需要在 Repository 中真正实现。

恢复不能直接调用旧：

```text
MarkSucceeded(old epoch)
```

因为 Resume 已经拥有新的：

```text
recovery_epoch
```

正确 Recovery Transition 必须同时检查：

```text
migration.current_epoch == recovery_epoch

plan_task.state == RUNNING

plan_task.owner_epoch == expected_old_epoch

plan_task.active_attempt_id == expected_old_attempt_id
```

然后完成：

---

## RecoverSucceeded

```text
task.state = SUCCEEDED
task.owner_epoch = NULL
task.active_attempt_id = NULL
```

同时：

```text
old task_attempt → COMMITTED
finished_at_ns = now
```

---

## RecoverRetryable

```text
task.state = RETRYABLE
owner cleared
attempt cleared
```

同时：

```text
old attempt → RETRYABLE
finished_at_ns = now
reason persisted
```

---

## RecoverInconsistent

```text
task.state = INCONSISTENT
owner cleared
attempt cleared
```

同时：

```text
old attempt → INCONSISTENT
finished_at_ns = now
reason persisted
```

---

# 10. P1：补 task_attempt 生命周期

当前：

```text
task_attempt
```

已经存在，但主要完成了 Claim 记录，没有完整生命周期。

它不作为恢复的唯一事实来源，但应该作为完整审计记录。

最终：

```text
Claim
→ RUNNING

准备 temp
→ COMMIT_INTENT

temp 完整写入 + fdatasync
→ TEMP_WRITTEN

Receipt durable
→ VERIFIED_DURABLE

Recovery success
→ COMMITTED

执行失败
→ RETRYABLE

恢复状态无法解释
→ INCONSISTENT
```

并写：

```text
started_at_ns
finished_at_ns
result
error
```

不要长期保留大量默认 0 时间戳。

---

# 11. P0：修 Recovery 的 SourceAvailable

新的 Recovery 当前已经检查 Source 是否能打开。

但：

```text
路径能打开
```

不能等价于：

```text
还是 Frozen Plan 中那个文件
```

例如：

```text
Frozen:
IMG001.jpg inode=100

之后：
IMG001.jpg 被删
同路径放入另一个文件 inode=200
```

此时：

```text
OpenSource()
```

仍然成功。

但是不能：

```text
source_available = true
```

否则：

```text
Receipt 存在
Temp 不存在
Final 不存在
```

时 Reconciler 可能错误选择 Retry。

正确行为：

```text
MutationGuard::Open(
    source_root,
    plan_asset.source_path,
    plan_asset.source_identity)
```

只有成功才：

```text
source_available = true
```

Source 已变化：

```text
INCONSISTENT / SOURCE_CHANGED
```

而不是 Retry 新文件。

---

# 12. Reconciler：当前修复结果保留

这一块最新代码已经改对。

必须继续保持：

```text
final_matches && temp_matches
→ AdoptFinalAndCleanupTemp

final_matches
→ AdoptFinal
```

不能再退回：

```text
final_matches && temp_exists
→ cleanup
```

因为不能删除无法证明与 Receipt 匹配的 temp。

同时：

```text
No Receipt + Temp exists
```

只能：

```text
Retry
```

不能 publish。

---

# 13. P0 完成后：切换真实生产主链

只有下面全部完成：

```text
AcquireNextExecutionEpoch implemented
ReadCurrentEpoch implemented

Claim fencing implemented
Receipt fencing implemented
Finish fencing implemented

Receipt idempotency fixed
ReadVerifiedReceipt fixed

RecoverSucceeded implemented
RecoverRetryable implemented
RecoverInconsistent implemented
```

才能切 CLI。

---

# 14. PipelineCommand 最终收敛方式

不需要让 `cli_app.cpp` 改成认识六个 Service。

保留现有：

```text
CLI
→ PipelineCommand
```

这层即可。

但是：

```cpp
PipelineCommand::Execute()
```

必须变成一个薄 wrapper。

最终只保留：

```cpp
Status PipelineCommand::Execute(CommandContext& context)
{
    return RunPipelineStage(
        stage_,
        workspace_path_,
        input_path_,
        target_path_,
        context);
}
```

或者等价实现。

最终：

```text
pipeline_command.cpp
```

应该只剩：

```text
constructor
Execute wrapper
```

目标：

```text
约 30～80 LOC
```

不再保留旧：

```text
scan implementation
plan implementation
migrate implementation
resume implementation
verify implementation
status implementation
SQLite helpers
TempNameFor
Plan Lock
Task counters
```

这些已经由：

```text
pipeline_services.cpp
Repository
PlanExecutionLock
MigrationAttemptPreparer
```

承担。

---

# 15. 切换新主链时必须跑一次“强制链接验证”

现在：

```text
photobridge_core
```

是 STATIC library。

未被 executable 真正引用的 object file 中，即使存在未定义符号，也可能暂时没有在最终链接阶段暴露。

所以：

```text
PipelineCommand → RunPipelineStage
```

一接上以后，第一件事就是完整 clean build。

必须：

```bash
rm -rf <build-dir>
重新 configure
重新 build
```

不能只 incremental build。

目的：

> 确认新的 `pipeline_services.o` 真正进入 executable，而且 Repository 所有声明都有真实 definition。

---

# 16. 新主链接通后再删旧实现

正确顺序：

```text
Repository 补齐
↓
PipelineCommand 调 RunPipelineStage
↓
clean build
↓
完整测试
↓
E2E scan → plan → migrate → resume → verify
↓
确认新链运行
↓
才删除 pipeline_command.cpp 中旧 1500+ LOC 实现
```

不要：

```text
先删旧实现
↓
然后再发现新 Services 还链接不过
```

---

# 17. P1：MutationGuard 与 CopyAndHash 去重

新 Migration 已经开始使用：

```text
MutationGuard
```

但 `CopyAndHash` 当前仍自己：

```text
StatFd before
→ copy/hash
→ StatFd after
```

同时 `CopyResult` 仍保存：

```text
source_before
source_after
```

最终应收敛。

---

## Step 1：先迁移 Receipt 数据来源

Receipt：

原：

```text
source_identity = CopyResult.source_before
```

改成：

```text
source_identity = MutationGuard.manifest_identity()
```

---

## Step 2：缩小 CopyResult

最终：

```text
CopyResult
├── bytes_copied
└── source_digest
```

删除：

```text
source_before
source_after
```

---

## Step 3：删除 CopyAndHash identity check

主流程：

```text
MutationGuard.VerifyBeforeRead
↓
CopyAndHash
↓
MutationGuard.VerifyAfterRead
```

注意：

```text
BinaryVerifier
```

自己的 fd before/after stability check 保留。

因为：

```text
MutationGuard
= Frozen Source Identity policy

BinaryVerifier
= Verify 操作期间 fd stability
```

职责不同。

---

# 18. P1：Pipeline Services 文件本身继续拆小

当前新：

```text
pipeline_services.cpp
```

已经达到约 1900 行。

虽然比把所有逻辑继续堆进：

```text
PipelineCommand
```

好，但它仍然太大。

新主链确认稳定后再拆：

```text
src/pipeline/
├── scan_service.cpp
├── plan_service.cpp
├── migration_service.cpp
├── recovery_service.cpp
├── verify_service.cpp
└── status_service.cpp
```

头文件：

```text
include/photobridge/pipeline/
```

最终：

```text
pipeline_services.cpp
```

删除。

注意：

> 这是结构收敛，不允许顺便改变业务语义。

先保证新铁路运行，再拆车站。

---

# 19. 当前已经完成的照片语义模块不要再改

最新代码已经真实接入：

```text
MetadataResolver
Provenance
```

Takeout 主链已经是：

```text
TakeoutParser
→ AssociationGraph
→ Canonical Photo
→ Metadata Resolution
→ Provenance
```

这部分本轮：

```text
KEEP
NO NEW FEATURES
```

不要继续扩：

```text
Metadata target preservation
Relation target preservation
```

秋招价值已经足够。

---

# 20. DiffEngine / AuditReport：保留现有新链

当前：

```text
DiffEngine
AuditReport
```

已经进入新 services 设计。

保留。

但它们优先级低于：

```text
Repository correctness
Production path switch
Recovery correctness
```

不要为了 Audit 展示拖慢主链切换。

---

# 21. 最后一批悬空模块：BytePermitPool / FdPermitPool

当前 production CMake 还保留：

```text
byte_permit_pool.cpp
fd_permit_pool.cpp
```

以及：

```text
byte_permit_pool_test.cpp
fd_permit_pool_test.cpp
```

但当前新 MigrationService 并没有真正使用它们。

本轮不再 HOLD 很久。

在主链稳定后直接二选一。

---

## 方案 A：不做并发迁移

推荐秋招封版选择。

直接删除：

```text
BytePermitPool
FdPermitPool
对应 tests
CMake entries
```

理由：

```text
当前 migrate 本身仍是单 task prepare
没有实际 inflight 多文件 I/O
Permit 不产生实际资源控制价值
```

---

## 方案 B：真正接入

只有在你明确决定做：

```text
多 task worker migration
```

时才保留。

否则不要为了“以后可能用”继续放在 production core。

### 本文推荐

```text
DELETE
```

因为当前秋招封版目标是：

```text
可靠恢复 > 并发吞吐扩展
```

---

# 22. 当前不要再加的功能

这轮以后禁止新增：

```text
openat2
statx
io_uring
完整 Task DAG
BoundedExecutor
DbCommandQueue
Metadata Target Preservation
Relation Target Preservation
Chunk Resume
Native App
新 Adapter
```

当前目标只是：

> **把已经写好的可靠迁移新链真正变成唯一生产链。**

---

# 23. 最新代码收敛实施顺序

严格按以下顺序。

---

## Phase 0 — 保存当前基线

记录：

```text
git rev-parse HEAD
git status

当前 build
当前 test count
```

创建：

```bash
git tag pre-final-convergence
```

---

## Phase 1 — Repository 补全

实现：

```text
AcquireNextExecutionEpoch
ReadCurrentEpoch

RecoverSucceeded
RecoverRetryable
RecoverInconsistent
```

同时增加测试。

此阶段不切 CLI。

---

## Phase 2 — Repository Fencing

修改：

```text
ClaimNextReady
PersistVerifiedReceipt
FinishTask
```

全部检查：

```text
migration.current_epoch
```

其中 Receipt 再检查：

```text
RUNNING
owner_epoch
active_attempt_id
```

---

## Phase 3 — Receipt Harden

完成：

```text
ReadVerifiedReceipt SQLITE_ROW / error handling
column types
range validation
true idempotency
conflicting receipt rejection
duplicate event prevention
```

---

## Phase 4 — task_attempt Audit

闭环：

```text
RUNNING
COMMIT_INTENT
TEMP_WRITTEN
VERIFIED_DURABLE
COMMITTED
RETRYABLE
INCONSISTENT
```

这一步不改变 Recovery 决策规则。

---

## Phase 5 — Recovery Source Identity

将新的：

```text
source_available
```

改为：

```text
Frozen identity matched
```

优先复用：

```text
MutationGuard
```

---

## Phase 6 — 新 Services 强制链接

修改：

```text
PipelineCommand::Execute()
```

只调用：

```text
RunPipelineStage(...)
```

此时做一次：

```text
clean configure
clean build
```

所有：

```text
undefined reference
```

必须在这一阶段解决。

---

## Phase 7 — 全量新链 E2E

必须真实执行：

```text
init
scan
plan
migrate
resume
verify
status
```

确认输出来自：

```text
pipeline_services
```

而不是旧实现。

---

## Phase 8 — 删除旧 Pipeline 实现

删除：

```text
pipeline_command.cpp
```

中的：

```text
SQLite helpers
Manifest readers
Plan Materialize
TempName
Migration implementation
Resume implementation
Verify implementation
Status implementation
```

只留 wrapper。

目标：

```text
30～80 LOC
```

---

## Phase 9 — MutationGuard / CopyAndHash 去重

按：

```text
Receipt identity
→ CopyResult
→ CopyAndHash
```

顺序迁移。

不要一次性删。

---

## Phase 10 — 拆 pipeline_services.cpp

在行为稳定以后拆成：

```text
scan_service
plan_service
migration_service
recovery_service
verify_service
status_service
```

每个文件尽量：

```text
100～400 LOC
```

避免再次出现新的 1900 行大文件。

---

## Phase 11 — 删除 Byte/Fd Permit

如果此时 Migration 仍然是：

```text
单 task / 无多 worker
```

删除：

```text
BytePermitPool
FdPermitPool
tests
CMake entries
```

不要继续悬空。

---

## Phase 12 — Process Crash E2E

最终必须补：

```text
partial temp
↓
SIGKILL
↓
resume

verified temp
↓
SIGKILL
↓
resume

rename completed / DB still RUNNING
↓
SIGKILL
↓
resume
```

最后：

```text
verify
```

---

## Phase 13 — README / Benchmark / 简历同步

最后才改：

```text
README
docs
benchmark
简历项目描述
```

README 只能写已经走真实 production 主链的能力。

---

# 24. 必须新增的关键测试

## Epoch

```text
AcquireEpochMonotonicallyIncreases
ClaimWithStaleEpochFails
FinishWithStaleEpochFails
ReceiptWithStaleEpochFails
```

## Receipt

```text
IdenticalReceiptReplayIsIdempotent
IdenticalReplayDoesNotDuplicateEvent
DifferentReceiptDigestFails
DifferentReceiptPathFails
DifferentReceiptIdentityFails
ReadReceiptPropagatesSqliteError
ReadReceiptRejectsInvalidEpoch
ReadReceiptRejectsInvalidSize
```

## Recovery

```text
RecoveryTransitionRequiresCurrentEpoch
RecoveryTransitionRequiresExpectedOldOwner
RecoverySourceReplacementIsNotRetryable
MatchingFinalCanBeAdopted
MismatchingFinalIsNeverOverwritten
UnverifiedTempIsNeverPublished
```

## Production Routing

增加一个非常重要的测试：

```text
PipelineCommandUsesRunPipelineStage
```

或者通过可观察的新 Service 输出 / Test Hook 证明：

```text
CLI 实际已经运行新主链
```

避免以后再次出现：

```text
新服务写好了
但真实 CLI 仍跑旧代码
```

---

# 25. 最终删除清单

已经退出或确认删除：

```text
ScanCommand
TaskGraph
TempCommit
MetadataVerifier
RelationVerifier
BoundedExecutor
DbCommandQueue
```

本轮建议继续删除：

```text
BytePermitPool
FdPermitPool
```

条件：

```text
最终没有实现多 task 并行 migration
```

---

# 26. 最终保留核心模块

最终可靠迁移核心只需要：

```text
PipelineCommand            # 薄 wrapper

ScanService
PlanService
MigrationService
RecoveryService
VerifyService
StatusService

TaskRuntimeRepository
MigrationAttemptPreparer
PlanExecutionLock

MutationGuard
CopyAndHash
BinaryVerifier
Reconciler
LinuxFileOps

CanonicalPlan
PlanArtifact
Task
DiffEngine
AuditReport
```

照片语义支线：

```text
TakeoutParser
AssociationGraph
CanonicalPhoto
MetadataResolver
ResolutionRecord
Provenance
```

LAN 支线：

```text
ServeCommand
HTTP Receiver
Upload Session
```

---

# 27. 最终代码结构目标

```text
CLI
 │
 └── PipelineCommand
          │
          ▼
     Pipeline Services
          │
    ┌─────┼──────────────┐
    │     │      │       │
   Scan  Plan  Migrate  Recovery
                  │       │
                  │       ▼
                  │   Reconciler
                  ▼
        MigrationAttemptPreparer
                  │
           MutationGuard
                  │
             CopyAndHash
                  │
            BinaryVerifier
                  │
       TaskRuntimeRepository
                  │
                SQLite
```

最终不再允许：

```text
旧 Pipeline implementation
+
新 Service implementation
```

两套长期共存。

---

# 28. 最新代码最终优先级

## 必须完成再封版

```text
P0
1. Repository missing implementation
2. Epoch global fencing
3. Receipt fencing
4. Receipt idempotency / read bug
5. Recovery Transition
6. Source Frozen Identity
7. PipelineCommand → RunPipelineStage
8. 删除旧 Pipeline implementation
```

## 应完成

```text
P1
9. task_attempt lifecycle
10. MutationGuard / CopyAndHash 去重
11. pipeline_services.cpp 拆小
12. Byte/Fd Permit 最终删除
```

## 不阻塞封版

```text
P2
13. Audit 输出格式优化
14. Benchmark 文档整理
15. README 排版
```

---

# 29. 最终验收标准

全部满足才认为 PhotoBridge 秋招封版完成。

```text
[ ] 新 Pipeline Services 是唯一生产路径

[ ] PipelineCommand < 100 LOC

[ ] task_runtime_repository.h 中所有声明都有真实 cpp definition

[ ] ExecutionEpoch 不再固定为 1

[ ] Claim 受 current_epoch fence

[ ] Receipt 受 current_epoch + task ownership fence

[ ] Finish 受 current_epoch fence

[ ] Receipt 真正内容幂等

[ ] ReadVerifiedReceipt 正确处理 SQLITE_ROW / ERROR

[ ] Recovery Transition 已实现

[ ] Recovery Source 必须匹配 Frozen Identity

[ ] Reconciler 是唯一 recovery decision table

[ ] 无 Receipt 的 Temp 永不发布

[ ] conflicting Final 永不覆盖

[ ] post-rename crash 可以 Adopt Final

[ ] MutationGuard / CopyAndHash 无重复 Frozen Identity policy

[ ] pipeline_services.cpp 已拆分或有明确后续拆分

[ ] 无 production caller 的 Byte/Fd Permit 已删除

[ ] clean build 成功

[ ] 全量 tests 成功

[ ] Sanitizer 成功

[ ] SIGKILL → resume → verify 成功

[ ] README 与真实代码一致
```

---

# 30. 收敛完成后的项目定位

收敛以后不要再把 PhotoBridge 描述成：

> 完整照片平台或完整 Google Takeout 无损语义迁移系统。

最终准确定位：

> **PhotoBridge 是一个 C++20/Linux 实现的可靠照片文件迁移引擎原型。系统通过 Frozen Manifest/Plan 固定迁移输入与执行语义，使用持久化 Task/Attempt、Execution Epoch 和 ownership fencing 防止 stale executor 更新状态；文件迁移采用临时文件、BLAKE3、`fdatasync`、独立读回验证、VerifiedReceipt、`renameat2(RENAME_NOREPLACE)` 和目录 `fsync` 建立 crash-safe 提交边界，并在重启后结合 Receipt、SQLite 状态与 Temp/Final 文件现实状态进行 Reconciliation 和恢复。**

照片语义部分作为辅助：

> **同时实现 Google Takeout 解析、Association Graph、Canonical Photo IR、确定性 Metadata Resolution 与 Provenance。**

---

# 31. 最终拍板

当前代码不需要再做大规模新设计。

剩余工作的本质只有一句话：

> **把已经写好的新可靠迁移主链补齐底层 Repository 正确性，然后真正替换旧主链。**

之后：

```text
删旧代码
→ 去重复
→ Crash E2E
→ README / Benchmark
→ 封版
```

不再增加新功能。
