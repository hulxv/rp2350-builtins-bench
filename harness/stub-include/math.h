/* Freestanding stand-in: bare-metal armv8m.main has no libc sysroot, so
   reuse LLVM-libc's own math macros (found via -isystem <libc>). */
#ifndef PICO_BENCH_MATH_H
#define PICO_BENCH_MATH_H
#include "include/llvm-libc-macros/math-macros.h"
#endif
