# PhotoBridge 最新代码最终收口方案 V6

> 审计对象：`https://github.com/hxm56669/photobridge` 当前公开 `main`  
> 审计日期：2026-09-14  
> 目标：秋招封版，不再扩功能，只完成“接口一致性 → Repository 正确性 → 新主链切换 → 重复/死代码清理 → Crash E2E”  
> 结论：**当前仓库处于一次未完成的重构中间态，不能直接封版。先恢复 clean build / test 一致性，再做生产主链切换。**

---

# 1. 最终审计结论

当前代码已经完成了不少正确方向的工作：

```text
新 Pipeline Services          已写
MigrationAttemptPreparer     已写
PlanExecutionLock            已写
RecoveryService              已写
Receipt/Frozen Plan 校验      已写在新恢复链
MetadataResolver             已接 parse
Provenance                   已接 parse
DiffEngine / AuditReport     已进入新服务
部分旧实验模块               已从 CMake 移除
```

但当前存在四类硬问题：

```text
A. 头文件 / cpp / tests 不一致
B. Repository 新语义只写了测试和声明，生产实现未跟上
C. 新 Pipeline Services 还没有替换旧 PipelineCommand
D. Recovery / Reconciler 仍有测试与实现不一致
```

因此现在最重要的目标不是继续“设计”，而是：

> **把仓库从“重构中间态”恢复成一条可 clean build、可全量测试、只有一个 production path 的代码线。**

---

# 2. P0：当前存在确定的接口/编译不一致

这一组必须最先清零。

---

## 2.1 `CopyResult` 头文件和 cpp 已经不一致

当前头文件：

```cpp
struct CopyResult {
    std::uint64_t bytes_copied = 0;
    Digest source_digest;
};
```

也就是说已经删除了：

```text
source_before
source_after
```

但是当前 `copy_and_hash.cpp` 仍然：

```cpp
result.source_before = before.value();

...

result.source_after = after.value();

if (result.source_before != result.source_after) {
    ...
}
```

这是确定的接口不一致。

### 结论

当前 `copy_and_hash.cpp` 必须立即同步到新 Header。

### 最终建议

保留两个 overload：

```cpp
CopyAndHash(
    FileOps&,
    Hasher&,
    int source_fd,
    int target_fd,
    span<byte>);
```

作为纯：

```text
read
→ hash
→ WriteAll
```

不再负责 Frozen Source Identity。

然后真正实现当前 Header 已声明但 cpp 还没有实现的：

```cpp
CopyAndHash(
    FileOps&,
    Hasher&,
    MutationGuard& source_guard,
    int target_fd,
    span<byte>);
```

其职责：

```text
source_guard.VerifyBeforeRead()
↓
CopyAndHash(... source_guard.fd() ...)
↓
source_guard.VerifyAfterRead()
```

这样：

```text
CopyResult
= bytes_copied + source_digest
```

保持现在 Header 的最终设计。

### 不要恢复

```text
CopyResult.source_before
CopyResult.source_after
```

因为 Receipt 已经开始使用：

```text
MutationGuard.manifest_identity()
```

继续恢复旧字段只会重新制造重复责任。

---

## 2.2 `MutationGuard` overload 不要继续扩

当前 Header 只有：

```cpp
MutationGuard::Open(
    FileOps&,
    int source_root_fd,
    const PhysicalAsset&);
```

当前 Migration/Verify 已经可以构造：

```cpp
PhysicalAsset{
    plan_asset.source_path,
    plan_asset.source_identity,
    ...
}
```

所以 V6 不再新增：

```text
RelativePath + FileIdentity overload
```

没有必要为了少构造一个 value object 再扩接口。

保持当前接口即可。

---

# 3. P0：TaskRuntimeRepository 三方不一致

这是当前最大的封版阻塞项。

---

## 3.1 Header 已声明、cpp 没实现

当前 Header 已声明：

```cpp
AcquireNextExecutionEpoch(...)
ReadCurrentEpoch(...)

RecoverSucceeded(...)
RecoverRetryable(...)
RecoverInconsistent(...)
```

