# PhotoBridge 四项性能优化：理解、基线、改造与验证计划

> 适用仓库：PhotoBridge
>
> 目标：不是为了“堆技术”，而是按 **先理解现状 → 建立基线 → 找瓶颈 → 最小改造 → 回归验证 → 再做下一项优化** 的方式推进。
>
> 四项优化分别针对：
>
> 1. SQLite：减少 SQL 编译与事务提交开销；
> 2. Worker Pool：把单 Task 串行执行改为多 Task 并行；
> 3. BLAKE3：确认 SIMD 实际生效，并评估是否需要多线程 Hash；
> 4. io_uring：把同步 `read → hash → write` 改成多缓冲异步流水。

---

# 一、总体原则

## 1. 不直接改代码，先留 Before 数据

每一项优化都遵循：

```text
理解当前实现
↓
确认真正瓶颈
↓
保存 Before Benchmark
↓
perf / strace 定位
↓
最小改动
↓
单元测试 / Crash Recovery 回归
↓
保存 After Benchmark
↓
比较收益和代价
```

不要直接：

```text
看到“可以优化”
→ 上新技术
→ 只跑一次
→ 说“性能提升了”
```

## 2. 四项优化的关系

```text
优化 1：SQLite
→ 状态持久化开销

优化 2：Worker Pool
→ Task 级并行

优化 3：BLAKE3 SIMD / TBB
→ Hash CPU 开销

优化 4：io_uring
→ 文件数据 I/O 流水
```

推荐实际实施顺序：

```text
原始版本
↓
优化 1：SQLite
↓
重新建立基线
↓
优化 2：Worker Pool
↓
重新建立基线
↓
优化 3：BLAKE3
↓
重新建立基线
↓
优化 4：io_uring
↓
最终联合调参
```

---

# 二、优化一：SQLite Prepared Statement + Batch Transaction

## 2.1 当前代码怎么实现

当前项目不是“完全没用 Prepared Statement”，而是使用方式不统一。

第一类路径是 `SqliteConnection::Execute()`：

```cpp
connection.Execute(
    "INSERT INTO benchmark_queue(value) VALUES(1);");
```

底层使用 `sqlite3_exec()`。如果在循环中反复调用，就会形成：

```text
SQL 解析
→ SQL 编译
→ 执行
→ finalize
→ autocommit

重复 N 次
```

Benchmark 当前 SQLite workload 基本属于：

```text
INSERT
→ commit
→ INSERT
→ commit
→ ...
```

因此当前数据同时包含：

```text
SQL 解析 / 编译
+
事务提交
+
synchronous=FULL 的持久化成本
```

第二类路径已经使用 Prepared Statement。例如 Manifest 一类批量写入路径已经存在：

```text
prepare 一次
↓
bind
↓
step
↓
reset
↓
clear bindings
↓
bind 下一条
```

因此第一项优化的准确目标是：

> **统一 SQLite 高频路径的使用方式。**

## 2.2 当前主要问题

### 问题 1：高频 SQL 反复 prepare

部分 Repository 虽然有 RAII `Statement`，但生命周期类似：

```text
函数进入
↓
prepare
↓
bind
↓
step
↓
函数结束
↓
finalize
```

下一次调用同一函数又重新 prepare。

### 问题 2：批量操作可能逐条提交

原来：

```text
INSERT task 1
COMMIT

INSERT task 2
COMMIT

INSERT task 3
COMMIT
```

这会把大量时间花在事务提交和持久化上。

## 2.3 优先修改哪些地方

优先级：

```text
1. plan_task INSERT
2. task_dependency INSERT
3. task_event INSERT
4. 高频 Task / Attempt UPDATE
5. 高频 SELECT
```

适合复用 Prepared Statement 的典型特征：

```text
SQL 文本固定
+
执行频率高
+
只有绑定参数变化
```

不需要为了统一强行改：

```text
BEGIN IMMEDIATE
COMMIT
ROLLBACK
PRAGMA
```

因为这些频率低，缓存 Prepared Statement 的收益很小。

## 2.4 Prepared Statement 复用目标

目标生命周期：

