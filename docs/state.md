# PhotoBridge 教学状态

项目：PhotoBridge
当前层级：R 支线进行中
当前里程碑：LAN Receiver
当前阶段/组：R10 — interruption / restart / mobile browser regression

## 收敛规则

- 先完成 `Local Folder → Manifest → LogicalAsset → Plan → Task → Copy → Resume → Verify` 最小闭环。
- 每次只推进本文件记录的一个唯一任务；完成后先更新本文件，再进入下一任务。
- 已有 C2～C14 类型和测试作为最终模型的合法子集保留；不继续横向创建未来阶段空壳，不为“完整架构”增加无实际调用的抽象。
- 不绕过 Plan、SQLite、临时文件协议或独立验证；未接入 CLI 的代码不算阶段完成。
- L1 仍按蓝图的 D→E→F→G→H→I/J→K→L 顺序推进；每个里程碑只在其出口证据齐全后切换。

## 已完成且已验证

- M1 可信输入：`RelativePath`、`FileIdentity`、dirfd-relative `DirectoryWalker`、`PhysicalAsset` 分类、批量 SQLite Frozen Manifest、稳定 digest、Mutation Guard、扫描异常传播。
- D1 实际实现（名称沿用历史 state，按蓝图属于 Local Folder/M1）：`LocalFolderSource` 从 root fd 递归扫描，按目录名稳定排序，将 regular file 流式写入 Manifest；`scan` CLI 能真实冻结并输出 manifest id、资产数和 digest。
- C1～C14 的领域类型、纯函数和持久化基础已存在，包含 LogicalAsset、Capability、PathMapper、Canonical Plan、JSONL Artifact、Task、TaskGraph、Runtime Repository、Copy/Hash、Temp Commit、Reconciler、Binary Verifier 和 CLI 路由；这些尚未组成完整 M2 闭环。
- `M2-C1 — plan`：从冻结 Manifest 生成确定性的 JSONL Frozen Plan，绑定 Manifest/digest、target root、capability/policy，并支持可重开与幂等写入。
- `M2-C2 — task persistence`：`migrate` 读取并校验 Frozen Plan，在 SQLite 事务中创建 migration/plan、逐资产写入一个 READY Task；重复 plan 返回 AlreadyExists，数据库无重复任务。
- `M2-C3 — single-thread executor preparation`：`migrate` claim 一个 READY Task，按 Frozen Plan 从 Manifest 读取 source root，使用 dirfd-relative Linux 操作打开 source fd 并校验冻结 `FileIdentity`；成功后保持 RUNNING，不宣称已发布 target。
- `M2-C4 — CopyAndHash + durable temp`：`migrate` 使用单线程和 1 MiB buffer 将冻结 source fd 流式复制到目标 root 下唯一 `.pbtmp`，计算 BLAKE3 并完成 `fdatasync`；未 rename，task 保持 RUNNING。
- `M2-C5 — no-replace publish + task success`：`resume` 重开 Frozen Plan，按 task 的 epoch/attempt 使用 dirfd-relative `RenameNoReplace` 发布 temp，目录 fsync 后以 fenced runtime 更新提交 SUCCEEDED；既有 target 不覆盖。
- `M2-C6 — independent binary verify`：`verify` 独立重开 source/target fd，分别计算摘要并比较冻结 source identity、大小和字节；匹配时输出 IDENTICAL，未复用迁移阶段结果。
- `M2-C7 — recovery/negative-path regression`：CLI 回归覆盖已有 final 的 no-replace 冲突、temp 证据保留、重复路径不产生第二份 target，以及 source 变化后 verify 失败；全量测试稳定通过。
- `M2-C8 — status`：新增只读 `status`，校验 JSONL artifact digest/semantic digest 与 SQLite 绑定，汇总 plan_task 状态，不执行 claim 或文件操作。
- `M2-C9 — multi-asset sequential execution`：多个单成员资产可由单线程逐 task migrate→resume，幂等 materialize 不重复插入；verify 独立检查全部 SUCCEEDED task。
- `L1-M3-D1 — Takeout fixture 与最小分类`：新增 dirfd-relative TakeoutParser 和 `parse --takeout` CLI；按原始相对路径稳定识别 media、JSON sidecar、album metadata 与未知 regular file；对 JSON 进行 8 MiB 有界读取和语法校验，未知、损坏、不可读或超限输入生成可诊断 parser error；不执行语义合并或元数据冲突决策。
- `L1-M3-D2 — JSON sidecar 候选`：为合法 JSON sidecar 生成直接的 `MetadataCandidate` 数据记录，绑定 `PhysicalAssetId`、字段、Google Takeout 来源、版本化 extraction rule 和 JSON pointer 证据；支持 title、description、favorited 与有明确 Unix 秒语义的 photoTakenTime.timestamp；范围/类型错误只生成 parser error，不产生伪候选；全量测试 132/132 通过。
- `L1-M3-D3 — media/sidecar 分类契约`：用 fixture 固化大小写不敏感的 RAW/JPEG/常见媒体、JSON/XMP sidecar、`Metadata.json` album metadata、未知扩展名和非 regular 文件分类；结果保持原始相对路径稳定排序；全量测试 133/133 通过。
- `L1-M3-D5 — SourceRelationCandidate`：以目录 + 小写 basename 索引生成唯一 media 与唯一 JSON/XMP sidecar 的关系候选；多媒体匹配和 orphan sidecar 只生成稳定诊断，不生成关系或 LogicalAsset；全量测试 134/134 通过。
- `L1-M3-D6 — orphan/missing/duplicate`：对无 sidecar media 生成 `missing_sidecar`，对无 media sidecar 生成 `orphan_sidecar`，对同 basename 多媒体/多 sidecar 生成 `ambiguous_sidecar_relation`；全量测试 134/134 通过。
- `L1-M3-D7 — filename truncation / same basename`：支持 `photo.jpg.json` 的明确扩展名 sidecar 规则；对严格前缀的疑似截断只生成 `truncated_basename_match` 诊断，不生成弱关系；全量测试 135/135 通过。
- `L1-M3-D8 — parser error report`：`parse --error-report` 输出稳定 JSONL，每行绑定安全 display path、错误码和诊断文本；报告写入失败显式返回 I/O 错误；全量测试 135/135 通过。
- `L1-M3-E1 — Edge/Evidence/Status`：新增直接的数据型 `AssociationEdge`、`Evidence` 与 `AssociationStatus`；parser 的已确认 sidecar 关系映射为 `kMetadataAttachment` confirmed edge，不进入 Composition union；全量测试 135/135 通过。
- `L1-M3-E2 — confirmed Composition graph`：实现最小 `AssociationGraph` 顶点/边校验和确定性 DSU 聚合；只有 confirmed `Composition` 边参与 LogicalAsset 合并，MetadataAttachment/ambiguous 边不会桥接；`parse` CLI 实际构建图并输出 logical asset 数；全量测试 138/138 通过。
- `L1-M3-E4 — Live Photo rule`：唯一 still + motion video 按同目录 basename 生成 `takeout.live-photo.v1` confirmed Composition；多 still/motion 只生成 `ambiguous_live_photo_relation`，并由 Graph 合并唯一 confirmed 组件；全量测试 140/140 通过。
- `L1-M3-E5 — RAW/JPEG rule`：唯一 RAW + JPEG 同 basename 生成 `takeout.raw-jpeg.v1` confirmed Composition；多重组合只生成 `ambiguous_raw_jpeg_relation`；全量测试 141/141 通过。
- `L1-M3-E6 — ambiguous edge`：confirmed/ambiguous 状态均可作为边保留；ambiguous Composition 不参与 DSU，组件结果仍按 canonical member 顺序稳定；全量测试 141/141 通过。
- `L1-M3-E7 — stable DSU aggregation`：多边 Composition 链按 canonical member ID 聚合，顶点/边逆序仍产生相同成员集合和逻辑 ID；MetadataAttachment 不参与桥接；全量测试 142/142 通过。
- `L1-M3-E8 — stable LogicalAssetId`：LogicalAssetId 只由排序后的成员集合决定，与关系插入顺序、边证据文本和运行时字段无关；全量测试 143/143 通过。
- Linux 虚拟机真实验证：`cmake --build build/debug -j2` 成功；CTest `134/134` 通过，0 失败。

