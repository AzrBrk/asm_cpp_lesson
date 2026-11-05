# Lesson 1 补讲 — 约束系统的空白页

> 定位：[`lesson01-extended-asm.md`](lesson01-extended-asm.md) 解决的是"能写对"。
> 这份补的是它一笔带过、但你写 context switch 时一定会撞上的东西：
> 约束串的完整文法、`&`、修饰符全集、宽度模型、立即数编码、`"m"` 的真面目、ABI 层。
>
> **证据环境**：x86-64 GCC 13.3（Ubuntu，SysV）+ MinGW-w64 GCC 13.2（PE，MS x64）。
> 原有 `-S` 片段、报错与运行值为该工具链的历史记录；新增勘误不是本机重跑声明。
> Windows 目标历史只给 `-S`，未提供运行证据。
> 你的环境是 MinGW：**约束语义两边完全一致**，寄存器选择和序言不同，差异处单独标注。

---

## 0. 先记住错误分三层

`asm` 出错按报出来的时间分三层，越晚越贵：

| 层 | 谁报 | 典型信息 | 触发条件 |
|---|---|---|---|
| C++ 前端 | GCC | `impossible constraint in 'asm'` | `"i"` 收到运行期变量；`"K"(0xFF)` |
| | GCC | `unknown register name 'x' in 'asm'` | clobber 写成字符串但不是寄存器名（变量名、拼错） |
| | GCC | `bp cannot be used in 'asm' here` | clobber 了 `"rbp"` |
| 汇编器 | as | `junk 'addq %rsi' after register` | 模板里两条指令之间没有分隔符 |
| | as | `incorrect register '%eax' used with 'q' suffix` | C 类型和模板后缀不一致 |
| | as | `operand type mismatch for 'rol'` | `%b1` 拿到的不是 `%cl` |
| | as | `can't encode register '%ah' ... requiring REX` | 高字节寄存器撞上 r8–r15 |
| **运行期** | 没人 | **静默错值** | 漏 `&`、漏 caller-saved clobber、模板没引用输入 |

前两层编译器拦你。第三层不会 —— 它只在你的测试没盖住的地方炸。
下面每一节，最后都落到这张表的某一行。

---

## 1. 约束串的完整文法

一个约束串 = **修饰符** + **一个或多个候选约束字符**：

```
"=rm"     "=&r"     "+r"     "0"     "g"     "=m"
```

输出的 `=` / `+` 放在开头；`&` 等修饰符跟在其后（例如 `"=&r"`），
不是所有修饰符都只能在第一个字符。含义（GCC 文档 *Constraint Modifier Characters*）：

| 修饰符 | 含义 | 备注 |
|---|---|---|
| `=` | 只写 | 旧值丢弃，输出必须真的被写 |
| `+` | 读改写 | **算两个操作数**，30 个操作数上限里占两位 |
| `&` | early clobber | 输出在输入读完之前就被写，见 §2 |
| `%` | 可交换 | 编译器可以把这一对操作数对调以省一条 `mov`；只读操作数才能用 |

### 数字匹配约束 `"0"`…`"9"`

`"0"` 表示"我和第 0 个操作数放同一个位置"。`"+r"` 就是 `"=r"` + 绑定的语法糖，
多输出时也能用 `+`；数字匹配适合输入和输出使用不同 C 对象的情形。
`rdtsc` 没有输入，不需要匹配。

```cpp
// 输出 %0 与输入 a 强制同址
__asm__("addq %2, %0" : "=r"(r) : "0"(a), "r"(b));
```

实测 `-S`（Linux GCC 13.3）：

```asm
	addq %rsi, %rax        # a 直接在 %rax，没有 mov；%rsi = b
```

`"=r"(r) : "0"(a)` 和 `"+r"(a)` 都表达同址读写，具体代码还取决于调用上下文。
有第二个输出也可以使用 `+`，并不存在“只能用数字匹配”的限制。

### 候选约束

`"rm"`、`"g"`、`"ri"` 是"这几类都行，分配器挑最便宜的"。三个后果：

