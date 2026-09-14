# PhotoBridge V4 — 秋招封版最终代码审计、正确性修复与主链收敛执行方案

> 审计基准：GitHub `hxm56669/photobridge` 当前 `main` 分支  
> 版本：V4（最终执行版）  
> 日期：2026-09-14  
> 状态：**可按 Phase 顺序开始实施**  
> 目标：不继续扩大产品范围，优先修复真实恢复协议与 fencing 缺口，统一重复实现，把成熟且有价值的模块接入真实主链，最后删除确认无生产 caller 的代码，使仓库达到秋招可展示、可追问、可验证的状态。

---

# 0. 最终原则

PhotoBridge V1 后续不再以“功能越多越好”或“已经写过的代码都不能删”为目标。

最终判断标准只有四条：

```text
1. Correctness First
2. 每个关键状态只有一个事实来源
3. 每个保留模块必须有真实 production caller
4. 每个简历技术点必须能从 CLI 追到真实代码 / SQLite / Linux 文件系统操作
```

最终路线：

```text
Recovery Correctness
        ↓
Execution Fencing
        ↓
Receipt / Attempt State
        ↓
MutationGuard 去重
        ↓
PipelineCommand 收敛
        ↓
低风险成熟模块集成
        ↓
确认死代码删除
        ↓
Crash E2E
        ↓
Benchmark 决定是否接并发模块
        ↓
Freeze V1
```

---

# 1. 当前仓库最终确认的核心问题

## 1.1 `migrate` 已经写 VerifiedReceipt，但 `resume` 没有真正消费它

当前 `migrate` 已经做：

```text
Claim Task
→ Create Temp
→ Copy + BLAKE3
→ fdatasync
→ 独立重新打开 Temp
→ BinaryVerifier
→ PersistVerifiedReceipt
→ Task 保持 RUNNING
```

但当前 `resume` 基本仍是：

```text
找到 RUNNING Task
→ 根据旧 attempt 计算 temp name
→ RenameNoReplace(temp, final)
→ FsyncDirectory
→ MarkSucceeded
```

没有做到：

```text
Read VerifiedReceipt
+
Observe Temp / Final
+
Reconciler::Decide
```

因此当前最大的真实缺口不是死代码，而是：

> **恢复路径绕过了项目已经建立的 durable verification evidence。**

---

## 1.2 ExecutionEpoch 目前只有底层 primitive，没有完整生产生命周期

当前已经存在：

```text
migration.current_epoch
plan_task.owner_epoch
plan_task.active_attempt_id
task_attempt.owner_epoch
```

同时普通完成状态已经检查：

```text
owner_epoch
+
attempt_id
+
RUNNING
```

但是：

```text
migrate 仍固定 ExecutionEpoch{1}
resume 仍只接受 owner_epoch == 1
```

因此当前准确表述只能是：

> Repository 已有 epoch/attempt ownership 基础，但 CLI 尚未完成真正的 executor epoch 生命周期。

---

## 1.3 当前 `FinishTask()` 没有检查全局 `migration.current_epoch`

仅把：

```text
migration.current_epoch: 41 → 42
```

并不能自动 fence：

```text
task.owner_epoch = 41
attempt = A
```

的旧执行者。

所以 V4 必须把：

```text
migration.current_epoch
```

真正加入所有 ownership-sensitive durable operation。

---

## 1.4 Receipt 目前的幂等语义不完整

当前：

```sql
ON CONFLICT(plan_id, task_id, attempt_id) DO NOTHING
```

然后仍继续记录：

```text
VERIFIED_DURABLE
```

存在两个问题：

```text
同 key + 不同 evidence
→ 当前可能被静默吞掉

同 key + 相同 evidence 重放
→ 当前可能重复写 event
```

V4 必须把 Receipt 改成真正的“内容幂等”。

---

## 1.5 `ReadVerifiedReceipt()` 需要补 SQLite 错误和数据完整性检查

当前读取逻辑只判断：

```text
SQLITE_DONE
```

没有先明确：

```text
step == SQLITE_ROW
```

如果 SQLite 返回：

```text
SQLITE_BUSY
SQLITE_ERROR
```

不应该继续读取 columns。

同时还应补：

```text
owner_epoch column type == INTEGER
content_size column type == INTEGER
owner_epoch > 0
content_size >= 0
```

避免坏 DB 数据被直接 cast 成 `uint64_t`。

---

## 1.6 `Reconciler` 的 temp cleanup 条件过宽

当前逻辑接近：

```text
final_matches && temp_exists
→ AdoptFinalAndCleanupTemp
```

但这里没有要求：

```text
temp_matches
```

如果：

```text
final 正确
temp 路径存在，但内容已经不是原 verified temp
```

不应自动 unlink。

V4 改为：

```text
final_matches && temp_matches
→ AdoptFinalAndCleanupTemp

final_matches && temp_exists && !temp_matches
→ AdoptFinal
→ 保留异常 temp
→ Audit
```

原则：

> **只能自动删除能够证明与 Receipt 匹配的临时文件。**

---

## 1.7 `MutationGuard` 与 `CopyAndHash` 的 Source Identity 责任重复

当前：

```text
MutationGuard
→ Source opened-fd identity before/after
```

而：

```text
CopyAndHash
→ 也 StatFd before/after
→ CopyResult 保存 source_before/source_after
```

Receipt 现在又依赖：