## 当前不变量

- `main.cpp` 只初始化日志并调用 `RunCli()`；业务命令返回 `Status`，退出码只在 `ExitCodeForStatus()` 转换。
- Source 只读；路径使用原始字节并拒绝绝对路径、空组件、`.`、`..`、NUL；Linux 文件访问保持 fd/dirfd 绑定。
- Manifest、Plan 和 Task 的冻结输入不可静默改变；运行时状态与不可变任务语义分离。

## 当前阻塞与决策

- 阻塞：无。Linux 虚拟机通过 SMB 映射为 `Z:`，SSH `192.168.133.131` 可执行构建和测试。
- 活跃 Decision：`D1-001`，严格按最小纵向闭环推进，不继续横向铺设未来阶段。
- 已知边界：当前 M2 只覆盖单成员资产、单线程、每次一个 task；commit intent/verified receipt 的完整崩溃窗口恢复仍属于后续 L1，不在本轮横向扩展。
- 已知待处理：失败扫描留下 BUILDING Manifest 的清理策略，等实际闭环需要时再单独决策，不提前扩展。

## 本轮收敛结果

已完成 L0/M2 最小纵向闭环：

- `Local Folder → Frozen Manifest → LogicalAsset → Canonical JSONL Plan → SQLite Task`
- `migrate`：单线程 claim、冻结身份校验、CopyAndHash、`.pbtmp`、fdatasync
- `resume`：RenameNoReplace、目录 fsync、epoch/attempt fenced SUCCEEDED
- `verify`：独立 source/target hash、大小与 identity 验证
- `status`：只读 runtime 汇总
- 异常回归：source mutation、已有 target 冲突、重复执行、双文件顺序执行

验证结果：Linux 虚拟机 `cmake --build build/debug -j2` 成功；CTest `148/148` 通过，0 失败。

## L1/M3 目标

- D：Takeout fixture、JSON sidecar/media 分类、候选与 parser error。
- E：带证据的 Association Graph，弱证据不合并且输入顺序稳定。
- F：Canonical Photo IR、Metadata Resolver、冲突决策和 Provenance。

## 下一唯一任务

知识原子：如何把一个最小 Google Takeout 目录 fixture 解析为受约束的媒体/sidecar 分类记录，而不是直接做语义合并。

已完成任务：`L1-M3-D1`：新增最小 Takeout fixture 解析入口和分类模型；只处理目录遍历、JSON sidecar 与媒体文件的稳定识别，未知/损坏文件产生可诊断 parser error；不创建 Association、Resolver 或 Planner 空壳。

已完成任务：`L1-M3-D2`：为合法 JSON sidecar 提取最小 `MetadataCandidate`；候选绑定物理资产、字段、提取规则和 JSON pointer 证据，类型/范围错误保留为 parser error，不进行 Association 或 Resolver 决策。

已完成任务：`L1-M3-D3`：收敛 media/sidecar 分类契约；覆盖大小写不敏感的 RAW/JPEG/常见媒体、JSON/XMP sidecar、`Metadata.json` album metadata、未知扩展名和非 regular 文件，并保持 parser 输出顺序稳定。

已完成任务：`L1-M3-D5`：从已解析的 Takeout 命名与 JSON 引用生成最小 `SourceRelationCandidate`；只记录有明确证据的 sidecar/media 关系，无法确认的 orphan、重复或歧义输入保留为诊断，不创建 Association Graph 或进行 union。

已完成任务：`L1-M3-D6`：完善 orphan、missing sidecar、duplicate export 的可诊断分类；区分“媒体无 sidecar”“sidecar 无媒体”和“同 basename 多候选”，保持输入顺序无关，并为后续 Association 提供不带决策的证据记录。

已完成任务：`L1-M3-D7`：增加 filename truncation 与同 basename 的稳定规则；区分精确 basename 证据和截断/重复导致的歧义，禁止仅凭弱匹配生成关系，保留可审计 parser error。

已完成任务：`L1-M3-D8`：将 parser errors 以稳定 JSONL 报告输出并接入 `parse --error-report`；报告绑定相对路径、错误码和诊断文本，不改变 parser 的候选/关系决策边界。

已完成任务：`L1-M3-E1`：新增直接的数据型 `AssociationEdge`、`Evidence` 与 `AssociationStatus`；将 parser 的 confirmed/ambiguous 关系映射为可审计边记录，只有明确 confirmed 的 Composition 才允许后续进入 union，当前不实现 DSU 或 LogicalAsset 构建。

已完成任务：`L1-M3-E2`：实现最小 `AssociationGraph` 顶点/边校验与确定性组件构建；仅 confirmed `Composition` 边参与 union，`MetadataAttachment`/`AssetRelation` 即使 confirmed 也只保留为边，不能桥接 LogicalAsset。

说明：`L1-M3-E3` 的 sidecar relation rule 已在 D5 中以 `takeout.basename.v1` 和 `takeout.sidecar-media-extension.v1` 实际落地并验证。

已完成任务：`L1-M3-E4`：增加 Live Photo 唯一 still + motion video 的 Composition 规则；多 still/motion 只生成 ambiguity，不进行 union，保持关系证据和 LogicalAssetId 稳定。

已完成任务：`L1-M3-E5`：增加唯一 RAW + JPEG 同 basename 的 Composition 规则；多 RAW/JPEG 组合只生成 ambiguity，不进行弱 union，保持输入顺序与 LogicalAssetId 稳定。

已完成任务：`L1-M3-E6`：固化 ambiguous edge 的保留与禁止合并语义；ambiguous Composition 只作为可审计边存在，不能进入 DSU，且输入顺序不影响组件结果。

