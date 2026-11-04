# asm_cpp_lesson

在 C++ 里写内联汇编：GCC Extended Asm / AT&T 语法 / x86-64。

目标不是"会用 asm"，而是能手写 context switch、看懂 `boost.context` 的 `jump_fcontext`。

编译环境：g++ (MinGW-w64)，统一 `-std=gnu++17 -O2 -Wall -Wextra`，汇编语法默认 **AT&T**，不开 `-masm=intel`。

## 目录

| 路径 | 内容 |
|---|---|
| [`AGENTS.md`](AGENTS.md) | 给接手这个仓库的 AI 的教学协议：角色、硬性约定、提交格式、评分口径、当前进度 |
| [`notes/course-map.md`](notes/course-map.md) | 课程地图：三级台阶（内联 → naked → `.S`）、九课依赖、每课证据 |
| [`lessons/lesson01/main.cpp`](lessons/lesson01/main.cpp) | Lesson 1 作业：`bswap64_asm` / `rotl64_asm` / `mask_imm_asm` |
| [`demos/hello-switch/`](demos/hello-switch/) | 最小上下文切换 demo：两个函数各自用栈，交替打印 `hello world` |
| [`notes/lesson01-extended-asm.md`](notes/lesson01-extended-asm.md) | Lesson 1 全部知识点：占位符、约束符、AT&T vs Intel、clobber、立即数编码 |
| [`notes/lesson01-deep-dive.md`](notes/lesson01-deep-dive.md) | Lesson 1 补讲：约束串文法、`&`、修饰符全集、宽度模型、立即数编码、`"m"`、ABI |
| [`notes/lesson02-clobber.md`](notes/lesson02-clobber.md) | Lesson 2 教案 v2：clobber / `"memory"` / `"cc"` / `rdtsc`（未开讲） |
| [`notes/lesson03-stack-registers.md`](notes/lesson03-stack-registers.md) | Lesson 3：栈帧、红区、对齐、callee-saved、状态清单 |
| [`notes/lesson04-early-clobber.md`](notes/lesson04-early-clobber.md) | Lesson 4：多输出、固定寄存器、数字匹配、`asm goto` |
| [`notes/lesson05-calling-convention.md`](notes/lesson05-calling-convention.md) | Lesson 5：两套 ABI、隐藏指针、`%al`、`.cfi`/`.seh` |
| [`notes/lesson05b-naked-and-asm.md`](notes/lesson05b-naked-and-asm.md) | Lesson 5.5：`naked` 与 `.S` 文件（原课程表缺的桥） |
| [`notes/lesson06-stack-switch.md`](notes/lesson06-stack-switch.md) | Lesson 6：`ctx_switch`、保存集合、栈分配、TEB/红区 |
| [`notes/lesson07-first-entry.md`](notes/lesson07-first-entry.md) | Lesson 7：假帧、对齐账、传参、`jmp` vs `ret` |
| [`notes/lesson08-minimal-scheduler.md`](notes/lesson08-minimal-scheduler.md) | Lesson 8：Fiber 结构、RR 调度、不变量、guard page |
| [`notes/lesson09-boost-context.md`](notes/lesson09-boost-context.md) | Lesson 9：逐段对照 `jump_fcontext` / `make_fcontext` |
| [`notes/faq.md`](notes/faq.md) | 练习中提出的问题与解答 |
| [`progress.md`](progress.md) | 课程表、评分记录、易错点清单 |

## 课程进度

| # | 课 | 状态 | 最佳分（正确性/ABI/性能） |
|---|---|---|---|
| 1 | Extended Asm 基本语法与约束符 | ✅ 通过 | 88（90 / 85 / 88） |
| 2 | Clobber List：memory 屏障与 cc | 🟡 教案 v2 就绪，未开讲 | — |
| 3–9 | 见 [`course-map.md`](notes/course-map.md) | 📝 教案就绪，未开讲 | — |
| 5.5 | naked / `.S`（新增过渡课） | 📝 教案就绪，未开讲 | — |

## 约定

1. 每段 asm 都必须能被 `-S` 解释清楚：哪条指令来自模板、哪个寄存器是分配器挑的、哪条是编译器插的。
2. 判定对错只看 `fiber_submit` 的编译/运行/`-S` 报告，不靠肉眼读代码。
3. 只写 `-O2` 下会被真正执行的代码路径；被优化掉的分支要能说清为什么。