```text
copy.source_before
```

所以不能一次性删掉 `CopyAndHash` 的 identity 字段。

必须先迁移数据依赖，再删重复逻辑。

---

## 1.8 `task_attempt` 表目前没有形成完整生命周期

Schema 已经有：

```text
task_attempt
├── owner_epoch
├── file_state
├── started_at_ns
├── finished_at_ns
├── result
├── error_code
└── error_message
```

`FileAttemptState` 也已经定义：

```text
RUNNING
COMMIT_INTENT
TEMP_WRITTEN
VERIFIED_DURABLE
COMMITTED
RETRYABLE
INCONSISTENT
...
```

但当前 Claim 基本只插入：

```text
attempt_id / plan_id / task_id / owner_epoch / started_at_ns=0
```

后续 Receipt、Success、Retry 没有把 attempt 行闭环更新。

V4 决定：

> `task_attempt` 不参与 Recovery 的“真实世界判断”，但必须成为完整的持久化执行审计记录。

---

# 2. V4 最终可靠迁移主线

```text
init
  ↓
scan
  ↓
Frozen Source Manifest
  ↓
plan
  ↓
Frozen Plan + Digest
  ↓
materialize tasks


migrate
  ↓
Read Frozen Plan
  ↓
Acquire Plan Execution Lock
  ↓
Open DB / Ensure Schema
  ↓
Materialize Plan（若尚未 materialize）
  ↓
检查：RUNNING count 必须为 0
  ↓
找到 READY / RETRYABLE
  ↓
AcquireNextExecutionEpoch
  ↓
ClaimNextReady(epoch)
  ↓
MutationGuard::Open
  ↓
VerifyBeforeRead
  ↓
Attempt state → COMMIT_INTENT
  ↓
Create deterministic owned temp
  ↓
CopyAndHash
  ↓
VerifyAfterRead
  ↓
fdatasync(temp)
  ↓
Attempt state → TEMP_WRITTEN
  ↓
独立重新打开 temp
  ↓
BinaryVerifier
  ↓
PersistVerifiedReceipt
  ↓
Attempt state → VERIFIED_DURABLE
  ↓
Task 保持 RUNNING
  ↓
command return


resume
  ↓
Read Frozen Plan
  ↓
Acquire Plan Execution Lock
  ↓
Open DB
  ↓
检查：
RUNNING count == 1
  ↓
读取 OLD Runtime / OLD Attempt / OLD Receipt
  ↓
验证 Receipt 与 Frozen Plan 一致
  ↓
AcquireNextExecutionEpoch
  ↓
Observe Source / Temp / Final
  ↓
Reconciler::Decide
  ↓
RecoveryService Apply
  ├── Retry
  ├── ResumeCommitFromTemp
  ├── AdoptFinal
  ├── AdoptFinalAndCleanupTemp
  ├── TargetConflict
  └── Inconsistent
  ↓
Recovery transition:
current recovery epoch
+
expected old epoch
+
expected old attempt
  ↓
Task terminal/retry state
+
Attempt terminal state


verify
  ↓
MutationGuard(Source frozen identity policy)
  ↓
BinaryVerifier(Source read stability)
  ↓
BinaryVerifier(Target read stability)
  ↓
DiffEngine
  ↓
AuditReport
```

---

# 3. Plan Execution Lock：必须覆盖 migrate 和 resume

当前 lock 主要在 resume 使用。

V4 统一：

```text
migrate
resume
```

都持有：

```text
locks/<plan_id>.lock
```

独占 `flock`。

函数改名建议：

```text
AcquirePlanLock
→ AcquirePlanExecutionLock
```

错误信息：

```text
plan already has an active executor
```

而不是：

```text
plan is already being resumed
```

---

# 4. Lock 与 Materialize / Epoch 的顺序

这是 V4 最终固定顺序。

## migrate

```text
Read plan file
↓
Parse / verify plan digest
↓
Acquire Plan Execution Lock
↓
Open DB / Ensure Schema
↓
MaterializePlan
↓
Read state counts
↓
如果 RUNNING > 0：
    返回 “resume required”
    不推进 epoch
↓
准备 READY / RETRYABLE
↓
确认确实有可执行任务
↓
AcquireNextExecutionEpoch
↓
ClaimNextReady
```

重要：

> **不能先推进 epoch，再发现已有 RUNNING task。**

否则会把旧 RUNNING owner fence 掉，却没有进行恢复。

---

## resume

V1 单线程版本固定要求：

```text
RUNNING count == 1
```

流程：

```text
Read plan
↓
Acquire lock
↓
Open DB
↓
读取 state counts
↓
RUNNING == 0
→ NotFound / nothing to resume

RUNNING > 1
→ INCONSISTENT / unsupported in single-thread V1

RUNNING == 1
→ 读取 old runtime / receipt
→ 做 DB 结构验证
→ AcquireNextExecutionEpoch
→ 开始 recovery observation
```

这样不会在：

```text
没有可恢复任务
DB 已损坏
多个 RUNNING 不符合 V1 invariant
```

时无意义推进 epoch。

---

# 5. ExecutionEpoch：最终语义

## 5.1 Epoch 属于一次 executor session

每次真正开始：

```text
migrate
resume
```

执行：

```text
AcquireNextExecutionEpoch(plan_id)
```

内部在一个 `BEGIN IMMEDIATE` 中：

