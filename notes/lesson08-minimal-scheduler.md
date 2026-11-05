# Lesson 8 教案 — 最小调度器

> **状态：已补队列合同、边界与测试矩阵，未开讲。**
> 前置：Lesson 6–7（切换与首次进入）。
> 本课目标：把"两个 fiber 互相跳"扩成"N 个 fiber 轮流跑"，
> 并建立一组**能被测试证明**的不变量。

---

## 开讲拆分

| 顺序 | 小任务 | 停点与证据 |
|---|---|---|
| 1 | 不切栈，先手工模拟三个状态/队列 | 每步 READY 队列、current、DEAD 集合，一项不重复 |
| 2 | 接上已通过 Lesson 6–7 的原语 | A/B/C 各 3 轮，严格核对轨迹，不只看最后总和 |
| 3 | 处理仅剩自己、队列空、正常结束 | 退出策略与栈回收证据，不出现空指针切换 |
| 4 | 压力测试 | 1000 轮、每 fiber 独立局部状态、每次稳定点检查不变量 |

blocking、guard page、计时后置；不要在本课引入无锁队列、多线程与抢占。
`Fiber` 控制块地址必须稳定：第一版固定数组或独立分配，
不能让 `vector` 扩容后队列指针、`&f->sp`、入口参数全部失效。

---

## 0. 从两个到 N 个：先换数据结构

`hello-switch` 里是三个全局 `void *`。N 个 fiber 就不能这么写了：

```cpp
struct Fiber {
    void       *sp;        // 保存的栈顶（switch 的输入/输出），不是 malloc 返回的基址
    std::uint8_t *stack;   // 分配基址（用于释放）；另需记录可用栈上下界/保护页
    std::size_t  stack_size;
    int          id;
    enum State { READY, RUNNING, DEAD } state;
};

Fiber  g_fibers[N];
Fiber *g_current;          // 当前正在跑的
Fiber *g_ready[N];         // 就绪队列（环形或链表）
```

**关键变化**：`ctx_switch` 的第一个参数从 `&g_sp_a` 变成 `&f->sp` ——
切换目标不再写死，而是"调度器算出来的下一个 fiber"。

---

## 1. 状态机

```
           create
             │
             ▼
          READY ──resume──▶ RUNNING
             ▲                 │
             │                 ├─ yield ──▶ READY（回队列尾）
             │                 │
             └── schedule ◀────┤
                               └─ return ──▶ DEAD（不再入队）
```

三条规则：

1. **同一时刻只有一个 RUNNING**（单线程）；
2. **READY 按队列顺序被选中**（round-robin）；
3. **DEAD 不再被调度**，它的栈可以回收 —— 但不能在它自己的栈上回收。

这里的检查针对一次调度事务完成后的**稳定点**；中途先写 READY 再写 RUNNING 的
几行代码不一定满足“恰有一个 RUNNING”。本课无并发，不要在半更新状态作断言。

---

## 2. 调度原语

```cpp
void yield();                 // 当前 fiber 让出 CPU
void schedule();              // 选下一个，切换过去
// 第一版只从就绪队列选择目标，不公开绕过队列任意 resume 的入口
```

最小心智模型：

```
yield():
    cur->state = READY;
    把 cur 放回就绪队列尾;
    schedule();               // 可能切到别人；以后被切回来时，从下一行继续

schedule():
    next = 就绪队列头;
    next->state = RUNNING;
    g_current = next;
    ctx_switch(&prev->sp, next->sp);   // 切走；以后回到这里时，prev 已经又是 RUNNING
```

**最容易绕晕的一点**：`schedule()` 是"切走"，也是"切回"。
同一行代码，对 A 来说是"我让出"，对 B 来说是"我恢复" —— 因为每个执行流有自己的栈，
A 的旧调用在恢复 A 时返回，B 的旧调用在恢复 B 时返回；
不是同一个 C++ 调用在同一帧中返回两次。

### 2.1 边界行为先规定，不能写到崩溃再补

本课采用上面的**直接切到下一个**模型，第一版 main 也参与 RR，拥有系统栈、不拥有堆栈分配：

| 情况 | 第一版约定 |
|---|---|
| 队列只有自己（yield 前无其他 READY） | 不切换，自己仍 RUNNING，不生成重复队列项 |
| 有其他 READY | 当前入尾，目标从头出队；目标必须 READY 且不在队列内重复 |
| fiber 完成 | 当前标 DEAD，不再入 READY；切到下一个有效上下文 |
| 当前 DEAD 且无 READY | 明确终止/报调度状态错误，不向空指针切换 |
| 最后一个工作 fiber 完成 | main 必须仍可恢复；回 main 后检查所有工作 fiber DEAD |
| main 结束 | 仅当所有工作 fiber 完成并回收后结束，不作为普通 DEAD 工作任务入队 |

