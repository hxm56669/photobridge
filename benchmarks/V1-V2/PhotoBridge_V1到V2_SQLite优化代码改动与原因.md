# PhotoBridge V1 → V2 SQLite 性能优化代码改动记录

> 目标：记录从 `perf-v1-baseline` 到 `perf-v2-sqlite` 的 SQLite 优化到底改了什么、为什么这样改，以及 benchmark 为什么需要同步调整。
>
> 本文只讨论这一轮 SQLite 优化，不扩展到 Copy、BLAKE3、io_uring。

---

# 一、这一轮优化解决什么问题

V1 的主要问题可以概括为两层：

```text
重复 SQL
→ 每次重新 prepare / finalize

大量独立写入
→ 每次都产生事务提交和持久化成本
```

SQLite 写入真正昂贵的地方不只是：

```text
执行一条 INSERT
```

还包括：

```text
解析 SQL
→ 编译成 sqlite3_stmt
→ 加锁
→ 修改 WAL / 数据页
→ 提交事务
→ fsync
```

所以，如果大量小写入一直重复执行：

```text
prepare
→ bind
→ step
→ finalize
→ commit
```

固定成本会远大于真正写入一个整数或一条状态记录的成本。

V2 的优化方向因此是：

```text
第一层：Prepared Statement 复用
→ 减少重复解析 / 编译 SQL

第二层：Batch Transaction
→ 多条写入共用一次事务提交
→ 减少锁、write、fsync 等系统调用
```

---

# 二、V1 的 Statement 做法

V1 在 `task_runtime_repository.cpp` 内部定义了一个局部 `Statement` RAII 类。

核心逻辑相当于：

```cpp
class Statement final {
public:
    Statement(sqlite3* database, const char* sql) noexcept {
        result_ = sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement_,
            nullptr);
    }

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }
};
```

也就是说，每次业务函数执行时：

```text
进入函数
↓
构造 Statement
↓
sqlite3_prepare_v2()
↓
bind
↓
sqlite3_step()
↓
函数结束
↓
sqlite3_finalize()
```

下一次执行相同 SQL：

```text
再次 sqlite3_prepare_v2()
↓
再次 sqlite3_finalize()
```

---

# 三、V1 为什么存在性能问题

例如同一个任务生命周期中会反复执行：

```text
查询当前 epoch
更新 task
更新 attempt
插入 event
查询 runtime
检查 ownership
```

SQL 文本其实长期不变。

变化的只是：

```text
plan_id
task_id
attempt_id
epoch
state
```

因此 V1 的问题是：

```text
SQL 结构没有变化
↓
但每次调用都重新 prepare
↓
SQLite 重复解析 / 编译 SQL
↓
执行结束又 finalize
↓
下一次继续重复
```

这部分属于纯固定开销。

所以 V2 不应该继续：

```text
每执行一次 SQL
→ 创建一次 statement
→ 销毁一次 statement
```

而应该：

```text
SQL 只 prepare 一次
↓
后面反复 bind 新参数
↓
step
↓
reset
↓
继续复用
```

---

# 四、V2 新增 `SqliteStatement`

V2 新增：

```text
include/photobridge/app/sqlite_statement.h
src/app/sqlite_statement.cpp
```

并在顶层 `CMakeLists.txt` 中加入：

```cmake
src/app/sqlite_statement.cpp
```

这个类负责持有：

```cpp
sqlite3_stmt*
```

核心职责：

```text
Prepare
Reset
Finalize
```

---

## 4.1 `Prepare()`

V2 的关键逻辑：

```cpp
int SqliteStatement::Prepare(
    sqlite3* database,
    const char* sql) noexcept
{
    if (statement_ == nullptr) {
        result_ = sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement_,
            nullptr);
    }

    return result_;
}
```

最关键的是：

```cpp
if (statement_ == nullptr)
```

含义：

```text
第一次使用
→ sqlite3_prepare_v2()

后续再次使用
→ statement 已存在
→ 不再重复 prepare
```

这就是 Statement Cache 的基础。

---

## 4.2 `Reset()`

执行完一次 prepared statement 后，不销毁它，而是：

```cpp
sqlite3_reset(statement_);
sqlite3_clear_bindings(statement_);
```

作用分别是：

```text
sqlite3_reset
→ 把 statement 从上一次执行结果恢复到可再次执行状态

sqlite3_clear_bindings
→ 清掉上一轮绑定的参数
```

于是下一轮可以：

