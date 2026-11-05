# 问答记录

练习过程中真正卡住过的问题，按提出顺序记录。答案都落到了具体的指令、寄存器或 `-S` 输出上。

---

## Q1：`and` 又不是加法，为什么会改标志位？

**问题**：`and` 不产生进位，为什么还要在 clobber 里写 `"cc"`？

**答**：`"cc"` 是 **condition code register**，指整个 EFLAGS，不是 carry flag。GCC 文档里的 `"cc"` 从来没有"进位"的意思。

常用算术条件位是 CF(0)、PF(2)、AF(4)、ZF(6)、SF(7)、OF(11)。
条件跳转具体读哪些位取决于指令；DF(10) 控制串指令方向，不是普通算术条件码。
声明 `"cc"` 不能替你恢复 ABI 要求的 DF 状态。

Intel SDM 对 `AND`/`OR`/`XOR`/`TEST` 的规定是硬的：**OF 和 CF 清零，SF/ZF/PF 按结果设置，AF 未定义**。所以 `and` 至少改了 5 个位。

这条规定是刻意设计的：`and eax, eax` 必须能当零测试用（`test` 指令的编码就是 `and` 去掉写回目的操作数，`85 /r` 对 `21 /r`），逻辑运算不产生有意义的进位，就规范地清掉，免得后面 `adc` 吃到脏 CF。

**反例最能说明问题**：`INC`/`DEC` 修改 OF、SF、ZF、AF、PF，**唯独不动 CF**。所以 `inc` 一个进位都不产生，照样必须写 `"cc"`。

`"cc"` 显式声明条件码破坏。本课程要求写全，但不能说省略它后所有编译器都假设 flags 不变：
GCC 13 的 x86 后端在没有 flag output 时会隐式加入 flags clobber，
见 [Lesson 2 §3.4](lesson02-clobber.md)。旧稿把未复现的错码归因于漏 cc，证据不足。

**判据**：检查具体指令的 SDM *Flags Affected*，不要把这里列出的常见指令当完整清单；
普通 `mov/lea/bswap/not` 不改算术 flags，`pushfq/popfq` 等不能套普通 push/pop 的规则。

---

## Q2：验证 `"i"` 约束，难道不该写两段对比吗？

**问题**：作业要"验证 `"i"` 约束"，只留一段怎么验证？

**答**：对比是对的，但**对照组要选对**。

写

```cpp
andq $0xFF, %0      // 字面量
andq %2, %0         // "i"(0xFF)
```

这两条在 `-S` 里都是同一件事（`andq $0xFF` 与 `andq $255`），验证的只是"编译器会把 255 写成十进制"。

真正要对比的是 **`"i"` vs `"r"`**：

```asm
	and $255, %rdx              # "i"：立即数编进指令字段，一条指令
	movl	$255, %eax          # "r"：常量先物化进通用寄存器
	and	%rax, %rdx          #     再做寄存器-寄存器 and
```

而且 `"i"` 展开时**自带 `$`**，模板里再写 `$%1` 会变成 `$$255`，汇编器直接报错。

`"i"` 的边界在失败路径上：传运行期变量会得到

```
error: impossible constraint in 'asm'
```

---

## Q3：`"r"(0xFFu)` 为什么编译不过，改成 64 位就好了？

**问题**：

```cpp
__asm__ volatile("andq %1, %0" : "=r"(out) : "r"(0xFFu));
```

报错，改成 `0xFFull` 就过了。

**答**：类型、机器模式、寄存器名、模板后缀四者必须对齐。

```
0xFFu  →  unsigned int  →  SImode（32 位）
       →  "r" 只承诺"装得下的通用寄存器"  →  %eax
       →  模板写的是 andq  →  `andq %eax, %rdx`
       →  Error: incorrect register '%eax' used with 'q' suffix
```

GCC 分配的是**和 C 类型同宽**的寄存器；`q` 后缀是你手写的，两者对不上就是汇编器报错。三种正解：

| 方案 | 写法 | 代价 |
|---|---|---|
| 对齐类型 | `"r"(0xFFull)`，操作数变 DImode → `%rax` | 无 |
| 对齐后缀 | `"andl %1, %0"` 配 `unsigned` | 结果只有 32 位，自己负责零扩展 |
| 强制宽度 | 模板用 `%q1` | 拿一个 32 位操作数的"高半部分"，值不保证 |

