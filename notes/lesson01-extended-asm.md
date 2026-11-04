# Lesson 1 — Extended Asm 基本语法与约束符

> 环境：g++ (MinGW-w64) `-std=gnu++17 -O2 -Wall -Wextra`，默认 AT&T 语法。
> 作业文件：[`lessons/lesson01/main.cpp`](../lessons/lesson01/main.cpp)

---

## 0. 四件套

```cpp
__asm__ volatile(
    "模板串"        // 原样送给汇编器，只做 %n 替换
    : 输出列表       // output
    : 输入列表       // input
    : clobber 列表   // 副作用声明
);
```

- `asm` 在 `-std=gnu++17` 下是关键字，`__asm__` 是它的等价拼写。
- `volatile`：不许因为"没用到输出"就删掉整块，不许把它搬来搬去。**它不保证与其他内存操作的顺序**——那是 `"memory"` 的事（Lesson 2）。
- 后两个冒号段可以省略。

编译器只做两件事：**按约束给操作数分配位置**，**把 `%n` 按位置替换成字节串**。它不搬运数据，不检查你用了没有，也不知道你改了谁。

---

## 1. 占位符编号

```cpp
__asm__ volatile("movq %[src], %[dst]"
                 : [dst] "=r"(lo)
                 : [src] "r"(a));
```

- `%0 … %(O-1)` 按**输出列表顺序**，接着 `%(O) …` 按**输入列表顺序**。
- **clobber 不参与编号。** `"cc"`、`"memory"`、`"rbx"` 都不占号，加了它们不用重新数格子。
- 命名操作数 `%[name]` 底层规则一样，只是免去人肉计数。

`%0` 展开成什么由约束符决定，不是"一个寄存器"：

| 约束 | 展开成 |
|---|---|
| `"r"` | `%rax` / `%r10`（带 `%`） |
| `"m"` | `-8(%rbp)` 这种**内存操作数** |
| `"i"` | `$255`（**自带 `$`**，模板里再写 `$%1` 会变成 `$$255`） |

所以：不要给占位符自己加 `%`；`%%` 只用于硬编码寄存器（`%%rsp`）；两个 `"m"` 操作数之间直接 `mov` 是 mem-to-mem，汇编器拒绝。

---

## 2. 约束符清单

| 约束 | 含义 | 备注 |
|---|---|---|
| `"r"` | 通用寄存器 | 分配器挑具体是哪个 |
| `"m"` | 内存操作数 | 不能当寄存器用 |
| `"i"` | 编译期立即数 | 运行期变量 → `impossible constraint in 'asm'` |
| `"n"` | 已知整数常量 | 与 `"i"` 类似，语义更窄 |
| `"g"` | 通用（r / m / i 皆可） | 让编译器自由选 |
| `"=r"` | **只写**输出 | 允许与某个输入复用同一寄存器 |
| `"+r"` | 读改写 | 输入输出绑成同一个操作数，省掉显式 `mov` |
| `"=&r"` | early clobber | 输出不许和任何输入同址（Lesson 4） |
| `"0"` | 与第 0 个操作数同址 | 手工做 `"+r"` 的另一种写法 |
| `"a"`/`"b"`/`"c"`/`"d"` | 固定 `%rax`/`%rbx`/`%rcx`/`%rdx` | 用于 `rdtsc`、`cpuid`、移位计数这类 ISA 硬限制 |

---

## 3. 铁律：模板里写出来的才算数

第一次提交的写法：

```cpp
__asm__ volatile("bswap %0" : "=r"(r) : "r"(x));
```

`-S`：

```asm
	movabsq	$72623859790382856, %rbx     # 输入常量落在 %rbx
/APP
	bswap %rdx                           # 但输出被分到 %rdx —— 里面是垃圾
/NO_APP
```

原因：模板里**没有出现 `%1`**，GCC 就没有任何理由发出搬运指令。约束符保证的是"这个值在某个寄存器里"，**不保证那个寄存器是你的输出寄存器**。

修法只有两种思路：

