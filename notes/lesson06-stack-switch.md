# Lesson 6 教案 — 栈切换

> **状态：已补恢复语义、平台边界与验收，未开讲。**
> 前置：Lesson 3（保存清单）、Lesson 5.5（naked / `.S`）。
> 本课目标：写出一个能用的 `ctx_switch`，并说清**为什么它必须离开 extended asm**、
> 保存集合怎么来的、栈怎么分配、哪些东西会静默坏掉。

---

## 开讲拆分

| 顺序 | 前置检查 | 小任务与停点 |
|---|---|---|
| 1 | 能读 `.S`，会算 push 偏移 | 只追踪旧栈保存，画出保存指针指向哪一个槽 |
| 2 | 知道返回地址由 call 产生 | 画 A→B→A，标三次切换各自实际返回的调用点 |
| 3 | 会算 `rsp%16` | 区分“恢复已运行上下文”与“首次进入”，后者交给 Lesson 7 |
| 4 | 能列 MS 非易失状态 | 审查 GPR demo 缺项；Windows 扩展未完成前不判 ABI 通过 |

本课先理解已有上下文的恢复，首次运行必须依赖 Lesson 7 的初始化，
不是把一块未初始化数组地址直接交给 `ctx_switch`。

---

## 0. 问题：把 `rsp` 换掉

到这里，所有"保存/恢复"都是在**同一根栈**上做的。真正让两个执行流独立的是这一步：

```
当前执行流的 rsp  →  存进内存
内存里的另一个 rsp →  装进 rsp
```

从这一条指令之后，你就在**另一根栈**上执行了。

GCC 文档对 extended asm 的硬规定：

> the compiler requires the value of the stack pointer to be the same after an asm statement
> as it was on entry to the statement.

所以这件事**不能**在 extended asm 里做。必须用 Lesson 5.5 的台阶 2 或 3。

---

## 1. 切换原语的骨架

```asm
ctx_switch(void **from, void *to):
    pushq %rbp / %rbx / %rsi / %rdi / %r12–%r15   # 8 个 callee-saved
    movq  FROM, %rax
    movq  %rsp, (%rax)        # *from = 当前 rsp
    movq  TO,   %rsp          # rsp = 对方的 rsp
    popq  %r15 ... %rbp       # 反过来弹 8 个
    ret                       # 返回到对方上次 call 的下一句
```

三句话记住它：

1. **保存的是"栈顶指针"，不是"整个栈"**。栈内容本来就在内存里，不用搬。
2. **恢复的是"上次停下的位置"**。返回地址在对方栈上，`ret` 一弹就回去了。
3. **它是"返回到对方"，不是"调用对方"**。所以对方看起来像"刚调用完 `ctx_switch` 返回"。

观察用版本在 [`demos/hello-switch/`](../demos/hello-switch/)，含栈布局图。
**它是 GPR-only 历史 demo，不是合格的 Windows ABI 参考实现。**
Windows 缺 XMM 保存、入站 shadow space 与 TEB 栈状态处理；见下文与 demo README。

---

## 2. 保存集合：为什么正好是这 8 个

| 类别 | 存不存 | 理由 |
|---|---|---|
| callee-saved GPR | ✅ `rbp rbx rsi rdi r12–r15` | 编译器假设它们跨调用不变 |
| `rsp` | ✅ | 就是切换本身 |
| caller-saved GPR | ❌ | 编译器在调用点已经当成消耗品 |
| flags | ❌（协作式） | 同上，调用后没人指望 |
| `xmm6–xmm15` | Windows 上 ✅ | MS x64 把它们算 callee-saved |
| `mxcsr` / x87 控制字 | 严格来说 ✅ | 控制位是 callee-saved；boost 存了 |
| TLS / canary | ❌ | 同一个线程，切栈不切 TLS |

**并集策略**：`rbp rbx rsi rdi r12–r15` 是两套 ABI 的 callee-saved 并集。
在 Linux 上多存 `rsi/rdi`，保存和恢复各多 16 字节访存。
这只统一了 **GPR** 部分；其他状态不因“取了并集”就自动正确。

### 2.1 先写合同，再写保存序列

本课程是**单 OS 线程、协作式、在普通函数调用边界切换、不迁移线程、不跨栈展开**。
`from` 必须指向仍存活且可写的保存槽，`to` 必须是另一有效上下文；
两块栈的内存要一直保留，直到上下文确定不再恢复。第一版禁止 `from` 与目标保存区别名。

