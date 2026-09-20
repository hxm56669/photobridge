# PhotoBridge 第四项性能优化：io_uring 多缓冲异步 Copy 改造说明

> 仓库：`https://github.com/hxm56669/photobridge.git`
>
> 基线：前三项优化完成后的 PhotoBridge
>
> 本文目标：记录第四项性能优化中 **代码具体改了什么、过去怎么实现、现在怎么实现、为什么这样设计，以及当前实现边界**。
>
> 本文只描述当前仓库真实代码，不把后续计划写成已经实现。

---

# 一、这次优化解决什么问题

第四项优化针对的是 PhotoBridge 文件迁移的数据面。

前三项优化完成后，PhotoBridge 已经具备：

```text
SQLite Prepared Statement + Batch
+
多 Worker Task 并行
+
BLAKE3 SIMD 路径确认
```

但是单个 Worker 处理一个文件时，Copy 路径仍然是严格同步的：

```text
read(source)
↓
等待 read 返回
↓
BLAKE3 Hash 当前块
↓
write(temp)
↓
等待 write 完成
↓
读取下一块
```

也就是说：

```text
一个 chunk 没走完
→ 下一个 chunk 不会开始
```

问题不只是 `read()` / `write()` 系统调用本身有开销。

更重要的是：

```text
磁盘 / 页缓存 I/O
CPU Hash
目标文件写入
```

这三部分无法形成多个请求在途的流水。

第四项优化的目标因此不是：

```text
把 read() 换成 io_uring_prep_read()
把 write() 换成 io_uring_prep_write()
```

而是：

```text
多个 Buffer
+
多个 I/O request 同时 in-flight
+
显式 offset
+
Completion 驱动
+
Hash 顺序控制
```

最终把单文件 Copy 从严格串行改成一个有界的异步流水。

---

# 二、优化前的代码基线

## 2.1 原来的 CopyAndHash

优化前的核心逻辑位于：

```text
src/filesystem/copy_and_hash.cpp
```

调用关系：

```text
MigrationAttemptPreparer
↓
CopyAndHash
↓
FileOps::Read
↓
BLAKE3 Update
↓
FileOps::Write
```

原来的核心循环可以抽象成：

```text
while (true) {
    read(source, buffer)

    if EOF:
        break

    hash(buffer)

    WriteAll(target, buffer)
}
```

其中 `WriteAll()` 负责处理短写：

```text
write
↓
如果只写了一部分
↓
更新 offset
↓
继续 write
↓
直到整个 chunk 写完
```

所以原来的正确性已经比较完整：

```text
EINTR
短写
EOF
Hash
bytes_copied
```

都已经处理。

问题在于：

```text
同步 Read
→ 同步 Hash
→ 同步 Write
→ 下一块
```

整个单文件数据面没有 I/O 重叠。

---

## 2.2 原来的 FileOps 仍然是同步接口

项目底层：

```text
FileOps::Read()
FileOps::Write()
```

本质都是同步调用。

Linux 实现使用：

```text
::read()
::write()
```

并在系统调用被信号中断时处理 `EINTR`。

因此如果简单在 `LinuxFileOps` 内部改成：

```text
submit io_uring
↓
wait CQE
↓
return
```

上层仍然会看到：

```text
Read()
↓
等待
↓
Write()
↓
等待
```

这只是换了系统调用入口，并没有形成真正异步流水。

因此这次没有把 io_uring 强行塞进 `FileOps::Read/Write`。

这是这次架构改动中非常重要的一点。

---

# 三、优化后的总体结构

当前仓库新增：

```text
include/photobridge/filesystem/io_uring_copy_engine.h
src/filesystem/io_uring_copy_engine.cpp
tests/unit/filesystem/io_uring_copy_engine_test.cpp
```

并修改：

```text
src/app/migration_attempt_preparer.cpp
benchmarks/photobridge_bench.cpp
CMakeLists.txt
vcpkg.json
tests/CMakeLists.txt
```

新的主路径变成：

```text
MigrationAttemptPreparer
↓
VerifyBeforeRead
↓
TryIoUringCopyAndHash
↓
io_uring 多缓冲 Copy + BLAKE3
↓
VerifyAfterRead
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

如果当前 Linux 环境在开始 I/O 前无法初始化 io_uring：

```text
TryIoUringCopyAndHash
↓
返回“不可用”
↓
CopyAndHash
↓
继续使用原同步实现
```

因此当前结构是：

```text
                MigrationAttemptPreparer
                         ↓
                  io_uring 可用？
                    ↙          ↘
                  是            否
                  ↓              ↓
        IoUring Copy        Sync Copy
                  ↘              ↙
                      ↓
                 fdatasync
                      ↓
              Independent Verify
                      ↓
                 Receipt
