# PhotoBridge V5：io_uring 性能优化、验证与瓶颈分析完整报告

> 项目：PhotoBridge  
> 优化阶段：V5 / 第四项性能优化 —— `io_uring`  
> Benchmark 对应提交：`c0563709e56335c6fb4cf6ee66369aa9af0968d7`  
> 测试时间：2026-09-20  
> 报告目的：记录代码设计、改造逻辑、Benchmark 方法、QD 对比、1 GiB 长尾、E2E A/B、`perf`、`strace`、SQLite 锁竞争证据，以及面试可直接使用的结论。  
>
> **版本说明：** 本报告中的性能数字以以上 Benchmark 提交和当时测试环境为准。后续 `main` 分支如果继续修改实现，不应直接拿本报告数字代表新的代码版本，除非重新跑同口径测试。

---

# 1. 一句话结论

V5 将 PhotoBridge 迁移主链中的第一遍 `Copy + BLAKE3 Hash` 从同步：

```text
read
→ hash
→ write
→ read
→ hash
→ write
```

改造成基于 `io_uring` 的有限深度流水：

```text
多个 read 在途
→ 按顺序 hash
→ 多个 write 提交
→ completion 驱动 slot 循环复用
```

在 **64 MiB Copy+Hash 微基准**中：

```text
同进程对照：
sync：248.95 MB/s
QD4 ：314.97 MB/s
QD8 ：306.01 MB/s

QD4 相比 sync：
吞吐约 +26.5%
平均 wall time 约 -19.1%
```

但在 **1 GiB、4 Worker、完整 `scan → plan → migrate → resume → verify` 主链**中：

```text
sync-control：
平均 8717.34 ms
0.11472 pipelines/s

io_uring：
平均 8505.58 ms
0.11759 pipelines/s

最终：
平均 wall time 约 -2.43%
端到端吞吐约 +2.50%
```

因此 V5 的真实结论不是：

> “用了 io_uring，整个项目快了 30%。”

而是：

> **io_uring 对被它直接优化的 Copy+Hash 数据面有效，局部能获得约 20%～30% 的收益；但完整迁移链还包含独立校验、SQLite 状态事务、`fdatasync/fsync`、扫描、计划与恢复等步骤，因此端到端收益被稀释到约 2.5%。**

同时，这不是“免费优化”：

```text
更高 I/O 并发
→ 更高 CPU 并发
→ 更多 buffer
→ 更多上下文切换 / 系统态工作
→ 更高 RSS
→ 换取更低 wall time
```

---

# 2. V5 要解决的原始问题

## 2.1 V4 后的数据路径

V4 完成 BLAKE3 SIMD 路径确认之后，核心 Copy 路径仍然是同步串行模型：

```text
source fd
↓
read 1 MiB
↓
BLAKE3 Update
↓
write / short-write retry
↓
下一块
```

伪代码：

```cpp
while (true) {
    n = Read(source_fd, buffer);

    if (n == 0) {
        break;
    }

    hasher.Update(buffer[0:n]);

    WriteAll(target_fd, buffer[0:n]);
}
```

这个模型的问题不是“同步 read/write 一定慢”，而是：

```text
等待 read 时
CPU 不能处理下一块

hash 当前块时
下一块 read 没有提前准备

等待 write 时
下一块 read 也没有真正形成 I/O 流水
```

因此三个阶段在单任务内部主要是串行的：

```text
Read A → Hash A → Write A
                     ↓
Read B → Hash B → Write B
```

---

# 3. 为什么选择 io_uring

`io_uring` 的核心价值不是“系统调用名字更高级”，而是：

> **允许用户态准备多个 I/O 请求，通过提交队列和完成队列管理多个在途请求，从而让 I/O 等待和 CPU 计算更容易发生重叠。**

在 PhotoBridge 中最适合改造的是高频数据面：

```text
read
write
```

因为大文件 Copy 会重复执行成百上千次。

例如 1 GiB、1 MiB chunk：

```text
约 1024 个数据块
→ 传统同步版本约 1024 次 read
→ 约 1024 次 write
```

而：

```text
open
stat
rename
fsync directory
```

属于低频控制面，不是第一优先级。

---

# 4. V5 生产主链改造

V5 不是只在 Benchmark 里增加一个 `io_uring` Demo，而是接入真实迁移主链。

核心位置：

```text
MigrationAttemptPreparer::Prepare()
```

主链变成：

```text
MarkCommitIntent
↓
CreateTempNoReplace
↓
VerifyBeforeRead
↓
TryIoUringCopyAndHash
├─ io_uring 可用 → io_uring Copy + BLAKE3
└─ io_uring 初始化阶段不可用 → 同步 CopyAndHash fallback
↓
fdatasync(temp)
↓
MarkTempWritten
↓
重新打开 temp
↓
VerifyBinary
↓
PersistVerifiedReceipt
```

这里有一个非常重要的设计原则：

> **V5 只替换数据搬运实现，不破坏原来的 crash-safe 持久化顺序。**

所以这些边界没有被删除：

```text
Copy 完成
↓
fdatasync(temp)
↓
数据库记录 temp 已经 durable
↓
独立 Verify
↓
持久化 receipt
```

---

# 5. 核心接口设计

Benchmark 提交中的核心接口：

```cpp
StatusOr<std::optional<CopyResult>> TryIoUringCopyAndHash(
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::uint64_t source_size,
    std::size_t chunk_size = 1024U * 1024U,
    std::size_t queue_depth = 4);
```

默认：

```text
chunk_size  = 1 MiB
queue_depth = 4
```

QD 最大限制：

```text
1 ≤ queue_depth ≤ 16
```

返回类型使用：

```text
StatusOr<std::optional<CopyResult>>
```

三个语义分别是：

```text
有 CopyResult
→ io_uring 成功完成

nullopt
→ io_uring 在任何数据 I/O 开始前不可用
→ 上层允许安全 fallback 到同步 CopyAndHash

Status error
→ 已经进入 io_uring 数据路径后发生真实错误
→ 直接返回错误
→ 不重新从头同步复制
```

这样做避免了：

```text
io_uring 已经写了一部分
→ 出错
→ 上层不知道写到哪里
→ 又从头同步 Copy
```

这种容易破坏错误语义的行为。

---

# 6. io_uring 不可用时的 fallback

V5 把“平台不支持”和“执行中 I/O 错误”分开。

初始化阶段常见不可用条件：

```text
ENOSYS
EPERM
EOPNOTSUPP
EINVAL
```

如果 ring 在任何实际数据 I/O 提交前就无法建立：

```text
TryIoUringCopyAndHash
→ nullopt
→ MigrationAttemptPreparer
→ CopyAndHash(sync)
```

这意味着：

```text
Linux 支持 io_uring
→ 使用优化路径

受限容器 / 老内核 / seccomp / 某些环境不支持
→ 自动保留原同步实现
```

这是工程兼容性，而不是 Benchmark 专用代码。

---

# 7. 同版本同步对照开关

为了真正比较 V5 自身，而不是：

```text
旧 commit
vs
新 commit
```

项目提供了：

```bash
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1
```

它让：

```text
TryIoUringCopyAndHash
→ 在 I/O 开始前报告 unavailable
→ 走原同步 fallback
```

因此正式 E2E A/B 能做到：

```text
同一个二进制
同一份代码
同样数据
同样 Worker 数
同样迁移协议
唯一主要变量：
io_uring 是否启用
```

这是本次 E2E 数据可信度最高的一组对照。

---

# 8. Buffer Slot 状态机

每个 in-flight slot 大致保存：

```text
buffer
sequence
offset
length
progress
phase
```

状态：

```text
Free
↓
Reading
↓
Ready
↓
Writing
↓
Free
```

其中：

## 8.1 Free

slot 当前没有任务，可以提交新的 read。

## 8.2 Reading

对应块的 read 已提交，正在等待完成。

## 8.3 Ready

read 已完成，buffer 中的数据可以参与 BLAKE3。

## 8.4 Writing

hash 完成后，对应数据正在提交/等待 write completion。

写完后重新回到：

```text
Free
```

再处理后面的 chunk。

---

# 9. 为什么 Hash 仍然必须保持顺序

多个 read 可以同时 in-flight：

```text
read block 0
read block 1
read block 2
read block 3
```

但文件 Hash 不能乱序：

```text
Hash(block 2)
→ Hash(block 0)
→ Hash(block 1)
```

这样会得到错误 digest。

因此 V5 使用 sequence / `next_hash` 一类顺序约束：

```text
Read 可以乱序完成
↓
Ready slot 等待
↓
Hash 必须按照 block 0、1、2、3...
↓
再提交对应 write
```

所以更准确的说法是：

> **V5 允许多个数据 I/O 请求在途，并让 I/O 与用户态 BLAKE3 计算出现重叠；但 BLAKE3 本身仍然按照文件字节顺序推进。**

不能夸大成：

> “read、hash、write 三者全部无序全并行。”

---

# 10. 短读短写处理

同步路径原本已经正确处理 short write。

V5 不能因为换成 `io_uring` 就假设：

```text
请求 1 MiB
→ completion 一定等于 1 MiB
```

因此 slot 中保留：

```text
length
progress
```

如果：

```text
result < remaining
```

则：

```text
更新 progress
↓
对剩余部分重新提交
```

直到当前块全部读/写完成。

