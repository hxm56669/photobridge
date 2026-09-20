# Benchmark results

Each version directory holds JSON reports and matching `.env.txt` machine
snapshots from `benchmarks/scripts/run_benchmark.sh`. These directories are
empty until measurements are run. Choose the label for the source revision
being measured; the script runs the current checkout and records its commit.

- `v1-baseline/`: initial baseline
- `v2-sqlite/`: SQLite changes
- `v3-copy/`: copy changes
- `v4-blake3/`: BLAKE3 changes
- `v5-io_uring/`: io_uring changes