```text
migration_plan.plan_id
→ migration_id
→ migration.current_epoch + 1
```

返回新 epoch。

当前代码中：

```text
migration_id = "migration-" + plan_id
```

所以当前 V1 实际是一 plan 对应一个 migration。

---

## 5.2 Claim 必须验证 epoch 是当前 epoch

`ClaimNextReady()` 不能只相信调用方传入的 epoch。

同一个事务里必须验证：

```text
supplied_epoch
==
migration.current_epoch
```

否则：

```text
stale caller
```

不允许领取 READY task。

---

## 5.3 PersistVerifiedReceipt 必须同时做两层 fencing

Receipt 是关键 durable evidence，所以必须在同一个事务里验证：

```text
1. migration.current_epoch == receipt.owner_epoch

2. plan_task:
   state == RUNNING
   owner_epoch == receipt.owner_epoch
   active_attempt_id == receipt.attempt_id
```

只有全部满足才能写 Receipt。

这样旧 executor 在 epoch 被推进后：

```text
即使还拿着旧 fd
也不能再写 VERIFIED_DURABLE evidence。
```

---

## 5.4 Normal Finish 必须检查 current epoch

普通：

```text
MarkSucceeded
MarkRetryable
```

条件必须包含：

```text
task.owner_epoch == supplied_epoch
task.active_attempt_id == supplied_attempt
task.state == RUNNING
migration.current_epoch == supplied_epoch
```

更新 0 行：

```text
STALE EXECUTOR
```

---

# 6. Recovery 不创建伪新 Attempt

恢复时保留旧历史证据：

```text
old runtime:
owner_epoch = 41
attempt = A

old receipt:
owner_epoch = 41
attempt = A
```

resume 获取：

```text
recovery_epoch = 42
```

不能在 Reconcile 前修改旧 Runtime 为：

```text
epoch=42
attempt=B
```

否则会破坏：

```text
CommitIntent
VerifiedReceipt
TaskRuntime
```

三者的历史一致性。

因此 V4 仍采用：

> **旧 evidence 做判断，新 epoch 授权恢复 transition。**

---

# 7. Recovery Transition API

新增到正式 Repository：

```cpp
Status RecoverSucceeded(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch recovery_epoch,
    ExecutionEpoch expected_old_epoch,
    std::string_view expected_old_attempt_id,
    std::string_view reason);

Status RecoverRetryable(...);

Status RecoverInconsistent(...);
```

事务中同时检查：

```text
migration.current_epoch == recovery_epoch

plan_task.state == RUNNING
plan_task.owner_epoch == expected_old_epoch
plan_task.active_attempt_id == expected_old_attempt_id
```

然后：

## Success

```text
plan_task.state = SUCCEEDED
owner_epoch = NULL
active_attempt_id = NULL

old task_attempt.file_state = COMMITTED
finished_at_ns = now
```

## Retryable

```text
plan_task.state = RETRYABLE
owner_epoch = NULL
active_attempt_id = NULL

old task_attempt.file_state = RETRYABLE
finished_at_ns = now
error = reason
```

## Inconsistent

```text
plan_task.state = INCONSISTENT
owner_epoch = NULL
active_attempt_id = NULL

old task_attempt.file_state = INCONSISTENT
finished_at_ns = now
error = reason
```

同时写 task event：

```text
event.owner_epoch = recovery_epoch
detail 包含：
old_epoch
old_attempt
reconcile_action
reason
```

---

# 8. `task_attempt` 生命周期真正闭环

它是：

```text
审计记录
```

不是：

```text
Recovery 的唯一事实来源
```

Recovery 仍由：

```text
TaskRuntime
+
Receipt
+
Filesystem Reality
```

决定。

但是 attempt 行必须有真实生命周期。

建议：

```text
Claim
→ file_state = RUNNING
→ started_at_ns = real timestamp

准备执行文件副作用前
→ COMMIT_INTENT

完整 temp 已写 + fdatasync
→ TEMP_WRITTEN

Receipt 成功持久化
→ VERIFIED_DURABLE

Recovery 成功
→ COMMITTED + finished_at_ns

普通 prep error
→ RETRYABLE + finished_at_ns

Recovery inconsistency
→ INCONSISTENT + finished_at_ns
```

`task_event.created_at_ns` 同样不再长期保持默认 0。

建议把：

```text
CurrentTimeNanoseconds()
```

从 `pipeline_command.cpp` 匿名函数抽成：

```text
common/time.*
```

或 Repository 内部统一时间 helper。

---

# 9. VerifiedReceipt：最终幂等模型

## 首次写入

```text
ownership fence OK
+
current epoch OK
↓
INSERT receipt
↓
attempt → VERIFIED_DURABLE
↓
append one VERIFIED_DURABLE event
↓
COMMIT
```

## 重复相同写入

如果 primary key 已存在：

```text
SELECT existing receipt
↓
逐字段比较
```

必须比较：

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

完全相同：

```text
idempotent OK
```

且：

```text
不重复写 VERIFIED_DURABLE event
```

## 重复但内容不同

```text
INCONSISTENT / Internal Error
```

绝不静默吞掉。

---

# 10. `ReadVerifiedReceipt()` 必须修复

正确结构：

```cpp
const int step = sqlite3_step(stmt);

if (step == SQLITE_DONE) {
    return NotFound;
}

if (step != SQLITE_ROW) {
    return SqliteError(...);
}
```

之后检查 column type：