这保证：

```text
异步 ≠ 放弃 POSIX I/O 边界处理
```

---

# 11. Ring 生命周期设计

V5 使用 Worker 线程内缓存的 ring。

逻辑上：

```text
Worker Thread
↓
thread_local IoUringContext
↓
第一次 Copy 初始化
↓
后续同 Worker 上继续复用
```

这样避免每个文件都：

```text
io_uring_queue_init
→ Copy 一个文件
→ io_uring_queue_exit
```

Ring entries 至少：

```text
max(8, queue_depth)
```

因此默认 QD4 不会只创建 4 个 ring entry。

---

# 12. QD 的含义

QD = Queue Depth。

这里可以理解为：

> **单个 Copy 任务允许同时管理多少个 I/O slot / 在途数据块。**

例如：

```text
chunk = 1 MiB
QD = 4
```

大致意味着：

```text
最多约 4 个 1 MiB buffer slot
```

每个 Worker 的数据 buffer 内存大致随：

```text
QD × chunk_size
```

增长。

如果：

```text
4 Worker × QD4 × 1 MiB
```

仅 io_uring Copy slot 的理论数据 buffer 量就可能来到：

```text
约 16 MiB
```

再加：

```text
Worker 原有 buffer
SQLite
程序堆
页表
ring
其他对象
```

所以 E2E RSS 增长是预期现象。

---

# 13. 为什么不直接把所有操作都改成 io_uring

## 13.1 read/write

这是高频数据面：

```text
1 GiB / 1 MiB
≈ 1024 chunks
```

收益最大。

## 13.2 fdatasync

`fdatasync(temp)` 是持久化协议边界：

```text
写完 temp
↓
fdatasync 成功
↓
才能认为 temp 内容 durable
↓
才能 MarkTempWritten
```

不是简单把它“异步化”就能消除成本。

如果要跨越这个 barrier 并发，需要重新证明 crash-safe 顺序。

## 13.3 fsync directory

这是 rename 后目录项持久化语义的一部分，同样属于 correctness barrier。

## 13.4 SQLite

SQLite 的瓶颈是：

```text
多连接
+
BEGIN IMMEDIATE
+
WAL 单 Writer
+
状态事务频繁
```

不是把某个 `write()` 换成 `io_uring` 就能解决。

## 13.5 VerifyBinary

V5 保留独立同步 Verify：

```text
Copy 时 Hash
≠
迁移完成后的独立重新读取验证
```

它的价值是：

> **验证不完全信任刚才 Copy 流程里看到的数据。**

如果未来优化 Verify，也应该单独设计和 Benchmark，而不是为了数字直接删除第二遍读取。

---

# 14. 与前三项优化的关系

## 14.1 与 SQLite 优化

耦合较弱。

```text
io_uring
→ 文件数据面

SQLite 优化
→ 状态持久化控制面
```

但两者都会影响 E2E。

## 14.2 与 Multi-worker

耦合很强。

Multi-worker 提供：

```text
Task 级并行
```

io_uring 提供：

```text
单 Task 内 I/O pipeline
```

最终并发强度近似来自：

```text
Worker 数 × 每个 Worker 的 QD
```

因此：

```text
Worker 越多
+
QD 越深
≠ 一定越快
```

可能出现：

```text
I/O 队列过深
CPU 争用
更多上下文切换
更高内存
SQLite 锁竞争
```

## 14.3 与 BLAKE3 SIMD

BLAKE3 SIMD 降低单块 Hash 成本。

io_uring 试图让：

```text
I/O 等待
与
Hash CPU 工作
```

更好地重叠。

`perf record` 中同时看到：

```text
blake3_hash_many_avx2
io_uring_submit
iou-wrk-*
```

说明 V4 与 V5 确实同时在实际运行路径工作。

---

# 15. Benchmark 环境

本次 V5 数据固定在：

| 项目 | 环境 |
|---|---|
| Git commit | `c0563709e56335c6fb4cf6ee66369aa9af0968d7` |
| OS | Linux |
| Kernel | `5.15.0-185-generic` |
| Arch | `x86_64` |
| 虚拟化 | VMware full virtualization |
| CPU | 13th Gen Intel Core i7-13700F |
| VM 可见 CPU | 8 |
| 拓扑 | 4 cores × 2 sockets × 1 thread/core |
| 内存 | 约 7.7 GiB |
| 文件系统 | ext4 |
| 磁盘 | `/dev/sda2`，约 100 GiB |
| GCC | 11.4.0 |
| CMake | 3.22.1 |
| io_uring 内核配置 | `CONFIG_IO_URING=y` |
| io-wq | `CONFIG_IO_WQ=y` |

注意：

> 这是虚拟机、本地 ext4、明显受 page cache 与虚拟磁盘 writeback 影响的环境。不能把结果直接外推到 NVMe、SMB、NAS 或手机网络迁移。

---

# 16. 环境检查命令

```bash
git rev-parse HEAD
uname -r
uname -m
lscpu
free -h
df -hT .
gcc --version
cmake --version
grep -E 'CONFIG_IO_URING|CONFIG_IO_WQ' /boot/config-$(uname -r)
```

## 16.1 关键字解释

### `git rev-parse HEAD`

```text
git
→ Git 工具

rev-parse
→ 解析 revision

HEAD
→ 当前检出的提交
```

作用：

> 固定 Benchmark 对应代码版本，避免“代码变了但还用旧性能数据”。

### `uname -r`

```text
-r
→ 输出 kernel release
```

io_uring 和内核版本关系很强，所以必须记录。

### `uname -m`

```text
-m
→ machine architecture
```

例如：

```text
x86_64
```

### `lscpu`

查看：

```text
CPU 型号
核心数
线程数
NUMA / socket
虚拟化
指令集
```

### `free -h`

```text
-h
→ human-readable
```

例如用 GiB/MiB 显示内存。

### `df -hT .`

```text
-h
→ 可读容量

-T
→ 显示 filesystem type

.
→ 当前目录所在文件系统
```

用来确认 Benchmark 数据最终运行在哪类文件系统上。

### `grep -E`

```text
grep
→ 文本匹配

-E
→ 使用扩展正则表达式
```

这里一次匹配：

```text
CONFIG_IO_URING
或
CONFIG_IO_WQ
```

### `$(uname -r)`

Shell 命令替换：

```text
先执行 uname -r
→ 再把输出拼进文件路径
```

---

# 17. Release Benchmark 构建

```bash
cmake --preset bench-release
cmake --build --preset bench-release -j2
```

## 17.1 为什么必须 Release

性能测试不能主要用 Debug：

```text
Debug
→ 优化关闭/较弱
→ assert/调试信息更多
→ 热点结构可能不同
```

Release 才接近真实部署性能。

## 17.2 命令关键字

### `cmake --preset bench-release`

```text
--preset
→ 选择 CMakePresets.json 中定义的配置

bench-release
→ 项目为 benchmark 准备的 Release preset
```

### `cmake --build`

告诉 CMake：

```text
不是重新配置
而是构建已经配置好的 build tree
```

### `-j2`

```text
-j
→ 并行构建 job

-j2
→ 最多两个编译任务并行
```

它只影响：

```text
编译过程
```

不等于：

```text
E2E workers = 2
```

---

# 18. 结果目录

```bash
mkdir -p benchmark-results/v5-io-uring/perf
mkdir -p benchmark-results/v5-io-uring/strace
```

也可写成：

```bash
mkdir -p benchmark-results/v5-io-uring/{perf,strace}
```

解释：

```text
mkdir
→ 创建目录

-p
→ 父目录不存在时一起创建；
   目录已经存在时不报错

{perf,strace}
→ Shell brace expansion
→ 展开成两个目录
```

---

# 19. 64 MiB Microbenchmark 的目的

这组测试不跑完整业务，而只观察：

```text
Copy
+
BLAKE3
+
fdatasync
```

目的是尽量回答：

> **只看 V5 改动直接覆盖的数据路径，io_uring 本身有没有收益？**

固定：

```text
64 MiB = 67108864 bytes
30 repetitions
```

---

# 20. 同步 Copy 正式命令

```bash
./build/bench-release/photobridge_bench \
  --workload copy-sync \
  --repetitions 30 \
  --copy-bytes 67108864 \
  --output benchmark-results/v5-io-uring/copy-sync.json
```

## 20.1 关键字解释

### `./build/bench-release/photobridge_bench`

运行 Release Benchmark 可执行文件。

### `--workload copy-sync`

只跑：

```text
同步 Copy + Hash + fdatasync
```

不启用 io_uring。

### `--repetitions 30`

同一 workload 正式重复 30 次。

意义：

```text
减少单次调度 / writeback / cache 抖动影响
```

### `--copy-bytes 67108864`

固定每次 Copy：

```text
67,108,864 bytes
= 64 MiB
```

### `--output xxx.json`

把每次 sample 和 summary 保存为 JSON，便于：

```text
复查
统计
版本对比
```

---

# 21. io_uring QD 测试命令

以 QD8 为例：

```bash
./build/bench-release/photobridge_bench \
  --workload copy-uring \
  --repetitions 30 \
  --copy-bytes 67108864 \
  --copy-uring-qd 8 \
  --output benchmark-results/v5-io-uring/copy-uring-qd8.json
```

只需替换：

