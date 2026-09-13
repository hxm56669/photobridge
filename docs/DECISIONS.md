# PhotoBridge 决策记录

## Decision ID：C1-001

- 问题：L0 中如何把 `PhysicalAsset` 稳定映射为 `LogicalAsset`。
- 当前选择：`PhysicalAssetId` 使用 `path-` 加相对路径原始字节的小写十六进制编码；单成员 `LogicalAsset` 的成员列表规范化为按 ID 升序排列的无空格 JSON 数组，并在 `PB_LOGICAL_ASSET_V1` 域下计算 BLAKE3，结果使用 `logical-` 加小写十六进制摘要。
- 备选：使用扫描顺序、文件身份或数据库 rowid；使用 DSU root；直接使用路径作为 `LogicalAssetId`。
- 选择原因：路径字节已是当前 Manifest 的稳定物理成员身份；逻辑 ID 只由成员集合决定，避免文件元数据变化、扫描顺序和数据库实现细节改变逻辑身份；域和版本前缀为后续规则演进保留边界。
- 正确性边界：当前 `PhysicalAssetId` 在 source manifest 内按相对路径区分；成员不能为空或重复；文件 identity、kind 和 extension 不参与 ID，因此源文件变化由 Mutation Guard 处理，而不是生成新逻辑资产。
- 兼容性影响：Manifest 写入复用同一个 `PhysicalAssetIdFor()`，后续关联图可复用 `LogicalAssetIdForMembers()`；变更域、规范数组编码或成员 ID 规则必须升级版本并重新评估历史计划兼容性。
- 何时重新评估：引入跨 source manifest 的全局资产身份、sidecar 关联或多成员 LogicalAsset 时。
- 关联文件/测试：`include/photobridge/model/physical_asset.h`、`src/model/physical_asset.cpp`、`include/photobridge/model/logical_asset.h`、`src/model/logical_asset.cpp`、`tests/logical_asset_test.cpp`、`src/app/manifest_builder.cpp`。

## Decision ID：C2-001

- 问题：L0 本地目录目标需要声明哪些能力，哪些语义不能未经探测承诺。
- 当前选择：用版本化 `TargetCapabilities` 快照表达 regular file、嵌套目录、字节精确内容、原子不覆盖发布和文件/目录同步；symlink 目标语义明确为 UNSUPPORTED；大小写折叠和 Unicode normalization 标记为 UNVERIFIABLE。
- 备选：用一个 `supports_directory` 布尔值；把大小写和 Unicode 规则硬编码为 Linux 普遍能力；把冲突处理推迟到 Executor。
- 选择原因：能力不是单一布尔值；计划阶段必须区分可保留、不可支持和需探测语义；Executor 不应临时修正计划外路径冲突。
- 正确性边界：`LocalDirectoryCapabilitiesV1()` 是纯函数，不探测挂载点；实际文件系统的 case-fold、normalization 和长度限制由后续目标探测/PathMapper 处理。
- 兼容性影响：能力版本进入后续 Plan 输入；能力枚举或语义改变时升级版本并重新生成计划。
- 何时重新评估：实现 C3 PathMapper、加入实际挂载点探测或支持 NAS 时。
- 关联文件/测试：`include/photobridge/model/capability.h`、`src/model/capability.cpp`、`tests/capability_test.cpp`。

## Decision ID：C3-001

- 问题：L0 如何把逻辑资产映射为目标相对路径并在执行前处理冲突。
- 当前选择：`LogicalAsset` 暂时携带可选源相对路径；`TargetPathMapper` 保留层级或按显式 policy 扁平化，执行 component/path 字节长度校验，并在 `MapAll` 中按逻辑资产 ID 稳定排序后拒绝精确路径和 ASCII 大小写折叠冲突。
- 备选：在 Executor 中遇到冲突时追加 `__2`；直接拼接用户字符串；按输入/扫描顺序决定目标名；未经探测假定 Unicode normalization。
- 选择原因：目标路径是 Frozen Plan 的输入，冲突必须在计划阶段可审计、可复现；`RelativePath` 保持结构安全；Executor 不应改变已冻结的语义。
- 正确性边界：当前 L0 Linux 目标允许原始非 NUL 路径字节，但 capability 未确认 Unicode normalization 时保守拒绝非 ASCII 目标路径；大小写冲突检测覆盖 ASCII，实际挂载点探测留给后续能力扩展。
- 兼容性影响：`MigrationPolicy` 和 capability version 必须进入后续 Plan 语义摘要；修改 prefix、扁平化或长度限制会生成不同目标路径，不能静默复用旧计划。
- 何时重新评估：实现真实挂载点探测、Unicode normalization 处理、完整媒体组件或 NAS 目标时。
- 关联文件/测试：`include/photobridge/model/path_mapper.h`、`src/model/path_mapper.cpp`、`tests/path_mapper_test.cpp`、`include/photobridge/model/logical_asset.h`。