但当前 `task_runtime_repository.cpp` 中找不到对应 definition。

与此同时：

```text
pipeline_services.cpp
tests/task_runtime_repository_test.cpp
```

都已经调用这些接口。

### 结论

这些不是 Future API。

它们已经成为：

```text
新 Pipeline
+
当前测试
```

的实际依赖。

必须立即实现。

---

## 3.2 Tests 已调用、Header 甚至没声明

当前测试还调用：

```cpp
repository.MarkCommitIntent(...)
repository.MarkTempWritten(...)
```

但是当前 `task_runtime_repository.h` 没有这两个方法。

### 结论

当前 test/header 本身也不一致。

由于：

```text
FileAttemptState
```

已经正式存在：

```text
kRunning
kCommitIntent
kTempWritten
kVerifiedDurable
kCommitted
kRetryable
kInconsistent
```

因此这里不建议删除测试。

应正式加入 Repository：

```cpp
Status MarkCommitIntent(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch epoch,
    const std::string& attempt_id);

Status MarkTempWritten(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch epoch,
    const std::string& attempt_id);
```

并在 cpp 中实现。

---

# 4. P0：先让 Repository 测试表达的契约全部成为真实实现

当前 `task_runtime_repository_test.cpp` 已经比 production implementation 更接近最终需求。

V6 不再重新发明另一套规则。

直接以这些已写测试为主要契约，把实现补齐。

---

# 5. ExecutionEpoch 最终实现

## 5.1 `ReadCurrentEpoch`

根据：

```text
plan_id
→ migration_plan.migration_id
→ migration.current_epoch
```

读取。

必须检查：

```text
ROW 数量正确
INTEGER 类型
value >= 0
```

返回：

```cpp
ExecutionEpoch
```

---

## 5.2 `AcquireNextExecutionEpoch`

使用：

```text
BEGIN IMMEDIATE
```

保证：

```text
read current
→ overflow check
→ +1
→ update
→ commit
```

原子完成。

不能在 Service 层自己拼 SQL。

---

## 5.3 `ClaimNextReady` 加 global epoch fence

当前 Claim 只使用调用方传入的 epoch 写：

```text
owner_epoch
```

最终必须在同一 transaction 检查：

```text
supplied epoch
==
migration.current_epoch
```

否则：

```text
stale caller
```

不能领取 READY task。

---

# 6. Normal Finish 必须受 global epoch fence

当前 `FinishTask()` 条件只有：

```text
plan_id
task_id
owner_epoch
active_attempt_id
RUNNING
```

必须增加：

```text
migration.current_epoch == supplied_epoch
```

所以：

```text
旧 executor epoch=1
↓
新的 session 将 current_epoch 推进为 2
↓
旧 executor MarkSucceeded(epoch=1)
```

必须失败。

这也是当前测试：

```text
EpochAdvanceFencesOldFinish
```

已经明确要求的行为。

---

# 7. VerifiedReceipt 必须完整实现三件事

当前生产实现仍是旧版：

```sql
ON CONFLICT(...) DO NOTHING
```

然后继续追加：

```text
VERIFIED_DURABLE
```

这和当前测试契约不一致。

---

## 7.1 Receipt 写入 fencing

`PersistVerifiedReceipt()` 在同一个 transaction 内必须确认：

```text
migration.current_epoch == receipt.owner_epoch

plan_task.state == RUNNING

plan_task.owner_epoch == receipt.owner_epoch

plan_task.active_attempt_id == receipt.attempt_id
```

任意不匹配：

```text
InvalidArgument / stale ownership
```

---

## 7.2 Receipt 内容幂等

第一次：

```text
INSERT
→ attempt = VERIFIED_DURABLE
→ append VERIFIED_DURABLE event
```

如果 PK 已存在：