已完成任务：`L1-M3-E7`：固化多边 Composition 链的稳定 DSU 聚合；组件成员按 canonical ID 排序，结果不依赖顶点/边输入顺序，MetadataAttachment 不得参与桥接。

已完成任务：`L1-M3-E8`：固化每个组件的 canonical `LogicalAssetId`；ID 只由排序后的成员集合决定，不受关系插入顺序、证据文本或运行时字段影响。

实现任务：`L1-M3-F1`：新增最小 Canonical Photo IR；将 LogicalAsset 成员和 confirmed 关系投影为稳定 media component/relationship 记录，保留候选来源，不在 IR 阶段解决元数据冲突。

已完成任务：`L1-M3-F1`：新增直接数据型 `CanonicalPhoto`、`PhotoMediaComponent` 与 `PhotoRelationship`；按 canonical member/asset ID 稳定投影媒体组件，只输出组件内 confirmed 关系，并保留 rule、evidence 和 status；`parse` CLI 已实际调用该投影，Linux 全量测试 146/146 通过。

实现任务：`L1-M3-F2`：将 MetadataCandidate 的受控值收敛为 `MetadataValue` variant，并为字段类型提供最小、可验证的访问边界；不实现候选选择或冲突决策。

已完成任务：`L1-M3-F2`：补充 `MetadataValueKind` 判别和 `IsMetadataValueCompatible` 字段类型契约；Takeout parser 写入候选前执行该契约，错误仍落为 parser error；新增 variant/字段兼容性测试，Linux 全量测试 148/148 通过。

实现任务：`L1-M3-F3`：固化 `TimeCandidate` 的绝对时间/本地时间、精度、来源和原始值边界；只做结构校验，不进行时区推断或候选选择。

已完成任务：`L1-M3-F3`：增加 `ValidateTimeCandidate`；校验 raw value、已知 precision/source、UTC offset 与 LocalDateTime 日历字段，保留 AbsoluteTime 原值且不隐式转换时区；Takeout parser 对时间候选执行校验，Linux 全量测试 151/151 通过。

实现任务：`L1-M3-F4`：增加最小版本化 MetadataRuleset/Rule Chain 数据记录；让候选提取规则可按明确顺序描述，不执行候选选择。

已完成任务：`L1-M3-F4`：新增版本化 `MetadataRuleset`/`MetadataRule`；固定 Takeout 四条 extraction rule 的链顺序，并在 parser 生成候选时验证 rule、field、source 绑定；未引入候选优先级或冲突决策，Linux 全量测试 154/154 通过。

实现任务：`L1-M3-F5`：增加直接数据型 `ResolutionRecord`，记录 selected value/source、rule/version、全部候选、conflict 与 reason；先提供结构校验，不实现 resolver 选择算法。

已完成任务：`L1-M3-F5`：新增 `ResolutionRecord` 与结构校验；成对记录 selected value/source，绑定 rule/version，保留同字段全部候选，并要求 conflict 有 reason；Linux 全量测试 157/157 通过，未引入 resolver 选择。

实现任务：`L1-M3-F6`：实现纯逻辑 MetadataResolver；按版本化规则链、asset ID、rule/evidence 的 canonical 顺序选择，并显式记录多值冲突与 tie-break，不读取系统时钟或候选插入顺序。

已完成任务：`L1-M3-F6`：新增无状态 `MetadataResolver`；验证 ruleset/candidate 绑定，按规则链、asset ID、rule、evidence 稳定排序，选择 canonical 首项，检测 distinct value 冲突并写入 tie-break reason；Linux 全量测试 160/160 通过。

实现任务：`L1-M3-F7`：将 ResolutionRecord 展开为逐候选 ProvenanceRecord，保留值、来源、rule/version、evidence 和 selected/rejected outcome；不改变 resolver 结果。

已完成任务：`L1-M3-F7`：新增 `ProvenanceRecord` 与 `BuildProvenance`；逐候选保留 value/source/rule/version/evidence，明确 selected 或 conflict tie-break rejected，空 resolution 不伪造记录；Linux 全量测试 162/162 通过。

实现任务：`L1-M3-F8`：固化 resolver 的输入顺序无关、规则版本绑定、冲突和空候选行为；完成 M3/F 出口验证后切换到 G1 SupportLevel/TargetCapabilities。

已完成任务：`L1-M3-F8`：新增 resolver 逆序输入、canonical tie-break、空候选、ruleset version 绑定测试；M3/F 出口满足“选择可解释、规则可追溯、结果确定性”，Linux 全量测试 164/164 通过。

实现任务：`L1-M4-G1`：收敛 `SupportLevel` 与 `TargetCapabilities` 的显式数据契约；保留已有 Local Directory 合法子集，不以 bool 替代能力等级。

已完成任务：`L1-M4-G1`：为 `SupportLevel` 增加合法性判别与稳定名称，`TargetCapabilities::Validate` 拒绝未知 support level；既有 Local Directory 八项能力和枚举子集保持不变，Linux 全量测试 165/165 通过。

实现任务：`L1-M4-G2`：将 Local Directory capability 与实际 planner/commit 前置检查绑定；对 unsupported/unverifiable 能力给出显式结果，不静默按 Full 处理。

已完成任务：`L1-M4-G2`：将 `TargetCapabilities::Require` 作为 planner 的统一能力边界，`TargetPathMapper` 对 regular file、byte exact、nested directory 等要求显式检查；unsupported/unverifiable 不再隐式升级，Linux 定向与全量测试 166/166 通过。

实现任务：`L1-M4-G3`：增加最小 LossAnalysis 记录，将目标能力不足映射为可审计的 loss/support 结果；只报告损失，不自动改变迁移计划。

已完成任务：`L1-M4-G3`：新增纯逻辑 `LossAnalysis`，逐项报告非 Full capability 的 level/representation/reason；plan 阶段实际输出 capability loss 数量但不改写计划，Linux 全量测试 168/168 通过。

实现任务：`L1-M4-G4`：收敛 Full TargetPathMapper 的 capability/policy 输入边界，并保持既有 LogicalAsset 教学 fixture 合法子集。

已完成任务：`L1-M4-G4`：保留现有 LogicalAsset 教学 fixture 兼容性，不引入会破坏合法子集的 ID 收紧；`TargetPathMapper` 已统一通过 `TargetCapabilities::Require` 检查 regular/byte-exact/nested 目标能力，既有路径、碰撞和安全边界全量回归通过，Linux 全量测试 168/168 通过。

实现任务：`L1-M4-G5`：显式化 PlannerInput，令 manifest、target、capabilities、policy、logical assets 成为不可遗漏的冻结输入，并保持现有 CanonicalMinimalPlan 调用链。

已完成任务：`L1-M4-G5`：以 `PlannerInput` 作为 CanonicalMinimalPlan 的公开 planner 边界名称，保留 `MinimalPlanInput` 兼容别名；CLI plan 已通过该显式输入组装并冻结 manifest/target/capability/policy/assets，Linux 全量测试 168/168 通过。

