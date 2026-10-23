/* Freestanding stand-in: bare-metal armv8m.main has no libc sysroot, so
   reuse LLVM-libc's generic errno values (found via -isystem <libc>). */
#ifndef PICO_BENCH_ERRNO_H
#define PICO_BENCH_ERRNO_H
#include "include/llvm-libc-macros/generic-error-number-macros.h"
#endif