## Decision ID：C4-001

- 问题：L0 计划如何冻结输入并保证语义投影不受插入顺序影响。
- 当前选择：构造 `CanonicalMinimalPlan` 时校验 source manifest/digest、target root、完整 capability 快照、policy 和每个资产的源/目标路径与 `FileIdentity`；资产按 `logical_asset_id` 排序；canonical payload 使用固定域、字段顺序和长度前缀。
- 备选：保存可变 planner 对象；按扫描顺序写计划；只保存 target path；在 C5 直接让业务代码操作 JSON DOM。
- 选择原因：计划必须是执行期不可变的语义输入；显式绑定避免换源、换目标或换能力后继续执行旧计划；长度前缀避免字符串边界歧义，后续 JSONL 只作为存储适配层。
- 正确性边界：C4 只负责 typed plan 和 semantic projection，不负责 JSONL 文件发布、artifact digest 或 SQLite task runtime；当前 L0 每个计划资产冻结一个源物理资产。
- 兼容性影响：canonical domain/version、字段顺序和 capability/policy 字段变化必须升级 plan semantic profile；运行时状态不能写入 canonical payload。
- 何时重新评估：实现 C5 JSON Lines、完整多成员 LogicalAsset 或需要对外兼容旧计划时。
- 关联文件/测试：`include/photobridge/model/canonical_plan.h`、`src/model/canonical_plan.cpp`、`tests/canonical_plan_test.cpp`。

## Decision ID：C5-001

- 问题：如何把冻结的 typed plan 持久化为可审计、可校验的计划文件。
- 当前选择：使用固定分区顺序的 JSONL artifact；原始路径字节使用 base64，`artifact_digest` 覆盖最终发布字节，`semantic_digest` 使用 C4 canonical payload 且排除随机 `plan_id`。
- 备选：直接序列化 JSON DOM；把路径按本地字符串编码；只保存一个摘要；把随机计划 ID 混入语义摘要。
- 选择原因：JSONL 便于流式生成和逐行审计，base64 保留非 UTF-8 路径，两个摘要分别表达字节完整性和计划语义身份。
- 正确性边界：C5 只负责 artifact 的内存读写与 digest 校验，不负责 SQLite runtime、任务调度或文件发布。
- 兼容性影响：artifact schema、section 顺序、base64 表示和摘要域变化必须升级格式版本；旧 artifact 不应静默按新规则解释。
- 何时重新评估：需要外部兼容的计划交换格式、签名封装或更大规模流式解析时。
- 关联文件/测试：`include/photobridge/model/plan_artifact.h`、`src/model/plan_artifact.cpp`、`tests/plan_artifact_test.cpp`。

## Decision ID：C6-001

- 问题：如何分离计划中的任务语义与执行期状态，并保证状态转换集中且可恢复。
- 当前选择：`TaskSpec` 保存不可变任务输入；`TaskRuntime` 保存 SQLite 承担的可变状态；task key 由 `PB_TASK_V1`、逻辑资产、任务类型、规范目标路径和可选源组件身份计算，排除运行时字段、估算大小和 expected digest。
- 备选：用运行时 attempt 或数据库 rowid 生成任务身份；让各 Worker 自由修改状态；把文件提交阶段混入 DAG `TaskState`。
- 选择原因：重启后可由稳定 task key 重建任务，调度状态与文件提交状态各自表达清晰语义；集中状态机避免非法跳转和重复提交。
- 正确性边界：C6 只定义类型、校验、task key 和状态转换；DAG 存储、SQLite 单写者和实际执行器留给后续阶段。
- 兼容性影响：task key 域、任务类型枚举和状态机是持久化协议的一部分，改变它们必须升级版本并处理已有计划/runtime。
- 何时重新评估：实现 TaskGraph、SQLite runtime 表、恢复扫描和实际文件提交协议时。
- 关联文件/测试：`include/photobridge/model/task.h`、`src/model/task.cpp`、`tests/task_test.cpp`。

