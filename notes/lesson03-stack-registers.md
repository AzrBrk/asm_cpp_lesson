# Lesson 3 教案 — 栈与寄存器保存

> **状态：已补推导与勘误，未开讲。主线为 MS x64，SysV 只作标注对照。**
> 前置：Lesson 1–2（约束、clobber）+ [`lesson01-deep-dive.md` §4](lesson01-deep-dive.md)。
> 本课目标：给你任意一个 `-O2` 函数，你能画出它的栈帧；并且能回答
> **"要让这个执行流停在这里、以后原样继续，哪些字节必须保留？"**

---

## 开讲拆分

| 顺序 | 前置检查 | 小任务与证据；完成即停 |
|---|---|---|
| 1 | 会用十六进制算地址 | 设入口 `rsp=0x1008`，手算两次 push、两次 pop，每步标内存地址 |
| 2 | 区分 call 与普通跳转 | 画 MS x64 入口的返回地址、四个 home slot、第 5 个参数 |
| 3 | 会算模 16 | 在真实 `-S` 中对每次 push/sub/call 记 `rsp` 变化 |
| 4 | 知道 ABI 是合同 | 从某个跨调用活值出发，解释谁负责保存它 |

先做纸上推导，再做 §9 的代码任务。**不要把“寄存器全存”当成保存集合推导。**

---

## 0. 这一课在整条线上的位置

前两课解决的是"**怎么写一条不会骗编译器的 asm**"。
从这一课开始换问题：**一个执行流的"状态"到底是什么？**

```
执行流的状态 = 寄存器 + 栈 + 返回地址 + （flags / SIMD / 控制字）
```

context switch 要做的，就是把这堆东西写进内存、再从另一块内存读回来。
所以这一课只做两件事：

1. **读懂** C 编译器生成的栈帧（不然你不知道自己在保存什么）；
2. **搞清** 哪些寄存器是"别人以为不会变"的（那才是必须保存的）。

---

## 1. 栈是倒着长的

下面是 `pushq %rbx` / `popq %rbx` 的**地址和值效果示意**：

```asm
pushq %rbx        # subq $8, %rsp ; movq %rbx, (%rsp)
popq  %rbx        # movq (%rsp), %rbx ; addq $8, %rsp
```

不要把示意当成 CPU 真拆出的指令/微操作，也不要推广到 `push %rsp`、`pop %rsp` 等特殊情形。
指令是否提供跨核原子性、是否可被异步事件观察，与这张等效图是不同问题。
示意的 `sub/add` 会写 flags，而真正普通 push/pop 不会；这里仅比较地址和值的变化。

- 在这组压栈/弹栈操作中，`rsp` 指向当前栈顶；函数还可用 `sub` 留出未初始化空间，
  所以不能说它永远指向“最后压入的值”；
- 压栈让 `rsp` **减小**，所以栈在内存里是往低地址长的；
- 64 位下每次 `push`/`pop` 固定 8 字节（16 位 `pushw` 只影响 2 字节，别用）。

`pushq %rbx` 的编码只有一个字节 `53`，`popq %rbx` 是 `5b` —— 这也是为什么"多存几个寄存器"
在指令长度上几乎不花钱，真正花钱的是**访存**和**对齐**。

---

## 2. 一个函数帧的解剖

### 2.1 先画你的平台：MS x64

令 `S` 是**刚进入函数、序言尚未执行**时的 `rsp`。只讨论普通整数/指针参数：

| 地址 | 内容 | 谁分配/写入 |
|---|---|---|
| `S+48` | 第 6 个参数 | 调用者 |
| `S+40` | 第 5 个参数 | 调用者 |
| `S+8 … S+39` | 32 字节 shadow space / home slots | 调用者留空间；被调用者可用，不保证已有参数副本 |
| `S` | 返回地址 | `call` 写入 |
| `S` 以下 | 保存寄存器、局部值、向下调用所需区域 | 本函数序言分配 |

`rcx/rdx/r8/r9` 的参数值**不自动写进 home slots**。
若序言累计向下移动 `D` 字节，当前 `rsp=S-D`，原第 5 参数变成
`D+40(%rsp)`；若只看到 `40(%rsp)` 就断定是第 5 参数，会读错函数体里的偏移。
同一个物理地址的表达式必须满足：`(S-D)+(D+40)=S+40`。

