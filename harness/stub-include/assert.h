/* Freestanding stand-in: bare-metal armv8m.main has no libc sysroot. */
#ifndef PICO_BENCH_ASSERT_H
#define PICO_BENCH_ASSERT_H
#define assert(x) ((void)0)
#endif