## Decision ID：C7-001

- 问题：如何在 Planner Freeze 前验证任务 DAG，并保证 artifact 中的顺序可复现。
- 当前选择：`TaskGraph` 以 task id 保存节点、以有向依赖边保存前置关系；添加时拒绝未知节点、自环、重复节点、重复 task key 和重复边；冻结校验使用确定性的 Kahn 算法，任务与依赖分别按端点排序输出。
- 备选：按插入顺序运行；让 Worker 发现缺失依赖；递归 DFS 但不固定遍历顺序；把环检测推迟到执行期。
- 选择原因：缺依赖和环都是计划错误，必须在执行前失败；稳定排序使 JSONL artifact、日志和恢复重建具有可比较结果。
- 正确性边界：C7 只负责内存中的节点/依赖图和初始 Ready 集合；完成计数、反向索引持久化和成功后的下游解锁由后续 runtime 层实现。
- 兼容性影响：task id、依赖端点和排序规则属于计划 artifact 的稳定表示；改变它们必须重新生成计划并重新校验 semantic digest。
- 何时重新评估：实现 SQLite `plan_task` / `task_dependency` 表、持久 Ready queue 和跨进程恢复时。
- 关联文件/测试：`include/photobridge/model/task_graph.h`、`src/model/task_graph.cpp`、`tests/task_graph_test.cpp`。

## Decision ID：C8-001

- 问题：如何把冻结计划、任务 runtime、依赖关系和文件尝试持久化，并支持 schema 演进。
- 当前选择：将 C8 作为 SQLite schema v2→v3 迁移；新增 `migration`、`migration_plan`、`plan_task`、`task_dependency`、`task_attempt`，其中计划内任务键唯一，依赖和尝试通过复合外键绑定到同一 plan/task。
- 备选：把 runtime 写回 JSONL plan；用数据库 rowid 表示依赖；不声明外键而由 Worker 自行校验；直接修改 v2 表结构。
- 选择原因：计划文件保持不可变，SQLite 负责可变执行状态；复合主键和外键防止跨计划依赖、孤立尝试和重复 task key；版本迁移保留已有 manifest 数据。
- 正确性边界：C8 只建立 schema 与版本迁移，不负责 runtime repository、单写者事务封装、Ready 计数更新或恢复算法。
- 兼容性影响：schema version 3、列含义、枚举整数和复合约束属于持久化协议；变更必须追加迁移并拒绝未知更高版本。
- 何时重新评估：实现 task repository、epoch lease、attempt 提交事务和崩溃恢复扫描时。
- 关联文件/测试：`include/photobridge/app/sqlite_schema.h`、`src/app/sqlite_schema.cpp`、`tests/sqlite_schema_test.cpp`。

## Decision ID：C9-001

- 问题：如何让任务 runtime 的关键状态更新可恢复，并拒绝过期 Worker 的回调。
- 当前选择：`TaskRuntimeRepository` 使用 prepared statements；Ready 晋级检查同一 plan 的依赖是否全部 SUCCEEDED；claim、成功和可重试更新使用 `BEGIN IMMEDIATE`，并同时匹配 plan/task、旧状态、epoch 与 attempt。
- 备选：把状态留在 Worker 内存；按 task id 直接覆盖状态；先返回 ACK 再提交；允许旧 epoch 完成任务。
- 选择原因：SQLite 是 runtime 的持久真相，事务提交后才视为成功；epoch/attempt 条件更新可阻断过期执行者和重复回调；稳定 task-id 排序使 claim 可复现。
- 正确性边界：C9 暂不实现 DB Writer 线程队列、文件锁、VerifiedReceipt 和文件提交协议；当前 repository 只覆盖任务状态与依赖就绪判断。
- 兼容性影响：task state 整数映射、`plan_task` 字段和条件更新语义属于 runtime 协议；修改必须同步 schema/recovery 规则。
- 何时重新评估：实现 CopyAndHash、commit intent、VerifiedReceipt、单写者队列和进程崩溃恢复时。
- 关联文件/测试：`include/photobridge/app/task_runtime_repository.h`、`src/app/task_runtime_repository.cpp`、`tests/task_runtime_repository_test.cpp`。

