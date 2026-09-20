# PhotoBridge 第二项性能优化报告  
## Multi-worker Benchmark、瓶颈分析、测试命令解释与面试结论

> 项目：PhotoBridge  
> 优化项：第二项优化——Multi-worker 并行迁移  
> 测试平台：Ubuntu Linux 虚拟机 / VMware  
> 基准提交：`a88a15c8a8b66d4ec5aa9d116bc4fa8e05738d29`  
> 测试目标：验证多 Worker 是否能提升端到端迁移性能，并定位 4 Worker 之后扩展收益下降的原因。  
> 说明：本文以本轮实际执行得到的 Benchmark、`perf stat`、`strace`、源码检索和正确性回归结果为依据，不额外虚构未测数据。

---

# 1. 本轮优化要解决什么问题

PhotoBridge 的完整迁移主链包含：

```text
scan
→ plan
→ migrate
→ resume
→ verify
```

第二项性能优化的核心不是修改单个文件的复制算法，而是提高 `migrate` 阶段的任务级并行度。

优化前可以近似理解为：

```text
任务 1：copy + hash + fdatasync + rename + 状态更新
↓
任务 2：copy + hash + fdatasync + rename + 状态更新
↓
任务 3
↓
...
```

优化后：

```text
             ┌→ Worker 1 → 文件 A
任务队列 ────┼→ Worker 2 → 文件 B
             ├→ Worker 3 → 文件 C
             └→ Worker 4 → 文件 D
```

也就是说，多个文件可以同时执行复制、哈希和部分 I/O 流程，从而把原来串行的文件级工作重叠起来。

本轮测试重点不是证明“线程越多越快”，而是回答三个更重要的问题：

1. 多 Worker 到底能不能带来真实的端到端性能提升？
2. Worker 数量从 1、2、4、8 增长时，性能如何扩展？
3. 为什么 4 Worker 之后收益明显下降？

---

# 2. 测试原则

本轮沿用 PhotoBridge V1 基准测试的基本规则：

```text
同一台机器
同一文件系统
同一份代码
同一 workload
同一数据规模
同一 Release 构建
同一 repetitions
只改变 e2e-workers
```

这样可以尽量保证：

```text
性能变化
≈ Worker 数量变化带来的影响
```

而不是混入编译选项、机器、数据规模等其它变量。

---

# 3. 测试环境记录

## 3.1 Git

```text
commit:
a88a15c8a8b66d4ec5aa9d116bc4fa8e05738d29
```

测试前 Git 状态中 benchmark-results 路径存在未提交结果文件变化，但不涉及本轮核心源代码，因此不影响 Benchmark 结论。

---

## 3.2 操作系统

```text
Linux hxmserver
kernel: 5.15.0-185-generic
architecture: x86_64
```

---

## 3.3 CPU

```text
CPU:
13th Gen Intel Core i7-13700F

虚拟化：
VMware

虚拟机可用 CPU：
8 vCPU

Thread(s) per core:
1

Core(s) per socket:
4

Socket(s):
2
```

缓存：

```text
L1d: 384 KiB
L1i: 256 KiB
L2 : 16 MiB
L3 : 60 MiB
```

这里最重要的是：

```text
虚拟机可见 8 个 CPU
```

因此本轮选择：

```text
1 / 2 / 4 / 8 Worker
```

具有明确意义。

---

## 3.4 内存

```text
总内存：约 7.7 GiB
Swap：4.0 GiB
```

本轮测试过程中 PhotoBridge 峰值 RSS 只在十几 MiB 量级，因此不存在明显的内存容量瓶颈。

---

## 3.5 文件系统

```text
磁盘：/dev/sda
文件系统：ext4
磁盘规模：约 100 GiB
```

---

## 3.6 编译环境

```text
g++:
Ubuntu 11.4.0

CMake:
3.22.1
```

Benchmark 使用 Release 构建。

---

# 4. Benchmark 数据规模

本轮主要使用：

```text
--workload e2e-large
```

该 workload 固定为：

```text
文件数量：64
单文件大小：16 MiB
总数据量：1 GiB
```

计算：

```text
64 × 16 MiB
= 1024 MiB
= 1 GiB
```

之所以选择 E2E-large，而不是只测一个 copy micro benchmark，是因为第二项优化影响的是完整迁移阶段的任务级并行。

我们希望观察：

```text
真实主链中的并行收益
```

而不仅仅是：

```text
单个 memcpy/read/write 的峰值速度
```

---

# 5. Benchmark 构建检查

## 5.1 构建命令

```bash
cmake --build --preset bench-release -j2
```

### `cmake`

调用 CMake。

### `--build`

表示进入“构建模式”，不是重新配置工程。

### `--preset bench-release`

使用项目中已经定义好的 `bench-release` build preset。

它对应 Benchmark 的 Release 构建配置。

### `-j2`

最多并行启动 2 个构建任务。

注意：

```text
-j2
```

只控制“编译时”的并行度。

它和运行 Benchmark 时的：

```text
--e2e-workers
```

完全不是同一个概念。

---

# 6. Benchmark 命令解释

正式测试的核心命令形式如下：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers N \
  --output benchmark-results/v3-multiworker/e2e-large-workerN.json