1. 模板必须能吃下**每一种**候选形态。`"g"` 可能给你寄存器、内存，**也可能给你立即数**：
   ```cpp
   __asm__ volatile("bswap %1" : "=r"(r) : "g"(0x7F));
   // -S:  bswap $127
   // as:  Error: operand size mismatch for `bswap'
   ```
2. 候选顺序不是优先级承诺，别依赖。
3. 拿不准就用最窄的（`"r"`），让报错提前到 C++ 前端。

---

## 2. `&`：Lesson 1 里最该补的一个字符

**语义**：这个输出在某些输入读完之前就可能被写，所以不能与未绑定的输入
（包括内存操作数的地址寄存器）重叠。不一定是第一条指令；绑定到自身的输入例外。

### 反例（实测会错，不是"理论上有风险"）

```cpp
__attribute__((noinline)) uint64_t no_amp(uint64_t x) {
    uint64_t r;
    __asm__ volatile("movq $0, %0\n\taddq %1, %0" : "=r"(r) : "r"(x + 1) : "cc"); // 故意只漏 &
    return r;
}
```

`-S`（Linux GCC 13.3，`-O2`）：

```asm
	movq $0, %rax          # %0
	addq %rax, %rax        # 输入也落在 %rax —— 已被清零
```

运行：

```
no_amp(10)   = 0      ← 期望 11
with_amp(10) = 11     ← 只把 "=r" 换成 "=&r"
```

加上 `&` 之后 `-S`：

```asm
	movq $0, %rax
	addq %rdi, %rax        # 输入被挪到 %rdi
```

### 为什么 `"+r"` 挡不住

`+r` 是"**我**读写同一个操作数"，它只约束你自己。
如果还有第二个输入，那个输入照样可能和输出同址 —— `+` 对它没有任何约束力。
模板里只要出现"先写输出、后读输入"，就是 `&` 的场景。

### GCC 文档的原话

> Use the `&` constraint modifier on all output operands that must not overlap an input.
> Otherwise, GCC may allocate the output operand in the same register as an unrelated input operand,
> on the assumption that the assembler code consumes its inputs before producing outputs.
> This assumption may be false if the assembler code actually consists of more than one instruction.

多输出 + 内存输出也会踩：寄存器输出的地址寄存器可能和 `"m"` 输出共享，
写 `a` 会改掉 `b` 的地址（文档同段）。规则一样：`&`。

### 它不是"每次都炸"

同一份代码在 MinGW GCC 13.2 上，分配器把输入放 `%rcx`、输出放 `%rax`，`-S` 里没重叠、恰好是对的：

```asm
h:
	imulq	%rdx, %rcx
/APP
	movq $0, %rax
	addq %rcx, %rax        # 恰好没错
