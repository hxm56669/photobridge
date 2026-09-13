# PhotoBridge V1 代码收敛设计规则

## 1. 文档目的

PhotoBridge V1 不追求：

- 最大代码量；
- 最大模块数量；
- 最通用架构；
- 最完整扩展能力；
- 为未来版本提前建设基础设施。

V1 的目标是：

> 用尽可能少而清晰的代码，把真正重要的迁移语义、确定性、Linux 文件系统正确性、崩溃一致性、恢复与验证做深。

因此以后所有新增代码和已有代码重构，都必须遵守：

```text
业务正确性
>
系统正确性
>
可测试性
>
可读性
>
当前确定需要的扩展能力
>
未来可能需要的扩展能力
```

禁止为了：

```text
“以后可能有多个实现”
“这样架构更漂亮”
“企业项目一般这样写”
“设计模式比较完整”
```

提前增加复杂度。

---

# 2. 总原则：复杂度必须有归属

PhotoBridge 允许复杂，但复杂度只能主要存在于以下区域：

```text
1. Association Graph / LogicalAsset
2. Source Manifest / Mutation Guard
3. Metadata Resolution
4. Frozen Plan / Deterministic Replay
5. Task DAG / Idempotency / Fencing
6. Durable Barrier
7. Crash-safe Commit
8. Recovery Reconciliation
9. Verification
10. Fault Injection
```

这些地方复杂属于：

> 问题本身复杂。

以下地方默认必须保持简单：

```text
CLI
配置读取
日志封装
普通 DTO
简单数据转换
SQLite 基础 Wrapper
文件分类
命令分发
普通字符串工具
普通枚举转换
```

这些地方出现大量代码，默认视为设计警报。

---

# 3. 第一原则：一个抽象必须解决一个真实问题

任何新增：

```text
interface
abstract class
base class
factory
builder
provider
manager
registry
dispatcher
adapter
sink
strategy
context
wrapper
```

之前必须回答：

> 它具体消除了什么真实复杂度？

允许的理由主要只有五种。

## 3.1 多个真实实现

例如：

```text
SourceAdapter
├── LocalFolderSource
└── GoogleTakeoutSource
```

确实存在多个不同 Source，因此抽象成立。

禁止因为：

```text
“以后可能支持别的 Source”
```

提前建立一套尚无实际实现需求的抽象体系。

## 3.2 隔离外部副作用

例如：

```text
FileOps
```

它隔离：

```text
open
read
write
fdatasync
renameat2
fsync
```

并允许 Fault Injection。

这种抽象应保留。

## 3.3 为测试提供替换点

例如：

```text
LinuxFileOps
FaultInjectFileOps
```

这是实际需要的 Test Seam。

允许。

## 3.4 隔离生命周期或所有权

如果某个类确实负责：

```text
fd 生命周期
transaction 生命周期
task ownership
execution epoch
```

允许单独建模。

## 3.5 表达核心业务概念

例如：

```text
PhysicalAsset
LogicalAsset
AssociationEdge
MigrationPlan
MigrationTask
VerificationResult
```

这些类型代表 PhotoBridge 本身的问题空间。

必须保留。

---

# 4. 单实现 Interface 删除规则

如果一个抽象：

```cpp
class Foo {
public:
    virtual ...
};

class LinuxFoo final : public Foo {
};
```

同时满足：

```text
当前只有一个实现
+
测试不需要替换
+
不是 Source / Target 等明确扩展边界
+
没有隔离副作用
```

默认：

```text
删除 Foo interface
LinuxFoo → Foo
```

例如：

```text
DirectoryWalker
    ↓
LinuxDirectoryWalker
```

如果 V1 永远只有 Linux 实现，且测试不依赖替换：

可以直接：

```text
DirectoryWalker
```

内部实现 Linux traversal。

---

# 5. 一个概念只能有一个主要数据模型

禁止同一个语义不断产生中间类型：

```text
FileInfo
↓
FileDescriptor
↓
ScannedFile
↓
SourceFile
↓
ParsedFile
↓
AssetDescriptor
↓
PhysicalAssetRecord
```

如果它们表达的本质仍然是同一个对象，就应该合并。

PhotoBridge 推荐维持清晰的数据演化：

```text
Filesystem
    ↓
DirectoryEntry
    ↓
PhysicalAsset
    ↓
MetadataCandidate / RelationCandidate
    ↓
LogicalAsset / Photo IR
    ↓
MigrationPlan
    ↓
MigrationTask
```

只有发生了：

> 明确的语义跃迁

才允许产生新的核心类型。

---

# 6. DTO 不承担架构责任

普通结构体优先：

```cpp
struct MetadataCandidate {
    ...
};
```

不要轻易演化为：