```

---

## 6.1 `./build/bench-release/photobridge_bench`

运行 PhotoBridge 的 Benchmark 可执行程序。

路径：

```text
build/bench-release/
```

表示运行的是 Release Benchmark 构建结果。

---

## 6.2 `--workload e2e-large`

指定测试项目：

```text
e2e-large
```

也就是 1 GiB 的完整本地 E2E pipeline。

---

## 6.3 `--repetitions 10`

同一个测试连续执行 10 次。

为什么不只跑一次？

因为一次测试可能受到：

```text
虚拟机调度
page cache
后台任务
文件系统瞬时抖动
CPU 调度
```

影响。

重复运行后可以观察：

```text
mean
median
min
max
P95
P99
```

从而判断结果是否稳定。

---

## 6.4 `--e2e-workers N`

本轮最关键变量。

例如：

```bash
--e2e-workers 1
```

代表迁移阶段使用 1 个 Worker。

```bash
--e2e-workers 4
```

代表使用 4 个 Worker。

本轮依次测试：

```text
1
2
4
8
```

---

## 6.5 `--output xxx.json`

把 Benchmark 原始数据保存为 JSON。

例如：

```bash
--output benchmark-results/v3-multiworker/e2e-large-worker4.json
```

这样后续分析不需要依赖终端截图，而是可以直接读取：

```text
每次 sample
wall time
CPU time
CPU utilization
RSS
read/write syscall
kernel read/write bytes
```

这是性能测试可复现性的重要组成部分。

---

# 7. 为什么正式测试前先 Warm-up

每一组 Worker 正式测试前先执行：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers N
```

例如 1 Worker 第一次 Warm-up：

```text
wall time ≈ 19.20 s
```

但正式 10 次测试平均只有：

```text
12.47 s
```

这个差异非常明显。

说明：

```text
第一次运行
→ 文件页尚未充分进入 page cache
→ 后续重复运行大量读取命中缓存
→ warm-cache 性能明显更高
```

因此本轮的 1/2/4/8 Worker 对比，本质上应描述为：

```text
同一台本地 ext4 虚拟机上的 warm-cache E2E 扩展性测试
```

不能直接把结果说成：

```text
真实机械磁盘 / 冷缓存 / SMB 网络盘吞吐
```

这是非常重要的测试边界。

---

# 8. 1 Worker 正式结果

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 1 \
  --output benchmark-results/v3-multiworker/e2e-large-worker1.json
```

结果：

```text
mean wall time:
12.469 s

median:
12.527 s

min:
11.963 s

max:
12.751 s

CPU utilization:
93.54%

peak RSS:
9412 KiB
```

换算实际数据吞吐：

```text
1024 MiB / 12.469 s
≈ 82.12 MiB/s
```

该数据作为本轮优化的串行基线：

```text
1 Worker = 1.00×
```

---

# 9. 2 Worker 正式结果

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 2 \
  --output benchmark-results/v3-multiworker/e2e-large-worker2.json
```

结果：

```text
mean wall time:
10.143 s

CPU utilization:
124.17%

peak RSS:
10816 KiB
```

换算数据吞吐：

```text
1024 / 10.143
≈ 100.96 MiB/s
```

相对 1 Worker：

```text
speedup
= 12.469 / 10.143
≈ 1.229×

wall time 下降：
≈ 18.66%
```

说明：

```text
1 → 2 Worker
有明显收益
```

---

# 10. 4 Worker 正式结果

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v3-multiworker/e2e-large-worker4.json
```

结果：

```text
mean wall time:
8.994 s

CPU utilization:
148.46%

peak RSS:
13404 KiB
```

换算数据吞吐：

```text
1024 / 8.994
≈ 113.86 MiB/s
```

相对 1 Worker：

```text
speedup:
1.386×

wall time 下降：
27.87%

数据吞吐提升：
38.64%
```

因此：

```text
1 → 4 Worker
是本轮最主要的一段性能收益
```

---

# 11. 8 Worker 第一次正式测试异常

第一次 8 Worker 正式 10 次结果中出现：

```text
第 1 次：
14.842 s
CPU utilization ≈ 92.20%
```

而其它大多数样本集中在：

```text
8.6 ～ 9.0 s
```

这个样本明显偏离其它结果。

因此不能简单：

```text
删除异常值
```

也不能直接：

```text
拿被异常值拉高的 mean 下结论
```

正确做法是：

```text
保留原始结果
+
重新补跑一组完整 10 次
```

---

# 12. 8 Worker 复测

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 8 \
  --output benchmark-results/v3-multiworker/e2e-large-worker8-repeat.json
```

复测结果：

```text
mean:
8.849 s

median:
8.835 s

min:
8.572 s

max:
9.202 s

CPU utilization:
155.45%

peak RSS:
18148 KiB
```

复测的 10 个样本稳定集中在：

```text
8.57 ～ 9.20 s
```

因此第一次的 `14.84 s` 可以视为一次明显的外部抖动样本，而不是稳定性能。

注意：

```text
我们没有删除原始结果。
```

而是通过独立复测证明：

```text
正常 8 Worker 性能 ≈ 8.85 s
```

---

# 13. 1 / 2 / 4 / 8 Worker 总结果

| Worker | 平均 Wall Time | 数据吞吐 | 相对 1 Worker Speedup | 并行效率 | CPU 利用率 | Peak RSS |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 12.469 s | 82.12 MiB/s | 1.000× | 100.0% | 93.54% | 9.19 MiB |
| 2 | 10.143 s | 100.96 MiB/s | 1.229× | 61.47% | 124.17% | 10.56 MiB |
| 4 | 8.994 s | 113.86 MiB/s | 1.386× | 34.66% | 148.46% | 13.09 MiB |
| 8 | 8.849 s | 115.71 MiB/s | 1.409× | 17.61% | 155.45% | 17.72 MiB |

---

# 14. Worker 扩展性分析

分阶段看：

```text
1 → 2 Worker
wall time 下降约 18.66%

2 → 4 Worker
wall time 再下降约 11.33%

4 → 8 Worker
wall time 只下降约 1.61%
```

总效果：

```text
1 → 4 Worker
12.469 s → 8.994 s

加速约：
1.39×

wall time 降低：
27.87%

吞吐提高：
约 38.64%
```

如果增加到 8 Worker：