```

---

# 四、新增 io_uring Copy Engine

## 4.1 为什么单独增加 Copy Engine

新增接口：

```text
TryIoUringCopyAndHash(...)
```

它没有修改原来的：

```text
FileOps::Read
FileOps::Write
CopyAndHash
```

而是形成两条并列数据路径：

```text
Sync CopyAndHash
```

和：

```text
io_uring CopyAndHash
```

这样做的原因是：

### 原因一：FileOps 的语义本来就是同步的

如果改变 FileOps，会影响大量已有调用方。

而真正需要 io_uring 的地方主要是：

```text
大文件连续数据 Copy
```

没有必要把整个文件系统抽象都改成异步模型。

### 原因二：保留稳定同步路径作为 fallback

原来的 CopyAndHash 已经经过大量测试。

保留它意味着：

```text
io_uring 不支持
→ 不影响 PhotoBridge 的基本可用性
```

### 原因三：控制第四项优化的影响范围

这次优化只改高频数据面。

没有同时重构：

```text
open
stat
rename
unlink
fdatasync
fsync directory
```

这样更容易证明：

```text
性能路径变了
但 Crash-safe 语义没有变
```

---

# 五、TryIoUringCopyAndHash 的接口设计

当前接口核心参数是：

```text
Hasher&
source_fd
target_fd
source_size
chunk_size
queue_depth
```

默认配置：

```text
chunk_size = 1 MiB
queue_depth = 4
```

Queue Depth 当前限制：

```text
1 <= QD <= 16
```

返回值使用：

```text
StatusOr<optional<CopyResult>>
```

这个返回值实际表达三种状态。

## 状态一：成功完成 io_uring Copy

```text
Status = OK
optional = CopyResult
```

表示：

```text
io_uring 路径已经真实执行
```

## 状态二：io_uring 在任何数据 I/O 开始前不可用

```text
Status = OK
optional = nullopt
```

调用者可以安全回退：

```text
CopyAndHash
```

## 状态三：io_uring 已开始工作，但执行过程中失败

```text
Status = Error
```

这时不会再回退同步 Copy。

这是一个非常重要的设计。

因为如果已经：

```text
读了一部分
+
写了一部分 temp
+
Hasher 已经 Update
```

此时再从头执行同步 Copy：

```text
会重复写数据
或者污染 Hash 状态
```

所以当前规则是：

```text
只有“开始任何 I/O 之前确认不可用”
→ 才允许 fallback

一旦已经提交 I/O
→ 后续错误直接失败
→ 不重新执行 Copy
```

---

# 六、io_uring Context 的生命周期

当前代码增加了：

```text
IoUringContext
```

它负责：

```text
io_uring_queue_init
io_uring_queue_exit
```

属于 RAII 封装。

构造：

```text
初始化 ring
```

析构：

```text
释放 ring
```

并且：

```text
禁止 copy
禁止 copy assignment
```

避免多个对象错误共享同一个 ring 生命周期。

---

# 七、为什么使用 thread_local 缓存 Ring

当前不是每复制一个文件都：

```text
queue_init
↓
Copy
↓
queue_exit
```

而是：

```text
thread_local IoUringContext
```

因此同一个 Worker 线程处理后续文件时，可以复用 ring。

逻辑可以理解成：

```text
Worker1
↓
第一次 Copy
↓
创建 Ring
↓
后续 Task
↓
复用 Ring
```

另一个 Worker：

```text
Worker2
↓
拥有自己的 thread_local Ring
```

所以当前结构实际上是：

```text
Worker1 → Ring1
Worker2 → Ring2
Worker3 → Ring3
Worker4 → Ring4
```

这和第二项 Worker Pool 的结构是自然兼容的。

优点：

```text
避免每个文件反复初始化 io_uring
+
Worker 之间不共享 Ring
+
减少 Ring 本身的同步问题
```

当 Queue Depth 改变时：

```text
cached_depth != queue_depth
↓
重新创建 Ring
```

---

# 八、BufferSlot：多缓冲状态机

io_uring 版本不再只有一个 buffer。

当前每个 Buffer 用：

```text
BufferSlot
```

描述。

核心字段：

```text
bytes
sequence
offset
length
progress
phase
```

可以理解为：

### bytes

真正保存当前 chunk 数据的内存。

### sequence

这是文件中的第几个 chunk。

例如：

```text
sequence = 0
sequence = 1
sequence = 2
...
```

### offset

该 chunk 在文件中的真实字节偏移。

计算：

```text
offset = sequence × chunk_size
```

### length

当前 chunk 实际长度。

最后一个 chunk 可能不足 1 MiB。

### progress

用于处理：

```text
short read
short write
```

### phase

表示 Buffer 当前处于什么阶段。

当前状态：

```text
kFree
kReading
kReady
kWriting
```

状态转换：

```text
FREE
↓
READING
↓
READY
↓
HASH
↓
WRITING
↓
FREE
```

其中 Hash 没有单独保存为一个长期 Phase，因为 Hash 在用户线程中同步完成，然后立即提交 Write。

---

# 九、为什么需要多个 Buffer

假设：

```text
QD = 4
chunk = 1 MiB
```

当前可以同时准备：

```text
Buffer0 → chunk0
Buffer1 → chunk1
Buffer2 → chunk2
Buffer3 → chunk3
```

初始阶段会一次准备多个 Read：

```text
Read chunk0
Read chunk1
Read chunk2
Read chunk3
↓
submit
```

这和过去：

```text
Read chunk0
↓
等
↓
Write chunk0
↓
等
↓
Read chunk1
```

有本质区别。

多个 Buffer 让内核能够同时持有多个文件 I/O request。

---

# 十、显式 offset：为什么不能继续依赖 fd 当前偏移

同步版本：

```text
read(fd)
write(fd)
```

可以依赖文件描述符内部的 current offset。

但是 io_uring 允许多个请求同时在途。

例如：

```text
Read chunk0
Read chunk1
Read chunk2
Read chunk3
```

完成顺序可能是：

```text
chunk1
chunk3
chunk0
chunk2
```

如果多个请求依赖同一个 fd current position：

```text
很难保证每个请求对应正确的文件区域
```

所以当前每个 SQE 都明确传入：

```text
offset + progress
```

Read：

```text
source_fd
buffer
remaining
explicit offset
```

Write：

```text
target_fd
buffer
remaining
explicit offset
```

因此每个 I/O request 都明确知道：

```text
自己应该读/写文件中的哪一段
```

不依赖共享文件偏移。

---

# 十一、SQE 如何关联回 Buffer

一个 io_uring 请求完成以后，CQE 必须能够回答：

```text
这是哪个 Buffer？
这是 Read 还是 Write？
```

当前使用：

```text
user_data
```

编码。

逻辑：

```text
index * 2 + operation_bit
```

其中：

```text
偶数 → Read
奇数 → Write
```

完成后：

```text
index = user_data / 2
write = user_data & 1
```

然后找到：

```text
slots[index]
```

这相当于给每个异步请求绑定一个轻量 Request ID。

---

# 十二、Submission：为什么不能假设一次 submit 全部成功

当前维护两个计数：

```text
queued
in_flight
```

### queued

已经放进 SQ，但是还没有确认提交给内核。

### in_flight

已经提交，正在等待 CQE 的请求。

`flush()` 不假设：

```text
io_uring_submit()
```

一定一次提交所有 queued SQE。

当前逻辑是：

```text
while queued != 0:
    submitted = io_uring_submit()

    queued -= submitted
    in_flight += submitted