```text
MetadataCandidateInterface
MetadataCandidateImpl
MetadataCandidateBuilder
MetadataCandidateFactory
MetadataCandidateView
MetadataCandidateRepository
```

数据对象只负责表达数据。

算法放到真正负责该语义的模块中。

---

# 7. 不允许 Wrapper 套 Wrapper

如果：

```text
A 调 B
B 调 C
C 调 Linux API
```

而 A/B/C：

- 不增加状态；
- 不增加不变量；
- 不改变错误语义；
- 不隔离生命周期；
- 不提供测试边界；

则中间层应该删除。

判断规则：

> 如果删除这一层以后，调用方只需要改一行函数名，那么这一层大概率没有存在价值。

---

# 8. Helper 收敛规则

`.cpp` 中允许存在匿名 namespace 小工具：

```cpp
namespace {

bool IsJson(...);
Status ParseTime(...);

}
```

但应满足：

```text
函数短
语义局部
只服务当前模块
没有自己的生命周期
没有独立业务状态
```

出现以下情况必须重新判断：

```text
匿名 namespace > 150～200 行

或

出现 5～8 个以上相关 helper

或

出现匿名 helper class

或

多个 .cpp 重复类似 helper
```

此时只能二选一：

```text
确实形成独立概念
→ 提升为正式模块

只是当前实现细节
→ 合并、简化、减少函数
```

不允许长期存在：

```text
正式类 80 行
匿名 namespace 500 行
```

---

# 9. 不提前实现未来阶段

当前阶段只允许实现：

```text
当前阶段所需能力
+
下一阶段马上需要的稳定接口
```

禁止：

```text
阶段 B 提前设计 Executor
阶段 C 提前设计 Task Repository
阶段 D 提前设计 Verification Framework
阶段 F 提前实现 Recovery Infrastructure
```

原则：

> 允许设计时知道未来方向，但禁止用现在的代码为尚不存在的问题买单。

---

# 10. YAGNI 是默认规则

以下理由不能作为新增代码的充分理由：

```text
以后可能支持 Windows
以后可能支持 S3
以后可能换数据库
以后可能换 Hash
以后可能多种 Walker
以后可能有插件
以后可能支持 RPC
以后可能拆成服务
```

只有：

```text
V1 已经需要
```

或者：

```text
为了核心正确性现在必须预留
```

才允许实现。

---

# 11. CLI 必须保持薄

CLI 只允许负责：

```text
解析参数
↓
建立依赖
↓
调用 Application/Core
↓
输出结果
```

允许：

```cpp
if (command == "scan") {
    return RunScan(...);
}
```

不需要为了 Command Pattern 创建：

```text
ICommand
CommandBase
CommandFactory
CommandRegistry
CommandDispatcher
CommandContext
CommandHandler
```

除非未来代码已经证明简单分发无法维护。

---

# 12. SQLite 只实现现在真正需要的持久化

数据库设计可以预先规划完整 Schema。

但代码实现必须按阶段推进。

例如阶段 C 不因为最终会有：

```text
migration_plan
plan_task
task_attempt
verification_result
```

就提前实现对应 Repository。

当前阶段：

```text
只实现当前真正读写的数据表
```

未来模块开始时再增加对应 persistence code。

---

# 13. Repository / DAO 默认不建立

SQLite 已经是一个明确的基础设施边界。

如果只有：

```text
一个调用点
一个数据库
一个实现
```

不要自动建立：

```text
IXxxRepository
SqliteXxxRepository
XxxDao
XxxMapper
```

优先使用：

```text
MigrationStore
```

或者少量明确的 storage service。

只有当某块存储逻辑已经明显形成：

```text
独立事务边界
复杂查询
独立状态机
```

才拆分。

---

# 14. Linux Correctness 代码不能为了减行数而删除

收敛代码不等于：

```text
换回 std::filesystem::copy_file
删除 FileIdentity
删除 fd-relative operation
删除 EINTR 处理
删除 fdatasync
删除 directory fsync
删除 renameat2
```

以下属于 PhotoBridge 技术核心：

```text
UniqueFd
FileIdentity
dirfd
openat/openat2
fstat/statx
Mutation Guard
renameat2
fdatasync
directory fsync
errno handling
short read/write handling
```

这些代码即使增加代码量，也属于：

> 有意义的复杂度。

---

# 15. 测试代码不因为收敛核心代码而削弱

V1 可以减少架构代码，但不能因此减少：

```text
Unit Test
Fixture Test
Fault Injection
Crash Test
Deterministic Replay Test
```

代码收敛主要针对：

```text
production abstraction overhead
```

而不是针对：

```text
correctness evidence
```

项目最终可以出现：

```text
核心实现 15k
测试 10k
```

这比：

```text
核心实现 25k
测试 2k
```