```text
task_id        TEXT
attempt_id     TEXT
owner_epoch    INTEGER
temp_path      BLOB
final_path     BLOB
content_size   INTEGER
source_digest  BLOB
target_digest  BLOB
source_identity BLOB
```

范围：

```text
owner_epoch > 0
content_size >= 0
digest exactly 32 B
source_identity decode valid
```

---

# 11. Receipt 恢复前还要验证 Frozen Plan 一致性

Receipt 不能只和 CommitIntent 比较。

RecoveryService 在调用 `Reconciler` 前必须验证：

```text
receipt.task_id == TaskSpec.id
receipt.final_path == Frozen TaskSpec.target_path
receipt.content_size == Frozen source_identity.size
receipt.source_identity == Frozen source_identity
receipt.source_digest exists
receipt.source_digest == receipt.target_digest
```

原因：

V1 是“原字节复制”。

成功的 temp 独立验证意味着：

```text
Source Digest
==
Temp/Target Digest
```

如果 Receipt 自身与 Frozen Plan 不一致：

```text
INCONSISTENT
```

不能拿它作为 adopt final 的证据。

---

# 12. CommitIntent：不新增数据库表

V4 保持：

```text
CommitIntent = durable claim + Frozen Plan 的确定性派生结果
```

新增正式 helper：

```text
BuildCommitIntent(
    plan_id,
    TaskSpec,
    TaskRuntime)
```

前置：

```text
runtime.state == RUNNING
runtime.owner_epoch > 0
runtime.attempt_id exists
```

输出：

```text
task_id
attempt_id
owner_epoch
temp_path = TempNameFor(plan_id, task_id, attempt_id)
final_path = TaskSpec.target_path
```

`TempNameFor()` 从 `pipeline_command.cpp` 匿名 namespace 移出，成为正式 migration helper。

---

# 13. Recovery 的 SourceAvailable 语义必须收紧

`ObservedFileState.source_available` 不能只是：

```text
路径存在
```

它必须表示：

```text
Source 可以打开
+
opened fd identity == Frozen Source Identity
```

也就是：

```text
MutationGuard::Open 成功
```

才算：

```text
source_available = true
```

这样 Reconciler 的：

```text
files absent + source available → Retry
```

才不会在 Source 已经被替换的情况下错误允许重做。

---

# 14. Recovery Temp Cleanup 最终安全策略

## 无 Receipt + Temp Exists

```text
绝不 publish
```

V4 默认：

```text
不自动删除
→ RecoverRetryable
→ 保留 orphan temp
→ Audit
```

原因：

没有 Receipt 时无法证明当前同名 temp 的内容仍然是可信旧 attempt 产生的内容。

下一次 retry 会生成：

```text
新的 attempt_id
→ 新 temp name
```

所以旧 temp 不阻塞执行。

后续可以增加独立 housekeeping，但不进入 V1 correctness 主链。

---

## Final Matches + Temp Matches

```text
AdoptFinalAndCleanupTemp
```

可以自动 cleanup，因为：

```text
temp path 对应 old attempt
+
size/hash 与 Receipt 一致
```

仍需：

```text
O_NOFOLLOW / regular-file policy
```

---

## Final Matches + Temp Exists But Does Not Match

```text
AdoptFinal
```

同时：

```text
保留异常 temp
→ Audit
```

不自动删除。

---

# 15. Reconciler 最终规则修正

把当前：

```text
if (final_matches && temp_exists)
    AdoptFinalAndCleanupTemp
```

改为：

```text
if (final_matches && temp_matches)
    AdoptFinalAndCleanupTemp

if (final_matches)
    AdoptFinal
```

其他核心规则保留：

```text
no receipt + final exists
→ TARGET_CONFLICT

no receipt
→ RETRY

final exists + mismatch
→ TARGET_CONFLICT

matching temp + final missing
→ RESUME_COMMIT_FROM_TEMP

receipt exists + both files missing + valid frozen source available
→ RETRY

receipt exists + files absent + source unavailable/changed
→ INCONSISTENT
```

---

# 16. RecoveryAction 执行语义

## kRetryTask

```text
不 publish
不自动删无 Receipt orphan temp
→ RecoverRetryable
```

---

## kResumeCommitFromTemp

前提：

```text
Receipt valid against Frozen Plan
temp size/hash == Receipt
final missing
```

执行：

```text
重新打开 temp
→ 再次 VerifyBinary
→ fdatasync(temp)
→ RenameNoReplace
→ FsyncDirectory
→ RecoverSucceeded
```

如果：

```text
rename 完成
→ crash before RecoverSucceeded
```

下一次：

```text
final matches receipt
→ AdoptFinal
```

闭环成立。

---

## kAdoptFinal

前提：

```text
final matches Receipt
```

执行：

```text
RecoverSucceeded
```

不重复 rename。

---

## kAdoptFinalAndCleanupTemp

执行顺序：

```text
再次确认 final matches
↓
再次确认 temp matches
↓
Unlink owned temp
↓
RecoverSucceeded
```

如果 cleanup 后再次 crash：

```text
下一次 final still matches
→ AdoptFinal
```

仍可恢复。

---

## kTargetConflict

```text
不覆盖
不自动改 final
记录 Audit
```

可以保持 RUNNING 等人工处理，或者转：

```text
INCONSISTENT / NEEDS_REVIEW
```

V1 建议统一为：