## Decision ID：C10-001

- 问题：如何复制源文件并计算摘要，同时覆盖短读、短写和源文件变更。
- 当前选择：`CopyAndHash` 通过 `FileOps` 逐轮执行 read、hasher update、`WriteAll` 和字节计数；复制前后读取源 fd 身份并拒绝变化；写入返回零字节视为无进展 I/O 错误；BLAKE3 通过可注入 `Hasher` 接口实现。
- 备选：一次性读入内存；假设 read/write 总是完成；复用目标 hash 作为源 hash；只检查路径而不检查已打开 fd 身份。
- 选择原因：流式复制的内存上界与文件大小无关，短传输和 `EINTR` 由 FileOps 端口统一处理，身份前后检查阻止复制过程中源内容漂移。
- 正确性边界：C10 只产生源复制摘要和身份结果，不声称目标已持久化；目标独立读回、fdatasync、VerifiedReceipt 和 rename 留给后续提交协议。
- 兼容性影响：`CopyResult` 字段语义和 Hasher finalize 规则属于执行层接口；修改必须同步提交/恢复协议。
- 何时重新评估：引入 sparse file、稀疏复制、校验和硬件加速或多 Worker I/O 时。
- 关联文件/测试：`include/photobridge/filesystem/copy_and_hash.h`、`src/filesystem/copy_and_hash.cpp`、`tests/copy_and_hash_test.cpp`。

## Decision ID：C11-001

- 问题：如何把复制结果安全发布到目标目录，同时保证崩溃后仍保留可恢复证据。
- 当前选择：在目标父目录中用 `CreateTempNoReplace` 创建独占 temp；复制成功后对 temp `fdatasync`，再用 `RenameNoReplace` 发布，最后同步目标目录；rename 失败不删除 temp，复制或同步失败只清理本次明确创建的 temp。
- 备选：直接写最终路径；rename 前覆盖旧目标；rename 失败后按扩展名批量清理；只同步文件不​​同步目录项。
- 选择原因：最终路径永远不被部分写入或覆盖，no-replace 冲突交给后续 reconciler；保留 rename 不确定性下的 temp 可支持恢复判断，目录同步覆盖新目录项持久化边界。
- 正确性边界：C11 不负责 VerifiedReceipt、目标独立读回 hash、数据库提交 intent 或恢复决策；本阶段只负责 temp→publish 的文件系统顺序。
- 兼容性影响：temp 命名归属、发布顺序和 no-replace 冲突语义是恢复协议输入；改变后必须同步 cleanup/reconciler 规则。
- 何时重新评估：实现 VerifiedReceipt、崩溃恢复、目标冲突采纳和多层目录持久化记录时。
- 关联文件/测试：`include/photobridge/filesystem/temp_commit.h`、`src/filesystem/temp_commit.cpp`、`tests/temp_commit_test.cpp`。

## Decision ID：C12-001

- 问题：进程崩溃后如何仅凭持久证据和当前观察结果决定重做、继续提交、采纳或人工检查。
- 当前选择：`Decide()` 是无副作用纯函数；无 receipt 时不自动采纳 final；有 receipt 时要求独立 digest/size 与 receipt 匹配，分别区分 temp 继续、final 采纳、清理 temp、目标冲突和不一致；已成功任务只允许 reverify 或标记不一致。
- 备选：按文件名采纳 final；只看存在性和大小；让 reconciler 直接删除/rename；把旧 attempt 当作当前 owner 继续提交。
- 选择原因：存在性不能证明归属，receipt 才是本次执行的验证证据；纯规则输出便于测试和审计，实际动作可在重新持锁并取得新 epoch 后执行。
- 正确性边界：C12 不操作文件、不持锁、不写数据库；源当前不可用时只能依据 receipt 校验已有 final，不能声称源仍满足 Manifest。
- 兼容性影响：receipt 绑定字段、观察字段和决策枚举是恢复协议的一部分；改变必须同步 runtime schema 与恢复测试。
- 何时重新评估：实现独立目标读回 verifier、VerifiedReceipt 持久化、commit intent 表和 kill-9 恢复流程时。
- 关联文件/测试：`include/photobridge/filesystem/reconciler.h`、`src/filesystem/reconciler.cpp`、`tests/reconciler_test.cpp`。

