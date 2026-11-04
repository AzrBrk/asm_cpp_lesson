# asm_cpp_lesson

在 C++ 里写内联汇编：GCC Extended Asm / AT&T 语法 / x86-64。

目标不是"会用 asm"，而是能手写 context switch、看懂 `boost.context` 的 `jump_fcontext`。

编译环境：g++ (MinGW-w64)，统一 `-std=gnu++17 -O2 -Wall -Wextra`，汇编语法默认 **AT&T**，不开 `-masm=intel`。

## 目录

| 路径 | 内容 |
|---|---|
| [`AGENTS.md`](AGENTS.md) | 给接手这个仓库的 AI 的教学协议：角色、硬性约定、提交格式、评分口径、当前进度 |
| [`lessons/lesson01/main.cpp`](lessons/lesson01/main.cpp) | Lesson 1 作业：`bswap64_asm` / `rotl64_asm` / `mask_imm_asm` |
| [`notes/lesson01-extended-asm.md`](notes/lesson01-extended-asm.md) | Lesson 1 全部知识点：占位符、约束符、AT&T vs Intel、clobber、立即数编码 |
| [`notes/lesson02-clobber.md`](notes/lesson02-clobber.md) | Lesson 2 进度：clobber 的「承诺 + 销毁」语义（进行中） |
| [`notes/faq.md`](notes/faq.md) | 练习中提出的问题与解答 |
| [`progress.md`](progress.md) | 课程表、评分记录、易错点清单 |

## 课程进度

| # | 课 | 状态 | 最佳分（正确性/ABI/性能） |
|---|---|---|---|
| 1 | Extended Asm 基本语法与约束符 | ✅ 通过 | 88（90 / 85 / 88） |
| 2 | Clobber List：memory 屏障与 cc | 🟡 进行中 | — |
| 3–9 | 见 `progress.md` | ⬜ 未开始 | — |

## 约定

1. 每段 asm 都必须能被 `-S` 解释清楚：哪条指令来自模板、哪个寄存器是分配器挑的、哪条是编译器插的。
2. 判定对错只看 `fiber_submit` 的编译/运行/`-S` 报告，不靠肉眼读代码。
3. 只写 `-O2` 下会被真正执行的代码路径；被优化掉的分支要能说清为什么。
