# Lesson 7 教案 — 首次进入

> **状态：教案 v1，未开讲。**
> 前置：Lesson 6（栈切换）、Lesson 3（帧与对齐）。
> 本课目标：让一个**从没运行过**的函数"从 `ctx_switch` 里返回"那样开始执行，
> 并给它传参数。

---

## 0. 问题：新栈上没有"上次"

`ctx_switch` 的工作是"返回到对方上次停下的地方"。可是新 fiber 从没调用过它 ——
它的栈顶是什么？返回地址在哪？

答案：**你伪造一个"刚被切走"的帧**，让 `ctx_switch` 以为它只是"上次被切走了"。

---

## 1. 假帧的布局

```
高地址
┌──────────────────────────┐ ← top（16 字节对齐）
│  fiber_exit（返回地址）   │ ← [top-8]   fiber 若 return 就落到这里
├──────────────────────────┤ ← [top-16]  ctx_switch 的 ret 目标
│  fiber 入口地址           │
├──────────────────────────┤
│  r15  r14  r13  r12       │  8 个「保存寄存器」槽位
│  rdi  rsi  rbx  rbp       │  （顺序和 push/pop 相反）
├──────────────────────────┤
└──────────────────────────┘ ← sp = top - 80
低地址
```

对应的初始化代码：

```cpp
uint64_t *top = stack + kStackWords;   // 16 对齐
uint64_t *sp  = top - 10;              // 80 字节

sp[8] = (uint64_t)entry;               // ctx_switch 的 ret 目标
sp[9] = (uint64_t)fiber_exit;          // fiber 的返回地址

*slot = sp;
```

---

## 2. 为什么是 `top - 10`、`top - 8`

一步一步算：

1. `top` 16 字节对齐（`alignas(16)` + 数组长度是 16 的倍数）。
2. `sp = top - 80`。
3. `ctx_switch` 从 `sp` 开始 `pop` 8 个寄存器 → `rsp = top - 16`。
4. `ret` 弹出 `[top-16]` 的入口地址 → `rsp = top - 8`。
5. 入口函数看到 `rsp = top - 8`，即 `(top - 8) % 16 == 8` —— **正好是函数入口的约定**。

第 5 步是全部意义所在：C 函数一进门就假设 `rsp % 16 == 8`，
它的序言据此把栈再对齐到 0 去 `call` 别人。差 8 字节，`movaps` 就会崩。

---

## 3. 传参数：往"保存槽"里塞

`ctx_switch` 的 `pop` 顺序是 `r15 r14 r13 r12 rdi rsi rbx rbp`，
所以 `rdi` 槽位（`sp[4]`）里放什么，入口函数就"收到"什么：

```cpp
sp[4] = 12345;                        // 入口函数的第一个参数
sp[8] = (uint64_t)&fiber_arg;
```

实测：

```
fiber 收到参数: 12345
```

这不是魔法：`popq %rdi` 把 12345 装进了 `rdi`，紧接着 `ret` 跳到
`fiber_arg`，而 SysV 的第一个参数正好在 `rdi`。
（Windows 上第一个参数在 `rcx`，所以要把值放进 `rcx` 槽位 —— 这就是
`make_fcontext` 要"把 data 放到正确的参数寄存器"的原因。）

---

## 4. 退出路径

fiber 函数返回时，会 `ret` 到 `[rsp]` —— 也就是 `sp[9]` 里放的 `fiber_exit`。
所以一个完整的 fiber 生命周期是：

```
伪造帧 → 首次进入 → 跑 → return → fiber_exit → 切回 main
```

`fiber_exit` 里再 `ctx_switch(&dead, sp_main)`。它的栈随后可以释放/复用 ——
但**不要在它自己的栈上释放**，因为那行代码还在用它。

> 本课程的 demo 里 fiber 都是死循环，`fiber_exit` 只是保险。真要支持"fiber 结束"，
> 这里要加一层"回收栈"的机制。

---

## 5. `ret` 还是 `jmp`：boost 的选择

