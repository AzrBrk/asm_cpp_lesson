# Lesson 5.5 教案 — 从 Extended Asm 走到 naked / `.S`

> **状态：教案 v1，未开讲。这一节是原九节课里缺的"桥"。**
> 前置：Lesson 3–5（栈、保存责任、调用约定）。
> 本课目标：把"内联汇编"和"独立汇编程文件"之间的那道坎补上 —— 之后 Lesson 6 的栈切换
> 才有地方落脚。

---

## 0. 三个台阶

| 台阶 | 形态 | 谁管函数帧 | 谁管 ABI 边界 | 什么时候用 |
|---|---|---|---|---|
| 1 | extended asm | 编译器 | 编译器 | 一条指令、一段小程序，夹在 C 里 |
| 2 | `naked` 函数 | 你（无 prologue） | 你（但函数还是 C++ 编译器编译的） | 需要换 `rsp`、需要"整函数就是一段 asm" |
| 3 | 独立 `.S` / `.asm` | 你 | 你 | 切换原语、启动代码、和 C 无关的整块汇编 |

**为什么不能一直停在台阶 1**：Lesson 2 那条"asm 前后 `rsp` 必须相同"直接判了
"在 extended asm 里切栈"死刑。要换 `rsp`，就必须让整个函数归你管 —— 台阶 2 或 3。

**为什么不能直接跳到台阶 3**：台阶 1 学的约束/clobber/ABI，正是台阶 3 里你要**手工遵守**的东西。
跳过它，你会在 `.S` 里犯"忘了保存 rbx""忘了对齐"这种错，而且没有编译器提醒你。

---

## 1. 台阶 2：`naked`

```cpp
__attribute__((naked)) void ctx_switch(void **, void *) {
    __asm__ volatile(
        "pushq %rbp\n\t"
        ...
        "ret\n\t"
    );
}
```

`naked` 的语义：**GCC 不为这个函数生成任何序言/尾声**，函数体就是你写的 asm。

看 `-S`（MinGW）：

```asm
.globl  _ZN...ctx_switchEPPvS0_
.seh_proc _ZN...ctx_switchEPPvS0_
_ZN...ctx_switchEPPvS0_:
	.seh_endprologue
/APP
	pushq %rbp
	...
	ret
/NO_APP
	ud2
	.seh_endproc
```

四个细节：

1. **没有任何 `push rbp`/`sub rsp`** —— 除了你写的；
2. `.seh_endprologue` 后面直接是 asm，说明"序言到此为止"（其实没有序言）；
3. `ret` 之后还有一条 `ud2` —— 编译器认为函数"没有正常返回路径"；
4. **`.seh` 里也没有记录你 push 了什么** —— 异常穿过它，unwinder 会算错。

### naked 的限制

| 限制 | 后果 |
|---|---|
| 只能用 **basic asm**（没有操作数） | 参数不能用名字，只能自己知道 ABI 寄存器 |
| 函数体里不能有普通 C 语句 | 不能 `return`、不能声明局部变量 |
| 没有 prologue/epilogue | callee-saved 要自己存、栈对齐要自己算 |
| 没有 unwind 信息 | 异常/调试器穿过它会出问题 |
| 编译器不做寄存器分配 | 你写什么就是什么 |

**参数怎么拿**：按 ABI 直接读寄存器，用宏区分平台：

```cpp
#if defined(_WIN32)
#  define ARG1 "%rcx"
#  define ARG2 "%rdx"
#else
#  define ARG1 "%rdi"
#  define ARG2 "%rsi"
#endif
__attribute__((naked)) void f(void **, void *) {
    __asm__ volatile("movq " ARG1 ", %rax\n\t...");
}
```

`naked` 函数**仍然是一个 C++ 函数**：调用它的人按 ABI 传参、按 ABI 期待你保存
callee-saved。所以它只是"函数体归你"，边界仍然归编译器约定 —— 这正是台阶 2 和台阶 3 的区别。

---

## 2. 台阶 3：`.S` 文件

一个最小可用的 `.S`（Linux 和 MinGW 共用一份，用预处理区分）：