```

这说明错误被某种分配掩盖，不能用“本机没错”证明接口正确。
规则：对每个输出检查其第一次写入是否早于其他未绑定输入的最后读取；
必要时加 `&`。额外限制可能增加寄存器压力，不能保证代价“最多一个寄存器”。

---

## 3. 操作数修饰符全集（不止 `b/w/k/q`）

修饰符只决定**操作数怎么印出来**，不搬运任何数据。

```cpp
// 同一个 uint64_t 操作数，四种印法（实测 Linux GCC 13.3）
"%b0" → %dil     "%w0" → %di     "%k0" → %edi     "%q0" → %rdi
```

完整表（x86，GCC 文档 *x86 Operand Modifiers*）：

| 修饰符 | 印出 | 用途 / 陷阱 |
|---|---|---|
| `%b0` | QImode 名（`%dil`） | 字节操作；**只是名字，不是"搬进低字节"** |
| `%h0` | 高字节名（`%ah`） | 只有 `%ah/%bh/%ch/%dh` 存在；配 r8–r15 直接汇编报错 |
| `%w0` | HImode 名（`%di`） | 16 位 |
| `%k0` | SImode 名（`%edi`） | 32 位 |
| `%q0` | DImode 名（`%rdi`） | 64 位 |
| `%z0` | **opcode 后缀** `b/w/l/q` | **不是寄存器名！** 实测 `movl %z0` → `movl q` |
| `%c0` | 不带 `$` 的常量 | `"i"` 常量想自己控制语法时用 |
| `%p0` / `%P0` | 裸符号 / 带 PLT 的符号 | 取函数地址、写跳板 |
| `%l0` | 标签名 | `asm goto` 的 label 操作数 |
| `%=` | 本次展开的唯一编号 | 局部标签，防重复展开撞名（§5） |
| `%%` | 字面 `%` | 硬编码寄存器 `%%rax` |

两个最常见误解：

1. **`%z` 不是"32 位寄存器"**。它印出来的是一个字母，拼在助记符后面用（实测）：
   ```cpp
   // 这里只展示模板文本替换，不是推荐的普通 extended asm 栈操作
   __asm__ volatile("push%z0 %0\n\tpop%z0 %0" : : "r"(x));
   // uint64_t → pushq %rdi / popq %rdi
   // uint32_t → pushl %edi / popl %edi
   ```
   想要 32 位寄存器名用 `%k`。64 位模式没有普通 `pushl %edi` 编码；
   `-S` 能印出文本不代表随后 `-c` 能汇编成功。
2. **`%h` 不是"高 8 位通用"**。x86-64 里只有四个高字节寄存器，且**任何需要 REX 前缀的指令都不能用**：
   ```
   movb %ah, %r8b
   Error: can't encode register '%ah' in an instruction requiring REX/REX2 prefix
   ```

---

## 4. 宽度模型：`%b1` 为什么有时是 `%cl`，有时是 `%sil`

### 寄存器别名表

```
64:  rax rbx rcx rdx rsi rdi rbp rsp  r8  r9  r10 r11 r12 r13 r14 r15
32:  eax ebx ecx edx esi edi ebp esp  r8d r9d ...            r15d
16:  ax  bx  cx  dx  si  di  bp  sp   r8w ...                r15w
 8:  al  bl  cl  dl  sil dil bpl spl  r8b ...                r15b
高8: ah  bh  ch  dh                    （就这四个，且与 REX 互斥）
```

### 三条硬规则（都有实测）

1. **写 32 位寄存器会把高 32 位清零**：
   ```
   movl $0xFFFFFFFF, %eax   →  rax = 00000000_ffffffff
   ```
2. **写 8/16 位只改那几位**：
   ```
   rax = 1122334455667788，movb $0, %al  →  rax = 1122334455667700
   ```
3. 高字节寄存器（`ah/bh/ch/dh`）和 `r8`–`r15`、`sil/dil/bpl/spl` 不能同时出现在一条指令里。

规则 1 是后面所有“用 32 位写来零扩展”技巧的根据：常量在 `0 … 0xFFFFFFFF` 内时，
`movl $imm, %eax`（5 字节）就能得到相同的无符号 64 位值。
但 7 字节的 `movq imm32` 会符号扩展，仅当常量不超过 `0x7FFFFFFF` 时能这样对照；
`0xFFFFFFFF` 若用 64 位立即数形式写入 `%rax`，需 10 字节的 `movabs`。

### 结论：`%b` 是"取名字"，不是"搬家"

`%b1` 展开成**`%1` 所在那个寄存器**的字节名。`%1` 在 `%rcx` 时是 `%cl`，在 `%rsi` 时就是 `%sil`。
所以下面这种写法**不是**"把 n 放进 cl"：

```cpp
uint64_t rotl64_asm(uint64_t a, unsigned n) {
    __asm__ volatile("rolq %b1, %0" : "+r"(a) : "r"(n) : "cc");
    return a;
}
```

实测：

```asm
	rolq %sil, %rax
	q4.cpp: Assembler messages:
	q4.cpp:3: Error: operand type mismatch for `rol'
```

`rol` 的计数只有两种编码：`imm8`，或者**硬编码的 `%cl`**。要让 `%b1` 稳定展开成 `%cl`，
必须先用 `"c"(n)` 把值钉进 `%rcx` 家族 —— 这就是定稿版里 `"c"` 不能省的原因。