```text
Repository 创建
↓
prepare 一次
↓
任务 1：bind → step → reset
↓
任务 2：bind → step → reset
↓
任务 3：bind → step → reset
↓
Repository 销毁
↓
finalize
```

可以收敛成统一 RAII：

```cpp
class SqliteStatement {
public:
    SqliteStatement(sqlite3* db, const char* sql);
    ~SqliteStatement();

    sqlite3_stmt* get() noexcept;
    Status Reset();

private:
    sqlite3_stmt* stmt_ = nullptr;
};
```

`Reset()` 负责：

```text
sqlite3_reset
+
sqlite3_clear_bindings
```

## 2.5 Batch Transaction

原来：

```text
INSERT 1
COMMIT
INSERT 2
COMMIT
...
```

优化：

```text
BEGIN IMMEDIATE
↓
INSERT 1
INSERT 2
INSERT 3
...
INSERT N
↓
COMMIT
```

核心收益：

```text
N 次 durable commit
↓
1 次 durable commit
```

## 2.6 哪些状态不能无脑 Batch

PhotoBridge 的 Crash Recovery 依赖明确 durable 边界。

这些不能随便攒批：

```text
Claim
Attempt 状态推进
VerifiedReceipt
Commit 状态
Recovery 状态转换
```

适合批量：

```text
Plan 初始化
大量 Task materialization
Dependency 初始化
Manifest asset
普通批量 event
```

原则：

```text
只优化“可批量的数据初始化/批写”
不破坏恢复协议要求的持久化边界
```

## 2.7 Benchmark 怎么设计

保留三组：

```text
A：sqlite3_exec + autocommit
B：Prepared Statement + autocommit
C：Prepared Statement + Batch Transaction
```

Batch size 再测：

```text
10
100
1000
5000
```

记录：

```text
commands/s
wall time
CPU
P95 / P99
fsync / fdatasync 相关次数
```

这样才能区分：

```text
SQL 编译减少带来多少
事务提交减少带来多少
```

## 2.8 实施顺序

```text
1. 保存当前 SQLite benchmark
2. 增加 prepared_autocommit benchmark
3. 增加 prepared_batch benchmark
4. 抽统一 SqliteStatement RAII
5. 先改高频 INSERT
6. 再改高频 UPDATE / SELECT
7. 保持 Crash-safe 事务边界
8. 跑 ctest
9. 跑 crash recovery
10. 重新跑 SQLite + E2E
```

## 2.9 与其他优化的耦合

与 BLAKE3、io_uring：

```text
代码耦合很弱
```

与 Worker Pool：

```text
性能耦合较强
```

因为多 Worker 后：

```text
多个 Worker
→ 更多 SQLite 状态写入
→ SQLite 单写者竞争更明显
```

因此推荐先优化 SQLite，再正式落地多 Worker。

---

# 三、优化二：Worker Pool，多 Task 并行迁移

## 3.1 当前 migrate 为什么是单 Task

当前主链大致是：

```text
MigrationService::Execute()
↓
找到 READY Task
↓
AcquireNextExecutionEpoch()
↓
ClaimNextReady()
↓
Prepare 一个 Task
↓
return
```

一次 `migrate` 只 Claim 一次，没有 Worker Pool。

## 3.2 还有哪些地方写死“单 RUNNING Task”

当前 Resume 也按 V1 单任务模型收敛：

```text
RUNNING count 必须正好为 1
```

所以如果直接上 4 Worker：

```text
Task A RUNNING
Task B RUNNING
Task C RUNNING
Task D RUNNING
```

旧 Resume 就不能工作。

因此第二项优化本质是：

```text
单 RUNNING Task 模型
↓
多 RUNNING Task 模型
```

而不是简单加几个 `std::thread`。

## 3.3 Worker Pool 应该放在哪里

不要放在：

```text
LinuxFileOps
CopyAndHash
MigrationAttemptPreparer
TaskRuntimeRepository
```

这些是“执行一个 Task”的底层能力。

应该放在：

```text
MigrationService
```

目标：

```text
                 MigrationService
                       ↓
                    Producer
                       ↓
                Bounded Queue
               ↓      ↓      ↓
            Worker1 Worker2 Worker3 ...
               ↓      ↓      ↓
          Prepare(task)
```