每次切换前，把 `prev` 保存在当前栈上的局部指针中，再更新 `g_current=next`。
恢复后不要假定 `next` 仍是全局当前值；它可能经历过多次切换。
回收列表只交给**已经运行在另一栈上**的执行流处理；状态 DEAD 本身不足以证明已离开那根栈。

---

## 3. main 也是一个 fiber

`main` 不是特殊公民：它也有 `rsp`。所以调度器初始化时要把 main 包成一个 Fiber：

```cpp
g_main.sp = nullptr;          // 第一次 switch 时由 ctx_switch 填
g_main.state = RUNNING;
g_current = &g_main;
```

这样 `yield()` 在 main 里调用也能工作 —— 否则"主协程"要写一套特判。

---

## 4. 栈的生命周期

| 阶段 | 做什么 | 注意 |
|---|---|---|
| create | 分配栈 + 伪造帧（Lesson 7） | 对齐、大小 |
| run | 正常使用 | 溢出检测（guard page） |
| return | 进入 `fiber_exit` | 不能在自己的栈上释放 |
| reap | 由调度器或 main 释放 | 确保它已经不在 RUNNING |

**guard page**：`mmap` 一页 `PROT_NONE` 放在栈底（低地址），溢出时立刻 `SIGSEGV`，
比"悄悄踩坏别的 fiber"好定位。Windows 的 `PAGE_GUARD` 是一次性告警；确定性越界实验可使用低端 `PAGE_NOACCESS`，
不要把两者等同，也不要承诺一个 guard page 能挡住跳过整页的大跨度访问。

---

## 5. 调度策略

第一版只做 round-robin：

```
就绪队列：A B C
A 跑 → yield → 队列变 B C A
B 跑 → yield → 队列变 C A B
```

以后可以加：

- 优先级（多条队列，先跑高优先级）；
- 协作式预算（在 yield 检查时间）；仅计时不会自动打断不 yield 的任务；
- 阻塞（`wait` 把 fiber 移出就绪队列，`wake` 放回）；
- 抢占（信号/定时器，但那就不是协作式了）。

---

## 6. 编译器视角：全局状态会被缓存吗

问题：fiber A 改了 `g_current`，切到 B，B 又改；切回 A 时，A 看到的还是旧值吗？

若边界是**不透明、可能修改相关内存**的普通函数调用，编译器不能假定相关可达内存未变。
但不能说“任意普通函数都改所有内存”，更不能保证 `-S` 中一定重新 load：
别名分析、内联、LTO、可见实现、函数属性都会改变结论。

但有两个前提：

1. 初版把 `.S` 切换原语单独编译，不开 LTO、不标 `pure/const`，不伪造无内存副作用的声明；
2. 如果你自己写了 `asm volatile` 直接改全局，就要 `"memory"`（Lesson 2）。

真正需要注意的是 **TLS**：`errno`、`strtok` 的状态是线程级的，fiber 之间共享 ——
两个 fiber 都用 `errno` 就会互相污染。

---

## 7. 异常：别跨 fiber 抛

C++ 异常展开依赖栈上的 `.cfi`/`.seh` 信息和 `__cxa_*` 运行时。
手写跨栈原语没有自动建立“另一根栈就是我的 C++ 调用者”的关系。
即使存在 `.cfi/.seh`，也不能据此允许跨 fiber 抛异常。

规则：

- 在用户入口的同栈外层捕获所有逃逸异常；记录失败并走正常完成路径；
- 不把异常展开到 trampoline 或另一个 fiber；初版不在活跃 catch/展开期间 yield；
- 普通函数先 yield、恢复后再在同一栈抛并捕获，不等于“异常穿越切换原语”；
- C++17 用错误码、结果对象或保存的 `std::exception_ptr` 传给 main。
  `std::expected` 是 C++23，不是本课程 `gnu++17` 可用的默认方案。

---

## 8. 测试与不变量

最小调度器要能被测试证明，而不是"看起来对"。建议的不变量：

1. **隔离**：每个 fiber 的局部变量互不影响（切回来值还在）。
2. **顺序**：round-robin 下，N 个 fiber 各跑一轮的顺序可预测。
3. **守恒**：`READY + RUNNING + DEAD == 总数`。
4. **不重入**：同一时刻只有一个 RUNNING（打印 `g_current->id` 验证）。
5. **栈**：每个 fiber 的 `rsp` 落在自己的栈区间内（`assert`）。