```asm
#if defined(_WIN32)
    .text
    .globl asm_add
asm_add:
    leaq (%rcx,%rdx), %rax      # MS x64：参数在 rcx/rdx
    ret
#else
    .text
    .globl asm_add
    .type asm_add, @function
asm_add:
    leaq (%rdi,%rsi), %rax      # SysV：参数在 rdi/rsi
    ret
    .size asm_add, .-asm_add
    .section .note.GNU-stack,"",@progbits
#endif
```

C++ 侧：

```cpp
extern "C" long asm_add(long, long);
```

编译命令（**`.S` 大写，走预处理器**；`.s` 小写不走）：

```bash
g++ -O2 main.cpp add.S -o a.out        # Linux
g++ -O2 main.cpp add.S -o a.exe        # MinGW
```

实测两边都能跑/链接，`nm` 里符号是 `asm_add`（没被 C++ 修饰）。

### 逐行解释

| 行 | 作用 | 不写会怎样 |
|---|---|---|
| `.text` | 进代码段 | 链接器可能把它当数据 |
| `.globl` | 导出符号 | 别的文件找不到 |
| `.type`/`.size` | 告诉链接器"这是一个函数，多大" | ELF 上调试/工具链信息缺失；PE 上不支持 |
| `.cfi_startproc`/`.cfi_endproc` | 生成 unwind 信息 | 异常/`gdb` 回溯在穿过它时断掉 |
| `.note.GNU-stack` | 声明"不需要可执行栈" | 链接器警告 `missing .note.GNU-stack section implies executable stack` |

### 加 CFI 的版本

```asm
asm_inc:
    .cfi_startproc
    leaq 1(%rdi), %rax
    ret
    .cfi_endproc
```

没有 `push`/`sub` 时，CFI 只需要 `startproc/endproc`（默认 CFA 就是 `rsp+8`）。
一旦你在 `.S` 里 `push` 了寄存器，就要同步写 `.cfi_def_cfa_offset` / `.cfi_offset`，
否则 unwinder 算出来的 CFA 是错的。

---

## 3. 符号与链接：`extern "C"` 是硬要求

C++ 会修饰名字。asm 里的符号是"裸"的，所以接口必须 `extern "C"`：

```cpp
long asm_add(long, long);   // 没有 extern "C"
```

实测链接错误：

```
undefined reference to `asm_add(long, long)'
```

链接器找的是 `_Z7asm_addll`，`.S` 里只有 `asm_add`。

**验证工具**：

```bash
nm a.out | grep asm_add        # T asm_add        ← 正确
objdump -d a.out | grep -A5 '<asm_add>'   # 看指令和字节
```

---

## 4. 两个平台的差异（写 `.S` 必须知道）

| | ELF（Linux） | PE/COFF（MinGW） |
|---|---|---|
| 段指令 | `.section .text` | `.text` / `.data` |
| 函数元数据 | `.type` / `.size` | `.def`（可选） |
| unwind | `.cfi_*` | `.seh_*` |
| 可执行栈声明 | `.note.GNU-stack` | 无 |
| 参数寄存器 | `rdi rsi rdx rcx r8 r9` | `rcx rdx r8 r9` |
| 栈参数 | 第 7 个起 | 第 5 个起 + 32 字节 shadow |
| 符号前缀 | 无 | x64 无（32 位才有 `_`） |

写跨平台 `.S` 的常见做法：用 `#if defined(_WIN32)` 把寄存器名、`.seh`、段声明分开，
正文逻辑共用 —— 和 demo 里的宏是一个思路。

### PIC：不要写绝对地址

```asm
leaq  symbol(%rip), %rax      # 推荐：RIP 相对，PIC 安全
movq  $symbol, %rax           # 绝对重定位，PIE 下会被链接器抱怨
```

MinGW 默认也生成 PIE/PIC 友好的代码，取全局变量地址用 `leaq sym(%rip)` 最稳。

---

## 5. 从 C 调 asm / 从 asm 调 C

### C → asm

编译器按 ABI 传参、对齐栈、期待你保存 callee-saved。你要做的：

1. 入口 `rsp % 16 == 8`；
2. 用了 callee-saved 就存/恢复；
3. 返回值放 `rax`（浮点 `xmm0`）；
4. 不碰 red zone（SysV 上尤其）。