```text
INCONSISTENT
```

避免长期留下一个“看似有 owner”的 RUNNING task。

---

## kInconsistent

```text
RecoverInconsistent
```

必须人工判断，不自动重试。

---

# 17. MutationGuard：最终接法

当前 `MutationGuard` 已经有：

```cpp
manifest_identity()
```

因此 V4 **不新增 opened_identity accessor**。

需要增加的只是更适合 Frozen Plan 的 overload：

```cpp
static StatusOr<MutationGuard> Open(
    FileOps& file_ops,
    int source_root_fd,
    const RelativePath& relative_path,
    const FileIdentity& frozen_identity);
```

现有：

```cpp
Open(FileOps&, int, const PhysicalAsset&)
```

转调这个基础 overload。

---

# 18. Source Identity 数据迁移顺序

## Step A

migrate 使用：

```text
MutationGuard::Open
```

Receipt：

原：

```text
source_identity = copy.source_before
```

改为：

```text
source_identity = guard.manifest_identity()
```

---

## Step B

更新 `CopyResult`：

最终只保留：

```text
bytes_copied
source_digest
```

删除：

```text
source_before
source_after
```

---

## Step C

删除 `CopyAndHash` 内：

```text
StatFd before
StatFd after
```

主线变成：

```text
MutationGuard.VerifyBeforeRead
↓
CopyAndHash
↓
MutationGuard.VerifyAfterRead
```

---

# 19. BinaryVerifier 仍保留自己的 before/after fd stability check

不要误解成：

```text
整个项目只能有 MutationGuard 调 StatFd
```

正确职责：

### MutationGuard

唯一负责：

```text
Frozen Source Identity Policy
```

即：

```text
这个 fd 是否仍然对应 Frozen Manifest 中的 Source
```

### BinaryVerifier

继续负责：

```text
它自己正在读取的 fd
在 verify 过程中是否保持稳定
```

所以 `binary_verifier.cpp` 中的：

```text
target_before
target_after
```

保留。

Source verify 时：

```text
MutationGuard
+
BinaryVerifier
```

同时存在是合理的，二者职责不同。

---

# 20. MigrationAttemptPreparer：抽取当前成熟 Prepare 逻辑

新增：

```text
include/photobridge/migration/migration_attempt_preparer.h
src/migration/migration_attempt_preparer.cpp
```

职责：

```text
已经 claim 的 Task
+
当前 epoch
+
MutationGuard
+
Target root
↓
Attempt → COMMIT_INTENT
↓
CreateTempNoReplace
↓
CopyAndHash
↓
MutationGuard VerifyAfterRead
↓
fdatasync
↓
Attempt → TEMP_WRITTEN
↓
独立 reopen temp
↓
VerifyBinary
↓
PersistVerifiedReceipt
↓
Attempt → VERIFIED_DURABLE
```

不做：

```text
rename final
MarkSucceeded
Reconciliation
```

---

# 21. 旧 TempCommit：确认删除

旧：

```text
copy
→ fdatasync
→ rename
→ dir fsync
```

已经低于真实主链能力。

等 `MigrationAttemptPreparer` 测试稳定后：

```text
DELETE temp_commit.*
DELETE old tests
DELETE CMake entry
```

---

# 22. PipelineCommand 必须拆分

当前约 1600+ 行，职责过多。

最终目录：

```text
include/photobridge/pipeline/
├── scan_service.h
├── plan_service.h
├── migration_service.h
├── recovery_service.h
├── verify_service.h
└── status_service.h

src/pipeline/
├── scan_service.cpp
├── plan_service.cpp
├── migration_service.cpp
├── recovery_service.cpp
├── verify_service.cpp
└── status_service.cpp
```

最终：

```text
pipeline_command.cpp < 250 LOC
```

只负责：

```text
CLI stage
→ 参数
→ service dispatch
→ 输出
```

---

# 23. SQLite 从 CLI 收回 Repository

以下必须退出 `pipeline_command.cpp`：

```text
Task/Attempt SQL
Receipt SQL
Epoch SQL
Recovery Transition SQL
```

至少建立：

```text
TaskRuntimeRepository
```

作为这些状态的唯一访问入口。

可以进一步拆：

```text
ManifestRepository
PlanRepository
```

但不要为了“分层漂亮”一次重构过头。

原则：

> 先收回 correctness-sensitive SQL，再处理普通查询。

---

# 24. MetadataResolver + Provenance：接入 parse

正确关系：

```text
TakeoutParser
      ↓
AssociationGraph
      ↓
LogicalAsset
      ├──→ CanonicalPhoto
      │
      └──→ group MetadataCandidate
                    ↓
              MetadataResolver
                    ↓
              ResolutionRecord
                    ↓
                Provenance
```

Metadata Resolution 与 `CanonicalPhoto` 并列，不塞进 Photo IR value object。

增加 deterministic test：

```text
MetadataCandidate 输入顺序变化
→ Resolution 结果不变
```

---

# 25. DiffEngine + AuditReport：保留并低风险接入

优先级低于：

```text
Recovery
Epoch
Receipt
MutationGuard
Pipeline 拆分
```

接入：

```text
verify
→ BinaryVerifier
→ DiffEngine
→ AuditReport
```

Recovery 同样写：

```text
reconcile action
reason
old epoch
recovery epoch
attempt
source/target path
```

Audit 只记录当前真实能力，不声称：