```text
12.469 s → 8.849 s

speedup:
1.409×

wall time 降低：
29.03%

吞吐提高：
约 40.90%
```

但是：

```text
4 → 8
只多换来约 1.6% wall-time 改善
```

这已经说明：

```text
4 Worker 左右出现明显性能拐点
```

---

# 15. 为什么 Speedup 不是 4×

如果任务完全可并行，并且没有任何共享资源：

```text
4 Worker
理论上可能接近 4×
```

但真实系统不是这样。

PhotoBridge E2E 中存在：

```text
串行阶段
SQLite 状态写入
文件系统元数据操作
fdatasync / fsync
线程同步
任务队列协调
调度成本
共享底层存储
```

因此不可能简单获得线性扩展：

```text
1 Worker → 4 Worker ≠ 4×
```

实际测得：

```text
≈ 1.39×
```

这不是优化失败，而是说明需要继续找：

```text
并行扩展上限在哪里
```

因此下面进入 `perf stat` 和 `strace` 分析。

---

# 16. perf stat 是什么

`perf` 是 Linux 性能分析工具。

本轮使用：

```bash
perf stat
```

不是用来定位某一行代码，而是统计：

```text
程序总体运行时间
CPU 使用情况
context switch
CPU migration
page fault
硬件性能计数器
```

它回答的问题是：

> Worker 增多之后，程序到底有没有真正使用更多 CPU？代价是什么？

---

# 17. perf stat 命令

以 4 Worker 为例：

```bash
perf stat \
  -o benchmark-results/v3-multiworker/perf/e2e-large-worker4-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 4
```

---

## 17.1 `perf`

Linux 性能分析工具。

---

## 17.2 `stat`

运行统计模式。

意思是：

```text
运行目标程序
+
统计性能事件
```

---

## 17.3 `-o xxx.txt`

把 `perf stat` 的统计结果写入文件。

例如：

```text
benchmark-results/v3-multiworker/perf/
e2e-large-worker4-stat.txt
```

---

## 17.4 `--repetitions 3`

这里是 Benchmark 自己运行 3 次 E2E。

所以 `perf` 测量的是：

```text
3 次完整 benchmark 的总行为
```

这样比单次运行更不容易被一次偶发抖动主导。

---

# 18. perf 中几个重要指标什么意思

## 18.1 `time elapsed`

真实墙钟时间。

例如：

```text
34.29 seconds time elapsed
```

相当于：

```text
用户真正等了多久
```

---

## 18.2 `task-clock`

进程所有线程实际占用 CPU 的总时间。

例如：

```text
47.5 s task-clock
```

并不代表墙钟时间也是 47.5 秒。

因为多个线程可以同时运行。

---

## 18.3 `CPUs utilized`

近似：

```text
task-clock / elapsed
```

例如：

```text
1.386 CPUs utilized
```

表示整个程序平均使用：

```text
约 1.386 个 CPU 核
```

因此：

```text
100% CPU ≈ 1 个核满载
150% CPU ≈ 平均使用 1.5 个核
```

这也解释了为什么多线程程序 CPU utilization 可以超过 100%。

---

## 18.4 `context-switches`

上下文切换次数。

也就是 CPU 从：

```text
线程 A
→ 线程 B
```

或者：

```text
进程 A
→ 进程 B
```

发生调度切换。

Worker 越多，通常：

```text
context switch 越多
```

---

## 18.5 `cpu-migrations`

线程从一个 CPU 核迁移到另一个 CPU 核的次数。

例如：

```text
CPU 1
→ CPU 5
```

迁核可能增加：

```text
调度成本
cache locality 损失
```

---

## 18.6 `page-faults`

缺页次数。

不一定代表“磁盘坏了”或“真的进行了磁盘读取”。

很多缺页属于：

```text
minor page fault
```

例如内存页第一次映射。

---

## 18.7 `cycles / instructions`

本轮输出：

```text
<not supported>
```

原因是当前 VMware 虚拟机没有向 Guest 暴露可用的相应硬件 PMU 计数器。

因此：

```text
不能拿 CPI、IPC 做结论
```

但这不影响：

```text
elapsed
task-clock
CPU utilized
context-switch
cpu-migrations
page-fault
```

这些软件事件的比较。

---

# 19. perf 测试结果

| 指标 | 1 Worker | 4 Worker | 8 Worker |
|---|---:|---:|---:|
| elapsed | 48.74 s | 34.29 s | 33.24 s |
| task-clock | 43.45 s | 47.52 s | 47.33 s |
| CPUs utilized | 0.891 | 1.386 | 1.424 |
| context-switches | 6,330 | 9,681 | 11,508 |
| cpu-migrations | 75 | 671 | 1,537 |
| page-faults | 1,802 | 4,077 | 7,495 |

---

# 20. perf 结果怎么理解

## 20.1 1 → 4 Worker

```text
elapsed:
48.74 → 34.29 s

CPUs utilized:
0.891 → 1.386
```

说明：

```text
多 Worker 确实让更多 CPU 工作重叠执行
```

所以墙钟时间明显下降。

与此同时：

```text
context switch:
6330 → 9681

CPU migration:
75 → 671
```

说明：

```text
并行不是免费的
```

调度成本明显提高。

---

## 20.2 4 → 8 Worker

最关键：

```text
elapsed:
34.29 → 33.24 s
仅再改善约 3.1%
```

而：

```text
CPUs utilized:
1.386 → 1.424
```

只增长约 2.7%。

但是：

```text
context switch:
9681 → 11508

CPU migration:
671 → 1537
```

CPU migration 增幅非常明显。

也就是说：

```text
Worker 翻倍
↓
真正新增 CPU 并行能力很少
↓
线程调度成本继续增长
```

这和 Benchmark 中：

```text
4 → 8 只快约 1.6%
```

高度一致。

---

