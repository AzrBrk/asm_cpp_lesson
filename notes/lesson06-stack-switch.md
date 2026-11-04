# Lesson 6 教案 — 栈切换

> **状态：教案 v1，未开讲。**
> 前置：Lesson 3（保存清单）、Lesson 5.5（naked / `.S`）。
> 本课目标：写出一个能用的 `ctx_switch`，并说清**为什么它必须离开 extended asm**、
> 保存集合怎么来的、栈怎么分配、哪些东西会静默坏掉。

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

完整可跑的版本在 [`demos/hello-switch/`](../demos/hello-switch/)，137 行，含栈布局图。

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
在 Linux 上多存 `rsi/rdi` 只是多 16 字节访存，换来一份代码两边都能跑。

---

## 3. 栈的对齐：两个数决定生死

- 任意函数**入口**：`rsp % 16 == 8`；
- **`call` 前**：`rsp % 16 == 0`。

`ctx_switch` 是"返回到"对方，所以对方入口的 `rsp` 也必须满足"函数入口"的约定。
在 [`hello-switch`](../demos/hello-switch/hello_switch.cpp) 里：

```
初始 sp = top - 80
pop 8 个（64 字节）→ rsp = top - 16
ret 弹返回地址   → rsp = top - 8
```

`top` 是 16 对齐的，所以 `top - 8` 满足 `%16 == 8` —— 正好是函数入口的样子。
**差 8 字节不会立刻崩**，但会在某个用 `movaps` 的函数里崩，而且换机器才复现。

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

**大小估算**：栈上会跑 `printf`、可能还有异常展开；16 KiB 起步，demo 用 128 KiB。
栈溢出在 Linux 上表现为踩到相邻内存或 guard page；在 Windows 上还会被 TEB 的栈限制
（`StackBase`/`StackLimit`）影响 —— 手写 `rsp` 切换**不会更新 TEB**，
所以 Windows 的异常/栈检查可能认为你在"栈外"。这是 boost 在 Windows 上也要小心的点。

---

## 6. 红区、canary、TLS

- **红区**：SysV 叶子函数会用 `rsp` 以下 128 字节。`ctx_switch` 自己 `push` 会写到调用者的
  红区里 —— 但调用者既然 `call` 了你，就不是叶子，红区不用。规则：**切换函数里别依赖红区**。
- **canary**：在 TLS 里，切换不破坏；但手工构造的栈帧如果让函数"返回"到没写过 canary 的帧，
  检查会失败。demo 里 `fiber_exit` 就是给这条留的后路。
- **TLS / errno**：切栈不切 TLS，所以 `errno` 是 fiber 之间共享的。
  两个 fiber 同时用 `errno` 会互相污染 —— 这是"协程不是线程"的一个具体后果。

---

## 7. 性能：一次切换多少钱

一次 `ctx_switch` 的成本：

- 16 次 `push`/`pop`（8 存 8 取）+ 2 次 `mov` + 1 次 `ret`；
- 全是 L1 命中，几十个周期量级；
- 真正贵的是**缓存**：换了栈之后，原来热的数据可能不在 L1 了。

测量方法（Lesson 2 的 `rdtsc`）：

```cpp
t0 = rdtsc();
for (int i = 0; i < 100000; ++i) { ctx_switch(&a, b); ctx_switch(&b, a); }
t1 = rdtsc();
```

注意：`rdtsc` 不是序列化指令，循环前后要 `lfence`，否则测出来偏小。
而且 10 万次切换会把预测器/缓存状态搅乱，数字只能横向比较，不能当绝对值。

---

## 8. 任务

### 任务 A：第三个 fiber

在 demo 基础上加 `fiber_c`，三个函数轮流打印。要求：

- 每个 fiber 有自己的 `sp`；
- 加一个 `yield_to(next)` 的小封装；
- 贴运行结果和 `-S`（证明没有多余 prologue）。

### 任务 B：对齐实验

把 `init_ctx` 里的 `top - 10` 改成 `top - 11`（故意错 8 字节），

- 先跑 demo，记下结果；
- 再加一个用 `double`/`printf("%f")` 的 fiber，看什么时候崩；
- 解释为什么"第一版不崩"。

### 任务 C：守卫页

用 `mmap` 分配栈，最后一页设 `PROT_NONE`，写一个递归函数把栈打爆，
确认 `SIGSEGV` 落在守卫页而不是别处。

### 任务 D：测量

用 `rdtsc` 测 10 万次切换的周期数，和 `setjmp/longjmp` 对比，说明差在哪。

---

## 9. 易错点

1. 在 extended asm 里换 `rsp`（编译器所有 `"m"` 操作数全废）。
2. 少存一个 callee-saved（`-O2` 下"碰巧"能用，换编译器就炸）。
3. 初始栈差 8 字节（`movaps` 时崩）。
4. 以为切栈会把 TLS 也切了（`errno` 互相污染）。
5. 用 `malloc` 的栈没做 16 字节对齐。
6. Windows 上不更新 TEB 的栈限制（异常/栈检查会误判）。
7. 切换函数里 `push` 踩到调用者的红区（同栈时）。
8. 忘了 `"memory"`：编译器把跨切换的全局变量缓存在寄存器里。

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