```cpp
// A：模板里显式搬运
__asm__ volatile("movq %1, %0\n\tbswap %0" : "=r"(r) : "r"(x));

// B：让输入输出共用同一个操作数
__asm__ volatile("bswap %0" : "+r"(x));
return x;
```

B 的 `-S` 比 A 少一条指令：

```asm
	movabsq	$72623859790382856, %rdx
/APP
	bswap %rdx
/NO_APP
```

`"=r"` 允许 GCC 把输出和某个输入分到同一寄存器，但**不保证**；一旦分到同一寄存器，A 里那句 `movq %1, %0` 就退化成 `movq %rbx, %rbx` 自拷贝——白占一个解码槽。

---

## 4. AT&T 与 Intel

GAS 默认吃 AT&T；`-masm=intel` 才切 Intel。

| 项目 | AT&T | Intel |
|---|---|---|
| 方向 | `src, dst` | `dst, src` |
| 寄存器 | `%rax` | `rax` |
| 立即数 | `$255` | `255` |
| 内存 | `8(%rbp)`、`(%rax,%rbx,8)` | `[rbp+8]`、`[rax+rbx*8]` |
| 裸数字 | **内存地址** | **立即数** |
| 大小后缀 | `movb/movw/movl/movq` | 靠寄存器或 `BYTE PTR` |
| 间接跳转 | `call *%rax` | `call rax` |

```asm
	movq  8(%rbp), %rax          # rax = [rbp+8]
	leaq  8(%rdi,%rsi,2), %rax   # rax = rdi + rsi*2 + 8（算地址，不访存）
	leaq  .LC0(%rip), %rax       # PIC 下取符号地址的标准姿势
```

**排序陷阱**：AT&T 里按 Intel 顺序写 `movq %rax, %rbx`，汇编器照编不误，只是数据流反了。`rol` 是重灾区——`rolq %cl, %rax` 的计数在前。

**`-masm=intel` 的陷阱**：模板串是原样送汇编器的，换语法后你手写的 `rolq %b2, %0` 会被当成 Intel 顺序解释，静默错码。本项目一律 AT&T。

---

## 5. 操作数修饰符

同一个操作数可以取不同宽度的寄存器名：

| 修饰符 | 展开 | 用途 |
|---|---|---|
| `%b1` | `%cl` | `rol`/`shl` 的计数 |
| `%w1` | `%cx` | 16 位 |
| `%k1` | `%ecx` | 32 位 |
| `%q1` | `%rcx` | 64 位 |

`rotl64_asm` 的写法：`unsigned n` 的 `%1` 展开是 `%ecx`，而 `rol` 只接受 `imm8` 或 `%cl`，所以必须用 `%b1`。

---

## 6. 类型 → 模式 → 寄存器名 → 后缀

```cpp
__asm__ volatile("andq %1, %0" : "=r"(out) : "r"(0xFFu));   // 编译失败
```

```
0xFFu  →  unsigned int  →  SImode（32 位）  →  %eax
       →  andq %eax, %rdx  →  incorrect register '%eax' used with 'q' suffix
```

改成 `0xFFull` → `DImode` → `%rax` 即可。**GCC 按 C 类型分配寄存器，后缀是你手写的，两者必须对齐。**

---

## 7. clobber：承诺 + 销毁

clobber 项被视为一条 asm 的 **def**。GCC 会：

1. 把该寄存器从这条 asm 附近的分配候选里剔除；有活值住在里面就先搬走（spill）。
2. 认定 asm 之后该寄存器的值已死。

所以漏写 = 告诉编译器"我没碰它"，于是它让某个变量继续住在那儿 —— 然后被你的 asm 擦掉。