```text
--copy-uring-qd 1
--copy-uring-qd 2
--copy-uring-qd 4
--copy-uring-qd 8
--copy-uring-qd 16
```

### `--copy-uring-qd`

指定：

```text
io_uring 单次 Copy 的 queue depth
```

不是：

```text
Worker 数
```

---

# 22. 分开运行的 QD Sweep 结果

| 模式 | Mean Wall | Mean Throughput | CPU Util | Peak RSS |
|---|---:|---:|---:|---:|
| Sync | 285.26 ms | 237.13 MB/s | 99.19% | 6,380 KiB |
| QD1 | 288.67 ms | 233.19 MB/s | 97.57% | 7,520 KiB |
| QD2 | 224.08 ms | 301.45 MB/s | 127.04% | 8,736 KiB |
| QD4 | 223.12 ms | 301.70 MB/s | 134.19% | 10,816 KiB |
| QD8 | 214.89 ms | 312.60 MB/s | 133.17% | 14,984 KiB |
| QD16 | 228.04 ms | 294.66 MB/s | 131.22% | 23,260 KiB |

这里单独运行时：

```text
QD8 吞吐最高
```

QD8 vs Sync：

```text
Throughput：
237.13
→ 312.60 MB/s
≈ +31.83%

Mean wall：
285.26
→ 214.89 ms
≈ -24.67%
```

但这组是：

```text
分开的 benchmark invocation
```

所以机器瞬时状态、writeback、cache 都可能不同。

它适合观察趋势，不是最强的 A/B 证据。

---

# 23. 同进程 copy-compare

为了减少不同进程、不同时间窗口造成的波动，又跑了：

```text
同一个 benchmark 进程
→ sync
→ QD1
→ QD2
→ QD4
→ QD8
→ QD16
```

正式：

```text
64 MiB
30 reps / mode
```

结果：

| 模式 | Mean Throughput | Mean Wall | CPU Util | Peak RSS | 相比 Sync 吞吐 |
|---|---:|---:|---:|---:|---:|
| Sync | 248.95 MB/s | 270.15 ms | 98.95% | 6,364 KiB | 基准 |
| QD1 | 245.61 MB/s | 274.58 ms | 97.54% | 7,900 KiB | -1.34% |
| QD2 | 288.75 MB/s | 233.04 ms | 124.65% | 8,928 KiB | +15.99% |
| QD4 | **314.97 MB/s** | **218.58 ms** | 134.03% | 11,088 KiB | **+26.52%** |
| QD8 | 306.01 MB/s | 220.36 ms | 135.35% | 15,096 KiB | +22.92% |
| QD16 | 292.56 MB/s | 230.15 ms | 133.80% | 23,328 KiB | +17.52% |

Mean wall 相比 Sync：

| 模式 | Wall 改善 |
|---|---:|
| QD1 | -1.64%，即更慢 |
| QD2 | +13.74% |
| QD4 | +19.09% |
| QD8 | +18.43% |
| QD16 | +14.81% |

---

# 24. QD 结果怎么解释

## 24.1 QD1 为什么没有提升

QD1 本质上仍然只能维持很浅的流水。

同时增加了：

```text
ring 管理
submission/completion
额外 buffer / bookkeeping
```

所以：

```text
没有足够并发收益
+
有 io_uring 管理开销
→ 反而略慢
```

## 24.2 QD2 开始明显提升

从 QD2 开始：

```text
一个 I/O 等待
另一个请求可以在途
```

流水开始真正形成。

## 24.3 QD4 是同进程平均吞吐最佳点

```text
314.97 MB/s
```

相对 Sync：

```text
+26.52%
```

同时内存仍然明显低于 QD8/QD16。

因此生产默认：

```text
QD4
```

有合理性。

## 24.4 QD8 为什么没有继续线性增长

更深队列开始遇到：

```text
CPU
虚拟磁盘
io-wq
调度
缓存
fdatasync
```

等其他限制。

QD 不是越大越快。

## 24.5 QD16 为什么下降

```text
更多 slot
更多 buffer
更多调度和 bookkeeping
```

但底层设备/CPU 并没有足够能力消费这些额外并发。

表现为：

```text
吞吐下降
RSS 显著增加
```

---

# 25. QD4 与 QD8 的特殊现象

同进程测试中：

```text
QD4 mean throughput 更高
```

但 QD4 出现过较明显长尾：

```text
wall ≈ 317 ms
wall ≈ 373 ms
```

QD8 的该组结果相对更稳定：

```text
P95 ≈ 243 ms
max ≈ 249 ms
```

因此不能只说：

> “QD4 在所有情况下绝对最好。”

更合理的是：

```text
QD4
→ 当前生产默认
→ 平均吞吐 / 内存成本平衡较好

QD8
→ 某些运行中更稳定或单独 sweep 更快
→ 但 RSS 更高
```

是否调整默认值应该看：

```text
Worker × QD
+
真实存储
+
完整 E2E
```

而不是单一 Microbenchmark。

---

# 26. 64 MiB 测试的 page cache 限制

64 MiB 样本中经常看到：

```text
/proc/self/io read_bytes = 0
```

而数据仍然成功读到了。

这说明测试强烈符合：

```text
source data 已经命中 Linux page cache
```

因此该 Microbenchmark 主要体现：

```text
内存页访问
BLAKE3
系统调用路径
io_uring submission/completion
调度
写入
fdatasync
```

而不是纯粹测试：

```text
冷盘 64 MiB 顺序读取速度
```

另外 `/proc/self/io` 对普通 `read/write` 与 io_uring 的记账方式也不应机械等价比较。

因此不能说：

> “io_uring 把真实磁盘读取速度提高了 26%。”

准确说法：

> **在当前 warm-cache、本地 ext4、VM 环境的 Copy+Hash workload 中，V5 降低了被测路径的 wall time。**

---

# 27. 1 GiB 单文件扩展测试

为了避免只有 64 MiB，又测试：

```text
1 GiB
= 1,073,741,824 bytes
5 repetitions
```

---

# 28. 1 GiB 同步结果

```text
Mean wall：4175.15 ms
Median：3836.66 ms
Min：3776.45 ms
Max：5399.45 ms

Mean throughput：261.96 MB/s
Median：279.86 MB/s

CPU util：99.78%
RSS：6136 KiB
```

每次大致：

```text
read syscalls：1027
write syscalls：1024
```

这和：

```text
1 GiB / 1 MiB ≈ 1024 chunks
```

非常吻合。

---

# 29. 1 GiB QD4 结果

5 次 wall：

```text
2419.91 ms
2552.63 ms
2497.17 ms
6496.82 ms
11444.49 ms
```

汇总：

```text
Mean：5082.20 ms
Median：2552.63 ms

Mean throughput：310.69 MB/s
Median throughput：420.64 MB/s
Max：443.71 MB/s
Min：93.82 MB/s
```

前 3 次 CPU utilization：

```text
约 152%～157%
```

后两个慢样本：

```text
约 51%
约 34%
```

这已经明显提示：

> 慢样本不是 CPU 算不过来，而更像线程在等待底层持久化 I/O。

---

# 30. 1 GiB QD8 结果

5 次 wall：

```text
2495.86 ms
4891.45 ms
6273.49 ms
5545.92 ms
2504.55 ms
```

汇总：

```text
Mean wall：4342.26 ms
Median：4891.45 ms

Mean throughput：288.64 MB/s
Median：219.51 MB/s
Max：430.21 MB/s
Min：171.16 MB/s
```

CPU utilization 同样明显两极：

```text
快样本：约 151%～155%
慢样本：约 67%～71%
```

这再次说明：

```text
长尾期间 CPU 并没有持续满负荷
```

---

# 31. 为什么 1 GiB 平均值不能直接拿来宣传

如果只看平均：

```text
QD4 Mean throughput > Sync
```

但：

```text
QD4 Mean wall 反而大于 Sync
```

看起来非常矛盾。

原因是样本数量只有 5，而且存在巨大长尾：

```text
2.4 s
2.5 s
2.5 s
6.5 s
11.4 s
```

这时：

```text
mean
```

会被少量异常样本严重拉动。

因此本报告不采用：

> “1 GiB QD4 比 Sync 提升 X%”

作为正式主结论。

1 GiB 测试更大的价值是：

> **暴露持久化写回长尾。**

---

# 32. 用 strace 验证 1 GiB 长尾

QD4 过滤 `fsync/fdatasync` 后得到：

```text
fsync：
0.458096 s
后续大多约 0.002 s

fdatasync：
0.821917 s
0.701493 s
0.722539 s
3.569254 s
4.182164 s
```

可以看到后两次 `fdatasync` 明显变长。

同步版本：

```text
fsync：
第一次 1.054234 s
后续大多约 0.002 s

fdatasync：
0.816875 s
0.415918 s
0.895935 s
0.803147 s
4.706749 s
```

同步路径同样出现：

```text
4.7 s 的 fdatasync 长尾
```

因此关键结论：

> **1 GiB 大文件测试中的极端长尾不是 io_uring 独有问题。同步和 io_uring 都会受虚拟磁盘 / Linux writeback / `fdatasync` 持久化等待影响。**

---

# 33. 1 GiB strace 命令

io_uring QD4：