## 3.4 Task 怎么进入队列

Producer：

```text
Claim READY Task
↓
得到 ClaimedTask
↓
Push 到 Queue
↓
继续 Claim
```

Worker：

```text
Pop ClaimedTask
↓
找到 Frozen Plan 对应 Asset
↓
MutationGuard
↓
CopyAndHash
↓
fdatasync
↓
Verify
↓
VerifiedReceipt
```

## 3.5 Claim 逻辑为什么要重构

更合理的 Repository 边界：

```text
TaskRuntimeRepository::ClaimNextReady()
↓
BEGIN IMMEDIATE
↓
SELECT READY Task
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
↓
返回 ClaimedTask
```

职责：

```text
Repository：原子领取
Worker：执行 Task
```

## 3.6 为什么多个 Worker 不会领取同一个 Task

依靠：

```text
BEGIN IMMEDIATE
+
state = READY 条件更新
```

Task A 被 Claim 后：

```text
READY → RUNNING
```

下一次 Claim 就看不到 Task A。

## 3.7 为什么队列必须有界

假设 100 万 Task，如果 Producer 无限 Claim：

```text
Queue 无限增长
+
内存持续上涨
+
SQLite 出现大量提前 RUNNING 的 Task
+
真正执行速度并没有增加
```

建议第一版：

```text
Worker = 4
Queue Capacity = 8
```

## 3.8 Backpressure 是什么

```text
Producer 太快
↓
Queue 满
↓
Producer 阻塞
↓
Worker 消费一个
↓
Queue 腾位置
↓
Producer 再 Claim
```

Backpressure 的作用是控制过载，而不是直接提高吞吐。

## 3.9 execution epoch 怎么处理

正确：

```text
一轮 migrate
↓
AcquireNextExecutionEpoch()
↓
epoch = N
↓
所有 Worker 共用 N
```

不能：

```text
每个 Worker 自己 Acquire epoch
```

否则后来的 Worker 会把前面的 Worker 变成 stale。

## 3.10 SQLite Connection 怎么处理

不建议 4 Worker 共用一个 Connection。

第一版建议：

```text
Producer → 1 个 Connection
Worker1 → 1 个 Connection
Worker2 → 1 个 Connection
Worker3 → 1 个 Connection
Worker4 → 1 个 Connection
```

都访问同一个 DB 文件。

## 3.11 SQLite 会遇到什么问题

WAL 允许多个 Reader，但写事务仍基本只有一个 Writer。

所以多 Worker 后可能：

```text
Worker1 写状态
Worker2 等
Worker3 等
Worker4 等
```

第一版先接受：

```text
独立 Connection
+
短事务
```

如果数据证明 SQLite 写竞争成为瓶颈，再考虑：

```text
Worker
↓
DB Command Queue
↓
Single SQLite Writer
```

## 3.12 Resume 怎么改

旧：

```text
只允许 1 个 RUNNING
```

新：

```text
找到全部 RUNNING Task
↓
Acquire recovery epoch
↓
Task A Reconcile
↓
Task B Reconcile
↓
Task C Reconcile
...
```

第一版建议：

```text
migrate：并行
resume：串行恢复所有 RUNNING
```

## 3.13 E2E Benchmark 也要改

旧：

```text
每个文件：migrate → resume
```

新：

```text
scan
↓
plan
↓
migrate --workers N
↓
resume
↓
verify
```

## 3.14 Before Benchmark

```bash
mkdir -p benchmark-before-worker

./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 5 \
  --output benchmark-before-worker/e2e-large.json \
  | tee benchmark-before-worker/e2e-large.txt
```

记录：

```text
wall time
MiB/s
CPU
P95 / P99
RSS
```

## 3.15 perf stat

```bash
perf stat \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -o benchmark-before-worker/perf-stat.txt \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1
```

重点：

```text
wall time
context-switches
cache-misses
task-clock
```

## 3.16 perf record

```bash
perf record -g \
  -o benchmark-before-worker/perf.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1

perf report \
  -i benchmark-before-worker/perf.data
```

重点看：

