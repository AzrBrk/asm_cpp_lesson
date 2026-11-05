# 课程地图 — 这套课在教什么，为什么是这个顺序

> 这份文件回答三个问题：**整条线怎么走、每课产出什么、哪里是坑。**
> 教案本身在各 `lessonNN-*.md` 里；这里只放地图。

---

## 1. 一句话

从"在 C++ 里写一条不会骗编译器的 asm"开始，
一路走到"读懂 `boost.context` 的 `jump_fcontext`"。

终点不是"会写汇编"，是**知道一个执行流的状态由什么组成、怎么原样保存和恢复**。

---

## 2. 三级台阶（这是原课程表没写出来的那句）

| 台阶 | 形态 | 谁管函数帧 | 谁管 ABI 边界 | 课程位置 |
|---|---|---|---|---|
| 1 | extended asm | 编译器 | 编译器 | Lesson 1–5 |
| 2 | `naked` 函数 | 你 | 你实现；编译器只按声明生成调用方 | Lesson 5.5、6、7 |
| 3 | 独立 `.S`/`.asm` | 你 | 你 | Lesson 5.5、9 |

**为什么必须走这个顺序**：台阶 1 学的约束/clobber/ABI，正是台阶 3 里要手工遵守的东西。
跳过台阶 1 直接写 `.S`，你会犯"忘了保存 rbx""忘了对齐"这类错，而且没有编译器拦你。

**为什么必须下台阶**：Lesson 2 那条"asm 前后 `rsp` 必须相同"直接判了
"在 extended asm 里切栈"死刑。要换 `rsp`，整个函数就必须归你管。

---

## 3. 九课 + 一节桥

| # | 课 | 核心问题 | 产出 | 教案 |
|---|---|---|---|---|
| 1 | Extended Asm 与约束符 | 一条 asm 怎么和编译器说话 | `bswap`/`rol`/`and` 三个函数 | [lesson01-extended-asm.md](lesson01-extended-asm.md) + [deep-dive](lesson01-deep-dive.md) |
| 2 | Clobber：memory / cc / rdtsc | 我弄坏了什么，怎么声明 | `rdtsc_asm`、`bad_store` | [lesson02-clobber.md](lesson02-clobber.md) |
| 3 | 栈与寄存器保存 | 一个执行流的状态是什么 | 画帧、手工保存/恢复 | [lesson03-stack-registers.md](lesson03-stack-registers.md) |
| 4 | Early clobber 与多输出 | 输出和输入谁先谁后 | 双输出 asm、`cpuid` | [lesson04-early-clobber.md](lesson04-early-clobber.md) |
| 5 | 调用约定 | 参数/返回值/保存责任 | 跨 ABI 的 asm 函数 | [lesson05-calling-convention.md](lesson05-calling-convention.md) |
| **5.5** | **naked 与 `.S`（桥）** | **怎么从内联走下来** | **`asm_add.S`、naked `max`** | [lesson05b-naked-and-asm.md](lesson05b-naked-and-asm.md) |
| 6 | 栈切换 | 怎么换 `rsp` | `ctx_switch` | [lesson06-stack-switch.md](lesson06-stack-switch.md) |
| 7 | 首次进入 | 新栈上怎么"开始" | 假帧、传参、退出 | [lesson07-first-entry.md](lesson07-first-entry.md) |
| 8 | 最小调度器 | 从 2 个到 N 个 | RR 调度 + 不变量测试 | [lesson08-minimal-scheduler.md](lesson08-minimal-scheduler.md) |
| 9 | 与 boost 对照 | 工业代码多了什么 | 逐行注释 `jump_fcontext` | [lesson09-boost-context.md](lesson09-boost-context.md) |

**过渡章节**：5.5 是这次新加的。原课程表里 3/5/6/7 各自沾一点"裸函数/独立汇编"，
但没有一节正面讲，导致"先内联再 native"这条线一直模糊。

---

## 4. 依赖关系

```
1 约束 ──▶ 2 clobber ──▶ 3 栈与保存 ──▶ 4 early clobber
                              │
                              ▼
                         5 调用约定 ──▶ 5.5 naked/.S
                                            │
                                            ▼
                                       6 栈切换 ──▶ 7 首次进入 ──▶ 8 调度器
                                                                      │
                                                                      ▼
                                                                 9 boost
```

