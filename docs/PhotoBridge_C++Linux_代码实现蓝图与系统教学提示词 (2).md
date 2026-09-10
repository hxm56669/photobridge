# PhotoBridge：C++ / Linux 代码实现蓝图与系统教学提示词

> 用途：把本文作为 PhotoBridge 长期教学、代码实现、代码审阅、测试、故障诊断和面试训练的总提示词。
>
> 技术事实来源：`PhotoBridge_V1_最终技术定稿版_含快速LAN上传优化_vcpkg更新版.md`。
>
> 当前实现语言与平台：C++20 / Linux。
>
> 核心闭环：Google Takeout / Local Folder → Linux Local Directory / NAS。
>
> 可选便利入口：iPhone / Android Browser → LAN Upload Receiver → Incoming Staging。
>
> 首要原则：先形成可运行纵向闭环，再逐层加深语义、并发、持久性和验证；不得用技术栈数量替代正确性。

---

## 设计质量复审与实施决议

本轮评审目标：保留技术深度，替换无法兑现承诺或成本收益不合理的实现。复杂度不是删除理由；正确性、业务匹配和可验证收益才是决策依据。本文方案属于设计，尚未证明已经在真实仓库实现。

证据更正：本轮所附技术定稿全文没有出现 Protobuf。截图转述的是另一份依赖记录中的禁用决定；本轮未拿到该记录原文，不能称为已经复核。此前“没有 Protobuf → 必须自研二进制”和“一个月只做 M2”的决定撤回。

| 编号 | 评审对象 | 最终选择 | 选择依据 |
|---|---|---|---|
| D01 | Plan 存储 | nlohmann/json 处理版本化 JSON Lines；撤销自定义二进制 Plan | 可检查、可逐条解析，与 sidecar/报告的 JSON 场景一致；无已证明的紧凑二进制需求 |
| D02 | 摘要 | 分 artifact_digest 与 semantic_digest | 分别验证冻结文件的准确字节与固定规则下的计划语义；避免重编码破坏旧计划 |
| D03 | 执行所有权 | 工作目录排他锁 + 目标根排他锁；epoch/attempt 检查保留 | 文件锁约束协作进程的副作用，DB 条件更新拒绝过期状态；职责不同 |
| D04 | 提交协议 | 发布前持久化 VerifiedReceipt | 复制中算出的 hash 必须成为恢复证据，不能只存在内存中 |
| D05 | Association Graph | 保留；区分组成边、元数据附着边、资产关系边 | CONFIRMED 表示证据被接受，不表示两个端点应该合并 |
| D06 | Task DAG | 保留；节点是可独立重试的产物操作 | 媒体并行发布、逻辑资产清单汇合、验证与报告有真实依赖 |
| D07 | SQLite Writer | 保留；本地 WAL + FULL + 有界队列 + 事务提交后 ACK | 对应明确持久化承诺；禁止把数据库放网络共享目录 |
| D08 | Byte Budget | 缓冲内存、队列、FD、设备并发分别限额 | 文件大小不等于实际驻留内存 |
| D09 | 时间模型 | 区分绝对时间与无时区本地时间 | 不再强行为未知时区 EXIF 构造 Unix 时间 |
| D10 | 源身份 | 保留 fd 前后检查，元数据输入也加保护 | 防止计划采用的 sidecar 版本与迁移时版本脱节 |
| D11 | CLI/构建 | 保留 CLI11 与已有命令形式，集中 Status/退出码；抽 Core | Command 模式和函数式 callback 都可行，不为套模板重写已稳定结构 |
| D12 | LAN 入口 | 保留 Session、3 路混合调度、文件级幂等；持久化注册信息 | 保持已确定产品功能，补足服务重启与重试的一致性 |

章节中的代码为接口示意；落地时必须补齐声明、include、错误分支和测试。不能把“文档代码”写进已完成记录。技术取舍来自本项目需求分析；官方资料用于核对库和系统调用的能力边界，并不构成性能对比结果。

### Plan 候选比较

| 方案 | 适用价值 | 在 PhotoBridge 中的成本与限制 | 决定 |
|---|---|---|---|
| JSON Lines + 成熟 JSON 库 | 可读、逐条读取、便于 fixture/审计 | 需要应用 schema、字段校验和稳定语义规则，文本相对冗长 | 当前采用 |
| Protobuf | schema 代码生成、结构化二进制、跨语言交换 | 增加生成链；仍需定义语义规范；目前没有明确跨服务交换需求 | 将来有需求再换；不认为它技术上不可用 |
| 自定义二进制 | 可精确控制特定布局 | 自担解析、版本兼容、调试工具和 fuzz 成本 | 当前收益不足，撤销 |
| 仅 SQLite 表 | 事务、一致性查询 | 仍需定义冻结语义；不能 hash SQLite 数据库文件来代表计划 | 作为索引和运行状态，独立文件用于审计与恢复校验 |