```text
Metadata target preservation
Relation target preservation
```

---

# 26. 确认删除项

## ScanCommand

真实 CLI 已经走：

```text
PipelineCommand(kScan)
```

旧 skeleton：

```text
DELETE
```

CommandDispatcher 测试使用测试内部 `FakeCommand` 保留。

---

## RequiresInput helper

如果仍是：

```cpp
return true;
```

删除 helper。

但：

```text
input_path 非空检查
```

必须保留。

---

## TaskGraph

当前没有真实多阶段生产 DAG。

Repository 的 dependency table 能力可以留。

无人使用的内存 `TaskGraph`：

```text
DELETE
```

删除前：

```bash
rg "TaskGraph"
```

确认 production caller = 0。

---

## MetadataVerifier / RelationVerifier

当前 Target 没有真正实现完整 Metadata / Relation preservation。

继续保留 verifier 只会制造：

```text
看起来完成了
实际 target 没有数据
```

所以：

```text
DELETE FROM V1
```

Git history 保留历史即可。

---

# 27. BoundedExecutor / BytePermit / FdPermit：HOLD

现在不删，也不在 correctness 阶段接。

原因：

```text
已有实现和测试
但并发化会扩大迁移状态机复杂度
```

最终必须：

```text
INTEGRATE
或
DELETE
```

不能长期悬空。

只有完成 V4 correctness phases 后重新跑 benchmark。

如果：

```text
串行任务调度确实限制 E2E
```

才接：

```text
BoundedExecutor
BytePermitPool
FdPermitPool
```

---

# 28. DbCommandQueue：HOLD

当前串行 migrate 不需要强行引入 DB writer thread。

现有 Queue API 也不一定适合：

```text
ClaimNextReady → 返回 ClaimedTask
```

这种有返回值事务。

最终：

```text
若接多 worker
→ 重新设计 DB ownership / command result

若不接多 worker
→ DELETE
```

Benchmark 不是保留 production dead module 的理由。

---

# 29. V4 最终模块决策表

| 模块 | 决策 |
|---|---|
| ScanCommand | DELETE |
| RequiresInput helper | DELETE |
| Plan Execution Lock | EXTEND migrate + resume |
| AcquireNextExecutionEpoch | NEW / COMPLETE |
| ClaimNextReady | ADD current_epoch fence |
| PersistVerifiedReceipt | ADD current_epoch + ownership fence |
| FinishTask | ADD current_epoch fence |
| VerifiedReceipt | FIX idempotency + read validation |
| task_attempt | COMPLETE audit lifecycle |
| Recovery Transition APIs | NEW |
| CommitIntent | RECONSTRUCT, NO NEW TABLE |
| Reconciler | INTEGRATE + FIX temp cleanup rule |
| RecoveryService | NEW |
| MutationGuard | INTEGRATE |
| CopyAndHash identity checks | REMOVE AFTER DATA MIGRATION |
| BinaryVerifier fd stability | KEEP |
| TempCommit | DELETE |
| MigrationAttemptPreparer | NEW |
| TaskRuntimeRepository | KEEP + EXPAND |
| TaskGraph | DELETE |
| MetadataResolver | INTEGRATE parse |
| Provenance | INTEGRATE parse |
| MetadataVerifier | DELETE V1 |
| RelationVerifier | DELETE V1 |
| DiffEngine | INTEGRATE verify |
| AuditReport | INTEGRATE verify/recovery |
| TakeoutParser | KEEP |
| AssociationGraph | KEEP |
| CanonicalPhoto | KEEP |
| LinuxFileOps | KEEP |
| LAN Receiver | KEEP |
| BoundedExecutor | HOLD → final integrate/delete |
| BytePermitPool | HOLD → final integrate/delete |
| FdPermitPool | HOLD → final integrate/delete |
| DbCommandQueue | HOLD → final integrate/delete |

---

# 30. 最终实施顺序

严格按顺序做，一次只处理一类语义。

---

## Phase 0 — Freeze Baseline

记录：

```text
git HEAD
working tree
build preset
test count
benchmark baseline
```

执行当前仓库实际支持的 Debug build/test。

创建：

```bash
git tag pre-v4-convergence
```

要求：

```text
[ ] clean tree
[ ] baseline tests 全通过
```

---

## Phase 1 — Harden VerifiedReceipt Repository

一次完成：

```text
[ ] ReadVerifiedReceipt step==ROW 检查
[ ] column types 检查
[ ] epoch/size 范围检查
[ ] 内容幂等
[ ] 同 key 不同内容失败
[ ] identical replay 不重复 event
```

先不改 CLI。

---

## Phase 2 — Close task_attempt Audit Lifecycle

实现：

```text
Claim → RUNNING + real started_at
Receipt → VERIFIED_DURABLE
MarkRetryable → RETRYABLE + finished_at
```

同时建立正式 attempt-state update helper。

后续 phases 再补：

```text
COMMIT_INTENT
TEMP_WRITTEN
COMMITTED
INCONSISTENT
```

原则：

```text
attempt state 是 audit
不是 Recovery 判断唯一依据
```

---

## Phase 3 — Plan Lock + Epoch Repository

完成：

```text
AcquirePlanExecutionLock
AcquireNextExecutionEpoch
ReadCurrentEpoch
```

但先不改变 resume 行为。

---

## Phase 4 — Add Global Fencing

依次给：