```

同时检查：

```text
submitted < 0
submitted == 0
submitted > queued
```

这样避免因为部分 submit 导致请求计数错误。

---

# 十三、初始填充 Queue Depth

文件大小先计算为：

```text
chunk_count
```

真正创建的 Buffer 数量：

```text
slot_count = min(queue_depth, chunk_count)
```

例如：

```text
文件 = 2 MiB
chunk = 1 MiB
QD = 8
```

实际上只需要：

```text
2 个 Slot
```

不会为了 QD=8 无意义创建 8 个数据 Buffer。

如果是大文件：

```text
QD = 4
```

第一轮：

```text
slot0 → chunk0 Read
slot1 → chunk1 Read
slot2 → chunk2 Read
slot3 → chunk3 Read
```

然后统一 submit。

---

# 十四、Completion 驱动，而不是 submit 一个等一个

真正的核心循环：

```text
while (in_flight != 0)
```

通过：

```text
io_uring_wait_cqe
```

获得完成事件。

这里处理：

```text
EINTR
CQE user_data
CQE result
Buffer phase
short I/O
negative result
```

这和“伪异步”最大的区别是：

错误做法：

```text
submit Read0
↓
wait Read0
↓
submit Write0
↓
wait Write0
↓
submit Read1
```

当前实现：

```text
submit Read0
submit Read1
submit Read2
submit Read3
↓
等待任意 Completion
↓
根据完成的 Buffer 推进对应状态
↓
继续保持多个 request 在途
```

因此当前确实存在：

```text
QD > 1
```

而不是仅仅用了 io_uring API。

---

# 十五、BLAKE3 为什么不能跟着 CQE 完成顺序直接 Hash

这是第四项优化最关键的正确性问题之一。

I/O Completion 可能乱序。

例如：

```text
chunk2 先完成
chunk0 第二个完成
chunk1 最后完成
```

但是 BLAKE3 输入必须保持原文件字节顺序：

```text
chunk0
→ chunk1
→ chunk2
```

不能：

```text
chunk2
→ chunk0
→ chunk1
```

否则最终 Digest 会改变。

因此当前增加：

```text
sequence
next_hash
```

Read 完成时，只把 Buffer 标记为：

```text
READY
```

然后 `hash_ready()` 查找：

```text
phase == READY
&&
sequence == next_hash
```

只有找到了“下一块应该 Hash 的 chunk”才会：

```text
Hasher::Update
↓
next_hash++
↓
提交这个 Buffer 的 Write
```

如果：

```text
chunk2 已完成
chunk1 还没完成
```

那么：

```text
chunk2 先保持 READY
```

等待：

```text
chunk1
```

Hash 顺序因此始终是：

```text
0 → 1 → 2 → 3 → ...
```

这保证了：

```text
异步 I/O 可以乱序
但 Hash 语义不能乱序
```

---

# 十六、Read 完成以后为什么先 Hash 再 Write

当前一个 Slot 的主要流程：

```text
Read Completion
↓
READY
↓
等到自己的 sequence == next_hash
↓
BLAKE3 Update
↓
提交 Write
```

这样可以保证：

```text
Buffer 在 Write 完成前不会被复用
```

因为 Write 使用的仍然是这个 Buffer 中的数据。

状态直到：

```text
Write CQE
```

回来以后才变成：

```text
FREE
```

这是 Buffer 生命周期管理最重要的规则：

```text
Write Completion 前
→ Buffer 内容必须保持有效
→ 不能拿去读取新的 chunk
```

---

# 十七、Write 完成后如何继续流水

当一个完整 Write 完成：

```text
slot.phase = FREE
bytes_copied += slot.length
completed++
```

如果后面还有文件数据：

```text
start_read(index)
```

直接复用这个 Slot：

```text
旧 chunk 写完
↓
Slot FREE
↓
分配下一个 sequence
↓
计算新 offset
↓
提交下一次 Read
```

例如 QD=4：

```text
slot0: chunk0 → write complete → chunk4
slot1: chunk1 → write complete → chunk5
slot2: chunk2 → write complete → chunk6
slot3: chunk3 → write complete → chunk7
```

这样形成一个固定容量的循环流水。

不会随着文件变大持续申请 Buffer。

---

# 十八、短读 / 短写现在怎么处理

同步版本已经有短写处理。

io_uring 版本同样不能假设：

```text
CQE res == requested bytes
```

当前每个 Slot 保存：

```text
progress
```

假设：

```text
length = 1 MiB
```

第一次只传输：

```text
700 KiB
```

则：

```text
progress += 700 KiB
```

如果：

```text
progress < length
```

会重新提交同一种 operation：

```text
buffer + progress
remaining = length - progress
offset = slot.offset + progress
```

因此：

```text
short read
short write
```

都会从未完成的位置继续。

不会：

```text
重新读整个 chunk
或者
重新写整个 chunk
```

---

# 十九、EOF 与 source_size 的处理

io_uring 路径不是一直 Read 到 EOF 来决定文件结束。

它接收：

```text
source_size
```

当前传入的是 Frozen Manifest 中记录的源文件 size。

所以：

```text
chunk_count
```

在开始前已经确定。

如果代码预期某一块应该存在，但 Read CQE 返回：

```text
0
```

则不是正常 EOF。

而是：

```text
source ended before expected size
```

返回 I/O 错误。

这和 PhotoBridge 的 Frozen Manifest / MutationGuard 思想是一致的：

```text
计划认为源文件应该有 N 字节
↓
实际读取提前结束
↓
不能把它当成正常成功
```

---

# 二十、CQE negative result 怎么处理

io_uring 与普通系统调用错误返回方式不同。

CQE：

```text
cqe->res < 0
```

表示操作失败。

当前通过：

```text
StatusFromErrno(-result, operation)
```

转回项目统一 `Status`。

例如：

```text
Read CQE error
→ read io_uring copy chunk