> 这一段结了 [`faq.md`](faq.md) 的 Q4。想自己先撞一次的话，就先把这段盖住去编译。

---

## 5. 模板串本身：分隔、转义、标签

1. **指令之间必须有分隔符**：`\n` 或 `;`。空格不行：
   ```asm
   movq %1, %0 addq %2, %0
   Error: junk `addq %rsi' after register
   ```
   `\t` 只是排版，不影响语义。
2. **一个 `asm` 是一"段"**，不是一条指令。多条指令共享一个操作数编号空间，也共享一次分配。
3. **`#` 是注释**（GAS/AT&T）。模板里写 `#` 会注释掉该行剩余内容 —— 多行模板里慎用。
4. **`%%` 才是字面 `%`**：`%%rax` → `%rax`。写单个 `%rax` 会被当成操作数引用。
5. **局部标签**：GAS 数字局部标签 `1:` / `1f` / `1b` 可以重复定义，
   `f` / `b` 找最近的前向/后向定义；不会仅因展开两次就撞名。
   普通命名标签才需要防重复，可用 `%=` 生成唯一后缀：

   ```cpp
   "jz .Ldone%=\n\t"      // 引用本次 asm 的唯一标签
   ...
   ".Ldone%=:\n\t"        // 定义同一标签
   ```
   示意展开：`jz .Ldone21` … `.Ldone21:`。
6. **basic asm vs extended asm**：
   - `asm("...")`（没有冒号）= basic asm：不能有操作数、永远隐式 volatile、只能写完整的固定指令。
     `naked` 函数里只能用它。
   - `asm("..." : ...)` = extended asm：本节所有规则的对象。

---

## 6. `volatile` 到底保证了什么

| 保证 | 不保证 |
|---|---|
| 不会被"输出没人用"删掉 | 不是内存屏障（那是 `"memory"`） |
| 不会被提出循环外 | 不阻止编译器把普通 load/store 跨过它重排 |
| 每次动态执行的副作用需保留 | 不保证不同 asm 始终相邻；必须相邻的指令放同一个模板 |
| —— | 不让你"少写 clobber" |

两个关键细节：

1. **没有输出的 extended asm 隐式 volatile**；basic asm 也隐式 volatile。
   但 basic asm 不处理 `%%` 转义：basic 用 `%rbx`，extended 才用 `%%rbx`。
2. **有输出、但输出没人用、又没 volatile → 整块删除**。实测：

   ```cpp
   __attribute__((noinline)) void del(void) {
       uint64_t r;
       __asm__("bswap %0" : "=r"(r) : "r"(g));   // 非 volatile，r 没人用
   }
   ```
   ```asm
   _Z3delv:
       endbr64
       ret                    ← asm 整块消失
   ```

推论：**调试期想看某条 asm 的真实开销，先确认它没被删。**
`-S` 里看不到 APP 标记时，先确认是否看对函数/实例，以及该路径是否被删除；
不能只凭标记缺失断定具体优化原因。

---

## 7. 立即数：约束管"能不能"，不管"多长"

### x86 的常量约束字母

| 约束 | 接受范围 | 典型用途 |
|---|---|---|
| `"i"` | 任意编译期整数常量 | 通用 |
| `"n"` | 已知数值的整数常量 | 比 `"i"` 更窄，少用 |
| `"I"` | 0 … 31 | 32 位移位计数 |
| `"J"` | 0 … 63 | 64 位移位计数 |
| `"K"` | **−128 … 127** | `imm8` 能装的有符号数 |
| `"L"` | `0xFF` / `0xFFFF` | `and` 做零扩展搬移 |
| `"M"` | 0 … 3 | `lea` 的比例因子 |
| `"N"` | 0 … 255 | `in` / `out` 端口号 |
| `"e"` | 32 位有符号常量 | x86-64 符号扩展立即数 |
| `"Z"` | 32 位无符号常量 | 零扩展立即数 |

两条必须记死的：