```bash
strace -f -ttT \
  -e trace=fdatasync,fsync \
  -o benchmark-results/v5-io-uring/strace/copy-uring-qd4-1g-sync.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-uring \
  --repetitions 5 \
  --copy-bytes 1073741824 \
  --copy-uring-qd 4
```

同步：

```bash
strace -f -ttT \
  -e trace=fdatasync,fsync \
  -o benchmark-results/v5-io-uring/strace/copy-sync-1g-sync.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-sync \
  --repetitions 5 \
  --copy-bytes 1073741824
```

---

# 34. strace 关键字解释

### `strace`

跟踪进程的 Linux system call。

它能看到：

```text
read
write
openat
fdatasync
fsync
futex
clock_nanosleep
io_uring_enter
...
```

但看不到：

```text
普通 C++ 函数内部执行
SIMD 指令细节
```

### `-f`

Follow。

跟踪：

```text
线程 / 子进程
```

PhotoBridge 有 Worker Pool 和 io_uring 相关线程行为，所以不能只跟主线程。

### `-tt`

为每条 syscall 打较高精度时间戳。

适合判断：

```text
调用发生在什么时候
```

### `-T`

记录每次 syscall 自身持续时间：

```text
fdatasync(...) = 0 <4.182164>
```

尖括号内就是调用耗时。

### `-e trace=...`

只跟指定 syscall。

例如：

```text
-e trace=fdatasync,fsync
```

能显著减少日志噪声。

### `-o`

输出到文件，而不是刷满终端。

---

# 35. 为什么不能删除 fdatasync

看到：

```text
fdatasync = 4 s
```

不能直接说：

> “删掉它就快了。”

PhotoBridge 的持久化协议依赖：

```text
temp 数据写入
↓
fdatasync(temp)
↓
确认数据已经达到 durable boundary
↓
MarkTempWritten
↓
继续 receipt / publish
```

如果直接删除：

```text
性能数字可能变好
```

但 crash window 会改变：

```text
数据库认为 temp 已写完
≠
数据真正 durable
```

这属于：

```text
用正确性换 Benchmark
```

不能接受。

未来要优化这部分，只能：

```text
重新设计 durability batching
或
重新定义持久化语义
```

然后重新做 crash test。

---

# 36. Copy perf stat

同步：

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v5-io-uring/perf/copy-sync-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-sync \
  --repetitions 10 \
  --copy-bytes 67108864
```

QD8：

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v5-io-uring/perf/copy-uring-qd8-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-uring \
  --repetitions 10 \
  --copy-bytes 67108864 \
  --copy-uring-qd 8
```

---

# 37. perf stat 关键字解释

### `perf stat`

做性能计数统计。

它回答的是：

```text
用了多少 CPU 时间
发生多少 context switch
多少 migration
多少 page fault
```

而不是给出函数热点调用栈。

### `-e`

指定 event。

### `task-clock`

任务累计 CPU 时间。

如果：

```text
task-clock > elapsed wall time
```

意味着多个线程/内核 worker 有并行 CPU 工作。

例如：

```text
task-clock / elapsed ≈ 1.25
```

可理解成平均约：

```text
1.25 个 CPU core-equivalent
```

在工作。

### `context-switches`

线程被调度切换的次数。

增加通常意味着：

```text
并发更多
等待/唤醒更多
```

但不能单独等价成“性能差”。

### `cpu-migrations`

任务从一个 CPU 迁移到另一个 CPU 的次数。

### `page-faults`

缺页异常次数。

这里多数不代表：

```text
程序崩溃
```

而是：

```text
进程访问尚未建立映射/物理页的虚拟内存
→ 内核完成缺页处理
```

### `-o`

将 `perf stat` 文本结果写入指定文件。

---

# 38. Copy perf stat 结果

同步：

```text
task-clock：3183.46 ms
CPUs utilized：0.988
context-switches：367
cpu-migrations：1
page-faults：687
elapsed：3.22356 s
user：0.77565 s
sys：2.41134 s
```

QD8：

```text
task-clock：3500.48 ms
CPUs utilized：1.255
context-switches：841
cpu-migrations：7
page-faults：20908
elapsed：2.78930 s
user：0.71676 s
sys：2.79577 s
```

可以看到：

```text
wall time 下降
但是
task-clock 增加
sys time 增加
context switch 增加
page fault 增加
```

---

# 39. perf stat 的核心解释

QD8 相比 Sync，大致：

```text
task-clock：+9.96%
平均 CPU core-equivalent：
0.988 → 1.255

context-switch：
367 → 841
约 +129%

page-fault：
687 → 20908
约 30 倍量级

sys：
2.41 s → 2.80 s
约 +16%
```

因此 io_uring 的价值不是：

> “减少 CPU 使用。”

而是：

> **用更多并发 CPU / 内核路径 / buffer 活动，减少端到端等待时间。**

这对后端面试非常重要。

---

# 40. page fault 为什么增加

io_uring 版本同时维护多个 slot：

```text
QD1
QD2
QD4
QD8
QD16
```

更高 QD 会触碰更多 buffer page。

所以：

```text
更高 RSS
+
更多内存页首次访问
```

和 page fault 增加是方向一致的。

但严格来说：

> **仅凭 `perf stat` 不能证明所有新增 page fault 都来自 BufferSlot。**

还可能涉及：

```text
liburing
ring memory
io-wq
allocator
benchmark 生命周期
```

所以报告只写“与更多 buffer/page 活动一致”，不做过度因果结论。

---

# 41. Copy strace 汇总

同步：

```text
47.95%  fdatasync
30.59%  write
10.23%  read
 5.62%  unlinkat
 5.05%  fsync

712 write
694 read
总 syscall：1644
```

QD8：

```text
51.79%  fdatasync
30.11%  io_uring_enter
 7.00%  unlinkat
 6.20%  fsync
 2.79%  write
 0.12%  read

1224 io_uring_enter
72 write
44 read
总 syscall：1681
```

---

# 42. Copy strace 命令

同步：

```bash
strace -f -c \
  -o benchmark-results/v5-io-uring/strace/copy-sync-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-sync \
  --repetitions 10 \
  --copy-bytes 67108864
```

QD8：

```bash
strace -f -c \
  -o benchmark-results/v5-io-uring/strace/copy-uring-qd8-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload copy-uring \
  --repetitions 10 \
  --copy-bytes 67108864 \
  --copy-uring-qd 8
```

### `-c`

Summary mode。

不打印每一条 syscall，而是聚合：

```text
% time
seconds
usecs/call
calls
errors
syscall
```

特别适合比较：

```text
Sync
vs
io_uring
```

---

# 43. 一个重要发现：io_uring 没有减少总 syscall 数

同步：

```text
1644 total syscalls
```

QD8：

```text
1681 total syscalls
```

因此不能说：

> “io_uring 的收益来自 syscall 总数大幅下降。”

实际变化更像：

```text
传统大量 read/write
↓
转成 io_uring_enter submission/completion 路径
```

QD8：

```text
io_uring_enter = 1224 calls / 10 reps
≈ 122 calls / run
```

说明当前实现已经改变 I/O 交互模式，但 submission/completion batching 仍然有继续研究空间。

---

# 44. strace 的性能数字为什么不能当 Benchmark

`strace` 会：

```text
ptrace / tracing
+
记录 syscall
+
线程跟踪
```

显著影响：

```text
wall time
线程调度
syscall 时间分布
```

因此：

```text
strace
→ 用来证明“发生了什么”

正式 JSON benchmark
→ 用来回答“到底快多少”
```

不能混用。

---

# 45. perf record：看热点而不是只看计数

命令示例：

```bash
perf record -F 199 -g \
  -o benchmark-results/v5-io-uring/perf/copy-uring-qd8.data \
  ./build/bench-release/photobridge_bench \
  --workload copy-uring \
  --repetitions 10 \
  --copy-bytes 67108864 \
  --copy-uring-qd 8
```

导出：

```bash
perf report --stdio \
  -i benchmark-results/v5-io-uring/perf/copy-uring-qd8.data \
  > benchmark-results/v5-io-uring/perf/copy-uring-qd8-report.txt
```

---

# 46. perf record / report 关键字

### `perf record`

对运行过程采样，保存：

```text
哪个函数 / 调用栈消耗 CPU
```

### `-F 199`

采样频率：

```text
约 199 Hz
```

不是：

```text
程序只运行 199 次
```

### `-g`

记录调用栈。

没有它只能更难判断：

```text
这个热点是谁调用出来的
```

### `-o xxx.data`

保存 perf 二进制采样数据。

### `perf report`

读取 `.data` 并生成热点报告。

### `--stdio`

输出纯文本，适合：

```text
保存
grep
提交到 benchmark-results
```

### `-i`

指定输入 perf data 文件。

### `> report.txt`

Shell stdout 重定向：

```text
把屏幕输出
→ 写入 report.txt
```

---

# 47. perf record 的 io_uring 证据

QD8 profile 中可以看到：

```text
blake3_hash_many_avx2
≈ 20%～21%

io_uring_submit
Children ≈ 14.42%

iou-wrk-*
Children ≈ 27.64%
```

而且：

```text
Lost Samples = 0
```

这说明至少在这个 Profile 里：

```text
用户态 PhotoBridge/BLAKE3
和
io_uring 内核 worker
```

都实际承担了 CPU 工作。

因此可以安全地说：