同一个机制的正面用法是 `rotl64_asm`：`unsigned n` 的 `%1` 会展开成 `%ecx`，而 `rol` 的计数只接受 `imm8` 或 `%cl`，所以要用 `%b1` 取低字节名 → `%cl`。

---

## Q4（已答）：`rotl64_asm` 里把 `"c"(n)` 换成 `"r"(n)` 会怎样？

答案在 [`lesson01-deep-dive.md` §4](lesson01-deep-dive.md)：`%b` 只是"取该寄存器所在的字节名"，
不搬运数据。`"r"(n)` 下实测展开成 `rolq %sil, %rax`，汇编器报
`Error: operand type mismatch for 'rol'` —— 因为 `rol` 的计数只接受 `imm8` 或 `%cl`。

想自己再撞一次的话：改完留好报错原文。

---

## Q5：为什么 `bad_store` 有时“看起来是对的”，仍是错误？

`"r"(p)` 只声明地址值，未声明模板写入 `*p`。优化器仍可合并/搬动 C++ 中的读取；
这份接口不能覆盖实际副作用。错误是否显现受内联、别名分析、优化级别影响。
修复应精确声明内存输出或确有必要时用 `"memory"`，不是加 `volatile int*` 掩盖。
详见 [Lesson 2 §2](lesson02-clobber.md)。这一答案是备课材料，不表示知识点已开讲/通过。

---

## Q6：学过 x86 汇编，为什么还是看不懂一个 `printf` 调用？

**问题**：指令都认识（`mov`/`lea`/`call`），但看到 `printf` 那几行就不知道在干什么。

**答**：因为 `printf` 调用点是 **ABI 的"全考点"**，指令本身只占一行。
把 `-S` 拉出来看：

```cpp
void show(int i, double d) {
    std::printf("%d %.2f\n", i, d);
}
```

SysV（Linux）——注意 Ubuntu 上是 `__printf_chk`，不是 `printf`：

```asm
show:
    movl  %edi, %edx          # i：第 3 个参数 → rdx
    leaq  .LC0(%rip), %rsi    # 格式串："%d %.2f\n" → rsi（第 2 个参数）
    movl  $2, %edi            # __printf_chk 的 flag（_FORTIFY_SOURCE）
    movl  $1, %eax            # 用了 1 个向量寄存器（d 在 xmm0）
    jmp   __printf_chk@PLT    # 尾调用，不是 call
```

MinGW（MS x64）：

```asm
show:
    movl   %ecx, %edx         # i：第 2 个参数 → rdx
    movq   %xmm1, %r8         # d 复制进第 3 个参数对应的 GPR
    movupd %xmm1, %xmm2       # 同时放进第 3 个参数对应的 XMM
    leaq   .LC0(%rip), %rcx   # 格式串 → rcx（第 1 个参数）
    jmp    _Z6printfPKcz      # 尾调用
```

一个"普通"的 `printf` 调用，考了七件事：

| # | 考点 | 在这段里长什么样 |
|---|---|---|
| 1 | 参数顺序与位置 | 格式串、`i`、`d` 各去哪 |
| 2 | 两个寄存器文件 | 整数走 GPR，`double` 走 `xmm` |
| 3 | 可变参数的 `%al` | SysV 的 `movl $1, %eax` |
| 4 | MS 的 varargs 双写规则 | `movq %xmm1, %r8` + `movupd %xmm1, %xmm2` |
| 5 | libc 的包装 | `__printf_chk`（fortify）、`_Z6printfPKcz`（C++ 名字） |
| 6 | PIC/PLT | `.LC0(%rip)`、`@PLT` |
| 7 | 尾调用与对齐 | 直接 `jmp`，没有 `call`/`ret` |

这些**不是指令知识**，是"编译器与运行时的合同"。只学指令表，就会卡在这里 ——
这跟聪不聪明无关，是材料少了一块。本课程的 Lesson 3（栈）、Lesson 5（调用约定）、
Lesson 5.5（符号/链接）就是补这一块。

顺带：你想真正"看懂 `printf`"，看到调用点就够了；`printf` 内部怎么解析格式串是 libc
的实现细节，不是这门课的目标。
