// Runtime check that the CPU supports the instruction set the binary was built
// for. This file is compiled without any -m architecture flags so that it can
// run (and fail gracefully) on older processors.
#include "cpu_check.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

const char* cpu_check_message() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER) && !defined(__clang__)
    int info[4];
    __cpuid(info, 1);
    const bool popcnt = (info[2] >> 23) & 1;
    __cpuidex(info, 7, 0);
    const bool bmi2 = (info[1] >> 8) & 1;
#else
    __builtin_cpu_init();
    const bool popcnt = __builtin_cpu_supports("popcnt");
    const bool bmi2 = __builtin_cpu_supports("bmi2");
#endif
#if defined(USE_POPCNT)
    if (!popcnt) return "this binary requires the POPCNT instruction; rebuild with -DTESSERA_ARCH=generic";
#endif
#if defined(USE_PEXT)
    if (!bmi2) return "this binary requires BMI2 (PEXT); rebuild with -DTESSERA_ARCH=popcnt or generic";
#endif
    (void)popcnt;
    (void)bmi2;
#endif
    return nullptr;
}

const char* build_arch_name() {
#if defined(USE_PEXT)
    return "x86-64-bmi2";
#elif defined(USE_POPCNT)
    return "x86-64-popcnt";
#else
    return "generic";
#endif
}