> **perf 证明 V5 实际运行时存在 `io_uring_submit` 和 `iou-wrk` 活动，同时用户态仍在执行 BLAKE3 AVX2。结合 CPU utilization > 100% 与更低 wall time，证据与“计算和 I/O 活动存在重叠”一致。**

不要夸大成：

> “perf 精确证明 read 和 hash 重叠了 X%。”

Profile 没有提供这种精确因果比例。

---

# 48. 同步 perf record 对照

同步路径中：

```text
fdatasync Children ≈ 21.91%
BLAKE3 AVX2 ≈ 23% 左右
并存在显著 read/write 内核开销
```

因此 V5 之后：

```text
BLAKE3
仍然是重要 CPU 热点

fdatasync
仍然是重要持久化热点

但 read/write 的执行路径改变
```

说明优化没有把系统变成：

```text
“只剩 io_uring”
```

而是发生了新的瓶颈分布。

---

# 49. E2E 为什么必须做

Microbenchmark 只能证明：

```text
你优化的局部快了
```

不能证明：

```text
用户真正执行一次照片迁移也快了相同比例
```

因此 V5 继续跑完整：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

E2E Benchmark 直接在 benchmark 进程内调用真实 CLI 入口，避免额外 child-process 启动开销。

`e2e-large` 固定：

```text
64 files
×
16 MiB / file
=
1 GiB
```

正式 Worker：

```text
4
```

---

# 50. E2E-large warmup

先跑一轮：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

目的：

```text
预热代码路径
页缓存
动态链接
数据库 schema / 文件系统状态
```

warmup 数据不进入正式 10 次统计。

---

# 51. E2E io_uring 正式命令

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v5-io-uring/e2e-large-4w.json
```

### `--e2e-workers 4`

指定真实 migrate 阶段的 Worker 数：

```text
4
```

它和：

```text
--copy-uring-qd
```

不是同一个维度。

E2E 使用生产默认 Copy QD：

```text
QD4
```

---

# 52. 同版本 Sync-Control 命令

```bash
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1 \
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v5-io-uring/e2e-large-4w-sync-control.json
```

这里：

```bash
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1
```

是 Shell 为当前进程临时设置环境变量。

只影响这一条命令，不代表永久写入系统环境。

目的：

> **在相同 V5 二进制中强制使用同步 fallback，构造最干净的 A/B。**

---

# 53. 正式 E2E A/B 结果

| 指标 | Sync Control | io_uring V5 | 变化 |
|---|---:|---:|---:|
| Mean Wall | 8717.34 ms | **8505.58 ms** | **-2.43%** |
| Median Wall | 8686.67 ms | **8483.06 ms** | **-2.34%** |
| Mean Throughput | 0.11472 pipelines/s | **0.11759 pipelines/s** | **+2.50%** |
| Mean CPU | 12792.87 ms | 13482.63 ms | 增加 |
| Mean CPU Util | 146.76% | **158.53%** | 增加 |
| Peak RSS | 13,340 KiB | **30,112 KiB** | +约 16.4 MiB |
| Read syscalls / 10 runs | 113,245 | 104,087 | 下降 |
| Write syscalls / 10 runs | 54,348 | 44,271 | 下降 |
| Kernel write bytes | 约 10.846 GB | 约 10.847 GB | 基本一致 |

io_uring 正式 10 次 wall：

```text
Min：8329.76 ms
Mean：8505.58 ms
Median：8483.06 ms
Max：8676.86 ms
```

这一组比 1 GiB 单文件 Copy 稳定得多。

---

# 54. 为什么 Micro +26%，E2E 只有 +2.5%

因为 Microbenchmark 测的是：

```text
Copy + Hash + fdatasync
```

而 E2E 是：

```text
scan
→ plan
→ migrate
    ├─ Claim
    ├─ SQLite 状态
    ├─ Copy + Hash        ← V5 主要优化这里
    ├─ fdatasync
    ├─ SQLite 状态
    ├─ VerifyBinary       ← 仍同步
    ├─ receipt
    ├─ rename / fsync
    └─ finish
→ resume
→ verify                 ← 又有完整校验工作
```

所以：

```text
局部快 26%
```

经过 Amdahl 定律式的稀释，完全可能变成：

```text
全链路快 2.5%
```

这不是优化失败。

反而说明 Benchmark 没有只挑一个对自己有利的局部数字。

---

# 55. E2E perf stat

io_uring：

```bash
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v5-io-uring/perf/e2e-large-4w-uring-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 4
```

Sync Control：

```bash
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1 \
perf stat \
  -e task-clock,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v5-io-uring/perf/e2e-large-4w-sync-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 4
```

---

# 56. E2E perf stat 结果

| 指标 | Sync | io_uring |
|---|---:|---:|
| task-clock | 43,455.47 ms | 48,057.69 ms |
| CPUs utilized | 0.942 | 1.453 |
| context-switches | 11,571 | 17,182 |
| cpu-migrations | 658 | 1,611 |
| page-faults | 4,077 | 194,742 |
| user | 21.525 s | 20.081 s |
| sys | 22.097 s | 28.184 s |
| perf elapsed | 46.130 s | 33.084 s |

大致变化：

```text
task-clock：+10.6%
context-switch：+48.5%
cpu migration：约 +145%
page fault：约 47.8 倍
sys time：+27.5%
```

---

# 57. 为什么不能拿 perf elapsed 说“E2E 快 28%”

这两次 `perf stat`：

```text
io_uring：33.08 s
sync：46.13 s
```

看起来差距巨大。

但它们是：

```text
分开运行
+
只有 3 reps
+
1 GiB 写入
+
虚拟磁盘 writeback 状态不同
+
perf 本身有采样/统计扰动
```

正式无 tracing 的 10 次同版本 A/B 才是性能主结论：

```text
约 +2.50% throughput
约 -2.43% mean wall
```

`perf` 在这里用于解释：

```text
CPU / scheduling 行为
```

而不是替代正式 Benchmark。

---

# 58. E2E strace

io_uring：

```bash
strace -f -c \
  -o benchmark-results/v5-io-uring/strace/e2e-large-4w-uring-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

Sync：

```bash
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1 \
strace -f -c \
  -o benchmark-results/v5-io-uring/strace/e2e-large-4w-sync-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

只用：

```text
1 repetition
```

因为 `strace -f` 对多线程完整 E2E 扰动很大。

---

# 59. E2E strace 对比

## Sync

```text
37.67%  clock_nanosleep   13.061 s
15.27%  futex              5.294 s
 9.67%  fsync              3.353 s
 9.44%  fdatasync          3.274 s
 9.31%  write              3.228 s
 8.66%  read               3.004 s
 3.89%  pread64            1.348 s
 2.20%  pwrite64           0.763 s
```

## io_uring

```text
38.12%  clock_nanosleep   12.161 s
18.05%  futex              5.758 s
10.60%  fsync              3.380 s
 9.40%  fdatasync          2.999 s
 6.13%  read               1.957 s
 4.75%  io_uring_enter     1.514 s
 3.65%  pread64            1.164 s
 3.10%  write              0.989 s
 2.48%  pwrite64           0.791 s
```

---

# 60. E2E strace 说明了什么

同步传统：

```text
read + write
≈ 6.23 s
```

io_uring：

```text
read + write + io_uring_enter
≈ 4.46 s
```

虽然不能把 strace 下的秒数当正式速度数据，但 syscall 分布清楚显示：

```text
普通 read/write 开销下降
io_uring_enter 出现
```

说明：

> **V5 的数据面确实进入了真实 E2E 主链。**

但新的主要占比来自：

```text
clock_nanosleep
futex
fsync
fdatasync
```

所以 Copy 不再是唯一问题。

---

# 61. 为什么 clock_nanosleep 高达约 38%

最开始需要排除：

```text
是不是 PhotoBridge 自己 sleep？
```

搜索：

```bash
grep -RInE \
  'sleep_for|sleep_until|usleep|nanosleep|clock_nanosleep' \
  src include \
  2>/dev/null
```

只发现测试 hook：

```text
src/common/test_hooks.cpp
```

继续查：

```bash
grep -RIn 'PauseForTest' src include 2>/dev/null
```

然后确认环境：

```bash
env | grep '^PHOTOBRIDGE_TEST_PAUSE_'
```

正式 Benchmark 时：

```text
无输出
```

所以 test pause hook 没有真正 sleep。

---

# 62. grep 命令关键字解释

### `-R`

Recursive：

```text
递归搜索子目录
```

### `-I`

忽略 binary file。

### `-n`

打印匹配行的行号。

### `-E`

使用 extended regular expression。

所以：

```text
A|B|C
```

表示多个候选。

### `2>/dev/null`

Shell：

```text
2
→ stderr

>
→ 重定向

/dev/null
→ 丢弃
```

即：

> 不显示权限/不存在文件等错误噪声。

---

# 63. SQLite busy handler 调用栈证据

随后使用：

```bash
strace -f -k -e trace=clock_nanosleep \
  -o benchmark-results/v5-io-uring/strace/e2e-large-nanosleep-stack.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

关键栈：

```text
clock_nanosleep
↓
__nanosleep
↓
unixSleep
↓
sqliteDefaultBusyCallback
↓
btreeBeginTrans
↓
sqlite3VdbeExec
↓
sqlite3_step / sqlite3_exec
↓
SqliteConnection::Execute
↓
TaskRuntimeRepository ...
```