实现任务：`L1-M4-G6`：固化 CanonicalPlanPayload 的字段版本、长度编码和运行时字段隔离；补充 payload 对冻结输入敏感、对执行状态不敏感的回归证据。

已完成任务：`L1-M4-G6`：将 `CanonicalPlanPayload` 作为正式语义投影 API，保留 `CanonicalPayload` 兼容入口；SemanticDigest 已绑定正式 API，字段版本/长度编码及执行态隔离回归通过，Linux 全量测试 168/168 通过。

实现任务：`L1-M4-G7`：固化完整 frozen plan 的稳定排序与序列化顺序；验证不同输入顺序生成相同 semantic/artifact 结果，继续禁止 planner 读取 worker/runtime 状态。

已完成任务：`L1-M4-G7`：补充多资产逆序输入的 canonical plan/artifact 字节与 semantic digest 相等测试；plan asset 输出保持稳定排序，Linux 全量测试 169/169 通过。

实现任务：`L1-M4-G8`：给 frozen plan JSONL 增加并校验独立 semantic projection version；保持 artifact digest 与 semantic digest 分离，并覆盖未知版本拒绝。

已完成任务：`L1-M4-G8`：frozen plan header 写入 `canonical-plan-semantic-v1`，读取时严格校验；artifact digest 仍绑定完整 JSONL 字节，semantic digest 绑定 canonical payload，未知版本回归被拒绝，Linux 全量测试 170/170 通过。

实现任务：`L1-M4-G9`：将 plan JSONL 发布路径接入 crash-safe 临时文件、fdatasync、no-replace 与目录 fsync；禁止直接 truncate final plan 文件。

已完成任务：`L1-M4-G9`：plan CLI 发布改用 sibling `.pbtmp`、短写循环、fdatasync、rename-no-replace、directory fsync；已有 final 相同字节幂等、不同字节冲突语义保留，Linux 目标链与全量测试 170/170 通过。

实现任务：`L1-M4-G10`：补全 frozen plan replay、semantic version upgrade refusal 与 artifact/JSONL corruption 回归；确认 Resume 只读取已发布 Plan，不重新 Planner。

已完成任务：`L1-M4-G10`：验证已发布 plan 可 round-trip replay；未知 semantic version、JSONL 损坏和缺失 LF 均拒绝；现有 CLI resume 测试从 frozen artifact 读取并继续 runtime，不重新运行 planner，Linux 全量测试 170/170 通过。

实现任务：`L1-M5-H1`：收敛 MigrationTask 类型与持久化任务状态边界；区分不可变 task 语义和可重建 runtime 状态，保持现有单任务闭环兼容。

已完成任务：`L1-M5-H1`：公开稳定 `TaskTypeName`/`IsKnownTaskType`，TaskSpec 继续将 immutable 语义与 TaskRuntime/attempt 字段分离；未知 task type 在 spec/key 入口一致拒绝，Linux 全量测试 171/171 通过。

实现任务：`L1-M5-H2`：固化 versioned TaskKey 语义；只绑定 task type、logical asset、target path、source asset 等不可变输入，明确排除执行态/估算态/输出元数据。

已完成任务：`L1-M5-H2`：公开 `PB_TASK_V1` TaskKey semantic version，并保持 BLAKE3 key 只绑定不可变 task type/asset/path/source；id、estimated bytes、expected digest 与 runtime 不影响 key，Linux 全量测试 172/172 通过。

实现任务：`L1-M5-H3`：确认 dependency table 的计划级外键、唯一性和 task-id 绑定；持久化依赖不得跨 plan 或引用未知 task。

已完成任务：`L1-M5-H3`：SQLite `task_dependency` 使用 `(plan_id, task_id, depends_on_task_id)` 主键及双复合外键；repository 拒绝未知、跨 plan、自依赖，新增跨 plan 回归，Linux 全量测试 173/173 通过。

实现任务：`L1-M5-H4`：固化 TaskGraph 与持久化依赖的确定性 cycle detection；循环计划必须在 READY/执行前拒绝，不能靠 worker 顺序绕过。

已完成任务：`L1-M5-H4`：TaskGraph 使用确定性 Kahn 检测循环，SQLite repository 在新增依赖前用递归 CTE 拒绝持久化 cycle；新增三节点持久化环回归，Linux 全量测试 174/174 通过。

实现任务：`L1-M5-H5`：固化 READY 计算规则；只有 task 为 Planned/Retryable 且全部同 plan 依赖为 Succeeded 才能进入 READY，依赖未完成时不得被手工 SetReady 绕过。

已完成任务：`L1-M5-H5`：`SetReady` 以 SQL 条件同时约束 Planned/Retryable 状态与全部同 plan 依赖 Succeeded；依赖未完成的 task 不能手工 READY，已有依赖/claim 测试覆盖，Linux 全量测试 174/174 通过。

实现任务：`L1-M5-H6`：固化 BeginExecution 入口；claim 必须原子地把 READY task 转为 RUNNING，写入 owner epoch/attempt，并拒绝非 READY 或空 fencing identity。

已完成任务：`L1-M5-H6`：`ClaimNextReady` 已将 READY 选择、owner epoch/attempt 写入、attempt_count 增加和 RUNNING 读取置于 `BEGIN IMMEDIATE` 事务；新增空 fencing identity 回归，Linux 全量测试 175/175 通过。

实现任务：`L1-M5-H7`：验证并固化多连接 claim 的原子互斥；同一 READY task 只能被一个 executor 成功认领，第二个 executor 必须观察到无可认领任务。

已完成任务：`L1-M5-H7`：`BEGIN IMMEDIATE` + 条件 UPDATE 已验证多连接 claim 互斥；同一 READY task 只有首个 executor 成功，第二个返回 NotFound，Linux 全量测试 176/176 通过。

实现任务：`L1-M5-H8`：固化 owner epoch + attempt_id 的 stale update fencing；旧 executor 的完成/失败更新必须影响 0 行，不能把新 attempt 推入 COMMITTED/SUCCEEDED。

已完成任务：`L1-M5-H8`：completion UPDATE 同时绑定 plan/task、owner_epoch、active_attempt_id 与运行状态；旧 epoch/attempt 的 success/retry 更新被拒绝，Linux 全量测试 176/176 通过。

实现任务：`L1-M5-H9`：为 Resume 增加计划级跨进程互斥锁；锁冲突显式失败，避免两个恢复器同时处理同一 temp/attempt。

已完成任务：`L1-M5-H9`：Resume 进入执行前获取 workspace `locks/<plan_id>.lock` 的非阻塞 exclusive flock；跨进程冲突返回 AlreadyExists，锁由 RAII fd 持有至恢复结束，Linux 全量测试 176/176 通过。

实现任务：`L1-M6-I1`：增加最小 DB command queue 边界，串行化 SQLite runtime 写入并返回调用方可检查的结果；保持现有同步 repository 语义兼容。

已完成任务：`L1-M6-I1`：新增单 worker `DbCommandQueue`，FIFO 执行 SQLite command，队列停止后拒绝新命令；现有同步 repository 保持兼容，Linux queue 定向与全量测试 178/178 通过。

