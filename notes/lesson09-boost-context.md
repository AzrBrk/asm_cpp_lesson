# Lesson 9 教案 — 用自己的模型读 `boost.context`

> **状态：已重写为 MinGW 主线，未开讲。**
> 前置：Lesson 6 的保存合同、Lesson 7 的 trampoline、Lesson 8 的生命周期测试。
> 目标不是背源码，而是回答：**每个字段谁写、谁读，哪个 ABI 条款要求它存在？**

---

## 0. 先固定版本与配置

本文固定 **Boost 1.86.0** 的代码供阅读，不跟随 `develop` 漂移。
这不是建议新工程安装旧版本，更不是对其所有可选分支的正确性背书。

| 主线/对照 | 保存与恢复 | 首次构造 |
|---|---|---|
| **MS x64 / PE / GAS，主线** | [jump_x86_64_ms_pe_gas.asm](https://github.com/boostorg/context/blob/boost-1.86.0/src/asm/jump_x86_64_ms_pe_gas.asm) | [make_x86_64_ms_pe_gas.asm](https://github.com/boostorg/context/blob/boost-1.86.0/src/asm/make_x86_64_ms_pe_gas.asm) |
| SysV / ELF / GAS，对照 | [jump_x86_64_sysv_elf_gas.S](https://github.com/boostorg/context/blob/boost-1.86.0/src/asm/jump_x86_64_sysv_elf_gas.S) | [make_x86_64_sysv_elf_gas.S](https://github.com/boostorg/context/blob/boost-1.86.0/src/asm/make_x86_64_sysv_elf_gas.S) |

先读普通保存路径：不定义 `BOOST_USE_TSX`；SysV 对照先排除影子栈、ILP32，
TLS guard 分支单独标注。实际构建若启用其他宏，必须先看预处理输出再推偏移。
`.asm` 是文件后缀，不代表 Intel/MASM 语法；这里 `ms_pe_gas.asm` 的内容是 GAS/AT&T。
本文不复制整份实现，直接在上游源码旁做自己的逐行笔记。

## 1. 开讲拆分

| 顺序 | 这次只解决一个问题 | 交什么后停 |
|---|---|---|
| 1 | `transfer_t` 为何让 MS 参数整体后移 | C++ 签名→隐藏返回指针→`rcx/rdx/r8` 对照 |
| 2 | 保存区每一块是什么 | 从所有 store/load 提取偏移、宽度、读写方 |
| 3 | 换 `rsp` 后返回给谁 | A→B→A，分别标旧 fctx、目标 fctx、payload |
| 4 | `make` 如何制造同一种恢复现场 | 从 `top` 算到 H、trampoline、用户入口，核对 shadow space |
| 5 | 与自己的原语做差异表 | 区分必需 ABI、可选配置、运行时支持，不只数代码行 |

SysV 对照放到主线之后。缺某项前置就回对应课，不在本课重讲完整指令表。

---

## 2. 接口：fctx 到底指谁

阅读接口定义：[detail/fcontext.hpp](https://github.com/boostorg/context/blob/boost-1.86.0/include/boost/context/detail/fcontext.hpp)。
需要识别的类型形状：

```cpp
using fcontext_t = void*;     // 本实现中表示可恢复现场的地址；对使用者是 opaque handle
struct transfer_t {
    fcontext_t fctx;          // 刚刚切走的来源上下文，不是当前目标
    void* data;              // 这次切换传来的 payload
};
// 只作接口阅读，不是课程作业实现
transfer_t jump_fcontext(fcontext_t to, void* data);
```

若 A 调用 `jump_fcontext(B, p)`：

- **B 收到的** `fctx` 是刚保存的 A，`data` 是 p；
- B 以后用这个 A 句柄切回，A 的旧调用才返回；
- A 得到的是“切回它的那个执行流”的句柄与那次 payload，不是原调用的参数副本。

不要把 `transfer_t.fctx` 注释成“目标上下文”，否则后续调度方向会全反。

### 2.1 隐藏返回缓冲区改变了 MS 参数位置

`transfer_t` 是两个 64 位指针，16 字节。普通 MS x64 不用 `rax+rdx` 返回该结构体，
而由调用者分配缓冲区、传隐藏指针：

| 逻辑项目 | MS x64 入口 | SysV LP64 入口 |
|---|---|---|
| 隐藏结果缓冲区 | `rcx` | 本例不需要 |
| `to` | `rdx` | `rdi` |
| `data` | `r8` | `rsi` |
| 返回结果 | 写缓冲区，`rax` 返回缓冲区地址 | `rax=fctx`，`rdx=data` |

所以“Windows 切换参数在 rcx/rdx”只适用于我们自己的 `void ctx_switch(void**,void*)`，
**不适用于这个带结构体返回的签名**。
主线第一项练习：先独立写一个普通 C++ 16 字节结构体返回函数，观察 MinGW `-S`，
再读汇编，不用源码注释猜 ABI。

---

## 3. MS 保存区：从实际偏移提取表

令 `S` 是 `jump_fcontext` 的入口 `rsp`。代码向下留 `0x118` 字节，
因此 `H = S - 0x118` 是普通保存区起点。
入口 `S%16=8`，`0x118%16=8`，所以 **H 为 16 对齐**。

以下来自固定版本的普通路径；地址以 H 为基准：

| 偏移/字节数 | 内容 | 为什么有它 |
|---|---|---|
| `0x000 … 0x09f` / 160 | `xmm6 … xmm15`，每个 16 字节 | MS 非易失 SIMD；每个 `movaps` 地址要对齐 |
| `0x0a0` / 4 | MXCSR | 保存浮点控制/状态的实现策略 |
| `0x0a4` / 2 | x87 control word | 浮点控制环境 |
| `0x0a6 … 0x0af` / 10 | 布局间隙 | 不当作有效状态解读 |
| `0x0b0` / 8 | 上游称 fiber storage 的 TIB 字段 | 平台上下文状态，不等于全部 C++ TLS |
| `0x0b8` / 8 | deallocation stack | Windows 栈管理相关状态 |
| `0x0c0` / 8 | stack limit | 栈下界 |
| `0x0c8` / 8 | stack base | 栈上界 |
| `0x0d0/0x0d8/0x0e0/0x0e8` / 各 8 | `r12/r13/r14/r15` | 非易失 GPR |
| `0x0f0/0x0f8/0x100/0x108` / 各 8 | `rdi/rsi/rbx/rbp` | 非易失 GPR |
| `0x110` / 8 | 当前调用的隐藏结果缓冲区指针 | 恢复时必须写回这个调用者自己的缓冲区 |
| `0x118` / 8 | 原 call 留下的返回地址 | 不在向下分配的 `0x118` 字节里；它原本就在 S |

检查表的办法：对每个保存 store，找到目标恢复 load；看**宽度**而不是只看偏移。
`stmxcsr` 写 4 字节、`fnstcw` 写 2 字节，不要把每行都画成 8 字节寄存器槽。
TEB/TIB 偏移是本实现的平台细节，不应未经文档/目标环境核查就复制进自己的通用库。

### 3.1 切换之后，哪些值仍代表来源

上游先把来源 H 放进 `r9`，再从 `rdx` 取目标 H 装入 `rsp`。
这时同一条 `mov offset(%rsp), reg` 已在读取**目标现场**。
最后读取目标保存的隐藏缓冲区地址：

- 缓冲区 `[+0]` 写来源 `r9`，作为结果 `fctx`；
- 缓冲区 `[+8]` 写此次 `r8`，作为结果 `data`；
- `rax` 指向该缓冲区；`rcx` 也设为它，供首次用户入口的聚合参数传递使用；
- 读取目标返回地址、把 `rsp` 推进到原 call 之后，再间接跳转。

此时“指针指向谁”比指令名字重要：恢复了目标寄存器，不代表来源句柄也要改成目标。

---

## 4. MS 首次构造：`0x150` 不是随手留的大空间

令 `T` 为向下取整到 16 字节边界的栈顶；`make_fcontext` 返回
`H = T - 0x150`，即 336 字节初始区域。
主体字段与上表对应，但几个恢复值为首次运行专门安排：

| 地址 | 初始用途 |
|---|---|
| `H+0x100` | 恢复到 `rbx` 的用户入口函数地址 |
| `H+0x108` | 恢复到 `rbp` 的 finish 地址 |
| `H+0x110` | 指向 `H+0x140` 的隐藏缓冲区地址 |
| `H+0x118` | 首次恢复目标 trampoline |
| `H+0x120 … H+0x13f` | 32 字节参数 home area |
| `H+0x140 … H+0x14f` | 首次接收的 `transfer_t` |

按恢复路径手算：

1. 换栈与恢复完成，跳到 trampoline 时 `rsp=H+0x120`，为 **0 mod 16**。
2. trampoline 压入 `rbp` 中的 finish 地址，`rsp=H+0x118`，为 **8 mod 16**。
3. 跳到 `rbx` 中的用户函数，模拟普通入口；其 home slots 恰在
   `[H+0x120,H+0x13f]`，没有越过 T。
4. 用户函数的 `transfer_t` 参数是按 MS 聚合参数规则间接传入，地址在 `rcx`。
5. 用户若返回，落到 finish。该低层实现最终调用 `_exit`，会结束进程，
   **不是自动把任务标 DEAD 并回到我们的调度器**。

高层 Boost.Context/Fiber 包装层负责其正常生命周期；不能把裸 `make_fcontext` 的
finish 行为直接当成调度器销毁协议。自己的 trampoline 应遵循 Lesson 7 的明确完成路径。

---

## 5. 再读 SysV 对照：不要混合两张偏移表

不启用影子栈的 LP64 路径：`jump` 向下留 64 字节，原返回地址在 `+0x40`；
`make` 留 `0x48`（72）字节，包括该首次恢复目标。

| SysV 偏移 | 内容 |
|---|---|
| `+0x00` / 4，`+0x04` / 2 | MXCSR、x87 CW |
| `+0x08` / 8 | 可选 TLS guard 槽，仅启用相关宏时读写 |
| `+0x10/+0x18/+0x20/+0x28` | `r12/r13/r14/r15` |
| `+0x30/+0x38` | `rbx/rbp`；make 中分别携带 fn/finish |
| `+0x40` | 返回地址/首次 trampoline 目标 |

SysV 的 16 字节双指针结果在 `rax/rdx`，首次用户参数在 `rdi/rsi`；
没有 MS 的隐藏缓冲区与 32 字节 home area，也不要求保存 `rsi/rdi`。
同线程最小切换并不自动需要替换 TLS guard；源码中的可选 guard 分支，
不能被概括为“所有切栈都要改 canary”。

固定偏移 + `mov` 便于布局控制，不证明比 push/pop 指令更少或更快；
性能比较必须在相同保存能力下测量。

## 6. CET、unwind、ontop：知道边界，不伪造能力

- **CET**：MS 主线这份文件没有因为使用 jmp 就自动支持所有 CET。
  SysV 的可选影子栈分支会改变现场与恢复协议，本文普通路径偏移不适用。
  `jmp` 不能单独修好后续 ret 的影子栈匹配；也不要混淆 shadow stack 与 IBT。
- **unwind**：看到 `.seh_proc` 或 `.cfi` 不代表可以把异常跨栈展开。
  应看元数据实际描述了什么；本课程保持“错误在各自入口内收口”的边界。
- **ontop_fcontext**：恢复目标栈后，先运行回调，再接续目标。
  用于目标栈上的受控工作，但**不能释放正在执行回调的那根栈**。
  高层销毁协议要切到另一根安全栈再释放；不能用“ontop 能释放目标栈”一句话略过所有权。
- **TLS**：fiber-specific storage 是额外设施，不表示任意 `thread_local`、
  `errno` 和全部运行时 TLS 都自动随 fiber 切换。

## 7. 任务与验收

| 任务 | 提交物 | 通过条件 |
|---|---|---|
| A：返回合同 | MinGW 的普通 C++ 16 字节结构体返回 `-S` + MS jump 参数图 | 识别隐藏 `rcx`、目标 `rdx`、payload `r8`；不是套我们原语签名 |
| B：保存布局 | 自己从主线 store/load 整理的字段表 | 每个偏移/宽度对应；`0x118` 既是分配量也是入口返回地址偏移，能解释为何 |
| C：首次进入 | `T→H→trampoline→fn→finish` 地址与余数账本 | home area/结果缓冲区都在界内；不混入 SysV 的 72 字节 |
| D：能力差异 | 自己原语与主线实现的差异表 | 分开 GPR、SIMD、控制字、TEB、参数、退出、unwind、CET，不说“都是存 rsp” |

不要求复制 Boost 源码进仓库，不以背行数、抄注释评分。
选做：给自己的切换原语增加单个 `void*` payload 返回，先自行规定 ABI 与首次进入协议；
修改后重新执行 Lesson 7/8 的参数与生命周期测试。

## 8. 本课自测与课程收束

1. 为什么两参数 `jump_fcontext` 在 Windows 中要读 `rdx/r8`？
2. A 切到 B，B 收到的 `transfer_t.fctx` 指谁？
3. `0x118`、`0x120`、`0x150` 各代表哪一层地址账？
4. MS 和 SysV 的 `transfer_t` 参数/返回分别在哪里？
5. 为什么保存 8 个 GPR 不够，为什么使用 jmp 也不证明 CET 安全？
6. 用户入口正常返回时，低层 finish 与调度器 DEAD 回收有何区别？

完成本课的标准是：能把自己的每个保存字段连回 ABI 条款，
能把每个恢复地址连回一次真实调用或首次构造；
**未提供编译/运行/反汇编证据的能力，继续标“未验证”**。课程进度不因教案补全而自动通过。
