# PhotoBridge benchmark

`photobridge_bench` 是独立的 Linux Release benchmark，不属于 GoogleTest。
它测量三条真实链路：

- `scanner_manifest`：固定数量文件的 dirfd 扫描、分类、SQLite Manifest 批量写入和冻结。
- `copy_hash_fdatasync`：真实文件的流式 CopyAndHash、BLAKE3 和目标 `fdatasync`。
- `sqlite_writer_ack`：单写者 `DbCommandQueue` 的固定批量 SQLite INSERT 与 ACK。
- `e2e_local_pipeline`：通过现有 CLI 真实执行 `scan → plan → migrate → resume → verify`；多文件计划会按当前单 task CLI 契约逐个 migrate/resume。
- `e2e_large_local_pipeline`：固定 1 GiB 本地端到端 profile，使用 64 个 16 MiB 文件。

每次测量记录 wall time、进程 CPU time、CPU 利用率、峰值 RSS、`/proc/self/io` 的读写字节和系统调用计数。报告计算 min/mean/median/P95/P99/max，并按 workload 的 items 或 bytes 计算吞吐率。

## Build and run

```bash
cmake --preset bench-release
cmake --build --preset bench-release -j2
./build/bench-release/photobridge_bench \
  --repetitions 5 \
  --scanner-files 100000 \
  --copy-bytes 67108864 \
  --sqlite-commands 5000 \
  --output benchmark-results.json
```

`bench-release` 使用 `build/bench-release`，固定为 Release，并且 build preset 只构建 `photobridge_bench`。可执行文件路径应为 `./build/bench-release/photobridge_bench`。

默认数据目录为系统临时目录下的 `photobridge_bench_data`。使用 `--data-root` 可把 fixture 和数据库放到指定 Linux/SMB 路径；程序只清理该目录下自己创建的 `run/` 子目录。`--keep-data` 可保留 fixture 供复查。

P95/P99 是重复样本的 wall-time 和吞吐率分位数；当重复次数较少时，P99 会退化为最大样本，正式比较应固定参数并使用至少 10 次重复。

## Local end-to-end

```bash
./build/bench-release/photobridge_bench \
  --workload e2e \
  --repetitions 30 \
  --e2e-files 4 \
  --e2e-file-bytes 65536 \
  --output e2e-local-results.json
```

每次迭代使用独立 workspace 和 target；源 fixture 在计时前创建，workspace 初始化也在计时前完成。端到端 wall time 包含 scan、plan、每个 task 的 migrate/resume、最终 verify，以及这些 CLI 阶段各自的 SQLite、临时文件、receipt、rename、目录同步和独立校验开销。

## Large Local end-to-end (1 GiB)

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 30 \
  --output /tmp/photobridge-e2e-large-local.json
```

`e2e-large` 会覆盖命令行传入的 `--e2e-files`/`--e2e-file-bytes`，固定为 64 × 16 MiB = 1 GiB；如果只想先做短验证，可降低 `--repetitions`，但总数据量仍保持 1 GiB/iteration。