1. **`"K"(0xFF)` 直接编译失败** —— 255 不在 −128…127 里：
   ```
   error: impossible constraint in 'asm'
   ```
   `"K"(127)` 才能过。
2. **约束不改变汇编器的编码选择**。`"i"(0xFF)` 合法，但 GAS 照样给它 imm32 的 7 字节编码，
   因为 `imm8` 的 `0xFF` 会符号扩展成 `0xFFFF_FFFF_FFFF_FFFF`，语义就变成空操作了。

**想省字节只能换指令，不能换约束。** `and $0xFF` 想变短，正解是 `movzbl %dl, %edx`。

---

## 8. 编码长度实测表（`as` + `objdump`）

```asm
48 81 e2 ff 00 00 00   and   $0xff,%rdx       7  ← imm32（0xFF 进不了 imm8）
48 83 e2 7f            and   $0x7f,%rdx       4  ← imm8
81 e2 ff 00 00 00      and   $0xff,%edx       6  ← 32 位版本
0f b6 d2               movzbl %dl,%edx        3  ← 零扩展搬移，正解
31 c0                  xor   %eax,%eax        2  ← 比 mov $0,%eax 短 3 字节
b8 01 00 00 00         mov   $0x1,%eax        5
48 c7 c0 01 00 00 00   mov   $0x1,%rax        7  ← imm32 符号扩展
48 b8 + 8 字节         movabs $0xffffffff,%rax 10 ← 0xFFFFFFFF 超出有符号 imm32
48 d3 c0               rol   %cl,%rax         3  ← 计数只能是 cl 或 imm8
48 0f c8               bswap %rax            3
48 8d 04 77            lea   (%rdi,%rsi,2),%rax 4 ← 三操作数加法，不改 flags
48 85 c0               test  %rax,%rax        3
48 83 f8 00            cmp   $0x0,%rax        4  ← 比 test 多 1 字节
53 / 5b                push/pop %rbx          1
```

两个能直接用的结论：

- **x86-64 有 `mov r64, imm64` 编码**：GAS 可用 `movabsq` 明确表示。
  写 `movq $常量,%rax` 时，装得下符号扩展的 `imm32` 就可用 7 字节的形式，
  装不下才升级成 `movabs`（10 字节）。`0xFFFFFFFF` 恰好卡在"无符号 32 位装得下、有符号装不下"，
  所以拿到的是 10 字节 `movabs`。
- **`lea` 能计算地址表达式，但不是免费指令**：`leaq (%rdi,%rsi,2),%rax` = `rax = rdi + rsi*2`，
  3 操作数、**不改 flags**。后面算栈指针、算 TLS 偏移全靠它。

---

## 9. `"m"` 的真面目（以及为什么 context switch 不能用它）

`"m"` 给你的**不是地址，是"一个内存操作数"**，展开成什么由编译器决定：

```asm
	addq %rax, 16(%rdi)     # "+m"(p->b[1])
	addq $1, (%rdi)         # "+m"(p->a)
```

四条后果：

1. 地址寄存器是分配器挑的，**你看不见也控制不了**。它可能和你某个 `"r"` 操作数同址 ——
   这正是 `&` 也要管内存输出地址的原因（§2）。
2. **mem-to-mem 非法**：`movq (%rax), (%rbx)` 汇编器拒绝。两个 `"m"` 之间必须过一个寄存器。
3. `leaq` 的源操作数正是内存寻址形式，所以可以对合适的 `"m"` 操作数取有效地址。
   若接口需要的是指针值，直接传 `"r"(&x)` 通常更直接；两者都不等于读取 `x` 的内容。
4. **`rsp` 在 asm 前后必须一样**。GCC 文档原话：
   > the compiler requires the value of the stack pointer to be the same after an asm statement
   > as it was on entry to the statement.

   这就是为什么"切栈"这种操作**不可能**用普通 extended asm 写：
   一旦 `%rsp` 变了，所有 `"m"` 操作数的地址、编译器插的 spill/reload 全部作废。
   正确姿势是 `naked` 函数 / 整函数汇编 / 独立 `.S` 文件 —— Lesson 6 之后的主线。