```text
Read existing Receipt
↓
逐字段比较
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

完全一致：

```text
OK
```

但是：

```text
不能重复写 VERIFIED_DURABLE event
```

任何字段不同：

```text
Internal / inconsistent evidence
```

当前测试已经覆盖：

```text
SameReceiptReplayIsIdempotent
ConflictingReceiptDigestFails
ConflictingReceiptPathFails
ConflictingReceiptIdentityFails
```

直接让实现满足这些测试即可。

---

## 7.3 Receipt 读取必须严格检查 SQLite

当前生产实现：

```cpp
if (sqlite3_step(...) == SQLITE_DONE) ...
// 然后直接 column...
```

改为：

```cpp
const int step = sqlite3_step(...);

if (step == SQLITE_DONE) {
    return NotFound;
}

if (step != SQLITE_ROW) {
    return SqliteError(...);
}
```

然后检查：

```text
owner_epoch 是 INTEGER 且 > 0
content_size 是 INTEGER 且 >= 0
digest 是 32 bytes
source_identity 可解码
```

当前测试已经有：

```text
ReceiptReadRejectsNegativeEpoch
ReceiptReadRejectsNegativeSize
```

---

# 8. task_attempt 生命周期正式闭环

当前 tests 已经明确期望：

```text
Claim
→ RUNNING

MarkCommitIntent
→ COMMIT_INTENT

MarkTempWritten
→ TEMP_WRITTEN

PersistVerifiedReceipt
→ VERIFIED_DURABLE

RecoverSucceeded
→ COMMITTED

MarkRetryable / RecoverRetryable
→ RETRYABLE

RecoverInconsistent
→ INCONSISTENT
```

这套状态机保留。

---

## 8.1 Claim

插入：

```text
file_state = RUNNING
started_at_ns = real timestamp
```

并记录：

```text
RUNNING event
created_at_ns > 0
```

---

## 8.2 `MarkCommitIntent`

必须验证：

```text
current epoch
RUNNING task ownership
active attempt
```

然后：

```text
attempt:
RUNNING → COMMIT_INTENT
```

写：

```text
COMMIT_INTENT event
```

---

## 8.3 `MarkTempWritten`

同样受：

```text
current epoch + ownership
```

保护。

状态：

```text
COMMIT_INTENT → TEMP_WRITTEN
```

---

## 8.4 `PersistVerifiedReceipt`

在 Receipt durable transaction 中：

```text
attempt → VERIFIED_DURABLE
```

---

## 8.5 Terminal

普通/恢复路径都必须填：

```text
finished_at_ns
result
error_code
error_message
```

不要让 attempt 只有“开始记录”没有结尾。

---

# 9. MigrationAttemptPreparer 必须接 attempt 生命周期

当前 `MigrationAttemptPreparer` 已经实现：

```text
CreateTemp
→ CopyAndHash(MutationGuard)
→ fdatasync
→ reopen temp
→ BinaryVerifier
→ PersistReceipt
```

这个结构保留。

但当前代码没有调用：

```text
MarkCommitIntent
MarkTempWritten
```

### 正确顺序

```text
MarkCommitIntent
↓
CreateTempNoReplace
↓
CopyAndHash
↓
fdatasync
↓
MarkTempWritten
↓
独立 reopen + VerifyBinary
↓
PersistVerifiedReceipt
```

这样测试定义的：

```text
RUNNING
→ COMMIT_INTENT
→ TEMP_WRITTEN
→ VERIFIED_DURABLE
```

才会进入 production path。

---

# 10. Reconciler：当前测试比实现新，按测试修 production

当前测试已经要求：

```text
final match + temp match
→ AdoptFinalAndCleanupTemp

final match + temp mismatch
→ AdoptFinal only
```

但是当前 production 仍然：

```cpp
if (final_matches && observed.temp_exists) {
    return kAdoptFinalAndCleanupTemp;
}
```

### 必须改成

```cpp
if (final_matches && temp_matches) {
    return kAdoptFinalAndCleanupTemp;
}

if (final_matches) {
    return kAdoptFinal;
}
```

---

# 11. Reconciler：Source Replacement 规则也未落 production

`ObservedFileState` 已经有：

```cpp
bool source_changed = false;
```

测试也已经要求：

```text
source_changed = true
→ kInconsistent
→ reason 包含 SOURCE_CHANGED
```

但 production `reconciler.cpp` 当前没有处理。

### 最前面增加

在通过 intent binding validation 后、普通 recovery decision 前：

```text
if source_changed:
    return INCONSISTENT("SOURCE_CHANGED ...")