# 21. 为什么 8 Worker 只用了约 1.4 个 CPU

这是本轮非常值得面试讲的点。

机器虽然有：

```text
8 vCPU
```

程序也启动：

```text
8 Worker
```

但不代表 8 个线程一直都处于：

```text
RUNNING
```

真实情况更多是：

```text
运行
→ 等 I/O
→ 等锁
→ 等 SQLite
→ 等同步
→ 再运行
```

因此：

```text
Worker 数 = 8
≠
CPU 使用 = 8 核
```

这是并发程序非常基础也非常重要的工程区别。

---

# 22. strace 是什么

`strace` 用来观察程序和 Linux 内核之间的系统调用。

例如：

```text
read
write
fsync
fdatasync
futex
openat
pread64
pwrite64
clock_nanosleep
```

本轮主要希望回答：

> Worker 增加以后，程序时间到底花在什么系统调用上？

---

# 23. strace 汇总命令

以 4 Worker 为例：

```bash
strace -c -f \
  -o benchmark-results/v3-multiworker/strace/e2e-large-worker4-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

---

# 24. strace 参数解释

## 24.1 `strace`

跟踪 Linux 系统调用。

---

## 24.2 `-c`

只输出汇总统计。

输出类似：

```text
% time
seconds
usecs/call
calls
errors
syscall
```

不是把每一次 syscall 都打印出来。

优点：

```text
结果非常适合比较 1 / 4 / 8 Worker
```

---

## 24.3 `-f`

跟踪：

```text
fork / clone 创建出来的子线程、子进程
```

这里非常重要。

因为 Multi-worker 是多线程程序。

如果不加：

```text
-f
```

就可能只看到主线程的一部分行为。

---

## 24.4 `-o`

把结果写入文件。

---

# 25. strace 1 Worker

主要结果：

```text
futex:
357 次
6.344 s

write:
2057 次
2.273 s

read:
6553 次
2.225 s

fsync:
480 次
2.068 s

fdatasync:
129 次
1.810 s

clock_nanosleep:
134 次
0.304 s
```

总 syscall：

```text
25107
```

---

# 26. strace 4 Worker

主要结果：

```text
clock_nanosleep:
677 次
14.516 s

futex:
1711 次
6.180 s

read:
6552 次
3.539 s

write:
2057 次
3.427 s

fdatasync:
129 次
3.110 s

fsync:
509 次
3.035 s
```

总 syscall：

```text
30313
```

最异常的变化：

```text
clock_nanosleep
0.30 s
→
14.52 s
```

---

# 27. strace 8 Worker

主要结果：

```text
clock_nanosleep:
1103 次
40.662 s

futex:
2844 次
6.832 s

write:
2057 次
3.967 s

read:
6552 次
3.557 s

fsync:
530 次
3.490 s

fdatasync:
129 次
3.364 s
```

总 syscall：

```text
32765
```

---

# 28. strace 1 / 4 / 8 对比

| 指标 | 1 Worker | 4 Worker | 8 Worker |
|---|---:|---:|---:|
| 总 syscall | 25,107 | 30,313 | 32,765 |
| clock_nanosleep 次数 | 134 | 677 | 1,103 |
| clock_nanosleep 累计时间 | 0.304 s | 14.516 s | 40.662 s |
| futex 次数 | 357 | 1,711 | 2,844 |
| fsync | 480 | 509 | 530 |
| fdatasync | 129 | 129 | 129 |

---

# 29. 这里为什么累计 syscall 时间会大于 wall time

8 Worker：

```text
clock_nanosleep 累计时间：
40.66 s
```

但整个 workload 实际 wall time 并没有 40 多秒。

原因是：

```text
strace -f -c
统计的是所有线程 syscall 时间的累计值
```

例如：

```text
Worker 1 sleep 100 ms
Worker 2 同时 sleep 100 ms
Worker 3 同时 sleep 100 ms
```

墙钟可能只过去：

```text
100 ms
```

但累计 syscall 时间可能达到：

```text
300 ms
```

所以：

```text
strace % time
不能直接等价成“用户等待时间百分比”
```

它更适合比较：

```text
不同配置下 syscall 行为如何变化
```

---

# 30. futex 是什么

`futex`：

```text
fast userspace mutex
```

是 Linux 中实现：

```text
mutex
condition_variable
线程等待
线程唤醒
```

的重要底层机制。

Multi-worker 中线程数量增多后：

```text
futex calls
357
→ 1711
→ 2844
```

说明：

```text
线程同步行为明显增加
```

但是：

```text
不能把所有 futex 都简单归因于 SQLite
```

因为：

```text
线程池
condition_variable
SQLite 内部同步
其它库
```

都有可能产生 futex。

因此更严谨的结论是：

```text
Worker 增多后线程同步开销显著增加
```

---

# 31. fsync / fdatasync 为什么重要

最有意思的是：

```text
fdatasync:

1 Worker = 129
4 Worker = 129
8 Worker = 129
```

也就是说：

```text
加 Worker
不会让必须完成的持久化工作消失
```

Multi-worker 可以让：

```text
copy
hash
部分 I/O
```

重叠执行。

但是持久化协议要求的：

```text
fdatasync / fsync
```

仍然必须完成。

因此并行度增加之后：

```text
真正可并行部分逐渐被加速
↓
不可消除的同步 / durability 成本比例越来越高
↓
加速收益逐渐饱和
```

---

# 32. 为什么开始怀疑 SQLite

`strace` 中最异常的是：

```text
clock_nanosleep
```

从：

```text
1 Worker:
0.30 s
```

到：

```text
8 Worker:
40.66 s
```

因此对源码进行搜索：

```bash
grep -RInE \
  'busy_timeout|sleep_for|sleep_until|usleep|nanosleep|clock_nanosleep|SQLITE_BUSY' \
  src include benchmarks 2>/dev/null