```text
CopyAndHash
BLAKE3
read/write
fdatasync
SQLite
futex
MigrationAttemptPreparer
```

## 3.17 strace

```bash
strace -f -c \
  -o benchmark-before-worker/strace-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1
```

详细：

```bash
strace -f -ttT \
  -e trace=read,write,pread64,pwrite64,openat,close,fdatasync,fsync,renameat2,futex \
  -o benchmark-before-worker/strace-detail.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1
```

## 3.18 实施顺序

```text
1. 保存单 Worker 基线
2. 把执行一个 ClaimedTask 抽成独立函数
3. Worker 数仍为 1，先验证行为不变
4. 重构 ClaimNextReady
5. epoch 改为“一轮 migrate 一个”
6. 加有界阻塞队列
7. 加 1/2/4/8 Worker
8. 每 Worker 独立 SQLite Connection
9. Producer 负责 Claim + Push
10. Queue 满时 Backpressure
11. Resume 支持多个 RUNNING
12. E2E 改成一次 migrate 多 Task
13. 测 1/2/4/8 Worker
```

---

# 四、优化三：BLAKE3 SIMD / TBB

## 4.1 当前 BLAKE3 是怎么实现的

当前并不是自己实现 Hash：

```text
CopyAndHash
↓
Read chunk
↓
Blake3Hasher::Update()
↓
blake3_hasher_update()
↓
Write chunk
```

所以 PhotoBridge 自己没有手写 SIMD。

## 4.2 当前很可能已经自动使用 SIMD

官方 BLAKE3 x86_64 实现会按 CPU 能力选择：

```text
SSE2
SSE4.1
AVX2
AVX-512
```

因此第一步不是“开启 AVX2”，而是证明：

```text
VM 暴露了什么指令集
↓
libblake3 编译了什么
↓
运行时实际走了什么
```

## 4.3 检查 VM CPU

```bash
lscpu
```

或：

```bash
lscpu | grep -i '^Flags'
```

```bash
grep -m1 '^flags' /proc/cpuinfo
```

重点：

```text
sse2
sse4_1
avx
avx2
avx512f
```

## 4.4 检查 libblake3 SIMD symbol

```bash
find build/bench-release/vcpkg_installed \
  -type f \
  \( -name 'libblake3.a' -o -name 'libblake3.so' \)
```

再：

```bash
nm -A <libblake3-path> \
  | grep -E 'avx2|avx512|sse41|sse2'
```

可能看到：

```text
blake3_hash_many_avx2
blake3_hash_many_avx512
blake3_hash_many_sse41
blake3_hash_many_sse2
```

注意：这只能证明库里有这些实现，不能证明运行时使用了它们。

## 4.5 保存 Copy 基线

```bash
mkdir -p benchmark-before-blake3

./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 5 \
  --output benchmark-before-blake3/copy.json \
  | tee benchmark-before-blake3/copy.txt
```

## 4.6 perf stat

```bash
perf stat -r 5 \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,page-faults \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

## 4.7 perf record：确认真实 SIMD 路径

```bash
perf record -g \
  -o benchmark-before-blake3/perf.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1

perf report \
  -i benchmark-before-blake3/perf.data
```

重点搜索：

```text
blake3
```

如果热点里出现：

```text
blake3_hash_many_avx2
```

才可以说运行时确实走 AVX2。

## 4.8 strace 在这里干什么

`strace` 看不到 SIMD，因为 SIMD 是用户态 CPU 指令。

这里用 strace 判断：

```text
Copy 慢
到底是 Hash
还是 read/write/fdatasync
```

```bash
strace -f -c \
  -e trace=read,write,fdatasync,fsync,openat,close \
  -o benchmark-before-blake3/strace-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

## 4.9 当前 Benchmark 的问题

当前 Copy workload 混在一起：

```text
read
+
BLAKE3
+
write
+
fdatasync
```

所以：

```text
Copy = 241 MB/s
```

不能解释成：

```text
BLAKE3 = 241 MB/s
```

## 4.10 增加 hash_only Benchmark

新增：

```text
hash_only
```

只做：

```text
内存 buffer
↓
Blake3Hasher
↓
Update
↓
Finalize
```