```

原则：

> 已经冻结的 Source 被替换后，不能当成“source available”重新执行。

---

# 12. RecoveryService 当前 Source 观察必须修正

当前新 Recovery：

```text
OpenSource 成功
→ source_available = true
→ StatFd
```

这还不够。

必须把：

```text
路径存在
```

和：

```text
Frozen Source 仍然存在
```

区分开。

---

## 最终行为

尝试：

```text
MutationGuard::Open(
    file_ops,
    source_root_fd,
    PhysicalAsset{source_path, frozen_identity, ...})
```

### 成功

```text
source_available = true
source_changed = false
source_identity = frozen identity
```

### NotFound

```text
source_available = false
source_changed = false
```

### Identity mismatch / replacement

```text
source_available = false
source_changed = true
```

### 其他真实 I/O error

直接返回 error，不伪装成 source missing。

---

# 13. New Pipeline Services 的上层顺序基本正确，保留

当前新的 Migration 已经：

```text
Read Frozen Plan
↓
PlanExecutionLock
↓
Materialize
↓
检查 RUNNING
↓
AcquireNextExecutionEpoch
↓
Claim
↓
MutationGuard
↓
MigrationAttemptPreparer
```

这个方向不用重新设计。

Recovery 已经：

```text
读取 old runtime
↓
读取 Receipt
↓
Receipt 与 Frozen Plan 校验
↓
Acquire recovery epoch
↓
Observe
↓
Reconciler
↓
Recover*
```

总体也保留。

本轮不要再次重写 Service 架构。

---

# 14. 新主链现在还不是 production path

这是当前第二大问题。

当前 CLI：

```text
cli_app
→ PipelineCommand
```

这层保留没问题。

但是当前：

```text
PipelineCommand::Execute()
```

仍然包含约 1600 行旧：

```text
scan
plan
migrate
resume
verify
status
```

实现。

而新的：

```text
RunPipelineStage()
```

没有被 `PipelineCommand` 调用。

所以当前仓库长期同时存在：

```text
旧 PipelineCommand 实现
+
新 Pipeline Services 实现
```

不能封版。

---

# 15. 什么时候才能切 Production Path

必须等下面全部完成：

```text
[ ] CopyAndHash header/cpp 一致
[ ] MutationGuard overload 有真实 definition
[ ] TaskRuntimeRepository header/cpp/tests 一致
[ ] AcquireNextExecutionEpoch 有 definition
[ ] ReadCurrentEpoch 有 definition
[ ] Recover* 有 definition
[ ] MarkCommitIntent 有 declaration + definition
[ ] MarkTempWritten 有 declaration + definition
[ ] Repository tests 编译并通过
[ ] Reconciler tests 通过
```

然后才能切：

```text
PipelineCommand::Execute()
→ RunPipelineStage(...)
```

---

# 16. PipelineCommand 最终形态

保留类本身作为 CLI Adapter。

最终：

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

或者等价代码。

然后删除 `pipeline_command.cpp` 中其余：

```text
SQLite helpers
Manifest read
Plan write/materialize
Task counters
Plan Lock
TempName
Migration
Resume
Verify
Status
```

目标：

```text
pipeline_command.cpp < 100 LOC
```

---

# 17. 切主链时必须做 clean build

当前是：

```text
photobridge_core STATIC
```

所以过去某些没有被最终 executable 拉入的 object code 可能暂时没有暴露所有未定义符号。

切换：

```text
PipelineCommand → RunPipelineStage
```

后必须：

```text
删除旧 build directory
重新 configure
重新 build
```

不能只跑 incremental build。

---

# 18. 当前 public main 按静态代码已经存在 clean build 风险

这一点必须明确。

## 确定的不一致 1

Header：

```text
CopyResult
只有 bytes_copied/source_digest
```

cpp：

```text
仍访问 source_before/source_after
```

---

## 确定的不一致 2

Header 声明：

```text
CopyAndHash(... MutationGuard& ...)
```

cpp 当前没有对应 definition。

---

## 确定的不一致 3

Tests 调用：

```text
MarkCommitIntent
MarkTempWritten
```

Header 当前没有 declaration。

---

## 确定的不一致 4

Header + tests 调用：

```text
AcquireNextExecutionEpoch
ReadCurrentEpoch
RecoverSucceeded
RecoverRetryable
RecoverInconsistent
```

cpp 当前没有 definition。

因此：

> 在做任何“架构优化”之前，第一目标必须是让 public main 恢复 clean compile + test。

---

# 19. CMake 当前清理结果

已经不再编译：

```text
BoundedExecutor
DbCommandQueue
TaskGraph
TempCommit
MetadataVerifier
RelationVerifier
```

这一批不要恢复。

---

# 20. CMake 还剩两个悬空模块

当前仍编译：

```text
BytePermitPool
FdPermitPool
```

以及对应 tests。

但新 MigrationService 当前没有使用它们。

### V6 最终决定

如果本轮封版不实现：

```text
真正多 task 并行 migration
```

则直接删除：

```text
byte_permit_pool.h/.cpp
fd_permit_pool.h/.cpp
对应 tests
CMake entry
```

本方案建议：

```text
DELETE
```

理由：

```text
当前 V1 是单 task preparation/recovery
Permit 没有 production caller
继续保留只会制造“项目用了资源预算”的错觉
```

---

# 21. `pipeline_services.cpp` 也不能成为第二个巨型文件

当前：

```text
pipeline_services.cpp ≈ 1900 行
```

所以切换成功以后还要拆。

但注意顺序：

```text
先保证唯一新主链可运行
↓
再拆文件
```

不能边修 correctness 边拆 1900 行文件。

---

# 22. 最终拆分

新主链稳定后拆成：

```text
src/pipeline/
├── pipeline_shared.cpp
├── scan_service.cpp
├── plan_service.cpp
├── migration_service.cpp
├── recovery_service.cpp
├── verify_service.cpp
└── status_service.cpp
```

其中：

```text
pipeline_shared
```

只放真正共享的：

```text
ReadFrozenPlan
MaterializePlan
ReadManifestSourceRoot
TaskSpecForPlanAsset
TempNameFor
Recovery file observation
```

不要再放所有逻辑。

目标：

```text
每个 Service 大约 100～400 LOC
```

---

# 23. 当前已经完成的模块不要再改

以下已经足够：

```text
TakeoutParser
AssociationGraph
CanonicalPhoto
MetadataResolver
Provenance
DiffEngine
AuditReport
LAN Receiver
```

这些不是当前收口重点。

不要继续增加：

```text
Metadata target preservation
Relation target preservation
新 Adapter
```

---

# 24. V6 最终实施顺序

严格按这个顺序。

---

## Phase 0 — 停止新增功能

当前分支只允许：

```text
compile fix
correctness fix
routing switch
dead-code cleanup
tests
benchmark/docs
```

不再增加新业务。

---

## Phase 1 — 恢复 Header / CPP / Test 一致性

### CopyAndHash

完成：

```text
[ ] cpp 不再访问 source_before/source_after
[ ] int-fd overload 编译
[ ] MutationGuard overload 有 definition
[ ] CopyResult 保持 bytes + digest
```

### Repository API

完成：

```text
[ ] Header 增加 MarkCommitIntent
[ ] Header 增加 MarkTempWritten
[ ] cpp 实现 AcquireNextExecutionEpoch
[ ] cpp 实现 ReadCurrentEpoch
[ ] cpp 实现 MarkCommitIntent
[ ] cpp 实现 MarkTempWritten
[ ] cpp 实现 RecoverSucceeded
[ ] cpp 实现 RecoverRetryable
[ ] cpp 实现 RecoverInconsistent
```

这一 Phase 结束标准：

```text
clean compile
```

先不急着切新 Pipeline。

---

## Phase 2 — 让 TaskRuntimeRepository tests 全过

补：

```text
global epoch fence
attempt lifecycle
receipt fencing
receipt idempotency
receipt read validation
recovery transitions
timestamps/events
```

这一阶段直接以当前：

```text
tests/task_runtime_repository_test.cpp
```

已表达的契约为主。

如果测试与最终产品语义冲突才改测试。

不要为了让测试变绿删除合理测试。

---

## Phase 3 — 让 Reconciler tests 全过

修：

```text
final match + temp mismatch
→ AdoptFinal only

