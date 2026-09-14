# PhotoBridge

PhotoBridge 是一个 Linux-first 的照片迁移原型：先生成不可变 Frozen Plan，再按 task/attempt 执行可恢复提交。

## 快速流程

在 Linux VM 中构建和测试：

```sh
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug
```

典型 CLI 流程：

```sh
photobridge init --workspace /path/to/workspace
photobridge scan --workspace /path/to/workspace --source /path/to/source
photobridge plan --workspace /path/to/workspace --manifest <manifest-id> --target /path/to/target
photobridge migrate --workspace /path/to/workspace --plan /path/to/plan.jsonl
photobridge status --workspace /path/to/workspace --plan /path/to/plan.jsonl
photobridge resume --workspace /path/to/workspace --plan /path/to/plan.jsonl
photobridge verify --workspace /path/to/workspace --plan /path/to/plan.jsonl
```

V1 每次 `migrate` 领取一个 READY task，写入并独立验证 temp，在 SQLite 中持久化 `VERIFIED_DURABLE` receipt 后保持 task 为 RUNNING；`resume` 根据 receipt 重新观察文件系统并发布或恢复该 task。多资产计划按 `status` 结果重复执行 `migrate`/`resume`，直到全部完成，再执行 `verify`。

## 提交顺序

唯一生产主链按以下顺序建立恢复证据：

1. `migrate` 获取新的 execution epoch，claim task 并持久化 RUNNING attempt；
2. attempt 依次进入 `COMMIT_INTENT`，流式 copy/hash 到 `O_EXCL` temp；
3. `fdatasync(temp)` 后进入 `TEMP_WRITTEN`，再独立 reopen temp 并验证 size/hash；
4. DB 事务持久化 `verified_receipt` 与 `VERIFIED_DURABLE` 状态；
5. `resume` 在新 recovery epoch 下重新核对 Frozen Source、receipt、temp 与 final；
6. 仅对匹配证据执行 `renameat2(RENAME_NOREPLACE)`，随后 fsync 目标目录并以 epoch/attempt fencing 写入 `COMMITTED`/SUCCEEDED。

目标已存在且内容不匹配、receipt 缺失或 hash 不匹配时不会覆盖目标；无 receipt 的 temp 不会被自动发布或清理；Frozen Source 被替换后 task 进入 INCONSISTENT，不会把新文件当作原文件重试。恢复只采纳有可信 receipt 且独立验证匹配的文件。workspace/SQLite/locks 应放在 Linux 本地文件系统，照片 target 可在支持同步语义的挂载点上。

## 封版验证

```sh
cmake --preset sanitized
cmake --build --preset sanitized -j2
ctest --preset sanitized

cmake --preset bench-release
cmake --build --preset bench-release -j2
./build/bench-release/photobridge_bench \
  --workload e2e \
  --repetitions 5 \
  --e2e-files 4 \
  --e2e-file-bytes 65536 \
  --output benchmark-results.json
```

测试包含进程级 SIGKILL→resume→verify 场景，覆盖 partial temp/no receipt、durable receipt+temp、rename 后 DB 仍为 RUNNING、final mismatch 和 Frozen Source replacement。

设计过程记录在 [`docs/state.md`](docs/state.md)、[`docs/DECISIONS.md`](docs/DECISIONS.md) 和蓝图文档中。