Write CQE error
→ write io_uring copy chunk
```

因此 io_uring 没有绕开 PhotoBridge 原有错误体系。

---

# 二十一、发生错误后为什么还要 Drain in-flight 请求

这是异步 I/O 和同步 I/O 很不同的一点。

同步函数发生错误：

```text
return
```

通常就结束了。

但 io_uring 中可能还有其他 request 正在使用：

```text
slots[index].bytes
```

如果直接：

```text
return
↓
slots 析构
```

而内核请求仍然在访问这些 Buffer：

```text
Buffer 生命周期就会出问题
```

所以当前代码在第一次 failure 后：

```text
不再继续正常状态推进
```

但是：

```text
继续等待并消费已有 in-flight CQE
```

直到：

```text
in_flight == 0
```

然后才允许局部 Buffer 释放。

当前注释明确表达了：

```text
Drain in-flight requests before freeing buffers
```

这是异步 Buffer 生命周期正确性的关键点。

---

# 二十二、失败后为什么重置 cached Ring

如果 io_uring 执行过程中出现 failure：

```text
cached_context.reset()
```

当前线程的 Ring 会被丢弃。

下一次 Copy 再重新创建。

这样做比：

```text
失败后继续复用一个状态不确定的 Ring
```

更保守。

属于：

```text
正确性优先
```

的选择。

---

# 二十三、最终一致性检查

所有 Completion 处理完成以后，不会直接认为成功。

还会验证：

```text
completed == chunk_count
next_hash == chunk_count
bytes_copied == source_size
```

分别表示：

### completed

所有 chunk 的 Write 都真正完成。

### next_hash

所有 chunk 都按顺序进入了 Hasher。

### bytes_copied

最终写入的总字节数符合 Frozen Source Size。

只有全部成立：

```text
Finalize BLAKE3
↓
生成 CopyResult
```

因此成功条件不是：

```text
Ring 空了就算成功
```

而是同时检查：

```text
I/O 完整性
+
Hash 完整性
+
字节数完整性
```

---

# 二十四、MigrationAttemptPreparer 的改动

优化前：

```text
CreateTemp
↓
CopyAndHash
↓
fdatasync
↓
MarkTempWritten
↓
VerifyBinary
↓
PersistVerifiedReceipt
```

现在：

```text
CreateTemp
↓
VerifyBeforeRead
↓
TryIoUringCopyAndHash
├─ 成功 → VerifyAfterRead
└─ io_uring 初始化不可用 → Sync CopyAndHash
↓
fdatasync
↓
MarkTempWritten
↓
VerifyBinary
↓
PersistVerifiedReceipt
```

这里最重要的是：

```text
io_uring 只替换 Copy 数据面
```

后面的 Crash-safe Commit 逻辑没有改变。

---

# 二十五、为什么仍然保留 MutationGuard

io_uring 提高的是 I/O 调度方式。

它不能解决：

```text
源文件在迁移期间被修改
```

所以当前仍然保持：

```text
VerifyBeforeRead
↓
Copy
↓
VerifyAfterRead
```

如果源文件身份发生变化：

```text
不能因为 Copy 字节成功
就认为迁移结果有效
```

这说明：

```text
性能优化
≠
放弃源文件一致性验证
```

---

# 二十六、为什么 fdatasync 没有改成 io_uring

当前 Copy 完成后仍然：

```text
file_ops.Fdatasync(temp)
```

保持同步持久化屏障。

这是正确的职责边界。

因为 PhotoBridge 后续状态机依赖：

```text
Copy 完成
↓
fdatasync(temp) 成功
↓
MarkTempWritten
```

这不是普通吞吐操作。

它是：

```text
Crash-safe 状态转换边界
```

即使把 fsync 提交成异步：

```text
后续状态推进前最终仍必须确认它完成
```

所以本轮没有为了“所有操作都 io_uring 化”而修改持久化协议。

---

# 二十七、为什么 rename / fsync directory 没改

PhotoBridge 最终发布逻辑仍然依赖：

```text
VerifiedReceipt durable
↓
rename-no-replace
↓
fsync(parent directory)
↓
SUCCEEDED
```

这部分属于：

```text
控制面
+
Crash-safe Commit
```

而不是大文件高频数据面。

每个 Task：

```text
rename 一次
directory fsync 一次
```

而大文件可能有很多 chunk I/O。

因此优化优先级是：

```text
高频 Read / Write
>>>>>>>>>
低频 rename / directory fsync
```

本轮保持这些语义不变，可以显著降低回归风险。

---

# 二十八、为什么 VerifyBinary 目前仍然是同步 Read

当前：

```text
VerifyBinary
```

仍然使用：

```text
FileOps::Read
↓
BLAKE3
```

没有改成 io_uring。

所以当前第四项优化准确描述应是：

```text
优化迁移阶段的 Copy + Hash 数据面
```

不能说：

```text
PhotoBridge 所有文件 I/O 都已经 io_uring 化
```

保留 Verify 同步路径有两个好处：

### 第一：缩小本轮改造范围

先证明：

```text
Copy 路径是否有实际收益
```

再决定第二次独立验证是否值得改。

### 第二：保留独立实现

Copy 使用新异步路径。

Verify 仍使用旧同步路径。

这在某种程度上也避免：

```text
Copy 和 Verify 共用完全相同的新流水实现
→ 同一个实现 Bug 同时污染两边
```

当前独立 Verify 的语义仍然保留。

---

# 二十九、构建系统的改动

## 29.1 vcpkg

新增依赖：

```text
liburing
```

---

## 29.2 CMake

新增：

```text
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
```

`photobridge_core` 新增源文件：

```text
src/filesystem/io_uring_copy_engine.cpp
```

并链接：

```text
PkgConfig::LIBURING
```

因此 io_uring Copy Engine 现在是正式构建的一部分。

---

# 三十、Benchmark 的改动

第四项优化没有只修改业务代码。

Benchmark 也同步增加了专门的 A/B 对照入口。

新增 Copy 模式：

```text
copy-sync
copy-uring
copy-uring-qd1
copy-uring-qd2
copy-uring-qd4
copy-uring-qd8
copy-uring-qd16
copy-compare
```

新增参数：

```text
--copy-uring-qd N
```

限制：

```text
1 ～ 16
```

默认：

```text
QD = 4
```

---

# 三十一、为什么 Benchmark 必须区分 Sync 和 io_uring

旧 Benchmark：

```text
CopyAndHash
+
fdatasync
```

新 Benchmark 可以分别测：

```text
Sync Copy
```

和：

```text
io_uring QD=1
io_uring QD=2
io_uring QD=4
io_uring QD=8
io_uring QD=16
```

这使第四项优化能够回答两个不同问题：

### 问题一

```text
io_uring 相对同步路径有没有收益？
```

### 问题二

```text
收益来自“换 API”
还是来自“更高 Queue Depth”？
```

例如：

```text
Sync vs QD=1
```

可以观察单请求模型差异。

```text
QD=1 vs QD=2/4/8/16
```

可以观察多请求在途带来的收益和过深队列的代价。

---

# 三十二、Benchmark 不允许偷偷 fallback

这是当前 Benchmark 一个很好的设计点。

业务主链允许：

```text
io_uring unavailable
→ sync fallback
```

但 Benchmark 如果测：

```text
copy-uring
```

而实际上偷偷执行 Sync：

```text
结果就没有意义
```

因此 Benchmark 中：

```text
TryIoUringCopyAndHash
↓
如果 optional == nullopt
↓
直接 Fail
```

明确拒绝：

```text
把同步 Copy 的结果标成 io_uring
```

这样保证性能数据真实。

---

# 三十三、单元测试新增了什么

新增：

```text
io_uring_copy_engine_test.cpp
```

当前测试覆盖重点包括以下几类。

## 33.1 多 Buffer + Hash 顺序

构造：

```text
9 MiB + 137 bytes
```

数据。

分别测试：

```text
QD=1
QD=2
QD=4
QD=8
QD=16
```

检查：

```text
bytes_copied
source digest
目标文件实际内容
```

其中 Digest 与预先一次性计算的 BLAKE3 比较。

这主要验证：

```text
即使 I/O Completion 乱序
Hash 最终仍然按文件顺序
```

---

## 33.2 Chunk 边界

测试：

```text
0
1
1024
1 MiB
1 MiB + 1
```

覆盖：

```text
空文件
极小文件
正好 chunk 边界
超过 chunk 一个字节
```

---

## 33.3 非法参数

覆盖：

```text
chunk_size = 0
QD = 0
QD = 17
```

确保不会让非法配置进入 Ring 状态机。

---

## 33.4 io_uring 不可用 fallback

通过：

```text
PHOTOBRIDGE_TEST_DISABLE_IO_URING=1
```

强制：

```text
TryIoUringCopyAndHash
→ nullopt
```

然后验证原来的同步：

```text
CopyAndHash
```

仍然能够完成 Copy。

---

## 33.5 Source 提前结束

声明：

```text
expected size > 实际 source size
```

验证提前 EOF 被认为：

```text
kIoError
```

而不是正常成功。

---

## 33.6 Write Completion 错误

使用：

```text
/dev/full
```

制造写失败。

验证：

```text
negative CQE result
→ kIoError
```

确保异步错误能够进入项目原有 Status 体系。

---

# 三十四、优化前后完整对比

| 项目 | 优化前 | 优化后 |
|---|---|---|
| Copy 模型 | 单 Buffer 同步循环 | 多 Buffer io_uring 状态机 |
| Read | `FileOps::Read` | `io_uring_prep_read` |
| Write | `FileOps::Write` + `WriteAll` | `io_uring_prep_write` |
| 请求并发 | 单请求 | QD 1～16 |
| 默认 QD | 无 | 4 |
| Chunk | 1 MiB caller buffer | 默认 1 MiB / Slot |
| 文件位置 | fd current offset | explicit offset |
| Completion | 系统调用直接返回 | CQE |
| 短 I/O | WriteAll | progress + resubmit |
| Hash 顺序 | 天然顺序 | sequence + next_hash 强制顺序 |
| Buffer 生命周期 | 一个 buffer 同步复用 | Write CQE 后才能复用 Slot |
| Ring 生命周期 | 无 | thread_local Ring |
| io_uring 不可用 | 不涉及 | 开始 I/O 前 fallback Sync |
| 中途 io_uring 错误 | 不涉及 | 直接失败，不重复 Copy |
| Crash-safe fdatasync | 同步 | 保持同步 |
| 独立 Verify | 同步 | 仍然同步 |
| rename/fsync | 原逻辑 | 完全不变 |
| Benchmark | Sync Copy | Sync + QD1/2/4/8/16 |
| 专门单测 | 无 | io_uring Copy Engine Test |

---

# 三十五、现在的真实数据流

以一个大文件、默认：

```text
chunk = 1 MiB
QD = 4
```

为例。

初始化：

```text
slot0 → chunk0 → submit Read
slot1 → chunk1 → submit Read
slot2 → chunk2 → submit Read
slot3 → chunk3 → submit Read
```

提交：

```text
SQ
↓
Kernel
```

Completion 可能：

```text
chunk1 Read Complete
chunk0 Read Complete
chunk3 Read Complete
chunk2 Read Complete
```

Hash 不能按 Completion 顺序。

所以：

```text
chunk1 READY
↓
等待 chunk0

