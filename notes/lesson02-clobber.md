# Lesson 2 教案 v2 — Clobber List：memory、cc、rdtsc

> **状态：教案 v2（扩写版），尚未开讲。** v1 只有知识点 1；这一版把 2/3/4 补全，
> 并改正了 v1 里"rdtsc 写 EFLAGS"这个错误。
> 前置：[`lesson01-extended-asm.md`](lesson01-extended-asm.md) + [`lesson01-deep-dive.md`](lesson01-deep-dive.md)。
> 本课学完，你应该能对任何一段 asm 回答三个问题：
> **该写哪些 clobber？为什么是这些？漏了会怎么坏？**
>
> 证据环境：x86-64 GCC 13.3（Linux，SysV）+ MinGW-w64 GCC 13.2（PE，MS x64）。
> 所有 `-S`、报错、运行值都是实测；Windows 目标只给 `-S`（PE 跑不了）。

---

## 0. 鸟瞰：clobber 是在跟谁说话

把一条 `asm` 想成一个你看不见实现的函数：

```
输入操作数  =  参数
输出操作数  =  返回值
clobber     =  副作用清单（"除了返回值，我还弄坏了什么"）
```

编译器对这三样的态度完全不同：

- **操作数**：它负责分配位置、必要时搬运、保证值可用；
- **clobber**：它**只能相信你**，然后据此安排优化。

`clobber` 是整个内联汇编接口里唯一一处**纯自觉**的地方：写漏了不会报错，
编译器只会带着错误的世界模型继续优化你的代码。所以本课的重点不是"记住语法"，
而是**建立编译器的心智模型**：它在 asm 前后分别相信什么。

副作用一共四类，前三类是本课主线：

| 类别 | 写法 | 典型来源 | 本课位置 |
|---|---|---|---|
| 寄存器 | `"rbx"`、`"r12"`… | 模板里硬编码的寄存器 | §1、§6 |
| 内存 | `"memory"` | 通过指针/全局变量偷偷读写 | §2 |
| 条件码 | `"cc"` | 任何写 EFLAGS 的指令 | §3 |
| 栈红区 | `"redzone"` | 在 rsp 以下写东西 | §5（x86 GCC 不认） |

---

## 1. 知识点 1：clobber 是 def，不是"借我用一下"

### 1.1 GCC 拿它当一条"写"

clobber 列表里的每一项，在 GCC 内部都是一条 **def（定义/写）**。它做两件事：

1. **asm 之前**：任何活着、恰好住在这个寄存器里的值，必须搬走（spill 到栈或换个寄存器）。
2. **asm 之后**：该寄存器里的值视为已死，不再被引用。

换句话说：**clobber 不是"我要用这个寄存器"，而是"我把这个寄存器弄坏了"。**
这两句话的差别，就是整个 Lesson 2 的差别。

### 1.2 漏写的代价：编译器让变量继续住在那儿

```cpp
uint64_t accumulate(const uint64_t* p, size_t n) {
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
        __asm__ volatile("movq $0, %%rbx");   // ← 漏了 ::: "rbx"
        acc += p[i];
    }
    return acc;
}
```

`-O2` 下 GCC 很愿意把 `acc` 或 `i` 放进 `%rbx`（callee-saved，循环里不用来回存）。
你的 asm 每轮清零，而 GCC 认为 `%rbx` 没被动过 —— 累加器永远是 0，循环可能连边界都算错。

### 1.3 代价是真实的：编译器会替你 push/pop

补上 `::: "rbx"` 之后，MinGW GCC 13.2 的 `-S`（实测）：

```asm
f:
	pushq	%rbx
	.seh_pushreg	%rbx
	.seh_endprologue
/APP
	movq $1, %rbx
/NO_APP
	leaq	1(%rcx), %rax
	popq	%rbx
	ret
```

所以 clobber **不是免费的**：它把"我弄坏了"翻译成一次真实的保存/恢复。
这就是为什么"能不加就不加"——但前提是模板里**真的没写**那个寄存器。

### 1.4 三条直觉

1. **clobber 只管"写"，不管"读"。** 读一个没在 input 列表里的寄存器，编译器不知道；
   读必须走 input 操作数。
