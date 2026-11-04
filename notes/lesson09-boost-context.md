# Lesson 9 教案 — 与 `boost.context` 对照

> **状态：教案 v1，未开讲。**
> 前置：Lesson 6–8（切换、首次进入、调度器）。
> 本课目标：把 [`jump_x86_64_sysv_elf_gas.S`](https://github.com/boostorg/context/blob/develop/src/asm/jump_x86_64_sysv_elf_gas.S)
> 逐行读懂，并说清**我们的 demo 少了什么、为什么工业代码必须补上**。

---

## 0. 先看接口

```cpp
namespace boost { namespace context {

typedef void* fcontext_t;                 // 本质就是"保存的 rsp"

struct transfer_t {
    fcontext_t  fctx;                     // 目标上下文
    void       *data;                     // 传给对方的指针
};

transfer_t jump_fcontext(fcontext_t const to, void *vp);
fcontext_t make_fcontext(void *sp, std::size_t size, void (*fn)(transfer_t));
transfer_t ontop_fcontext(fcontext_t const to, void *vp, transfer_t (*fn)(transfer_t));

}}
```

三个函数的分工：

| 函数 | 干什么 | 对应本课程 |
|---|---|---|
| `make_fcontext` | 在栈上伪造一个"首次进入"的帧 | Lesson 7 `init_ctx` |
| `jump_fcontext` | 保存当前、切到目标，返回 `transfer_t` | Lesson 6 `ctx_switch` |
| `ontop_fcontext` | 切到目标栈上**先执行一个函数**，再继续 | Lesson 8 的"在对方栈上做事" |

---

## 1. `jump_fcontext` 的骨架（去掉注释/CET 后约 40 行）

来源：`jump_x86_64_sysv_elf_gas.S`（147 行，含大段注释和 CET 分支）。

```asm
jump_fcontext:
    leaq  -0x40(%rsp), %rsp     # 往下留 64 字节保存区
    stmxcsr  (%rsp)             # 保存 MXCSR
    fnstcw   0x4(%rsp)          # 保存 x87 控制字
    movq  %fs:0x28, %rcx        # 读 TLS 里的 stack guard
    movq  %rcx, 0x8(%rsp)
    movq  %r12, 0x10(%rsp)      # 保存 callee-saved
    movq  %r13, 0x18(%rsp)
    movq  %r14, 0x20(%rsp)
    movq  %r15, 0x28(%rsp)
    movq  %rbx, 0x30(%rsp)
    movq  %rbp, 0x38(%rsp)
    movq  %rsp, %rax            # 返回给调用者的 fcontext_t

    movq  %rdi, %rsp            # 切到目标栈
    movq  0x40(%rsp), %r8       # 取出对方的"返回地址"
    ldmxcsr  (%rsp)             # 恢复 MXCSR
    fldcw    0x4(%rsp)
    movq  0x8(%rsp), %rdx       # 恢复 stack guard 到 TLS
    movq  %rdx, %fs:0x28
    movq  0x10(%rsp), %r12      # 恢复 callee-saved
    ...
    leaq  0x48(%rsp), %rsp      # 把栈恢复到"函数返回后"
    movq  %rsi, %rdx            # transfer_t.data
    movq  %rax, %rdi            # 第一个参数 = fctx
    jmp   *%r8                  # 跳过去（不是 ret）
```

对照我们的 `ctx_switch`，差别一目了然：

| 项目 | 我们 | boost | 为什么 |
|---|---|---|---|
| 保存区 | `push` 8 次 | 一次性 `sub` + `mov` | 布局固定，省指令、方便对齐 |
| 跳转 | `ret` | `jmp *%r8` | CET 影子栈 |
| flags | 不存 | 不存 | 协作式 |
| MXCSR/x87 | 不存 | 存 | 控制位是 callee-saved |
| stack guard | 不存 | 存 | 栈保护 canary 在 TLS |
| 返回值 | 无 | `transfer_t`（`rax`+`rdx`） | 能顺带传一个指针 |
| 影子栈 | 无 | `rstorssp`/`saveprevssp` | CET |

---

## 2. `transfer_t` 怎么"返回"两个值

SysV 的返回值约定：

- 第一个返回值 `rax`；
- 第二个（仅 128 位整型/结构体）`rdx`。

`jump_fcontext` 返回 `transfer_t`（两个指针），所以：

```
rax = fctx（新上下文）
rdx = data（调用时传进来的 vp）
```

而"切到目标之后"的第一次调用，两个值走的是**参数寄存器**：

```
rdi = fctx
rsi = data
```

源码里的 `movq %rsi, %rdx; movq %rax, %rdi` 就是在做这个转换。
**这就是为什么 `jump_fcontext` 的签名是 `transfer_t` 而不是 `void`** ——
它同时是"切换"和"传参"。

---

## 3. `make_fcontext`：和我们的 `init_ctx` 对得上

```asm
make_fcontext:                 # rdi=栈顶, rsi=大小, rdx=fn
    movq  -0x8(%rdi), %r9      # CET 用
    movq  %rdi, %rax
    andq  $-16, %rax           # 栈顶向下对齐到 16
    leaq  -0x48(%rax), %rax    # 留 72 字节保存区
    movq  %rdx, 0x30(%rax)     # 存 fn
    stmxcsr  (%rax)
    fnstcw   0x4(%rax)
    movq  %fs:0x28, %rcx
    movq  %rcx, 0x8(%rax)      # 存 canary
    leaq  trampoline(%rip), %rcx
    movq  %rcx, 0x40(%rax)     # 首次进入的返回地址
    ...
    ret                        # 返回 fcontext_t（= rax）
```

对照 Lesson 7 的假帧：

| | 我们 | boost |
|---|---|---|
| 栈顶对齐 | 数组 `alignas(16)` | `andq $-16` 强制对齐 |
| 保存区 | 80 字节 | 72 字节 = mxcsr(0x0) + x87cw(0x4) + guard(0x8) + 6 个 GPR(0x10–0x38) + 返回地址(0x40) |
| 入口 | 直接放 `fiber_a` | 放 `trampoline`，`fn` 另存 |
| 为什么多一层 trampoline | —— | 要先把 `fn` 变成 `transfer_t` 参数、处理 CET |
| 返回地址 | `fiber_exit` | `finish`（直接 `_exit`） |

**注意**：boost 的 `make_fcontext` 不保存 `rsi/rdi`（SysV 上它们是 caller-saved），
只存 6 个 callee-saved。这和我们"两套 ABI 并集存 8 个"的选择不同 ——
boost 每个平台一份文件，不需要并集。

---

## 4. `ontop_fcontext`：在对方的栈上跑一下

语义：切到目标上下文，**先执行你给的函数**，再决定是继续还是切回。

用途：在目标 fiber 的栈上安全地做一件事（比如释放它的栈、读它的局部状态），
做完再切回来。没有它，你只能在"当前栈"上操作，碰不到对方的栈。

---

## 5. 那些"多余"的东西为什么必须有

| 代码 | 解决什么问题 | 没有会怎样 |
|---|---|---|
| `stmxcsr`/`ldmxcsr` | MXCSR 的控制位（舍入、flush-to-zero）是 callee-saved | 两个 fiber 的浮点行为互相污染 |
| `fnstcw`/`fldcw` | x87 控制字同理 | 同上 |
| `%fs:0x28` guard | `-fstack-protector` 的 canary 在 TLS，切换要保证一致 | 跨切换后 canary 检查失败 |
| `rstorssp`/`saveprevssp` | CET 影子栈 | `ret` 触发 `#CP`；`jmp` 也要恢复 SSP |
| `_CET_ENDBR` | CET 的间接跳转目标标记 | 开了 CET 的机器上直接崩 |
| `.cfi_undefined rip` | 告诉 unwinder"这里没有调用者" | 异常/调试回溯错乱 |

**这一课最重要的一句话**：demo 和 boost 的差别，不在"切换逻辑"，在"可移植性和运行时契约"。
切换逻辑 40 行就够；剩下的 100 行是给操作系统、调试器、安全机制看的。

---

## 6. Windows 变体

boost 在 Windows 上有四份实现：

```
jump_x86_64_ms_pe_masm.asm     # MSVC 的 MASM
jump_x86_64_ms_pe_gas.asm      # GAS（MinGW 用这个）
jump_x86_64_ms_pe_clang_gas.S  # clang
```

差异：

- 参数寄存器 `rcx/rdx`（不是 `rdi/rsi`）；
- 调用前 32 字节 shadow space；
- callee-saved 多 `rsi/rdi/xmm6–15`；
- unwind 用 `.seh_*`；
- 没有 `.note.GNU-stack`。

**你在这台 MinGW 上对照时，读 `jump_x86_64_ms_pe_gas.asm`，不要读 ELF 版** ——
寄存器名字和栈布局都不一样。

---

## 7. 我们的 demo 与 boost 的差距（表）

| 能力 | hello-switch | boost.context |
|---|---|---|
| 切换 | ✅ | ✅ |
| 首次进入 | ✅ | ✅ |
| 传参 | 手工塞槽位 | `transfer_t` |
| 对齐 | ✅ | ✅ |
| MXCSR/x87 | ❌ | ✅ |
| canary | ❌ | ✅ |
| CET | ❌ | ✅ |
| 平台 | 一份宏 | 每平台一份 |
| unwind | ❌ | 有 CFI |
| 测试 | 打印 | 完整单测 |

---

## 8. 任务

### 任务 A：逐行注释

把 `jump_x86_64_sysv_elf_gas.S` 的核心 40 行抄下来，每行写一句"为什么"。
不许抄注释，用自己的话。

### 任务 B：画栈布局

画出 `make_fcontext` 返回的 `fcontext_t` 指向的那 72 字节，标出每个字段的偏移
（`0x0` 到 `0x40`），并解释每个字段在 `jump_fcontext` 里被谁读。

### 任务 C：补一个能力

给我们的 `hello-switch` 加上 **`transfer_t` 式传参**（`ctx_switch` 返回一个 `void*`）。
提示：SysV 返回值在 `rax`，第二个在 `rdx`；Windows 只有 `rax`。

### 任务 D：问答

1. 为什么 `jump_fcontext` 用 `jmp` 不用 `ret`？
2. `transfer_t` 的两个字段分别走哪两个寄存器？
3. `make_fcontext` 为什么要 `andq $-16`？
4. MXCSR 不保存会出什么 bug？
5. Windows 版和 ELF 版差哪几件事？

---

## 9. 易错点

1. 读 ELF 版却用在 Windows 上（寄存器全错）。
2. 以为 `jump_fcontext` 是"调用"，其实它是"切走 + 返回"。
3. 忘了 `transfer_t` 的第二个值在 `rdx`。
4. 以为 boost 存了 flags（它没存）。
5. 以为 `make_fcontext` 的 `size` 参数只是"建议"（它要求栈顶/大小都合法）。
6. 忽略 CET：在开了 CET 的机器上 `ret` 切栈直接 `#CP`。

---

## 10. 自测

1. `fcontext_t` 本质是什么？
2. `jump_fcontext` 保存了哪些寄存器？和 Lesson 3 的清单对得上吗？
3. `transfer_t` 在"切走"和"进入"两个方向各走哪些寄存器？
4. `make_fcontext` 的 72 字节里有什么？
5. 为什么 boost 不保存 flags？
6. 影子栈和普通栈的 `ret` 为什么要一致？

---

## 11. 课程收束

到这里，整条线闭合了：

```
约束/clobber → 栈与保存 → ABI → naked/.S → 切换 → 首次进入 → 调度器 → boost
```

你已经能从"一条 `asm` 该写什么"一路追到"`jump_fcontext` 为什么这么写"。
剩下的不是新知识，是把每一课的**证据**（`-S`、`objdump`、运行结果）攒够。