source_changed
→ Inconsistent
```

同时修无 Receipt temp 的 reason，使行为与测试一致：

```text
Retry
but do not auto-clean unproven temp
```

---

## Phase 4 — 把 Attempt Lifecycle 接进 Preparer

Production 真正调用：

```text
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
```

顺序：

```text
RUNNING
→ COMMIT_INTENT
→ TEMP_WRITTEN
→ VERIFIED_DURABLE
```

---

## Phase 5 — 修 Recovery Source Observation

不再：

```text
OpenSource success = source_available
```

改为：

```text
Frozen Identity Match
→ source_available

Identity mismatch
→ source_changed
```

---

## Phase 6 — Repository / Reconciler / Core Tests 全绿

至少确保：

```text
task_runtime_repository_test
reconciler_test
copy_and_hash_test
mutation_guard_test
binary_verifier_test
```

全部通过。

---

## Phase 7 — 新主链切换

修改：

```text
PipelineCommand::Execute
```

只调用：

```text
RunPipelineStage
```

然后立即：

```text
clean configure
clean build
全量 tests
```

---

## Phase 8 — CLI E2E 验证

真实执行：

```text
init
scan
plan
migrate
status
resume
verify
```

确认新 Services 真正运行。

增加/保留一个 routing regression test：

> 未来不能再出现“新 Service 写好了但 CLI 仍走旧路径”。

---

## Phase 9 — 删除旧 PipelineCommand 实现

确认新链稳定后：

```text
pipeline_command.cpp
```

只保留 constructor + wrapper。

目标：

```text
< 100 LOC
```

---

## Phase 10 — 删除 Permit 悬空代码

若仍未实现多 task workers：

```text
DELETE BytePermitPool
DELETE FdPermitPool
DELETE tests
DELETE CMake entries
```

---

## Phase 11 — 拆 `pipeline_services.cpp`

只做结构移动，不改行为。

拆成多个 Service 文件。

---

## Phase 12 — Crash E2E

至少覆盖：

```text
A. partial temp / no Receipt
→ SIGKILL
→ resume
→ 不得 publish 半文件