已经抓到的真实业务入口包括：

```text
ClaimNextReadyImpl
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
```

还观察到 SQLite 退避等待：

```text
1 ms
2 ms
5 ms
10 ms
15 ms
20 ms
25 ms
50 ms
...
```

---

# 64. strace `-k` 的作用

```text
-k
→ 为 syscall 记录 stack trace
```

普通：

```bash
strace -e trace=clock_nanosleep
```

只能告诉你：

```text
程序 sleep 了
```

加 `-k` 后才能进一步看到：

```text
谁调用 sleep
→ sqliteDefaultBusyCallback
→ 哪个 PhotoBridge 状态操作触发
```

这一步把：

```text
“怀疑 SQLite 锁竞争”
```

升级成：

```text
“有调用栈证据的 SQLite busy backoff”
```

---

# 65. SQLite 当前配置

PhotoBridge SQLite 采用：

```text
journal_mode = WAL
synchronous = FULL
foreign_keys = ON
busy_timeout = 5000 ms
```

其中：

```cpp
sqlite3_busy_timeout(database, 5000);
```

意味着遇到锁竞争时：

```text
不会立即 SQLITE_BUSY
↓
SQLite default busy handler 等待 / 重试
↓
最多累计到 timeout 边界
```

这正对应 Profile 中的：

```text
sqliteDefaultBusyCallback
→ unixSleep
→ clock_nanosleep
```

---

# 66. WAL 为什么仍然有写锁竞争

一个常见误区：

> “用了 WAL，就可以多个 Writer 完全并发。”

不对。

WAL 的重要优势是：

```text
Reader 和 Writer 更容易并发
```

但 SQLite 仍然保持：

```text
同一时刻一个 Writer
```

PhotoBridge 的实际并发模型是：

```text
Producer / 主连接
→ ClaimNextReady
→ SQLite write transaction

Worker 1
→ 独立 SQLite connection

Worker 2
→ 独立 SQLite connection

Worker 3
→ 独立 SQLite connection

Worker 4
→ 独立 SQLite connection
```

Worker 会执行：

```text
MarkCommitIntent
MarkTempWritten
PersistVerifiedReceipt
完成状态推进
```

这些操作中存在：

```text
BEGIN IMMEDIATE
```

于是：

```text
Producer + 多 Worker
→ 多连接竞争单 Writer
→ busy handler
→ clock_nanosleep 退避
```

---

# 67. BEGIN IMMEDIATE 为什么会放大竞争

例如 `AdvanceAttempt()`：

```text
BEGIN IMMEDIATE
↓
读 current epoch
↓
检查 ownership
↓
读 attempt state
↓
UPDATE attempt
↓
INSERT event
↓
COMMIT
```

写锁覆盖的不是只有：

```text
UPDATE 那一瞬间
```

而是整个事务生命周期。

`ClaimNextReadyImpl()`：

```text
BEGIN IMMEDIATE
↓
epoch check
↓
SELECT ready task
↓
UPDATE plan_task
↓
INSERT task_attempt
↓
INSERT task_event
↓
读取 runtime
↓
COMMIT
```

`PersistVerifiedReceipt()`：

```text
BEGIN IMMEDIATE
↓
epoch check
↓
ownership check
↓
INSERT verified_receipt
↓
UPDATE task_attempt
↓
INSERT task_event
↓
COMMIT
```

所以多 Worker 时竞争不是偶然。

---

# 68. SQLite 锁竞争是 V5 造成的吗

不是。

更准确的关系是：

```text
V3 Multi-worker
→ 已经引入多 SQLite connection 写竞争可能

V5 io_uring
→ Copy 路径进一步加快

Copy 占比下降
→ SQLite / durability / Verify 占比更显眼
```

所以这叫：

> **瓶颈迁移 / bottleneck shift**

而不是：

> “io_uring 把 SQLite 搞慢了。”

---

# 69. futex 又说明什么

E2E strace：

```text
futex ≈ 15%～18%
```

`futex` 常用于：

```text
mutex
condition_variable
线程等待 / 唤醒
库内部同步
```

PhotoBridge 已经有：

```text
Producer
Bounded Queue
Worker Pool
SQLite
io_uring / io-wq
```

所以多线程同步成本上升并不意外。

但是仅凭 `strace -c` 不应直接说：

> “全部 futex 都来自 bounded queue。”

如果未来要进一步定位，需要：

```text
perf lock
或
更针对性的 stack/profile
```

V5 到这里已经没有必要继续深挖。

---

# 70. V5 当前瓶颈总图

V5 后完整主链的瓶颈可概括为：

```text
                   ┌→ SQLite WAL 单 Writer
                   │   → BEGIN IMMEDIATE
                   │   → busy handler / nanosleep
                   │
scan → plan → migrate
                   │
                   ├→ io_uring Copy + Hash
                   │   → 已获得局部加速
                   │
                   ├→ fdatasync(temp)
                   │   → durability barrier
                   │   → 大文件存在 writeback 长尾
                   │
                   ├→ VerifyBinary(sync)
                   │   → 第二遍独立 read + hash
                   │
                   ├→ receipt / event / state transaction
                   │
                   └→ rename / directory fsync
↓
resume
↓
verify
```

因此下一阶段不能继续只盯着：

```text
read/write
```

---

# 71. V5 的资源代价

## CPU

正式 E2E：

```text
Sync：146.76%
io_uring：158.53%
```

说明：

```text
V5 使用了更多 CPU 并发
```

## 内存

正式 E2E：

```text
Sync RSS：13,340 KiB
io_uring：30,112 KiB
```

约多：

```text
16.4 MiB
```

这和：

```text
4 Worker
×
QD4
×
1 MiB slot
```

的额外 buffer 规模方向一致。

## 调度

`perf stat`：

```text
context-switch
cpu-migration
page-fault
```

都增加。

因此 V5 的工程取舍是：

```text
更高 CPU / 内存 / 调度开销
→ 换更低 wall time
```

---

# 72. 为什么仍然选择 QD4 作为默认

虽然不同测试中：

```text
QD4 / QD8
```

谁最高存在一定波动，但默认 QD4 有几个优势：

```text
同进程平均吞吐最高
内存明显低于 QD8/QD16
E2E 4 Worker × QD4 表现稳定
不会把每 Worker 的队列压得太深
```

因此目前：

```text
QD4 = 合理工程默认
```

而不是：

```text
QD4 = 理论最优值
```

真实部署应该允许：

```text
按设备重新 Benchmark
```

---

# 73. Benchmark 可信度分级

本次证据按可信度建议这样排序。

## 第一层：正式同代码 E2E A/B

最重要：

```text
V5 io_uring
vs
V5 强制 sync fallback
```

结论：

```text
约 +2.5% E2E throughput
```

## 第二层：同进程 64 MiB copy-compare

用于：

```text
QD 调优
局部收益
```

结论：

```text
QD4 +26.5% mean throughput
```

## 第三层：perf / strace

用于解释：

```text
为什么会快
成本是什么
瓶颈转移到哪里
```

不是正式速度成绩。

## 第四层：1 GiB 单文件 5 次

主要用于：

```text
发现 fdatasync/writeback 长尾
```

不适合拿 mean 直接宣传。

---

# 74. 不应该怎么描述 V5

以下说法不准确：

```text
“io_uring 让整个 PhotoBridge 性能提升 30%。”
```

错误原因：

```text
30% 左右只出现在特定 Copy Microbenchmark
```

以下说法也不准确：

```text
“io_uring 大幅减少了系统调用次数。”
```

因为：

```text
Sync total syscall ≈ 1644
QD8 total syscall ≈ 1681
```

只是类型发生变化。

也不要说：

```text
“io_uring 更省 CPU。”
```

实际 CPU / sys / context switching 普遍更高。

也不要说：

```text
“QD 越大越快。”
```

QD16 已经出现收益下降和 RSS 增加。

---

# 75. 面试 2～3 分钟正式回答

## 问：你在 PhotoBridge 里为什么用了 io_uring？效果怎么样？

可以直接回答：

> PhotoBridge 原来的迁移 Copy 路径是同步的 1 MiB 分块循环，也就是 read 一块、做一次 BLAKE3、再 write 一块，整个单任务数据面基本是串行的。前面我已经做了 Multi-worker 和 BLAKE3 SIMD，所以第四项优化我把第一遍 Copy+Hash 改成了 io_uring。
>
> 我的实现不是简单把 read/write API 换掉，而是做了一个有界 BufferSlot 状态机，slot 在 Free、Reading、Ready、Writing 之间转换。多个 read 可以同时在途，但 BLAKE3 必须按照文件 sequence 顺序更新；短读短写也通过 progress 继续重提请求。每个 Worker 线程复用自己的 ring，默认 1 MiB chunk、QD4，并且如果 io_uring 在任何数据 I/O 提交前不可用，会安全 fallback 到原同步路径。一旦已经提交 I/O，错误直接返回，不做盲目重复 Copy。
>
> 性能上我没有只看 microbenchmark。64 MiB 同进程对照里，同步大约 249 MB/s，QD4 大约 315 MB/s，局部吞吐提升约 26.5%；QD1 基本没有收益，QD16 又因为队列过深和内存开销出现回落。
>
> 但完整 1 GiB、4 Worker 的 `scan → plan → migrate → resume → verify` 同版本 A/B 里，io_uring 的端到端吞吐只提高约 2.5%，平均耗时从约 8.72 秒降到 8.51 秒。这个差异说明 Copy 只是整个迁移链的一部分，后面还有同步独立校验、fdatasync/fsync、SQLite 状态事务和恢复流程。
>
> `perf` 还显示 io_uring 版本 CPU 并发和系统态开销更高，RSS 也从约 13 MiB 增加到约 29 MiB，所以它本质上是用更多并发 CPU 和 buffer 内存换更低 wall time，而不是免费优化。
>
> 最后我用 strace 和调用栈继续定位，发现 E2E 中大量 `clock_nanosleep` 实际来自 SQLite 的 `sqliteDefaultBusyCallback`。项目虽然用了 WAL，但仍然只有一个 Writer，Producer 和多个 Worker 的 `BEGIN IMMEDIATE` 会竞争写锁。所以 V5 做完以后，下一阶段瓶颈已经从单纯的 Copy I/O 转移到了 SQLite 写事务、持久化刷盘和同步 Verify。

