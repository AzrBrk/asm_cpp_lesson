// Lesson 1 — Extended Asm 基本语法与约束符
//
// 三个函数全部用内联汇编实现，不调用库函数：
//   1. bswap64_asm   —— bswap，"+r" 让输入输出绑死在一个寄存器
//   2. rotl64_asm    —— rol，计数只能来自 %cl，故用 "c"(n) + %b 修饰符
//   3. mask_imm_asm  —— and 的立即数形式 vs 寄存器形式对照
//
// 编译：g++ -std=gnu++17 -O2 -Wall -Wextra main.cpp -o main.exe
// 看汇编：g++ -std=gnu++17 -O2 -S main.cpp -o main.s
// 判定标准：-O2 下的结果正确 + clobber 完整 + 没有多余的寄存器搬运

#include <cassert>
#include <cstdint>
#include <cstdio>

// %0 = x（读改写），bswap 原地反转 8 字节
static inline uint64_t bswap64_asm(uint64_t x) {
    __asm__ volatile(
        "bswap %0"
        : "+r"(x)
        :
    );
    return x;
}

// %0 = a（读改写），%1 = n（"c" 约束钉在 %rcx 家族）
// rol 的计数只接受 imm8 或 %cl，所以用 %b1 取低字节名 %cl
static inline uint64_t rotl64_asm(uint64_t a, unsigned n) {
    __asm__ volatile(
        "rolq %b1, %0"
        : "+r"(a)
        : "c"(n)
        : "cc"
    );
    return a;
}

// 对照实验：
//   第一段 "i" → 立即数被编进指令，-S 里是 and $255, %rdx 一条
//   第二段 "r" → 常量先物化进 GPR，-S 里是 movl $255,%eax + and %rax,%rdx 两条
// 注意 "r"(0xFFu) 会因 unsigned 是 32 位而展开成 %eax，与 andq 的 q 后缀冲突，
// 所以这里必须用 0xFFull。
static inline uint64_t mask_imm_asm(uint64_t x) {
    __asm__ volatile("and %1, %0" : "+r"(x) : "i"(0xFFu) : "cc");
    __asm__ volatile("and %1, %0" : "+r"(x) : "r"(0xFFull) : "cc");
    return x;
}

int main() {
    std::printf("%016llx\n", bswap64_asm(0x0102030405060708));
    std::printf("%016llx\n", rotl64_asm(0x0102030405060708, 8));
    std::printf("%016llx\n", mask_imm_asm(0x0102030405060708));
    return 0;
}
