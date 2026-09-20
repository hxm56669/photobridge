# PhotoBridge V4 —— BLAKE3 SIMD 性能验证与瓶颈分析报告

> 项目：PhotoBridge  
> 阶段：第三项性能优化 / V4  
> 主题：BLAKE3 SIMD 实际生效验证、CPU 热点分析、Worker 扩展性验证与 TBB 取舍  
> 当前 Git 基线：`1b5cd4ad7da074fb46ddf46922382e5631771388`  
> 测试环境：Ubuntu 22.04 / Linux 5.15 / VMware / 8 vCPU  
> 结论：**生产哈希路径不修改，继续使用官方 BLAKE3 AVX2 SIMD；不引入 TBB。**

---

# 一、这次 V4 优化到底做了什么

第三项最初的优化目标不是“强行把 BLAKE3 改成 SIMD”，而是先回答三个问题：

```text
1. 当前 BLAKE3 到底有没有 SIMD？
2. 真实 PhotoBridge Copy / E2E 路径有没有走 SIMD？
3. 如果已经走 SIMD，还有没有必要继续上 TBB 多线程 Hash？
```

最终证据链为：

```text
Ubuntu / VM 暴露 AVX2
↓
项目链接的官方 BLAKE3 库存在 AVX2 实现
↓
诊断程序对 1 MiB 输入记录到 AVX2 函数调用 257 次
↓
真实 Copy perf 采样出现 blake3_hash_many_avx2
↓
真实 4 Worker E2E 中 blake3_hash_many_avx2 占用户态 CPU 热点约 87%
↓
1 / 2 / 4 / 8 Worker 实测继续增加 Worker 的收益逐步收敛
↓
4 Worker → 8 Worker 只再提升约 2.9%
↓
不在缺少收益证据时引入 TBB
↓
保留“官方 AVX2 SIMD + 文件级 Worker 并行”
```

因此，V4 的价值不是“又加了一项新技术”，而是：

> **用 CPU 指令集、运行时诊断、perf 热点与 E2E 扩展性数据证明现有 BLAKE3 路径已经正确使用 SIMD，并通过数据拒绝没有必要的 TBB 复杂度。**

---

# 二、V4 前 BLAKE3 生产路径

当前 PhotoBridge 没有自己实现 BLAKE3，也没有自己手写 SSE / AVX2。

核心路径仍然是：

```text
CopyAndHash
↓
LinuxFileOps::Read
↓
Blake3Hasher::Update
↓
blake3_hasher_update
↓
LinuxFileOps::Write
↓
下一块
↓
Blake3Hasher::Finalize
```

当前 Copy 仍是：

```text
read
→ BLAKE3
→ write
→ 下一块
```

另外，迁移主链中 BLAKE3 不只执行一次：

```text
source
↓
CopyAndHash
↓
source digest
↓
fdatasync(temp)
↓
重新打开 temp
↓
VerifyBinary
↓
再次计算 BLAKE3
```

最终 `verify` 阶段还会重新读取 source 和 final 再做独立 Hash。

这意味着：

```text
BLAKE3 是 PhotoBridge 数据面的核心 CPU 工作之一
```

但第二次 Hash 属于独立验证语义，**不能为了性能直接删除**。

---

# 三、V4 实际代码改动

本轮没有修改生产哈希路径。

没有做：

```text
没有改 CopyAndHash
没有改 Blake3Hasher
没有改 blake3_hasher_update
没有引入 blake3_hasher_update_tbb
没有手写 AVX2
没有增加线程池式 Hash
没有修改 benchmark 实现
```

只在：

```text
tests/unit/filesystem/copy_and_hash_test.cpp
```

增加了官方 BLAKE3 multi-chunk 正确性测试：

```cpp
TEST(CopyAndHashTest, Blake3MatchesOfficialMultiChunkVector)
```

测试输入：

```text
16 KiB
内容按照 0..250 循环
```

验证两种方式：