---

# 76. 面试官追问：为什么不用普通异步线程池，而用 io_uring

回答重点：

```text
Worker Pool
→ Task 级并行

io_uring
→ 单 Task 内 I/O 级并行
```

如果只继续堆 Worker：

```text
任务并发增加
但每个任务内部仍同步 read/hash/write
```

而且会进一步增加：

```text
SQLite 竞争
线程切换
```

所以 V5 想研究的是不同层级的并发。

---

# 77. 面试官追问：为什么 QD4

回答：

```text
不是拍脑袋
→ 我测了 QD1/2/4/8/16
```

结果：

```text
QD1 无收益
QD2 开始有效
QD4 同进程平均吞吐最高
QD8 接近且某些场景更稳定
QD16 吞吐回落、RSS 明显增加
```

所以：

> QD4 是当前 VM + 4 Worker + 1 MiB chunk 下的工程平衡点，不声称它对所有设备最优。

---

# 78. 面试官追问：为什么 E2E 提升这么小

回答：

```text
因为 Amdahl 定律
```

V5 只优化：

```text
migrate 中第一遍 Copy+Hash
```

没有消除：

```text
scan
plan
SQLite state
fdatasync/fsync
independent VerifyBinary
resume
final verify
```

所以：

```text
Microbenchmark +26%
≠
E2E +26%
```

最终 +2.5% 是合理结果。

---

# 79. 面试官追问：为什么还保留同步 Verify

回答：

> Copy 阶段的 Hash 是 Copy 流程内部产生的证据，但项目的可靠性目标要求独立重新打开 temp 文件再做一次 Verify。V5 的目标是优化第一遍数据搬运，不改变原有 crash-safe 和 independent verification 语义。未来可以单独给 Verify 设计异步读取，但不能为了 Benchmark 直接删掉它。

---

# 80. 面试官追问：io_uring 有什么代价

直接给数据：

```text
Copy perf：
CPU equivalent 0.988 → 1.255
context-switch 367 → 841
page-fault 687 → 20908

E2E：
CPU util 146.76% → 158.53%
RSS 13.0 MiB → 29.4 MiB
```

结论：

```text
它不是省 CPU
而是提高并发度
```

---

# 81. 面试官追问：怎么证明真的用了 io_uring

三层证据：

```text
第一层：代码路径
TryIoUringCopyAndHash 已接入 MigrationAttemptPreparer

第二层：strace
出现大量 io_uring_enter
传统 read/write 占比下降

第三层：perf
出现 io_uring_submit
出现 iou-wrk-* 内核 worker
```

所以不是：

```text
只链接了 liburing
```

而是运行时真的走了它。

---

# 82. 面试官追问：1 GiB 为什么会突然 11 秒

回答：

> 我一开始也不能直接归因于 io_uring，所以用 `strace -ttT` 只跟踪 `fdatasync/fsync`。结果 QD4 的慢样本里 `fdatasync` 出现 3.57 秒和 4.18 秒等待；同步版同样出现过 4.71 秒 `fdatasync`。所以长尾主要和虚拟磁盘 writeback / durability flush 强相关，不是 io_uring 独有退化。

注意说：

```text
“强相关 / 主要来源”
```

不要绝对说：

```text
“100% 都是 fdatasync”
```

---

# 83. 面试官追问：SQLite 为什么又成瓶颈

回答：

> 我做完 io_uring 后用 E2E strace 发现 `clock_nanosleep` 占了约 38%。项目代码自身没有开启测试 sleep，于是我用 `strace -k` 抓调用栈，最终定位到 `sqliteDefaultBusyCallback → btreeBeginTrans`。PhotoBridge 用的是 WAL + FULL + 5 秒 busy timeout，WAL 可以改善读写并发，但仍然只有一个 Writer。Producer 的 Claim 和多个 Worker 的状态推进都使用独立连接并执行 `BEGIN IMMEDIATE`，所以会发生写锁竞争并进入 busy backoff。这是优化后的瓶颈迁移。

---

# 84. 下一阶段最值得优化什么

## 优先级 1：SQLite 写事务

不是简单：

```text
busy_timeout 从 5000 改 0
```

那只会更快失败。

真正值得研究：

```text
减少写事务数量
缩短 BEGIN IMMEDIATE 持锁区间
合并安全可合并的状态 + event 写入
研究 dedicated DB writer
```

但必须保持：

```text
epoch fencing
task ownership
receipt ordering
crash-safe state machine
```

不能因为性能把状态机原子性拆坏。

## 优先级 2：Verify

当前：

```text
第一遍 Copy+Hash：io_uring
第二遍 VerifyBinary：同步
```

可单独 Benchmark：

```text
sync verify
vs
io_uring verify
```

前提是不降低独立验证语义。

## 优先级 3：io_uring batching

当前：

```text
io_uring_enter 约 122 次 / 64 MiB run（QD8 strace）
```

未来可以研究：

```text
更有效 SQE batching
registered buffers
fixed files
```

但必须：

```text
先 Benchmark
再决定要不要增加复杂度
```

## 优先级 4：Worker × QD 联合矩阵

例如：

```text
Worker = 1 / 2 / 4 / 8
QD     = 1 / 2 / 4 / 8
```

找组合最优，而不是单独调一个参数。

## 优先级 5：真实存储

补：

```text
本地 cold cache
本地 warm cache
SMB cold
SMB warm
```

这样才能判断真实校园网 / NAS / 手机迁移场景。

---

# 85. 推荐的 Worker × QD 测试矩阵

| Workers | QD1 | QD2 | QD4 | QD8 |
|---:|---:|---:|---:|---:|
| 1 | ✓ | ✓ | ✓ | ✓ |
| 2 | ✓ | ✓ | ✓ | ✓ |
| 4 | ✓ | ✓ | ✓ | ✓ |
| 8 | ✓ | ✓ | ✓ | ✓ |

每组至少记录：

```text
wall
throughput
CPU util
RSS
context switch
SQLite busy / nanosleep
fdatasync tail
```

目标不是找：

```text
最高 Micro MB/s
```

而是找：

```text
完整 E2E 性价比最高点
```

---

# 86. Benchmark 常用指标解释

## Wall Time

真实经过时间：

```text
开始
→ 结束
```

用户最直接感受到的时间。

## CPU Time

所有线程累计占用 CPU 的时间。

所以多线程程序可以：

```text
CPU time > wall time
```

## CPU Utilization

本项目近似：

```text
CPU time / wall time × 100%
```

例如：

```text
158%
```

表示平均使用约：

```text
1.58 个 core-equivalent
```

不是“CPU 坏了”。

## Throughput

单位时间完成的工作。

Copy：

```text
bytes/s
MB/s
```

E2E：

```text
pipelines/s
```

## P95

95% 的样本：

```text
≤ 这个值
```

## P99

99% 的样本：

```text
≤ 这个值
```

本次某些 5 次小样本：

```text
P95 / P99 实际几乎等于最大值
```

因此样本少时不能过度解读 percentile。

## RSS

Resident Set Size：

```text
当前进程实际驻留物理内存页规模
```

V5 的 QD buffer 会直接影响它。

---

# 87. `tee` 的作用

Benchmark 常见：

```bash
./photobridge_bench ... \
  | tee benchmark-results/v5-io-uring/run.txt
```

解释：

```text
|
→ pipe
→ 把左边 stdout 送到右边程序 stdin

tee
→ 一份打印到终端
→ 一份写入文件
```

优点：

```text
跑的时候能看
跑完以后也有文本证据
```

如果 Benchmark 同时使用：

```text
--output result.json
```

则：

```text
JSON
→ 机器可分析

tee txt
→ 人可阅读原始输出
```

---

# 88. Shell 反斜杠 `\`

命令中：

```bash
perf stat \
  -e ... \
  -o ... \
  ./program
```

行尾：

```text
\
```

表示：

```text
下一行仍属于同一条 shell command
```

它只是为了命令可读性。

---

# 89. `/proc/self/io` 指标解释

Benchmark 记录：

```text
rchar
wchar
read_bytes
write_bytes
syscr
syscw
```

需要注意：

```text
rchar / wchar
→ 进程发起的字符 I/O 统计

read_bytes / write_bytes
→ 内核认为真正引起的存储层字节统计

syscr / syscw
→ read/write 类 syscall 计数
```