```

---

# 33. grep 命令每个关键字什么意思

## 33.1 `grep`

文本搜索工具。

---

## 33.2 `-R`

递归搜索目录。

也就是：

```text
src/
include/
benchmarks/
```

内部的所有子目录都会继续搜索。

---

## 33.3 `-I`

忽略二进制文件。

---

## 33.4 `-n`

显示行号。

例如：

```text
src/app/sqlite_connection.cpp:202
```

代表：

```text
文件：
src/app/sqlite_connection.cpp

行号：
202
```

---

## 33.5 `-E`

启用扩展正则表达式。

因此可以写：

```text
A|B|C
```

表示：

```text
A 或 B 或 C
```

---

## 33.6 搜索表达式

```text
busy_timeout
sleep_for
sleep_until
usleep
nanosleep
clock_nanosleep
SQLITE_BUSY
```

目的就是寻找：

```text
程序里谁可能主动 sleep / 退避等待
```

---

## 33.7 `2>/dev/null`

把标准错误输出丢弃。

避免一些无关错误干扰搜索结果。

---

# 34. 源码搜索结果

结果：

```text
src/common/test_hooks.cpp:16:
std::this_thread::sleep_for(...)

src/app/sqlite_connection.cpp:202:
sqlite3_busy_timeout(database, 5000)
```

测试 Hook 的 `sleep_for` 只有在相关测试环境变量启用时才会触发。

正常 Benchmark 并没有使用该测试 Hook。

因此更值得关注的是：

```cpp
sqlite3_busy_timeout(database, 5000);
```

也就是：

```text
SQLite connection
遇到数据库 busy / lock 时

最长允许等待：
5000 ms
```

---

# 35. Multi-worker 与 SQLite connection 的关系

当前 Multi-worker 设计中，Worker 并行执行迁移任务，并存在多个数据库连接参与状态写入。

因此可能出现：

```text
Worker 1
→ 更新 SQLite

Worker 2
→ 同时更新 SQLite

Worker 3
→ 同时更新 SQLite

Worker 4
→ 同时更新 SQLite
```

即使 SQLite 使用 WAL：

```text
WAL 并不意味着多个 writer 可以无限并发提交
```

多个写事务仍然需要协调写锁。

因此：

```text
Worker 越多
→ 并发写状态越频繁
→ SQLite writer contention 越容易出现
```

---

# 36. 为什么还不能只凭 grep 就直接断言

看到：

```cpp
sqlite3_busy_timeout(...)
```

再看到：

```text
clock_nanosleep
```

两者具有很强关联。

但严谨的性能分析不能只做：

```text
相关性
→ 直接宣布因果
```

所以继续进行了定向 `strace`。

---

# 37. 定向追踪 clock_nanosleep

命令：

```bash
strace -f -tt -T \
  -e trace=clock_nanosleep \
  -o benchmark-results/v3-multiworker/strace/worker8-nanosleep-trace.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 8
```

---

# 38. 定向 strace 参数解释

## 38.1 `-f`

继续跟踪所有 Worker 线程。

---

## 38.2 `-tt`

输出精确时间戳。

例如：

```text
08:57:31.484002
```

这样可以看多个 Worker 的 sleep 是否同时发生。

---

## 38.3 `-T`

输出每一个 syscall 实际持续时间。

例如：

```text
<0.005732>
```

表示这次 syscall 约持续：

```text
5.732 ms
```

---

## 38.4 `-e trace=clock_nanosleep`

只跟踪：

```text
clock_nanosleep
```

其它系统调用全部忽略。

目的不是做整体性能分析，而是：

```text
验证 sleep 的具体模式
```

---

# 39. 定向 trace 捕获到了什么

实际 trace 中反复出现：

```text
1 ms
→ 2 ms
→ 5 ms
→ 10 ms
→ 15 ms
→ 20 ms
→ 25 ms
→ 50 ms
→ 100 ms
```

例如：

```text
1000000 ns   = 1 ms
2000000 ns   = 2 ms
5000000 ns   = 5 ms
10000000 ns  = 10 ms
15000000 ns  = 15 ms
20000000 ns  = 20 ms
25000000 ns  = 25 ms
50000000 ns  = 50 ms
100000000 ns = 100 ms
```

并且这种模式在多个 Worker 线程中反复出现。

---

# 40. 为什么这个序列很关键

这个：

```text
1
2
5
10
15
20
25
50
100 ms
```

不是随机 sleep。

它和 SQLite `busy_timeout` 使用的递增退避等待行为高度吻合。

因此现在证据链变成：

```text
Worker 增多
↓
多个 SQLite connection 并发访问同一数据库
↓
writer contention 增强
↓
SQLite busy handler 开始退避等待
↓
clock_nanosleep 数量和累计时间暴涨
↓
更多 Worker 处于等待状态
↓
CPU utilized 无法继续明显提高
↓
4 → 8 Worker 收益迅速变小
```

到这里，瓶颈分析才真正形成闭环。

---

# 41. 完整证据链

## 第一层：Benchmark

发现：

```text
1 Worker:
12.469 s

4 Worker:
8.994 s