```text
A. 一次性 Update 16 KiB
B. 每次 1024 B，连续流式 Update
```

两种方式都必须得到同一个官方 BLAKE3 digest：

```text
f875d6646de28985646f34ee13be9a576fd515f76b5b0a26bb324735041ddde4
```

这个测试的意义：

```text
验证官方多块输入向量
+
验证 PhotoBridge Blake3Hasher 的 streaming 语义
+
避免后续改 Hash 路径时破坏多块输入正确性
```

---

# 四、测试环境

保存环境信息使用：

```bash
mkdir -p benchmark-results/v3-blake3/perf

{
  echo "=== Git ==="
  git rev-parse HEAD
  git status --short

  echo
  echo "=== Kernel ==="
  uname -a

  echo
  echo "=== CPU ==="
  lscpu

  echo
  echo "=== CPU Flags ==="
  grep -m1 '^flags' /proc/cpuinfo

  echo
  echo "=== Compiler ==="
  g++ --version

  echo
  echo "=== Perf ==="
  perf --version
} > benchmark-results/v3-blake3/environment.txt
```

## 4.1 命令解释

### `mkdir -p`

```text
mkdir
→ 创建目录

-p
→ 父目录不存在时一起创建
→ 目录已经存在时不报错
```

### `git rev-parse HEAD`

```text
git rev-parse
→ 解析 Git 引用

HEAD
→ 当前检出的提交
```

用于固定本次实验对应的源码版本。

### `git status --short`

```text
--short
→ 使用简短格式显示工作区改动
```

用于记录 benchmark 是否在脏工作区执行。

### `uname -a`

输出：

```text
Kernel
主机架构
内核版本
```

### `lscpu`

输出：

```text
CPU 型号
逻辑 CPU 数量
核心拓扑
缓存
虚拟化信息
CPU Feature Flags
```

### `grep -m1 '^flags' /proc/cpuinfo`

```text
grep
→ 查找文本

-m1
→ 找到第一条后停止

^flags
→ 只匹配以 flags 开头的行
```

用于单独保存 CPU 指令集能力。

## 4.2 实际环境

```text
CPU：13th Gen Intel Core i7-13700F
VM：VMware
vCPU：8
Kernel：Linux 5.15.0-185-generic
Compiler：g++ 11.4.0
perf：5.15.200
```

CPU Flags 中明确存在：

```text
sse2
sse4_1
avx
avx2
```

没有：

```text
avx512f
```

因此本机能够运行 BLAKE3 AVX2 路径。

---

# 五、AVX2 实际生效验证

只看到 CPU 支持 AVX2 不够。

还需要区分：

```text
CPU 支持 AVX2
≠
BLAKE3 库编译了 AVX2
≠
运行时一定使用 AVX2
```

本轮已经额外使用诊断程序对项目实际链接的 BLAKE3 库进行了运行时检查。

输入：

```text
1 MiB
```

结果：

```text
AVX2 函数调用次数：257 次
```

因此可以确认：

```text
项目实际链接的 BLAKE3
→ 在当前 CPU / VM 环境下
→ 确实会进入 AVX2 路径
```

后续 `perf` 又在真实 PhotoBridge 路径中观察到：

```text
blake3_hash_many_avx2
```

因此不是“测试程序能走 AVX2，但生产路径没走”。

---

# 六、Copy 1 GiB 基线

命令：

```bash
mkdir -p benchmark-results/v3-blake3

./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 5 \
  --output benchmark-results/v3-blake3/copy-1g.json \
  | tee benchmark-results/v3-blake3/copy-1g.txt
```

## 6.1 参数解释

### `./build/bench-release/photobridge_bench`

执行 Release 模式的 benchmark 可执行文件。

### `--workload copy`

只运行 Copy workload。

这一 workload 实际包含：

```text
read
+
BLAKE3
+
write
+
fdatasync
```

所以：

