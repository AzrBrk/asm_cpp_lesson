# Lesson 2 — Clobber List：memory 屏障与 cc

> 状态：🟡 进行中。目前只讲了知识点 1，任务尚未开始。

## 知识点 1：clobber 是"我弄坏了"的声明，不是"借我用一下"

| | 语义 | 编译器怎么用 |
|---|---|---|
| 操作数列表（output/input） | "给我一个位置，值我得管" | 分配寄存器/地址，保证可读或被写 |
| clobber 列表 | "这个东西我弄坏了" | 从分配候选里**剔除**，并认为 asm 之后其值**已死** |

GCC 把 clobber 项当成一条 asm 的 **def（写）**。它只做两件事：

1. 这条 asm 之前，任何活着的、恰好住在这个寄存器里的值，必须先搬走（spill 或重新分配）。
2. 这条 asm 之后，该寄存器里的值视为无意义，不再被引用。

漏写 = 告诉编译器"我没碰它" → 它让某个变量继续住在那儿 → 被你的 asm 擦掉。

### 例子

```cpp
uint64_t accumulate(const uint64_t* p, size_t n) {
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
        __asm__ volatile("movq $0, %%rbx");   // ← 漏了 ::: "rbx"
        acc += p[i];
    }
    return acc;
}
```

`-O2` 下 GCC 很愿意把 `acc` 或 `i` 放进 `%rbx`（callee-saved，循环里不用来回存）。asm 每轮把它清零，而 GCC 认为 `%rbx` 没被碰过 —— 累加器变成 0、循环死在原地。

加上 `::: "rbx"` 之后，GCC 要么把变量挪走，要么在 asm 前后 `pushq`/`movq` 存一次 —— 正确，代价就是它为你的承诺付的账（Lesson 2 的性能考点）。

### 三条直觉

1. **clobber 只管"写"，不管"读"。** 读一个没在 input 列表里的寄存器，编译器不知道；读必须走 input 操作数。
2. **clobber 不参与 `%n` 编号。** 加 `"cc"`、`"memory"`、寄存器名都不用重新数格子。
3. **硬编码寄存器必须进 clobber，走 `%n` 的操作数不要进。** 重复声明会让 GCC 认为它自己的分配都无效。

### 与 `volatile` 的分界

- `volatile`：别删我、别搬我。
- clobber：我动了什么东西。
- 两者互不替代。`asm volatile("movl %1, (%0)")` 阻止不了 GCC 把 `*p` 的读提到这条 `mov` 之前 —— 那是 `"memory"` 的活。

---

## 待讲

- 知识点 2：`"memory"` 是编译器屏障——不能把 load/store 跨过它重排或缓存到寄存器。
- 知识点 3：`"cc"` 的边界（回顾见 [Lesson 1 笔记](lesson01-extended-asm.md) 与 [faq Q1](faq.md#q1-and-又不是加法为什么会改标志位)）。
- 知识点 4：`rdtsc` —— 改 `%rax`/`%rdx`、读 TSC、写 EFLAGS，clobber 的教科书案例。

## 任务（未开始）

1. `uint64_t rdtsc_asm()` —— 正确声明 clobbers。
2. `void bad_store(int* p, int v)` —— **故意漏写 `"memory"`**，asm 里 `movl %1, (%0)`，调用前后读 `*p`，在 `-O2` 下观察读被提到写之前。
3. 分别提交，对比 `-S`：**哪一行被优化掉了，为什么**。
4. 回答：为什么 `bad_store` 在某些编译器和优化级别下"看起来是对的"，却仍是 UB？

## 遗留账（来自 Lesson 1）

- `mask_imm_asm` 的 `main` 里仍没有独立参考值比对。
- `#ifdef ERROR0` 的失败实验引用未声明的 `m`，打开只会撞 undeclared identifier。
- `"r"` 换 `"c"` 的报错实验还没做（[faq Q4](faq.md#q4待答rotl64_asm-里把-cn-换成-rn-会怎样)）。