实现任务：`L1-M6-I2`：固化 command queue 的 promise/future acknowledgement 与异常/错误传播；调用方必须等待并检查 ACK，不能把入队当成已持久化。

已完成任务：`L1-M6-I2`：`Submit` 返回 future，worker 将 command 成功/失败（含异常转 Internal）写入 ACK；停止后 future 明确返回 InvalidArgument，测试覆盖 FIFO、failure propagation 与 stop rejection，Linux 全量测试 178/178 通过。

实现任务：`L1-M6-I3`：区分 durable runtime event 与可重建 progress；为 task 状态变更保留可审计的持久事件边界，progress 只能从 durable state 重建。

已完成任务：`L1-M6-I3`：schema v4 新增 `task_event` durable event 表及 plan/task 索引；claim 与 SUCCEEDED/RETRYABLE completion 事件和 plan_task 状态更新共用事务，新增事件顺序回归；Linux 全量测试 179/179 通过。

实现任务：`L1-M6-I4`：固化 DB writer worker 的启动、FIFO drain、停止与 join 生命周期；停止不得遗留未兑现 future 或悬空线程。

已完成任务：`L1-M6-I4`：`DbCommandQueue` worker 在构造时启动、按 FIFO drain，Stop 可重复调用且 join 后无悬空线程；新增 Stop 前 drain 回归，Linux 全量测试 180/180 通过。

实现任务：`L1-M6-I5`：为 executor command queue 增加显式容量上限；队列满时立即以 ACK 报告 backpressure，不无限制积压任务。

已完成任务：`L1-M6-I5`：`DbCommandQueue` 增加 max_pending 容量，满队列以 IoError ACK 施加 backpressure；阻塞 worker/排队/第三条拒绝回归通过，Linux 全量测试 181/181 通过。

实现任务：`L1-M6-I6`：增加有界 byte permit pool；任务申请超过总预算立即拒绝，预算不足时等待释放，Stop/异常路径必须可归还 permit。

已完成任务：`L1-M6-I6`：新增可关闭 `BytePermitPool`，超容量请求 InvalidArgument、预算不足等待释放、Close 唤醒等待者；覆盖 usage 与释放回归，Linux 全量测试 184/184 通过。

实现任务：`L1-M6-I7`：增加有界 file-descriptor permit pool；打开文件前申请、关闭后释放，超过预算或关闭状态不得继续消耗资源。

已完成任务：`L1-M6-I7`：新增可关闭 `FdPermitPool`，Acquire/Release 维护并发 fd 预算，Close 拒绝后续申请并唤醒等待；Linux 定向与全量测试 186/186 通过。

实现任务：`L1-M6-I8`：固化 worker/DB command failure propagation；任一 command 的 error/exception 必须经 ACK 返回调用方，不得被吞掉或伪装为成功。

已完成任务：`L1-M6-I8`：queue 对 SQLite failure 与 command exception 均经 future 返回错误，异常转换为 Internal；新增 exception ACK 回归，Linux 全量测试 187/187 通过。

实现任务：`L1-M6-I9`：固化 graceful stop/join；重复 Stop 安全，队列 drain 后线程退出，所有 submitted future 均有终态。

已完成任务：`L1-M6-I9`：DbCommandQueue 支持 graceful drain、重复 Stop、join 与 submitted future 全部终态；Linux queue 全量测试 187/187 通过。

实现任务：`L1-M6-I10`：增加最小 BoundedExecutor，验证 1/2/4 worker 执行同一确定性 task 集合得到相同结果；DB 写入仍由单 writer queue 串行化。

已完成任务：`L1-M6-I10`：新增有界 FIFO `BoundedExecutor`，1/2/4 worker 对同一确定性 task 集合产生相同结果；队列满、异常传播和 graceful stop 均有 ACK/回归覆盖，Linux 全量测试 189/189 通过。

实现任务：`L1-M6-J1`：固化 copy/commit 使用的 WriteAll / Read loop；短读、短写和 zero-progress 必须有确定错误，不得静默截断或死循环。

已完成任务：`L1-M6-J1`：`CopyAndHash` 的读循环与内部 `WriteAll` 已处理短读/短写，zero-progress 写入返回 `IoError`；`VerifyBinary` 同样拒绝越界读结果，相关回归与 Linux 全量测试 189/189 通过。

实现任务：`L1-M6-J2`：固化 source-before/source-after identity 校验；复制或验证期间源文件身份发生变化时必须拒绝提交。

已完成任务：`L1-M6-J2`：`CopyAndHash` 与 `MutationGuard` 均比较读前/读后 `FileIdentity`，发现源文件变化即返回错误；Linux 定向与全量测试 189/189 通过。

实现任务：`L1-M6-J3`：固化由 plan/task/attempt 派生的确定性 temp 名称；重试不得覆盖其他任务的临时文件。

已完成任务：`L1-M6-J3`：执行与 resume 使用相同的 `.photobridge.<plan>.<task>.<attempt>.pbtmp` 派生规则，临时文件以 `O_EXCL` 创建；CLI、临时提交和 Linux 文件操作测试通过，定向 26/26、全量 189/189 通过。

实现任务：`L1-M6-J4`：临时文件内容完成后必须执行 fdatasync，并在失败时停止发布。

已完成任务：`L1-M6-J4`：`CopyToTempAndPublish` 与 CLI 执行路径均在内容复制完成后调用 `Fdatasync`，失败即停止发布并清理可安全清理的 temp；新增失败回归，Linux 全量测试 190/190 通过。

实现任务：`L1-M6-J5`：临时文件需完成内容 hash/验证 receipt，且 durable ACK 必须发生在发布成功之后。

已完成任务：`L1-M6-J5`：schema v5 新增 `verified_receipt`，claim 同时登记 `task_attempt`；migrate 在 fdatasync 后独立读回 temp、比较 size/hash，再以事务 ACK 持久化 receipt 和 `VERIFIED_DURABLE` 事件，之后才允许 resume 发布；Linux 全量测试 191/191 通过。

实现任务：`L1-M6-J6`：发布必须使用 rename-no-replace；目标已存在时返回冲突，不覆盖已有文件。

已完成任务：`L1-M6-J6`：`LinuxFileOps::RenameNoReplace` 使用 `renameat2(RENAME_NOREPLACE)`，目标存在映射为 `AlreadyExists`，冲突回归确认已有目标内容保持不变；Linux 定向测试 6/6 通过。

实现任务：`L1-M6-J7`：rename 后必须 fsync 目标父目录；目录同步失败不得报告完整提交成功。

已完成任务：`L1-M6-J7`：temp commit 与 resume 均在 rename 后调用 `FsyncDirectory`，同步失败直接返回，不写成功状态；Linux 提交/目录同步定向测试 7/7 通过。

实现任务：`L1-M6-J8`：COMMITTED/SUCCEEDED 状态必须在文件发布和目录 fsync 之后，经带 epoch/attempt 条件的 DB 事务 ACK 才能落盘。