> **Copy 吞吐不能直接等价为 BLAKE3 吞吐。**

### `--copy-bytes 1073741824`

```text
1073741824 bytes
=
1024 MiB
=
1 GiB
```

### `--repetitions 5`

同一个 workload 重复执行 5 次。

### `--output xxx.json`

把结构化结果写入 JSON。

### `| tee xxx.txt`

```text
|
→ 把左边程序的标准输出传给右边

tee
→ 一边显示在终端
→ 一边写入文件
```

## 6.2 实测结果

```text
wall ms:
min  = 3704.84
mean = 7922.54
max  = 22821.61

平均 bytes/s：220,537,878
平均 CPU 利用率：82.77%
```

这里出现一次：

```text
22.82 s
```

的明显长尾。

因此这一组只作为 Copy 大样本观察，不直接拿均值作为最终稳定性能结论。

---

# 七、perf stat：整体 CPU 行为

命令：

```bash
perf stat -r 5 \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults \
  -o benchmark-results/v3-blake3/perf/copy-1g-stat.txt \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

## 7.1 `perf stat`

```text
perf stat
→ 统计整个进程执行期间的性能计数器
```

### `-r 5`

```text
-r
→ repeat

-r 5
→ 整条命令重复执行 5 次
→ perf 自动给出均值和波动
```

### `-e`

指定要采集的事件。

本次包括：

```text
task-clock
→ 进程真正占用 CPU 的时间

cycles
→ CPU 周期数

instructions
→ 执行的机器指令数

branches
→ 分支指令

branch-misses
→ 分支预测失败

cache-references
→ Cache 访问

cache-misses
→ Cache Miss

context-switches
→ 上下文切换次数

cpu-migrations
→ 线程迁移 CPU 的次数

page-faults
→ 缺页次数
```

### `-o`

把 perf 输出写入文件。

### `--`

表示：

```text
perf 自己的参数到这里结束
后面全部是被测程序及其参数
```

## 7.2 实测结果

```text
task-clock：8386.10 ms
CPU utilization：0.786 CPUs
context-switches：284
cpu-migrations：7
page-faults：677
elapsed：10.67 s
```

但：

```text
cycles             <not supported>
instructions       <not supported>
branches           <not supported>
branch-misses      <not supported>
cache-references   <not supported>
cache-misses       <not supported>
```

## 7.3 原因

当前运行在 VMware VM 中。

Guest 没有获得这些硬件 PMU 事件，因此：

```text
perf 软件事件可用
硬件 Performance Counter 不可用
```

因此本轮不能用：

```text
IPC
cycles
cache miss rate
branch miss rate
```

做结论。

这也是为什么后续主要使用：

```text
cpu-clock 用户态采样
```

定位热点。

---

# 八、perf record：真实 Copy 热点

第一次命令：

```bash
perf record -e cpu-clock:u -g \
  -o benchmark-results/v3-blake3/perf/copy-1g-user.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 1
```

导出：

```bash
perf report --stdio \
  -i benchmark-results/v3-blake3/perf/copy-1g-user.data \
  > benchmark-results/v3-blake3/perf/copy-1g-user-report.txt

head -80 benchmark-results/v3-blake3/perf/copy-1g-user-report.txt
```

## 8.1 参数解释

### `perf record`

```text
按照一定频率采样程序运行时热点
→ 保存 perf.data
```

### `-e cpu-clock:u`

```text
cpu-clock
→ 软件 CPU 时钟采样事件