io_uring 的记账路径与传统 read/write 不能机械一一对应。

所以报告只把它作为：

```text
辅助证据
```

不能靠：

```text
read_syscalls = 2
```

就宣称：

> “1 GiB 只发生了两次内核 I/O。”

真正的数据 I/O 是通过 io_uring 请求执行的。

---

# 90. 最终数据总表

## 90.1 Copy 同进程 64 MiB

| Mode | Throughput | Mean Wall | CPU | RSS |
|---|---:|---:|---:|---:|
| Sync | 248.95 MB/s | 270.15 ms | 98.95% | 6.2 MiB |
| QD1 | 245.61 | 274.58 ms | 97.54% | 7.7 MiB |
| QD2 | 288.75 | 233.04 ms | 124.65% | 8.7 MiB |
| QD4 | **314.97** | **218.58 ms** | 134.03% | 10.8 MiB |
| QD8 | 306.01 | 220.36 ms | 135.35% | 14.7 MiB |
| QD16 | 292.56 | 230.15 ms | 133.80% | 22.8 MiB |

## 90.2 E2E 1 GiB × 4 Worker

| Mode | Mean Wall | Throughput | CPU Util | RSS |
|---|---:|---:|---:|---:|
| Sync Control | 8717.34 ms | 0.11472 pipelines/s | 146.76% | 13.0 MiB |
| io_uring QD4 | **8505.58 ms** | **0.11759 pipelines/s** | 158.53% | 29.4 MiB |
| 变化 | **-2.43%** | **+2.50%** | 上升 | 上升 |

## 90.3 Copy perf

| 指标 | Sync | QD8 |
|---|---:|---:|
| CPU equivalent | 0.988 | 1.255 |
| ctx switch | 367 | 841 |
| page fault | 687 | 20,908 |
| elapsed | 3.224 s | 2.789 s |

---

# 91. 本次 V5 证明了什么

已经能确认：

```text
1. io_uring 已真实进入生产迁移 Copy+Hash 主链
2. 有明确同步 fallback
3. 有短读短写处理
4. 有 Hash 顺序约束
5. ring 在 Worker 内复用
6. QD 可调并做过 1/2/4/8/16 对比
7. 64 MiB Microbenchmark 有明确局部收益
8. perf / strace 证明 io_uring 实际在工作
9. 1 GiB 长尾与 fdatasync/writeback 强相关
10. 完整 E2E 做了同代码 Sync-Control A/B
11. E2E 真实收益约 2.5%
12. 成本是更高 CPU、RSS、调度活动
13. SQLite busy backoff 已成为后续重要瓶颈
```

---

# 92. 本次 V5 没有证明什么

不能声称：

```text
所有 Linux 机器都提升 26%
所有 SSD 都提升
SMB/NAS 一定提升
冷缓存读取一定提升
QD4 永远最优
io_uring 更省 CPU
整个 PhotoBridge 快了 30%
```

也没有完成：

```text
SMB cold/warm benchmark
真实手机网络端到端 benchmark
Worker × QD 完整二维矩阵
VerifyBinary io_uring 化
registered buffer / fixed file 优化
SQLite 下一轮锁竞争优化
```

---

# 93. 简历可以怎么写

推荐不要写夸张的：

```text
使用 io_uring 将整体性能提升 30%
```

可以写：

> **针对迁移阶段同步 `read/hash/write` 数据路径引入 io_uring 有界队列流水，设计 QD1～16 对照实验；64 MiB Copy+Hash 同进程基准下 QD4 吞吐由约 249 MB/s 提升至 315 MB/s（+26.5%），并通过 perf/strace 定位端到端收益受持久化刷盘、独立校验与 SQLite 单写者锁竞争限制。**

如果简历空间更小：

> **基于 io_uring 重构 Copy+Hash I/O 流水，完成 QD1～16、perf/strace 与 1 GiB E2E A/B；局部吞吐最高提升约 26%，并定位 fdatasync 与 SQLite 写锁竞争等后续瓶颈。**

这里建议：

```text
“局部吞吐”
```

一定写清楚。

---

# 94. 面试最核心结论

建议背下面这一段：

> **我没有只看 io_uring 的 microbenchmark。Copy+Hash 隔离场景里，QD4/QD8 能提升大约二三十个百分点，但同版本完整 1 GiB 主链 A/B 只有约 2.5% 的端到端吞吐提升。继续用 perf 和 strace 分析后发现，一方面 `fdatasync` 会产生明显 writeback 长尾，另一方面多 Worker 下 SQLite WAL 仍然只有一个 Writer，`BEGIN IMMEDIATE` 竞争会进入 `sqliteDefaultBusyCallback`，在调用栈里表现成大量 `clock_nanosleep`。所以我最终把这次优化理解成一次瓶颈迁移：io_uring 加速了第一遍 Copy 数据面，但系统新的主要限制转向了持久化、独立校验和 SQLite 状态事务。它是用更多 CPU 并发和 buffer 内存换 wall time，不是免费加速。**

---

# 95. V5 最终评价

从工程角度看，V5 的价值不只是：

```text
用了 io_uring
```

而是完成了一个完整性能工程闭环：

```text
发现同步数据路径
↓
设计异步有限队列
↓
保留 fallback 和 crash-safe barrier
↓
QD 参数实验
↓
Microbenchmark
↓
1 GiB scaling
↓
perf
↓
strace
↓
E2E same-code A/B
↓
定位新瓶颈
↓
得到下一轮优化方向
```

这比单纯写一句：

```text
“掌握 io_uring”
```

更有面试价值。

---

# 96. 推荐保留的 Benchmark 原始文件

建议仓库中最终保留：

```text
benchmark-results/v5-io-uring/
├── copy-sync.json
├── copy-compare-qd8.json
├── copy-sync-1g.json
├── copy-uring-qd4-1g.json
├── copy-uring-qd8-1g.json
├── e2e-large-4w.json
├── e2e-large-4w-sync-control.json
├── perf/
│   ├── copy-sync-stat.txt
│   ├── copy-uring-qd8-stat.txt
│   ├── copy-sync-report.txt
│   ├── copy-uring-qd8-report.txt
│   ├── e2e-large-4w-uring-stat.txt
│   └── e2e-large-4w-sync-stat.txt
└── strace/
    ├── copy-sync-summary.txt
    ├── copy-uring-qd8-summary.txt
    ├── copy-sync-1g-sync.txt
    ├── copy-uring-qd4-1g-sync.txt
    ├── e2e-large-4w-uring-summary.txt
    ├── e2e-large-4w-sync-summary.txt
    └── e2e-large-nanosleep-stack.txt
```

这样以后面试或重新测试时，可以做到：

```text
结论
→ 能追到 summary
→ 能追到原始 sample
→ 能追到 syscall/profile 证据
```

---

# 97. 数据口径备注

本报告中的 Copy 吞吐沿用 Benchmark 当前记录口径，以：

```text
bytes / second
```

换成十进制：

```text
MB/s
```

表达。

若换成：

```text
MiB/s
```

数字会不同。

因此后续写简历和报告时应保持同一单位，不要把：

```text
MB/s
与
MiB/s
```

混用。

---

# 98. 最终收口

V5 可以正式收口为：

```text
问题：
同步 Copy+Hash 无法形成单任务 I/O pipeline

方案：
io_uring + 1 MiB BufferSlot + 可调 QD + Worker 内 ring 复用

正确性：
顺序 Hash
短读短写续传
初始化失败安全 fallback
I/O 开始后错误不盲目重试
fdatasync / Verify / receipt 持久化语义不变

局部结果：
QD4 同进程 Copy throughput ≈ +26.5%

完整结果：
1 GiB / 4 Worker E2E throughput ≈ +2.5%

代价：
CPU ↑
RSS ↑
context switch ↑
system work ↑

新瓶颈：
SQLite 单 Writer 锁竞争
fdatasync / fsync
同步独立 Verify
完整 Pipeline 其他阶段

工程结论：
io_uring 有效，但收益受 Amdahl 定律和持久化协议约束；
优化后系统瓶颈发生迁移。
```

---

# 99. 后续如果继续优化的顺序

推荐：

```text
第一：
SQLite 写事务数量 / 持锁时间

第二：
VerifyBinary I/O 路径

第三：
Worker × QD 联合调参

第四：
io_uring SQE/CQE batching、registered buffer 等

第五：
SMB / NAS cold-warm benchmark
```

而不是继续盲目：

```text
把 QD 从 16 加到 32 / 64
```

---

# 100. 本报告使用的主要证据文件

```text
copy-compare-qd8.json
copy-sync-1g.json
copy-uring-qd4-1g.json
copy-uring-qd8-1g.json

copy-sync-stat.txt
copy-uring-qd8-stat.txt

copy-sync-summary.txt
copy-uring-qd8-summary.txt

copy-sync-report.txt
copy-uring-qd8-report.txt

copy-sync-1g-sync.txt
copy-uring-qd4-1g-sync.txt

e2e-large-4w.json
e2e-large-4w-sync-control.json

e2e-large-4w-uring-stat.txt
e2e-large-4w-sync-stat.txt

e2e-large-4w-uring-summary.txt
e2e-large-4w-sync-summary.txt

e2e-large-nanosleep-stack.txt
```

---

**报告结束。**