2. **clobber 不参与 `%n` 编号。** 加 `"cc"`、`"memory"`、寄存器名都不用重新数格子。
3. **硬编码寄存器必须进 clobber；走 `%n` 的操作数不要进。** 重复声明会让 GCC 认为
   它自己的分配都无效，轻则多一次搬运，重则报错。

### 1.5 和 `volatile` 的分界

| | 管什么 | 不管什么 |
|---|---|---|
| `asm volatile` | 别删我、别把我搬出循环 | 不表示我碰了内存；不表示我改了什么 |
| clobber | 我弄坏了哪些**寄存器/内存/flags** | 不表示"别优化我" |

两个都要，且互不替代。**`volatile` 的 asm 照样可以漏 `"memory"`** ——
§2 的反例就是一条 `asm volatile`，照样错。

---

## 2. 知识点 2：`"memory"` —— 编译器屏障

### 2.1 编译器为什么"敢"缓存内存

C++ 抽象机里，内存只有你的代码会改。基于这一点，编译器会做很多"聪明事"：

- 把 `*p` 读进寄存器，后面再用就不再访存（CSE）；
- 把一次读**下沉**到后面（sink），或者把一次写**上提**（hoist）；
- 把整个循环折叠成一条乘法（§2.7 的实测）。

而内联汇编是唯一能让"内存悄悄变了"的地方。**你不声明，编译器就继续按原计划优化。**

### 2.2 `"memory"` 让编译器做的三件事

GCC 文档（*Clobbers and Scratch Registers*）的定义：

1. asm 可能读写**操作数列表之外**的内存；
2. 因此执行 asm 前，可能需要把寄存器里的脏值**刷回内存**；
3. asm 之后，**不再假设** asm 前读到的内存值还有效 —— 需要时重新读。

原文一句话概括：

> Using the `"memory"` clobber effectively forms a read/write memory barrier for the compiler.

注意"for the compiler"这个限定词，§2.8 会展开。

### 2.3 反例：漏写之后，两个 `*p` 被合并成一次读（实测）

```cpp
int before = *p;
__asm__ volatile("movl $99, (%0)" : : "r"(p));   // 漏 "memory"
return before + *p;
```

`-O2`（Linux GCC 13.3）：

```asm
#APP
	movl $99, (%rdi)
#NO_APP
	movl	(%rdi), %eax      # 只读这一次
	addl	%eax, %eax        # 99 + 99
ret
```

运行结果：

```
bad_store(&x) = 198     ← 期望 100（1 + 99）
```

编译器做了两件"合法"的事：

1. 它认为 asm 不碰内存，所以 `before` 和 `*p` 是同一个值 → 合并成一次读；
2. 它认为 asm 不碰内存，所以这次读可以**沉到 asm 后面** → 读到的是刚写进去的 99。

两个 99 相加，198。

### 2.4 加上 `"memory"` 之后

```asm
	movl	(%rdi), %eax      # 读在 asm 之前 → before = 1
#APP
	movl $99, (%rdi)
#NO_APP
	addl	(%rdi), %eax      # asm 之后重新读 → 99
ret
```

```
good_store(&x) = 100
```

注意关键是**两件事都发生了**：读没有被合并，也没有被搬过 asm。
`"memory"` 同时挡住了 CSE 和重排。

### 2.5 什么时候必须写

只要满足任意一条，就应该写 `"memory"`：

- 模板里有**指针解引用**，而那块内存不在 `"m"` 操作数里；
- 模板里访问**全局变量/静态变量**；
- 模板里**调用函数**（`call`）—— 你不知道它碰了什么；
- 你需要在 asm 前后建立"内存访问的顺序"（自旋锁、发布数据、切换栈）。

### 2.6 什么时候不需要，以及怎么写得更窄

`"memory"` 是"我不知道具体碰了哪块"时的兜底，它的代价是全局的。
能精确描述就别用它：

```cpp
// 只读写 *p 指向的一个 int：用 "+m" 就够，不必 "memory"
__asm__ volatile("addl $1, %0" : "+m"(*p));
```

GCC 文档还给了一个更细的手法：同一个对象**同时**作为 `"r"(x)`（基址）和 `"m"(*x)`（内存）出现，
让别名分析知道"asm 碰的是这一块"。这在要复用基址寄存器时很常见。