- 3 和 4 可以交换（都只依赖 1/2）；
- 5.5 必须在 6 之前，否则 6 没有落脚点；
- 7 依赖 3 的对齐账和 6 的切换原语；
- 9 依赖 6/7（读代码时对照）。

---

## 5. 每课的"证据"

这套课判定"懂了"的标准不是"能跑"，是**能看到 `-S`/`objdump`/运行值**：

| 课 | 必须看到的证据 |
|---|---|
| 1 | `/APP` 里外的指令、立即数编码字节 |
| 2 | 漏 `"memory"` 的 load 合并/重排证据、保存寄存器的代价 |
| 3 | MS x64 帧布局、shadow space、对齐填充；红区仅作 SysV 对照 |
| 4 | 逐输出读写时序；漏 `&` 时允许的危险分配，不强求某个错值 |
| 5 | 参数偏移、隐藏指针、`%al` |
| 5.5 | `nm` 符号、`objdump` 指令、`bt` 回溯 |
| 6 | 切换前后 `rsp`、运行交替输出 |
| 7 | 按平台推导假帧；入口参数、shadow space、退出前对齐 |
| 8 | 不变量 `assert` 全过 |
| 9 | 逐行注释、栈布局图 |

**复现不了 ≠ 安全**：`&` 和 `"memory"` 的错误是否显现取决于优化与分配。
GCC 13 的 x86 后端还会隐式破坏条件码，不能把它解释成“漏 `"cc"` 只是暂时没炸”
（见 [Lesson 2 §3.4](lesson02-clobber.md)）。

横切的分类表在 [`inline-asm-ub.md`](inline-asm-ub.md)：L0 硬错误 / L1 GCC 契约 /
L2 未指定 / L3 C++ UB。判断"这段 asm 是不是 UB"时先查那张表。

---

## 6. 环境

| 项目 | 值 |
|---|---|
| 编译器 | g++ (MinGW-w64)，`-std=gnu++17 -O2 -Wall -Wextra` |
| 汇编语法 | AT&T，禁止 `-masm=intel` |
| 平台 | Windows x64（MS ABI）；Linux 仅作对照 |
| 参考实现 | boost.context 用 **`ms_pe_gas`** 那一份，不是 ELF |

两套 ABI 的差异（`rsi/rdi` 的归属、shadow space、`xmm6–15`）在
[Lesson 5](lesson05-calling-convention.md) 有表。

---

## 7. 怎么用这套教案

1. **先读本节，不预习整课答案**（手机上也行），不要求立刻动手；
2. 开讲时按"一次一个知识点"推进，讲完一个点做一个小任务；
3. 每个任务都要贴 `-S`/`objdump`/运行结果，**没有证据不算过**；
4. 评分：正确性 40% / ABI 30% / 性能 30%，≥ 80 通过；
5. 卡住时给反例、给报错、指位置，**不给完整可编译实现**。

### 7.1 每个知识点的闭环

教案是备课材料，不是一次性讲稿。每次只执行下面一轮：

**检查前置 → 解释一个概念 → 手算一个局部例子 → 做一个小任务 → 交证据 → 停。**

先回答“编译器知道什么”，再回答“CPU 实际做什么”。不能用“运行一次正常”替代两者。
每课新增的“开讲拆分”表规定停止点；未通过当前点，不进入下一行。
`asm goto`、跨 ABI 对照、guard page、计时都是后置拓展，不阻塞主线。

### 7.2 工具与证据：先确认你在编译哪个平台

在 MinGW 终端运行；命令中的文件名按当前任务替换：

```bash
# 三项一起交：编译器版本、目标三元组、64 位指针所用目标
g++ --version
g++ -dumpmachine
# 先生成 .s，再用 -c 真正经过汇编器；-S 成功不代表指令合法
g++ -std=gnu++17 -O2 -Wall -Wextra -S main.cpp -o main.s
g++ -std=gnu++17 -O2 -Wall -Wextra -c main.cpp -o main.o
objdump -dr main.o
# 链接并运行，记录 stdout、stderr 和退出码
g++ -std=gnu++17 -O2 -Wall -Wextra main.cpp -o main.exe
./main.exe
```