Windows 主线需要保留：8 个非易失 GPR、`xmm6–15` 的低 128 位、非易失浮点控制状态，
以及符合平台约定的栈边界/运行时状态。`xmm6–15` 保存区合计 **160 字节**；
若用对齐访存，每个地址还必须 16 对齐，不能只在旧布局尾部随意加 160。
MXCSR 控制位与状态位保存责任不同；保存整个寄存器可以作为课程策略，
但要明说这是选择，不要说 ABI 要求所有状态位都非易失。

TEB 的布局不能凭本课硬编码用于任意 Windows 版本；先读 Lesson 9 固定源码的实现，
并承认手写版不涵盖完整异常、栈增长和 CET 支持。编译成功不是这些能力的证据。

### 2.2 `from` 是保存结果，`to` 是恢复输入

设 A 已停在自己的 `call ctx_switch` 内，`A.sp` 指向保存区。B 切回 A 时：

1. 在 B 栈压入 B 的寄存器，写 `B.sp`；
2. 读取旧的 `A.sp`，装入 `rsp`；
3. 后面的 pop 和 ret 都从 **A 栈**取数据；
4. A 的那次调用现在才返回，接着执行 A 的下一行。

不是一次调用在同一栈上“返回两次”，也不是复制 C++ 调用帧。
在 `-S` 中检查：平台参数寄存器的值在使用前，不能已被恢复序列覆盖。

---

## 3. 栈的对齐：两个数决定生死

- 任意函数**入口**：`rsp % 16 == 8`；
- **`call` 前**：`rsp % 16 == 0`。

**恢复已有调用**时，ret 后回到调用者，应该还原其 `call` 前 `rsp`（通常为 0 mod 16）；
**首次进入普通函数**时，才需要伪造成 8 mod 16。不能把这两种余数混为一谈。
在 [`hello-switch`](../demos/hello-switch/hello_switch.cpp) 里：

```
初始 sp = top - 80
pop 8 个（64 字节）→ rsp = top - 16
ret 弹返回地址   → rsp = top - 8
```

这是 **SysV 的简化初始布局**。Windows 还缺新入口上方的 32 字节 home slots；
仅满足余数不等于帧合法。见 Lesson 7 的 MS 布局推导。
错 8 字节可能在对齐访存处崩，也可能暂时不崩，必须用地址账本验收。

---

## 4. 第一次进入：栈上没有"上次"

新建的 fiber 从没调用过 `ctx_switch`，它的栈上没有返回地址。
解法是**伪造一个"刚被切走"的帧** —— 这是 Lesson 7 的全部内容，这里先记住结论：

```
[8 个保存槽][返回地址 = fiber 入口][fiber 的返回地址]
```

第一次切进去时，`pop` 8 个垃圾、`ret` 到入口，就"启动"了它。

---

## 5. 栈从哪来

| 方案 | 优点 | 坑 |
|---|---|---|
| 静态数组 `uint64_t st[N]` | 最简单，demo 够用 | 大小编译期固定；地址可能没对齐 |
| `malloc` | 动态大小 | 对齐要自己 round up |
| `mmap` + guard page | 溢出时立刻 `SIGSEGV`，能定位 | 平台 API；Windows 是 `VirtualAlloc` |
| 复用调用者栈 | 省内存 | 两个执行流会互相踩，基本不可行 |

**大小估算**：栈上会跑 `printf`、可能还有异常展开；没有通用的 16 KiB 安全下限；demo 用 128 KiB 也不是任意工作负载的保证。
栈溢出在 Linux 上表现为踩到相邻内存或 guard page；在 Windows 上还会被 TEB 的栈限制
（`StackBase`/`StackLimit`）影响 —— 单纯手写 `rsp` 切换**不会更新 TEB**，
所以 Windows 的异常/栈检查可能认为你在"栈外"。这是 boost 在 Windows 上也要小心的点。

---

## 6. 红区、canary、TLS

- **红区**：SysV 不能用红区保存跨普通调用存活的值，所以普通被调切换函数压栈不是额外错误。
  extended asm 偷偷 call/push 才可能撞上编译器仍以为有效的红区值。
- **canary**：保护函数的栈内副本随各自栈保留。Linux TLS guard 与 MinGW 实现不能混同。
  `fiber_exit` 的作用是结束路径，不是修复 canary。
- **TLS / errno**：切栈不切 TLS，所以 `errno` 是 fiber 之间共享的。
  两个 fiber 同时用 `errno` 会互相污染 —— 这是"协程不是线程"的一个具体后果。