### 2.2 SysV 对照（不是 MinGW 默认）

以 SysV 为例，一个可能的调用现场（高地址在上）：

```
高地址
┌──────────────────────────┐
│ 调用者的局部变量 / 填充   │
├──────────────────────────┤
│ 第 7、8、… 个参数         │  ← 调用者压的
├──────────────────────────┤
│ 返回地址                  │  ← call 压的（8 字节）
├──────────────────────────┤ ← 函数入口时的 rsp（%16 == 8）
│ 保存的 rbp（可选）        │
│ canary（可选）            │
│ 局部变量                  │
│ 对齐填充                  │
├──────────────────────────┤ ← 函数体里的 rsp（%16 == 0）
│ 红色区域（叶子函数可用）   │  ← rsp 以下 128 字节
└──────────────────────────┘
低地址
```

三条可以直接验证的结论：

1. **返回地址就在 `0(%rsp)`**（刚进函数时）。
   证据：7 参数函数在 `-O2` 下读第 7 个参数用的是 `8(%rsp)`：

   ```asm
   seven:
       addq %rsi, %rdi
       ...
       addq 8(%rsp), %rax      # 第 7 个参数：跳过 8 字节返回地址
       ret
   ```

2. **帧指针是可选的**。`-fno-omit-frame-pointer` 可用于请求保留，
   但实际是否出现还受目标与函数形态影响；例如：

   ```asm
   fp_frame:
       pushq %rbp
       movq  %rsp, %rbp
       movq  %rdi, -8(%rbp)    # 局部变量用 rbp 相对寻址
       ...
       popq  %rbp
       ret
   ```

   `-O2` 默认省略帧指针，局部变量直接用 `rsp` 相对寻址 —— 这也是为什么
   **asm 里改 `rsp` 会同时毁掉编译器所有的 `"m"` 操作数和 spill 槽**。

3. **启用栈保护且函数符合插桩条件时，帧里会有 canary 副本**。
   不要假定所有 Ubuntu GCC 命令默认启用；以下是 Linux 特定示意：

   ```asm
   subq $40, %rsp
   movq %fs:40, %rax          # 从 TLS 读 canary
   movq %rax, 24(%rsp)        # 放到帧里
   ...
   movq 24(%rsp), %rdx
   subq %fs:40, %rdx          # 函数返回前比对
   jne  __stack_chk_fail
   ```

   这个 Linux 实现的参考 guard 在 **TLS**，副本在栈里；不能说“canary 不在栈上”。
   Windows/MinGW 的 guard 来源和序列须看目标输出，不能照搬 `%fs:40`。
   同线程切栈不自动改变该 TLS 参考值。
   但如果你手工构造的栈帧让函数"返回"到一个没写过 canary 的帧，检查就会失败。

---

## 3. 红色区域：rsp 以下 128 字节

SysV AMD64 规定：**信号处理函数不会改写 `rsp` 以下 128 字节**。
所以叶子函数可以不动 `rsp`，直接把临时值放在那儿。

证据（`-O2 -fno-stack-protector`）：

```asm
red:
    movq %rdi, -24(%rsp)     # 没 sub rsp
    addq $1, %rdi
    movq %rdi, -16(%rsp)
    movq -24(%rsp), %rax
    ...
    ret
```

三个后果：

1. **MS x64 没有红色区域。** 同一个函数在 MinGW 下会先 `subq $24, %rsp`。
2. 红区中的值**不能要求跨普通调用存活**；`ctx_switch` 作为普通被调函数压栈，
   不会因此破坏一个遵守 ABI 的调用者。
3. SysV 的异步信号交付保证不破坏这 128 字节；信号帧仍需额外栈空间。
   Windows 没有这个保证，不能在那里借用 `rsp` 以下未分配的字节。

---

## 4. 对齐：16 字节是硬约束

ABI 规定：

- **`call` 指令执行前**，`rsp` 必须是 16 的倍数；
- 所以**函数入口**（`call` 压完返回地址）`rsp % 16 == 8`；
- 函数序言里压几个寄存器、减多少字节，最后都要把这个账算平。