## Decision ID：C13-001

- 问题：如何证明目标文件内容，而不把源复制流的摘要误当成目标落盘结果。
- 当前选择：`VerifyBinary` 对目标 fd 独立执行 read→hash，记录实际字节数并在读前后校验目标身份；有冻结期望摘要时返回 `IDENTICAL` 或 `MISMATCH`，没有期望摘要时返回 `NOT_APPLICABLE`。
- 备选：复用 `CopyAndHash` 的 source digest；只比较文件大小；验证期间按路径重新打开而不绑定 fd；把 mismatch 当作 I/O 异常。
- 选择原因：独立读回覆盖目标实际内容，大小和强摘要各自参与判断；`NOT_APPLICABLE` 与真正 mismatch 分开，报告不会把缺少验证输入伪装成成功或失败。
- 正确性边界：C13 不发布文件、不持久化 receipt、不执行媒体/元数据/关系验证；它只验证一个已打开目标 fd 的二进制内容。
- 兼容性影响：二进制验证枚举和摘要/大小比较规则属于报告与恢复协议；变更必须同步 verifier 和 audit 输出。
- 何时重新评估：支持稀疏文件、分块摘要、目标读回限速和多种二进制摘要算法时。
- 关联文件/测试：`include/photobridge/filesystem/binary_verifier.h`、`src/filesystem/binary_verifier.cpp`、`tests/binary_verifier_test.cpp`。

## Decision ID：C14-001

- 问题：如何把 L0 的 scan、plan、migrate、resume、verify 暴露为稳定 CLI，而不把尚未实现的业务阶段伪装成成功迁移。
- 当前选择：用统一 `PipelineCommand` 承载五个阶段；CLI11 负责阶段选择和必需参数校验，命令只校验工作区路径/阶段输入并输出明确的阶段边界，业务执行仍由后续 Application Service 接入。
- 备选：每个阶段各自复制一套命令；在 CLI 中直接调用文件系统和 SQLite；返回裸进程整数；让占位命令输出“迁移完成”。
- 选择原因：统一命令契约减少路由漂移，`Status` 保持领域错误到退出码的单一转换点；显式边界让当前能力可测试，也不会把未完成的扫描、复制和验证误报为成功。
- 正确性边界：C14 只保证五阶段的 CLI11 解析、参数约束、输出注入和 `Status` 路由；它不声称已经生成 Manifest/Plan、执行文件提交、恢复任务或验证目标内容。
- 兼容性影响：阶段名称、选项名称和输出格式属于 CLI 协议；后续接入真实服务应保留这些入口，并仅替换 `PipelineCommand::Execute()` 的应用调用。
- 何时重新评估：D1 接入 Local Folder Scanner 后，将 `scan` 从边界确认改为真实 Manifest 生成，并补充跨阶段集成测试。
- 关联文件/测试：`include/photobridge/cli/pipeline_command.h`、`src/cli/pipeline_command.cpp`、`src/cli/cli_app.cpp`、`tests/cli_test.cpp`。

## Decision ID：D1-001

- 问题：已有代码提前实现了多个后续阶段，继续横向扩展会让最小纵向闭环变重、难以验证。
- 当前选择：从当前状态开始严格按蓝图的最小纵切推进；已有类型只作为最终模型的合法子集保留；每次只完成 state 中的一个唯一任务；不为未来阶段新增空壳抽象。
- 备选：一次性重写全部分层；删除所有后续阶段代码；继续先补齐 Planner/Executor/网络等横向模块。
- 选择原因：蓝图要求先验证 Local Folder → Frozen Manifest → Plan → Task → Copy → Resume → Verify；过早横向实现不能替代闭环验证。
- 正确性边界：本决策不自动删除已有代码，也不声称 C2～C14 已完成 L0 闭环；每个阶段仍需真实代码、测试和状态记录证明。
- 兼容性影响：后续新增接口必须服务于当前纵切，并保持 `RelativePath`、`FileIdentity`、`PhysicalAsset`、Manifest、Plan、Task 和 FileOps 契约兼容。
- 何时重新评估：D1 scan、plan、migrate、resume、verify 最小闭环完成并通过可重复测试后。
- 关联文件/测试：`docs/state.md`、`src/cli/pipeline_command.cpp`、`src/filesystem/local_folder_source.cpp`、`tests/local_folder_source_test.cpp`。
