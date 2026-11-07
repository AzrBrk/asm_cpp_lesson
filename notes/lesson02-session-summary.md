# Lesson 2 学习总结与交接（截至 2026-11-07）

## 新 session 从这里开始

- 先读本文件、[进度](../progress.md) 和 [第二课源码](../lessons/lesson02/main.cpp)。
- Lesson 2 已开讲，四个主线知识点已完成；**整课未结课，不得记为通过**。
- 2026-11-07 学习者主动暂停，要求提交 GitHub，明天可能新开 session。恢复时继续下面唯一待答题。
- 继续坚持：一次一个知识点，先解释再给任务，停下来等学习者代码或口述；不要代写完整答案。
- 精确停点：已讲 caller-saved 与 callee-saved 的调用保存责任，下面的问题尚未回答。不要重讲 rdtsc 或已完成的栈推导。

**唯一待回答的问题（不要直接给答案）**：给定调用前 r10 = 123，调用者在返回后
仍需此值；栈空间和对齐假设均已正确处理。`call worker`，worker 将 r10 改成 7，
返回前不恢复。问：

1. worker 不恢复 r10 是否违反 Windows x64 ABI？
2. 返回后真实 r10 是多少？为什么恢复 rsp 不能恢复它？
3. 如果换成 worker 将 rbx 改成 7，返回前不恢复，ABI 判断有何不同？

已解释：r10 是 caller-saved，rbx 是 callee-saved；普通 C++ 调用由 GCC 识别，
asm 内隐藏的 call 不自动建立普通调用的数据流模型，memory 不声明寄存器破坏。
先核对回答，再逐点完成调用边界，禁止代写巨大 clobber 列表或完整手写 call。

## 2026-11-07 新完成内容（口述推导，无新增运行证据）

- rsp 相对寻址：入口 4096、原对象 4140；减 16 后用 44(%rsp) 写到 4124，
	相对原对象 -16 字节。恢复 rsp 不撤销写入。rsp 算术本身正确，错误在改变基址后仍用原位移。
- 提前 `leaq 44(%rsp), %rcx` 得到 4140；改变 rsp 不改变 rcx，随后 (%rcx) 仍访问 4140。
	地址正确不等于内存读写声明完整，也不等于整段临时改栈实现 ABI 安全。
- Windows x64 没有红区：当前 rsp 以下的新空间须先分配再访问，不能事后追溯保护。
	已分配且允许使用的槽不必每次再减 rsp；页面可写不等于 ABI 保证内容保留。
- shadow space：调用者预留 4 个 8 字节槽，即 32 字节；无参数普通调用也不能省。
	不要求调用者提前复制四个寄存器，被调用者按需使用，调用者不可在此保留跨调用活值。
- 基址误用已纠正：调用前 rsp = 4096，入口 rsp = 4088；入口 8(%rsp) = 4096，
	不是 4104。第三槽 4112 相对入口 +24、相对调用前 +16。
- 调用前对齐：call 前 rsp % 16 = 0；压入返回地址后入口余数 8 正常。
	从入口 4088 无其他操作时减 40 = 4048，其中 32 字节 shadow space + 8 字节填充。
	不能背“永远减 40”；先 push rbx 到 4080 后，减 32 = 4048 合规，减 40 = 4040 不对齐。
- 对照题措辞曾导致把减 32、减 40 串行计算，已澄清为两条独立路径。草稿中的 4008
	是历史连续路径，不是独立减 40 的正确答案；保留学习者草稿，不替其修改。
- 最后布局题四项均正确：保存 rbx 值 4080–4087，shadow space 4048–4079，
	下次被调用者入口 4040、返回地址 4040–4047；保存值不在 home slots 内。
- 所有片段仅用于布局推导，不是完整安全实现；不据口述题给整课实现评分或宣布通过。
- 本次归档包含第二课源码、学习者 draft.md、根目录 main.s 及 .vscode 配置；main.s
	是已有的生成汇编，本次未重新编译，不作为今天新增实测。exe 和 build 目录留在本机并忽略。