8 Worker:
8.849 s
```

现象：

```text
1 → 4 有明显收益
4 → 8 几乎不再加速
```

---

## 第二层：perf

发现：

```text
CPUs utilized
0.891
→ 1.386
→ 1.424
```

4 → 8 后 CPU 使用几乎不增长。

但：

```text
context switch
CPU migration
page fault
```

继续增长。

说明：

```text
增加线程主要增加了调度和等待成本
```

---

## 第三层：strace -c

发现：

```text
clock_nanosleep
0.30 s
→ 14.52 s
→ 40.66 s
```

与此同时：

```text
futex calls
357
→ 1711
→ 2844
```

说明：

```text
线程同步 / 休眠等待急剧增加
```

---

## 第四层：源码搜索

发现：

```cpp
sqlite3_busy_timeout(database, 5000);
```

---

## 第五层：定向 strace

实际捕获：

```text
1
2
5
10
15
20
25
50
100 ms
```

递增等待。

因此可以较有把握地把主要 sleep 来源定位为：

```text
SQLite busy timeout / writer lock contention
```

---

# 42. 本轮第二项优化最终结论

Multi-worker 优化是有效的。

不是：

```text
没有收益
```

而是：

```text
收益存在
但有明显性能拐点
```

本机上：

```text
1 Worker
→ 4 Worker
```

端到端性能从：

```text
12.469 s
下降到
8.994 s
```

相当于：

```text
约 1.39× speedup
```

吞吐：

```text
82.12 MiB/s
→
113.86 MiB/s
```

提高约：

```text
38.6%
```

但是：

```text
4 Worker
→ 8 Worker
```

只有：

```text
约 1.6%
```

额外 wall-time 收益。

同时：

```text
RSS 增加
context switch 增加
CPU migration 增加
SQLite busy waiting 急剧增加
```

因此当前测试环境下：

```text
4 Worker
是更合理的默认工程配置
```

不是因为：

```text
4 Worker 的绝对 wall time 最低
```

实际上 8 Worker 略快。

而是因为：

```text
4 Worker 已经拿到绝大部分性能收益
+
8 Worker 只多快约 1.6%
+
额外同步/调度/内存成本明显增加
```

这属于：

```text
性能 / 资源成本
之间的工程折中
```

---

# 43. 为什么不是把默认值直接设置为 8

如果只追求当前机器上的最低平均时间：

```text
8 Worker ≈ 8.849 s
```

略快于：

```text
4 Worker ≈ 8.994 s
```

但差距只有：

```text
约 145 ms / 1 GiB
```

同时：

```text
Peak RSS:
13.09 MiB
→ 17.72 MiB

CPU migration:
671
→ 1537

clock_nanosleep:
14.52 s
→ 40.66 s
```

因此：

```text
8 Worker
属于边际收益非常低
```

选择 4 Worker 更容易解释为：

```text
性能拐点配置
```

---

# 44. 当前结论的适用范围

必须明确：

本轮结论是在：

```text
Ubuntu VM
8 vCPU
VMware
ext4
本地文件系统
warm cache
1 GiB E2E workload
64 × 16 MiB
```

下得到。

因此不能直接说：

```text
所有机器 4 Worker 都是最优
```

更严谨的说法：

> 在本轮 8 vCPU VMware + ext4 的 warm-cache E2E Benchmark 中，4 Worker 已经取得绝大部分并行收益；增加到 8 Worker 仅获得约 1.6% 的额外 wall-time 改善，同时 SQLite busy 等待和调度成本明显增加，因此当前环境下 4 Worker 是更合理的默认值。

---

# 45. 为什么 kernel read_bytes 接近 0

本轮正式 Benchmark 中经常看到：

```text
kernel read_bytes ≈ 0
```

但：

```text
rchar
```

仍然很大。

这意味着：

```text
应用确实执行了 read
```

但数据多数已经：

```text
位于 Linux page cache
```

所以没有形成真正的块设备读取。

这进一步证明：

```text
本轮更偏向 warm-cache CPU / 软件栈 / SQLite / filesystem 协调测试
```

而不是冷存储性能测试。

---

# 46. 为什么逻辑读取量会大于 1 GiB

虽然 workload 配置数据规模是：

```text
1 GiB
```

但 E2E pipeline 中可能出现多轮逻辑读取，例如：

```text
copy 时读取
hash 时读取
verify 时再次读取
SQLite / manifest 等其它读取
```

因此：

```text
logical rchar
>
source data size
```

并不异常。

---

# 47. P95 / P99 的限制

每组正式 Benchmark：

```text
10 samples
```

样本数量并不大。

当前 Benchmark 的统计实现中，10 次样本下：

```text
P95 / P99
很容易退化到 max
```

因此报告中更应该优先看：

```text
mean
median
min/max
样本稳定性
```

而不是过度强调：

```text
P99
```

如果以后要做正式性能论文级分析，应增加到：

```text
30
50
100+
```

更多样本。

---

# 48. strace 本身会改变程序性能

必须注意：

```text
strace
```

会拦截系统调用。

因此它具有明显测量开销。

所以：

```text
strace 下的 wall time
不能直接和普通 benchmark wall time 混在一起比较
```

本轮使用 `strace` 的目的只是：

```text
比较 syscall 行为
+
定位 sleep / lock 模式
```

而不是拿它测最终吞吐。

---

# 49. perf 结果同样应该和正式 Benchmark 分开

正式性能数字应以：

```text
普通 Release Benchmark
```

为准。

`perf stat` 用于解释：

```text
为什么会快
为什么会饱和
```

所以最终简历上写：

```text
1.39× speedup
```

应该来自正式 10 次 Benchmark。

而不是来自：

```text
perf 下的 elapsed
```

---

# 50. 正确性回归

最终使用实际存在的 Test preset：

```bash
ctest --preset debug --output-on-failure \
  | tee benchmark-results/v3-multiworker/ctest-debug.txt