:u
→ user mode only
→ 只采用户态
```

所以这个报告不能表示：

```text
总 wall time 占比
```

只能表示：

```text
用户态 CPU 样本分布
```

### `-g`

```text
记录调用栈
→ 可以分析调用关系
```

### `perf report --stdio`

```text
把 perf.data 转成文本报告
```

### `-i`

指定输入 perf.data。

### `head -80`

只看报告前 80 行。

## 8.2 第一次结果

```text
WriteDeterministicFile   73.29%
blake3_hash_many_avx2    26.21%
```

这次不能直接说：

```text
BLAKE3 只占 26%
```

原因是 `perf record` 从整个 benchmark 进程开始采样，而 benchmark 在正式 Copy 前会生成测试文件：

```text
WriteDeterministicFile
```

所以数据准备本身也被采进来了。

---

# 九、用 10 次 Copy 摊薄数据准备成本

为了减少一次性 `WriteDeterministicFile` 对采样结果的影响，改成：

```bash
perf record -e cpu-clock:u -g \
  -o benchmark-results/v3-blake3/perf/copy-1g-user-10x.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload copy \
  --copy-bytes 1073741824 \
  --repetitions 10
```

报告：

```bash
perf report --stdio \
  -i benchmark-results/v3-blake3/perf/copy-1g-user-10x.data \
  > benchmark-results/v3-blake3/perf/copy-1g-user-10x-report.txt

head -60 benchmark-results/v3-blake3/perf/copy-1g-user-10x-report.txt
```

结果：

| 用户态热点 | Self |
|---|---:|
| `blake3_hash_many_avx2` | **78.21%** |
| `WriteDeterministicFile` | 20.39% |
| `blake3_compress_subtree_wide` | 0.47% |
| `blake3_hash_many` | 0.14% |
| `CopyAndHash` | 0.14% |
| `write` | 0.11% |

这说明：

```text
把 benchmark 准备阶段摊薄以后
→ blake3_hash_many_avx2 成为 Copy 主链最明显的用户态 CPU 热点
```

但必须准确表达：

> **78.21% 是用户态 CPU 样本，不是 78.21% 的总 wall time。**

I/O 等待和内核态时间没有包含在这个比例里。

---

# 十、4 Worker E2E 的 perf 热点

命令：

```bash
perf record -e cpu-clock:u -g \
  -o benchmark-results/v3-blake3/perf/e2e-large-4w-user.data \
  -- \
  ./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 4 \
  --repetitions 3
```

导出：

```bash
perf report --stdio \
  -i benchmark-results/v3-blake3/perf/e2e-large-4w-user.data \
  > benchmark-results/v3-blake3/perf/e2e-large-4w-user-report.txt

head -80 benchmark-results/v3-blake3/perf/e2e-large-4w-user-report.txt
```

结果：

| 用户态热点 | Self |
|---|---:|
| `blake3_hash_many_avx2` | **87.07%** |
| `WriteDeterministicFile` | 10.07% |
| `blake3_compress_subtree_wide` | 0.56% |
| `blake3_hash_many` | 0.16% |
| MigrationService worker lambda | 0.18% |
| SQLite `sqlite3VdbeExec` | 0.08% |
| `read` | 0.07% |

这一步非常关键，因为它证明：

```text
不是只有单独 Copy workload 会走 AVX2
↓
真实 4 Worker E2E 主链也会走 blake3_hash_many_avx2
```

而且在用户态 CPU 时间中，BLAKE3 是明显主热点。

---

# 十一、Worker 扩展性实验

为了判断：

```text
文件级 Worker 并行是否已经足够
```

分别测试：

```text
1 Worker
2 Worker
4 Worker
8 Worker
```

固定：

```text
workload：e2e-large
64 files × 16 MiB
总输入：1 GiB
每组：3 repetitions
```

---

## 11.1 1 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 1 \
  --repetitions 3 \
  --output benchmark-results/v3-blake3/e2e-large-1w.json \
  | tee benchmark-results/v3-blake3/e2e-large-1w.txt
```

结果：

```text
wall mean：12.432 s
CPU utilization：93.43%
peak RSS：9288 KiB
吞吐：约 82.37 MiB/s
```

解释：

```text
CPU≈93%
→ 基本相当于持续占用 1 个 CPU 核
```

---