chunk0 READY
↓
Hash chunk0
↓
submit Write chunk0
↓
Hash chunk1
↓
submit Write chunk1
```

Write 完成：

```text
chunk0 Write Complete
↓
slot0 FREE
↓
slot0 读取 chunk4
```

于是流水持续：

```text
Kernel 正在 Read 后续块
+
Worker 正在 Hash 当前可用块
+
Kernel 可能正在 Write 前一块
```

直到：

```text
所有 chunk Write Complete
+
所有 chunk Hash Complete
+
bytes_copied == source_size
```

最后：

```text
Finalize BLAKE3
↓
CopyResult
```

---

# 三十六、与 Worker Pool 的关系

第二项优化解决：

```text
Task 级并行
```

第四项优化解决：

```text
单 Task 内部 I/O 并行度
```

二者叠加：

```text
Worker1 → File A → QD=4
Worker2 → File B → QD=4
Worker3 → File C → QD=4
Worker4 → File D → QD=4
```

理论上可以同时存在多个 Ring 和多个 I/O request。

所以最终真正需要关注的是：

```text
Worker 数 × Queue Depth
```

不是单独把 QD 调得越高越好。

---

# 三十七、与 BLAKE3 优化的关系

第三项优化解决的是：

```text
每一个 Hash chunk 的 CPU 计算效率
```

第四项解决的是：

```text
数据什么时候进入 Hash
以及 I/O 能否与 CPU 计算重叠
```

所以关系是：

```text
BLAKE3 SIMD
→ 加速 CPU Hash

