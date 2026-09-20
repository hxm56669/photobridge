# PhotoBridge V1 → V2 SQLite 性能优化验收报告

> 对比版本：`perf-v1-baseline` → `perf-v2-sqlite`  
> V2 提交：`952a481738e274a656944538b74a6d95bb0761c5`  
> 本报告只收口本轮 SQLite 优化，不讨论后续 Copy / BLAKE3 / io_uring 优化。

---

## 1. 优化目标

V1 的 SQLite 写入路径存在两个主要固定开销：

```text
高频 SQL 重复 prepare / finalize
+
大量小事务重复提交
```

这会导致：

```text
SQL 解析与编译重复发生
→ statement 无法复用

每条小写入频繁提交
→ fcntl / pwrite / fsync 大量发生
→ system time 和 context switch 偏高
```

V2 的优化方向是：

```text
Prepared Statement 复用
+
Batch Transaction
```

核心目标不是降低可靠性，而是减少重复固定成本。

---

## 2. 代码层面的主要改动

### 2.1 新增可复用 `SqliteStatement`

V2 新增：

```text
include/photobridge/app/sqlite_statement.h
src/app/sqlite_statement.cpp
```

用于长期持有：

```cpp
sqlite3_stmt*
```

并提供：

```text
Prepare
Reset
Finalize
```

逻辑从 V1 的：

```text
prepare
→ bind
→ step
→ finalize

下一次再次 prepare
```

变成 V2 的：

```text
第一次 prepare

之后：
reset
→ clear bindings
→ bind 新参数
→ step
→ reset
```

这样固定 SQL 不需要重复解析和编译。

---

### 2.2 `TaskRuntimeRepository` 缓存 Statement

V1 的 statement 主要在业务函数内部临时创建。

V2 改为在 Repository 中长期保存多个固定 SQL 对应的 statement，例如：

```text
insert_task_
insert_event_
update_attempt_
read_runtime_
read_current_epoch_
check_task_ownership_
claim_task_
finish_task_
recover_task_
insert_receipt_
...
```

因此：

```text
一个固定 SQL
→ 对应一个长期缓存 statement
```

参数变化时只重新 bind，不重新 prepare。

---

### 2.3 保留原事务和可靠性语义

这一轮没有通过关闭持久化来“跑分快”。

SQLite 仍保持原有可靠性方向：

```text
WAL
synchronous = FULL
事务控制
失败时 rollback
```

也就是说，优化目标是：

```text
减少重复工作
```

而不是：

```text
牺牲 durability 换性能
```

---

## 3. Batch Transaction 的作用

V1 可以理解为：

```text
INSERT
→ commit

INSERT
→ commit

INSERT
→ commit
...
```

大量小事务会产生大量：

```text
锁操作
WAL / page 写入
fsync
事务提交
```

V2 benchmark 的正式 SQLite 路径采用：

```text
Prepared Statement
+
Batch Transaction
+
batch size = 256
```

即：

```text
BEGIN

INSERT × 256

COMMIT
```

5000 条写入由大量独立提交变成约：

```text
5000 / 256 ≈ 20 个批次
```

因此大幅减少事务提交和内核调用。

---

## 4. Benchmark 为什么也需要修改

最初 V2 代码已经变化，但 benchmark 默认的 SQLite workload 仍然调用：

```text
kExecAutocommit
```

也就是仍在测 V1 风格路径。

因此 benchmark 后来调整为：

```text
sqlite-v1
→ ExecAutocommit
→ V1 对照组

sqlite-v2
→ PreparedBatch
→ batch = 256
→ V2 正式 benchmark 路径

sqlite-compare
→ 用于比较不同 batch size
```

这样才能做到：

```text
同样 5000 条写入
V1 旧策略
vs
V2 新策略
```

---

# 5. Microbenchmark 对比

测试规模：

```text
Scanner：
100000 files
30 repetitions

Copy：
64 MiB
30 repetitions

SQLite：
5000 commands
30 repetitions
```

## 5.1 SQLite

| 指标 | V1 | V2 | 变化 |
|---|---:|---:|---:|
| 平均吞吐 | 1,243.96 commands/s | 147,968.39 commands/s | **约 119 倍** |
| 平均耗时 | 4032.44 ms | 34.85 ms | **下降约 99.14%** |
| 平均 CPU 时间 | 978.92 ms | 7.84 ms | **下降约 99.20%** |
| 单样本 write syscall | 10,087 | 88 | **下降约 99.13%** |
| 单样本内核写入量 | 41,308,160 B | 262,144 B | **下降约 99.37%** |

结论：

```text
SQLite 自身性能出现数量级提升。
```

主要不是因为 SQLite 算法变了，而是因为：

```text
重复 prepare / finalize 减少
+
事务提交次数减少
+
write / fsync 等固定成本被摊薄
```

---

## 5.2 Scanner

