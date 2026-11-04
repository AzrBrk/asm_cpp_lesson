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
| 2 | `naked` 函数 | 你 | 编译器（C++ 函数边界） | Lesson 5.5、6、7 |
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
| 2 | 漏 `"memory"` 的错值、`push`/`pop` 代价 |
| 3 | 帧布局、红区、对齐填充 |
| 4 | 多输出漏 `&` 的错值 |
| 5 | 参数偏移、隐藏指针、`%al` |
| 5.5 | `nm` 符号、`objdump` 指令、`bt` 回溯 |
| 6 | 切换前后 `rsp`、运行交替输出 |
| 7 | 假帧 80 字节、参数 12345 |
| 8 | 不变量 `assert` 全过 |
| 9 | 逐行注释、栈布局图 |

**复现不了 ≠ 安全**：`&` 和 `"memory"` 能稳定复现错值，`"cc"` 不一定
（见 [Lesson 2 §3.4](lesson02-clobber.md)）—— 这是 UB 的两种面孔。

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

1. **先读教案**（手机上也行），不要求动手；
2. 开讲时按"一次一个知识点"推进，讲完一个点做一个小任务；
3. 每个任务都要贴 `-S`/`objdump`/运行结果，**没有证据不算过**；
4. 评分：正确性 40% / ABI 30% / 性能 30%，≥ 80 通过；
5. 卡住时给反例、给报错、指位置，**不给完整可编译实现**。

---

## 8. 当前状态

| 课 | 教案 | 开讲 | 任务 |
|---|---|---|---|
| 1 | ✅ 已补讲 | ✅ 已通过（88） | ✅ 完成 |
| 2 | ✅ v2 | ⬜ 未开始 | ⬜ 未开始 |
| 3–9 | ✅ v1 | ⬜ 未开始 | ⬜ 未开始 |
| 5.5 | ✅ 新增 | ⬜ 未开始 | ⬜ 未开始 |
| demo | ✅ hello-switch | — | 可观摩 |

教案是"材料"，不是"讲稿"：开讲时仍然一次只讲一个知识点，讲完停下来等代码。