io_uring
→ 提高 I/O Pipeline 并行度
```

当前 Hash 本身仍然：

```text
单 Worker 内按文件顺序 Update
```

io_uring 并没有把 BLAKE3 改成乱序或并行归并 Hash。

这是为了保证 Digest 正确性。

---

# 三十八、当前内存代价

io_uring 多 Buffer 不是免费的。

当前每个 Slot：

```text
chunk_size
```

默认：

```text
1 MiB
```

默认 QD：

```text
4
```

因此一个活跃 io_uring Copy 的主要 Slot Buffer 大约：

```text
4 × 1 MiB
= 4 MiB
```

同时 Migration Worker 仍保留原来的：

```text
1 MiB 同步 fallback buffer
```

因此一个活跃 Worker 在 Copy 阶段仅这部分数据 Buffer 大致可理解为：

```text
4 MiB io_uring slots
+
1 MiB sync fallback buffer
```

如果：

```text
4 Worker × QD4
```

数据 Buffer 规模会进一步叠加。

所以：

```text
更高 QD
→ 更多 in-flight I/O
→ 更高内存占用
```

必须通过 Benchmark 决定。

---

# 三十九、为什么当前默认 QD=4

当前代码没有写死：

```text
QD 越大越好
```

而是：

```text
默认 4
Benchmark 测 1 / 2 / 4 / 8 / 16
```

这是正确的性能优化思路。

因为不同环境：

```text
VM
SSD
HDD
SMB
page cache
不同 Worker 数
```

最佳 Queue Depth 可能完全不同。

所以 QD=4 更准确地说是：

```text
当前默认实验配置
```

最终是否最优仍然要由数据证明。

---

# 四十、这次优化刻意没有做什么

当前没有实现：

```text
Registered Buffer
Registered File
SQPOLL
IORING_SETUP_SQPOLL
O_DIRECT
Linked Read/Write
io_uring fdatasync
io_uring rename
io_uring directory fsync
io_uring VerifyBinary
```

所以面试中不要说：

```text
我把整个文件系统 I/O 全部重写成了 io_uring。
```

准确表达应该是：

```text
我只把迁移高频数据面的 Copy + Hash
从同步 read/hash/write
改成多 Buffer、显式 offset、CQE 驱动的 io_uring 流水。