---

## 10. ABI：GCC 替你兜的底，和兜不住的底

### clobber 一个 callee-saved 寄存器 = 编译器帮你 push/pop

```cpp
uint64_t f(uint64_t x) {
    __asm__ volatile("movq $1, %%rbx" ::: "rbx");
    return x + 1;
}
```

MinGW GCC 13.2 `-S`：

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

Linux GCC 13.3 是同一件事，只是 `.seh_*` 换成 `.cfi_*`。

**clobber 不是免费的**：它把"我弄坏了"翻译成一次真实的保存/恢复。
所以 `"rbx"` 能不加就不加 —— 但模板里真写了 `%%rbx` 就必须加。

### 两个特殊寄存器

| 写法 | 结果（实测） |
|---|---|
| `::: "rbp"` | `error: bp cannot be used in 'asm' here` |
| `::: "rsp"` | `warning: listing the stack pointer register 'rsp' in a clobber list is deprecated` |

`rsp` 不能作为普通可分配 GPR；`rbp` 在省略帧指针且目标允许时可以参与分配。
`rbp` 能否列 clobber 取决于当前函数是否用它作帧指针，不能概括为永远报错。

### 两套 x86-64 ABI 的差异（你写调度器时会疼的地方）

| | SysV AMD64（Linux） | MS x64（Windows / MinGW） |
|---|---|---|
| 整数参数 | `rdi rsi rdx rcx r8 r9` | `rcx rdx r8 r9` |
| 返回值 | 整数标量 `rax`；部分结构体 `rax:rdx` | 整数标量 `rax`；16 字节双指针结构体走隐藏缓冲区 |
| callee-saved | `rbx rbp r12–r15` | `rbx rbp rdi rsi r12–r15`，**还有 `xmm6–xmm15`** |
| caller-saved | 其余 GPR | 其余 GPR（`rax rcx rdx r8–r11` 等） |
| 寄存器之外 | 第 7 个起压栈 | 第 5 个起压栈；调用前还要留 **32 字节 shadow space** |
| 红色区域 | 有，`rsp` 以下 128 字节 | **没有** |
| 序言标记 | `.cfi_*` | `.seh_*` |

注意 `rsi`/`rdi`：在 Linux 上是 caller-saved（随便写），在 Windows 上是 callee-saved（写了要恢复）。
同一个 `asm` 在两个平台上"对不对"可能不同 —— 这就是 ABI 层的坑。

### 最贵的一个坑：`asm` 里 `call`

GCC 不知道你的模板 `call` 了什么，除非你声明。实测：

```cpp
extern "C" uint64_t side_effect(uint64_t);   // 返回 42
uint64_t bad_call(uint64_t a, uint64_t b) {
    uint64_t s = a + b;
    __asm__ volatile("call side_effect" : : "D"(s));   // 没写 clobber
    return s;
}
```

> 这段在 Linux/SysV 上测。你的 MinGW 第一个参数是 `%rcx`，要复现就把 `"D"` 换成 `"c"`。

```
bad_call(3,4)  = 42     ← 期望 7
good_call(3,4) = 7      ← 加上 "rax","rcx","rdx","rsi","r8"–"r11","cc","memory" 之后
```

`-S` 里 GCC 把 `s` 放在 `%rax` 上穿过 `call` —— 它认为 `%rax` 没被动过。
`call` 之后 `%rax` 是 `side_effect` 的返回值，于是函数返回 42。

x86 有个现成的约束字符干这事：**`"U"`** = call-clobbered 整数寄存器。
但列整数 clobber 并不够：还涉及向量寄存器、输入被调用者改写、栈对齐、
shadow space 和 SysV 红区。这个例子只演示失败机制，不是可靠的调用模板。
实际调用放在 C++，或放进 Lesson 5.5 的整函数汇编中。

---

## 11. 读 `-S` 的清单（MinGW 版）

拿你自己的 `lessons/lesson01/main.cpp` 生成的 `main`：