- .vscode 当前 IntelliSense / Runner 使用 C++23，且有本机绝对路径；配置保留原样，
	课程验收仍须显式使用 -std=gnu++17 -O2 -Wall -Wextra，不能据编辑器配置改变验收标准。

官方依据：
- [GCC Extended Asm](https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html)：模板不解析、栈指针前后相同及 clobber 合同。
- [Microsoft x64 stack usage](https://learn.microsoft.com/en-us/cpp/build/stack-usage?view=msvc-170)：rsp 以下空间及 home space 归属。
- [Microsoft x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170)：shadow space、对齐及寄存器保存责任。

## 环境与 2026-11-06 的实测证据

- Windows x64，Microsoft x64 ABI；MinGW-w64 GCC 16.2.0，目标 `x86_64-w64-mingw32`。
- 使用 `-std=gnu++17 -O2 -Wall -Wextra`，AT&T 语法，未启用 `-masm=intel`。
- 代理实际读取源码并编译、运行、生成汇编；默认及 `-DUSE_INCL` 分支均编译无警告、运行退出码 0。
- 验证产物放在系统临时目录，没有替学习者修改作业源码。
- 下列输出是本 session 的实测，不是教案历史输出：

```text
bad_store output: 198
good_store output: 100
precise_store output: 100
increment_store output: 21, d after increment: 11
rdtsc_asm output: 000043B18BA69D19
```

时间戳只表示某次读取值，不是固定参考值；未验证计时精度、跨核同步或严格递增。

## 已完成的知识点

### 1. 寄存器 clobber 与 ABI

- `ret_test` 硬编码 `movq $1, %%rbx`，正式版本声明 `"rbx"`。
- 实测 GCC 在模板外生成 `pushq %rbx` / `popq %rbx`，返回值由 `leaq 1(%rcx), %rax` 计算。
- 学习者另提交漏 clobber 的汇编，确认没有保存恢复；返回值仍可正常，但调用者的 `rbx` 被改成 1。
- 已纠正：保存的是**调用者进入函数前的 rbx 值**，不是被调用函数独有的变量。
- 不要求强迫 GCC 将某个 C++ 活值分配到 rbx 才能证明 ABI 违约。

### 2. 内存声明与精确操作数

- `"r"(p)` 只声明地址值输入，不声明 `*p` 的内存读写；即使换成 `"+r"(p)` 也不会自动覆盖所指对象。
- `bad_store`：旧值读取、隐藏 store、再次读取放在同一函数中；GCC 将两次读取合并为一次并放在 asm 后，随后 `addl %eax, %eax`，实测 198。
- `good_store` 增加 `"memory"`：asm 前读取旧值，asm 后重新读取新值，实测 100。
- `precise_store` 用 `"=m"(*p)`，模板直接引用 `%0`，无需额外括号或 memory clobber；本次函数指令与 good_store 相同。
- asm 只写对象用 `=m`；asm 需要旧值并写回用 `+m`。函数外层 C++ 的读取不要求 asm 操作数使用 `+`。
- `"memory"` 描述可能读写操作数未覆盖的内存，不是内存一定改变，不强制所有局部值写回。
- 同样指令不能证明两种声明范围相同或在大函数中代价相同。

### 3. 条件码 clobber

- `increment_store` 使用 `"+m"(*p)` 与 `"cc"`，支持默认 addl 和 `-DUSE_INCL` 的 incl；两者都实测返回 21、对象值变成 11。
- 学习者已确认 `"cc"` 是**破坏声明**，不是保存请求或结果输出。
- incl 保持 CF，不代表不改其他条件码，也不代表 GCC 会将 CF 提供给后续 C++。
- PF 取决于结果低 8 位中 1 的数量；内存原值为 10 不代表执行前 PF 已反映 10。
- 教案查证的是 GCC 13 x86 隐式 flags clobber，**没有查证本机 GCC 16.2 后端实现**。不要求复现漏 cc 错值。

### 4. rdtsc 双输出与拼接

- 源码使用本机 32 位 unsigned lo/hi，分别 `"=a"` / `"=d"`，返回 `(uint64_t(hi) << 32) | lo`。
- 高 32 位在 edx，低 32 位在 eax；学习者曾口误为 ebx，已纠正。
- rdtsc 不修改 EFLAGS，不访问普通内存；本实验不需要 cc 或 memory，不将输出寄存器重复列为 clobber。
- 实测模板外为 `salq $32, %rdx`、`movl %eax, %eax`、`orq %rdx, %rax`。OR 是按位或；32 位写入会清零对应 64 位寄存器高半部。
- 单个 uint64_t 的 `=a` 输出不能取得 edx 中的高位；不要使用 `=A` 代替双输出。
- 学习者已确认：`hi << 32` 的计数等于提升后左操作数位宽，是 UB，与 hi 是否为 0 无关；必须先转换再移位，不能事后转换。
- 可建议将 unsigned 改为 uint32_t 以直接表达宽度，但不要由代理代改；输出变量初始化不是必需的。

## 已讲但尚未完成整课验收的边界

- rdtsc 不是序列化指令；读取计数器不等于可靠计时。TSC tick 不等同当前核心周期。
- 学习者已回答：memory clobber 是给编译器看的，不会自动生成 CPU fence。
- 已纠正：这不能直接等同 std::memory_order，后者是 C++ 原子内存顺序语义。
- 已引入 extended asm 前后 rsp 必须相同的合同；memory / rsp clobber 不能解除它。
- 仅恢复 rsp 不足以证明内部栈操作安全；相关地址推导已在 2026-11-07 完成。

## 剩余内容与验收

按 [教案](lesson02-clobber.md) 逐点完成，不能一次倒完：

1. 先核对顶部 r10 / rbx 待答题；rsp 寻址、无红区、shadow space 和对齐已完成，不重讲。
2. 完成调用 ABI 边界：普通调用交给 C++，不要凭整数 clobber 列表拼手写 call；尚需参数寄存器读写声明、向量寄存器及 unwind 边界。GCC 13 不识别 redzone 是历史结论，不推广到 GCC 16.2。
3. 完成教案任务 C：构造有/无 memory 屏障的循环，实测优化差异并数指令。当前 precise_store 对照**不能替代**这项代价实验。
4. 补齐必要的整课问答和指令文档依据；核对后给整课三项评分，再更新进度为通过。

## 本次易错点与维护提醒

- 学习者最初向 int 对象使用 movq，越过 4 字节对象边界，现已改 movl。漏内存声明与越界是两件独立问题。
- 最初误把 `addl %eax, %eax` 算作第二次内存读取，已纠正。
- 曾把写入位置说成 APP 后，已纠正为 APP 内写、NO_APP 后读。
- `rcx` 保存地址数值；调用前 `leaq 44(%rsp), %rcx` 得到调用者的对象地址。call 改变 rsp，不能将该地址继续称为被调用函数当前 rsp+44。
- 学习者已把 increment_store 调用与 printf 拆为两个语句；C++17 实参求值先后未指定，之前先读 d 导致打印旧值本身不是 UB。
- 源码保留 bad_store 是**故意违反 asm 内存契约的反例**，不可拿整个程序作为完全安全的参考实现。运行出 198 不代表接口合法。
- 当前没有独立 assert 验证；本次靠运行输出和汇编核对，不声称已有自动化测试套件。
- 本 session 给过小任务评分（大多 100，rdtsc 98.5），不是整课评分。rdtsc 扣分对应本次冗余零扩展，不应要求学习者为 GCC 生成指令手工修补。
- README、course-map、教案中“Lesson 2 未开讲”是旧快照；最新状态以 progress、本总结和已更新的 AGENTS 入口为准。