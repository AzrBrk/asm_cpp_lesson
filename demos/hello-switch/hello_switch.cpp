// hello_switch.cpp — 两个函数各自站在自己的栈上，交替打印 hello world
//
// 没有调度器，只有一个 ctx_switch：
//   把「当前 rsp」存进 *from，把 rsp 换成 to，恢复寄存器，ret 到对方上次停下的地方。
//
// 编译（Linux）:  g++ -std=gnu++17 -O2 -Wall -Wextra hello_switch.cpp -o hello_switch
// 编译（MinGW）:  g++ -std=gnu++17 -O2 -Wall -Wextra hello_switch.cpp -o hello_switch.exe
//
// 预期输出：
//   hello world
//   hello world
//   hello world
//   done

#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#  define ARG_FROM "%rcx"      // MS x64：第 1 个参数
#  define ARG_TO   "%rdx"      // MS x64：第 2 个参数
#else
#  define ARG_FROM "%rdi"      // SysV：第 1 个参数
#  define ARG_TO   "%rsi"      // SysV：第 2 个参数
#endif

namespace {

constexpr int kStackWords = 1 << 14;   // 每个栈 128 KiB

alignas(16) std::uint64_t g_stack_a[kStackWords];
alignas(16) std::uint64_t g_stack_b[kStackWords];

void *g_sp_main = nullptr;
void *g_sp_a    = nullptr;
void *g_sp_b    = nullptr;
void *g_sp_dead = nullptr;     // fiber 正常返回时的落脚点（本 demo 用不到）

int g_rounds = 0;

void fiber_a();
void fiber_b();
void fiber_exit();

// ── 上下文切换 ────────────────────────────────────────────────────────────
// 进来时：ARG_FROM = &from_sp，ARG_TO = to_sp
// 1) 把 8 个 callee-saved 寄存器压到「当前栈」
// 2) *from_sp = rsp          （保存当前栈顶）
// 3) rsp = to_sp             （换到对方的栈）
// 4) 弹出对方的 8 个寄存器、ret 到对方上次 call ctx_switch 的下一句
//
// 为什么只存 callee-saved：caller-saved 的值本来就由 C 代码自己负责，
// 编译器已经默认"调用完就没了"。callee-saved 才是编译器以为"调用前后不变"的那批。
// Windows 上 rsi/rdi 也是 callee-saved，所以两个平台都存这 8 个。
//
// 为什么必须 naked：普通函数有 prologue/epilogue，编译器会假设 rsp 在函数内不变；
// 这里中间就换了栈，只能用 naked（只发这一小段 asm，不生成任何额外指令）。
__attribute__((naked)) void ctx_switch(void **, void *) {
    __asm__ volatile(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %rsi\n\t"
        "pushq %rdi\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"

        "movq " ARG_FROM ", %rax\n\t"
        "movq %rsp, (%rax)\n\t"        // *from_sp = 当前 rsp
        "movq " ARG_TO ", %rsp\n\t"    // rsp = 对方的 rsp

        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rdi\n\t"
        "popq %rsi\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "ret\n\t"
    );
}

// 在 g_stack_* 顶部伪造一个「刚被 ctx_switch 切走」的栈帧：
//
//   高地址
//   ┌──────────────┐ ← top（16 字节对齐）
//   │  fiber_exit  │ ← [top-8]   fiber 若 return 就落到这里
//   ├──────────────┤ ← [top-16]  ctx_switch 的 ret 目标
//   │   fiber_x    │
//   ├──────────────┤
//   │ r15 r14 r13  │  8 个「保存寄存器」槽位
//   │ r12 rdi rsi  │
//   │ rbx rbp      │
//   └──────────────┘ ← sp = top - 80，ctx_switch 从这里开始 pop
//   低地址
//
// 第一次切进来时，ctx_switch 弹出 8 个槽位（全是 0），ret 到 fiber_x。
// 这时 rsp = top - 8，正好满足「函数入口 rsp % 16 == 8」的 ABI 约定。
void init_ctx(void **slot, std::uint64_t *stack, void (*entry)()) {
    std::uint64_t *top = stack + kStackWords;
    std::uint64_t *sp  = top - 10;                 // 80 字节 = 8 个寄存器槽 + ret 槽 + 1 个空槽

    for (int i = 0; i < 10; ++i) sp[i] = 0;
    sp[8] = reinterpret_cast<std::uint64_t>(entry);        // ctx_switch 的 ret 目标
    sp[9] = reinterpret_cast<std::uint64_t>(fiber_exit);   // fiber 的返回地址

    *slot = sp;
}

void fiber_exit() { ctx_switch(&g_sp_dead, g_sp_main); }

void fiber_a() {
    for (;;) {
        std::fputs("hello ", stdout);
        ctx_switch(&g_sp_a, g_sp_b);   // 跳到 b 上次停下的地方
    }
}

void fiber_b() {
    for (;;) {
        std::fputs("world\n", stdout);
        if (++g_rounds == 3) ctx_switch(&g_sp_b, g_sp_main);  // 3 轮后回 main
        ctx_switch(&g_sp_b, g_sp_a);
    }
}

}  // namespace

int main() {
    init_ctx(&g_sp_a, g_stack_a, fiber_a);
    init_ctx(&g_sp_b, g_stack_b, fiber_b);

    ctx_switch(&g_sp_main, g_sp_a);   // main → a；b 三轮后切回来，这行才返回
    std::puts("done");
    return 0;
}