更健康。

---

# 16. 当前 ABC 已有代码的收敛流程

不要直接大规模删代码。

按以下顺序处理。

## 第一步：建立文件清单

对所有：

```text
include/photobridge/**
src/**
```

记录：

```text
文件名
代码行数
所属阶段
主要职责
直接调用者
直接依赖
```

先看复杂度分布在哪里。

## 第二步：每个类打标签

只允许以下标签：

```text
CORE
CORRECTNESS
TEST_SEAM
INFRASTRUCTURE
CONVENIENCE
PREMATURE
DUPLICATE
```

### CORE

PhotoBridge 核心业务概念。

例如：

```text
PhysicalAsset
MetadataCandidate
Source Manifest
```

保留。

### CORRECTNESS

保证正确性。

例如：

```text
FileIdentity
UniqueFd
RelativePath
FileOps
```

保留。

### TEST_SEAM

为了测试替换系统副作用。

例如：

```text
FileOps
```

保留。

### INFRASTRUCTURE

必要基础设施。

例如：

```text
Status
StatusOr
SqliteConnection
Logging
```

保持极简。

### CONVENIENCE

只是让调用更舒服。

仔细判断是否值得存在。

### PREMATURE

只为未来扩展存在。

优先删除。

### DUPLICATE

与已有模型或模块职责重叠。

必须合并。

---

# 17. 已有代码删除优先级

按照：

```text
P1 单实现 interface
        ↓
P2 Wrapper 套 Wrapper
        ↓
P3 重复 DTO
        ↓
P4 只使用一次的小类
        ↓
P5 Factory / Builder
        ↓
P6 Future Infrastructure
        ↓
P7 重复 Helper
```

逐层收敛。

最后才考虑修改真正的核心模块。

---

# 18. ABC 阶段推荐最终边界

## A：工程基线

建议只保留：

```text
Status
StatusOr
UniqueFd
Logging
SqliteConnection
FileOps
CLI Skeleton
CMake / Test Infrastructure
```

禁止继续发展 `common/` 成为大型自研框架。

`common/` 应该始终很小。

## B：Scanner + Source Manifest

主要保留：

```text
RelativePath
FileIdentity
DirectoryEntry
DirectoryWalker
PhysicalAsset
Scanner
Source Manifest
Mutation Guard
```

这一阶段复杂度主要应该来自：

```text
路径安全
fd-relative traversal
FileIdentity
symlink
TOCTOU
Mutation Detection
```

而不是来自：

```text
Walker interface hierarchy
Sink hierarchy
Builder hierarchy
```

## C：Google Takeout Parser

主要保留：

```text
PhysicalAsset Classification
JSON Sidecar Parsing
MetadataCandidate
RelationCandidate
Orphan Detection
Takeout Naming Rules
```

复杂度应该来自：

```text
Google Takeout 脏数据
文件关联证据
异常 JSON
时间字段
Sidecar 语义
```

而不是构建：

```text
Generic Parser Framework
Parser Factory
Parser Registry
Parser Strategy
```

---

# 19. 新类建立前必须通过“五问”

以后任何新增 class / interface 之前必须回答：

1. 它代表一个独立的业务概念吗？
2. 它是否拥有独立状态或生命周期？
3. 它是否维护一个明确的不变量？
4. 它是否形成真正的 Test Seam？
5. 如果不用它，调用方会明显变复杂吗？

结果：

```text
0 个 YES
→ 不允许建类

1 个 YES
→ 优先函数 / struct

2 个 YES
→ 谨慎考虑

3 个以上 YES
→ 类通常合理
```

---

# 20. 新 Interface 建立前必须通过“四问”

```text
现在是否真的有 ≥2 个实现？

或者

测试是否需要 fake implementation？

或者

它是否是 Source / Target 这样的明确系统边界？

或者

它是否隔离不可控外部副作用？
```

全部为 NO：

```text
禁止创建 interface。
```

---

# 21. 新文件建立规则

不要：

```text
一个 enum 一个文件
一个 20 行 struct 一个 .h + .cpp
一个简单 helper 一个模块
```

如果几个类型：

```text
生命周期一致
语义一致
总是共同使用
```

允许放在同一头文件。

只有当：

```text
独立变化
独立依赖
独立职责
```

明显成立时，再拆文件。

---

# 22. 函数拆分规则

不要为了：

```text
“一个函数不能超过 20 行”
```

机械拆函数。

如果一个完整操作：

```text
打开 fd
↓
校验 identity
↓
read
↓
再次校验
```

本身就是一个连续正确性流程，可以保持在同一个主函数中。

函数拆分标准是：

> 是否形成独立语义。

不是：

> 行数是否超过某个数字。

---

# 23. 新增代码的 Complexity Budget

