// Prints the best instruction set level supported by the build machine:
// "bmi2", "popcnt" or "generic". PEXT is avoided on AMD CPUs before Zen 3,
// where it is microcoded and slower than magic bitboards.
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

int main() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    int r[4];
    __cpuid(r, 0);
    char vendor[13] = {};
    std::memcpy(vendor, &r[1], 4);
    std::memcpy(vendor + 4, &r[3], 4);
    std::memcpy(vendor + 8, &r[2], 4);
    __cpuid(r, 1);
    unsigned eax = unsigned(r[0]), ecx = unsigned(r[2]);
    __cpuidex(r, 7, 0);
    unsigned ebx7 = unsigned(r[1]);
#elif defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    char vendor[13] = {};
    std::memcpy(vendor, &b, 4);
    std::memcpy(vendor + 4, &d, 4);
    std::memcpy(vendor + 8, &c, 4);
    __cpuid(1, a, b, c, d);
    unsigned eax = a, ecx = c;
    __cpuid_count(7, 0, a, b, c, d);
    unsigned ebx7 = b;
#else
    std::printf("generic");
    return 0;
#endif
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
    bool popcnt = (ecx >> 23) & 1;
    bool bmi2 = (ebx7 >> 8) & 1;
    unsigned family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    bool slowPext = std::strcmp(vendor, "AuthenticAMD") == 0 && family < 0x19;
    if (bmi2 && popcnt && !slowPext) std::printf("bmi2");
    else if (popcnt) std::printf("popcnt");
    else std::printf("generic");
    return 0;
#endif
}