已完成任务：`L1-M6-J8`：resume 严格按 rename、directory fsync、`MarkSucceeded` 顺序执行；完成事务同时校验 plan/task/owner_epoch/attempt/state 并写 durable event，stale completion 不改变状态；相关 Linux 回归 4/4 通过。

实现任务：`L1-M6-J9`：final 已存在或 hash 不匹配时必须报告 target conflict，绝不覆盖或自动追加后缀。

已完成任务：`L1-M6-J9`：`RENAME_NOREPLACE` 冲突、无 receipt 的 final、receipt/hash 不匹配均进入 conflict/inconsistent 决策；CLI 冲突回归确认已有文件不变，Reconciler/Linux 定向测试 6/6 通过。

实现任务：`L1-M6-J10`：为关键 FileOps 操作提供可注入 fault points，覆盖读写、sync、rename、目录 sync 失败并验证错误不可伪装为成功。

已完成任务：`L1-M6-J10`：`FileOps` 依赖注入已覆盖 read/write zero-progress、fdatasync、rename 和 directory-fsync fault；新增目录同步失败回归，Linux 定向 27/27、全量测试 192/192 通过。

实现任务：`L1-M6-K1`：固化恢复观察对象，收集 source/temp/final 存在性、size、opened-fd identity 与独立 digest，禁止仅凭路径存在推断归属。

已完成任务：`L1-M6-K1`：`ObservedFileState` 已包含 source/temp/final 存在性、size、独立 digest 与可选 opened-fd identity；采集字段与纯决策分离，Linux Reconciler/CLI 定向测试 8/8 通过。

实现任务：`L1-M6-K2`：固化纯 `ReconcileDecision`，所有恢复分支必须由冻结 task、runtime、intent、receipt 和观察值确定地产生，不执行 I/O 或写库。

已完成任务：`L1-M6-K2`：`Decide()` 仅验证输入并返回确定的 retry/resume/adopt/conflict/inconsistent action，不触碰文件或数据库；Reconciler 四分支测试通过。

实现任务：`L1-M6-K3`：恢复时仅按 receipt 归属识别并处理本任务 temp；无 receipt 或无法验证归属时不得批量清理。

已完成任务：`L1-M6-K3`：无 receipt 的 temp 只返回“重做并清理本任务 temp”规则，有 receipt 则必须匹配 intent 的 temp 路径、size/hash 后才允许 resume；不存在按扩展名批量删除逻辑，定向 recovery 测试 2/2 通过。

实现任务：`L1-M6-K4`：rename 后崩溃恢复时，只有可信 receipt + 独立匹配 final 才能采纳 final，并完成目录同步与状态接管。

已完成任务：`L1-M6-K4`：Reconciler 对 receipt 匹配 final 返回 adopt/reverify 分支，对 temp+final 返回 adopt-and-cleanup；intent/receipt 绑定不匹配拒绝，独立规则测试 2/2 通过。

实现任务：`L1-M6-K5`：已标记成功但 final 缺失或不可验证时必须报告 inconsistent，不能自动伪造成功或覆盖目标。

已完成任务：`L1-M6-K5`：SUCCEEDED task 在 final 缺失、size/hash 不匹配或 source 也不可用时统一返回 `kInconsistent`；回归测试通过。

实现任务：`L1-M6-K6`：固化目标独立二进制读回验证，比较 size、digest 与 opened-fd identity，验证失败不得进入 committed。

已完成任务：`L1-M6-K6`：`VerifyBinary` 独立读回目标，比较 bytes/digest，并检查 opened fd 的前后 `FileIdentity`；identical/mismatch/not-applicable/mutation 均有覆盖，Linux 定向测试 3/3 通过。

实现任务：`L1-M6-K7`：增加 metadata verification 边界；验证结果必须区分匹配、不匹配与不可验证，不得把缺失 metadata 当成成功。

已完成任务：`L1-M6-K7`：新增纯 `VerifyMetadata`，缺失数据返回 `UNVERIFIABLE`、值相等返回 `IDENTICAL`、值不等返回 `MISMATCH`；Linux 定向 6/6、全量测试 193/193 通过。

实现任务：`L1-M6-K8`：增加 relation verification 边界；关系验证必须区分完整匹配、冲突与不可验证，不得仅凭单边关系通过。

已完成任务：`L1-M6-K8`：新增纯 `VerifyRelations`，比较关系端点、类型、状态、规则和证据；缺失任一侧返回 `UNVERIFIABLE`，Linux 定向 7/7、全量测试 194/194 通过。

实现任务：`L1-M6-K9`：增加确定性的恢复 diff engine，输出 expected/observed 的新增、删除、变化项，供审计与人工处理使用。

已完成任务：`L1-M6-K9`：新增 `DiffFileStates`，按 path 稳定排序输出 added/removed/changed，比较 size 与 digest；Linux 定向 3/3、全量测试 195/195 通过。

实现任务：`L1-M6-K10`：增加可审计恢复报告，将决策、验证状态、diff 与错误原因以稳定文本输出，不隐藏冲突或不可验证状态。

已完成任务：`L1-M6-K10`：新增稳定 JSONL `RenderAuditReport`，输出 action/verification/diff/error；Linux 定向 2/2、全量测试 196/196 通过。

实现任务：`L1-M7-L1`：固化可重复的 deterministic fault hooks，为 FileOps/commit 流程按命名故障点注入失败。

已完成任务：`L1-M7-L1`：`FileOps` 抽象与 deterministic test doubles 已覆盖 copy、verify、fdatasync、rename、directory-fsync 故障点；Linux fault 回归 13/13 通过。

实现任务：`L1-M7-L2`：覆盖 EINTR、短读和短写，确保重试循环不丢数据、不死循环。

已完成任务：`L1-M7-L2`：Linux `Read`/`Write` 对 EINTR 重试，copy loop 处理短读，`WriteAll` 处理短写并拒绝 zero-progress；Linux I/O 定向测试 2/2 通过。

实现任务：`L1-M7-L3`：固化 ENOSPC/EIO/permission 错误映射和 commit 停止边界，失败不得留下被认为成功的状态。

已完成任务：`L1-M7-L3`：POSIX errno 分类覆盖 EIO/ENOSPC/EACCES/EPERM，copy/sync/rename/dir-sync 失败均返回错误并阻断成功状态；Linux 定向测试 11/11 通过。

实现任务：`L1-M7-L4`：覆盖 receipt ACK、rename、directory fsync、DB completion 各崩溃窗口，恢复结果必须可判定且不可伪造成功。

已完成任务：`L1-M7-L4`：receipt-before-publish、rename failure、directory sync failure、stale DB completion 与 Reconciler missing/mismatch 分支均有回归；Linux crash-window 组合测试 12/12 通过。

实现任务：`L1-M7-L5`：增加 kill -9 恢复 harness，验证进程被终止后 lock、runtime、temp/receipt 状态可被下一次 resume 观察。