## 11.2 2 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 2 \
  --repetitions 3 \
  --output benchmark-results/v3-blake3/e2e-large-2w.json \
  | tee benchmark-results/v3-blake3/e2e-large-2w.txt
```

结果：

```text
wall mean：10.038 s
CPU utilization：123.70%
peak RSS：10964 KiB
吞吐：约 102.01 MiB/s
```

相对 1 Worker：

```text
加速比：1.238×
wall time 降低：约 19.25%
```

---

## 11.3 4 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 4 \
  --repetitions 3 \
  --output benchmark-results/v3-blake3/e2e-large-4w.json \
  | tee benchmark-results/v3-blake3/e2e-large-4w.txt
```

结果：

```text
wall mean：8.791 s
CPU utilization：145.08%
peak RSS：13436 KiB
吞吐：约 116.48 MiB/s
```

相对 1 Worker：

```text
加速比：1.414×
wall time 降低：约 29.29%
```

---

## 11.4 8 Worker

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 8 \
  --repetitions 3 \
  --output benchmark-results/v3-blake3/e2e-large-8w.json \
  | tee benchmark-results/v3-blake3/e2e-large-8w.txt
```

结果：

```text
wall mean：8.536 s
CPU utilization：151.76%
peak RSS：17920 KiB
吞吐：约 119.96 MiB/s
```

相对 1 Worker：

```text
加速比：1.456×
wall time 降低：约 31.34%
```

相对 4 Worker：

```text
加速：1.030×
wall time 仅再降低：约 2.90%
```

---

# 十二、Worker 扩展曲线总表

| Worker | Wall Mean | 吞吐 | CPU 利用率 | Peak RSS | 相对 1W 加速 |
|---:|---:|---:|---:|---:|---:|
| 1 | 12.432 s | 82.37 MiB/s | 93.43% | 9288 KiB | 1.000× |
| 2 | 10.038 s | 102.01 MiB/s | 123.70% | 10964 KiB | 1.238× |
| 4 | 8.791 s | 116.48 MiB/s | 145.08% | 13436 KiB | 1.414× |
| 8 | 8.536 s | 119.96 MiB/s | 151.76% | 17920 KiB | 1.456× |

趋势：

```text
1W → 2W
收益明显

2W → 4W
仍有明显收益

4W → 8W
仅约 2.9% 改善
```

说明：

```text
文件级并行扩展开始接近平台
```

不是说：

```text
CPU 已经 100% 吃满 8 核
```

实际上 CPU 利用率仍只有约 152%，相当于平均约 1.52 个逻辑 CPU。

因此这里真正说明的是：

> **当前 E2E 受 Hash + I/O + fsync + runtime 状态推进等多项因素共同约束；简单增加 Worker 已经不能线性换取吞吐。**

---

# 十三、为什么不引入 TBB

原计划中可以继续实验：

```text
blake3_hasher_update_tbb()
```

它代表：

```text
单文件内部
→ 使用多个线程参与 BLAKE3 Hash
```

而当前 Worker Pool 已经实现：

```text
Worker1 → File A → BLAKE3 SIMD
Worker2 → File B → BLAKE3 SIMD
Worker3 → File C → BLAKE3 SIMD
Worker4 → File D → BLAKE3 SIMD
```

两种并行层次不同：

```text
Worker Pool
→ 文件级并行

