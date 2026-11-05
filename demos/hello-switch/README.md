# hello-switch — 两个函数相互跳

一个文件、一个切换函数、两段私有的栈。

> **课程勘误：这是历史 GPR-only 观察 demo，不是 Windows x64 ABI 安全实现。**
> 除 XMM/控制字与 TEB 状态外，初始 `top-80` 布局没有给 Windows 用户入口留下
> 32 字节 home slots；正常 return 到普通 C++ `fiber_exit` 还存在 8 字节入口对齐错误。
> 后者当前死循环路径不会执行，但不能据此作为完成路径作业答案。
> 代码此处保留作对照；Windows 主线必须按 [Lesson 6](../../notes/lesson06-stack-switch.md)
> 与 [Lesson 7](../../notes/lesson07-first-entry.md) 重建合同/布局，不能以“编译过了”判通过。

两个函数 `fiber_a` / `fiber_b` 各自跑在自己的栈上，用 `ctx_switch` 互相跳：
a 打印 `hello `，跳到 b；b 打印 `world`，跳回 a。三轮后 b 跳回 `main`。

```
hello world
hello world
hello world
done
```

没有调度器、没有链表、没有 `ucontext`。这里仅展示 `rsp` 与 8 个 GPR 的切换骨架，
不是完整运行时状态清单。

## 编译运行

```bash
# Linux
g++ -std=gnu++17 -O2 -Wall -Wextra hello_switch.cpp -o hello_switch && ./hello_switch

# Windows / MinGW（和你本机一致）
g++ -std=gnu++17 -O2 -Wall -Wextra hello_switch.cpp -o hello_switch.exe && hello_switch.exe
```

已验证：Linux GCC 13.3（`-O0/-O1/-O2/-O3/-Os` 都跑过）+ MinGW-w64 GCC 13.2（能编过，
PE 在本机跑不了）。代码里没有任何平台 API，只有两个宏在切参数寄存器。

## 三个角色

| 东西 | 是什么 |
|---|---|
| `g_stack_a` / `g_stack_b` | 两块 128 KiB 的静态数组，当两个函数的私有栈 |
| `g_sp_a` / `g_sp_b` / `g_sp_main` | 每个执行流"上次停在哪"的 `rsp` |
| `ctx_switch(&from, to)` | 存 `rsp`、换 `rsp`、恢复寄存器、`ret` |

`ctx_switch` 不"调用"对方 —— 它是**返回到**对方上次 `call ctx_switch` 的下一句。
所以严格说不是两个函数互相调用，是**两个函数互相让对方"从上次的调用里返回"**。

## 切一次，发生六件事

以 a 主动让给 b 为例：

1. `push` 8 个 callee-saved 寄存器（rbp, rbx, rsi, rdi, r12–r15）到 **a 的栈**；
2. `*from = rsp`：把 a 的栈顶记下来；
3. `rsp = to`：换成 b 的栈顶；
4. 从 **b 的栈**上 `pop` 8 个寄存器；
5. `ret`：弹出 b 栈上那条返回地址，跳到 b 上次 `call ctx_switch` 的下一句；
6. b 继续跑，就像刚才那次调用"刚刚返回"一样。

同理，b 让给 a 时步骤一样，只是栈换回来。

**关键点：`ret` 弹的那条返回地址，是每个执行流自己的。** 谁被切走，谁的返回地址就留在
自己的栈上；谁被切回来，就从自己的栈上把它弹出来。

## 第一次怎么"跳进"一个从没跑过的函数

`fiber_a` 还没执行过，它的栈上没有"上次的返回地址"。所以 `init_ctx` 手工伪造一个：

```
高地址
┌──────────────┐ ← top（16 字节对齐）
│  fiber_exit  │ ← [top-8]   fiber 若 return 就落到这里
├──────────────┤ ← [top-16]  ctx_switch 的 ret 目标
│   fiber_a    │
├──────────────┤
│ r15 r14 r13  │  8 个「保存寄存器」槽位（全 0）
│ r12 rdi rsi  │
│ rbx rbp      │
└──────────────┘ ← sp = top - 80
低地址
```