已完成任务：`L1-M7-L5`：新增 Linux `LockRecoveryTest`，子进程持锁后 SIGKILL，下一 owner 成功获取同一稳定 lock；结合 CLI/runtime/reconciler 窗口回归，Linux 全量测试 197/197 通过。

实现任务：`L1-M7-L6`：执行重复 migrate/resume/verify soak，确认重复操作不重复覆盖、不产生额外 temp、结果和 durable receipt 稳定。

已完成任务：`L1-M7-L6`：CLI soak 回归覆盖多资产重复 migrate/resume、冲突后重试、完成后重复 migrate 和重复 verify；4/4 通过，temp/目标/状态断言稳定。

实现任务：`L1-M7-L7`：增加 scanner benchmark 基线，记录固定 fixture 的扫描耗时与资产数，作为后续性能比较基准。

已完成任务：`L1-M7-L7`：100k 文件 streaming scanner fixture 通过，资产数与冻结 manifest 均为 100k；Linux 基线耗时 12.81s。

实现任务：`L1-M7-L8`：增加 copy/hash benchmark 基线，验证大 payload 的流式复制、hash 和短写处理不产生额外全量内存。

已完成任务：`L1-M7-L8`：新增 4 MiB/64 KiB buffer 流式 copy/hash 回归，完整 payload 与摘要契约通过；Linux 全量测试 198/198 通过。

实现任务：`L1-M7-L9`：增加 SQLite writer 批量 benchmark 基线，验证 FIFO ACK、事务提交和容量背压在固定 command 集合下稳定。

已完成任务：`L1-M7-L9`：新增固定 256 command queue ACK benchmark baseline，所有 future 均成功；Linux 定向 6/6、全量测试 199/199 通过。

实现任务：`L1-M7-L10`：更新 README/docs/demo，说明 Frozen Plan、receipt-before-rename、resume、verify、冲突与测试运行方式。

已完成任务：`L1-M7-L10`：README 已补 Linux 构建/测试、CLI 快速流程、receipt-before-rename 提交顺序、冲突和恢复规则；文档后 Linux 全量测试 199/199 通过。

实现任务：`L1-M7-L11`：补充并验证 resume demonstration，展示 migrate 留下 RUNNING/temp/receipt，resume 发布并完成状态，verify 独立确认目标。

已完成任务：`L1-M7-L11`：CLI demo 回归覆盖 migrate 生成 RUNNING/temp/receipt、冲突不覆盖、resume 完成并清理 temp、verify 返回 IDENTICAL；2/2 通过。

实现任务：`L1-M7-L12`：完成一次蓝图/收敛规则/实现/测试一致性 review，修正遗留边界并给出最终验证结果。

已完成任务：`L1-M7-L12`：review 修正 `FdPermitPool(0)` 永久等待边界，确认 attempt 唯一性、receipt-before-rename、rename-no-replace、目录 fsync、fencing、恢复/审计与 benchmark 文档一致；Linux Debug 构建与全量测试 200/200 通过。

实现任务：`L1-M7-Benchmark-1`：建立蓝图要求的 `benchmarks/` 目录与独立 `photobridge_bench` Linux target；正式压测不得复用 GoogleTest 入口。

已完成任务：`L1-M7-Benchmark-1`：新增 `benchmarks/photobridge_bench.cpp` 和 `benchmarks/README.md`，CMake 建立独立 `photobridge_bench` 目标；保留 `tests/` 中既有压力回归作为 correctness/soak 证据。

实现任务：`L1-M7-Benchmark-2`：验证 Release benchmark 的三条 workload、JSON 报告、P95/P99 计算和 Linux 进程资源采样。

已完成任务：`L1-M7-Benchmark-2`：Linux Release 构建成功；小参数 smoke run 的 scanner/copy/hash/SQLite 三项均通过，JSON 可被标准解析器读取；复测确认每个 scanner 样本重新打开 root fd，避免目录流 offset 污染重复样本。

实现任务：`L1-M7-Benchmark-3`：使用固定正式参数运行 Release benchmark，收集重复样本的吞吐率、P95/P99、CPU、RSS 和进程 IO 统计，并完成结果审查。

已完成任务：`L1-M7-Benchmark-3`：Release 5 次正式压测完成；scanner 平均 49,814 files/s、wall P95 2031.86 ms，copy/hash/fdatasync 平均 241.05 MB/s、wall P95 292.61 ms，SQLite writer ACK 平均 1,396.8 commands/s、wall P95 3841.93 ms；Debug 全量 CTest 200/200 通过。详细基线记录于 `docs/benchmark.md`，原始 JSON 保存在 Linux VM `/tmp/photobridge-benchmark-release.json`。

本轮 M7 benchmark 收口：正式代码位于 `benchmarks/photobridge_bench.cpp`，独立目标为 `photobridge_bench`；`tests/` 中的旧压力用例继续作为回归测试，不作为正式性能入口。

实现任务：`L1-M7-Benchmark-4`：为本地端到端链路增加 30 次独立样本 benchmark，覆盖真实 CLI 的 `scan → plan → migrate → resume → verify`。

已完成任务：`L1-M7-Benchmark-4`：新增显式 `e2e_local_pipeline` workload；每次迭代使用独立 workspace/target，源 fixture 与 workspace 初始化不计入样本计时，多资产按现有单 task CLI 契约逐个 migrate/resume；已补充运行说明，进入编译和 smoke 验证。

实现任务：`L1-M7-Benchmark-5`：执行本地端到端 Release benchmark 30 次，记录完整 pipeline 的 wall/P95/P99、吞吐、CPU、RSS 和 IO，并验证每次结果为独立成功迁移。

已完成任务：`L1-M7-Benchmark-5`：e2e smoke 已通过（2 次、2 文件、4 KiB/文件）；正式 30 次压测使用 4 文件、64 KiB/文件，平均 91.94 ms/pipeline，wall P95 122.78 ms、P99 129.27 ms，平均 11.13 pipelines/s，CPU utilization 49.08%，peak RSS 7,956 KiB；30/30 次 verify 均为 IDENTICAL。

本轮端到端 benchmark 收口：Release 构建目标为 `photobridge_bench`，原始报告为 Linux VM `/tmp/photobridge-e2e-local-30.json`；计时覆盖 `scan → plan → migrate × 4 → resume × 4 → verify`，不包含 fixture 创建和 workspace 初始化。

实现任务：`L1-M7-Benchmark-6`：为 benchmark 增加专用 `bench-release` CMake configure/build preset，固定 Release 配置并只构建 `photobridge_bench`。

已完成任务：`L1-M7-Benchmark-6`：`CMakePresets.json` 新增 `bench-release` configure/build preset，目标目录为 `build/bench-release`，固定 `VCPKG_TARGET_TRIPLET=x64-linux`；benchmark README 已切换为 preset 命令；Linux VM 实机验证 configure/build/rebuild 和 benchmark `--help` 均成功。

实现任务：`L1-M7-Benchmark-7`：运行 Large Local E2E，验证较大多资产 pipeline 在本地 Linux 文件系统上的端到端耗时和资源曲线。