TBB BLAKE3
→ 单文件 Hash 内部并行
```

如果无条件叠加：

```text
4 Worker
×
每个 Worker 内部再开 TBB 线程
```

可能导致：

```text
线程数膨胀
→ oversubscription
→ context switch 增加
→ Cache 局部性变差
→ 调度成本上升
→ 实际性能反而下降
```

当前证据已经说明：

```text
AVX2 SIMD 已生效
+
BLAKE3 是主要用户态热点
+
Worker 已经提供文件级并行
+
4W→8W 收益只约 2.9%
```

因此当前合理工程决策是：

> **不在没有明确收益证据的情况下引入 TBB。**

这不是“没做优化”，而是：

```text
先验证现状
→ 再测瓶颈
→ 再测试并行扩展
→ 发现进一步复杂化缺少收益依据
→ 主动停止
```

这是一次完整的性能优化决策闭环。

---

# 十四、最终正式 4 Worker 基准

第三项收口前，按正式口径再跑：

```text
4 Worker
1 GiB
10 repetitions
```

命令：

```bash
./build/bench-release/photobridge_bench \
  --workload e2e-large \
  --e2e-workers 4 \
  --repetitions 10 \
  --output benchmark-results/v3-blake3/e2e-large-4w-final.json \
  | tee benchmark-results/v3-blake3/e2e-large-4w-final.txt
```

结果：

```text
wall min：8.402 s
wall mean：8.568 s
wall max：8.767 s
CPU mean：12.368 s
CPU utilization mean：144.35%
Peak RSS：13460 KiB
```

换算吞吐：

```text
1 GiB / 8.568 s
≈ 119.51 MiB/s
```

10 次的范围：

```text
8.402 s ～ 8.767 s
```

波动较小，说明这组 4 Worker E2E 数据稳定。

---

# 十五、最终测试回归

生产路径没有改，但增加了新的官方向量测试，因此仍然执行完整 Debug 回归：

```bash
cmake --preset debug
cmake --build --preset debug -j2
ctest --preset debug --output-on-failure
```

## 15.1 参数解释

### `cmake --preset debug`

使用项目中名为 `debug` 的 CMake configure preset。

### `cmake --build --preset debug`

使用 debug build preset 构建。

### `-j2`

最多并行执行 2 个构建任务。

### `ctest --preset debug`

使用 debug 对应的测试 preset。

### `--output-on-failure`

只有测试失败时打印该测试的详细输出。

## 15.2 结果

```text
218 / 218 tests passed
```

结论：

```text
新增 BLAKE3 multi-chunk test 通过
+
全项目回归通过
+
生产哈希路径未发生行为变化
```

---

# 十六、Git 状态与改动边界

本轮最终应该只保留：

```text
M tests/unit/filesystem/copy_and_hash_test.cpp
```

benchmark 输出目录：

```text
benchmark-results/v3-blake3/
```

属于测试证据。

过程中曾出现：

```text
D benchmark-results/v4-blake3/.gitkeep
```

这是无关删除，已经要求通过：

```bash
git restore benchmark-results/v4-blake3/.gitkeep
```

恢复。

### `git restore`

```text
把工作区文件恢复到当前 Git 版本
```

用于避免把无关改动带进第三项提交。

---

# 十七、当前瓶颈怎么理解

## 17.1 用户态 CPU 主要瓶颈：BLAKE3

真实 4 Worker E2E：

```text
blake3_hash_many_avx2
≈ 87.07% 用户态 CPU samples
```

因此从：

```text
用户态 CPU 热点
```

看，BLAKE3 是主热点。

但不能写成：

```text
BLAKE3 占总耗时 87%
```

因为 `cpu-clock:u` 不统计：

```text
I/O wait
内核态
阻塞时间
fdatasync 等待
SQLite 锁等待中的非运行时间
```

---

## 17.2 E2E 不是纯 CPU-bound

如果系统完全 CPU-bound，而且 4 Worker 都能持续跑 Hash，那么：

```text
CPU 利用率应该明显接近 400%
```

实际：

```text
4 Worker：约 144%
8 Worker：约 152%
```

说明：

```text
Worker 并不是一直在 CPU 上计算 Hash
```

过程中还会遇到：

```text
read / write
fdatasync
SQLite 状态推进
文件打开与关闭
验证阶段
Worker 调度与等待
```

因此：

> **BLAKE3 是用户态 CPU 热点，但不是整个 wall time 的唯一瓶颈。**

---

## 17.3 4 → 8 Worker 收益变小

```text
4 Worker：8.791 s
8 Worker：8.536 s
```

只提升：

```text
约 2.9%
```

说明：

```text
继续单纯增加 Worker
→ 已经进入边际收益快速下降阶段
```

所以第三项不应该继续靠：

```text
无限加 Worker
```

也不应该直接：

```text
再套一层 TBB
```

下一阶段更值得验证的是：

```text
同步 read → hash → write 流水
```

本身是否造成 I/O 串行等待。

这正好对应第四项优化：

```text
io_uring / 多 Buffer Pipeline
```

---

# 十八、这次实验的限制

## 18.1 VMware PMU 不完整

硬件性能计数器不可用：

```text
cycles
instructions
cache-references
cache-misses
branches
branch-misses
```

因此不能声称：

```text
IPC 提升多少
Cache Miss 降低多少
分支预测改善多少
```

当前没有数据支持这些结论。

---

## 18.2 `kernel read bytes = 0`

多组 benchmark 中：

```text
kernel read bytes = 0
```

但 read syscall 数量很多。

这通常意味着：

```text
读取大量命中 page cache
```

所以当前结果更接近：

```text
热缓存 / 内存缓存条件下
CPU + 文件系统路径表现
```

而不是冷盘真实物理读取性能。

因此不能把本轮数据直接推广到：

```text
机械盘
SMB
NAS
冷缓存
手机网络上传
```

---

## 18.3 Copy 1 GiB 有一次长尾

5 次 Copy 中出现：

```text
22.82 s
```

明显慢于其他样本。

本轮没有进一步把它归因于：

```text
host 调度
VM 抖动
磁盘写回
fdatasync
后台任务
```

因此对这一异常只记录，不强行解释。

正式 E2E 4 Worker 10 次数据反而非常稳定，因此最终收口主要使用后者。

---

# 十九、第三项 V4 的最终结论

## 19.1 事实

```text
1. 当前 CPU / VM 暴露 AVX2。
2. 项目实际链接的 BLAKE3 运行时进入 AVX2。
3. 真实 Copy 与真实 E2E 都采样到了 blake3_hash_many_avx2。
4. 4 Worker E2E 中 BLAKE3 占约 87% 用户态 CPU 样本。
5. Worker 1→2→4 有明显收益。
6. Worker 4→8 只继续提升约 2.9%。
7. 当前没有 TBB 带来收益的实验数据。
8. 当前生产 Hash 路径保持不变。
9. 新增官方 multi-chunk 正确性测试。
10. Debug 全量 218/218 通过。
```

## 19.2 工程决策

```text
保留：
官方 BLAKE3
+
运行时 AVX2 SIMD
+
文件级 Worker 并行