这样得到纯 Hash：

```text
GB/s
cycles
instructions
```

## 4.11 真正可继续实验的是 TBB

普通：

```text
blake3_hasher_update()
≈ 单 OS 线程 + SIMD
```

TBB：

```text
blake3_hasher_update_tbb()
≈ 多线程 + SIMD
```

## 4.12 为什么不能无条件全部用 TBB

小输入会有：

```text
线程调度成本
任务拆分成本
同步成本
```

应测：

```text
128 KiB
256 KiB
512 KiB
1 MiB
4 MiB
```

## 4.13 与 Worker Pool 的耦合很强

Worker Pool 是文件级并行；TBB 是单文件 Hash 内部并行。

如果：

```text
4 Worker
×
每 Worker 多个 TBB 线程
```

可能导致：

```text
CPU oversubscription
↓
context switch 增加
↓
cache miss 增加
↓
性能下降
```

对照片迁移场景优先考虑：

```text
Worker1 → File A → SIMD
Worker2 → File B → SIMD
Worker3 → File C → SIMD
Worker4 → File D → SIMD
```

TBB 是否保留由数据决定。

## 4.14 与 SQLite 的关系

代码耦合很弱，只在最终 E2E 叠加影响。

## 4.15 不能删第二次 Hash

当前：

```text
CopyAndHash
↓
source digest
↓
fdatasync(temp)
↓
重新打开 temp
↓
VerifyBinary
↓
再次 BLAKE3
```

第二次 Hash 属于独立验证，不能为了性能删掉。

## 4.16 实施顺序

```text
1. 不改代码
2. 检查 CPU Flags
3. 检查 libblake3 SIMD symbol
4. perf 确认实际运行路径
5. 保存 Copy 1 GiB 基线
6. 增加 hash_only benchmark
7. 判断 Hash 是否真是瓶颈
8. 如果已有 AVX2，不再做所谓“开启 SIMD”
9. 评估 blake3[tbb]
10. 测不同阈值
11. 比较普通 SIMD vs TBB
12. 再和 1/2/4 Worker 联合测试
13. 检查 oversubscription
14. 决定是否保留 TBB
```

---

# 五、优化四：io_uring，多缓冲异步 I/O 流水

## 5.1 当前 I/O 是怎么实现的

底层 `LinuxFileOps` 使用同步：

```cpp
::read(...)
::write(...)
```

当前 Copy：

```text
read(source)
↓
等待返回
↓
BLAKE3
↓
write(temp)
↓
等待返回
↓
下一块
```

## 5.2 当前真正的问题

不是简单说 `read/write` 系统调用慢，而是：

```text
read
↓
wait
↓
hash
↓
write
↓
wait
↓
下一块 read
```

没有形成 I/O 重叠。

## 5.3 VerifyBinary 也有同样问题

当前 Verify：

```text
read chunk
↓
等待
↓
hash
↓
read 下一 chunk
```

所以主要数据面有两个：

```text
CopyAndHash：read + write
VerifyBinary：read
```

## 5.4 io_uring 真正解决什么

不是只把：

```text
read()
```

换成：

```text
io_uring_prep_read()
```

核心是：

```text
一次允许多个 I/O request 在途
```

例如：

```text
Read chunk0
Read chunk1
Read chunk2
Read chunk3
↓
收 Completion
```

## 5.5 最容易做错的伪异步

错误：

```text
submit read
↓
wait cqe
↓
hash
↓
submit write
↓
wait cqe
```

这仍然：

```text
Queue Depth = 1
```

几乎没有发挥 io_uring 的价值。

## 5.6 正确目标：多 Buffer Pipeline

例如：

```text
QD = 4
Buffer0 = 1 MiB
Buffer1 = 1 MiB
Buffer2 = 1 MiB
Buffer3 = 1 MiB
```

流水：

```text
Read A
Read B
Read C
Read D
↓
Completion
↓
Hash
↓
Submit Write
↓
同时继续 Read 后续 chunk
```

## 5.7 Buffer 生命周期