```text
Reset
↓
重新 bind 参数
↓
sqlite3_step
```

而不用：

```text
finalize
↓
重新 prepare
```

---

## 4.3 析构仍然 finalize

`SqliteStatement` 析构时：

```cpp
sqlite3_finalize(statement_);
```

所以资源生命周期变成：

```text
TaskRuntimeRepository 生命周期
↓
SqliteStatement 一直存在
↓
重复使用 prepared statement
↓
Repository 析构
↓
统一 finalize
```

这样仍然符合 RAII。

---

# 五、V2 新增 `ReusableStatement`

V2 在 `task_runtime_repository.cpp` 中增加了：

```cpp
class ReusableStatement final
```

它本身不拥有 `sqlite3_stmt*`。

真正的 statement 由：

```text
TaskRuntimeRepository
```

保存。

`ReusableStatement` 只是一次调用期间的使用包装。

核心流程：

```text
拿到缓存 SqliteStatement
↓
Prepare()
    ↓
    第一次真正 prepare
    后面不会重复 prepare
↓
Reset()
↓
bind
↓
step
↓
当前调用结束
↓
再次 Reset()
```

这样可以保证：

```text
statement 可以长期缓存
+
每次函数调用之间绑定参数不会串
```

---

# 六、`TaskRuntimeRepository` 从“只保存 connection”改成“保存 statement cache”

## V1

V1 私有成员基本只有：

```cpp
SqliteConnection* connection_;
```

所以 SQL statement 都在业务函数内部临时创建。

---

## V2

V2 增加了一组长期缓存：

```cpp
SqliteStatement insert_task_;
SqliteStatement check_dependency_cycle_;
SqliteStatement insert_dependency_;
SqliteStatement set_ready_;
SqliteStatement insert_event_;
SqliteStatement update_attempt_;

mutable SqliteStatement read_runtime_;
mutable SqliteStatement read_current_epoch_;
mutable SqliteStatement check_task_ownership_;

SqliteStatement read_attempt_state_;
SqliteStatement update_epoch_;
SqliteStatement claim_next_ready_;
SqliteStatement claim_task_;
SqliteStatement insert_attempt_;
SqliteStatement finish_task_;
SqliteStatement recover_task_;
SqliteStatement insert_receipt_;

mutable SqliteStatement read_receipt_;
```

含义就是：

```text
一个固定 SQL
→ 对应 Repository 中一个缓存 statement
```

例如：

```text
读取 epoch
→ read_current_epoch_

更新 attempt
→ update_attempt_

插入事件
→ insert_event_

领取任务
→ claim_task_

保存 VerifiedReceipt
→ insert_receipt_
```

---

# 七、业务函数具体怎么变化

以“读取当前 epoch”为例。

## V1

大致结构：

```text
ReadCurrentEpochForPlan()
↓
函数内部创建 Statement
↓
sqlite3_prepare_v2()
↓
bind plan_id
↓
step
↓
返回
↓
Statement 析构
↓
sqlite3_finalize()
```

下一次调用重新做一次。

---

## V2

改为：

```text
ReadCurrentEpochForPlan(
    database,
    read_current_epoch_,
    plan_id)
↓
ReusableStatement
↓
复用 read_current_epoch_
↓
Reset
↓
bind plan_id
↓
step
↓
Reset
```

SQL 没变，因此：

```text
prepare 一次
→ 重复执行很多次
```

---

# 八、为什么不直接把所有 SQL 改成 `connection.Execute()`

`connection.Execute()` 底层使用的是：

```text
sqlite3_exec()
```

它适合：

```text
BEGIN IMMEDIATE
COMMIT
ROLLBACK
PRAGMA
DDL
```

这类：

```text
执行次数少
+
不需要复杂参数绑定
```

但对于高频：

```text
INSERT
UPDATE
SELECT
```

如果每次都传 SQL 字符串：

```text
SQL 文本
→ SQLite 解析
→ 编译
→ 执行
```

无法充分复用编译结果。

所以 V2 的原则是：

```text
事务控制 SQL
→ Execute()

高频参数化 SQL
→ Prepared Statement + bind
```

---

# 九、事务安全语义没有为了性能被删掉

这一点很重要。

V2 并没有为了追求性能直接取消：

```text
BEGIN IMMEDIATE
COMMIT
ROLLBACK
```

例如任务完成、恢复状态切换、领取任务等关键操作仍然保持：

```text
BEGIN IMMEDIATE
↓
检查 epoch / ownership
↓
修改 task / attempt / event
↓
COMMIT
```

