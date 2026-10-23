/* Freestanding stand-in: bare-metal armv8m.main has no libc sysroot, so
   reuse LLVM-libc's own FE_* definitions (found via -isystem <libc>). */
#ifndef PICO_BENCH_FENV_H
#define PICO_BENCH_FENV_H
#include "include/llvm-libc-macros/fenv-macros.h"
#include "include/llvm-libc-types/fenv_t.h"
#include "include/llvm-libc-types/fexcept_t.h"
#endif