这张图只适合作为 **SysV 的简化初始入口**对照。第一次切到 a：
`ctx_switch` 从 `sp` 开始 `pop` 8 个 0，然后 `ret` 到 `fiber_a`。
此时 `rsp = top - 8`，正好满足 ABI 的"函数入口 `rsp % 16 == 8`"。

这是首次进入的基础；Windows 还需 shadow space，正常退出需 trampoline 调整，见 Lesson 7。

## 为什么只存 8 个寄存器

编译器在函数调用前后**假设不变**的，只有 callee-saved 那批：

| | SysV（Linux） | MS x64（Windows） |
|---|---|---|
| 必须保存 | `rbx rbp r12–r15` | `rbx rbp rsi rdi r12–r15` |
| 可以随便写 | `rax rcx rdx rsi rdi r8–r11` | `rax rcx rdx r8–r11` |

所以 demo 保存 GPR 部分的并集：`rbp rbx rsi rdi r12–r15`，共 8 个。
caller-saved 不用管 —— 编译器本来就没指望它们在调用后还活着。

## 为什么必须 `naked`

普通函数 GCC 会插 prologue/epilogue，并且**假设 `rsp` 在函数体内不变**。
这个函数中间就把 `rsp` 换了，不能依赖编译器生成普通函数帧；
可写 naked basic asm，也可用独立 `.S`，不是“只能 naked”。

这也是 Lesson 2 那条规则的直接后果：**`rsp` 在 asm 前后必须一样**。
所以要换栈，就不能用普通 extended asm。

## 已知简化（别拿去生产）

- **Windows 上没保存 `xmm6`–`xmm15`**：MS x64 把它们也算 callee-saved。
  这个 demo 全是整数和 stdio，没有浮点值跨切换存活；严格 ABI 安全要再压 160 字节。
- **没有 unwind 信息**：naked 函数没写 `.seh_*`，异常穿过它就别想了。
- **栈不会回收**：fiber 结束的路径只有 `fiber_exit` 一条，本 demo 用不到。
- **Windows 入口缺 shadow space，退出入口对齐错误**：见顶部勘误，不只差 160 字节 XMM 保存。
- **没有 CET 支持**：不能在启用硬件强制机制的环境下保证此 ret 切栈有效。

## 和课程的对应

| 这里的东西 | 课程位置 |
|---|---|
| 保存/恢复 8 个寄存器 | Lesson 3「栈与寄存器保存」 |
| `ctx_switch` 的寄存器集合 | Lesson 5「调用约定」 |
| 换 `rsp` | Lesson 6「栈切换」 |
| `init_ctx` 伪造栈帧 | Lesson 7「首次进入」 |
| 两个 fiber + `g_rounds` | Lesson 8「最小调度器」的雏形 |
| `boost.context` 的 `jump_fcontext` | 就是 `ctx_switch` 加返回值/传参 |

`jump_fcontext` 多做的事：把"让出时带的参数"和"恢复时拿到的返回值"也塞进寄存器，
以及用 `fcontext_t` 把 `rsp` 包成一个类型。骨架就是这 30 行。

## 可以自己改的实验

1. 把 `if (++g_rounds == 3)` 改成 `5`、`10`，看输出。
2. 在 `fiber_a` 里加一个局部变量 `int n = 0; ++n;`，打印它 —— 证明每个 fiber 的栈是独立的。
3. 纸上分析删除一对 pop 的后果：不仅寄存器未恢复，`rsp` 还少推进 16 字节，
   ret 会把保存槽当地址。不要把它当“只漏恢复寄存器”的隔离实验。
4. 加第三个 fiber，把 `g_sp_*` 换成数组 —— 这就是 Lesson 8 的调度器。