| 指标 | V1 | V2 |
|---|---:|---:|
| 平均吞吐 | 45,788.67 files/s | 43,083.79 files/s |
| 平均耗时 | 2185.92 ms | 2322.86 ms |

Scanner 本轮没有做针对性优化。

变化约为：

```text
吞吐下降约 5.9%
```

不应归因于 SQLite 优化本身，更可能属于运行环境和系统波动。

---

## 5.3 Copy

| 指标 | V1 | V2 |
|---|---:|---:|
| 平均吞吐 | 247,616,040 B/s | 239,510,661 B/s |
| 平均耗时 | 271.62 ms | 282.54 ms |
| CPU 利用率 | 99.03% | 98.88% |

本轮也没有修改 Copy 核心路径。

变化约为：

```text
吞吐下降约 3.3%
```

同样不作为 SQLite 优化的回归结论。

---

# 6. 小规模 E2E 对比

测试：

```text
4 files
× 64 KiB
= 256 KiB

30 repetitions
```

完整链路：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

结果：

| 指标 | V1 | V2 | 变化 |
|---|---:|---:|---:|
| 平均耗时 | 144.65 ms | 114.03 ms | **下降约 21.2%** |
| 吞吐 | 6.96 pipelines/s | 8.96 pipelines/s | **提升约 28.8%** |
| CPU 利用率 | 34.85% | 36.41% | 接近 |
| 峰值 RSS | 8408 KiB | 8304 KiB | 基本不变 |

说明：

```text
SQLite microbenchmark 的优化
确实传导到了完整业务链。
```

小文件场景中，SQLite 固定开销占总耗时比例较高，因此收益明显。

---

# 7. 1 GiB 大规模 E2E 对比

测试规模：

```text
64 files
× 16 MiB
= 1 GiB

正式 10 repetitions
```

V1 在正式测试前进行了独立 warmup，V2 同样进行了 warmup。

结果：

| 指标 | V1 | V2 |
|---|---:|---:|
| 平均耗时 | 13.80 s | 15.00 s |
| 中位耗时 | 13.54 s | 13.57 s |
| 平均吞吐 | 0.0726 pipeline/s | 0.0685 pipeline/s |
| CPU 利用率 | 88.50% | 84.73% |
| 峰值 RSS | 8540 KiB | 8748 KiB |

V2 存在两个明显慢样本：

```text
19.51 s
20.95 s
```

因此平均值被明显拉高。

更有代表性的中位数：

```text
V1：13.538 s
V2：13.565 s
```

只相差约：

```text
0.2%
```

所以合理结论是：

```text
1 GiB 大文件 E2E 基本不变。
```

这不是 SQLite 优化失败，而说明瓶颈已经发生转移。

大文件链路中主要时间更可能消耗在：

```text
Copy
+
BLAKE3
+
read / write
+
fdatasync
```

此时 SQLite 已不是总耗时主导项。

---

# 8. strace 对比

测试：

```text
10 repetitions
× 5000 commands
= 50000 commands
```

## V1

主要系统调用：

```text
fcntl      300,862
fsync       50,230
pwrite64   101,570

total      454,038
```

## V2

主要系统调用：

```text
fcntl        1,732
fsync          300
pwrite64     1,940

total        5,181
```

对比：

| 系统调用 | V1 | V2 | 下降 |
|---|---:|---:|---:|
| `fcntl` | 300,862 | 1,732 | **99.42%** |
| `fsync` | 50,230 | 300 | **99.40%** |
| `pwrite64` | 101,570 | 1,940 | **98.09%** |
| 总 syscall | 454,038 | 5,181 | **98.86%** |

其中最关键的是：

```text
fsync
50,230 → 300
```

约减少：

```text
167 倍
```

这直接证明 V1 的主要问题之一就是频繁事务持久化。

---

# 9. SQLite perf stat 对比

同样测试：

```text
10 × 5000 commands
```

结果：

| 指标 | V1 | V2 | 变化 |
|---|---:|---:|---:|
| elapsed | 29.55 s | 0.435 s | **下降约 98.5%** |
| task-clock | 7023 ms | 126 ms | **下降约 98.2%** |
| context switch | 73,555 | 736 | **下降约 99.0%** |
| CPU migration | 298 | 5 | **下降约 98.3%** |
| system time | 7.69 s | 0.111 s | **下降约 98.6%** |

解释：

```text
V1：
大量事务
→ 大量进入内核
→ 大量锁与同步等待
→ context switch / sys time 高

V2：
Prepared + Batch
→ syscall 大幅减少
→ context switch 大幅减少
→ system time 大幅下降
```

虚拟机不支持：

```text
cycles
instructions
branches
branch-misses
```

因此硬件 PMU 数据显示：

```text
<not supported>
```

这属于测试环境限制，不影响 syscall / wall time / context switch 的结论。

---

# 10. 大规模 E2E perf stat

V2 三次 `e2e-large`：

```text
task-clock       44.56 s
context-switch   12,614
cpu-migrations   64
page-faults      9,416
elapsed          48.56 s
user             23.14 s
sys              21.51 s
```