```text
FREE
↓
READ_PENDING
↓
READ_COMPLETE
↓
HASHING
↓
WRITE_PENDING
↓
WRITE_COMPLETE
↓
FREE
```

最重要的规则：

```text
write CQE 没返回前
→ 这个 buffer 不能复用
```

## 5.8 为什么必须显式 offset

异步请求可能乱序完成，所以每个 request 必须记录：

```text
offset
length
buffer_index
operation
sequence
```

例如：

```cpp
struct IoRequest {
    std::uint64_t offset;
    std::size_t length;
    std::size_t buffer_index;
    Operation operation;
};
```

不要依赖共享 fd current offset。

## 5.9 BLAKE3 顺序问题

Completion 可能：

```text
chunk2
chunk0
chunk1
```

但 Hash 必须：

```text
chunk0
chunk1
chunk2
```

所以：

```text
I/O 可以乱序完成
↓
Hash 必须按 offset 顺序推进
```

可以维护：

```text
next_hash_offset
```

## 5.10 为什么不把整个 FileOps 全 io_uring 化

当前 FileOps 接口本质是同步：

```text
Read() → 返回结果
Write() → 返回结果
```

如果内部做：

```text
submit
↓
wait
↓
return
```

还是同步语义，浪费 io_uring。

更合理：

```text
                CopyEngine
                   │
        ┌──────────┴──────────┐
        ↓                     ↓
 SyncCopyEngine        IoUringCopyEngine
```

## 5.11 为什么第一版主要只改 read/write

技术上 io_uring 还能做：

```text
openat
statx
fsync
fdatasync
rename
unlink
close
...
```

但第一版不建议全部改。

### fdatasync 不优先

当前：

```text
Copy 完成
↓
fdatasync(temp)
↓
必须确认成功
↓
MarkTempWritten
↓
Verify
```

它本身就是持久化屏障，最终还是要等待完成。

### rename 不优先

当前 Crash-safe Commit：

```text
VerifiedReceipt durable
↓
rename-no-replace
↓
fsync(directory)
↓
SUCCEEDED
```

换成 io_uring rename 不会让它更原子、更 durable，而且每个 Task 只调用一次。

### open/stat 不优先

相比 1 GiB 文件的上千次 read/write，open/stat/rename/fsync 都是低频控制面。

因此优化优先级：

```text
read/write
>>>>>>>>>
open/stat/rename/fsync
```

## 5.12 与 SQLite 的关系

代码耦合很弱，只共同影响最终 E2E。

## 5.13 与 Worker Pool 的关系

耦合很强。

Worker Pool：

```text
Task 级并行
```

io_uring：

```text
单 Task 内 I/O 级并行
```

如果：

```text
4 Worker × QD=8
```

可能同时 32 个 I/O request，在虚拟机或普通磁盘上不一定更快。

因此要联合调参：

```text
Worker 数 × QD
```

## 5.14 与 BLAKE3 的关系

理想流水：

```text
磁盘读下一块
      ↘
        CPU Hash 当前块
      ↗
磁盘写上一块
```

但如果再叠加：

```text
Worker Pool
+
BLAKE3 TBB
+
io_uring 高 QD
```

可能：

```text
CPU 过载
+
I/O 队列过深
+
context switch 增加
+
cache miss 增加
```

## 5.15 Before Benchmark

```bash
mkdir -p benchmark-before-uring

./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 5 \
  --output benchmark-before-uring/copy.json \
  | tee benchmark-before-uring/copy.txt
```

记录：

```text
MiB/s
wall time
CPU
P95 / P99
RSS
read syscall
write syscall
```

## 5.16 strace

```bash
strace -f -c \
  -e trace=read,write,pread64,pwrite64,fdatasync,fsync,openat,close \
  -o benchmark-before-uring/strace-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

详细：

```bash
strace -f -ttT \
  -e trace=read,write,fdatasync,fsync \
  -o benchmark-before-uring/strace-detail.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

## 5.17 perf stat

```bash
perf stat \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -o benchmark-before-uring/perf-stat.txt \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

## 5.18 perf record

```bash
perf record -g \
  -o benchmark-before-uring/perf.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1

perf report \
  -i benchmark-before-uring/perf.data