我们的 `ctx_switch` 用 `ret` 跳到对方。boost 的 `jump_fcontext` 用的是：

```asm
movq 0x40(%rsp), %r8    # 取出对方的返回地址
...
leaq 0x48(%rsp), %rsp   # 把栈恢复到"函数返回后"的样子
jmp  *%r8               # 直接跳，不用 ret
```

为什么不用 `ret`？因为 **CET 的影子栈**（shadow stack）：

- `ret` 会同时弹普通栈和影子栈；如果你只是 `movq %rsp` 换了普通栈，
  影子栈没换，`ret` 会因两者不匹配而触发 `#CP`；
- 用 `jmp` 就绕开了这个检查（boost 还额外用 `rstorssp`/`saveprevssp` 恢复影子栈）。

在没有 CET 的普通机器上，`ret` 和 `jmp` 都行；要可移植，boost 选 `jmp`。
**这是"为什么工业代码比 demo 长"的第一个原因。**

---

## 6. `make_fcontext` 就是干这个的

boost 的 `make_fcontext(stack_top, stack_size, fn)` 返回一个 `fcontext_t`，
内部做的正是本课的假帧构造：

1. 从栈顶往下留出保存区；
2. 写入入口 trampoline 的地址；
3. 写入 `fn` 作为"入口函数的参数"（它用一个 trampoline 把 `fn` 转成调用）；
4. 返回新的栈顶指针。

差别在于它还处理了对齐、CET、以及"第一次进入时把 `fn` 当参数传进去"的细节。
Lesson 9 会逐行对。

---

## 7. 任务

### 任务 A：给 fiber 传参

把 demo 改成 `fiber_a(long id)` / `fiber_b(long id)`，通过保存槽传不同的 id，
打印 `fiber 1` / `fiber 2`。要求：两个平台都能编，说明槽位怎么选。

### 任务 B：实现 `fiber_exit`

让 fiber 函数正常 `return`（不是死循环），由 `fiber_exit` 切回 main。
要求：在 fiber 返回后打印一条消息，证明没有崩。

### 任务 C：对齐实验

把 `top - 10` 改成 `top - 11`，观察：

- 纯整数的 fiber 是否还正常？
- 加一个 `printf("%f")` 的 fiber 会不会崩？
- 用 `gdb` 看崩在哪条指令（大概率是 `movaps`）。

### 任务 D：`jmp` 版

把 `ctx_switch` 的最后一条 `ret` 换成"先取返回地址到 `%r8`，恢复 `rsp`，再 `jmp *%r8`"。
对比两种写法的 `-S`/`objdump`，说明为什么 boost 选后者。

---

## 8. 易错点

1. 初始 `sp` 算错 8 字节（`movaps` 崩）。
2. 假帧里寄存器槽顺序和 `pop` 顺序不一致（参数/寄存器全错）。
3. 忘了写"fiber 的返回地址"，函数一 `return` 就跳到 0。
4. 在 `fiber_exit` 里释放自己正在用的栈。
5. 以为 `ret` 一定安全（CET 下不是）。
6. 参数槽位按 SysV 写，拿到 Windows 上用（`rdi` vs `rcx`）。
7. 入口函数声明成 `void f()` 却传参数（ABI 上没问题，C++ 里是 UB）。

---

## 9. 自测

1. 假帧里为什么需要"两个返回地址"？
2. `top - 80` 和 `top - 8` 是怎么算出来的？
3. 参数是怎么"传"给入口函数的？
4. fiber 正常返回后会发生什么？
5. `ret` 和 `jmp` 在首次进入里的区别？
6. `make_fcontext` 和本课的 `init_ctx` 是什么关系？

---

## 10. 和前后课的关系

| 课 | 关系 |
|---|---|
| Lesson 3 | 帧布局与对齐账 |
| Lesson 6 | 切换原语提供 `pop`/`ret` |
| Lesson 8 | 每个新 fiber 都要走一次本课的初始化 |
| Lesson 9 | `make_fcontext` = 本课的工业实现 |