证据（MinGW，`main` 里调用 `printf`）：

```asm
main:
    pushq %rsi
    pushq %rbx
    subq  $40, %rsp        # 32 字节 shadow space + 8 字节对齐填充
```

MS x64 还要求调用者为被调用者预留 **32 字节 shadow space**（就算参数寄存器够用）。
SysV 没有 shadow space，但同样要 16 字节对齐。

对齐错了可能在对齐访存处故障，也可能暂时不显现；Linux 常见 `SIGSEGV`，
Windows 可能报访问异常，不能把某个异常名当作必然结果。

---

## 5. callee-saved：编译器的"承诺"

编译器在函数调用前后，对两类寄存器的假设完全不同：

| | SysV（Linux） | MS x64（Windows/MinGW） |
|---|---|---|
| callee-saved（被调用者必须原样还） | `rbx rbp r12–r15` | `rbx rbp rdi rsi r12–r15`，还有 `xmm6–xmm15` |
| caller-saved（调用后随便） | 其余 GPR | 其余 GPR（含 `rax rcx rdx r8–r11`） |

在**普通 extended asm 中正确声明 clobber** 后，编译器才会负责保住 ABI 的值；
裸写寄存器并不会被它自动发现。可能生成：

```asm
callee:
    pushq %r12
    pushq %rbx
    ...
    popq  %rbx
    popq  %r12
    ret
```

**这就是 context switch 只保存 callee-saved 的全部理由**：
从 C 代码的视角看，一次 `call ctx_switch` 之后，只有这批寄存器是"必须还在"的。
caller-saved 本来就被编译器当成消耗品。

> 注意 Windows 的 `rsi`/`rdi`：在 Linux 上随便写，在 Windows 上写了就要恢复。
> 同一段 asm 在两边"对不对"可能不同。

---

## 6. flags 要不要保存？—— 协作式不用，抢占式要

这里必须分清两种"切换"：

| | 切换发生在哪里 | flags 状态 |
|---|---|---|
| **协作式**（本课程） | 函数里显式 `call ctx_switch` | flags 是 caller-saved，调用点之后没人指望它 |
| **抢占式**（信号/中断） | 任意指令边界 | 必须 `pushfq`/`popfq`，否则恢复后 `jcc` 读错 |

SysV 没有把 EFLAGS 列为 callee-saved；`call` 之后条件码本来就作废。
所以 boost 的 `jump_fcontext` **不保存 flags** —— 你可以去源码里数，没有 `pushfq`。
本课程的整函数汇编没有 clobber 列表；条件码可以按调用约定失效，
不意味着 DF、浮点控制状态等也能随意遗留。

`pushfq` / `popfq` 的练习后移到整函数汇编。普通 extended asm 中的 push
可能踩 SysV 红区，不能提供一个未限定平台的通用片段。
抢占恢复还涉及操作系统上下文与特权位，并非加两条指令就完成。

---

## 7. 可恢复状态的完整清单

把上面所有东西合成一张表 —— **这就是 Lesson 6 要保存的东西**：

| 类别 | 内容 | 协作式切换要存吗 |
|---|---|---|
| callee-saved GPR | `rbx rbp r12–r15`（+ Win 的 `rsi rdi`） | ✅ |
| 栈指针 | `rsp` | ✅ |
| 返回地址 | 栈上的 `rip`（`call` 压的） | ✅（跟着栈走） |
| caller-saved GPR | MS：`rax rcx rdx r8–r11`；SysV 还含 `rsi rdi` | ❌（编译器按调用约定处理活值） |
| flags | EFLAGS | ❌（协作式）；抢占式 ✅ |
| SIMD callee-saved | `xmm6–xmm15`（仅 Windows） | Windows 上 ✅ |
| MXCSR / x87 控制字 | 控制位是 callee-saved | 严格来说 ✅（boost 存了） |
| TLS | 线程本地状态；段寄存器用法依平台 | 同线程默认共享；Windows 的部分 TEB 栈字段需另处理 |
| canary | 栈内副本 + 平台相关参考 guard | 保留各自栈；不要误抄 Linux guard 实现到 MinGW |