```asm
main:
	.seh_pushreg	%rsi
	.seh_pushreg	%rbx
	subq	$40, %rsp
	.seh_stackalloc	40
	.seh_endprologue
	movabsq	$72623859790382856, %rbx     # 0x0102030405060708
	call	__main
	movq	%rbx, %rdx
/APP
 # 18 "main.cpp" 1
	bswap %rdx
 # 0 "" 2
/NO_APP
	leaq	.LC0(%rip), %rsi
	movq	%rsi, %rcx
	call	_Z6printfPKcz
```

逐项认：

| 片段 | 是什么 |
|---|---|
| `/APP` … `/NO_APP` | **你写的模板**（Linux 上是 `#APP`/`#NO_APP`） |
| ` # 18 "main.cpp" 1` | 模板在源码里的行号，GCC 自己插的注释，不是你的 |
| `.seh_pushreg` / `.seh_stackalloc` | 异常展开信息，不是指令（Linux 对应 `.cfi_*`） |
| `call __main` | MinGW 在 `main` 里插的 C 运行时初始化，不是你写的 |
| `movabsq $7262…, %rbx` | 编译器把你写死的常量物化进寄存器 |
| `movq %rbx, %rdx` | 分配器为了三次 printf 复用同一个值插的搬运 |
| `leaq .LC0(%rip), %rsi` | PIC 下取字符串地址的标准姿势 |

判断法则一句话：**`/APP` 里面 = 你写的（只做了 `%n` 替换）；外面 = 编译器写的。**

三种量长度的办法：

```bash
# 1) 汇编 + 反汇编，看字节
g++ -O2 -S main.cpp -o main.s && as main.s -o main.o && objdump -d main.o

# 2) 让编译器把指令和源码交错打出来
g++ -O2 -Wa,-alh main.cpp -c -o /dev/null

# 3) 让 -S 自己带上 C 源码注释
g++ -O2 -fverbose-asm -S main.cpp -o main.s
```

---

## 12. 自查：这 12 个问题答不上来就是还没懂

1. `"=r"`、`"+r"`、`"=&r"` 三个各解决什么问题？哪个能省掉一条 `mov`，哪个必须加？
2. 模板先写输出、后读输入，为什么 `"+r"` 挡不住事故？给一段能跑错的代码。
3. `%b1` 展开成 `%cl` 的前提是什么？不满足时报什么错？
4. `%z0` 展开的是什么？和 `%k0` 差在哪？
5. `%ah` 什么时候不能用？报错原文是什么？
6. `movl $0xFFFFFFFF, %eax` 之后 `%rax` 是什么？`movb $0, %al` 之后呢？
7. `"K"(0xFF)` 和 `"i"(0xFF)` 分别什么结果？为什么换了约束也省不下那 3 个字节？
8. `and $0xFF,%rdx` 7 字节、`movzbl %dl,%edx` 3 字节，差在哪，为什么 `andq $0x7F` 只要 4 字节？
9. 为什么 `volatile` 不能替代 `"memory"`？为什么 `"memory"` 又不能替代 `mfence`？（Lesson 2）
10. `asm` 前后为什么 `%rsp` 必须一样？这条规则判了什么死刑？
11. 模板里 `call` 一个函数，最少要写哪些 clobber？漏了会怎样（给出实测值）？
12. 同一个 `asm`，为什么在 Linux 上对、在 MinGW 上可能不对？举一个寄存器。

---

## 附：本文所有实验的最小复现命令

```bash
# Linux / WSL（SysV）
g++ -std=gnu++17 -O2 -S x.cpp -o -        # 看代码生成
g++ -std=gnu++17 -O2 x.cpp -o x && ./x    # 看运行期错值
as x.s -o x.o && objdump -d x.o           # 量编码长度

# Windows / MinGW（MS x64，和你本机一致）
g++ -std=gnu++17 -O2 -Wall -Wextra -S main.cpp -o main.s
g++ -std=gnu++17 -O2 -Wall -Wextra main.cpp -o main.exe && main.exe
```