```

## 5.19 改完后 strace 看什么

传统版：

```text
read
write
```

io_uring 版：

```text
io_uring_setup
io_uring_enter
io_uring_register
```

例如：

```bash
strace -f -c \
  -e trace=io_uring_setup,io_uring_enter,io_uring_register,read,write,fdatasync,fsync \
  -o benchmark-after-uring/strace-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

注意：

```text
syscall 数变少
≠
性能一定更高
```

最终还是看：

```text
MiB/s
wall time
CPU
```

## 5.20 改代码前检查环境

```bash
uname -r
```

```bash
cat /proc/sys/kernel/io_uring_disabled 2>/dev/null
```

```bash
grep CONFIG_IO_URING /boot/config-$(uname -r) 2>/dev/null
```

vcpkg：

```bash
./vcpkg/vcpkg search liburing
```

## 5.21 实施顺序

```text
1. 完全不改代码
2. 保存 Copy 1 GiB 基线
3. strace 看 read/write/fdatasync
4. perf 看 I/O 是否真值得优化
5. 检查 kernel / VM io_uring 支持
6. 引入 liburing
7. 写最小 IoUringContext RAII
8. 先只做 io_uring copy benchmark
9. explicit offset
10. 4 buffers / QD=4
11. 正确处理 short read / short write
12. 正确处理 CQE negative res
13. 正确处理 EOF
14. 管理 buffer lifetime
15. 加 BLAKE3，保证 Hash 顺序
16. 形成 IoUringCopyEngine
17. 接入 MigrationAttemptPreparer
18. fdatasync / receipt / rename / fsync 保持原语义
19. 如果 Copy 有收益，再改 VerifyBinary read
20. 测 QD=1/2/4/8/16
21. 和 Worker Pool 联合测试
22. 最后重新跑 E2E
```

---

# 六、四项优化的联合 Benchmark 设计

四项完成后不要只保留一个最终数字。

应该保留阶段性结果：

```text
Baseline
↓
SQLite optimized
↓
+ Worker Pool
↓
+ BLAKE3 optimized
↓
+ io_uring
```

## 6.1 SQLite 组

```text
exec + autocommit
prepared + autocommit
prepared + batch10
prepared + batch100
prepared + batch1000
```

指标：

```text
commands/s
wall time
CPU
fsync / fdatasync 次数
```

## 6.2 Worker Pool 组

```text
1 Worker
2 Worker
4 Worker
8 Worker
```

指标：

```text
E2E MiB/s
wall time
CPU
RSS
context-switches
SQLite busy / wait
```

## 6.3 BLAKE3 组

```text
hash_only：
普通 SIMD
TBB

Copy：
普通 SIMD
TBB

Worker：
1 Worker + SIMD
4 Worker + SIMD
1 Worker + TBB
4 Worker + TBB
```

指标：

```text
GB/s
cycles
instructions
context-switches
cache-misses
```

## 6.4 io_uring 组

```text
Sync
QD=1
QD=2
QD=4
QD=8
QD=16
```

再联合：

```text
Worker=1/2/4
×
QD=1/2/4/8
```

指标：

```text
MiB/s
wall time
CPU
RSS
context-switches
read/write syscall
io_uring_enter
```

---

# 七、最终推荐架构

```text
                        Frozen Plan
                             ↓
                      MigrationService
                             ↓
                     Acquire Epoch Once
                             ↓
                         Producer
                             ↓
                    ClaimNextReady()
                             ↓
                    Bounded Task Queue
                             ↓
             ┌───────────────┼───────────────┐
             ↓               ↓               ↓
          Worker1         Worker2         Worker3 ...
             ↓               ↓               ↓
       IoUringCopyEngine / SyncCopyEngine
             ↓
        Multi Buffer Pipeline
             ↓
          Async Read
             ↓
       BLAKE3 SIMD Hash
             ↓
         Async Write
             ↓
            temp
             ↓
         fdatasync
             ↓
    Independent Verification
             ↓
      VerifiedReceipt
             ↓
           resume
             ↓
     rename-no-replace
             ↓
       fsync(directory)
             ↓
         SUCCEEDED
```

SQLite 状态更新先保持：

