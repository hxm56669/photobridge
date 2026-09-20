# SQLite optimization comparison

The V1 baseline is `benchmark-results/v1-baseline/micro.json`. Its SQLite
workload, `sqlite_batch_insert`, uses `sqlite3_exec` with one autocommit per
INSERT. The name is retained in new reports for direct comparison.

`--workload sqlite-compare` measures the same 5000 INSERTs in three modes:

| Mode | Report workload | Transaction boundary |
| --- | --- | --- |
| A | `sqlite_batch_insert` | `sqlite3_exec`, one commit per INSERT |
| B | `sqlite_prepared_autocommit` | Reused prepared statement, one commit per INSERT |
| C | `sqlite_prepared_batch_10/100/1000/5000` | Reused prepared statement, one commit per batch |

Build and run on the same Linux host and data filesystem as V1:

```bash
cmake --preset bench-release
cmake --build --preset bench-release -j2
./build/bench-release/photobridge_bench \
  --workload sqlite-compare \
  --repetitions 30 \
  --sqlite-commands 5000 \
  --output benchmark-results/v2-sqlite/sqlite-compare.json

./build/bench-release/photobridge_bench \
  --workload e2e \
  --repetitions 30 \
  --e2e-files 4 \
  --e2e-file-bytes 65536 \
  --output benchmark-results/v2-sqlite/e2e.json
```

The report records wall time, CPU time, throughput and P95/P99 for each mode.
For `fsync`/`fdatasync` call counts, run the same workload separately under
`strace -f -c -e trace=fsync,fdatasync`. For example, run `--workload sqlite`
and `--workload sqlite-batch-5000` with the same `--sqlite-commands 5000` and
`--repetitions 1`. Do not compare the traced wall time with the untraced
report. The existing Plan materialization and Manifest
transactions already batch initialization writes. Claim, Attempt,
VerifiedReceipt and Recovery commits keep their original durable boundaries.

## Results on the Ubuntu benchmark host

Release build on `hxmserver`, 5000 INSERTs, 30 repetitions. The V1 archived
report is `benchmark-results/v1-baseline/micro.json`; the V2 raw report is
`benchmark-results/v2-sqlite/sqlite-compare.json`.

| Mode | Mean wall time / 5000 rows | Mean CPU time | `fsync` calls / 5000 rows |
| --- | ---: | ---: | ---: |
| V1 archived A | 4032.45 ms | 978.92 ms | about 5023 (V1 strace, 10 reps) |
| V2 run A: exec + autocommit | 2960.49 ms | 755.92 ms | 5025 |
| V2 run B: prepared + autocommit | 2696.77 ms | 656.14 ms | 5025 |
| V2 run C: prepared + batch 10 | 731.01 ms | 130.05 ms | — |
| V2 run C: prepared + batch 100 | 61.70 ms | 12.40 ms | 60 |
| V2 run C: prepared + batch 1000 | 10.52 ms | 3.62 ms | — |
| V2 run C: prepared + batch 5000 | 5.98 ms | 4.45 ms | 11 |

B is 8.9% faster than A in the same V2 run. The large C gains come mainly
from fewer durable commits. They describe the synthetic benchmark; production
Plan and Manifest initialization already use batch transactions. The V1
archived A result differs substantially from the unchanged A mode run now, so
cross-run timing must be interpreted with that variation in mind. The strace
counts are separate one-repetition runs; its timing is excluded from the table.

E2E results (mean wall time):

| Source and run | Four 64 KiB files, 30 reps | 64 × 16 MiB, 10 reps |
| --- | ---: | ---: |
| V1 archived report | 144.65 ms | 13.80 s |
| `perf-v1-baseline` tag rebuilt and rerun on the same host | 119.98 ms | 24.15 s |
| V2 working tree | 104.17 ms | 17.26 s |

The rebuilt V1 large-file result changed markedly from the archived V1 result
without any source changes. This run does not establish an attributable
large-file E2E speedup. Raw V1 control files are stored alongside the V2 E2E
reports as `v1-control-e2e*.json`.