B. Verified Receipt + matching temp
→ SIGKILL
→ resume
→ publish

C. rename 已完成 / task 仍 RUNNING
→ SIGKILL
→ resume
→ Adopt Final

D. Final mismatch
→ 永不覆盖

E. Source replacement
→ 不 Retry 新文件
```

---

## Phase 13 — Sanitizer + Benchmark

执行：

```text
Debug tests
ASan/UBSan
Release benchmark
```

Benchmark 只测最终 production path。

不要再 benchmark 已删除实验模块。

---

## Phase 14 — README / 简历同步

最后才更新。

README 只能描述：

```text
真实 CLI 已经过的能力
```

---

# 25. 最终代码依赖图

```text
CLI
 ↓
PipelineCommand            # 薄 adapter
 ↓
Pipeline Services
 ├── ScanService
 ├── PlanService
 ├── MigrationService
 │      ↓
 │ TaskRuntimeRepository
 │      ↓
 │ MutationGuard
 │      ↓
 │ MigrationAttemptPreparer
 │      ↓
 │ CopyAndHash
 │      ↓
 │ BinaryVerifier
 │
 ├── RecoveryService
 │      ↓
 │ Reconciler
 │      ↓
 │ TaskRuntimeRepository
 │
 ├── VerifyService
 │      ↓
 │ MutationGuard
 │      ↓
 │ BinaryVerifier
 │      ↓
 │ DiffEngine / AuditReport
 │
 └── StatusService
```

不再允许：

```text
PipelineCommand 旧实现
+
Pipeline Services 新实现
```

长期共存。

---

# 26. 最终保留模块

可靠迁移核心：

```text
PipelineCommand          # thin adapter
Pipeline Services

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