失败仍然：

```text
ROLLBACK
```

所以这一轮核心代码优化不是：

```text
为了性能关闭事务
```

也不是：

```text
关闭 synchronous=FULL
```

而是：

```text
保持原有一致性边界
+
减少 SQL prepare/finalize 的重复固定成本
```

---

# 十、SQLite 配置没有为了跑分快而改变

V1 和 V2 的 `SqliteConnection` 配置仍然保持：

```text
journal_mode = WAL
synchronous = FULL
foreign_keys = ON
busy_timeout = 5000 ms
```

也就是说没有通过：

```text
把 FULL 改成 OFF
```

这种方式“优化”性能。

这是正确的，因为 PhotoBridge 的重点之一就是：

```text
Crash-safe
```

如果为了 benchmark 直接降低 durability：

```text
性能数字会变好
但已经不是同一种可靠性语义
```

V1 / V2 就失去公平对比意义。

---

# 十一、第二层优化：Batch Transaction 的原理

除了 Prepared Statement，benchmark 还专门加入了 Batch Transaction 对比。

V1 写 5000 条数据时，可以理解成：

```text
INSERT 1
→ commit

INSERT 2
→ commit

INSERT 3
→ commit

...

INSERT 5000
→ commit
```

SQLite 每次 commit 都可能涉及：

```text
锁状态变化
WAL / page 写入
同步持久化
fsync
```

真正昂贵的不是：

```text
写入整数 1
```

而是：

```text
提交 5000 次事务
```

---

# 十二、Batch 后变成什么

例如 batch size = 256：

```text
BEGIN IMMEDIATE
↓
INSERT × 256
↓
COMMIT

BEGIN IMMEDIATE
↓
INSERT × 256
↓
COMMIT

...
```

5000 条命令不再对应：

```text
5000 次事务提交
```

而大约变成：

```text
5000 / 256
≈ 20 个事务
```

于是：

```text
事务提交次数
大幅下降
↓
fcntl / lock 操作下降
↓
pwrite / write 次数下降
↓
fsync 次数下降
↓
总 wall time 大幅下降
```

这通常比单纯 Prepared Statement 带来的提升更明显。

---

# 十三、为什么 Prepared Statement 和 Batch 要一起做

这两个优化解决的是不同成本。

## Prepared Statement

解决：

```text
SQL 解析 / 编译成本
```

从：

```text
prepare
step
finalize

prepare
step
finalize
```

变成：

```text
prepare 一次

reset
bind
step

reset
bind
step
...
```

---

## Batch Transaction

解决：

```text
事务提交 / 持久化成本
```

从：

```text
一条 INSERT
→ 一次 commit
```

变成：

```text
多条 INSERT
→ 一次 commit
```

所以：

```text
Prepared Statement
→ 优化 CPU / SQL 编译固定成本

Batch Transaction
→ 优化锁 / write / fsync / commit 固定成本
```

二者不是一回事。

---

# 十四、为什么不能无限增大 Batch

理论上：

```text
batch 越大
→ commit 次数越少
→ 吞吐越高
```

但不能只看吞吐。

batch 太大会带来：

```text
单事务持续时间变长
↓
写锁持有时间变长
↓
失败时一次 rollback 的工作更多
↓
单次提交延迟增大
```

因此不能简单：

```text
5000 条全部一次事务
```

然后就认为是最佳工程方案。

合理做法是测：

```text
10
100
256
1000
5000
```

再结合：

```text
吞吐
延迟
系统调用
事务持续时间
业务恢复粒度
```

选择折中值。

---

# 十五、为什么当前 benchmark 使用 256

当前用于 V2 正式 microbenchmark 的测试路径是：

```text
Prepared Statement
+
Batch Transaction
+
batch size = 256
```

目的不是修改：

```text
5000 commands
```

这个测试规模。

而是保持：

```text
V1：5000 commands
V2：5000 commands
```

只改变 SQLite 写入策略。

因此比较的是：

```text
同样完成 5000 条写入
↓
V1：Exec + Autocommit

对比

V2：Prepared + Batch 256
```

工作量不变，执行策略变化。

---

# 十六、Benchmark 为什么也必须修改

最开始 V2 代码改完以后，原 benchmark 默认仍然是：

```cpp
RunSqlite(
    options,
    SqliteMode::kExecAutocommit)
```

这意味着：