```

测试结果：

```text
全部成功
```

---

# 51. ctest 命令解释

## `ctest`

CMake 配套的测试运行工具。

---

## `--preset debug`

使用：

```text
debug
```

test preset。

本项目实际可用 test presets：

```text
debug
sanitized
```

注意：

```text
bench-release
```

是构建相关 preset，不是有效的 test preset。

因此下面这个命令是错误的：

```bash
ctest --preset bench-release
```

会得到：

```text
No such test preset
```

---

## `--output-on-failure`

正常测试通过时保持简洁。

如果失败：

```text
把失败测试的详细输出打印出来
```

非常适合 CI 和回归验证。

---

## `|`

Shell 管道。

把左边命令的标准输出传给右边。

---

## `tee`

同时：

```text
在终端显示
+
写入文件
```

因此：

```bash
| tee benchmark-results/v3-multiworker/ctest-debug.txt
```

可以既看到测试结果，又保存测试记录。

---

# 52. 本轮推荐保留的结果文件

建议目录：

```text
benchmark-results/
└── v3-multiworker/
    ├── environment.txt
    ├── e2e-large-worker1.json
    ├── e2e-large-worker2.json
    ├── e2e-large-worker4.json
    ├── e2e-large-worker8.json
    ├── e2e-large-worker8-repeat.json
    ├── ctest-debug.txt
    ├── perf/
    │   ├── e2e-large-worker1-stat.txt
    │   ├── e2e-large-worker4-stat.txt
    │   └── e2e-large-worker8-stat.txt
    └── strace/
        ├── e2e-large-worker1-summary.txt
        ├── e2e-large-worker4-summary.txt
        ├── e2e-large-worker8-summary.txt
        └── worker8-nanosleep-trace.txt
```

---

# 53. 完整复现命令汇总

## 53.1 Build

```bash
cmake --build --preset bench-release -j2
```

---

## 53.2 1 Worker Warm-up

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 1
```

---

## 53.3 1 Worker 正式

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 1 \
  --output benchmark-results/v3-multiworker/e2e-large-worker1.json
```

---

## 53.4 2 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 2 \
  --output benchmark-results/v3-multiworker/e2e-large-worker2.json
```

---

## 53.5 4 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 4 \
  --output benchmark-results/v3-multiworker/e2e-large-worker4.json
```

---

## 53.6 8 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 8 \
  --output benchmark-results/v3-multiworker/e2e-large-worker8.json
```

---

## 53.7 8 Worker 独立复测

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 10 \
  --e2e-workers 8 \
  --output benchmark-results/v3-multiworker/e2e-large-worker8-repeat.json
```

---

## 53.8 perf 1 Worker

```bash
perf stat \
  -o benchmark-results/v3-multiworker/perf/e2e-large-worker1-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 1
```

---

## 53.9 perf 4 Worker

```bash
perf stat \
  -o benchmark-results/v3-multiworker/perf/e2e-large-worker4-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 4
```

---

## 53.10 perf 8 Worker

```bash
perf stat \
  -o benchmark-results/v3-multiworker/perf/e2e-large-worker8-stat.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 3 \
  --e2e-workers 8
```

---

## 53.11 strace 1 Worker

```bash
strace -c -f \
  -o benchmark-results/v3-multiworker/strace/e2e-large-worker1-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 1
```

---

## 53.12 strace 4 Worker

```bash
strace -c -f \
  -o benchmark-results/v3-multiworker/strace/e2e-large-worker4-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 4
```

---

## 53.13 strace 8 Worker

```bash
strace -c -f \
  -o benchmark-results/v3-multiworker/strace/e2e-large-worker8-summary.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 8
```

---

## 53.14 搜索 sleep 来源

```bash
grep -RInE \
  'busy_timeout|sleep_for|sleep_until|usleep|nanosleep|clock_nanosleep|SQLITE_BUSY' \
  src include benchmarks 2>/dev/null
```

---

## 53.15 定向追踪 clock_nanosleep

```bash
strace -f -tt -T \
  -e trace=clock_nanosleep \
  -o benchmark-results/v3-multiworker/strace/worker8-nanosleep-trace.txt \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --repetitions 1 \
  --e2e-workers 8
```

---

## 53.16 正确性回归

```bash
ctest --preset debug --output-on-failure \
  | tee benchmark-results/v3-multiworker/ctest-debug.txt