照片语义：

```text
TakeoutParser
AssociationGraph
CanonicalPhoto
MetadataResolver
ResolutionRecord
Provenance
```

LAN：

```text
ServeCommand
HTTP Server
UploadSessionStore
```

---

# 27. 最终删除/不恢复

已经退出：

```text
ScanCommand
TaskGraph
TempCommit
MetadataVerifier
RelationVerifier
BoundedExecutor
DbCommandQueue
```

不要恢复。

本轮继续删除：

```text
BytePermitPool
FdPermitPool
```

前提：

```text
没有真正接入并行 migration
```

---

# 28. 封版前必须通过的 Gate

## Gate A — Compile Gate

```text
[ ] Header / cpp / tests 无接口错位
[ ] clean configure + build 成功
[ ] 无 undefined reference
```

---

## Gate B — Repository Gate

```text
[ ] Epoch monotonically increases
[ ] stale Claim rejected
[ ] stale Finish rejected
[ ] stale Receipt rejected
[ ] wrong attempt Receipt rejected
[ ] Receipt replay idempotent
[ ] conflicting Receipt rejected
[ ] Attempt lifecycle 完整
[ ] Recovery transition 完整
```

---

## Gate C — Recovery Gate

```text
[ ] Reconciler tests 全过
[ ] source replacement → inconsistent
[ ] temp mismatch 不自动 cleanup
[ ] no Receipt temp 不 publish
[ ] matching final 可 adopt
```

---

## Gate D — Routing Gate

```text
[ ] PipelineCommand 只走 RunPipelineStage
[ ] 旧 Pipeline 实现删除
[ ] CLI E2E 走新链
```

---

## Gate E — Cleanup Gate

```text
[ ] pipeline_command.cpp < 100 LOC
[ ] pipeline_services 已拆小
[ ] 无 production caller 的 Permit 删除
[ ] CMake 无悬空 production module
```

---

## Gate F — Final Validation

```text
[ ] 全量测试通过
[ ] ASan / UBSan 通过
[ ] SIGKILL → resume → verify 通过
[ ] Benchmark 重跑
[ ] README 与真实实现一致
```

---

# 29. 当前完成度重新评估

按“文件有没有写出来”：

```text
约 70%
```

按“当前 main 是否已经形成可运行的新生产主链”：

```text
约 40%～50%
```

原因不是核心设计没写，而是：

```text
新 tests 已写
新 service 已写
新 header 已写
        ↓
部分 cpp 没同步
        ↓
旧 Pipeline 仍是生产入口
```

所以剩余工作本质上是：

> **完成一次没有收尾的接口迁移和 production routing 切换。**

---

# 30. 这次收口之后不要再继续重构

当 Gate A～F 全过后：

```text
PhotoBridge V1 Freeze
```

后续只允许：

```text
真实 bug fix
文档修正
面试准备
```

不再继续：

```text
架构重构
并发框架
新业务模块
新系统调用关键词
```

---

# 31. 秋招最终技术主线

代码收口以后只讲四个核心点：

```text
1. Frozen Manifest / Frozen Plan

2. Execution Epoch
   + Task / Attempt ownership fencing

3. Temp
   → Copy + BLAKE3
   → fdatasync
   → independent verification
   → VerifiedReceipt
   → renameat2(RENAME_NOREPLACE)
   → directory fsync

4. DB / Filesystem Reconciliation
   → Retry / Resume Temp / Adopt Final / Conflict / Inconsistent
```

第二层再讲：

```text
MutationGuard
Association Graph
Metadata Resolver
LAN Receiver
Benchmark
```

---

# 32. 最终拍板

当前仓库下一步**不是再写 V7 架构**，也不是继续增加能力。

只需要：

```text
先把 Header / cpp / tests 对齐
↓
让 Repository 与 Reconciler 测试全部变绿
↓
把新 Services 切成唯一生产路径
↓
删旧 Pipeline
↓
删最后悬空 Permit
↓
Crash E2E
↓
封版
```

这是当前最新代码最短、风险最低、最适合秋招的收口路线。