`objdump` / `nm` 用同一套 MinGW 工具链的版本；没有命令时先确认安装路径。
MSYS2/Bash 中用 `echo $?` 取刚结束程序的退出码，PowerShell 用 `$LASTEXITCODE`。
不要在运行与取退出码之间插别的命令。编译失败就停，不要运行上一次留下的 exe。

| 工具/术语 | 初学时只需要知道什么 |
|---|---|
| `-S` | C++ → 汇编文本，检查模板替换和编译器搬运，不显示最终字节 |
| `-c` | 编译并汇编 → 目标文件，尚未链接 |
| `objdump -dr` | 指令字节 + 反汇编 + 重定位；默认 AT&T，不加 Intel 选项 |
| `nm` | 符号名；`T` 是已定义代码符号，`U` 是仍待链接的引用 |
| live（活值） | 后面还会读到的值；不是“寄存器当前非零” |
| spill / reload | 寄存器值写到栈 / 再读回；看 asm 外的访存 |
| ABI | 函数边界合同，不是 CPU 自动帮你存寄存器 |
| trampoline | 把首次进入调整成正常函数调用的汇编跳板 |

### 7.3 如何区分材料中的证据

- **推导/示意**：允许某种分配、偏移怎么算；不是你机器的实测输出。
- **历史实测**：旧教案保留的特定工具链结果；不保证你的版本复现相同寄存器或报错文本。
- **本次提交证据**：完整命令、编译 stderr、运行 stdout/退出码、相关 `-S`/反汇编。
  只有这一项能用来给你的作业评分。

没有 Windows 运行条件时，只能标“MinGW 编译/反汇编已检查，运行未验证”。
主线只要求你的 Windows x64 环境；Linux 对照可以读图，不要求额外搭一台机器。

### 7.4 查证入口

| 问题 | 主来源与阅读位置 |
|---|---|
| 约束、`volatile`、`&`、clobber | [GCC Extended Asm](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html)，对应同名小节 |
| 固定寄存器/立即数范围 | [GCC Machine Constraints](https://gcc.gnu.org/onlinedocs/gcc/Machine-Constraints.html)，x86 family |
| `naked` 允许什么 | [GCC x86 Function Attributes](https://gcc.gnu.org/onlinedocs/gcc/x86-Function-Attributes.html)，`naked` |
| MS 参数/返回/浮点保存 | [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)，Parameter passing / Return values / MXCSR |
| shadow space / 栈边界 | [Microsoft x64 stack usage](https://learn.microsoft.com/en-us/cpp/build/stack-usage) |
| SysV 对照 | [x86-64 psABI](https://gitlab.com/x86-psABIs/x86-64-ABI)，寄存器、栈、参数分类章节 |
| 指令语义/flags/异常 | [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)，Vol.2 对应指令的 Operation / Flags Affected / Exceptions |

官网不可访问时可查官方仓库：[GCC 文档源码](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13/gcc/doc/extend.texi)、
[Microsoft 文档源码](https://github.com/MicrosoftDocs/cpp-docs/blob/main/docs/build/x64-calling-convention.md)。
编码长度必须带具体操作数用 `objdump` 验证；某条指令的周期数不能从字节数推出来。

---

## 8. 当前状态

| 课 | 教案 | 开讲 | 任务 |
|---|---|---|---|
| 1 | ✅ 已补讲 | ✅ 已通过（88） | ✅ 完成 |
| 2 | ✅ 已补学习闭环并勘误 | ⬜ 未开始 | ⬜ 未开始 |
| 3–9 | ✅ 已补推导、任务验收与平台边界 | ⬜ 未开始 | ⬜ 未开始 |
| 5.5 | ✅ 已补工具链与调用边界 | ⬜ 未开始 | ⬜ 未开始 |
| demo | ✅ hello-switch | — | 可观摩 |

教案是"材料"，不是"讲稿"：开讲时仍然一次只讲一个知识点，讲完停下来等代码。