```text
每 Worker 独立 Connection
↓
短事务
```

如果后续数据证明写竞争严重，再考虑：

```text
DB Command Queue
+
Single Writer
```

---

# 八、最终面试表达

不要只说：

```text
我用了 Prepared Statement、线程池、AVX2 和 io_uring。
```

更好的表达：

```text
我先建立 Scanner、Copy、SQLite 和 E2E 基线，
然后用 perf 和 strace 把 CPU、I/O 和持久化开销拆开。

SQLite 侧，我减少重复 SQL 编译和批量事务提交，
但保留 Crash Recovery 依赖的关键事务边界。

迁移执行侧，我把原来一次只能处理一个 Task 的模型改成
Producer + Bounded Queue + Worker Pool，
通过事务 Claim 保证一个 Task 只被领取一次，并用 Backpressure
限制队列和 RUNNING Task 数量。

Hash 侧，我没有自己手写 SIMD，
而是先用 perf 验证官方 BLAKE3 是否已经走 AVX2，
再通过 hash-only Benchmark 判断是否值得引入 TBB。

I/O 侧，我没有把整个 FileOps 全部 io_uring 化，
而是只优化高频数据面，把同步 read → hash → write
改成多 Buffer 异步流水，
而 rename、fsync、receipt 等 Crash-safe Commit 控制面仍保持原语义。

最后再联合测试 Worker 数、BLAKE3 并行度和 io_uring Queue Depth，
用数据决定配置，而不是单纯增加技术复杂度。
```

---

# 九、四项优化最重要的边界

## SQLite

不能为了 Batch：

```text
破坏 durable state 边界
```

## Worker Pool

不能只：

```text
加几个 std::thread
```

必须一起解决：

```text
Claim
Queue
Backpressure
Epoch
SQLite Connection
Resume 多 RUNNING
```

## BLAKE3

不能在官方库已经自动 SIMD 的情况下，说成“自己手写 SIMD 优化”。

必须先用 perf 证明实际路径。

## io_uring

不能：

```text
submit 一个
→ wait 一个
```

这种 QD=1 的伪异步。

也不要为了技术展示把低频控制面全部异步化。

---

# 十、最终执行 Checklist

```text
[ ] 保存原始 SQLite benchmark
[ ] 完成 Prepared Statement 对照
[ ] 完成 Batch Transaction 对照
[ ] Crash Recovery 回归通过

[ ] 保存单 Worker E2E 基线
[ ] 抽 ExecuteClaimedTask
[ ] 重构 ClaimNextReady
[ ] 实现 Bounded Queue
[ ] 实现 1/2/4/8 Worker
[ ] Resume 支持多个 RUNNING
[ ] Worker Pool E2E 回归通过

[ ] 检查 VM CPU Flags
[ ] 检查 libblake3 SIMD symbol
[ ] perf 确认运行时 SIMD 路径
[ ] 增加 hash_only Benchmark
[ ] 决定是否需要 TBB
[ ] 检查 Worker × TBB oversubscription

[ ] 保存同步 Copy 基线
[ ] strace 记录 read/write/fdatasync
[ ] perf 记录热点
[ ] 检查 io_uring kernel 支持
[ ] 引入 liburing
[ ] 实现 Multi Buffer + explicit offset
[ ] 正确处理 CQE / short I/O / EOF
[ ] 保证 Hash 顺序
[ ] 接入 Copy
[ ] 再评估 Verify
[ ] QD=1/2/4/8/16 对照

[ ] 最终联合 Benchmark
[ ] Worker × BLAKE3 × io_uring QD 调参
[ ] 保存最终 E2E 数据
[ ] 整理面试可讲的性能优化闭环
```

---

# 十一、最终原则

整个优化过程始终保持：

```text
先证明问题存在
↓
再改
↓
再证明改动有效
```

而不是：

```text
先上技术
↓
再想办法解释为什么用了它
```

PhotoBridge 最有价值的不是“技术数量多”，而是：

```text
性能优化
+
Crash-safe 语义不被破坏
+
Benchmark 可复现
+
perf / strace 有证据
+
每项复杂度都有明确收益
```
