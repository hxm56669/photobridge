# PhotoBridge

PhotoBridge 是一个 Linux-first 的照片迁移原型：先生成不可变 Frozen Plan，再按 task/attempt 执行可恢复提交。

## 快速流程

在 Linux VM 中构建和测试：

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j2
ctest --test-dir build/debug --output-on-failure
```

典型 CLI 流程：

```sh
photobridge init --workspace /path/to/workspace
photobridge scan --workspace /path/to/workspace --source /path/to/source
photobridge plan --workspace /path/to/workspace --manifest <manifest-id> --target /path/to/target
photobridge migrate --workspace /path/to/workspace --plan /path/to/plan.jsonl
photobridge resume --workspace /path/to/workspace --plan /path/to/plan.jsonl
photobridge verify --workspace /path/to/workspace --plan /path/to/plan.jsonl
```

## 提交顺序

执行阶段按以下顺序建立恢复证据：

1. claim task 并持久化 attempt；
2. 流式 copy/hash 到 `O_EXCL` temp；
3. `fdatasync(temp)`，独立读回 temp 并验证 size/hash；
4. DB 事务持久化 `verified_receipt`，等待 `VERIFIED_DURABLE` ACK；
5. `renameat2(RENAME_NOREPLACE)` 发布，随后 fsync 目标目录；
6. 以 owner epoch/attempt fencing 写入成功状态。

目标已存在、receipt 缺失或 hash 不匹配时不会覆盖目标；恢复只采纳有可信 receipt 且独立验证匹配的文件。workspace/SQLite/locks 应放在 Linux 本地文件系统，照片 target 可在支持同步语义的挂载点上。

设计过程记录在 [`docs/state.md`](docs/state.md)、[`docs/DECISIONS.md`](docs/DECISIONS.md) 和蓝图文档中。