持久化和 Commit 控制面仍保持原来的同步语义。
```

---

# 四十一、为什么这种收敛方式比“全部 io_uring 化”更合理

PhotoBridge 的核心目标不是：

```text
展示 io_uring API 数量
```

而是：

```text
在不破坏 Crash-safe 协议的前提下优化热点路径
```

真正高频的是：

```text
文件内容 read/write
```

低频的是：

```text
open
stat
fdatasync
rename
fsync directory
```

而低频操作中还有很多属于：

```text
状态机持久化边界
```

所以本轮选择：

```text
数据面异步化
+
控制面保持稳定
```

收益和复杂度更加匹配。

---

# 四十二、第四项优化的核心设计逻辑

可以压缩成下面这条因果链：

```text
原同步 Copy 每次只允许一个 chunk 前进
↓
I/O 与 Hash 无法形成多请求流水
↓
不直接污染同步 FileOps
↓
增加独立 IoUringCopyEngine
↓
一个 Worker 创建多个 BufferSlot
↓
使用 explicit offset 提交多个 Read
↓
CQE 允许乱序完成
↓
sequence + next_hash 保证 BLAKE3 输入顺序
↓
Hash 后提交对应 Write
↓
Write CQE 后 Buffer 才允许复用
↓
short I/O 用 progress 继续提交
↓
错误时先 drain in-flight request
↓
Copy 完成后仍然执行原 fdatasync / Verify / Receipt
↓
性能路径改变，但 Crash-safe 语义保持
```

---

# 四十三、面试时可以怎么讲这次改动

可以直接按下面的逻辑表达：

```text
PhotoBridge 原来的单文件 Copy 是同步 read → hash → write，
一个 chunk 必须完整走完才能处理下一块。