已完成任务：`L1-M7-Benchmark-7`：3 次正式 Large E2E 全部成功；每次 100 个 1 MiB 文件，共 100 MiB，覆盖 `scan → plan → migrate × 100 → resume × 100 → verify`；平均 3799.98 ms、wall P95/P99 为 3934.93 ms、0.2633 pipelines/s、等效 26.32 MiB/s、CPU 70.20%、peak RSS 8456 KiB。原始报告为 `/tmp/photobridge-e2e-large-local.json`。

实现任务：`L1-M7-Benchmark-8`：将 Large Local E2E 固定为 1 GiB profile，并保留普通小型 e2e profile 不变。

已完成任务：`L1-M7-Benchmark-8`：新增 `e2e-large` workload，强制使用 64 个 16 MiB 文件，精确 1 GiB/iteration；README 和 benchmark 文档已给出 30 次运行命令。

实现任务：`L1-M7-Benchmark-9`：运行 1 GiB Large Local E2E 30 次，并用新结果覆盖此前 100 MiB/3 次的 Large baseline 报告。

已完成任务：`L1-M7-Benchmark-9`：1 GiB Large Local E2E 30 次完成并覆盖旧 JSON；30/30 成功，平均 10988.20 ms/pipeline，wall P95 11138.38 ms、P99 16285.56 ms，0.0915 pipelines/s，等效 93.19 MiB/s，CPU 92.60%，peak RSS 8256 KiB；最终 verify 全部为 IDENTICAL。原始报告为 `/tmp/photobridge-e2e-large-local.json`。

实现任务：`R1-1`：建立 `serve` 启动信息契约，生成当前进程使用的高熵 token，选择默认 LAN 地址并构造带 token 的访问 URL；显式 `--bind`/`--port` 可覆盖地址和端口。

已完成任务：`R1-1`：新增可测试的 `ServeBootstrap`，支持 IPv4/IPv6 URL 格式、端口边界和 token 生成；`photobridge serve` 已接入 CLI 并输出 bind/port/token/url。暂不启动 HTTP 监听，token 只作为本次 bootstrap 结果存在；Linux 编译成功，ServeBootstrap 定向测试 3/3 通过，CLI smoke 成功。

实现任务：`R1-2`：增加自包含的内嵌上传页面响应，包含文件选择、上传按钮和状态区域；页面不引用第三方脚本、样式或远程资源。

已完成任务：`R1-2`：新增 `GetUploadPageResponse()` 及静态 HTML/CSS/JavaScript 页面，固定 `text/html; charset=utf-8` 响应类型；页面资源检查测试 2/2 通过，并纳入 Linux 全量回归。

实现任务：`R1-3`：启动 HTTP 监听并让 `GET /` 返回静态页面，缺失或错误 token 必须拒绝。

已完成任务：`R1-3`：`serve` 默认启动 cpp-httplib 监听，`GET /?t=<token>` 返回自包含页面，错误或缺失 token 返回 401；新增常量时间 token 比较、终端 QR 输出和 `--bootstrap-only` 测试入口。Linux 编译成功，R1-3 定向测试通过。

实现任务：`R3`：将页面接入 session 创建 API，并建立 Receiver 独立 session/file 状态存储。

已完成任务：`R3`：新增独立 `receiver.db`、`upload_session`/`upload_file` 状态表和 `POST /api/v1/sessions`、`GET /api/v1/sessions/<session_id>`；创建时完成文件名安全检查、同 session 冲突后缀和 session/file quota reservation，网络状态不进入 Core 的 `plan_task`。session store 定向测试通过，HTTP 路由已接入。

实现任务：`R4`：实现单文件流式 PUT 上传、临时文件、大小/hash 校验和 no-replace 发布。

已完成任务：`R4`：新增流式 `PUT /api/v1/sessions/<session_id>/files/<file_id>`，通过 ContentReader 分块写入 session `.pbtmp`，处理短写/EINTR、严格大小校验、BLAKE3、可选 client hash、文件 `fsync`、`renameat2(RENAME_NOREPLACE)` 和目标目录 fsync 后才写 RECEIVED。失败保留可诊断状态并不发布半成品；Linux 编译和前序定向测试通过。

已完成任务：`R5`：补充 session 总预留容量限制、文件/路径边界和接收临时文件扫描排除规则；覆盖 traversal、文件 quota、总 quota 以及 `.pbtmp` 不进入 LocalFolderSource 的回归。

实现任务：`R6`：固化同一 `(session_id, file_id)` 的互斥上传和幂等重试语义。

已完成任务：`R6`：数据库状态转换将同一 file_id 串行化为 STARTED/BUSY/ALREADY_RECEIVED；已 RECEIVED 的重试不再读取请求体或生成第二个正式文件，失败状态可重新接收，覆盖状态机定向测试通过。

实现任务：`R7`：完成 batch session API、重试流程和 `.session-complete` 收口。

已完成任务：`R7`：新增 `POST /api/v1/sessions/<session_id>/complete`，只有全部文件为 RECEIVED 才能进入 COMPLETE；页面已接入 session 创建、逐文件 PUT、失败重试和 complete 流程，并保持 token 只经 Authorization 访问 API。session completion 回归通过。

实现任务：`R8`：将浏览器上传调度升级为三通道 mixed-size scheduler，large/small lane 规则固定且不突破服务端上传上限。

已完成任务：`R8`：页面使用 A/B/C 三个异步 lane，large threshold 固定为 256 MiB；A 优先 large，B/C 优先 small，large 同时最多两个，空队列时允许借道。调度逻辑和页面静态契约测试通过。

实现任务：`R9`：为 HTTP Receiver 增加 active-upload connection bound 和共享 Byte Budget，验证资源上限与释放路径。

已完成任务：`R9`：服务端限制 3 个活跃上传处理器、16 MiB 共享 Byte Budget、2 GiB payload 上限和 32 次 keep-alive；每个 ContentReader chunk 只在写入期间持有 byte permit，文件处理使用 RAII fd permit，失败和异常路径均释放资源。

实现任务：`R10`：补齐中断/重启恢复、移动浏览器错误路径和 LAN 端到端回归。

已完成任务：`R10`：Receiver 重启时核对 RECEIVING 的 final/size/hash，能接管已发布 final，未完成 temp 回到可重试 FAILED；页面具备 mobile viewport、可访问状态区和失败提示。真实 HTTP 回归验证 401/200 页面、session 创建、流式上传、同 file_id 幂等重试、complete 和最终文件落盘均成功。

R 支线完成：R1 bootstrap/address/token/QR、R2 static page、R3 session、R4 streaming upload、R5 security/quota、R6 file idempotency、R7 batch retry/complete、R8 three-lane scheduler、R9 Byte Budget/active-upload bound、R10 interruption/mobile regression 均已实现。

约束：输入顺序不影响输出；解析器只产生候选和证据，不替后续 Resolver 做决定；路径仍拒绝逃逸和不安全组件。