写一个 `test_scheduler()`，跑 1000 轮，最后断言计数总和。

但总和只是代理指标，不能发现“A 多跑一次、B 少跑一次”。最小验收矩阵：

| 输入/场景 | 必须断言的输出 |
|---|---|
| 0 个工作 fiber | main 无需切换即可正常结束，无悬空分配 |
| 1 个工作 fiber，1000 轮 | 每轮恢复局部值正确；不重复入队；完成后 main 可继续 |
| A/B/C 各 3 轮 | 轨迹恰为 `A0,B0,C0,A1,B1,C1,A2,B2,C2`（只记录工作任务） |
| A 1 轮、B 3 轮、C 2 轮 | 每个独立计数正确，DEAD 之后轨迹不再出现其 id |
| 每个稳定点 | READY 各出现一次；RUNNING 不在 READY 队列；DEAD 不在队列 |
| 栈局部哨兵跨 yield | 保存各自局部变量地址、前后内容与区间；不靠读取到的是巧合常数 |
| 回收 | 分配数=回收数；回收动作的当前栈不是被回收栈；之后不恢复该上下文 |

上面三任务等长轨迹就是 **9 项**，不是把 9 项序列再重复三次。
主栈不属于工作任务数组分配，单独处理区间与所有权。
若扩展 BLOCKED，守恒式改为 `READY+RUNNING+BLOCKED+DEAD=总数`。
栈范围比较采用明确的平台地址表示与有效边界，不对不相关 C++ 对象做可移植性假设。

---

## 9. 和 boost.fiber 的差距

| 能力 | 最小调度器 | boost.fiber |
|---|---|---|
| 调度策略 | RR | RR + 优先级 + 自定义 |
| 同步 | 无 | mutex/condition_variable/future |
| 栈 | 固定大小 | 可配置 + guard |
| 异常 | 不支持跨 fiber | 有定义的行为 |
| TLS | 线程本地变量共享 | 另有 fiber-specific storage；不自动虚拟化全部 C++ `thread_local`/`errno` |
| 抢占 | 无 | 无（协作式） |

看清差距，才知道 Lesson 9 为什么要读 boost：它把"能用"变成"可移植、可调试、符合标准"。

---

## 10. 任务

### 任务 A：三 fiber RR

实现 `Fiber` 结构 + 就绪队列 + `yield/schedule`，三个 fiber 各打印自己的 id 和轮次，
跑 3 轮后回 main。

### 任务 B：不变量测试

给任务 A 加 `assert`：局部变量隔离、计数守恒、`rsp` 在栈区间内。

### 任务 C：blocking

选做：先增加 BLOCKED 状态。`wait` 只阻塞当前工作 fiber，`wake` 只将 BLOCKED 入 READY 一次。
规定重复 wake 如何处理，并在队列为空但尚有 BLOCKED 时明确报告死锁，
不是假装“所有任务结束”。用生产者/消费者验证唤醒与计数，无并发原子操作。

### 任务 D：栈溢出

选做：沿用 Lesson 6 的独立故障进程与保护页方法，4 KiB 不是运行库安全栈大小保证。
不要移除保护页后继续让任意越界写污染当前练习进程；分析可能后果即可。

---

## 11. 易错点

1. `yield()` 里保存的是"当前 fiber 的 sp"，不是"下一个的"。
2. 把 main 忘了包成 fiber。
3. DEAD fiber 还在队列里被调度。
4. 在自己的栈上释放自己的栈。
5. 以为 `errno` 是 fiber 私有的。
6. 异常穿过 `yield()`。
7. 队列操作不是 O(1)（链表/环形都行，别每次扫数组）。
8. 没有测试，只靠打印"看起来对"。

---

## 12. 自测

1. `yield` 和 `schedule` 的分工？
2. 为什么 main 也要是一个 Fiber？
3. `ctx_switch` 在 A 和 B 两边各"返回"一次，怎么理解？
4. 全局变量跨切换会被缓存吗？为什么？
5. 为什么异常不能跨 fiber？
6. guard page 放在栈的哪一端？

---

## 13. 和后续课的关系

| 课 | 关系 |
|---|---|
| Lesson 9 | 把本课的"能用"对照 boost 的"可移植" |
| 之后 | 协程库、异步 IO、用户态线程都从这里长出来 |