```text
代码已经进入 V2
↓
但 benchmark 还在主动模拟 V1 写法
↓
跑出来的 SQLite 数字仍然是旧路径
```

这种测试不能体现 V2 优化。

所以 benchmark 现在需要明确区分：

```text
sqlite-v1
→ ExecAutocommit

sqlite-v2
→ PreparedBatch
→ batch = 256
```

同时保留：

```text
sqlite-compare
```

用于实验不同 batch size。

---

# 十七、为什么不能删除 V1 Benchmark 路径

不能把旧逻辑直接删掉。

因为后面必须能够回答：

```text
V2 为什么快？
```

如果只有 V2：

```text
147000 commands/s
```

这个数字本身意义有限。

保留 V1 控制组后：

```text
相同机器
相同 Release
相同 5000 commands
相同 SQLite durability 配置

唯一主要变量：
SQLite 写入策略
```

这样性能结论才有说服力。

因此 benchmark 设计应该长期保持：

```text
sqlite-v1
→ 历史控制组

sqlite-v2
→ 当前正式策略

sqlite-compare
→ 参数实验组
```

---

# 十八、需要特别区分：核心主链优化 vs benchmark 实验

这是这一轮最重要的真实性边界。

## `perf-v2-sqlite` 核心代码可以明确证明的改动

当前 V2 tag 可以明确证明：

```text
新增 SqliteStatement
+
TaskRuntimeRepository 缓存 Prepared Statement
+
通过 Reset + rebind 重复使用 statement
+
关键事务一致性语义仍然保留
```

---

## 当前 benchmark 额外测试的内容

benchmark 还测试：

```text
Prepared Statement
+
Batch Transaction
+
batch = 256
```

这是为了量化：

```text
如果重复 SQLite 写入采用批事务
可以减少多少 commit / syscall 成本
```

因此在描述项目真实主链时要区分：

```text
Statement Cache
→ 已进入 V2 核心代码

Prepared + Batch 256
→ 当前 V2 benchmark 的性能策略测试
```

如果后续把 256 batch 真正接入主业务批量写入路径，再可以表述为：

```text
V2 主链正式使用 Prepared Statement + Batch Transaction
```

在此之前不要把 benchmark 实验路径直接说成所有主链数据库更新都已经按 256 条批量提交。

---

# 十九、这一轮修改可以怎么概括

最短版本：

```text
V1：
高频 SQL 每次重新 prepare / finalize
+
大量小事务重复提交

V2：
把高频 SQL 抽成可复用 SqliteStatement
→ Repository 内缓存 prepared statement
→ 每次只 reset + rebind + step

Benchmark：
增加 Prepared + Batch Transaction 对照
→ 256 条写入共用一次事务
→ 减少锁、write、fsync 和 commit 开销
```

---

# 二十、面试时怎么解释为什么这样优化

可以按下面逻辑：

```text
我先通过 V1 benchmark 和 strace 看 SQLite 写入路径。

V1 的问题不是单条 INSERT 本身计算复杂，
而是大量小写入不断重复 SQL prepare、锁操作和事务提交。

所以我把优化拆成两层：

第一层是 Prepared Statement 缓存。
TaskRuntimeRepository 中固定 SQL 只 prepare 一次，
后续通过 reset、重新 bind 参数和 step 来复用，
避免重复解析和编译 SQL。

第二层是批事务实验。
把多条写入放到一个事务里，
减少 commit、fcntl、pwrite 和 fsync 次数。

同时我没有为了跑分快去关闭 WAL、FULL synchronous
或者删除原来的事务边界，
所以优化前后的可靠性目标保持一致。
```

---

# 二十一、核心代码路径

这一轮重点阅读：

```text
include/photobridge/app/sqlite_statement.h
src/app/sqlite_statement.cpp

include/photobridge/app/task_runtime_repository.h
src/app/task_runtime_repository.cpp

src/app/sqlite_connection.cpp

benchmarks/photobridge_bench.cpp

CMakeLists.txt
```

---

# 二十二、最终理解

这一轮优化的核心不是：

```text
“SQLite 换了一个 API 所以变快”
```

而是：

```text
找到重复固定成本
↓
把 SQL 编译结果复用
↓
把多个小提交合并
↓
减少用户态重复工作
+
减少内核态系统调用和持久化次数
```

对应两个关键词：

```text
Statement Reuse
+
Transaction Batching
```

但在项目真实性表达上必须继续区分：

```text
核心代码已集成的 Statement Cache
vs
benchmark 中验证的 Batch 256 策略
```
