#include <cassert>
#include <cstdint>
#include <cstdio>


__attribute__((noinline)) uint64_t ret_test(uint64_t x){
    __asm__ volatile(
        "movq $1, %%rbx"
        :
        :
        :"rbx"
    );
    return x + 1ull;
}

__attribute__((noinline)) int bad_store(int *p){
    int before = *p;
    __asm__ volatile(
        "movl $99, (%0)"
        :
        :"r"(p)
    );
    return before + *p;
}

__attribute__((noinline)) int good_store(int *p){
    int before = *p;
    __asm__ volatile(
        "movl $99, (%0)"
        :
        :"r"(p)
        :"memory"
    );
    return before + *p;
}

__attribute__((noinline)) int precise_store(int *p){
    int before = *p;
    __asm__ volatile(
        "movl $99, %0"
        :"=m"(*p)
    );
    return before + *p;
}

__attribute__((noinline)) int increment_store(int* p){
    int old = *p;
    __asm__ volatile(
#ifdef USE_INCL
        "incl %0"
#else
        "addl $1, %0"
#endif
        : "+m"(*p)
        :
        : "cc"
    );
    return old + *p;
}



__attribute__((noinline)) uint64_t rdtsc_asm(){
    unsigned lo = 0, hi = 0;
    __asm__ volatile(
        "rdtsc":"=a"(lo), "=d"(hi)
    );

    return (uint64_t(hi) << 32) | lo;
}

int main(){
    int a = 1;
    std::printf("bad_store output: %d\n", bad_store(&a));

    int b = 1;
    std::printf("good_store output: %d\n", good_store(&b));

    int c = 1;
    std::printf("precise_store output: %d\n", precise_store(&c));

    int d = 10;
    int result = increment_store(&d);
    std::printf("increment_store output: %d, d after increment: %d\n", result, d);

    std::printf("rdtsc_asm output: %016llX\n", rdtsc_asm());

}