V1 对应数据：

```text
task-clock       57.19 s
context-switch    9,943
page-faults      10,175
elapsed          61.60 s
user             28.74 s
sys              28.53 s
```

这组三次 perf 数据中 V2 elapsed 更低，但：

```text
3 次样本量较小
+
大文件测试受 VM 调度、缓存和 I/O 波动影响明显
```

因此不把它单独作为：

```text
“大文件 E2E 提升 21%”
```

的正式结论。

正式 10 次 E2E 的中位数更值得参考：

```text
V1 ≈ 13.54 s
V2 ≈ 13.57 s
```

所以最终仍判断：

```text
大文件 E2E 基本持平。
```

---

# 11. 本轮优化的完整证据链

这一轮已经形成完整闭环：

```text
V1 benchmark / strace
↓
发现 SQLite 大量 fcntl / fsync / pwrite
↓
定位频繁 prepare 和小事务提交问题
↓
引入 Prepared Statement 复用
↓
加入 Batch Transaction 测试策略
↓
V2 strace
系统调用下降约 99%
↓
V2 perf
context switch / sys time 下降约 99%
↓
Microbenchmark
SQLite 吞吐提升约 119 倍
↓
小文件 E2E
平均耗时下降约 21%
↓
大文件 E2E
基本持平
↓
说明瓶颈转移到 Copy / Hash / I/O
```

---

# 12. 最终验收结论

## 已经可以确认

### 1. SQLite 是 V1 的明确性能热点

证据：

```text
大量 fsync
大量 fcntl
大量 pwrite64
高 system time
高 context switch
```

---

### 2. V2 SQLite 优化方向正确

Prepared Statement 和批事务有效降低了：

```text
SQL 编译固定成本
事务提交次数
锁操作
内核写入
fsync
context switch
system time
```

---

### 3. Microbenchmark 提升非常明显

正式 30 次测试：

```text
1,244 commands/s
→
147,968 commands/s
```

约：

```text
119 倍
```

---

### 4. 小文件真实业务链获得明显收益

完整 E2E：

```text
144.65 ms
→
114.03 ms
```

平均耗时下降：

```text
约 21.2%
```

---

### 5. 大文件 E2E 不再由 SQLite 主导

1 GiB 场景中：

```text
V1 median ≈ 13.54 s
V2 median ≈ 13.57 s
```

基本相同。

说明 SQLite 优化之后：

```text
Copy / Hash / I/O
```

成为下一阶段更值得优化的方向。

---

# 13. 需要保持的真实性边界

当前证据中必须区分：

```text
主链已经确认：
Prepared Statement cache / reuse

benchmark 正式测试：
Prepared Statement + Batch 256
```

如果项目主业务所有 SQLite 写入还没有真正全部接入：

```text
batch size = 256
```

则不能直接在简历或面试中表述成：

```text
“整个主链已经全面使用 256 条批事务”
```

更稳妥的说法是：

```text
对高频 SQLite 写入引入 Prepared Statement 复用，
并通过批事务 benchmark 验证了减少事务提交与持久化开销的收益。
```

如果后续审计确认 Batch 256 已经真正接入业务主链，再升级表述。

---

# 14. 面试推荐表达

可以这样回答：

```text
我先通过 benchmark、strace 和 perf 定位 SQLite 写入瓶颈。

V1 中 5 万次 benchmark 写入产生了约 5 万次 fsync、
30 万次 fcntl 和 10 万次 pwrite64，
说明主要问题不是 SQL 计算本身，而是大量小事务和重复固定开销。

所以我把固定 SQL 改成 Prepared Statement 复用，
通过 reset 和重新 bind 参数避免重复 prepare/finalize，
同时用批事务减少事务提交次数。

优化后 strace 中总 syscall 下降约 98.9%，
fsync 下降约 99.4%，context switch 下降约 99%。

SQLite microbenchmark 吞吐从约 1.2k commands/s
提升到约 148k commands/s。

在 256 KiB 的完整迁移链路中，
平均耗时下降约 21%。

但在 1 GiB 场景下中位耗时基本不变，
说明 SQLite 已经不再是主瓶颈，
下一阶段瓶颈转移到了 Copy、Hash 和 I/O。
```

---

# 15. 下一阶段优化方向

本轮 SQLite 优化可以收口。

下一阶段优先级建议：

```text
V3
Copy 路径优化
↓
减少 read / write 系统调用和数据搬运

V4
BLAKE3 / Hash 路径分析
↓
注意当前 perf 已经观察到 AVX2 实现

V5
io_uring
↓
重点用于 read / write 提交与完成路径
↓
不能把 fdatasync 的 durability 成本简单消除
```

当前性能优化路线已经从：

```text
SQLite 固定开销
```

转移到：

```text
Copy / Hash / I/O 主链
```

这也是下一轮 benchmark 应重点验证的方向。