最后两行值得单独记：**切栈不切 TLS**。所以 `errno`、canary、线程局部变量
在 fiber 之间是共享的 —— 这既是方便，也是坑（两个 fiber 同时改 `errno` 会互相污染）。

---

## 8. 把清单落到代码

复习一下 [`demos/hello-switch`](../demos/hello-switch/) 里的 8 个 push：

```asm
pushq %rbp
pushq %rbx
pushq %rsi      # Windows 需要；SysV 上是多存的
pushq %rdi      # 同上
pushq %r12
pushq %r13
pushq %r14
pushq %r15
```

这是“两套 ABI 的 **GPR** callee-saved 并集”。SysV 上多存两个，保存与恢复各多
16 字节访存。**不等于完整 Windows 上下文**：还缺 XMM、浮点控制与平台栈状态，
见 Lesson 6；不要凭这 8 个 push 宣称跨平台 ABI 安全。

---

## 9. 任务（等你有电脑再开始）

### 任务 A：读帧

给你一段 `-O2 -S` 输出（开讲时发），标出：

- 返回地址、MS 第 5 个整数参数、实际存在的局部槽和对齐空间；
- 哪几条是编译器插的、哪几条来自源码。

先记入口 `S`，再为每条 push/sub 记录 `D`，最后统一换算 `S+偏移`。
被优化掉的局部变量标“无栈槽”，没有 canary 就标“未观察到”，不要硬画。

### 任务 B：保存/恢复

先在普通函数的 extended asm 里正确声明 `rbx/r12` clobber，
用 `-S` 找出编译器生成的保存/恢复。此阶段不手写 push/pop。
到 Lesson 5.5 再做整函数版，用汇编调用者装入哨兵值、调用、比对；
不能在两个独立 asm 之间假定编译器始终不碰某寄存器。

### 任务 C：帧指针对照

同一个函数分别用 `-O2` 和 `-O2 -fno-omit-frame-pointer -fno-stack-protector` 编译，
贴 `-S`，说明 `rbp` 链和 `rsp` 相对寻址的差别。

### 任务 D：问答题

1. 为什么 `call` 前 `rsp % 16 == 0`，而函数入口是 `8`？
2. 哪些寄存器在 Windows 上是 callee-saved、在 Linux 上不是？
3. 为什么协作式切换不保存 flags，抢占式必须保存？
4. 切栈为什么不影响 canary？

---

## 10. 易错点

1. 把 push/pop 的地址效果示意误当成固定微操作数量。
2. 忘记 `push`/`pop` 成对，导致 `rsp` 偏移，`ret` 弹错地址。
3. 在 asm 里改 `rsp` 却没把账算平（Lesson 6 会讲为什么这直接判死刑）。
4. 把 `rsi`/`rdi` 当 caller-saved 写到 Windows 代码里。
5. 以为栈上局部变量的地址在切换后还有效 —— 它只在**那个 fiber 的栈**上有效。
6. 以为 `-O2` 一定有帧指针。
7. 用红区存跨普通调用的值；把 SysV 信号保证记反。
8. 把 Linux TLS guard 与栈副本混为一谈，或拿去解释所有 MinGW 输出。

---

## 11. 自测

1. `pushq %rbx` 之后 `rsp` 变多少？`popq` 呢？
2. 函数入口时 `rsp % 16` 等于几？为什么？
3. 第 7 个参数在 `-O2` 的 SysV 代码里怎么取？
4. 红色区域多大？谁能用？Windows 有吗？
5. `rbx`、`r12` 在两边 ABI 里分别归谁保存？
6. 为什么 context switch 不用保存 `rax`？
7. 协作式切换要不要 `pushfq`？为什么？
8. canary 存在哪？切栈会不会破坏它？

---

## 12. 和前后课的关系

| 课 | 用本课的什么 |
|---|---|
| Lesson 4 early clobber | 多输出时"谁先写"的时序，本质还是寄存器的读写顺序 |
| Lesson 5 调用约定 | §4–§6 的展开（参数、对齐、保存责任） |
| Lesson 6 栈切换 | §7 的清单直接变成 `push`/`pop` 序列 |
| Lesson 7 首次进入 | §2 的帧图 + §4 的对齐账 |
| Lesson 9 boost | `jump_fcontext` 保存的正是 §7 那张表 |