每完成一个功能，需要判断：

```text
这个功能本质复杂度是多少？
```

简单功能：

```text
预计 50 行
实际 400 行
```

必须 Review。

核心算法：

```text
预计 500 行
实际 700 行
```

可能合理。

默认警报：

```text
简单功能实现代码
>
问题本身复杂度的 3 倍
```

说明可能存在过度架构。

---

# 24. 当前代码量目标

目前 ABC 已经约：

```text
7000 行核心代码
```

推荐经过收敛后目标：

```text
4000 ～ 5000 行
```

不是硬指标。

真正指标是：

```text
每一层都能解释为什么存在。
```

如果收敛后仍然：

```text
5500 行
```

但所有代码都对应：

```text
业务语义
Linux correctness
test seam
```

则可以接受。

禁止为了达到数字而删正确性代码。

---

# 25. 后续阶段代码预算意识

建议整个 V1 大致控制：

```text
A/B/C
4k～5k

D/E
Association / IR / Resolver
3k～4k

F/G
Plan / DAG / Fencing
3k～4k

H/I/J
Executor / Persistence /
Crash Commit / Recovery
4k～6k

K/L
Verification /
Fault Injection
2k～3k
```

最终：

```text
约 16k～22k 核心实现
```

属于合理范围。

不用追求严格数字。

作用只是防止：

```text
ABC 7000
D/E 再 7000
F/G 再 8000
```

最后失控到数万行，而大量代码只是架构层。

---

# 26. 后续功能取舍三级制度

以后想到任何新能力，统一分类：

## P0 — 核心正确性

直接影响：

```text
数据不丢
结果不重复
计划确定
Crash 后可恢复
结果可验证
```

必须实现。

例如：

```text
Mutation Guard
Plan Digest
Fencing
Durable Barrier
Reconciliation
```

## P1 — 明确提升当前闭环

对 V1 已有场景具有明显价值。

例如：

```text
必要 Path Collision Handling
Bounded Queue
Byte Budget
Copy + Hash Fusion
```

根据阶段实现。

## P2 — 展示型 / Future Work

例如：

```text
io_uring
lock-free queue
PMR
SIMD
S3
Immich
Windows
Plugin Framework
Chunk Resume
```

默认：

```text
不实现。
```

除非：

```text
Benchmark
或真实需求
```

证明必须做。

---

# 27. “面试价值”不能单独成为实现理由

不能因为：

```text
这个技术词写进简历很好看
```

就加入代码。

允许加入简历的前提：

```text
它解决了项目中的真实问题
+
你能说明为什么需要
+
有测试 / benchmark / failure case 支撑
```

否则：

```text
只学习
不实现。
```

---

# 28. 一个模块完成后的强制 Review

每组完成后必须回答：

1. 这个模块最核心的不变量是什么？
2. 哪些类型是真正业务模型？
3. 哪些类只是实现细节？
4. 是否存在单实现 interface？
5. 是否存在 Wrapper 套 Wrapper？
6. 是否存在重复 DTO？
7. 是否提前实现未来需求？
8. 是否存在超过 200 行的匿名 helper 区域？
9. 是否存在只调用一次但单独抽成 class 的组件？
10. 删除哪个类以后系统语义完全不受影响？

如果存在：

优先删除。

---

# 29. 判断一个模块是否已经“够了”

满足：

```text
功能闭环
+
核心异常处理完成
+
核心不变量有测试
+
接口足够支持下一阶段
```

之后：

```text
立即停止继续优化架构。
```

不要继续：

```text
重命名
抽象
泛型化
设计模式化
未来兼容化
```

直接进入下一阶段。

---

# 30. 最终设计风格

PhotoBridge V1 统一采用：

```text
核心模型明确
+
普通代码直接
+
系统边界有抽象
+
Linux 正确性严格
+
测试边界清楚
+
未来能力延迟
```

而不是：

```text
每层一个 Interface
每个动作一个 Class
每种构造一个 Factory
每种数据一个 DTO
每个模块一个 Manager
```

---

# 31. 最终判断口诀

以后看到一段新设计，依次问：

```text
这是业务复杂度吗？
    ↓
这是正确性复杂度吗？
    ↓
这是测试必须的吗？
    ↓
这是当前阶段必须的吗？
```

前三个都不是，第四个也是：

```text
NO
```

则：

> 不写。

对于已有代码：

```text
不能说明为什么存在
        ↓
删除

两个东西表达同一个概念
        ↓
合并

只有一个实现且没有测试价值
        ↓
去接口化

只为了未来
        ↓
延迟

核心正确性代码
        ↓
保留并做深
```

PhotoBridge 最终追求的不是：

> “我设计了很多层。”

而是：

> “每一层都有必须存在的理由。”