```

---

# 54. 面试时 2～3 分钟怎么讲

可以按下面逻辑讲。

---

## 面试回答版本

> PhotoBridge 的第二项性能优化是把 migrate 从单 Worker 改成可配置的多 Worker 文件级并行。我没有直接把线程数设得越大越好，而是专门做了 1、2、4、8 Worker 的 1 GiB 端到端 Benchmark。
>
> 在同一台 8 vCPU VMware 虚拟机上，1 Worker 平均耗时 12.47 秒，4 Worker 降到 8.99 秒，相当于大约 1.39 倍加速，端到端吞吐从约 82 MiB/s 提升到 114 MiB/s。但增加到 8 Worker 后只有 8.85 秒，只比 4 Worker 再快大约 1.6%，所以出现了明显的性能拐点。
>
> 我继续用 perf stat 定位，发现 1 Worker 到 4 Worker 时 CPU 平均利用从约 0.89 个核提高到 1.39 个核，所以前面的加速确实来自更多并行执行；但 4 Worker 到 8 Worker 时只增加到约 1.42 个核，context switch 和 CPU migration 反而继续明显增长。
>
> 然后我用 strace -c 看系统调用，发现 clock_nanosleep 的累计等待从 1 Worker 的约 0.3 秒增加到 4 Worker 的 14.5 秒，再到 8 Worker 的 40.7 秒。我进一步搜索代码，发现 SQLite connection 设置了 5 秒 busy timeout，于是对 clock_nanosleep 做定向 strace。最终抓到了 1、2、5、10、15、20、25、50、100 毫秒的递增退避序列，和 SQLite busy handler 的等待模式吻合。
>
> 所以最后的结论是，多 Worker 对 copy/hash 这类文件级工作是有效的，但 Worker 增多后，多 SQLite connection 的并发写竞争和线程调度成本逐渐成为瓶颈。在当前测试环境下，4 Worker 已经取得绝大部分性能收益，所以我把它作为更合理的默认并发度，而不是简单设置成 8。

---

# 55. 面试官可能追问：为什么不用 8 Worker

回答：

> 8 Worker 的绝对 wall time 确实略低，但只比 4 Worker 快大约 1.6%。与此同时，CPU migration、context switch、RSS 和 SQLite busy waiting 都继续增加，所以我更关注边际收益和资源成本，而不是只看最低的单个耗时。当前环境下 4 Worker 位于比较明显的性能拐点。

---

# 56. 面试官可能追问：为什么只有 1.39×，不是 4×

回答：

> 因为完整 E2E 并不是完全可并行。copy/hash 可以在文件级重叠执行，但 fdatasync、fsync、SQLite 状态写入、文件系统元数据和线程同步仍然存在共享资源和串行部分。随着并行部分被加速，这些不可并行部分会逐渐成为新的瓶颈，所以扩展不是线性的。

---

# 57. 面试官可能追问：你怎么证明是 SQLite

回答：

> 我不是只通过 wall time 猜的。首先 strace 发现 Worker 增多后 clock_nanosleep 从 0.3 秒增长到 40 多秒；然后源码搜索发现 SqliteConnection 设置了 sqlite3_busy_timeout 5000 ms；最后我只跟踪 clock_nanosleep，抓到了 1、2、5、10、15、20、25、50、100 ms 的递增退避序列。结合多个 Worker 持有独立 SQLite connection 并发写同一数据库，这个证据链说明主要 sleep 与 SQLite busy waiting 高度吻合。

---

# 58. 面试官可能追问：WAL 为什么还有写竞争

回答：

> WAL 主要改善的是读写并发，让 reader 和 writer 更容易同时工作，但并不等于多个 writer 可以无限并行提交。多个写事务仍然需要协调数据库写锁，所以 Worker 数增加以后，多个连接同时更新任务状态时仍然可能进入 busy handler。

---

# 59. 面试官可能追问：perf 为什么 8 Worker 只用了 1.4 个核

回答：

> Worker 数只是线程数量，不等于这些线程一直处于运行态。实际 migrate 里有文件 I/O、fdatasync、fsync、SQLite busy wait 和线程同步，大量时间线程处于等待状态，所以 8 个 Worker 的平均 CPU 利用并不会达到 8 个核。

---

# 60. 面试官可能追问：为什么还要用 strace，perf 不够吗

回答：

> perf stat 能告诉我 CPU 利用、上下文切换和迁核发生了变化，但不能直接告诉我线程具体在等什么。strace 能看到系统调用，所以我先用 perf 判断“CPU 没继续扩展”，再用 strace 找到大量 clock_nanosleep，最后做定向 trace 才把等待原因落到 SQLite busy timeout 上。

---

# 61. 这轮优化最值得写进简历的表述

可以写成类似：

> 为 migrate 阶段引入可配置多 Worker 文件级并行，并基于 1 GiB E2E workload 对 1/2/4/8 Worker 进行 Benchmark；在 8 vCPU Linux VM 上，4 Worker 将平均耗时由 12.47 s 降至 8.99 s，端到端吞吐由 82.1 MiB/s 提升至 113.9 MiB/s，约 1.39× 加速；结合 perf 与 strace 定位 4 Worker 后扩展受线程调度和 SQLite busy 等待限制，并据此选择 4 Worker 作为当前环境下的默认并发度。

注意：

```text
“4 Worker 默认”
```

应限定为：

```text
当前测试环境
```

不要包装成所有机器的通用最优配置。

---

# 62. 本轮测试还有哪些没有覆盖

本轮没有覆盖：

```text
SMB 网络共享
冷缓存存储
真实手机上传链路
机械硬盘
不同 SSD
不同 CPU 核数
NUMA
大量小文件 workload
超大文件 workload
```

因此这份报告回答的是：

```text
Multi-worker 在当前本地 E2E Benchmark 中的扩展行为
```

不是：

```text
PhotoBridge 在所有硬件上的最终性能上限
```

---

# 63. 后续如果继续优化 Multi-worker，可以考虑什么

本轮已经证明：

```text
4 Worker 后 SQLite writer contention 明显增强
```

因此如果未来专门继续优化这一项，可以研究：

```text
Worker
只负责 copy/hash/文件操作
↓
状态更新 command
进入独立队列
↓
单独 DB writer
批量提交 SQLite
```

也就是把：

```text
N 个 Worker
×
N 个 SQLite writer
```

逐渐改造成：

```text
N 个文件 Worker
+
1 个集中 SQLite writer
```

潜在好处：

```text
减少 writer lock contention
减少 busy_timeout 退避
更容易批量事务
```

但这是下一轮新的架构优化。

本轮第二项优化已经完成验证，不需要为了“让 Benchmark 更漂亮”继续修改设计。

---

# 64. 最终一句话总结

```text
PhotoBridge 的 Multi-worker 优化成功把文件级迁移并行化：
1 → 4 Worker 将 1 GiB E2E 平均耗时从 12.47 s 降到 8.99 s，
实现约 1.39× 加速；
但 4 → 8 Worker 仅再提升约 1.6%。
perf、strace 与定向 clock_nanosleep trace 进一步证明，
随着 Worker 增多，线程调度开销和 SQLite writer busy waiting
逐渐成为扩展瓶颈，因此当前 8 vCPU VMware 环境下
4 Worker 是更合理的性能/资源折中点。
```

---

# 65. 本轮状态

```text
第二项优化：Multi-worker 并行迁移
代码实现：完成
Release Benchmark：完成
1/2/4/8 Worker 对比：完成
8 Worker 异常复测：完成
perf 分析：完成
strace 分析：完成
SQLite busy 定向验证：完成
debug 正确性回归：全部通过
性能结论：完成
瓶颈定位：完成
```

**本轮第二项性能优化测试正式收口。**