在 Worker Pool 和 BLAKE3 优化完成以后，
我把第四项优化集中到单文件数据面。

我没有直接改 FileOps，因为 FileOps 的接口本身是同步语义，
如果内部 submit 后马上 wait，本质仍然是 QD=1 的伪异步。

所以我单独增加了 IoUringCopyEngine。
默认使用 1 MiB chunk、QD=4，
每个 BufferSlot 保存 sequence、offset、length、progress 和 phase。

开始时会一次提交多个带显式 offset 的 Read。
CQE 可以乱序回来，但 BLAKE3 不能乱序，
所以我维护 next_hash，只允许按 chunk sequence 顺序 Update。

一个 chunk Hash 完后再提交对应 Write，
而这个 Buffer 必须等 Write CQE 返回以后才能重新用于后续 Read。

短读短写通过 progress + explicit offset 继续提交；
CQE negative result 转换成项目统一 Status。
发生错误以后也不会立即释放 Buffer，
而是先 drain 已经 in-flight 的请求。

同时我保留原同步 Copy 作为 fallback，
但只允许在 io_uring 初始化失败、还没有开始任何数据 I/O 时回退。
如果异步 Copy 已经开始后失败，就直接让这个 attempt 失败，
避免重复写入和 Hash 状态污染。

最后，fdatasync、独立 Verify、VerifiedReceipt、rename 和 directory fsync
这些 Crash-safe 控制面都没有改变。

因此这次优化本质上是：
只异步化高频数据面，
而不破坏原来的持久化协议。
```

---

# 四十四、后续性能验证应该回答的问题

代码完成并不代表优化有效。

接下来 Benchmark 应回答：

```text
1. Sync vs io_uring QD1 差多少？
2. QD1 → QD2 → QD4 → QD8 → QD16 的趋势是什么？
3. wall time 是否下降？
4. MiB/s 是否提高？
5. CPU utilization 怎么变化？
6. context-switch 是否变化？
7. syscall 模式是否从 read/write 转向 io_uring_enter？
8. page cache 环境和真实磁盘环境是否一致？
9. Worker × QD 是否出现过度并发？
10. 默认 QD4 是否真的合理？
```

建议继续使用：

```text
Benchmark
+
perf stat
+
perf record
+
strace
```

共同解释结果。

---

# 四十五、当前代码结论

从代码结构上，第四项优化已经完成了从：

```text
同步单 Buffer Copy
```

到：

```text
独立 io_uring Copy Engine
+
多 Buffer
+
真实 QD > 1
+
explicit offset
+
CQE 驱动
+
Hash 顺序控制
+
short I/O 重提交
+
in-flight drain
+
安全同步 fallback
```

的核心改造。

同时保留：

```text
MutationGuard
fdatasync
MarkTempWritten
Independent Verify
VerifiedReceipt
rename-no-replace
directory fsync
```

这些原有可靠性边界。

因此当前第四项优化最准确的定位是：

```text
在不修改 Crash-safe Commit 协议的情况下，
把迁移阶段高频 Copy + Hash 数据面
从同步串行 I/O
改造成多 Buffer io_uring 异步流水。
```

---

# 四十六、相关代码目录

```text
include/photobridge/filesystem/
├── copy_and_hash.h
└── io_uring_copy_engine.h

src/filesystem/
├── copy_and_hash.cpp
├── io_uring_copy_engine.cpp
├── binary_verifier.cpp
└── linux_file_ops.cpp

src/app/
└── migration_attempt_preparer.cpp

src/pipeline/
└── migration_service.cpp

tests/unit/filesystem/
├── copy_and_hash_test.cpp
└── io_uring_copy_engine_test.cpp

benchmarks/
└── photobridge_bench.cpp

CMakeLists.txt
vcpkg.json
tests/CMakeLists.txt
```

---

# 四十七、一句话总结

```text
过去：
一个 Worker 内部只有同步 read → hash → write。

现在：
一个 Worker 内部使用多个 Buffer 和显式 offset，
通过 io_uring 同时保持多个 I/O request 在途，
CQE 可以乱序，但 BLAKE3 仍按 sequence 顺序计算，
Write 完成后才复用 Buffer；
同时原来的 fdatasync、独立 Verify 和 Crash-safe Commit 语义保持不变。
```