- `"cc"`：任何写 EFLAGS 的指令。见 [faq.md](faq.md#q1-and-又不是加法为什么会改标志位)。
- `"memory"`：asm 读写的内存不在操作数列表里（Lesson 2）。
- 寄存器名：**只在模板里硬编码了**才写（`%%rsp`）。走了 `%0`/`%1` 的操作数由分配器管，**不要重复声明**。
- 漏写 `"rbx"` 的代价：GCC 把循环变量/累加器放在 `%rbx`，你的 `movq $0, %%rbx` 把它清零，`-O2` 下循环直接跑飞或提前退出。

---

## 8. 立即数的三档编码

| 形态 | 编码 | 长度 | 说明 |
|---|---|---|---|
| `and r/m64, imm8` | `48 83 /4 ib` | 4 | `ib` **符号扩展**到 64 位 |
| `and r/m64, imm32` | `48 81 /4 id` | 7 | `id` **符号扩展**到 64 位 |
| `and r/m64, r64` | `48 21 /r` | 3 | 立即数得先 `mov` 进寄存器 |

`$0xFF`（=255，正数）**不能**走 imm8——imm8 里的 `0xFF` 符号扩展成 `0xFFFF_FFFF_FFFF_FFFF`，就变成空操作了。所以 GAS 给它选 imm32，一条 7 字节指令。`$0x7F` 才够格走 imm8（4 字节）。

想要短编码又要把低字节清零，x86 的正解不是 `and`，是**零扩展搬移**：`movzbl %dl, %edx`（3 字节）。

`"i"` vs `"r"` 的实际对照：

```asm
	and $255, %rdx              # 一条
	movl	$255, %eax          # 两条 + 多占一个寄存器
	and	%rax, %rdx
```

---

## 9. 三个版本的 `-S` 对照

| 版本 | 写法 | `-S` 关键行 | 问题 |
|---|---|---|---|
| v1 | `"bswap %0" : "=r"(r) : "r"(x)` | `bswap %rdx`，输入在 `%rbx` | 模板没引用 `%1`，结果是垃圾 |
| v2 | 加 `movq %1, %0`，mask 两段都改 `%1` | `movq %rbx,%rdx` + `bswap %rdx` | 改写了 `"r"` 输入寄存器；mask `return x` 而不是 `r` |
| v3 | `"+r"(x)`，`rolq %b1`，对照 `"i"`/`"r"` | `bswap %rdx` 一条直达 | 两段 `and` 漏 `"cc"` |
| v4 | 补 `"cc"` | 不变，clobber 生效 | 通过，88 分 |

---

## 10. 自查清单

1. 模板里出现的每个 `%n`，都能说清它来自哪个列表的哪个位置？
2. 每一条会写 EFLAGS 的指令，`"cc"` 写了吗？
3. 硬编码的寄存器（`%%xxx`）都进 clobber 了吗？走 `%n` 的有没有被重复声明？
4. 模板里的后缀，和操作数的 C 类型对得上吗？
5. `movq %1, %0` 这种搬运，能不能用 `"+r"` 让编译器直接绑死？
6. `-S` 里每一条寄存器搬运，是模板来的，还是分配器插的？

---

## 附：Lesson 1 作业（最终版）

```cpp
#include <cstdint>
#include <cstdio>

static inline uint64_t bswap64_asm(uint64_t x) {
    __asm__ volatile("bswap %0" : "+r"(x));
    return x;
}

static inline uint64_t rotl64_asm(uint64_t a, unsigned n) {
    __asm__ volatile("rolq %b1, %0" : "+r"(a) : "c"(n) : "cc");
    return a;
}

static inline uint64_t mask_imm_asm(uint64_t x) {
    __asm__ volatile("and %1, %0" : "+r"(x) : "i"(0xFFu) : "cc");   // 立即数编进指令
    __asm__ volatile("and %1, %0" : "+r"(x) : "r"(0xFFull) : "cc"); // 常量先物化进 GPR
    return x;
}
```

`-O2` 下三行的实际出口：

```asm
	movabsq	$72623859790382856, %rdx
	bswap %rdx                    # 零冗余
	rolq  %cl, %rdx               # 零冗余
	and   $255, %rdx              # 7 字节 imm32
	movl  $255, %eax
	and   %rax, %rdx              # 对照用的 "r" 版本
```