判据：**`"memory"` 的作用域是"整个程序的内存"，而你想表达的通常只是"这一块"。**

### 2.7 性能代价：一条屏障能让循环折叠消失（实测）

```cpp
int g = 7;
int sum_none(int n)    { int s=0; for (int i=0;i<n;i++) s += g; return s; }
int sum_barrier(int n) { int s=0; for (int i=0;i<n;i++) { __asm__ volatile("" ::: "memory"); s += g; } return s; }
```

`-O2`：

```asm
sum_none:                       # 省略了 n <= 0 的分支
	movl	g(%rip), %eax
	imull	%edi, %eax          # 整个循环 = 一次乘法
	ret

sum_barrier:
.L7:
	addl	$1, %eax
	addl	g(%rip), %edx       # 每轮重新读 g
	cmpl	%eax, %edi
	jne	.L7
```

差距不是"多一两条指令"：这一条屏障直接杀掉了整个循环的折叠；同样的机制也会挡住
循环不变量外提和向量化。GCC 文档也明说：flush 寄存器有性能影响。

所以别把 `"memory"` 当 `volatile` 的升级版无脑贴。**每加一次，问一句：我到底在保护哪块内存？**

### 2.8 它不是 CPU 屏障（本课最容易混的一条）

GCC 文档紧接着写：

> Note that this clobber does not prevent the processor from doing speculative reads
> past the asm statement. To prevent that, you need processor-specific fence instructions.

两句话拆开：

| | `"memory"` | `mfence` / `lfence` / `sfence` |
|---|---|---|
| 作用对象 | **编译器** | **CPU** |
| 挡住什么 | 编译期重排、CSE、缓存到寄存器 | 乱序执行、store buffer 可见性 |
| 保证其他线程看到顺序 | ❌ | 部分（配合 atomic 才有完整语义） |
| 开销 | 优化机会 | 流水线 |

- `sfence`：之前的 store 对其他核可见之前，之后的 store 不能先可见；
- `lfence`：之后的 load 不能先于之前的 load 执行（也常用于 rdtsc 前后）；
- `mfence`：全序屏障。

以及：**`"memory"` 不解决数据竞争**。两个线程同时读写同一块内存，仍然是 C++ 的 UB，
要靠 `std::atomic` / 锁 / 平台原语。内联汇编不是并发工具箱。

### 2.9 惯用法：空 asm 当编译器屏障

```cpp
__asm__ volatile("" ::: "memory");     // 编译器屏障：不生成任何指令
```

它等于 `std::atomic_signal_fence(std::memory_order_seq_cst)`：
只阻止编译器重排，不生成 CPU 指令。**测性能时不要用它替代真 fence；
写无锁代码时也不要以为它够。**

---

## 3. 知识点 3：`"cc"` —— EFLAGS

### 3.1 EFLAGS 里有什么

跟优化器相关的主要六位：

| 位 | 名字 | 谁在读它 |
|---|---|---|
| 0 | CF 进位/借位 | 多精度算术、无符号比较 |
| 2 | PF 奇偶 | 很少 |
| 4 | AF 辅助进位 | BCD，基本不用 |
| 6 | ZF 零 | `==`、循环条件 |
| 7 | SF 符号 | 有符号比较 |
| 11 | OF 溢出 | 有符号比较、溢出检查 |