---

## 7. 性能：一次切换多少钱

先只对上面的 **GPR-only 骨架**静态计数，不外推 Windows 完整版的性能：

- 16 条 `push`/`pop` + 3 条 `mov` + 1 条 `ret`（共 20 条，不含调用方 `call`）；
- 数据区读写、ret 目标预测、缓存命中与平台扩展影响实际耗时；
- 不能从这些条数推断“必定 L1 命中”或“固定几十周期”。

计时是后置选做。让 main 往返 worker **N 次**，worker 每次切回 main，
则区间内是 **2N 次单向切换**；先用计数器核对，再用 `delta / (2N)` 报每次 TSC ticks。
预热、重复取分布，移除打印，记录 CPU/工具链/计时屏障和计时器本身开销。
编译器与 CPU 排序要求见 Lesson 2 §4.6，不把“加 lfence”当万能公式。
不要直接拿 `setjmp/longjmp` 当切栈参考：跳回已返回函数的环境是非法用法，
它也不提供新栈创建语义。

---

## 8. 任务

### 任务 A：第三个 fiber

在 demo 基础上加 `fiber_c`，三个函数轮流打印。要求：

- 每个 fiber 有自己的 `sp`；
- 加一个 `yield_to(next)` 的小封装；
- 贴运行结果和 `-S`（证明没有多余 prologue）。

Windows 作业先完成 §2.1 的保存合同；只扩展旧 demo 的输出不算 ABI 通过。
初始化须先完成 Lesson 7。函数声明、实现与调用边界用单独目标文件检查，不开 LTO。

### 任务 B：对齐实验

用独立的坏版本，把当前正确初始指针再偏移 8 字节，

- 先跑 demo，记下结果；
- 再加一个用 `double`/`printf("%f")` 的 fiber，看什么时候崩；
- 在调试器停于入口、第一次 call 前，实测余数；不崩也算得到证据，不要求制造崩溃。

### 任务 C：守卫页

选做：主线用 `VirtualAlloc`，Linux 对照用 `mmap`。
保护页放在**低地址端**，与向下生长方向一致。为确定性故障实验用不可访问页；
Windows `PAGE_GUARD` 是一次性告警机制，不等于永久不可访问的 `PAGE_NOACCESS`。
递归必须产生不可优化掉的栈帧；用反汇编确认，别被尾递归优化成循环。
实验独立进程运行，在调试器核对故障访问地址，不能以“任意崩溃”作为成功。

### 任务 D：测量

选做：按 §7 测往返次数，报告 2N 分母、TSC ticks 分布与测量边界；
未完成正确性/ABI 验收前不计时。

---

## 9. 易错点

1. 在 extended asm 里换 `rsp`（编译器所有 `"m"` 操作数全废）。
2. 少存一个 callee-saved（`-O2` 下"碰巧"能用，换编译器就炸）。
3. 初始栈差 8 字节（`movaps` 时崩）。
4. 以为切栈会把 TLS 也切了（`errno` 互相污染）。
5. 用 `malloc` 的栈没做 16 字节对齐。
6. Windows 上不更新 TEB 的栈限制（异常/栈检查会误判）。
7. 把普通函数调用与 extended asm 内隐藏 push/call 的红区问题混为一谈。
8. 给独立 `.S` 函数想象一个 `"memory"` clobber；它没有这个列表。
   应通过普通不透明函数边界表达副作用，不加 `pure/const` 等错误属性。

---

## 10. 自测

1. `ctx_switch` 保存的是"栈"还是"栈顶"？为什么？
2. 为什么它用 `ret` 而不是 `call`？
3. 8 个 push 之后，`rsp` 相对入口变了多少？
4. 初始栈为什么要 `top - 80`、`top - 8` 这两个偏移？
5. 切栈为什么不用切 TLS？
6. 协作式切换为什么不用保存 flags？
7. Windows 上不更新 TEB 会有什么后果？

---

## 11. 和前后课的关系

| 课 | 关系 |
|---|---|
| Lesson 3 | §7 清单直接变成这里的 push/pop |
| Lesson 5 | `ctx_switch` 必须遵守调用约定 |
| Lesson 5.5 | 提供 naked / `.S` 两种落地方式 |
| Lesson 7 | 第一次进入要伪造帧 |
| Lesson 8 | 把两个 fiber 扩成 N 个 |
| Lesson 9 | `jump_fcontext` 就是这里的工业版 |
