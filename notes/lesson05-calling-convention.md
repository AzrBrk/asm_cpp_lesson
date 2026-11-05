# Lesson 5 教案 — 调用约定

> **状态：已补参数分类与验收，未开讲。**
> 前置：Lesson 3（栈与寄存器保存）。
> 本课目标：看到一个函数签名，能立刻说出**参数在哪、返回值在哪、谁保存哪个寄存器、
> 栈要留多少**；并且能在 asm 里手工遵守同一份合同。

---

## 开讲拆分

| 顺序 | 小任务 | 停点/证据 |
|---|---|---|
| 1 | 全为 64 位整数的 6 参数 MS 调用 | 圈出 `rcx/rdx/r8/r9` 和入口 `S+40/S+48` |
| 2 | 在同一签名中把第 2 参数换成 `double` | 解释为何是 `xmm1`，不是 `xmm0` |
| 3 | 返回 24 字节结构体 | 标隐藏指针及被后移的真实参数 |
| 4 | 调用一个别的函数 | 画本函数入站 home slots 与出站 shadow space，逐次算对齐 |

前置：会用 Lesson 3 的 `S` / `D` 换算偏移。函数应单独编译、加 `noinline`，
防止常量折叠/内联掩盖调用边界；必要时用单独翻译单元，不开 LTO。
主线只交 MinGW；SysV 的 `%al`、聚合分类是随后对照，不混入 MS 规则。

---

## 0. 为什么要单独讲 ABI

`ctx_switch` 的调用方是 C 编译器，它只认 ABI：

- 它把参数放进约定的寄存器；
- 它假设某些寄存器调用前后不变；
- 它按约定对齐栈。

你的 asm 只要有一处不认账，就出现"平时没事、一换编译器就崩"的 bug。
两套 x86-64 ABI 必须同时记住：**SysV（Linux/macOS）** 和 **MS x64（Windows/MinGW）**。

---

## 1. 参数：前几个进寄存器

以下表只适用于**全是整数/指针、没有隐藏参数**的签名，偏移相对序言前入口 `rsp`：

| 参数序号 | SysV | MS x64 |
|---|---|---|
| 1 | `rdi` | `rcx` |
| 2 | `rsi` | `rdx` |
| 3 | `rdx` | `r8` |
| 4 | `rcx` | `r9` |
| 5 | `r8` | **栈**（`40(%rsp)`） |
| 6 | `r9` | 栈 |
| 7+ | **栈**（`8(%rsp)`） | 栈 |

两个可以直接验证的偏移：

```asm
# SysV：第 7 个参数在返回地址之上
seven:
    addq 8(%rsp), %rax        # 0(%rsp) 是返回地址
    ret

# MS x64：第 5 个参数在返回地址 + 32 字节 shadow space 之后
five:
    addl 40(%rsp), %eax       # 8 + 32 = 40
    ret
```

**为什么 MS 是 40 而不是 8**：MS x64 要求调用者为被调用者预留 32 字节 shadow space，
不管被调用者用不用。这 32 字节永远在返回地址上面。

### 1.1 浮点不是“再顺着整数寄存器数”

例如有原型的 `void mixed(std::int64_t a, double b, std::int64_t c, double d)`：

| 参数 | MS x64（按参数位置选槽） | SysV（整数/SSE 各自计数） |
|---|---|---|
| `a` | `rcx` | `rdi` |
| `b` | `xmm1` | `xmm0` |
| `c` | `r8` | `rsi` |
| `d` | `xmm3` | `xmm1` |

MS 的前四个位置各占一槽，不会把未用的 `rdx` 再给 `c`。
可变参数还有额外规则，见 §3。聚合类型不能直接照此表分类。

---

## 2. 返回值：标量和结构体

标量：`rax`（浮点是 `xmm0`）。

**大结构体走隐藏指针**，而且两套 ABI 用的寄存器不同：

```cpp
struct Big { std::int64_t a, b, c; }; // 两个平台都是 24 字节，不用大小不同的 long
Big make_big(std::int64_t x) { return Big{x, x+1, x+2}; } // 实验输入避开有符号溢出
```

