# Lesson 8 教案 — 最小调度器

> **状态：教案 v1，未开讲。**
> 前置：Lesson 6–7（切换与首次进入）。
> 本课目标：把"两个 fiber 互相跳"扩成"N 个 fiber 轮流跑"，
> 并建立一组**能被测试证明**的不变量。

---

## 0. 从两个到 N 个：先换数据结构

`hello-switch` 里是三个全局 `void *`。N 个 fiber 就不能这么写了：

```cpp
struct Fiber {
    void       *sp;        // 保存的栈顶（switch 的输入/输出）
    std::uint8_t *stack;   // 栈底（用于释放）
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

---

## 2. 调度原语

```cpp
void yield();                 // 当前 fiber 让出 CPU
void schedule();              // 选下一个，切换过去
void resume(Fiber *f);        // 直接切到指定 fiber（用于启动）
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
`ctx_switch` 在两边各"返回"一次。

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
比"悄悄踩坏别的 fiber"好定位。Windows 用 `VirtualAlloc` + `PAGE_GUARD`。

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
- 时间片（用 `rdtsc` 计，Lesson 2）；
- 阻塞（`wait` 把 fiber 移出就绪队列，`wake` 放回）；
- 抢占（信号/定时器，但那就不是协作式了）。

---

## 6. 编译器视角：全局状态会被缓存吗

问题：fiber A 改了 `g_current`，切到 B，B 又改；切回 A 时，A 看到的还是旧值吗？

答案：**不会**，因为 `yield()` 是一次普通的函数调用，编译器在调用点必须假设
"被调用者可能改了任何全局/内存"。所以 `g_current`、队列头这些会在调用后重新读。

但有两个前提：

1. `yield()` 不能被优化成"没有副作用"（它调用了不透明的函数，不会）；
2. 如果你自己写了 `asm volatile` 直接改全局，就要 `"memory"`（Lesson 2）。

真正需要注意的是 **TLS**：`errno`、`strtok` 的状态是线程级的，fiber 之间共享 ——
两个 fiber 都用 `errno` 就会互相污染。

---

## 7. 异常：别跨 fiber 抛

C++ 异常展开依赖栈上的 `.cfi`/`.seh` 信息和 `__cxa_*` 运行时。
你的 `ctx_switch` 是 naked、没有 unwind 信息，异常穿过它 = 未定义。

规则：

- fiber 函数内部自己 `try/catch`；
- 异常不许越过 `yield()`；
- 真要跨 fiber 传错误，用返回值/错误码/`std::expected`。

---

## 8. 测试与不变量

最小调度器要能被测试证明，而不是"看起来对"。建议的不变量：

1. **隔离**：每个 fiber 的局部变量互不影响（切回来值还在）。
2. **顺序**：round-robin 下，N 个 fiber 各跑一轮的顺序可预测。
3. **守恒**：`READY + RUNNING + DEAD == 总数`。
4. **不重入**：同一时刻只有一个 RUNNING（打印 `g_current->id` 验证）。
5. **栈**：每个 fiber 的 `rsp` 落在自己的栈区间内（`assert`）。

写一个 `test_scheduler()`，跑 1000 轮，最后断言计数总和。

---

## 9. 和 boost.fiber 的差距

| 能力 | 最小调度器 | boost.fiber |
|---|---|---|
| 调度策略 | RR | RR + 优先级 + 自定义 |
| 同步 | 无 | mutex/condition_variable/future |
| 栈 | 固定大小 | 可配置 + guard |
| 异常 | 不支持跨 fiber | 有定义的行为 |
| TLS | 共享 | 可切换（fiber_local） |
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

加 `wait(f)` / `wake(f)`：fiber 可以主动移出就绪队列，等另一个 fiber 唤醒。
用一个生产者/消费者跑通。

### 任务 D：栈溢出

给一个 fiber 分配 4 KiB 栈，写递归爆栈，加 guard page，确认 `SIGSEGV` 落在 guard page。
没有 guard page 时会发生什么？用 `gdb` 看。

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