不增加：
TBB 单文件多线程 Hash
```

原因：

```text
SIMD 已经生效
+
现有并行已经有明显收益
+
4W→8W 已出现边际收益快速下降
+
TBB 会与 Worker Pool 形成双层并行
+
没有证据证明复杂度值得
```

---

# 二十、面试可以怎么讲

推荐 1～2 分钟版本：

```text
第三项优化我没有直接去“开启 AVX2”，因为官方 BLAKE3 本身就支持运行时 SIMD 分派。

我先检查 VM CPU Flags，确认系统暴露了 AVX2；然后用项目实际链接的 BLAKE3 做诊断，1 MiB 输入里记录到 AVX2 路径被调用 257 次。

之后我用 perf 对真实 Copy 和 4 Worker E2E 做用户态采样，热点里明确出现 blake3_hash_many_avx2。把 benchmark 数据准备成本摊薄后，Copy 中它大约占 78% 的用户态 CPU 样本；4 Worker E2E 中约占 87%。

这说明 BLAKE3 确实是主要用户态 CPU 热点，但我没有直接上 TBB。因为项目第二项已经做了文件级 Worker 并行，所以我继续测了 1、2、4、8 Worker。E2E 从大约 12.43 秒下降到 10.04、8.79、8.54 秒，4 到 8 Worker 只再提升约 2.9%。