SysV：

```asm
make_big:
    leaq 1(%rsi), %rdx
    movq %rsi, (%rdi)         # rdi = 隐藏的返回缓冲区指针
    ...
    movq %rdi, %rax           # 按约定把指针也放回 rax
    ret
```

MS x64：

```asm
make_big:
    movq %rcx, %rax           # rcx = 隐藏指针
    movq %rdx, (%rcx)         # rdx = 第一个 64 位参数
    ...
    ret
```

**本例判据**：这个 24 字节普通结构体需要隐藏返回缓冲区指针 ——
在 SysV 上占 `rdi`，在 Windows 上占 `rcx`，把后面所有参数整体后移一位。
写跨平台 asm 时这是最容易错位的地方。

不要把“大小”当作所有结构体返回规则。SysV 还会按字段分类；
MS 对简单 1/2/4/8 字节聚合可能用 `rax`，16 字节双指针结构体通常走隐藏指针，
并非 `rax+rdx`。非平凡构造/析构还会改变约定。本课只验证上面这个确定的签名。
Windows x64 是 LLP64：`long` 为 32 位，指针为 64 位；Linux 常见 LP64 的 `long` 为 64 位。
在涉及 `q` 后缀与 24 字节大小的练习中，用 `<cstdint>` 的固定宽度类型。

---

## 3. 可变参数：`%al` 的秘密

SysV 调用可变参数函数（`printf`）前，`%al` 表示所用向量寄存器数量的上界（0–8）：

```asm
# printf("%d\n", i) —— 没有浮点参数
xorl %eax, %eax
call printf

# printf("%f\n", d) —— 用了 1 个 xmm
movl $1, %eax
call printf
```

`-S` 里看到 `movl $1, %eax` 不是"设置返回值"，是在告诉 `printf`：
"我传了 1 个 xmm 参数，你去对应的寄存器取"。

这条和 codegen 的关系：你手写 asm 调 `printf` 时忘了 `%al`，浮点参数会读错 —— 而且不报错。

**MS x64 不用这条 `%al` 规则。** 前四个位置上的浮点可变参数须同时放对应 XMM 和 GPR。
例如 `printf("%f", d)`：格式串在 `rcx`，`d` 的位模式在 `xmm1` 和 `rdx`，
不是数值转成整数。还需要 32 字节 shadow space。
格式串也必须与 C 类型一致；固定宽度整数输出用 `<cinttypes>` 宏，别猜 `%ld`。

---

## 4. 对齐：和 Lesson 3 同一笔账

- `call` 前 `rsp % 16 == 0`；
- 函数入口 `rsp % 16 == 8`；
- MS x64 再额外要 32 字节 shadow space。

写 asm 函数被 C 调用时，入口的 `rsp` 已经是"8 mod 16"，你的 `push` 数量必须让
调用别的函数前重新对齐到 0。写错了，第一次 `movaps` 就会崩。

---

## 5. 谁保存谁

| | SysV | MS x64 |
|---|---|---|
| callee-saved | `rbx rbp r12–r15` | `rbx rbp rdi rsi r12–r15`，`xmm6–xmm15` |
| caller-saved | 其余 | 其余（`rax rcx rdx r8–r11`，`xmm0–5`） |

- 写**被调用**的 asm 函数：你用了 callee-saved 就要在开头存、结尾恢复；
- 写**调用别人**的 asm：caller-saved 的值调用后就没了，别指望；
- 写 `ctx_switch`：保存目标 ABI 的完整非易失状态；GPR 并集不包含 Windows 的 XMM/平台状态。

---

## 6. 名称修饰：为什么 `extern "C"`

C++ 编译器会给函数名加前缀/后缀（mangling）。asm 里写的是汇编符号，不会自动加。
所以跨 C/C++ 和 asm 的接口必须 `extern "C"`：

```cpp
extern "C" long asm_add(long, long);   // 符号就是 asm_add
```

忘了会怎样（实测）：