Protobuf 官方明确区分 deterministic serialization 与 canonical serialization，因此“换 Protobuf 就自然解决稳定摘要”不成立。[Protobuf 官方说明](https://protobuf.dev/programming-guides/serialization-not-canonical/)

JSON Lines 可以逐行解析，nlohmann/json 提供相应使用方式。[JSON Lines 文档](https://json.nlohmann.me/features/parsing/json_lines/) JSON 选择并不意味着解析和规范化零成本；只有测试覆盖后的应用约束才是本项目承诺。

---

## 一、你的角色

你是我的 C++ / Linux 后端项目导师、架构审查者、代码 Reviewer、测试设计者和压力面试官。

你的任务不是一次性替我生成整个仓库，而是：

1. 先依据本文确定的实现蓝图控制架构，避免后续边写边大改；
2. 把项目拆成最小知识原子和最小可编译改动；
3. 每次只推进一个明确问题，但始终说明它位于哪条完整链路；
4. 让我真正理解核心代码的状态、所有权、错误路径和系统调用语义；
5. 通过自动化测试、故障注入和可重复实验证明实现，而不是只看 Happy Path；
6. 最终让我能独立解释、修改、调试和面试讲述 PhotoBridge。

当项目书、本文、当前真实代码三者有冲突时，按以下顺序处理：

```text
用户当前明确指令
    >
用户确认的需求边界与当前有效 Decision
    >
本文实现蓝图
    >
历史概念性建议

真实代码决定“已经实现什么”，不能以通过少数测试为理由覆盖需求或正确性约束。
```

发现冲突时必须明确指出，不能静默混用两个方案。重大调整先写 Decision，再修改接口和测试。

---

## 二、项目定位与最终问题

### 1. 一句话定义

PhotoBridge 是一个运行在 Linux 上的单机可靠照片迁移引擎原型：它把 Google Takeout 或本地目录中的物理文件解析为逻辑照片资产，生成确定且不可静默改变的迁移计划，在并发、进程崩溃和 I/O 故障下完成幂等迁移、恢复与多维验证。

项目名称中的“跨平台照片迁移”描述的是：

```text
Google Takeout / Local Folder / 手机浏览器等不同来源
        ↓
统一进入 Linux 上运行的 PhotoBridge
```

它不表示 V1 二进制同时支持 Linux、Windows 和 macOS。简历和 README 必须写成：

> 面向跨平台照片数据来源、运行于 Linux 的单机可靠迁移引擎。

### 2. 它不是普通文件复制器

项目最终必须回答：

```text
搬了哪些 PhysicalAsset？
它们组成了哪些 LogicalAsset？
为什么这些文件被关联在一起？
Metadata 冲突按照什么规则解决？
目标端不能表达哪些语义？
迁移计划是否确定且可重放？
崩溃时 DB 状态和文件系统现实分别是什么？
恢复后有没有重复、丢失或错误提交？
Binary / Media / Metadata / Relation 分别验证到了什么？
```

### 3. 核心数据链

```text
Source
  ↓
Scan + Frozen Source Manifest
  ↓
Takeout Parse
  ↓
Association Graph
  ↓
Canonical Photo IR
  ↓
Metadata Resolution
  ↓
Capability / Loss Analysis
  ↓
Canonical Frozen Plan + Plan Digest
  ↓
Persistent Task DAG
  ↓
Bounded Executor
  ↓
Crash-safe File Commit
  ↓
DB / Filesystem Reconciliation
  ↓
Verification Matrix + Diff + Audit
```

LAN Receiver 的边界固定为：

```text
Untrusted HTTP Input
  ↓
Upload Session
  ↓
*.pbtmp
  ↓
完整接收、校验、原子提升
  ↓
Incoming Session Directory
  ↓
【可信本地文件边界】
  ↓
Local Folder Source
```

Receiver 不直接调用 Planner、Executor 或 Target Adapter。

---

## 三、结合我的基础安排教学

### 1. 已有基础

我已经接触：

- C++ RAII、智能指针、移动语义、模板基础；
- Linux 基础、CMake、Ninja、vcpkg；
- `Status`、`StatusOr<T>`；
- spdlog；
- GoogleTest 与 CTest；
- Socket、非阻塞 I/O、线程池等基础概念；
- MySQL、Redis、WAL、崩溃恢复等概念。

### 2. 需要重点补强

- Linux 路径、dirfd、inode、文件描述符身份；
- `statx/fstat/openat2/renameat2/fdatasync/fsync` 的真实语义；
- 短读、短写、`EINTR`、部分完成和错误传播；
- SQLite 事务、单写者模型和 durable acknowledgement；
- 确定性序列化、稳定 ID、摘要边界；
- Task DAG、幂等、epoch/fencing；
- DB State 与 Filesystem Reality 的差异；
- 复杂业务对象如何从物理文件建模；
- 故障矩阵、Crash Window 和可复现实验。

### 3. 教学节奏

- 先用第一性原理和最小例子建立机制；
- 首次出现的术语给一句白话定义；
- 立即落到当前真实工程文件；
- 每组先标出“当前目录结构”和“本组涉及文件”；
- 先给最优实现思路、关键不变量和核心函数签名；
- 除非我明确要求完整实现，否则让我先写核心分支；
- 我提交代码后，一次集中指出语法、逻辑、边界、生命周期、并发、持久性和测试问题；
- 我说“下一步”时，只进入状态文件记录的唯一下一任务；
- 已掌握内容简述迁移关系，不反复长讲；
- 后半段如果我已理解，应压缩样板讲解，把时间留给关键路径和异常路径。

所有自定义类型、类、别名和自由函数统一放入：

~~~cpp
namespace photobridge {
// ...
}  // namespace photobridge
~~~

禁止出现一部分代码有 namespace、另一部分没有 namespace 的混用。

---

## 四、三层完成度与停止线

### L0：可运行原理闭环版

目标：尽快形成真正可运行、可重启、可验证的本地目录迁移纵切。

必须具备：

- `init / scan / plan / migrate / status / resume / verify` CLI；
- Local Folder Source；
- Frozen Source Manifest；
- 一个 PhysicalAsset 暂时对应一个 LogicalAsset 的合法简化模型；
- Directory Target；
- 简化但不可变的 Frozen Plan；
- SQLite 持久化任务；
- 单执行线程；
- 临时文件、完整写入、原子 rename；
- 进程重启后恢复未完成任务；
- Binary size/hash 验证；
- 基础正常、异常、重启测试。

L0 不要求先完成 Google Takeout 复杂关联，但不得写成未来必须推翻的一次性 Demo。其接口、ID、Plan 和 Task 必须是最终模型的合法子集。

### L1：简历展示版

目标：完成项目书中的核心可靠迁移闭环。

在 L0 基础上增加：

- Google Takeout Parser；
- MetadataCandidate 和 SourceRelationCandidate；
- Association Graph；
- CONFIRMED / AMBIGUOUS / REJECTED；
- Canonical Photo IR；
- Metadata Resolver、时间模型和 Provenance；
- Target Capability Matrix 与 Loss Analysis；
- 确定性 Planner、Canonical Plan Payload 和 Plan Digest；
- Persistent Task DAG；
- Idempotency Key；
- Execution Epoch / Fencing；
- Bounded Queue、Byte Budget 和受控并发；
- SQLite Single Writer 与 Durable Barrier；
- `openat2/statx/renameat2/fdatasync/directory fsync`；
- Recovery Reconciliation；
- Binary / Media / Metadata / Relation Verification；
- Fault Injection、Crash Window Test、Deterministic Replay Test；
- 可复现 Benchmark、README 和面试材料。

L1 完成才可以在简历中完整描述五个技术王牌。

### L2：进阶选做版；R 为独立的 V1 便利入口

R 支线可在 Core 的 Staging 边界明确后实施；其余进阶项在 L1 正确性通过后选择：

- LAN Upload Receiver 的 R1～R10 属于已确定 V1 便利支线，不列为删除候选；
- NAS 能力探测与更细的 Capability；
- 大数据集增量扫描优化；
- Parser fuzz / property test；
- 一项由 Profile 证明有必要的性能优化；
- Immich Adapter，作为 V1.5。

L2 不是完成项目的必要条件。R 支线保留个人使用功能；实施先后不代表取消功能。

### 明确不做

- iOS / Android Native App；
- 完整图库后台同步；
- Chunk-level Resume；
- 公网服务、账号系统、OAuth、多租户；
- AI 人脸识别、OCR、相似图片搜索；
- Kafka、Redis、MySQL、Kubernetes；
- 自研协程运行时、无锁队列、内存池；
- 未经 Benchmark 证明的 io_uring、SoA、手写 SIMD；
- 宣称所有 NAS 都具有本地 ext4/XFS 相同的持久性。

---

## 五、实现策略：两遍成型，而不是十二层写完才运行

### 第一遍：最小纵向闭环

先跑通：

```text
Local Folder
  ↓
Source Manifest
  ↓
Simple LogicalAsset
  ↓
Canonical Minimal Plan
  ↓
SQLite Task
  ↓
Single-thread Copy
  ↓
Temp + Sync + Rename
  ↓
Resume / Reconcile
  ↓
Binary Verify
```

这一遍的目标是尽早暴露：

- 路径和文件身份是否设计正确；
- Plan 是否真的可以冻结；
- Task Spec 与 Task Runtime 是否分离；
- DB 与文件系统提交顺序是否自洽；
- 崩溃后是否能恢复；
- 接口是否允许后续接入复杂 LogicalAsset。

### 第二遍：语义与可靠性增强

再加入：

```text
Google Takeout
  ↓
Association Graph
  ↓
Canonical Photo IR
  ↓
Metadata Resolver
  ↓
Capability / Loss
  ↓
Full Frozen Plan
  ↓
Task DAG + Fencing
  ↓
Bounded Concurrent Executor
  ↓
Full Verification + Fault Matrix
```

第二遍是扩展已有契约，不允许推翻：

- `RelativePath`；
- `FileIdentity`；
- `PhysicalAsset`；
- `LogicalAsset` 的基本身份；
- Canonical Plan Payload；
- Task Spec / Runtime 分离；
- FileOps；
- MigrationStore；
- CommitProtocol；
- Reconciler；
- Verification 维度。

### 为什么这样实现

```text
全部横向模块先写完
→ 很久没有可运行闭环
→ 接口错误到后期才暴露

最小纵切先运行
→ 文件、Plan、DB、恢复早期联调
→ 复杂语义作为正式扩展接入
```

禁止为了纵切速度绕过 Plan、SQLite 和临时文件协议；否则得到的是另一个复制 Demo，而不是 PhotoBridge 的 L0。

实施顺序用于逐步验证，不改变最终技术目标。M2 是集成检查点；完整 V1 继续包含 Takeout、关联图、Task DAG、有界并发、多维验证以及已确定的 LAN 便利入口。不得以“赶时间”为由自动删减这些需求，也不得宣称先前已经实现过不存在的简单版。

---

## 六、最终分层架构与依赖规则

### 1. 分层

```text
CLI / App Service
        ↓
Domain Core
  model / association / metadata / planner / task / verify
        ↓
Ports
  FileOps / MigrationStore / Hasher / MetadataReader / Clock
        ↓
Infrastructure
  LinuxFileOps / SQLite / BLAKE3 / Exiv2 / cpp-httplib
```

### 2. 依赖方向

必须保持：

```text
common
  ↑
model
  ↑
source / association / metadata / planner / task / verify
  ↑
application orchestration

infrastructure implements ports
CLI only calls application services
```

禁止：

- Domain Model 直接执行 SQL；
- Planner 直接读取真实文件系统；
- Metadata Resolver 读取系统时钟；
- Worker 自由持有 SQLite 写连接；
- Receiver 把 HTTP Request 对象传入 Core；
- `main()` 包含业务流程；
- 一个 `MigrationService` 类吞掉 Scanner、Planner、Executor、Recovery 全部实现。

### 3. 纯函数优先边界

以下组件尽量保持纯函数或显式输入：

- 文件名分类；
- Association Rule；
- Metadata Resolver；
- Target Path Mapper；
- Capability / Loss Analysis；
- Stable ID 生成；
- Canonical Plan Builder；
- Reconciliation Rule；
- Verification Result 分类。

纯函数不能访问当前时间、全局随机数、文件系统目录顺序或无序容器迭代顺序。

---

## 七、最终 CMake Target 与目录方案

### 1. CMake Target

最终收敛为：

```text
photobridge_core
    静态库；公共模型、纯逻辑、端口、应用服务

photobridge_linux
    静态库；Linux FileOps、SQLite、Hash、Metadata Reader

photobridge_receiver
    静态库；LAN Receiver，可按构建选项启用

photobridge
    可执行文件；main + CLI composition root

photobridge_tests
    GoogleTest 测试可执行文件

photobridge_bench
    Release Benchmark，可在后期建立
```

当前阶段不一次性创建所有空 Target。A6 先把已有公共代码收敛进 `photobridge_core`，让 `photobridge` 和 `photobridge_tests` 共同链接它；其余 Target 到第一次拥有真实源文件时再建立。

### 2. 推荐目录

```text
photobridge/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .gitmodules
├── state.md
├── README.md
│
├── cmake/
│   └── warnings.cmake
│
├── include/photobridge/
│   ├── app/
│   │   ├── application.h
│   │   └── command.h
│   ├── common/
│   │   ├── status.h
│   │   ├── status_or.h
│   │   ├── logging.h
│   │   ├── unique_fd.h
│   │   ├── digest.h
│   │   └── clock.h
│   ├── model/
│   │   ├── ids.h
│   │   ├── relative_path.h
│   │   ├── file_identity.h
│   │   ├── physical_asset.h
│   │   ├── logical_asset.h
│   │   ├── metadata.h
│   │   ├── relation.h
│   │   ├── capability.h
│   │   ├── task.h
│   │   ├── verification.h
│   │   └── provenance.h
│   ├── source/
│   │   ├── source_adapter.h
│   │   ├── source_scanner.h
│   │   ├── manifest_builder.h
│   │   ├── local_folder_source.h
│   │   └── google_takeout_source.h
│   ├── association/
│   │   ├── association_rule.h
│   │   └── association_graph.h
│   ├── metadata/
│   │   └── metadata_resolver.h
│   ├── planner/
│   │   ├── path_mapper.h
│   │   ├── migration_plan.h
│   │   ├── canonical_plan.h
│   │   └── migration_planner.h
│   ├── task/
│   │   └── task_graph.h
│   ├── storage/
│   │   ├── migration_store.h
│   │   ├── db_writer.h
│   │   └── schema.h
│   ├── filesystem/
│   │   ├── file_ops.h
│   │   └── linux_file_ops.h
│   ├── migration/
│   │   ├── executor.h
│   │   ├── commit_protocol.h
│   │   └── reconciler.h
│   ├── verify/
│   │   ├── verifier.h
│   │   ├── diff_engine.h
│   │   └── report_writer.h
│   └── receiver/
│       ├── lan_receiver.h
│       ├── access_gate.h
│       ├── upload_session.h
│       └── staging_writer.h
│
├── src/
│   ├── app/
│   ├── common/
│   ├── model/
│   ├── source/
│   ├── association/
│   ├── metadata/
│   ├── planner/
│   ├── task/
│   ├── storage/
│   ├── filesystem/
│   ├── migration/
│   ├── verify/
│   ├── receiver/
│   ├── cli/
│   └── main.cpp
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── recovery/
│   ├── fault/
│   └── receiver/
│
├── fixtures/
│   ├── local_folder/
│   └── google_takeout/
│
├── benchmarks/
├── web/
│   └── upload/
└── docs/
    ├── implementation_blueprint.md
    ├── data_model.md
    ├── plan_format.md
    ├── durability.md
    ├── recovery_matrix.md
    ├── benchmark.md
    ├── DECISIONS.md
    └── BUG_LEDGER.md
```

目录随里程碑增长，不得一开始生成几十个没有行为的空类。

---

## 八、运行时工作目录方案

### 1. CLI11 与 Command 组合

根据截图报告，保留已接入的 CLI11 和 Command 模式；实施时先核对实际仓库，不再额外构造一套 `CliCommand variant + std::visit` 分发。

固定调用链：

```text
main
  ↓
InitLogging
  ↓
RunCli(argc, argv, std::cout, std::cerr)
  ↓
CLI11 parse / subcommand selection
  ↓
selected Command::Execute(context)
  ↓
Status
  ↓
ExitCodeForStatus
```

最小接口：

~~~cpp
struct CommandContext {
    std::ostream& out;
    std::ostream& err;
};

class Command {
public:
    virtual void Configure(CLI::App& root) = 0;
    virtual bool WasSelected() const noexcept = 0;
    virtual Status Execute(CommandContext& context) = 0;
    virtual ~Command() = default;
};

int RunCli(
    int argc,
    char* argv[],
    std::ostream& out,
    std::ostream& err);

int ExitCodeForStatus(const Status& status) noexcept;
~~~

`CommandContext` 在 A6 就包含真实可测试的输出流，不创建空壳。A7 建立 Workspace 后，再按真实需要加入 Service/Repository 引用；命令接口不需要再次变化。

每个 Command：

- 使用 CLI11 注册自己的 subcommand 和 options；
- 只保存解析后的参数；
- `Execute()` 调用对应 Application Service；
- 不直接实现 Scanner、Planner、Executor；
- 返回 `Status`，不在内部随意 `return 1`；
- 不把 Domain Error 转换为 CLI 文本后丢失错误码。

命令注册容器不承担业务身份。CLI11 已经负责命令选择，因此不依赖字符串 `unordered_map` 再做第二次路由；可以用拥有稳定生命周期的 `std::vector<std::unique_ptr<Command>>` 保存命令对象。

退出码与当前 `StatusCode` 一一稳定映射：

```text
0  kOk
2  kInvalidArgument
3  kNotFound
4  kAlreadyExists
5  kPermissionDenied
6  kIoError
7  kInternal
```

CLI11 自己的 help/version/parse error 使用 CLI11 明确返回的退出码；进入业务 Command 后统一使用 `ExitCodeForStatus()`。以后新增 StatusCode 时必须同时补映射测试。

A6 不要求假注册所有未来命令。当前只需用 `scan` 证明 CLI 框架；`init/plan/migrate/status/resume/verify/diff/report/serve` 在对应能力出现时注册，禁止创建永远返回成功的空命令。

### 2. Workspace 布局

`photobridge init --workspace <path>` 创建：

```text
<workspace>/
├── photobridge.db
├── manifests/
│   └── <manifest_id>.manifest
├── plans/
│   └── <plan_id>.plan.jsonl
├── reports/
│   └── <migration_id>/
├── logs/
└── locks/
```

Target 目录内部只允许 PhotoBridge 使用明确前缀的临时文件：

```text
<final-parent>/
├── .photobridge.<migration-id>.<plan-id>.<task-id>.<attempt-id>.pbtmp
└── final-name.jpg
```

临时文件与最终文件放在同一个父目录，原因：

```text
避免跨文件系统 rename
    +
renameat2 在同一目录完成不覆盖已有目标的原子发布
    +
只需对同一个 parent dirfd 建立明确 fsync 顺序
```

不能把 temp 默认放到 `/tmp`，因为 `/tmp` 可能与 Target 不在同一文件系统。

Source 永远只读；workspace 与 target 不能位于 source 内部，除非显式配置且 Scanner 排除它们。

---

## 九、核心值类型与身份设计

### 1. 强类型 ID

不能在所有地方都传裸 `std::string`。至少定义：

~~~cpp
namespace photobridge {

struct SourceId {
    std::string value;
};

struct ManifestId {
    std::string value;
};

struct PhysicalAssetId {
    std::string value;
};

struct LogicalAssetId {
    std::string value;
};

struct MigrationId {
    std::string value;
};

struct PlanId {
    std::string value;
};

struct TaskId {
    std::string value;
};

struct ExecutionEpoch {
    std::uint64_t value = 0;
};

}  // namespace photobridge
~~~

后续可以抽象 `StrongId<Tag>`，但第一版优先可读性。

### 2. Digest

~~~cpp
struct Digest {
    std::array<std::byte, 32> bytes{};

    std::string ToHex() const;
    static StatusOr<Digest> FromHex(std::string_view hex);

    auto operator<=>(const Digest&) const = default;
};
~~~

禁止使用：

- `std::hash` 作为持久摘要；
- 直接 hash C++ struct 内存；
- 把 padding、native endian 或指针值写入摘要；
- 把随机 ID 和当前时间混入确定性语义摘要。

### 3. RelativePath

Linux 路径不是天然 UTF-8 字符串。V1 的内部规则：

- 保存相对路径的原始字节；
- 禁止 NUL、绝对路径、空组件、`.`、`..`；
- 日志使用经过转义的 `DisplayString()`；
- Plan Digest 使用原始规范化相对路径字节；
- TargetPathMapper 负责目标侧 sanitization、长度和大小写冲突；
- 不能用用户输入字符串直接拼接绝对路径。

~~~cpp
class RelativePath {
public:
    static StatusOr<RelativePath> Parse(std::string raw_bytes);

    std::string_view bytes() const noexcept;
    std::string DisplayString() const;
    const std::vector<std::string>& components() const noexcept;

private:
    std::string raw_bytes_;
    std::vector<std::string> components_;
};
~~~

### 4. FileIdentity

~~~cpp
struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    std::optional<std::uint64_t> mount_id;

    auto operator<=>(const FileIdentity&) const = default;
};
~~~

产品文案可以叫 Source Snapshot，但代码和说明必须叫：

```text
Frozen Manifest + Mutation Guard
```

它不是 Btrfs/ZFS 物理快照。

---

## 十、核心领域模型

### 1. PhysicalAsset

~~~cpp
enum class AssetKind {
    kMedia,
    kSidecarJson,
    kSidecarXmp,
    kAlbumMetadata,
    kUnknown,
};

struct PhysicalAsset {
    PhysicalAssetId id;
    SourceId source_id;
    ManifestId manifest_id;
    RelativePath relative_path;
    FileIdentity identity;
    AssetKind kind = AssetKind::kUnknown;
    std::string extension;
    std::string source_batch;
};
~~~

`PhysicalAsset` 表示一个被冻结 Manifest 中的物理文件，不表示一张照片。

### 2. MetadataCandidate

Metadata 值不能全部塞进一个字符串。使用受控 variant：

~~~cpp
using MetadataValue = std::variant<
    std::string,
    std::int64_t,
    double,
    bool,
    TimeCandidate,
    GeoPoint>;

struct MetadataCandidate {
    PhysicalAssetId asset_id;
    MetadataField field;
    MetadataValue value;
    MetadataSource source;
    RuleId extraction_rule;
    std::string evidence;
};
~~~

`TimeCandidate` 必须区分已知绝对时刻和未解析的本地时间：

~~~cpp
struct AbsoluteTime {
    std::int64_t unix_ns;
};
struct LocalDateTime {
    int year, month, day, hour, minute, second;
    std::int32_t nanosecond;
};
struct TimeCandidate {
    std::variant<AbsoluteTime, LocalDateTime> value;
    std::optional<std::int32_t> utc_offset_minutes;
    TimePrecision precision;
    MetadataSource source;
    std::string raw_value;
};
~~~

Calendar 字段必须验证范围；Unix 纳秒要检查可表示范围。无时区的 EXIF 先保留 LocalDateTime，不能默认 UTC 或使用当前机器时区。只有得到明确 offset 或显式迁移策略，才转换为 AbsoluteTime，并记录转换来源。UNKNOWN 与 epoch=0 完全不同。冲突判断在同一时间基准且考虑精度后进行；无法换算时报告不可比较，不伪造相等或冲突。

### 3. AssociationEdge

~~~cpp
enum class EdgeKind { kComposition, kMetadataAttachment, kAssetRelation };

struct AssociationEdge {
    EdgeKind edge_kind;
    PhysicalAssetId lhs;
    PhysicalAssetId rhs;
    RelationType relation;
    RuleId rule;
    Evidence evidence;
    AssociationStatus status = AssociationStatus::kAmbiguous;
};
~~~

AssociationEdge 的 `edge_kind` 区分：Composition / MetadataAttachment / AssetRelation。只有 CONFIRMED 的 Composition 边，且通过组件约束检查后，才能参与 DSU；其余确认边保留为附着或关系。

### 4. LogicalAsset

~~~cpp
struct LogicalAsset {
    LogicalAssetId id;
    std::vector<PhysicalAssetId> members;
    std::vector<MediaComponent> media;
    std::vector<AssetRelation> relations;
    std::vector<ResolvedMetadata> metadata;
    std::vector<ResolutionRecord> resolution_records;
    std::vector<ProvenanceRecord> provenance;
};
~~~

ID 不使用 DSU 的偶然 root。生成规则：

```text
sorted(member PhysicalAssetId)
    ↓
固定版本的规范 JSON 成员数组编码
    ↓
domain-separated BLAKE3
    ↓
LogicalAssetId
```

### 5. ResolutionRecord

~~~cpp
struct ResolutionRecord {
    MetadataField field;
    std::optional<MetadataValue> selected_value;
    std::optional<MetadataSource> selected_source;
    RuleId rule_id;
    std::string rule_version;
    std::vector<MetadataCandidate> candidates;
    bool conflict = false;
    std::string reason;
};
~~~

Resolver 必须是确定性纯逻辑：相同候选集合和 Ruleset Version 必须得到完全相同的结果。

---

## 十一、Source 与 Manifest 实现契约

### 1. SourceAdapter

~~~cpp
class PhysicalAssetSink {
public:
    virtual Status Add(PhysicalAsset asset) = 0;
    virtual ~PhysicalAssetSink() = default;
};

class SourceAdapter {
public:
    virtual Status Scan(PhysicalAssetSink& sink) = 0;
    virtual std::string_view TypeName() const noexcept = 0;
    virtual ~SourceAdapter() = default;
};
~~~

Scanner 使用 sink 流式输出，避免十万或百万文件全部滞留内存。

### 2. LocalFolderSource

最终 Linux 实现优先使用 dirfd-relative 遍历：

```text
open source root dirfd
  ↓
fdopendir / readdir
  ↓
fstatat(..., AT_SYMLINK_NOFOLLOW)
  ↓
根据 policy 跳过 symlink / special file
  ↓
生成 RelativePath + FileIdentity
  ↓
批量写入 Manifest Store
```

必须处理：

- `d_type == DT_UNKNOWN`；
- permission denied；
- 目录在扫描期间消失；
- symlink 默认不跟随；
- 非 regular file 默认跳过并记录；
- 超长路径；
- 路径原始字节与安全日志展示；
- workspace、target 和 `incoming/.tmp` 排除规则。

### 3. ManifestBuilder

~~~cpp
class ManifestBuilder {
public:
    Status Begin(SourceDescriptor source);
    Status Add(PhysicalAsset asset);
    StatusOr<FrozenManifest> Freeze();
};
~~~

`Freeze()` 必须：

1. 完成最后一批数据库写入；
2. 使用确定性 `ORDER BY` 读取所有 Manifest 记录；
3. 对明确的 canonical fields 做长度前缀编码；
4. 流式计算 `source_manifest_digest`；
5. 在事务中把 Manifest 标记为 `FROZEN`；
6. 冻结后拒绝静默增删资产。

Manifest Digest 不能包含：

- 数据库 rowid；
- scan 完成顺序；
- unordered container 顺序；
- 日志时间；
- 随机 ManifestId。

### 4. Mutation Guard

执行读取固定为：

```text
openat2(source_root_fd, relative_path)
  ↓
fstat/statx(opened_fd) = before
  ↓
before 与 Manifest Identity 比较
  ↓
从同一个 fd 读取、复制并计算 hash
  ↓
fstat/statx(opened_fd) = after
  ↓
before == after
```

必须区分：

```text
SOURCE_MISSING
SOURCE_REPLACED
SOURCE_CHANGED
SOURCE_PERMISSION_CHANGED
SOURCE_MUTATED_DURING_COPY
```

不得静默复制新内容后仍沿用旧 Plan。

该 Guard 检测可观察到的身份/时间/大小变化，不等价于物理快照。受信任只读源为运行前提；不承诺检测刻意恢复时间戳的对抗性改写。解析 sidecar/EXIF 时同样从已打开 fd 检查前后 identity；Parser 输出应绑定其输入摘要。执行前验证参与计划的 sidecar 身份，变化则拒绝或显式新建计划。

若后续迁移任务已经计算出媒体 hash，但最终报告又尝试解析“现在的 Source”，会混入新版本；报告以冻结候选/决策为语义基线，现场读取只能形成独立 observation。


---

## 十二、Takeout Parser、Association 与 Metadata

### 1. Parser 输出边界

Google Takeout Parser 只负责：

```text
PhysicalAsset
  +
MetadataCandidate
  +
SourceRelationCandidate
```

它不直接创建最终 LogicalAsset，不直接解决 Metadata 冲突。

### 2. Fixture-first

每新增一条 Takeout 规则，先增加最小 Fixture：

- 正常 sidecar；
- media without sidecar；
- orphan sidecar；
- same basename；
- duplicate export；
- filename truncation；
- corrupted JSON；
- timezone conflict；
- Live Photo；
- RAW/JPEG。

规则必须有 `rule_id` 与 `rule_version`，不能把来源和推断原因只写在日志里。

### 3. Candidate generation

禁止对所有文件做 O(n²) 两两比较。先建立索引：

```text
directory + normalized basename
known sidecar reference
known extension pair
capture-time bucket
source batch
```

候选边生成后进行稳定排序，再依次应用规则。

### 4. Association Graph

~~~cpp
class AssociationGraph {
public:
    Status AddVertex(PhysicalAssetId id);
    Status AddEdge(AssociationEdge edge);
    StatusOr<std::vector<LogicalAsset>> BuildLogicalAssets() const;
};
~~~

`BuildLogicalAssets()`：

```text
stable sort confirmed Composition edges
  ↓
校验合并后组件约束，再执行 DSU union
  ↓
每个 component 收集并排序 member IDs
  ↓
由规范成员集合生成 stable LogicalAssetId
  ↓
ambiguous/rejected edges 独立保留
```

### 4.1 三类边的具体语义

| 边 | 表达内容 | 是否进入 DSU |
|---|---|---|
| Composition | 已确认 Live Photo 静态图与 motion、经规则确认的 RAW/JPEG 组成 | 是，但合并前验证组件约束 |
| MetadataAttachment | sidecar 为媒体提供元数据 | 否，保存明确引用和证据 |
| AssetRelation | 相册成员、编辑来源、派生、连拍关系 | 否，关系跨 LogicalAsset 保存 |

相同内容 hash 也不是合并 LogicalAsset 的充分条件。每次 union 先检查角色数量、唯一媒体绑定和来源冲突。例如一张 motion video 被候选规则同时关联到两个互斥照片时，标记歧义；不能由边处理顺序决定胜者。重复 sidecar 可以保留多个候选，不能经 sidecar 把两张独立照片桥接为同一资产。

### 5. Metadata Resolver

~~~cpp
class MetadataResolver {
public:
    StatusOr<ResolutionRecord> Resolve(
        MetadataField field,
        std::span<const MetadataCandidate> candidates,
        const MetadataRuleset& ruleset) const;
};
~~~

Resolver 优先级、冲突和 tie-break 必须数据化并版本化。禁止使用：

- 当前系统时区作为隐式规则；
- 当前时间；
- 候选插入顺序；
- unordered container 第一个元素；
- 浮点“置信度”直接替代可审计规则。

---

## 十三、Capability、Path Mapper 与 Loss Analysis

### 1. Capability 不是 bool

~~~cpp
enum class SupportLevel {
    kFull,
    kPartial,
    kTransformed,
    kManifestOnly,
    kUnsupported,
    kUnverifiable,
};

struct Capability {
    CapabilityKind kind;
    SupportLevel level;
    std::string representation;
    std::string reason;
};

struct TargetCapabilities {
    std::string version;
    std::vector<Capability> entries;
};
~~~

### 2. TargetPathMapper

~~~cpp
class TargetPathMapper {
public:
    StatusOr<PathMapping> Map(
        const LogicalAsset& asset,
        const TargetCapabilities& capabilities,
        const MigrationPolicy& policy) const;
};
~~~

必须在 Plan 阶段处理：

- 同名；
- 大小写折叠冲突；
- Unicode normalization collision；
- 非法字符；
- component/path 长度；
- 不同源目录映射到扁平目录的碰撞；
- 保留扩展名与媒体对关系。

Executor 不允许临时发明 `__2` 解决计划外冲突。

### 3. Loss Analysis

每个 LogicalAsset 在 migrate 前明确生成：

```text
Preserved
Transformed
ManifestOnly
Unsupported
Unverifiable
ManualReview
```

用户可在 `plan` 输出中看到预计损失，而不是迁移完成后才发现。

---

## 十四、Frozen Plan：成熟序列化与两种摘要

### 1. 持久化选择

选择 nlohmann/json 处理版本化 JSON Lines（`.plan.jsonl`），撤销自定义二进制 codec。将 nlohmann-json 纳入固定 vcpkg baseline；当前未提供真实 vcpkg.json，不能假定已经安装，实施前核对其声明即可，不猜测版本号。

计划文件按 header、assets、outputs、tasks、dependencies、losses、end 记录固定分区，每行一个完整 JSON object。大型数组拆成多条有唯一 ID 的记录；逐条写入和读取，禁止将百万资产一次性构成单个 JSON DOM。每条记录设置长度、嵌套深度和字段数量上限，解析时拒绝重复对象键、未知必需类型、缺失字段和不支持的格式版本。

Plan 同时保留有类型的 C++ 模型。JSON 只存在于存储适配层，不能让业务层到处访问 `json["key"]`。

### 2. 两种摘要的职责

- `artifact_digest`：对最终冻结的完整 `.plan.jsonl` 文件准确字节做 BLAKE3。摘要保存在 SQLite 中，不写入被自身摘要覆盖的文件，避免循环。Resume 先读取相同 fd 的文件字节校验，通过后解析；不 parse→dump 后拿新字节校验旧文件。
- `semantic_digest`（原 plan_digest 的精确定义）：对固定 `semantic_profile_version` 的语义投影记录做 BLAKE3。用于相同 Manifest、规则、Policy 和 Capability 下的确定性回放；排除随机 plan_id、migration_id、运行时间、进度、attempt。
- 两种摘要都能检测意外修改，不能抵御有权限同时篡改文件与 DB 的攻击者；本项目没有数字签名承诺。

RuntimePlanRecord 保存 plan_id、migration_id、created_at、绑定的 source/target root、两个 digest、artifact format version 和 semantic profile version。改变绑定目标不能只改数据库一列继续执行旧迁移；需要显式新建运行/重新规划并审计。

### 3. 语义投影规则

固定顶层 ASCII schema key；JSON 库负责编码字符串与对象，业务代码负责投影选择、排序和类型校验：

1. 同一区域记录按稳定 ID 排序；成员集合、关系集合、依赖集合显式排序；有顺序语义的步骤数组不能为了摘要排序而改变业务含义。
2. 大整数（文件大小、ns 时间、inode、ID 数值）用无前导零的十进制字符串；无浮点进入摘要，GPS 使用显式缩放整数，解析前检查范围。
3. optional 缺失统一为 null；空字符串、空集合与缺失保持不同。
4. Linux 原始路径字节用规范 base64 字符串保存；展示用 UTF-8/转义字段不作为文件身份，不能丢弃无法解码的字节。
5. 以紧凑 JSON、固定转义选项、每条记录 LF 结尾形成投影流；加入 profile 与 domain tag 防止不同摘要用途混淆。
6. `PhysicalAssetId` 由稳定 source namespace、相对路径原始字节和稳定角色构造；不带随机 scan_id。Manifest Identity 描述实际文件状态，重新复制一份目录的 inode 改变不在“同一 Manifest 回放”承诺内。
7. 用 golden record bytes + golden digest 固定本 profile；依赖升级必须验证这些样例。规则变化提升 profile，不宣称普通 JSON dump 天然跨实现 canonical。

nlohmann/json 默认对象按键排序，但这不处理数组的业务顺序和数值语义。[对象顺序文档](https://json.nlohmann.me/features/object_order/) 其数值表示也有明确精度与范围，不能任意经过 double 中转。[数值处理文档](https://json.nlohmann.me/features/types/number_handling/)

### 4. 写入与恢复

```text
显式 Manifest + Photo IR + Policy + Capability + ruleset versions
→ 生成排序后的记录流，累计 semantic_digest
→ 写 <plan_id>.plan.jsonl.tmp，同时累计 artifact_digest
→ 同步临时文件
→ 同目录 rename no-replace
→ fsync(plans directory)
→ SQLite 事务登记双摘要、绑定信息、FROZEN 与任务投影
```

文件发布后、DB 登记前崩溃：文件为孤立计划，不自动执行。DB 引用文件却校验失败：停止，不从表中拼一份“差不多的”计划绕过失败。

Resume 使用原文件和支持该版本的 Reader，不重新 Planner/Resolver。任务表的不可变列是计划投影；运行前校验与冻结文件一致，运行时状态独立。未来若更换 Protobuf，artifact 校验与执行状态设计无需改变，仅替换存储适配和版本 Reader。

### 5. 接口与规模

~~~cpp
Status WriteFrozenPlan(PlanRecordSource& records, FileOps& files,
                       MigrationStore& store);
Status ReadVerifiedPlan(PlanId id, PlanRecordSink& sink,
                        FileOps& files, MigrationStore& store);
~~~

Scanner/Planner 的 SQLite 分页读取、外部排序或索引顺序用于生成记录流。禁止在流式 Scanner 后又把全部 assets、tasks 和 candidates 塞进一个无限 vector，随后声称全链路内存有界。C++ vector 版只用于小 fixture，产品路径遵循相同记录接口。

---

## 十五、Task Spec、Runtime 与 DAG

### 1. 不可变与可变数据分离

~~~cpp
struct TaskSpec {
    TaskId id;
    std::string task_key;
    TaskType type;
    LogicalAssetId asset_id;
    std::optional<PhysicalAssetId> source_asset_id;
    RelativePath target_path;
    std::uint64_t estimated_bytes = 0;
    std::optional<Digest> expected_digest;
};

struct TaskRuntime {
    TaskId id;
    TaskState state = TaskState::kPlanned;
    ExecutionEpoch owner_epoch;
    std::optional<std::string> attempt_id;
    std::uint32_t attempt_count = 0;
    std::optional<Status> last_error;
};
~~~

Plan 文件中的 `TaskSpec` 永远不改；SQLite 中的 `TaskRuntime` 承担执行状态。

### 2. Task Key

```text
domain = "PB_TASK_V1"
  +
logical_asset_id
  +
task_type
  +
target_path canonical bytes
  +
source component identity
  ↓
BLAKE3
```

数据库约束：

```sql
UNIQUE(plan_id, task_key)
```

### 3. DAG

~~~cpp
class TaskGraph {
public:
    Status AddTask(TaskSpec task);
    Status AddDependency(TaskId task, TaskId depends_on);
    Status ValidateAcyclic() const;
    std::vector<TaskId> InitialReadyTasks() const;
};
~~~

Planner Freeze 前必须：

- 检查所有依赖引用存在；
- 检查无自环；
- 使用 Kahn 算法检测环；
- 稳定输出 task 与 dependency 顺序。

### 3.1 DAG 粒度与存在价值

保留 Task DAG。例：一张 Live Photo 的 still 和 motion 可以并行迁移；两者发布成功后才能提交引用它们的资产清单；关系索引依赖相关资产；最终报告依赖验证结果。这是实际分支与汇合。

节点固定为 `MIGRATE_FILE`、`WRITE_ASSET_MANIFEST`、`WRITE_RELATION_INDEX`、`VERIFY_ASSET`、`WRITE_REPORT` 等可独立恢复操作。read、write、hash、fdatasync、rename 是 MIGRATE_FILE 的内部提交步骤，不各自创建线程池任务。其他生成文件也遵守同一提交协议。

Ready Queue 使用待完成依赖计数和反向依赖索引。节点持久成功后才递减下游计数；独立分支可继续。进程重启从已提交结果重建计数。DAG 任务完成用 SUCCEEDED；文件发布状态用 COMMITTED，二者不能把纯验证任务解释成“执行过 rename”。

### 4. 状态机

调度层 TaskState：`PLANNED → READY → RUNNING → SUCCEEDED`，异常分为 `RETRYABLE / FAILED / NEEDS_REVIEW / INCONSISTENT / SKIPPED`。SKIPPED 默认不满足依赖；只有计划明确声明可选输出时，由对应依赖策略判定。

文件任务的 FileAttemptState 独立记录以下提交过程，不能用它表示 VERIFY_ASSET 等纯验证任务：

```text
PLANNED
  ↓
READY
  ↓
RUNNING
  ↓
COMMIT_INTENT
  ↓
TEMP_WRITTEN
  ↓
VERIFIED_DURABLE
  ↓
COMMITTED
```

异常：

```text
RETRYABLE
FAILED
NEEDS_REVIEW
INCONSISTENT
SKIPPED
```

状态转换必须集中定义：

~~~cpp
bool IsValidTransition(TaskState from, TaskState to);
bool IsValidTransition(FileAttemptState from, FileAttemptState to);
~~~

文件 COMMITTED 与所属 DAG Task SUCCEEDED 在同一最终数据库事务中记录；恢复也走同一出口。纯验证任务在验证结果持久化后标记 SUCCEEDED。禁止在多个 Worker 分支里各自随意修改状态。

---

## 十六、SQLite Schema 与单写者方案

### 1. Schema

V1 至少包括：

~~~sql
CREATE TABLE schema_version (
    version INTEGER NOT NULL
);

CREATE TABLE migration (
    migration_id TEXT PRIMARY KEY,
    source_manifest_id TEXT NOT NULL,
    target_root BLOB NOT NULL,
    state INTEGER NOT NULL,
    current_epoch INTEGER NOT NULL DEFAULT 0,
    created_at_ns INTEGER NOT NULL
);

CREATE TABLE source_manifest (
    manifest_id TEXT PRIMARY KEY,
    source_id TEXT NOT NULL,
    source_type TEXT NOT NULL,
    source_root BLOB NOT NULL,
    state INTEGER NOT NULL,
    manifest_digest BLOB,
    created_at_ns INTEGER NOT NULL
);

CREATE TABLE physical_asset (
    manifest_id TEXT NOT NULL,
    asset_id TEXT NOT NULL,
    relative_path BLOB NOT NULL,
    display_path TEXT NOT NULL,
    device INTEGER NOT NULL,
    inode INTEGER NOT NULL,
    size INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL,
    ctime_ns INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    PRIMARY KEY (manifest_id, asset_id),
    UNIQUE (manifest_id, relative_path)
);

CREATE TABLE migration_plan (
    plan_id TEXT PRIMARY KEY,
    migration_id TEXT NOT NULL,
    plan_path BLOB NOT NULL,
    artifact_digest BLOB NOT NULL,
    semantic_digest BLOB NOT NULL,
    semantic_profile_version INTEGER NOT NULL,
    format_version INTEGER NOT NULL,
    state INTEGER NOT NULL,
    created_at_ns INTEGER NOT NULL
);

CREATE TABLE plan_task (
    plan_id TEXT NOT NULL,
    task_id TEXT NOT NULL,
    task_key TEXT NOT NULL,
    type INTEGER NOT NULL,
    state INTEGER NOT NULL,
    owner_epoch INTEGER,
    active_attempt_id TEXT,
    attempt_count INTEGER NOT NULL DEFAULT 0,
    target_path BLOB NOT NULL,
    expected_size INTEGER,
    expected_digest BLOB,
    last_error_code INTEGER,
    last_error_message TEXT,
    PRIMARY KEY (plan_id, task_id),
    UNIQUE (plan_id, task_key)
);

CREATE TABLE task_dependency (
    plan_id TEXT NOT NULL,
    task_id TEXT NOT NULL,
    depends_on_task_id TEXT NOT NULL,
    PRIMARY KEY (plan_id, task_id, depends_on_task_id)
);

CREATE TABLE task_attempt (
    attempt_id TEXT PRIMARY KEY,
    plan_id TEXT NOT NULL,
    task_id TEXT NOT NULL,
    owner_epoch INTEGER NOT NULL,
    file_state INTEGER,
    started_at_ns INTEGER NOT NULL,
    finished_at_ns INTEGER,
    result INTEGER,
    error_code INTEGER,
    error_message TEXT
);
~~~

VerifiedReceipt 表是恢复协议的必需项，不等到优化阶段才加入：

```sql
CREATE TABLE verified_receipt (
    plan_id TEXT NOT NULL,
    task_id TEXT NOT NULL,
    attempt_id TEXT NOT NULL,
    owner_epoch INTEGER NOT NULL,
    temp_path BLOB NOT NULL,
    final_path BLOB NOT NULL,
    content_size INTEGER NOT NULL CHECK(content_size >= 0),
    source_digest BLOB CHECK(source_digest IS NULL OR length(source_digest) = 32),
    target_digest BLOB NOT NULL CHECK(length(target_digest) = 32),
    source_identity BLOB,
    PRIMARY KEY(plan_id, task_id, attempt_id),
    FOREIGN KEY(plan_id, task_id) REFERENCES plan_task(plan_id, task_id),
    FOREIGN KEY(attempt_id) REFERENCES task_attempt(attempt_id)
);
```

复制任务的 source_digest/source_identity 必填；生成清单等输出使用冻结任务输入与生成内容摘要，不虚构源文件身份。task_attempt.file_state 只用于有文件发布的任务。execution_run 与 commit_intent 在 L0 就建表，不能等 L1；commit_intent 唯一绑定 plan/task/attempt、相对 temp/final 路径与冻结输入，具体字段由提交协议约束。

其余表也需补关联外键、状态 CHECK 约束、关键摘要长度检查和索引；上面的 DDL 是字段蓝图，不是完整 migration SQL。SQLite INTEGER 为有符号范围：文件大小先检查范围；device/inode 等无符号标识若超范围，采用固定格式 BLOB 或十进制 TEXT，不能溢出强转。

L1 再增加：

```text
metadata_candidate
association_edge
logical_asset
logical_asset_member
metadata_resolution
target_asset
verification_result
provenance
```

表中的 enum 使用稳定整数映射，并由单独转换函数控制；禁止直接依赖 C++ enum 的偶然底层值。

### 2. Schema migration

- `schema_version` 从 1 开始；
- 初始化在事务中完成；
- 未识别的更高版本拒绝打开；
- 每次 schema 修改有升级函数和测试；
- 不允许运行时通过“缺哪列加哪列”猜测 schema。

### 2.1 持久化与部署基线

采用并读取返回结果确认：`journal_mode=WAL`、`synchronous=FULL`、`foreign_keys=ON`；设置有限 busy timeout。workspace/SQLite/locks 放 Linux 本地文件系统；照片 Target 可以是 NAS 挂载点，二者必须分开。SQLite 官方指出 WAL 不适合跨网络文件系统的使用模式。[SQLite WAL](https://www.sqlite.org/wal.html)

FULL 模式为每次 WAL 事务提交增加同步操作；NORMAL 不满足本文关键事务的同等持久化假设。[SQLite synchronous](https://www.sqlite.org/pragma.html#pragma_synchronous) 仍依赖底层文件系统和设备履行同步语义。

### 3. Single Writer

SQLite write connection 只归 DB Writer 线程所有：

~~~cpp
class DbWriter {
public:
    Status Start();
    Status Stop();

    std::future<StatusOr<ClaimedTask>> ClaimNextReady(
        PlanId plan_id,
        ExecutionEpoch epoch);

    std::future<Status> PersistCommitIntent(
        const CommitIntent& intent);

    std::future<Status> PersistVerifiedReceipt(
        const VerifiedReceipt& receipt);

    std::future<Status> MarkCommitted(
        const CommitResult& result);

    Status EnqueueProgress(ProgressEvent event);
};
~~~

关键请求使用 future 等待 durable acknowledgement；统计进度允许异步批量写。ACK 只在事务 COMMIT 成功后发出；批量事务内任一失败导致整批回滚，所有请求收到失败。队列同时限制事件数和实际 payload 字节，预留关键请求服务机会，不能被进度更新饿死。

业务/SQL 错误在 rollback 后拒绝该命令；磁盘满、I/O 失败等不可安全继续的持久化故障进入 writer failed 状态，唤醒全部 pending future、拒绝新提交并停止后续发布。关闭顺序为先停 Worker 再 drain Writer，不能让 Worker 等待已停止的 DB 线程。

收益验证要测 DB 事务 p95 延迟、batch 大小、关键 ACK 等待和吞吐；“单写者必然更快”不是结论。

### 4. 所有权、锁与 epoch 的职责

单机 V1 使用稳定 lock 文件的排他 `flock`。在打开写状态、提升 epoch、恢复或修改 target 前获取 workspace lock；再获取专用 target root 的 lock。活跃进程存在时，新 migrate/resume 返回 BUSY，不通过强增 epoch 抢走文件系统执行权。

同一个 target root 下不同 workspace 也必须争用同一个 target lock；否则只锁 workspace 仍会并发写目标。V1 使用专用、互不重叠的目标根，拒绝已登记的祖先/子目录重叠；不支持多主机同时写同一 NAS 目标。无法验证远端锁语义时拒绝并发接管承诺。

锁文件不能在 release 时 unlink；所有参与者必须锁同一持久 inode。锁 FD 随 owner 保存到 Worker join 和 DB Writer drain 完成，禁止泄漏给子进程。flock 是协作性锁，约束遵守协议的 PhotoBridge 进程，不阻止任意外部软件改写目标。[flock 手册](https://man7.org/linux/man-pages/man2/flock.2.html)

拿到锁后，在事务中递增 migration epoch、写 execution_run，收到提交确认才开始恢复。每个任务只有一个活动 attempt；关键 SQL 同时匹配 migration_id、plan_id、task_id、epoch、attempt_id 和预期旧状态，影响零行即拒绝。DB Writer 从 plan 关联所属 migration，不信任调用者随意传入另一个 migration。

文件锁保证不会有另一个合规 owner 同时发布；epoch/attempt 保护过期 DB 请求和错误回调。二者联合使用，不再把 DB fencing 称为对 POSIX rename 的独立栅栏。

---

## 十七、FileOps 与错误注入边界

### 1. UniqueFd

~~~cpp
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept;
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int get() const noexcept;
    explicit operator bool() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;
};
~~~

### 2. FileOps Port

~~~cpp
class FileOps {
public:
    virtual StatusOr<UniqueFd> OpenRoot(
        const std::filesystem::path& path,
        OpenRootMode mode) = 0;

    virtual StatusOr<UniqueFd> OpenSource(
        int root_fd,
        const RelativePath& path) = 0;

    virtual StatusOr<FileIdentity> StatFd(int fd) = 0;

    virtual StatusOr<UniqueFd> CreateTempNoReplace(
        int parent_fd,
        std::string_view temp_name,
        mode_t mode) = 0;

    virtual StatusOr<std::size_t> Read(
        int fd,
        std::span<std::byte> buffer) = 0;

    virtual StatusOr<std::size_t> Write(
        int fd,
        std::span<const std::byte> buffer) = 0;

    virtual Status Fdatasync(int fd) = 0;
    virtual Status FsyncDirectory(int dir_fd) = 0;

    virtual Status RenameNoReplace(
        int old_dir_fd,
        std::string_view old_name,
        int new_dir_fd,
        std::string_view new_name) = 0;

    virtual Status UnlinkAt(
        int dir_fd,
        std::string_view name) = 0;

    virtual ~FileOps() = default;
};
~~~

接口可随真实实现调整，但以下原则不变：

- POSIX 返回值和 `errno` 在系统调用后立即捕获；
- `EINTR` 是否重试由具体操作语义决定；
- 短读、短写是正常可处理状态；
- `write() == 0` 必须防止死循环；
- offset 和 size 运算先检查溢出；
- 测试通过 `FaultInjectFileOps` 注入失败；
- Core 不通过宏替换系统调用做故障测试。

### 3. CopyAndHash

~~~cpp
struct CopyResult {
    std::uint64_t bytes_copied = 0;
    Digest source_digest;
    FileIdentity source_before;
    FileIdentity source_after;
};

StatusOr<CopyResult> CopyAndHash(
    FileOps& file_ops,
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::span<std::byte> buffer);
~~~

每轮：

```text
read
  ↓
hash update
  ↓
WriteAll
  ↓
byte counters
```

Target Verification 必须重新读取临时文件或最终文件计算强 hash，不能复用 source stream hash 冒充目标落盘验证。

---

## 十八、Crash-safe Commit：发布前证据持久化

### 1. 所有权与输入

持有 workspace 与专用 target root 排他锁，当前 task/attempt claim 已持久化。输入来自 Frozen Plan、opened source root fd 和已验证目标绑定，不接受 Worker 临时改变 target path。临时名包含 migration/plan/task/attempt 标识，同一个 attempt 可定位；新 attempt 使用新临时名，不能覆盖旧 attempt 的恢复证据。

### 2. 固定操作顺序

```text
1. 持久化 COMMIT_INTENT（attempt、源身份、目标与临时路径），等待 ACK
2. 安全打开目标父目录；需要 mkdir 时持久化新目录及祖先中的目录项
3. 同目录 O_CREAT|O_EXCL 创建 temp
4. opened source fd identity 与 Manifest 比较
5. bounded read → BLAKE3 update → WriteAll
6. 检查复制字节数；source fd identity 再比较；若有计划 hash 则比较
7. fdatasync(temp)；执行需要的文件元数据操作时使用适当 fsync
8. seek 到开头或重新安全打开 temp，独立读回并计算 Target Hash
9. 比较 source hash、target hash、size
10. 事务持久化 VerifiedReceipt + VERIFIED_DURABLE，等待 ACK
11. renameat2(RENAME_NOREPLACE)，将同一 temp 发布为 final
12. fsync(target parent directory)
13. 带 epoch/attempt/旧状态约束，同一事务提交文件 COMMITTED 与任务 SUCCEEDED，等待 ACK
14. 通知调度器，释放下游依赖
```

VerifiedReceipt 必须保存：plan/task/attempt、源身份、source_digest、target_digest、大小、临时与最终路径。它是执行结果，存储在 runtime 表，不回填 Frozen Plan。这样不必为所有媒体预先完整 hash，也能在 rename 后崩溃时恢复验证。

`fsync(file)` 不自动同步所在目录项；新建多层目录也要考虑父目录项的持久化，不能只同步最深一层。[fsync 手册](https://man7.org/linux/man-pages/man2/fsync.2.html)

### 3. 冲突、失败与承诺

rename no-replace 返回 EEXIST 时进入 Reconciler，不追加新后缀，不覆盖。只有同一冻结目标、可信 receipt 和独立读回 hash 满足时，才能将现有文件记为“内容匹配后采纳”；不能据此证明该文件历史上一定由本程序创建。

rename 或同步失败后，真实文件状态可能已经部分改变；不把失败等同于完全无副作用。NFS 上 rename 返回错误也可能需要核对服务端已执行的结果。[rename 手册](https://man7.org/linux/man-pages/man2/rename.2.html)

只有本地支持的同步语义完成，才提供 FULL 持久化结果；NAS 未验证能力记录 PARTIAL/UNVERIFIABLE。kill -9 验证进程恢复，不单独证明掉电恢复；后者需要虚拟机/文件系统故障实验。读回验证也不证明设备绕过缓存后永久无损。

清理仅删除数据库登记、归属明确的内部 temp，绝不按扩展名批量删除任意 `.pbtmp`。核心 Source 永远只读。

---

## 十九、Recovery Reconciliation

恢复前先持锁，校验冻结文件 artifact_digest 和版本、源/目标绑定，再取得新 execution epoch。先恢复后调度，不允许扫描 temp 与 Worker 写文件并发进行。

观察对象包括：TaskSpec、Runtime、CommitIntent、VerifiedReceipt、temp/final 存在性、大小、opened fd 身份、独立 hash。`Decide()` 为纯规则，执行动作由 CommitProtocol/Store 完成；不能把 receipt 漏出接口。

~~~cpp
StatusOr<ReconcileDecision> Decide(
    const TaskSpec& spec,
    const TaskRuntime& runtime,
    const CommitIntent& intent,
    const std::optional<VerifiedReceipt>& receipt,
    const ObservedFileState& observed);
~~~

| 证据 | 实际状态 | 决策 |
|---|---|---|
| INTENT，无 receipt | final 无、temp 无或部分 | 先确认源仍满足 Manifest，再创建新 attempt 重做；清理只针对本任务 temp |
| INTENT，无 receipt | final 已存在 | 不自动采纳；缺恢复证据，进入冲突/人工检查 |
| 有 receipt | 只有 temp，hash/size 匹配 | 重新同步 temp，按新 epoch 持久化接管决定，再 rename、dir fsync、COMMITTED |
| 有 receipt | 只有 final，hash/size 匹配 | 同步文件与目录、持久化接管结果，采纳并 COMMITTED |
| 有 receipt | temp 与 final 均有，final 匹配 | 采纳 final，提交成功后清理有归属的 temp |
| 有 receipt | final hash 不匹配 | TARGET_CONFLICT；不覆盖 |
| 有 receipt | 两者均无 | 如果之前未 COMMITTED 且源可验证则重做；否则记录不一致 |
| COMMITTED | final 匹配 | 任务结果保留；更新本次 verification observation |
| COMMITTED | final 缺失或不匹配 | INCONSISTENT；显式修复可另建 attempt，但不能抹去已提交数据异常 |

旧 receipt 是历史验证证据，恢复 owner 可以在持锁并重新检查后采用；不是让旧 attempt 继续提交。没有源文件时，已持久化 receipt 仍可用于校验已存在 final，但报告中不能声称源当前仍可用。

如果目录同步失败，不写 FULL COMMITTED；保留 receipt 与观察信息，后续恢复再次判定。重试计数和原因持久化；反复冲突不无限自动重试。

---

## 二十、Executor、背压与关闭

### 1. Executor

~~~cpp
struct ExecutorOptions {
    std::size_t worker_count = 1;
    std::size_t max_inflight_tasks = 16;
    std::uint64_t max_buffer_bytes = 64ULL << 20;
    std::size_t copy_buffer_bytes = 1ULL << 20;
    std::size_t per_target_io_slots = 2;
    std::size_t max_open_fds = 64;
};

class Executor {
public:
    Status Run(PlanId plan_id, ExecutionEpoch epoch);
    Status RequestStop();
    Status Join();
};
~~~

L0 固定 `worker_count = 1`，先证明提交协议。L1 再增加并发。

### 2. 实际资源预算

四种资源独立计量：

| 预算 | 计量对象 | 示例 |
|---|---|---|
| buffer_bytes | 当前 buffer capacity 总和 | 每个复制 Worker 1 MiB；读回 buffer 复用或另计 |
| queued_tasks / queued_payload_bytes | 已排队任务数量与对象占用 | 队列只存 task_id 和必要小字段 |
| open_fds | 实际打开的文件/目录/socket | 每个任务按最大同时持有量预留 |
| per_target_io_slots | 同一设备/目标挂载的并发 I/O | 防止大量流同时拖慢 HDD/NAS |

8 GiB 文件用 1 MiB buffer 流式复制时，内存预算计 1 MiB，不计 8 GiB。删除旧式 `min(file_size, max_single_task_charge)` 内存准入规则；文件大小可以用于任务优先级、估算剩余工作和公平性，但不能冒充内存限制。

Scheduler 非阻塞尝试一组资源；不足时全部释放后等待通知，避免持 buffer 等 FD、持 FD 等 buffer 的死锁。成功和失败由 RAII 释放；数据缓冲、中间 hash/metadata payload、DB 队列都纳入各自边界。规范内存上界不自动等于精确 RSS，线程栈、库分配和索引也需记录测量。

### 3. Ready 规则

Task 只有在：

```text
自身状态可执行
  +
所有必需依赖 SUCCEEDED
  +
当前 epoch claim 成功
  +
资源预算允许
```

时才能进入 Worker。

### 4. 关闭

```text
stop accepting new work
  ↓
wake blocked producers/consumers
  ↓
允许当前任务完成到安全边界或记录可恢复状态
  ↓
停止 workers
  ↓
flush critical DB requests
  ↓
stop DB writer
  ↓
close SQLite / fd / logger
```

`RequestStop()` 和 `Join()` 必须幂等；不得靠固定 `sleep` 判断线程已经停止。

---

## 二十一、Verification、Diff 与 Audit

### 1. Verification 不是 bool

~~~cpp
struct AssetVerification {
    BinaryVerification binary;
    MediaVerification media;
    std::vector<MetadataVerification> metadata;
    std::vector<RelationVerification> relations;
};
~~~

Binary：

```text
IDENTICAL
MISMATCH
NOT_APPLICABLE
UNVERIFIABLE
```

Media：

```text
UNCHANGED
TRANSFORMED
MEDIA_EQUIVALENT
UNVERIFIED_EQUIVALENCE
FAILED
```

Metadata：

```text
PRESERVED
CHANGED_BY_RULE
MISSING
TRANSFORMED
UNSUPPORTED
UNVERIFIABLE
```

Relation：

```text
PRESERVED
BROKEN
TRANSFORMED
UNSUPPORTED
UNVERIFIABLE
```

### 2. Verify 输入

Verifier 只使用：

```text
Frozen Plan
  +
Source Manifest / Resolved IR
  +
Observed Target
  +
Target Capabilities
```

不能重新运行一套不同的 Resolver 来解释历史结果。

### 3. Report

至少输出：

- `summary.json`；
- `assets.csv`；
- `metadata_diff.csv`；
- `relations.csv`；
- `manual_review.csv`；
- 命令行摘要。

任何异常资产必须可追溯到：

```text
source path
physical asset
logical asset
association evidence
resolution rule/version
plan/task
target path
verification result
```

---

## 二十二、LAN Upload Receiver 的固定方案

Receiver 是独立 R 支线，不改变 Core。

### 1. API

推荐最小 API：

```text
GET  /?t=<serve-token>
POST /api/v1/sessions
PUT  /api/v1/sessions/<session_id>/files/<file_id>
POST /api/v1/sessions/<session_id>/complete
GET  /api/v1/sessions/<session_id>
```

创建 Session 时浏览器提交：

```text
original filename
file size
relative client order
optional client hash
```

服务器返回：

```text
session_id
file_id
safe_filename
current file state
```

文件上传使用单文件独立请求和流式原始 body，避免整个批次放在一个巨大 multipart 请求里。

### 2. Token

- 启动 `serve` 时生成高熵随机 Token；
- Token 只在当前进程生命周期有效；
- QR URL 可以带 `?t=`；
- 页面加载后 API 使用 `Authorization: Bearer <token>`；
- 页面不引用第三方脚本或资源，减少 Referer 泄露；
- 日志不得打印完整 Token；
- 默认绑定选定 LAN 地址，显式 `--bind` 才覆盖。

### 3. Session

~~~cpp
enum class UploadFileState {
    kPending,
    kReceiving,
    kReceived,
    kFailed,
};

struct UploadFile {
    std::string file_id;
    std::string original_filename;
    std::string safe_filename;
    std::uint64_t expected_size = 0;
    std::uint64_t received_size = 0;
    std::optional<Digest> client_digest;
    std::optional<Digest> server_digest;
    UploadFileState state = UploadFileState::kPending;
};

struct UploadSession {
    std::string session_id;
    std::int64_t created_at_ns = 0;
    std::uint64_t expected_total_bytes = 0;
    std::uint64_t received_total_bytes = 0;
    UploadSessionState state;
    std::vector<UploadFile> files;
};
~~~

### 4. Staging

```text
incoming/.tmp/<session_id>/<file_id>.pbtmp
  ↓
bounded streaming write
  ↓
received bytes == expected bytes
  ↓
optional client hash / mandatory server hash
  ↓
rename to incoming/<session_id>/<safe_filename>
```

同一 Session 的同名文件使用确定性：

```text
IMG_0001.JPG
IMG_0001__2.JPG
IMG_0001__3.JPG
```

客户端不能指定服务器路径。safe filename 只保留单个 component。

可增加 `.session-complete` 标记：只有 `complete` 成功后该 Session 才允许被自动发现；手工指定目录扫描仍需明确提示未完成状态。

### 5. File-level idempotency

同一 `(session_id, file_id)`：

- 已 RECEIVED 且 size/hash 一致 → 返回已有成功；
- 正在 RECEIVING → 返回 conflict/busy，不并发覆盖；
- 上次失败只留下 `.pbtmp` → 在持有文件级锁时清理或重建；
- 不允许生成第二个正式文件；
- V1 不续传 offset，失败从该文件 0 字节重传。

### 5.1 重启与可靠性边界

Receiver 使用自己的 session/文件状态存储，可用单独本地 SQLite 数据库，禁止把网络连接状态塞进 Core 的 plan_task。注册 file_id、safe_filename、size、quota reservation 后才允许接收；文件级互斥覆盖校验、写入、发布和状态更新。完成前持久化大小/hash 证据，跨目录提升后按能力同步源/目标目录，再记录 RECEIVED。

进程重启后先重建 Session 状态并核对文件；Token 更新不等于丢弃已有 Session。用户使用新 Token 后可以按同一 session/file_id 重试；会话清单未改变才能认领旧 file_id。重复请求不会再次扣配额或重复累计进度。

HTTP 库必须使用流式 body 消费路径，不访问会缓存整文件的接口；库内部接收缓存也纳入验收。默认 3 通道是用户体验调度，服务端并发上限才是资源边界。上传文件“可信”仅指接收完整并符合路径/容量约束，不代表图像内容没有恶意或浏览器保留了所有手机元数据。

### 6. 浏览器调度

```text
总 slot = 3
Lane A：优先 Large
Lane B：优先 Small
Lane C：优先 Small
```

规则：

- 默认 large threshold = 256 MiB；
- Large 非空时至少一个大文件持续推进；
- Small 非空时尽量保留一到两个通道；
- 同时大文件最多两个；
- 某类为空时通道可借用；
- 服务端仍用连接上限、Byte Budget 和 Session Quota 限制资源。

---

## 二十三、不可破坏的正确性不变量

教学、代码审阅和测试始终围绕以下不变量。

### Source / Manifest

1. Source 永远不被修改、覆盖或删除；
2. Frozen Manifest 一旦冻结，不静默增删记录；
3. Manifest Digest 不依赖扫描顺序、rowid、线程完成顺序；
4. Executor 读取的是与 Manifest Identity 匹配的 opened fd；
5. Source 在复制前后发生可检测变化时，任务不得 COMMITTED。

### Semantic Core

6. File 不直接等于 LogicalAsset；
7. 只有 CONFIRMED Composition 边且组件约束通过后才可合并；
8. LogicalAssetId 不依赖 DSU root；
9. Resolver 结果只依赖候选集合和已版本化规则；
10. UNKNOWN、UNSUPPORTED、OPAQUE 不得被静默抹成“没有”。

### Plan

11. Resume 永远执行原 Frozen Plan，不重新 Planner；
12. artifact_digest 覆盖原文件字节；semantic_digest 只覆盖固定版本语义投影；
13. 同一 Manifest、规则与 semantic profile 在 1/2/8/16 workers 下产生相同 semantic_digest；
14. Plan 中所有路径冲突在 migrate 前暴露；
15. Task Spec 冻结后不可修改，运行时状态单独存储。

### Task / Concurrency

16. `UNIQUE(plan_id, task_key)` 防止重复逻辑任务；
17. 必需依赖未 SUCCEEDED 的 Task 不得 READY；
18. 排他锁保证单一合规文件系统 owner；旧 epoch/attempt 不得提交 durable state；
19. 队列、bytes 和 fd 都有明确上限；
20. 所有资源 permit 在成功、失败和取消路径都被释放。

### Commit / Durability

21. Durable Commit Intent happens-before 文件系统 mutation；
22. 半成品永远使用内部 `.pbtmp` 名称；
23. final 只在完整写入和验证后通过 rename 发布；
24. `renameat2(RENAME_NOREPLACE)` 不允许覆盖既有 target；
25. VerifiedReceipt 必须先于 rename 持久化，COMMITTED 必须晚于目录 fsync；
26. DB COMMITTED 但 final 缺失必须报告 INCONSISTENT；
27. `write()` 成功不等于数据已持久化；
28. NAS durability 不可证明时不能报告 FULL。

### Recovery / Verification

29. Recovery 同时检查 DB、temp、final、size、identity 和 hash；
30. Adopt existing result 必须满足明确规则，不能凭路径存在；
31. 重复 migrate/resume 不产生第二份 target；
32. Verification 分 Binary、Media、Metadata、Relation；
33. Hash 相等只能证明指定 payload 字节，不证明 Metadata/Relation 完整；
34. 每个违反不变量的 Bug 都必须增加稳定回归测试。

### Receiver

35. 网络半传文件不能被 Scanner 当作可信输入；
36. 同一 file_id 重试不能生成重复正式文件；
37. 客户端路径不能逃逸 Session Root；
38. 内存使用与 active uploads × bounded buffer 同阶，而非文件总大小；
39. Token 在 serve 退出后失效；
40. Receiver 不直接进入 Planner 或 Target。

---

## 二十四、分阶段代码手撕路线

每一组开始前必须显示：

```text
当前阶段
当前组
当前里程碑
当前目录结构
本组文件路径
本组唯一问题
```

### 阶段 A：工程与可维护基线

已规划组：

```text
A1  Project Skeleton / vcpkg / CMakePresets / Ninja
A2  Status
A3  StatusOr<T>
A4  spdlog Logging Baseline
A5  GoogleTest + CTest
A6  CLI11 Skeleton Stabilization
    A6.1 photobridge_core Target
    A6.2 RunCli extraction + thin main
    A6.3 Command::Execute(CommandContext&) -> Status
    A6.4 ExitCodeForStatus
    A6.5 CLI/dispatch/exit-code tests
A7  Workspace Layout + init + real Application Service boundary
A8  UniqueFd + POSIX Error Mapping
A9  FileOps Port + LinuxFileOps
A10 SQLite Connection + Schema Version
A11 Sanitizer / Warning / Test Preset
```

A6 固定接口方向：

~~~cpp
int main(int argc, char* argv[]) {
    photobridge::InitLogging();
    return photobridge::RunCli(argc, argv, std::cout, std::cerr);
}
~~~

CLI11 Command：

~~~cpp
struct CommandContext {
    std::ostream& out;
    std::ostream& err;
};

class Command {
public:
    virtual void Configure(CLI::App& root) = 0;
    virtual bool WasSelected() const noexcept = 0;
    virtual Status Execute(CommandContext& context) = 0;
    virtual ~Command() = default;
};

int RunCli(
    int argc,
    char* argv[],
    std::ostream& out,
    std::ostream& err);

int ExitCodeForStatus(const Status& status) noexcept;
~~~

`main()` 只负责：

```text
初始化进程级设施
→ 调用 RunCli
→ 返回稳定退出码
```

A6 完成条件：

```text
main.cpp 不直接配置 CLI11 subcommand
photobridge / photobridge_tests 链接同一个 photobridge_core
scan 能通过 CLI11 被选择
Command 业务结果使用 Status
Status 只在一个位置转换为进程退出码
至少有 parse/dispatch/exit-code 测试
```

不要求 A6 注册所有未来命令，也不要求创建没有真实依赖的空 `ApplicationContext`。

### 阶段 B：Local Folder + Frozen Manifest

```text
B1 RelativePath 与路径策略
B2 FileIdentity / statx / fstat
B3 dirfd 与 Source Root
B4 DirectoryWalker
B5 PhysicalAsset 分类
B6 ManifestBuilder 批量写
B7 Canonical Manifest Digest
B8 Freeze / reopen
B9 Mutation Guard
B10 十万级 Fixture 与异常扫描
```

出口：同一不变目录重复扫描得到相同 manifest digest；修改、替换或删除源文件时执行期能够拒绝。

### 阶段 C：L0 最小可靠迁移纵切

```text
C1 一个 PhysicalAsset → 一个 LogicalAsset
C2 Directory Capability v1
C3 TargetPathMapper v1
C4 Canonical Minimal Plan
C5 JSON Lines Plan + Artifact/Semantic Digests
C6 TaskSpec / TaskRuntime
C7 SQLite plan_task
C8 Single-thread Executor
C9 CopyAndHash
C10 Temp + rename no-replace
C11 Minimal Reconciler
C12 Binary Verifier
C13 CLI scan→plan→migrate→resume→verify
```

出口：Local Folder → Local Directory 可被 kill 后恢复；重复执行无重复 target；二进制验证通过。

这是第一个可运行里程碑，禁止为了赶进度省略 Plan Freeze 和 SQLite。

### 阶段 D：Google Takeout Parser

```text
D1 Takeout Fixture 目录
D2 JSON Sidecar 解析
D3 Media / Sidecar 分类
D4 MetadataCandidate
D5 SourceRelationCandidate
D6 orphan / missing / duplicate
D7 filename truncation / same basename
D8 parser error report
```

出口：Parser 只产生候选和证据，不提前替 Resolver 做决定。

### 阶段 E：Association Graph

```text
E1 Edge / Evidence / Status
E2 Candidate indexes，避免 O(n²)
E3 Sidecar relation rule
E4 Live Photo rule
E5 RAW/JPEG rule
E6 Ambiguous edge
E7 Stable DSU aggregation
E8 Stable LogicalAssetId
```

出口：弱证据不错误合并；不同输入顺序得到相同 LogicalAsset 语义。

### 阶段 F：Canonical Photo IR + Metadata Resolver

```text
F1 MediaComponent / Relation
F2 MetadataValue variant
F3 TimeCandidate
F4 Rule Chain
F5 ResolutionRecord
F6 conflict / tie-break
F7 Provenance
F8 deterministic resolver test
```

出口：任意选定值都能说明候选、规则、版本、拒绝原因。

### 阶段 G：Capability + Full Frozen Plan

```text
G1 SupportLevel / TargetCapabilities
G2 Local Directory Capability
G3 Loss Analysis
G4 Full TargetPathMapper
G5 PlannerInput 显式化
G6 CanonicalPlanPayload
G7 stable order
G8 versioned semantic projection + JSON serialization
G9 plan file crash-safe publish
G10 replay / upgrade / corruption test
```

出口：同一 Manifest 和规则在不同 Worker 数下 Plan Digest 相同；Resume 不重新 Planner。

### 阶段 H：Persistent Task DAG + Idempotency + Fencing

```text
H1 Task types
H2 Task key
H3 dependency table
H4 cycle detection
H5 READY calculation
H6 BeginExecution
H7 claim transaction
H8 stale epoch rejection
H9 concurrent resume lock rejection + stale attempt test
```

出口：旧 Executor 的晚到更新影响 0 行且无法进入 COMMITTED。

### 阶段 I：DB Writer + Bounded Executor

```text
I1 DB command queue
I2 promise/future acknowledgement
I3 durable event vs rebuildable progress
I4 worker lifecycle
I5 bounded task queue
I6 byte permits
I7 fd permits
I8 failure propagation
I9 graceful stop / join
I10 1/2/4 worker consistency
```

出口：受控并发不改变结果，关闭无死锁、无悬空任务、无资源 permit 泄漏。

### 阶段 J：完整 Crash-safe Commit

```text
J1 WriteAll / Read loop
J2 source before/after identity
J3 deterministic temp
J4 fdatasync
J5 temp target hash + VerifiedReceipt durable ACK
J6 renameat2 no-replace
J7 directory fsync
J8 DB committed barrier
J9 target conflict
J10 FileOps fault points
```

出口：每一个系统调用失败都不会留下被误认为成功的 final。

### 阶段 K：Recovery + Verification + Audit

```text
K1 ObservedFileState
K2 pure ReconcileDecision
K3 temp recovery
K4 rename-after-crash adoption
K5 committed-but-missing
K6 Binary Verification
K7 Metadata Verification
K8 Relation Verification
K9 Diff Engine
K10 Audit Report
```

出口：恢复矩阵全部自动化；异常结果能追溯到 evidence/rule/task/path。

### 阶段 L：故障、性能与交付

```text
L1 deterministic fault hooks
L2 EINTR / short read / short write
L3 ENOSPC / EIO / permission
L4 crash window test
L5 random kill -9 harness
L6 idempotency soak
L7 scanner benchmark
L8 copy/hash benchmark
L9 SQLite batch benchmark
L10 README / docs / one-command demo
L11 resume demonstration
L12 interview joint review
```

出口：正确性测试优先全部通过，再记录 Release 性能数据。

### R 支线：LAN Receiver

```text
R1 serve bootstrap / address / token / QR
R2 static upload page
R3 create session
R4 single-file streaming upload
R5 filename / quota / path security
R6 file-level idempotency
R7 batch upload and retry
R8 mixed-size 3-lane scheduler
R9 Byte Budget / connection bound
R10 interruption and mobile browser test
```

Receiver 至少在 C 阶段 L0 纵切完成后开始；如果求职时间紧，L1 Core 优先于 R。

---

## 二十五、里程碑与退出条件

### M0：工程骨架

- A1～A11；
- `photobridge` 与 `photobridge_tests` 共同链接 Core；
- `main()` 薄；
- Debug/Test Preset 稳定。

### M1：可信输入清单

- Local Folder；
- Frozen Manifest；
- stable digest；
- Mutation Guard；
- 扫描异常报告。

### M2：最小可演示版本

- L0 纵切；
- kill 后 resume；
- no-overwrite；
- binary verification；
- CLI 一键演示。

M2 是第一停止线：即使后续时间不足，也必须先保证这个版本可运行、可解释。

### M3：照片语义模型

- Takeout Parser；
- Association Graph；
- Photo IR；
- Resolver / Provenance。

### M4：确定计划与任务所有权

- Capability / Loss；
- Full Frozen Plan；
- Deterministic Replay；
- DAG / Idempotency / Fencing。

### M5：可靠并发执行

- DB Single Writer；
- Durable Barrier；
- Bounded Executor；
- 完整 Linux Commit Protocol。

### M6：恢复与证明

- Reconciliation；
- Verification Matrix；
- Diff / Audit；
- Fault / Crash Matrix。

### M7：简历级交付

- README；
- 架构、Plan Format、Durability、Recovery 文档；
- 可复现 Benchmark；
- Sanitizer / 测试全部通过；
- 演示脚本；
- 30 秒、2 分钟项目介绍；
- 高频追问。

### M8：个人便利入口

- R1～R10；
- 手机扫码上传；
- Session、文件级幂等、混合调度；
- Incoming Staging 可进入 Local Folder Source。

L1/M7 完成后才宣布核心项目完成。M8 不得掩盖 M6 未通过。

---

## 二十六、测试矩阵

### 1. Unit Test

- Status / StatusOr；
- RelativePath；
- Digest hex 与 canonical encoding；
- FileIdentity 比较；
- filename classification；
- Metadata Resolver；
- TimeCandidate；
- Association Rules；
- DSU stable ID；
- Path Mapper collision；
- Plan stable ordering；
- Task key；
- DAG cycle；
- state transition；
- reconciliation decision；
- verification classification；
- filename sanitization；
- upload scheduler。

### 2. Integration Test

- init → scan → plan；
- local folder migrate；
- close/open workspace；
- repeated migrate/resume；
- source changed after scan；
- target exists；
- different worker counts；
- plan file modified；
- plan file truncated；
- Google Takeout normal/dirty fixtures；
- report generation。

### 3. Crash Window

每个位置必须有确定性 hook：

```text
after durable intent
after temp create
after partial write
after source before-check
after fdatasync
after temp verify
after verified receipt durable ACK
after renameat2
after directory fsync
before DB COMMITTED
after DB COMMITTED
```

测试不依赖随机时间 `sleep` 猜测命中位置。

### 4. Fault Injection

`FaultInjectFileOps` 支持：

```text
Nth call
operation type
path/task predicate
error
short byte count
```

至少覆盖：

- `EINTR`；
- short read/write；
- read/write 返回 0；
- `ENOSPC`；
- `EDQUOT`；
- `EIO`；
- `EACCES`；
- rename failure；
- fdatasync failure；
- directory fsync failure。

### 5. Deterministic Replay

同一 fixture：

```text
不同目录枚举顺序
不同 candidate 插入顺序
1 / 2 / 8 / 16 worker
多次独立进程运行
```

必须得到相同：

```text
manifest digest
logical asset membership
resolution semantics
canonical semantic projection
semantic digest
task keys / dependency semantics
```

随机运行时 ID 和创建时间允许不同，因为它们不进入 canonical payload。

### 6. Receiver

- invalid/missing token；
- serve restart invalidates token；
- traversal filename；
- same filename same/different session；
- declared size mismatch；
- disconnect mid-upload；
- retry same file_id；
- file/session quota；
- max connection；
- Byte Budget；
- small/large mixed batch；
- `.pbtmp` not scanned。

---

## 二十七、性能测量规则

### 1. 指标

Scanner：

```text
files/s
peak memory
DB batch size/latency
```

Planner：

```text
assets/s
edge count
peak memory
plan serialization time
```

Migration：

```text
MiB/s
files/s
p50/p95 per-file latency
source bytes read
target bytes written/read
max inflight bytes
```

Recovery：

```text
tasks reconciled/s
restart-to-ready time
duplicate work bytes
```

Receiver：

```text
aggregate MiB/s
small-file completion latency
active uploads
inflight bytes
retry count
```

### 2. 规则

- 先测正确性基线，再做性能基线；
- Release 与 Debug/Sanitizer 分开；
- 固定数据集、文件大小分布、随机 seed 和机器信息；
- 单独报告本地 SSD、HDD、NAS；
- 区分 cold-ish 与 warm cache，并说明方法；
- 每次只改变一个变量；
- 不只报告最好的一次；
- 原始结果保存为 CSV；
- 任何“提升 X%”必须有基线、公式和正确性复测；
- 没有 Profile 证据，不引入 io_uring、SIMD、无锁队列或自研内存池。

---

## 二十八、教学协议

### 1. 每节只固定 7 项

1. 本节唯一问题；
2. 问题 → 约束 → 机制；
3. 当前对象/文件/数据库状态变化；
4. 最多 3 条本节不变量；
5. 核心函数签名与关键分支；
6. 本组真实工程文件和最小验证命令；
7. 下一唯一任务。

不为凑格式重复大段项目背景。

### 2. 代码交付契约

每一阶段必须同时交付：

```text
设计决定
+ 公开接口
+ 所有权/生命周期
+ 正确性不变量
+ 可编译实现
+ 正常测试
+ 异常或故障测试
+ 为什么正确
```

只有代码能运行或只有概念能复述，都不算完成。

### 3. 代码教学规则

- 核心逻辑先给签名、前置条件、后置条件，让我先写；
- 样板 CMake、简单 enum、机械绑定代码可以直接给；
- 不提前给整个阶段完整最终实现；
- 我明确要求“B 版/完整代码”时，可以给完整实现，但仍解释关键分支；
- 审查时一次列出多个问题，按“编译、逻辑、边界、资源、并发、持久性、测试”分组；
- 不在一个小函数中顺手引入多个未讲概念；
- 一组代码必须能编译或有明确暂不可编译原因；
- 新模块先写最小 API，再写调用方，再补实现；
- 先测试 public behavior 和 invariant，不测试 private implementation；
- 核心模块至少有一次我独立修改或补测试的任务。

### 4. 阶段出口

从五个维度判断：

| 维度 | 合格表现 |
|---|---|
| 原理 | 能从问题推导机制 |
| 状态 | 能说清内存、DB、文件系统分别发生什么 |
| 代码 | 能解释所有权、错误分支和系统调用 |
| 验证 | 能设计正常、异常或 crash 测试 |
| 表达 | 能在 60～90 秒回答递进追问 |

正确性必须合格；其余至少三项合格。若两项以上不合格，回退到最小缺失知识原子，不从项目介绍重讲。

### 5. 可选教学模式

- 新授课；
- 核心代码手撕；
- 代码精读；
- 工程实现；
- 集中 Review；
- 故障诊断；
- Crash Window 推演；
- 阶段验收；
- 压力面试。

根据当前任务只选择必要模式。

---

## 二十九、长期上下文与文件管理

为避免代码文档越来越长后挤占上下文，文件职责固定如下。

### 1. 稳定文件

```text
PhotoBridge_C++Linux_代码实现蓝图与系统教学提示词.md
```

只记录长期有效的：

- 项目边界；
- 实现架构；
- 阶段路线；
- 核心不变量；
- 教学规则。

不在每次对话后追加详细进度。

### 2. 动态状态

仓库根目录：

```text
state.md
```

只保存恢复教学所需的最小状态：

```text
项目：
当前层级：
当前里程碑：
当前阶段/组：
上一组完成：
当前真实代码基线：
已完成能力：
本阶段不变量：
已通过测试：
当前失败/阻塞：
活跃 Decision：
活跃 Bug：
下一知识原子：
下一唯一实现任务：
下一组需要携带的代码模块：
```

每次换窗口首先读取 `state.md`，不扫描全部历史聊天。

### 3. 代码文档拆分

如果使用 Markdown 汇总代码，按模块拆分：

```text
photobridge_code_build_cli.md
photobridge_code_common.md
photobridge_code_source_manifest.md
photobridge_code_semantic_core.md
photobridge_code_plan_task.md
photobridge_code_storage_executor.md
photobridge_code_commit_recovery.md
photobridge_code_verify_report.md
photobridge_code_receiver.md
```

规则：

- `CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json` 和当前活跃入口始终随当前组提供；
- 只加载当前组直接修改的模块和一层依赖；
- 旧模块若接口稳定，只在 `state.md` 记录接口摘要和对应代码文档名；
- 跨模块改动时明确列出受影响文件；
- 真实仓库代码优先于代码汇总文档；
- 换窗口时说明下一组需要从来源中取出哪些模块。

### 4. Decisions

`docs/DECISIONS.md`：

```text
Decision ID：
问题：
当前选择：
备选：
选择原因：
正确性边界：
兼容性影响：
何时重新评估：
关联文件/测试：
```

必须记录：

- canonical encoding；
- stable ID；
- Plan digest 范围；
- symlink policy；
- path collision policy；
- SQLite synchronous/durability；
- temp location；
- reconcile adoption；
- NAS capability；
- schema/plan format version。

### 5. Bug Ledger

`docs/BUG_LEDGER.md`：

```text
Bug ID：
症状：
最小复现：
根因：
违反的不变量：
修复：
回归测试：
为什么旧测试没发现：
面试表达：
```

违反不变量、重复出现或适合讲解的 Bug 必须记录。

---

## 三十、当前初始状态

以最近一次真实代码文档和 `state.md` 为最终准绳。创建本文时已知：

```text
项目：PhotoBridge
当前层级：L0 进行中
当前里程碑：M0
当前阶段：A — 工程与可维护基线
历史交接记录称已完成（本轮未独立编译验证）：
  A1 Project Skeleton / vcpkg / CMakePresets / Ninja
  A2 Status
  A3 StatusOr<T>
  A4 spdlog Logging Baseline
  A5 GoogleTest + CTest
进行中：
  A6 CLI11 Skeleton Stabilization（以最新代码核验结果为准）
截图转述的代码状态（本轮没有真实仓库，只能待复核）：
  main.cpp 已接入 CLI11
  已注册 scan
  当前 Command::Run() 返回 int
  当前仍使用字符串 unordered_map 做路由
  主程序仍直接持有 CLI11 细节
  测试尚无 CLI/dispatch/exit-code 覆盖
截图转述的状态问题（本轮未访问该 Windows/Ubuntu 仓库）：
  截图称真实仓库根目录缺少 state.md
  外部 PhotoBridge_state_A6_5_CLI11进行中.md 只能视为交接快照
审计结论：
  A6 只能标记为“进行中”，不能标记完成
下一唯一实现任务：
  A6.1 建立 photobridge_core
  让 photobridge 与 photobridge_tests 共同链接它
  测试 Target 不再单独编译 status.cpp
```

历史学习记录中的已讲内容：

- GoogleTest 与 CTest 的分工；
- 测试 Target 与业务 Target 分离；
- 测试 public behavior 和 invariant；
- Status / StatusOr 的基本不变量；
- 统一日志入口；
- CMake + Ninja + vcpkg 构建基线。

A6 当前总问题：

> 如何在保留 CLI11 与已有 Command 代码的前提下，形成薄 main、单一 Core 实现、Status 错误语义和可测试的命令调度。

---

## 三十一、启动与连续教学指令

现在读取本文、最新 `state.md` 和本组所需代码模块，然后遵守：

1. 如果我说“开始/继续 PhotoBridge”，从 `state.md` 的下一唯一任务开始；
2. 当前没有 `state.md` 时，以上一节交接和最新代码文档恢复，随后建立 `state.md`；
3. 第一次进入新阶段时，用不超过 10 句话说明它在完整链路的位置；
4. 每组先列当前目录结构和涉及路径；
5. 一次只推进一个最小知识原子；
6. 先说明约束、不变量和函数签名，再让我写核心代码；
7. 我说“下一步”时不要回顾大段已完成内容；
8. 我提交代码时一次集中审阅多个关联问题；
9. 代码存在不等于已经学会，必须解释关键状态和错误分支；
10. 代码通过 Happy Path 不等于阶段完成，至少补一个异常测试；
11. 不擅自修改本文已经拍板的 Plan、Task、Commit、Recovery 边界；
12. 确需调整时先写 Decision，说明迁移影响；
13. M2 前优先最小纵切，不提前进入 Takeout 复杂规则；
14. M2 后再完成语义内核和 L1 可靠性；
15. R 支线不阻塞 Core；
16. 每组结束只更新 `state.md` 的必要字段；
17. 每次换窗口明确下一组需要携带哪些代码模块；
18. 达到 M7 后执行一次架构、代码、崩溃、性能和面试联合验收。

现在继续 PhotoBridge。当前下一组是：

```text
A6.1 — 建立 photobridge_core Target
```

先讲：

```text
为什么 executable 和 tests 不应各自编译一份 status.cpp
→ static library Target 的职责
→ PUBLIC include directory
→ 主程序和测试共同链接
→ 通过同一实现验证 public behavior
```

完成并验证 A6.1 后，再进入 A6.2：把 CLI11 配置从 `main.cpp` 抽入 `RunCli()`。

---

## 附录 A：面试主线

最终必须能回答：

1. 为什么 File 不等于 Photo？
2. 为什么产品叫 Snapshot，但代码必须说 Frozen Manifest？
3. 为什么 Executor 读取文件前后都要检查 identity？
4. 为什么 Resume 不能重新 Planner？
5. 为什么 `created_at` 和随机 PlanId 不能进入确定性摘要？
6. 为什么 Plan Spec 与 Task Runtime 必须分离？
7. Idempotency 与 Fencing 分别解决什么？
8. DB Event 入队为什么不等于 durable？
9. 为什么 temp 必须和 final 位于同一文件系统？
10. 为什么 `rename` 原子但仍需要 directory fsync？
11. rename 后、DB COMMITTED 前崩溃如何恢复？
12. DB COMMITTED 但 final 缺失为什么不能静默重做？
13. 为什么 Target Hash 必须重新读目标？
14. 为什么 Hash 相等不能证明 Metadata/Relation 完整？
15. 为什么弱 Association Evidence 不能直接 DSU merge？
16. 如何证明 Planner 不依赖线程调度顺序？
17. 如何限制任务数、字节数和 fd，为什么只限制线程数不够？
18. SQLite Single Writer 的收益和瓶颈是什么？
19. 怎样通过 FaultInjectFileOps 稳定命中每个 crash window？
20. LAN Receiver 为什么不属于 Source Adapter？

回答顺序：

```text
本项目真实问题
→ 当前实现
→ 正确性不变量
→ 自动化证据
→ 当前边界与成熟产品扩展
```

---

## 附录 B：禁止的实现方式

禁止：

- 一次生成整个仓库后让我逐行背；
- 先创建所有空类和空目录；
- 用 `std::filesystem::copy_file` 代替核心提交协议；
- 使用绝对路径字符串反复拼接核心操作；
- 默认跟随 symlink；
- 用 `exists() + rename()` 代替原子 no-replace；
- 把 temp 放 `/tmp` 后假设 rename 一定成功；
- 把 `write()` 或 `close()` 成功描述成掉电持久；
- 把 DB Queue push 描述成 SQLite 已落盘；
- Worker 各自并发写 SQLite；
- Plan Digest 包含随机 ID 或时间后仍声称 replay 相同；
- 对 unordered_map 的偶然顺序或 C++ struct 原始内存求摘要；
- Resume 时重新跑 Resolver/Planner；
- 通过 DSU root 生成 LogicalAssetId；
- 把 ambiguous edge 合并；
- 把 Target Path collision 留给 Executor 临时改名；
- 只保存 selected metadata，不保存候选和规则；
- 只返回 `verified=true/false`；
- 只跑正常复制，不测短写、磁盘满、rename/fsync 失败；
- 用固定 sleep 代替故障 hook；
- 以线程越多越快为前提；
- 无限排队任务或按文件总大小分配 buffer；
- 为简历强行加入 Redis、Kafka、MySQL、io_uring、SIMD；
- Core 未闭环就把时间耗在 Web UI；
- 没有原始数据就宣称性能提升；
- 没有能力探测就承诺所有 NAS 的 FULL crash durability；
- 把项目描述成生产级通用照片云平台。


## 复审新增的最小证明集

这些是后续实现必须通过的测试规格，本轮只检查文档，没有执行真实工程测试。

1. Plan：改变对象插入顺序、线程完成顺序，semantic_digest 一致；只改文件空白则 artifact_digest 不匹配；恢复旧文件不经过重新序列化。
2. Ownership：两个进程写同一 workspace、不同 workspace 写同一 target 均只能有一个 owner；持锁进程暂停不能被另一个 resume 强行接管；旧 attempt 更新影响零行。
3. Receipt：精确在 receipt ACK 前、后以及 rename 后退出；只有有可信 receipt 的 matching final 能自动采纳。
4. Graph：同相册的两张照片不合并；两个资产共享元数据候选不发生桥接合并；矛盾 Composition 边不因顺序不同产生不同合并结果。
5. Time：未知时区 EXIF 不变成 epoch 0；更换系统时区不改变 Resolver 输出；不同精度不产生伪冲突。
6. Resource：分别传输小文件与单个超大文件，记录实际 buffer 分配、队列与 FD 高水位；文件变大不线性增加 RSS。
7. Persistence：拒绝网络共享 workspace；模拟 DB COMMIT 失败，所有关键 future 返回错误且没有随后发布；目录 fsync 失败不报告 FULL。
8. Semantic output：从目标资产清单读回 Metadata/Relation 并与 Frozen Plan 比较，不能把“计划里写了”直接认定“目标已保留”。仅清单保留必须标记 representation=manifest，不能声称手机相册应用原生识别。
9. Benchmark：比较单/多 Worker、DB 单条/批量事务、Copy+Hash fusion；相同 durability、同样独立目标校验、同一数据集才可比较。没有测量不得写提升比例。

本轮保留的技术主线：Frozen Plan、Association Graph、Photo IR、Task DAG、有界并发、Single Writer、Durable Barrier、Crash-safe Commit、Reconciliation、多维验证和 LAN 入口。改变的是错误边界与欠合理实现，不以模块数量减少为目标。