`"cc"` 指**整个条件码寄存器**，不是 carry —— 这是 v1 就纠正过的点，见
[`faq.md` Q1](faq.md#q1-and-又不是加法为什么会改标志位)。

### 3.2 哪些指令写 flags（按 Intel SDM 的 Flags Affected 栏）

**写（模板里出现就必须 `"cc"`）：**

| 指令 | 写哪些 |
|---|---|
| `add` `sub` `adc` `sbb` `neg` `cmp` | 全写 |
| `and` `or` `xor` `test` | OF=CF=0，SF/ZF/PF 按结果，AF 未定义 |
| `inc` `dec` | **CF 不变**，OF/SF/ZF/AF/PF 按结果 |
| `shl` `shr` `sar` | CF/OF/SF/ZF/PF，AF 未定义 |
| `rol` `ror` | **只写 CF 和 OF** |
| `imul`（2/3 操作数） | CF/OF，其余未定义 |
| `mul` | CF/OF，其余未定义 |
| `div` `idiv` | 全部未定义 |
| `bt` `bts` `btr` `btc` | CF（`bt` 的 ZF 不受影响） |
| `bsf` `bsr` | ZF，其余未定义 |

**不写（不用 `"cc"`）：**

`mov` `movzx` `movsx` `lea` `push` `pop` `bswap` `nop` `not` `cmovcc` `xchg`
`cpuid` `rdtsc` `rdtscp` `prefetch`；以及所有跳转/调用/返回（`jcc` 只**读** flags）。

最反直觉的两个：

- **`inc`/`dec` 不产生进位，照样必须写 `"cc"`** —— 它们改了 OF/SF/ZF。
- **`not` 不改 flags，`lea` 不改 flags，`bswap` 不改 flags** —— 这三条经常被误加 `"cc"`。

> 完整清单永远以 SDM 每条指令的 *Flags Affected* 栏为准。这张表只覆盖常用的。

### 3.3 漏写 `"cc"` 的机制

GCC 内部把条件码当成一个隐式寄存器。你声明 `"cc"`，等于告诉它：

> "这条 asm 之后，之前算好的 flags 全部作废。"

不声明，编译器就认为 **flags 的值跨过了这条 asm 仍然有效**。于是它有机会把
"asm 之前算的 `cmp`" 和 "asm 之后的 `jcc`/`cmov`/`setcc`" 配对 —— 而中间的 asm
已经把 flags 改掉了。这就是静默错码的路径。

### 3.4 一个必须诚实交代的实验结果

我拿 GCC 13.3 构造了十种形状（比较结果跨 asm 复用、循环条件、三元、连续 if、
do-while、`&&`、掩码比较……），**加不加 `"cc"` 生成的代码完全一样**。
现代 GCC 在这些形状里不会把 flags 的活跃区间拉过一条 volatile asm ——
它宁可把 `cmp` 重新发在 `jcc` 前面。

但这**不等于**它不会错。有学习者报告在 Lesson 1 的 `mask_imm_asm` 里踩过
（掩码 + 比较的场景），说明这条路径是可达的，只是依赖具体的分配/优化路径。
"我这儿没炸"只能证明"这次没被优化器抓到"，不能当安全证明。

### 3.4.1 怎么判断一个 bug 是不是 `"cc"` 的锅

真被 `"cc"` 坑到时，症状是**分支/比较走错**，不是算术结果错 ——
`and`/`add` 的结果寄存器写得没问题，错的只是 flags 被后面的 `jcc`/`setcc`/`cmov` 复用。

在 `-S` 里找 `/APP`（Linux 是 `#APP`）段：

- 段**前**有 `cmp`/`test`，段**后**紧跟 `jcc`/`setcc`/`cmov`，中间没有重新 `cmp`
  → 嫌疑成立（编译器认为 flags 跨过了 asm）；
- 段后重新发了 `cmp` → 这条路径没被利用。

反过来，如果现象是"`0x0F & 0x0F` 算出来是 `1` 而不是 `0x0F`"，那**不是 flags 的问题**：
`and` 的结果没错，先怀疑 `bool`/`!!` 转换、`return` 了错的变量、或 `printf` 的格式串/类型不匹配。

所以 `"cc"` 和 `&`、`"memory"` 不一样：**它的错误不容易当场复现。**
但这不代表可以漏：

1. 这是**契约**：你改了什么就声明什么。优化器今天没利用这条信息，不代表换版本、
   换优化选项、换架构之后也不利用。
2. flags 模型是**目标相关**的。x86 上是 EFLAGS，别的架构上是另一套（甚至拆成多条指令），
   "没炸"只是这一台机器上的偶然。

判断标准不变：**SDM 说它写 flags，就写 `"cc"`。** 这条没有灰色地带。

### 3.5 反过来：你没法让 C 代码消费 asm 里的 flags

一个常见误解是"我在 asm 里 `cmp`，然后 C 里 `if` 就能用"。
不行 —— 编译器会在 `if` 前面重新发一条 `cmp`。

真要拿 flags 当结果，用**标志输出操作数**（GCC 6+，x86 支持）：

```cpp
bool z;
__asm__("testq %1, %1" : "=@ccz"(z) : "r"(x));   // ZF → z
```

`-S` 里 GCC 会在 asm 之后自己补 `sete %al`。这比在模板里手写 `setcc` 更干净，
因为它让优化器知道"这是一个布尔值"，而不是一段黑盒。

### 3.6 过度声明的代价

`"cc"` 声明多了会阻止编译器复用 flags，理论上多一条 `cmp`。
但实测（同上四种形状）GCC 13 很难被影响 —— 所以这一项**别为了省指令去赌**，
写全。

---

## 4. 知识点 4：`rdtsc` —— 把"以为要写"和"实际要写"摆在一起

### 4.1 它做什么

`rdtsc` 把时间戳计数器读进 **EDX:EAX**（高 32 位在 EDX，低 32 位在 EAX）。

x86-64 上有个顺带效果：**写 32 位寄存器会把高 32 位清零**。所以 `rdtsc` 之后
`RAX = 低 32 位`、`RDX = 高 32 位`，两者的高半部分都是 0。

### 4.2 它到底 clobber 什么

| 资源 | 改了吗 |
|---|---|
| `%rax` / `%eax` | ✅ 低 32 位时间戳 |
| `%rdx` / `%edx` | ✅ 高 32 位时间戳 |
| EFLAGS | **❌ 不改**（SDM：Flags Affected = None） |
| 内存 | ❌ |

所以 `rdtsc` 只需要两个输出操作数，**不需要 `"cc"`**。
v1 教案里"rdtsc 写 EFLAGS"是错的，这一版改正。

### 4.3 约束怎么写

正确形状：**两个 32 位输出**，分别绑 `a` 和 `d`，再拼起来。

```cpp
// 形状（不是完整实现）：两个 unsigned 输出
__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
// 返回值 = ((uint64_t)hi << 32) | lo
```

`-S`：

```asm
#APP
	rdtsc
#NO_APP
	salq	$32, %rdx
	movl	%eax, %eax
	orq	%rdx, %rax
```

### 4.4 坑一：`"A"` 在 x86-64 上不成立

`"A"` 在 **i386** 上表示 `edx:eax` 这一对。x86-64 上没有这个"对"的寄存器类，
GCC 文档明确写了：

> This is not correct on x86-64 as it would allocate tick in either ax or dx.

实测（GCC 13.3）：

```cpp
uint64_t t; __asm__ volatile("rdtsc" : "=A"(t));
```

```asm
#APP
	rdtsc
#NO_APP
	ret                 # 只返回 %rax
```

运行：

```
"=A":      000000003460c1c7   高 32 位 = 0
"=a"/"=d": 000c9d4b3460ce30   高 32 位 = 826699
```

高 32 位永远是 0 —— 因为 `rdtsc` 写 `%eax` 时把 `%rax` 的高半清零了，
而 `"=A"` 只给了你 `%rax` 一个寄存器。**时间戳的上半截直接丢了，还不报错。**

### 4.5 坑二：`"=a"(uint64_t)` 也只拿到 32 位

即使你写 `"=a"(t)` 且 `t` 是 `uint64_t`，`rdtsc` 只写了 `%eax`，
`%rax` 的高 32 位是被"清零规则"顺带弄成 0 的，不是时间戳的高位。
**必须拆成两个 32 位输出再拼。**

### 4.6 坑三：`rdtsc` 不是序列化指令

`rdtsc` 允许乱序执行：它可能在你之前的一堆指令**执行完之前**就读了计数器，
也可能在之后才读。所以：

- 测一段代码的耗时，要在前后加 `lfence`（或直接 `rdtscp`）；
- `rdtscp` 会等前面的指令执行完（但仍不是完整序列化），并且多写一个 `%rcx`；
- 跨核计时还要考虑 TSC 同步（`constant_tsc` / `nonstop_tsc`）。

这一条和 §2.8 是同一个主题的两次现身：**编译器屏障 ≠ CPU 屏障。**

### 4.7 如果把它当作业，判分看什么

- 两个输出是否分别绑到 `a` / `d`；
- 是否**没有**乱加 `"cc"`（不是扣分重点，但说明你懂）；
- 是否没有把 `"A"` 用在 x86-64 上；
- 拼接是否用 `uint64_t` 移位，而不是 32 位相加；
- `-S` 里有没有多余的 `mov`。

---

## 5. 知识点 5：`"redzone"` 和 x86 的现实

GCC 文档里还有第三种特殊 clobber：`"redzone"`。它的定义是：

> 告诉编译器，asm 可能写到栈指针以下的红区（red zone）—— 某些 ABI 保证
> 那块区域不会被信号处理/中断改写，叶子函数会把临时值放在那里。

什么时候要写：**asm 里 `push` 或 `call`，却没有先 `subq` 让开红区。**
因为 `push`/`call` 会写 `rsp` 以下的空间，正好踩到编译器藏在那里的临时值。

但有个现实问题：**x86 的 GCC 13 不认这个名字。** 实测：

```
error: unknown register name 'redzone' in 'asm'
error: unknown register name 'dirflag' in 'asm'
```

所以 x86 上不能靠 `"redzone"` 兜底，正确做法是：

- 模板里要碰栈，先 `subq $N, %rsp`，结束时 `addq $N, %rsp`；
- 或者干脆**不在 extended asm 里 pop/push/call**（下一课会讲为什么这条几乎等价于
  "context switch 不能用 extended asm 写"）。

顺带：SysV 有 128 字节红区，**MS x64 没有红区**。你的 MinGW 环境下，
"往 rsp 以下写"连 ABI 都不保证，更要自己让开。

---

## 6. 知识点 6：clobber 与 ABI（为 context switch 铺路）

### 6.1 两类寄存器

| | SysV（Linux） | MS x64（Windows/MinGW） |
|---|---|---|
| caller-saved | `rax rcx rdx rsi rdi r8–r11` | `rax rcx rdx r8–r11` |
| callee-saved | `rbx rbp r12–r15` | `rbx rbp rdi rsi r12–r15`，还有 `xmm6–xmm15` |

"caller-saved"的意思是：**被调用者可以随便写，不需要恢复**。
所以你的 asm 里如果有一条 `call`，被调用函数可以合法地改掉所有这些寄存器。

### 6.2 `call` 的灾难（实测）

```cpp
extern "C" uint64_t side_effect(uint64_t);   // 返回 42
uint64_t bad_call(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    __asm__ volatile("call side_effect" : : "D"(s));   // 漏 clobber
    return s;
}
```

```
bad_call(3,4) = 42      ← 期望 7
```

`-S` 里 GCC 把 `s` 放在 `%rax` 上穿过 `call` —— 它认为 `%rax` 没被动过。
`call` 之后 `%rax` 是 `side_effect` 的返回值。

正确做法：把 caller-saved 全列上，再加 `"cc"` 和 `"memory"`：

```cpp
// Linux/SysV
: "rax","rcx","rdx","rsi","rdi","r8","r9","r10","r11","cc","memory"
// Windows/MS x64：rsi/rdi 是 callee-saved，去掉；有浮点值在活就加 "xmm0"–"xmm5"
```

x86 还有个现成的约束字符 **`"U"`** = call-clobbered 整数寄存器。
但显式列全更清楚，也方便跨平台。

> 这段在 Linux/SysV 上测。MinGW 第一个参数是 `%rcx`，要复现把 `"D"` 换成 `"c"`。

### 6.3 callee-saved 的代价

模板里硬编码了 `%rbx`，就写 `"rbx"`：GCC 给你 `pushq`/`popq`（§1.3）。
不写：GCC 可能让某个变量住那儿，被你的 asm 擦掉。

### 6.4 `rsp` / `rbp` 特例

- `"rbp"` → `error: bp cannot be used in 'asm' here`
- `"rsp"` → `warning: listing the stack pointer register 'rsp' in a clobber list is deprecated`
- GCC 文档：asm 前后 `rsp` 必须相同

这就是为什么**切栈必须用 `naked` 函数/整函数汇编**，不能用 extended asm。

### 6.5 一张预告表：Lesson 6 要保存什么

写 context switch 时，你要保存/恢复的正是"从旧协程的视角看，可能被改掉的一切"：

| 类 | 内容 |
|---|---|
| callee-saved GPR | `rbx rbp r12–r15`（+ Windows 的 `rsi rdi`） |
| 栈指针 | `rsp` |
| 返回地址 | 栈上的 `ret` 地址 / 或显式存 `rip` |
| flags | 至少 `pushfq` |
| SIMD | 看约定，Windows 还要 `xmm6–15` |

这份表和 clobber 表是同一张表的两面：**clobber 是你对编译器承诺"我动了什么"；
context switch 是你对另一个执行流承诺"我给你恢复什么"。**

---

## 7. 怎么"看见"这些问题（证据学）

### 7.1 三个问题，一张 `-S`

看任何一段 asm 的 `-S`，问三句：

1. **asm 前后的内存访问顺序变了吗？**（`"memory"` 管这个）
2. **某个值跨过 asm 还活在一个寄存器里吗？**（寄存器 clobber 管这个）
3. **`cmp`/`test` 和它的消费者之间隔着 asm 吗？**（`"cc"` 管这个）

### 7.2 让编译器"有动机"做坏事

编译器不会无缘无故重排。要让它出手，得给它动机：

- 同一个地址读两次 → 它想 CSE（§2.3）；
- 循环里的全局量不变 → 它想折叠（§2.7）；
- 比较结果后面还要用 → 它想复用 flags（§3.4）。

**复现不了，往往是因为你没给它动机，不是因为它不会。**

### 7.3 `-O0` 是对的，`-O1` 就错（实测）

同一份 `bad_store`：

| 优化级别 | 结果 |
|---|---|
| `-O0` | 100（"看起来对"） |
| `-O1` | 198（错） |
| `-O2` | 198（错） |

这是"为什么看起来是对的"最直接的答案：**低优化级别下编译器不做缓存和重排，
你的错误被掩盖了。** 而 `-O2` 才是这门课的唯一裁判。

### 7.4 一个反直觉的对照：`volatile` 指针反而"救"了它

把参数改成 `volatile int*`，`bad_store` 在 `-O2` 下又变回 100 ——
因为两次读都是 volatile，编译器不能合并、也不敢随便搬。

**但这不能当解法**：`volatile` 是另一套契约（对 MMIO/信号处理有意义），
它不保证跨 asm 的顺序，也不解决多线程。它只是恰好让这个例子不复现。

---

## 8. 本课任务（等你有电脑再开始）

> 现在不用做。开讲后按顺序来，一次只交一个。

### 任务 A：`rdtsc_asm`

- 用内联汇编读 TSC，返回 `uint64_t`；
- 不许用 `__builtin_ia32_rdtsc` 之类的内建；
- 提交：源码 + `-O2 -Wall -Wextra` 编译输出 + 运行输出 + `-S` 片段。

验收：高 32 位不是常数 0；`-S` 里没有多余搬运；约束与 §4 一致。

### 任务 B：`bad_store` / `good_store`

- `void bad_store(int* p, int v)`：**故意漏 `"memory"`**，asm 里写 `*p = v`；
- `void good_store(int* p, int v)`：补上；
- 调用前后读 `*p`，在 `-O2` 下让差异可见；
- 提交两者的 `-S` 和运行结果。

验收：能指出**哪一行 load 被合并/搬走了**，不是只说"结果不对"。

### 任务 C：`"memory"` 的性能代价

- 构造一个循环，分别在"有屏障 / 无屏障"下编译；
- 用 `-S` 说明优化被挡住了哪一步（折叠？外提？向量化？）；
- 数指令条数，给出量化差异。

验收：能说清"为什么这里必须付这个代价"或"为什么可以不付"。

### 任务 D：问答题（不写代码）

1. 为什么 `bad_store` 在 `-O0` 下是对的、`-O2` 下是错的？"看起来对"为什么不算证据？
2. 为什么 `"memory"` 不能替代 `mfence`？举一个两者行为不同的场景。
3. `rdtsc` 要不要 `"cc"`？给出 SDM 依据。
4. 你的 `asm` 里 `call` 了一个函数，clobber 至少写哪些？漏了会怎样？

---

## 9. 本课评分口径

| 项 | 权重 | 本课看什么 |
|---|---|---|
| 正确性 | 40% | rdtsc 高低位拼接；store 可见性；边界值 |
| ABI 安全性 | 30% | clobber 完整且不过度；没碰 `rsp`/`rbp`；callee-saved 有代价意识 |
| 性能 | 30% | `"memory"` 范围最小；无冗余搬运；不把 `volatile` 当屏障 |

≥ 80 通过。评语必须落到具体指令/寄存器，不接受"更好/更清晰"。

---

## 10. 预判易错点

1. 把 `"memory"` 当 `mfence` 用（§2.8）。
2. 以为 `asm volatile` 自带 `"memory"`（§1.5、§2.3）。
3. `and`"又不是加法"所以不写 `"cc"`（§3.2）。
4. `inc`/`dec` 不写 `"cc"`（§3.2）。
5. `bswap`/`lea`/`not` 多写 `"cc"`（不致命，但说明没查 SDM）。
6. `rdtsc` 写 `"cc"`（§4.2）。
7. `rdtsc` 用 `"=A"`（§4.4）。
8. `rdtsc` 用 `"=a"(uint64_t)` 以为拿到 64 位（§4.5）。
9. clobber 列表写 C 变量名（直接编译错误）。
10. 把 `%n` 操作数的寄存器重复写进 clobber（§1.4）。
11. 在 extended asm 里 `push`/`call` 不 `sub rsp`（§5）。
12. 以为 `"memory"` 能保证别的线程看到顺序（§2.8）。
13. 用 `-O0` 的结果判断对错（§7.3）。
14. 用 `volatile int*` 掩盖漏写 `"memory"`（§7.4）。

---

## 11. 自测（先别对答案，开讲时问）

1. clobber 是 def 还是 use？这个区别在寄存器分配上意味着什么？
2. 一条 `asm volatile` 里只写了 `movl $1, (%rdi)`，为什么它可能是错的？
3. `"memory"` 让编译器做的三件事分别是什么？
4. 什么时候可以用 `"+m"` 替代 `"memory"`？省下了什么？
5. `"memory"` 和 `mfence` 各挡什么？为什么两个都要？
6. `inc` 为什么不改 CF 却仍要 `"cc"`？
7. 为什么 `not`/`bswap`/`lea` 不需要 `"cc"`？
8. `rdtsc` 改哪两个寄存器？改不改 flags？依据是什么？
9. `"=A"` 在 x86-64 上为什么错？错的时候会报错吗？
10. 为什么切栈不能用 extended asm 写？

---

## 12. 和后续课程的关系

| 课 | 本课留下的东西 |
|---|---|
| Lesson 3 栈与寄存器保存 | §1.3 的 push/pop、§6.5 的保存清单 |
| Lesson 4 early clobber | [`lesson01-deep-dive.md` §2](lesson01-deep-dive.md) 的 `&`，本课 §6.2 的 call |
| Lesson 5 调用约定 | §6 的两套 ABI |
| Lesson 6 栈切换 | §2.5 的顺序、§5 的 rsp、§6.5 的集合 |
| Lesson 8 最小调度器 | 全部 |

---

## 13. 遗留账（Lesson 1）

- `mask_imm_asm` 的 `main` 里仍没有独立参考值比对，验证靠肉眼。
- `#ifdef ERROR0` 的失败实验引用未声明的 `m`，打开只会撞 undeclared identifier。
- ~~`rotl64_asm` 把 `"c"` 换成 `"r"` 会怎样~~ —— 已由
  [`lesson01-deep-dive.md` §4](lesson01-deep-dive.md) 结清（`rolq %sil, %rax`，
  汇编器报 `operand type mismatch`）。

---

## 附：v1 → v2 改了什么

| 位置 | v1 | v2 |
|---|---|---|
| 知识点 2/3/4 | 只有一行标题 | 各成一节，带实测 |
| `rdtsc` 与 flags | 说"写 EFLAGS"（错） | 改正：不改 flags，不需要 `"cc"` |
| `"memory"` | 一句"编译器屏障" | 三件事 + 反例 + 性能 + 与 CPU 屏障的分界 |
| `"cc"` | 指向 Lesson 1 | 指令表 + 机制 + 实测复现不了的诚实说明 |
| `"redzone"` | 未提 | 新增；并说明 x86 GCC 不认 |
| 任务 | 4 条一句话 | 每项带验收标准 |
| 评分 | 无 | 本课专项 |