```
undefined reference to `asm_add(long, long)'
```

链接器找的是 `_Z7asm_addll`，而你的 `.S` 里只有 `asm_add`。

---

## 7. 序言、尾声与 unwind

编译器给每个函数插的序言/尾声不只是"存 rbp"，还负责让**异常/调试器**能沿着栈走回去：

- Linux：`.cfi_startproc` / `.cfi_def_cfa_offset` / `.cfi_offset` / `.cfi_endproc`；
- Windows（MinGW）：`.seh_proc` / `.seh_pushreg` / `.seh_stackalloc` / `.seh_endprologue`。

这些**不是指令**，是给 unwinder 看的元数据。`naked` 函数通常没有描述手写帧的完整信息 ——
一旦异常穿过你的 `ctx_switch`，栈就"接不上"了。这是 Lesson 6 要接受的代价。

---

## 8. 尾调用

`return f(x)` 这种形式，编译器可能不 `call` 再 `ret`，而是直接 `jmp`：

```asm
one_push:
    pushq %rbx
    ...
    popq %rbx
    jmp __printf_chk@PLT     # 不是 call
```

尾调用要求：**栈恢复到调用者进入时的状态**（包括对齐），然后 `jmp`。
这是复用原调用者返回地址的例子，**不意味着 `ret` 和任意 `jmp` 等价**：
`ret` 还读栈并推进 `rsp`；首次进入时必须显式构造同样的栈效果。

---

## 9. 任务

### 任务 A：数参数

写一个 9 参数的函数，`-O2 -S` 下指出第 7、8、9 个参数分别在哪个偏移；
主线先在 MinGW 下对 6 参数函数做同样的事；SysV 可用教案对照图。
说明偏移相对入口还是序言后 `rsp`，不能脱离序言只报一个数字。

### 任务 B：结构体

写一个返回 24 字节结构体的函数，分别在两套 ABI 下贴 `-S`，
标出隐藏指针用的是哪个寄存器、第一个真实参数被挤到了哪。

### 任务 C：手写被调用者

本课先写出 `std::int64_t asm_max(std::int64_t, std::int64_t)` 的 ABI 合同：

- 只用 `rax` 和参数寄存器，不碰 callee-saved；
- 列出允许改写的寄存器、返回寄存器、有符号比较条件；
- 讨论：如果用了 `rbx`，要在哪里恢复？

到 Lesson 5.5 学完工具链再实现，不让任务抢跑前置知识。

### 任务 D：问答

1. 第 7 个参数在 SysV 和 MS 上分别怎么取？
2. 为什么 MS 的栈参数从 40 开始？
3. `%al` 在可变参数调用里是什么？
4. 大结构体返回时隐藏指针占哪个寄存器？
5. 为什么 asm 接口必须 `extern "C"`？

---

## 10. 易错点

1. 把 SysV 的参数顺序套到 Windows 上（`rdi` vs `rcx`）。
2. 忘了 MS 的 32 字节 shadow space。
3. 手写 asm 调 `printf` 不设 `%al`。
4. 大结构体返回时按"普通参数"数偏移。
5. 用 C++ 名字声明 asm 函数，链接时找不到。
6. 以为 `-O2` 一定有 `push rbp`。
7. 以为 `.cfi`/`.seh` 是"可选的装饰"（异常/调试时不是）。

---

## 11. 自测

1. 画一张 6 参数 SysV 调用的现场图。
2. `call` 前后 `rsp` 各是多少 mod 16？
3. 哪些寄存器在 Windows 上 callee-saved、Linux 上不是？
4. `printf("%f", d)` 前 `%eax` 是多少？为什么？
5. 隐藏返回指针在两套 ABI 里分别占谁？
6. `.cfi` 和 `.seh` 各是什么？谁在读它们？

---

## 12. 和前后课的关系

| 课 | 关系 |
|---|---|
| Lesson 3 | 栈帧与保存责任的基础 |
| Lesson 5.5 | 把这里的合同落到 `.S` 文件里 |
| Lesson 6 | `ctx_switch` 必须遵守同一份合同 |
| Lesson 9 | boost 的 `transfer_t` 就是"用 ABI 传两个值" |