### asm → C

从 `.S` 里 `call printf` 之类：

1. **调用前 `rsp % 16 == 0`**（你负责把账算平）；
2. MS x64 还要预留 32 字节 shadow space；
3. caller-saved 的值调用后就没了；
4. 可变参数要设 `%al`。

一个最简单的对齐模板（进入时 `rsp % 16 == 8`）：

```asm
my_func:
    pushq %rbp              # 8 字节 → 现在 %16 == 0，可以 call
    movq  %rsp, %rbp
    call  some_c_function
    popq  %rbp
    ret
```

`push` 一个 8 字节寄存器，恰好把 `8` 补成 `0` —— 这就是"为什么序言总先 push rbp"。

---

## 6. 工具链：怎么确认自己写对了

| 目的 | 命令 |
|---|---|
| 看 C 生成的汇编 | `g++ -O2 -S x.cpp -o -` |
| 看 `.S` 生成的机器码 | `g++ -c x.S -o x.o` + `objdump -d x.o` |
| 看符号 | `nm x.o` / `nm a.out` |
| 看重定位 | `objdump -r x.o` |
| 调试 | `gdb a.out`，`layout asm` / `x/8gx $rsp` |
| 检查栈对齐 | 在函数入口 `p $rsp % 16` |

**判断 `.S` 写没写对的最小闭环**：`nm` 符号对 → `objdump -d` 指令对 → 跑起来结果对 →
`gdb` 里 `bt` 能穿过它。

---

## 7. 任务

### 任务 A：第一个 `.S`

写 `asm_max(long, long)`，返回较大值：

- Linux 和 MinGW 共用一份 `.S`（用 `#if defined(_WIN32)` 分寄存器）；
- C++ 侧 `extern "C"`；
- 提交两边的 `nm`、`objdump -d` 和运行结果。

### 任务 B：加 CFI

给任务 A 的函数加 `.cfi_startproc/.cfi_endproc`，再用 `gdb` 在它里面 `bt`，
确认栈能回溯。然后故意删掉 CFI，看 `bt` 变成什么样。

### 任务 C：asm 调 C

在 `.S` 里写 `asm_call_printf`：调用 `printf("%ld\n", x)`。要求：

- 对齐算对（需要时 `subq $8, %rsp`）；
- MinGW 下预留 32 字节 shadow space；
- 说明 `%al` 要不要设。

### 任务 D：naked 版

用 `__attribute__((naked))` 重写任务 A，和 `.S` 版对比 `-S`/`objdump` 输出，
说明两者的差别（编译器还管哪些东西）。

---

## 8. 易错点

1. `.s` 小写导致预处理没跑，`#if` 失效。
2. 忘了 `extern "C"`，链接器报 mangled 名字。
3. 在 `.S` 里用了 callee-saved 却没存/恢复。
4. `call` 前没对齐 `rsp`（平时没事，`movaps` 崩）。
5. MinGW 下没留 shadow space。
6. `.type`/`.note.GNU-stack` 直接抄到 PE 上，汇编器报错。
7. 用了绝对地址，PIE 下链接告警。
8. naked 函数里写 C 语句（编译不过或行为未定义）。

---

## 9. 自测

1. 三个台阶各自"谁管函数帧、谁管 ABI 边界"？
2. `naked` 函数里为什么不能写 `return`？
3. `.S` 和 `.s` 的区别？
4. `extern "C"` 到底改变了什么（符号层面）？
5. `.cfi` 是给谁看的？不写会怎样？
6. 进入 `.S` 函数时 `rsp % 16` 是多少？要 `call` 别人之前要变成多少？
7. MS x64 的 shadow space 多大、什么时候要留？

---

## 10. 和前后课的关系

| 课 | 关系 |
|---|---|
| Lesson 3–5 | 这里的每一条都是那三课的手工版 |
| Lesson 6 | 栈切换必须写成台阶 2 或 3 |
| Lesson 7 | 首次进入要构造假帧（`.S` 里最典型的操作） |
| Lesson 9 | boost 的 `jump_fcontext` 就是台阶 3 的工业版 |