```text
ClaimNextReady
PersistVerifiedReceipt
MarkSucceeded
MarkRetryable
```

增加：

```text
migration.current_epoch == supplied_epoch
```

检查。

Receipt 同时检查：

```text
RUNNING ownership
```

新增 stale tests。

---

## Phase 5 — Fix Reconciler Rules

只修改 pure decision：

```text
final_matches + temp_matches
→ cleanup

final_matches + temp mismatch
→ adopt only

no receipt + temp
→ retry, no auto cleanup
```

完善 tests。

---

## Phase 6 — Add Recovery Transition APIs

新增：

```text
RecoverSucceeded
RecoverRetryable
RecoverInconsistent
```

要求：

```text
recovery current epoch
+
expected old ownership
```

全部匹配才允许 transition。

同时闭环 old attempt state。

---

## Phase 7 — Integrate Reconciler Into Resume

顺序：

```text
lock
→ exactly one RUNNING
→ read old runtime/receipt
→ validate receipt against frozen plan
→ acquire recovery epoch
→ observe source/temp/final
→ Decide
→ Apply
```

完成核心 crash windows。

---

## Phase 8 — Real migrate Epoch Flow

migrate 修改：

```text
lock
→ materialize
→ RUNNING count must be 0
→ resolve READY/RETRYABLE
→ acquire epoch
→ claim
```

不再 `ExecutionEpoch{1}`。

---

## Phase 9 — MutationGuard Data Migration

### 9A

主 migrate/verify 接 `MutationGuard` overload。

### 9B

Receipt identity 改用：

```text
guard.manifest_identity()
```

### 9C

`CopyResult` 移除 identity。

### 9D

删除 CopyAndHash 重复 StatFd。

保留 BinaryVerifier 自身 stability check。

---

## Phase 10 — Extract MigrationAttemptPreparer

从 Pipeline 搬出：

```text
COMMIT_INTENT
Create Temp
Copy
fdatasync
TEMP_WRITTEN
Independent Verify
Receipt
VERIFIED_DURABLE
```

只搬代码，不顺便改行为。

---

## Phase 11 — Delete TempCommit

确认新 Preparer 覆盖生产行为后删除旧模块。

---

## Phase 12 — Split PipelineCommand

拆：

```text
ScanService
PlanService
MigrationService
RecoveryService
VerifyService
StatusService
```

优先把 correctness-sensitive SQL 收回 Repository。

目标：

```text
pipeline_command.cpp < 250 LOC
```

---

## Phase 13 — Integrate MetadataResolver + Provenance

只改 parse 支线。

不改变 migrate 主链。

---

## Phase 14 — Integrate DiffEngine + AuditReport

接入：

```text
verify
recovery
```

属于展示与可解释性增强，不得阻塞 correctness phases。

---

## Phase 15 — Delete Confirmed Isolated Code

删除：

```text
ScanCommand
TaskGraph
MetadataVerifier
RelationVerifier
```

以及对应 test/CMake。

每个删除前：

```bash
rg "<TypeName>"
```

确认无 production caller。

---

## Phase 16 — Process-level SIGKILL E2E

必须真实覆盖：

```text
1. partial temp / no receipt
2. verified receipt + temp
3. rename completed / DB still RUNNING
4. final durable / before recovery transition
```

最终：

```text
SIGKILL
→ restart
→ resume
→ verify
```

---

## Phase 17 — Final Concurrency Decision

重新跑 E2E benchmark。

然后对：

```text
BoundedExecutor
BytePermitPool
FdPermitPool
DbCommandQueue
```

做最终：

```text
INTEGRATE
或
DELETE
```

不允许保持“core 编译但 production 无 caller”。

---

# 31. 必须新增/强化的测试

## Receipt

```text
SameReceiptReplayIsIdempotent
ConflictingReceiptDigestFails
ConflictingReceiptPathFails
ConflictingReceiptIdentityFails
ReceiptSqliteStepErrorPropagates
ReceiptNegativeEpochRejected
ReceiptNegativeSizeRejected
StaleEpochCannotPersistReceipt
WrongAttemptCannotPersistReceipt
```

---

## Epoch

```text
ClaimRequiresCurrentEpoch
FinishRequiresCurrentEpoch
EpochAdvanceFencesOldFinish
MigrateWithRunningTaskDoesNotAdvanceEpoch
ResumeWithoutRunningDoesNotAdvanceEpoch
MultipleRunningRejectedBySingleThreadResume
```

---

## Attempt

```text
ClaimCreatesRunningAttempt
VerifiedReceiptMarksAttemptVerifiedDurable
RetryMarksAttemptRetryable
RecoverySuccessMarksAttemptCommitted
InconsistentMarksAttemptInconsistent
```

---

## Reconciler

```text
FinalMatchTempMatchAllowsCleanup
FinalMatchTempMismatchAdoptsWithoutCleanup
NoReceiptTempExistsRetriesWithoutPublish
NoReceiptFinalExistsConflicts
ReceiptTempMatchResumesCommit
ReceiptFinalMatchAdopts
```

---

## Frozen Receipt Validation

```text
ReceiptWrongSourceIdentityRejected
ReceiptWrongContentSizeRejected
ReceiptSourceDigestTargetDigestMismatchRejected
ReceiptWrongFinalPathRejected
```

---

## Crash E2E

