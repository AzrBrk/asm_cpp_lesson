# 问答记录

练习过程中真正卡住过的问题，按提出顺序记录。答案都落到了具体的指令、寄存器或 `-S` 输出上。

---

## Q1：`and` 又不是加法，为什么会改标志位？

**问题**：`and` 不产生进位，为什么还要在 clobber 里写 `"cc"`？

**答**：`"cc"` 是 **condition code register**，指整个 EFLAGS，不是 carry flag。GCC 文档里的 `"cc"` 从来没有"进位"的意思。

EFLAGS 里跟优化器相关的至少六位：CF(0) 进位、PF(2) 奇偶、ZF(6) 零、SF(7) 符号、OF(11) 溢出、DF(10) 方向。你写的每一个 `if (x == 0)` 读的是 **ZF**，`x < 0` 读 SF，有符号比较读 OF/SF/ZF —— CF 反而用得最少。

Intel SDM 对 `AND`/`OR`/`XOR`/`TEST` 的规定是硬的：**OF 和 CF 清零，SF/ZF/PF 按结果设置，AF 未定义**。所以 `and` 至少改了 5 个位。

这条规定是刻意设计的：`and eax, eax` 必须能当零测试用（`test` 指令的编码就是 `and` 去掉写回目的操作数，`85 /r` 对 `21 /r`），逻辑运算不产生有意义的进位，就规范地清掉，免得后面 `adc` 吃到脏 CF。

**反例最能说明问题**：`INC`/`DEC` 修改 OF、SF、ZF、AF、PF，**唯独不动 CF**。所以 `inc` 一个进位都不产生，照样必须写 `"cc"`。

漏写 `"cc"` 的后果：GCC 把 EFLAGS 视为隐式寄存器 `reg:CC`，没声明就是"这条 asm 前后 flags 不变"，调度器于是可以把 `cmp`/`test` 提到 asm 前面复用，或者把 asm 后面的条件跳转当成还基于旧的 flags —— 静默的 miscompile。

**判据**：不写 flags 的只有 `mov` / `lea` / `push` / `pop` / `bswap` / `nop` / `cmov` / `not`（`not` 不改 flags）；`add` `sub` `and` `or` `xor` `test` `cmp` `shl` `shr` `rol` `imul` `inc` `dec` 全都要写。

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

## Q4（待答）：`rotl64_asm` 里把 `"c"(n)` 换成 `"r"(n)` 会怎样？

留给自己的家庭作业。提示：`rol` 的计数操作数在编码里只有两种形态——`imm8`，或者**硬编码的 `%cl`**，没有"任意 r64 当计数"这条编码。改完把编译错误原样留存。

---

## Q5（待答）：为什么 `bad_store` 有时"看起来是对的"，却仍是 UB？

Lesson 2 的问题，等 `"memory"` 屏障讲完再答。