因此我认为当前已经进入并行收益平台，再给每个 Worker 内部叠加 TBB 容易产生过度订阅，而且没有性能数据证明值得。所以第三项最终没有修改生产 Hash 路径，而是保留官方 AVX2 SIMD，并补了官方 multi-chunk test vector 做正确性回归。

这个优化的重点不是堆技术，而是先证明 SIMD 已经生效，再用 perf 和扩展性数据决定不引入额外复杂度。
```

---

# 二十一、面试追问准备

## Q1：为什么 AVX2 已经用了，BLAKE3 还是热点？

```text
SIMD 只能降低单位字节的计算成本，不能让 Hash 变成零成本。
PhotoBridge 迁移和独立验证都会读取并计算 BLAKE3，大文件上 Hash 数据量很大，所以即使 AVX2 已经生效，它仍然可能是用户态 CPU 主热点。
```

## Q2：为什么不直接上 TBB？

```text
因为项目已经有文件级 Worker Pool。
TBB 会形成“文件级并行 + 单文件内部并行”的双层并行。
没有调参就叠加容易造成 oversubscription。
我实际测了 1/2/4/8 Worker，4→8 已经只提升约 2.9%，所以没有数据证明 TBB 值得增加复杂度。
```

## Q3：87% 是不是说明总时间 87% 都在 BLAKE3？

```text
不是。
这是 perf 的 cpu-clock:u 用户态 CPU 采样占比，只代表进程真正运行在用户态 CPU 上时，样本主要落在 BLAKE3。
I/O 等待、内核态、fdatasync 等都不在这个比例里。
```

## Q4：为什么 8 Worker 只有 152% CPU？

```text
因为 E2E 不是纯 Hash benchmark。
Worker 还要做 read/write、fdatasync、SQLite 状态推进、验证和文件系统操作。
因此线程存在等待，不会 8 个 Worker 始终同时在 CPU 上计算。
```

## Q5：为什么不删第二次 Hash？

```text
第二次 Hash 属于独立验证语义。
Copy-time Hash 只能证明复制过程当时读到了什么字节，不能单独证明最终 temp / final 仍然是正确内容。
为了 Crash Recovery 和最终验证语义，不能用性能优化破坏这一层正确性保证。
```

---

# 二十二、V4 输出文件建议目录

本轮实际输出主要位于：

```text
benchmark-results/v3-blake3/
```

虽然目录名沿用了实验阶段的 `v3-blake3`，但从项目优化版本定义上，本报告把本阶段记为：

```text
V4 / 第三项优化 / BLAKE3 SIMD 验证
```

推荐保留：

```text
benchmark-results/v3-blake3/
├── environment.txt
├── copy-1g.json
├── copy-1g.txt
├── e2e-large-1w.json
├── e2e-large-1w.txt
├── e2e-large-2w.json
├── e2e-large-2w.txt
├── e2e-large-4w.json
├── e2e-large-4w.txt
├── e2e-large-8w.json
├── e2e-large-8w.txt
├── e2e-large-4w-final.json
├── e2e-large-4w-final.txt
└── perf/
    ├── copy-1g-stat.txt
    ├── copy-1g-user.data
    ├── copy-1g-user-report.txt
    ├── copy-1g-user-10x.data
    ├── copy-1g-user-10x-report.txt
    ├── e2e-large-4w-user.data
    └── e2e-large-4w-user-report.txt
```

---

# 二十三、最终一句话

```text
第三项优化最终没有改 BLAKE3 生产实现：
我通过 CPU Flags、运行时诊断和 perf 证明官方库已经实际使用 AVX2，
再通过 1/2/4/8 Worker 扩展实验确认文件级并行已接近收益平台，
因此没有在缺少收益证据的情况下引入 TBB，只补充官方多块输入正确性测试，
并保留“AVX2 SIMD + Worker Pool”作为当前最合理方案。
```