```text
PartialTempNeverBecomesFinal
VerifiedTempCanResume
PostRenameCrashAdoptsFinal
MismatchingFinalNeverOverwritten
RepeatedResumeIsIdempotent
```

---

# 32. README 最终允许宣称

V4 全部完成后才能写：

```text
Frozen Manifest
Frozen Plan + Digest
Plan Execution Lock
Persistent Task / Attempt
Execution Epoch + Global Fencing
Source Mutation Guard
Copy + BLAKE3
VerifiedReceipt
Receipt Idempotency
fdatasync
Independent Temp Verification
renameat2(RENAME_NOREPLACE)
Directory fsync
DB / FS Reconciliation
Crash Recovery
Binary Verification
Diff / Audit
Google Takeout Parser
Association Graph
Canonical Photo IR
Metadata Resolver
Provenance
LAN Upload Receiver
```

根据 Phase 17 决定是否保留：

```text
Bounded Executor
Byte/FD Budget
Single DB Writer
```

未真正接入前不写简历。

---

# 33. 秋招最终四个主技术点

## 1. Crash-safe Candidate + Publish

```text
Temp
→ Copy/Hash
→ fdatasync
→ independent verify
→ durable Receipt
→ atomic rename
→ directory fsync
```

## 2. Recovery Reconciliation

```text
Frozen Plan
+
old Task/Attempt
+
VerifiedReceipt
+
Temp/Final Reality
→ pure Reconciler
→ fenced Recovery Transition
```

## 3. Execution Fencing

```text
Plan Lock
+
Monotonic current_epoch
+
Task owner_epoch
+
attempt_id
+
current-epoch checked durable writes
```

## 4. Frozen Source + Mutation Guard

```text
Frozen Manifest
+
opened-fd FileIdentity
+
before/after source read validation
```

照片业务作为第二层：

```text
Takeout Parser
Association Graph
Metadata Resolver
Canonical Photo IR
```

---

# 34. 最终验收清单

## Repository / Fencing

```text
[ ] Claim 检查 current epoch
[ ] Receipt 检查 current epoch + active ownership
[ ] Finish 检查 current epoch
[ ] Recovery transition 检查 recovery epoch + expected old ownership
[ ] Receipt 内容幂等
[ ] Receipt read 错误处理完整
```

## Recovery

```text
[ ] migrate 已有 RUNNING 时不推进 epoch
[ ] resume 单线程模式要求恰好 1 个 RUNNING
[ ] Receipt 与 Frozen Plan 校验
[ ] Reconciler 是唯一 recovery decision table
[ ] no-receipt temp 永不 publish
[ ] temp mismatch 永不自动 cleanup
[ ] matching final 可 adopt
[ ] conflicting final 不覆盖
```

## Attempt

```text
[ ] RUNNING
[ ] COMMIT_INTENT
[ ] TEMP_WRITTEN
[ ] VERIFIED_DURABLE
[ ] COMMITTED / RETRYABLE / INCONSISTENT
```

均有真实持久化审计状态。

## Source

```text
[ ] MutationGuard 负责 Frozen Source Identity policy
[ ] Receipt identity 来自 frozen/guard identity
[ ] CopyAndHash 不重复维护 Source Identity
[ ] BinaryVerifier 保留 fd stability check
```

## Structure

```text
[ ] TempCommit 删除
[ ] MigrationAttemptPreparer 建立
[ ] RecoveryService 建立
[ ] PipelineCommand < 250 LOC
[ ] correctness-sensitive SQL 收回 Repository
[ ] ScanCommand 删除
[ ] TaskGraph 删除
```

## Integration

```text
[ ] MetadataResolver 被 parse 调用
[ ] Provenance 被 parse 调用
[ ] DiffEngine 被 verify 调用
[ ] AuditReport 有真实输出
```

## Validation

```text
[ ] clean build
[ ] remaining tests all pass
[ ] sanitizer pass
[ ] SIGKILL → resume → verify pass
[ ] benchmark rerun
[ ] README 与真实代码一致
```

## Final Dead-code Decision

```text
[ ] BoundedExecutor integrate/delete
[ ] BytePermitPool integrate/delete
[ ] FdPermitPool integrate/delete
[ ] DbCommandQueue integrate/delete
```

---

# 35. 最终禁止事项

封版期间不再新增：

```text
openat2
statx
io_uring
Chunk Resume
Native Mobile App
新 Target Adapter
完整 Metadata Target Preservation
Relation Target Preservation
复杂 GUI
```

也禁止：

```text
为了保留旧代码制造无业务价值抽象
为了测试数量保留无人调用 production module
为了技术词把串行正确主线提前并发化
```

---

# 36. 最终拍板

PhotoBridge V1 秋招封版真正需要证明的是：

> **一次基于 Frozen Manifest / Frozen Plan 的文件迁移，即使在 Source 变化、Temp 写到一半、Receipt 已持久化、rename 已发生但数据库尚未完成、旧 executor 晚到等不同 crash window 下，系统仍能通过 ExecutionEpoch、Task/Attempt ownership、VerifiedReceipt 与文件系统真实状态做出确定恢复决策，不发布未经验证的半成品、不静默覆盖冲突目标，并最终通过 Binary Verification 与 Audit 证明迁移结果。**

V4 是后续代码修改的最终执行基准。

除非真实测试发现新的 correctness bug：

```text
不再扩大架构范围
不再重新设计主线
直接按 Phase 0 → Phase 17 实施
```
