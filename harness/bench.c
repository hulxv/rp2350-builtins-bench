//===-- On-target benchmark for compiler-rt soft-float builtins ----------===//
//
// Runs on an RP2350 Cortex-M33 compiled with -mfloat-abi=soft, so every
// float operation goes through a builtin. Each builtin exists in up to
// three variants linked side by side:
//
//   asm     the symbol the shipped armv8m.main archive provides
//           (hand-written arm/*.S where one exists, else generic C)
//   legacy  the generic compiler-rt C source, renamed legacy___<name>
//   libc    the LLVM-libc backed .cpp source, renamed libc___<name>
//
// Timing uses SysTick on the core clock, so results are cycles/op and do
// not depend on clk_sys. Code and data both live in SRAM (no XIP cache).
//
// Results are written as TSV text into the last 64K of flash, then the
// chip reboots into BOOTSEL so the host can read them back with picotool.
//===----------------------------------------------------------------------===//

#include <stddef.h>
#include <stdint.h>

#include "builtins_gen.h"

#define N_INPUTS 1024
#define N_PASSES 7
#define CHUNK 128

#define RESULTS_FLASH_OFFSET 0x3F0000u
#define RESULTS_FLASH_SIZE 0x10000u
#define RESULTS_MAGIC 0x52324250u // "PB2R"

#define REG(addr) (*(volatile uint32_t *)(addr))
#define SYST_CSR REG(0xe000e010)
#define SYST_RVR REG(0xe000e014)
#define SYST_CVR REG(0xe000e018)
#define CPUID REG(0xe000ed00)
#define CFSR REG(0xe000ed28)
#define WATCHDOG_CTRL REG(0x400d8000)

//===----------------------------------------------------------------------===//
// Freestanding support
//===----------------------------------------------------------------------===//

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  while (n--)
    *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    while (n--)
      d[n] = s[n];
  }
  return dst;
}

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = dst;
  while (n--)
    *d++ = (unsigned char)c;
  return dst;
}

//===----------------------------------------------------------------------===//
// Bootrom calls (constants from pico-sdk bootrom_constants.h)
//===----------------------------------------------------------------------===//

typedef void *(*rom_lookup_fn)(uint32_t code, uint32_t mask);
typedef void (*rom_void_fn)(void);
typedef void (*rom_erase_fn)(uint32_t, size_t, uint32_t, uint8_t);
typedef void (*rom_program_fn)(uint32_t, const uint8_t *, size_t);
typedef int (*rom_reboot_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

#define ROM_CODE(a, b) ((uint32_t)(a) | ((uint32_t)(b) << 8))
#define RT_FLAG_FUNC_ARM_SEC 0x0004u

static void *rom_func(uint32_t code) {
  rom_lookup_fn lookup =
      (rom_lookup_fn)(uintptr_t)(*(volatile uint16_t *)0x16);
  return lookup(code, RT_FLAG_FUNC_ARM_SEC);
}

//===----------------------------------------------------------------------===//
// Output buffer
//===----------------------------------------------------------------------===//

static struct {
  uint32_t magic;
  uint32_t length;
  uint32_t sink;
  uint32_t reserved;
  char text[RESULTS_FLASH_SIZE - 16];
} g_out __attribute__((aligned(256)));

static volatile uint32_t g_sink;

static void put_str(const char *s) {
  while (*s && g_out.length < sizeof(g_out.text) - 1)
    g_out.text[g_out.length++] = *s++;
}

static void put_ch(char c) {
  char s[2] = {c, 0};
  put_str(s);
}

static void put_u32(uint32_t v) {
  char buf[11];
  int i = 10;
  buf[i] = 0;
  do {
    buf[--i] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  put_str(&buf[i]);
}

static void put_hex(uint64_t v, int digits) {
  static const char hex[] = "0123456789abcdef";
  char buf[17];
  buf[digits] = 0;
  for (int i = digits - 1; i >= 0; --i, v >>= 4)
    buf[i] = hex[v & 0xf];
  put_str("0x");
  put_str(buf);
}

// Write g_out to flash and reboot into BOOTSEL. Runs from SRAM, so XIP
// can be torn down freely.
static __attribute__((noreturn)) void finish(void) {
  g_out.magic = RESULTS_MAGIC;
  g_out.sink = g_sink;
  uint32_t bytes = (16 + g_out.length + 255) & ~255u;

  ((rom_void_fn)rom_func(ROM_CODE('I', 'F')))();
  ((rom_void_fn)rom_func(ROM_CODE('E', 'X')))();
  ((rom_erase_fn)rom_func(ROM_CODE('R', 'E')))(
      RESULTS_FLASH_OFFSET, RESULTS_FLASH_SIZE, 1u << 16, 0xd8);
  ((rom_program_fn)rom_func(ROM_CODE('R', 'P')))(
      RESULTS_FLASH_OFFSET, (const uint8_t *)&g_out, bytes);
  ((rom_void_fn)rom_func(ROM_CODE('F', 'C')))();

  // REBOOT_TYPE_BOOTSEL | NO_RETURN_ON_SUCCESS, both USB interfaces on.
  ((rom_reboot_fn)rom_func(ROM_CODE('R', 'B')))(0x2 | 0x100, 10, 0, 0);
  for (;;)
    ;
}

//===----------------------------------------------------------------------===//
// Builtin table
//===----------------------------------------------------------------------===//

enum Kind { K_F_FF, K_D_DD, K_32_32, K_64_32, K_32_64, K_64_64 };
enum Fmt { F16, F32, F64, I32, U32, I64, U64 };
enum Family { FAM_ADDSUB, FAM_MUL, FAM_DIV, FAM_NEG, FAM_EXTEND, FAM_TRUNC,
              FAM_FIX, FAM_FLOAT };
enum Variant { V_ASM, V_LEGACY, V_LIBC, N_VARIANTS };

static const char *const variant_names[N_VARIANTS] = {"asm", "legacy", "libc"};

struct Builtin {
  const char *name;
  enum Kind kind;
  enum Fmt in, out;
  enum Family family;
  int has_asm;
  void *fn[N_VARIANTS];
};

#define BUILTIN(NAME, KIND, IN, OUT, FAM, HAS_ASM)                             \
  extern void __##NAME(void);                                                  \
  extern void legacy___##NAME(void);                                           \
  extern void libc___##NAME(void);
BUILTINS
#undef BUILTIN

static const struct Builtin builtins[] = {
#define BUILTIN(NAME, KIND, IN, OUT, FAM, HAS_ASM)                             \
  {#NAME, KIND, IN, OUT, FAM, HAS_ASM,                                         \
   {(void *)__##NAME, (void *)legacy___##NAME, (void *)libc___##NAME}},
    BUILTINS
#undef BUILTIN
};

#define N_BUILTINS (sizeof(builtins) / sizeof(builtins[0]))

extern void bench_nop(void);

//===----------------------------------------------------------------------===//
// Input generation
//===----------------------------------------------------------------------===//

struct FmtInfo {
  int exp_bits, frac_bits, bias, width;
};

static const struct FmtInfo fmt_info[] = {
    [F16] = {5, 10, 15, 16},  [F32] = {8, 23, 127, 32},
    [F64] = {11, 52, 1023, 64}, [I32] = {0, 0, 0, 32},
    [U32] = {0, 0, 0, 32},    [I64] = {0, 0, 0, 64},
    [U64] = {0, 0, 0, 64},
};

static uint64_t g_rng;

static uint64_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

// Uniform integer in [lo, hi].
static int rr(int lo, int hi) {
  return lo + (int)((uint32_t)rnd() % (uint32_t)(hi - lo + 1));
}

static uint64_t mask_bits(int w) { return w == 64 ? ~0ull : (1ull << w) - 1; }

static uint64_t fp_make(enum Fmt f, int sign, int bexp, uint64_t frac) {
  const struct FmtInfo *fi = &fmt_info[f];
  return ((uint64_t)(sign & 1) << (fi->width - 1)) |
         ((uint64_t)bexp << fi->frac_bits) | (frac & mask_bits(fi->frac_bits));
}

static uint64_t fp_rand_frac(enum Fmt f) {
  return rnd() & mask_bits(fmt_info[f].frac_bits);
}

static int fp_max_exp(enum Fmt f) { return (1 << fmt_info[f].exp_bits) - 1; }

static uint64_t fp_inf(enum Fmt f, int sign) {
  return fp_make(f, sign, fp_max_exp(f), 0);
}

static uint64_t fp_qnan(enum Fmt f, int sign) {
  uint64_t q = 1ull << (fmt_info[f].frac_bits - 1);
  return fp_make(f, sign, fp_max_exp(f), q | fp_rand_frac(f));
}

// Normal value with unbiased exponent `e`.
static uint64_t fp_normal_e(enum Fmt f, int sign, int e) {
  return fp_make(f, sign, e + fmt_info[f].bias, fp_rand_frac(f));
}

static uint64_t fp_subnormal(enum Fmt f) {
  return fp_make(f, (int)rnd(), 0, fp_rand_frac(f) | 1);
}

// A moderate-range normal (keeps add/mul/div results normal).
static uint64_t fp_moderate(enum Fmt f) {
  int r = f == F64 ? 60 : f == F32 ? 30 : 6;
  return fp_normal_e(f, (int)rnd(), rr(-r, r));
}

static uint64_t fp_special(enum Fmt f, int i, int which) {
  return ((i ^ which) & 1) ? fp_qnan(f, (int)rnd()) : fp_inf(f, (int)rnd());
}

enum Cat { C_NORMAL, C_SUBNORMAL, C_SPECIAL, C_CANCEL, C_EXTREME, C_OVERFLOW,
           C_SMALL, C_EXACT, C_FULL, N_CATS };

static const char *const cat_names[N_CATS] = {
    "normal", "subnormal", "special", "cancellation", "extreme",
    "overflow", "small", "exact", "full"};

static const enum Cat fam_cats[][5] = {
    [FAM_ADDSUB] = {C_NORMAL, C_SUBNORMAL, C_SPECIAL, C_CANCEL, N_CATS},
    [FAM_MUL] = {C_NORMAL, C_SUBNORMAL, C_SPECIAL, C_EXTREME, N_CATS},
    [FAM_DIV] = {C_NORMAL, C_SUBNORMAL, C_SPECIAL, C_EXTREME, N_CATS},
    [FAM_NEG] = {C_NORMAL, N_CATS},
    [FAM_EXTEND] = {C_NORMAL, C_SUBNORMAL, C_SPECIAL, N_CATS},
    [FAM_TRUNC] = {C_NORMAL, C_SUBNORMAL, C_SPECIAL, C_OVERFLOW, N_CATS},
    [FAM_FIX] = {C_NORMAL, C_SMALL, C_OVERFLOW, C_SPECIAL, N_CATS},
    [FAM_FLOAT] = {C_EXACT, C_FULL, N_CATS},
};

static uint64_t g_a[N_INPUTS], g_b[N_INPUTS];

static void gen_one(const struct Builtin *bi, enum Cat cat, int i,
                    uint64_t *a, uint64_t *b) {
  enum Fmt in = bi->in, out = bi->out;
  *b = 0;
  switch (bi->family) {
  case FAM_ADDSUB:
  case FAM_MUL:
  case FAM_DIV:
    switch (cat) {
    case C_NORMAL:
      *a = fp_moderate(in);
      *b = fp_moderate(in);
      return;
    case C_SUBNORMAL:
      *a = fp_subnormal(in);
      *b = fp_subnormal(in);
      return;
    case C_SPECIAL:
      *a = fp_special(in, i, 0);
      *b = fp_special(in, i, 1);
      return;
    case C_CANCEL:
      // y ~= -x: flip the sign, perturb the low 8 fraction bits.
      *a = fp_moderate(in);
      *b = *a ^ (1ull << (fmt_info[in].width - 1)) ^ (rnd() & 0xff);
      return;
    case C_EXTREME: {
      // Results that overflow (even i) or underflow (odd i).
      int top = fp_max_exp(in) - 1;
      int big = top - rr(0, 3), small = 1 + rr(0, 3);
      int ea = (i & 1) ? small : big;
      int eb = bi->family == FAM_MUL ? ea : ((i & 1) ? big : small);
      *a = fp_make(in, (int)rnd(), ea, fp_rand_frac(in));
      *b = fp_make(in, (int)rnd(), eb, fp_rand_frac(in));
      return;
    }
    default:
      break;
    }
    break;
  case FAM_NEG:
    *a = fp_make(in, (int)rnd(), rr(1, fp_max_exp(in) - 1), fp_rand_frac(in));
    return;
  case FAM_EXTEND:
    switch (cat) {
    case C_NORMAL:
      *a = fp_make(in, (int)rnd(), rr(1, fp_max_exp(in) - 1),
                   fp_rand_frac(in));
      return;
    case C_SUBNORMAL:
      *a = fp_subnormal(in);
      return;
    case C_SPECIAL:
      *a = fp_special(in, i, 0);
      return;
    default:
      break;
    }
    break;
  case FAM_TRUNC: {
    int tb = fmt_info[out].bias, tf = fmt_info[out].frac_bits;
    switch (cat) {
    case C_NORMAL: // normal in the destination format
      *a = fp_normal_e(in, (int)rnd(), rr(1 - tb, tb));
      return;
    case C_SUBNORMAL: // subnormal in the destination format
      *a = fp_normal_e(in, (int)rnd(), rr(1 - tb - tf, -tb));
      return;
    case C_OVERFLOW:
      *a = fp_normal_e(in, (int)rnd(), rr(tb + 1, tb + 16));
      return;
    case C_SPECIAL:
      *a = fp_special(in, i, 0);
      return;
    default:
      break;
    }
    break;
  }
  case FAM_FIX: {
    int w = fmt_info[out].width, sgn = out == I32 || out == I64;
    int s = sgn ? (int)rnd() : 0;
    switch (cat) {
    case C_NORMAL:
      *a = fp_normal_e(in, s, rr(0, sgn ? w - 2 : w - 1));
      return;
    case C_SMALL: // |x| < 1
      *a = fp_normal_e(in, (int)rnd(), rr(-24, -1));
      return;
    case C_OVERFLOW:
      *a = fp_normal_e(in, s, rr(w, w + 16));
      return;
    case C_SPECIAL:
      *a = fp_special(in, i, 0);
      return;
    default:
      break;
    }
    break;
  }
  case FAM_FLOAT: {
    int w = fmt_info[in].width, sgn = in == I32 || in == I64;
    if (cat == C_EXACT) {
      int64_t v = rr(sgn ? -(1 << 20) : 0, 1 << 20);
      *a = (uint64_t)v & mask_bits(w);
    } else {
      *a = rnd() & mask_bits(w);
    }
    return;
  }
  }
  *a = 0;
}

//===----------------------------------------------------------------------===//
// Timing
//===----------------------------------------------------------------------===//

// One timed pass over all inputs: total SysTick cycles. Chunked so the
// 24-bit down-counter cannot wrap within a measurement.
#define DEFINE_PASS(NAME, RET, CALL)                                           \
  static uint32_t NAME(void *fp, int n) {                                      \
    uint32_t total = 0;                                                        \
    RET acc = 0;                                                               \
    for (int base = 0; base < n; base += CHUNK) {                              \
      uint32_t t0 = SYST_CVR;                                                  \
      for (int i = base; i < base + CHUNK; ++i)                                \
        acc ^= CALL;                                                           \
      uint32_t t1 = SYST_CVR;                                                  \
      total += (t0 - t1) & 0xffffff;                                           \
    }                                                                          \
    g_sink ^= (uint32_t)acc ^ (uint32_t)((uint64_t)acc >> 32);                 \
    return total;                                                              \
  }

typedef uint32_t (*fn_f_ff)(uint32_t, uint32_t);
typedef uint64_t (*fn_d_dd)(uint64_t, uint64_t);
typedef uint32_t (*fn_32_32)(uint32_t);
typedef uint64_t (*fn_64_32)(uint32_t);
typedef uint32_t (*fn_32_64)(uint64_t);
typedef uint64_t (*fn_64_64)(uint64_t);

DEFINE_PASS(pass_f_ff, uint32_t, ((fn_f_ff)fp)((uint32_t)g_a[i], (uint32_t)g_b[i]))
DEFINE_PASS(pass_d_dd, uint64_t, ((fn_d_dd)fp)(g_a[i], g_b[i]))
DEFINE_PASS(pass_32_32, uint32_t, ((fn_32_32)fp)((uint32_t)g_a[i]))
DEFINE_PASS(pass_64_32, uint64_t, ((fn_64_32)fp)((uint32_t)g_a[i]))
DEFINE_PASS(pass_32_64, uint32_t, ((fn_32_64)fp)(g_a[i]))
DEFINE_PASS(pass_64_64, uint64_t, ((fn_64_64)fp)(g_a[i]))

static uint32_t (*const pass_fns[])(void *, int) = {
    [K_F_FF] = pass_f_ff,   [K_D_DD] = pass_d_dd,   [K_32_32] = pass_32_32,
    [K_64_32] = pass_64_32, [K_32_64] = pass_32_64, [K_64_64] = pass_64_64,
};

static uint64_t call_one(enum Kind k, void *fp, uint64_t a, uint64_t b) {
  switch (k) {
  case K_F_FF:
    return ((fn_f_ff)fp)((uint32_t)a, (uint32_t)b);
  case K_D_DD:
    return ((fn_d_dd)fp)(a, b);
  case K_32_32:
    return ((fn_32_32)fp)((uint32_t)a);
  case K_64_32:
    return ((fn_64_32)fp)((uint32_t)a);
  case K_32_64:
    return ((fn_32_64)fp)(a);
  case K_64_64:
    return ((fn_64_64)fp)(a);
  }
  return 0;
}

struct Stats {
  uint32_t med, min, max; // cycles/op x100
};

static void sort_u32(uint32_t *v, int n) {
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && v[j - 1] > v[j]; --j) {
      uint32_t t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
}

static struct Stats measure(enum Kind k, void *fp) {
  uint32_t (*pass)(void *, int) = pass_fns[k];
  uint32_t samples[N_PASSES];
  pass(fp, N_INPUTS); // warm-up
  for (int p = 0; p < N_PASSES; ++p) {
    uint32_t cyc = pass(fp, N_INPUTS);
    samples[p] = (uint32_t)(((uint64_t)cyc * 100 + N_INPUTS / 2) / N_INPUTS);
  }
  sort_u32(samples, N_PASSES);
  struct Stats s = {samples[N_PASSES / 2], samples[0], samples[N_PASSES - 1]};
  return s;
}

//===----------------------------------------------------------------------===//
// Correctness cross-check
//===----------------------------------------------------------------------===//

// Mask to the result width and collapse every NaN to one pattern, so the
// check ignores NaN payload/sign differences (not observable by C code).
static uint64_t canon(enum Fmt f, uint64_t r) {
  const struct FmtInfo *fi = &fmt_info[f];
  r &= mask_bits(fi->width);
  if (fi->exp_bits) {
    uint64_t exp = (r >> fi->frac_bits) & mask_bits(fi->exp_bits);
    uint64_t frac = r & mask_bits(fi->frac_bits);
    if (exp == mask_bits(fi->exp_bits) && frac)
      r = fp_make(f, 0, fp_max_exp(f), 1ull << (fi->frac_bits - 1));
  }
  return r;
}

static int in_digits(enum Fmt f) { return fmt_info[f].width / 4; }

static void cross_check(const struct Builtin *bi, enum Cat cat,
                        enum Variant ref) {
  uint32_t mismatches = 0;
  int first = -1;
  uint64_t r_ref = 0, r_libc = 0;
  for (int i = 0; i < N_INPUTS; ++i) {
    uint64_t x = canon(bi->out, call_one(bi->kind, bi->fn[ref], g_a[i], g_b[i]));
    uint64_t y =
        canon(bi->out, call_one(bi->kind, bi->fn[V_LIBC], g_a[i], g_b[i]));
    if (x != y) {
      if (first < 0) {
        first = i;
        r_ref = x;
        r_libc = y;
      }
      ++mismatches;
    }
  }
  // M <builtin> <category> <ref> <mismatches> <n> [a b ref libc]
  put_str("M\t");
  put_str(bi->name);
  put_ch('\t');
  put_str(cat_names[cat]);
  put_ch('\t');
  put_str(variant_names[ref]);
  put_ch('\t');
  put_u32(mismatches);
  put_ch('\t');
  put_u32(N_INPUTS);
  if (first >= 0) {
    int two = bi->kind == K_F_FF || bi->kind == K_D_DD;
    put_ch('\t');
    put_hex(g_a[first], in_digits(bi->in));
    put_ch('\t');
    if (two)
      put_hex(g_b[first], in_digits(bi->in));
    else
      put_ch('-');
    put_ch('\t');
    put_hex(r_ref, in_digits(bi->out));
    put_ch('\t');
    put_hex(r_libc, in_digits(bi->out));
  }
  put_ch('\n');
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

static const char *g_cur_name = "-";
static const char *g_cur_cat = "-";
static const char *g_cur_variant = "-";

void bench_fault(uint32_t ipsr, uint32_t lr) {
  __asm volatile("msr msplim, %0" ::"r"(0));
  put_str("FAULT\t");
  put_u32(ipsr);
  put_ch('\t');
  put_hex(lr, 8);
  put_ch('\t');
  put_hex(CFSR, 8); // UFSR.STKOF (bit 20) = stack overflow
  put_ch('\t');
  put_str(g_cur_name);
  put_ch('\t');
  put_str(g_cur_cat);
  put_ch('\t');
  put_str(g_cur_variant);
  put_ch('\n');
  finish();
}

// Closed-world libc builtins (libcc___*, see scripts/build.py): their
// internal soft-float calls bind to the libc builtins themselves, as on a
// target without arm/*.S. libc's rounding-mode probe does float adds, so an
// inexact add can re-enter __addsf3 without bound. MSPLIM turns that into a
// clean STKOF fault (reported by bench_fault) instead of trampling SRAM.
extern uint32_t libcc___addsf3(uint32_t, uint32_t);
extern uint64_t libcc___adddf3(uint64_t, uint64_t);

static void probe(const char *name, uint64_t (*call)(void), int digits) {
  g_cur_name = name;
  put_str("P\t");
  put_str(name);
  put_str("\tstart\n");
  __asm volatile("msr msplim, %0" ::"r"(0x20080000u));
  uint64_t r = call();
  __asm volatile("msr msplim, %0" ::"r"(0));
  put_str("P\t");
  put_str(name);
  put_str("\tok\t");
  put_hex(r, digits);
  put_ch('\n');
}

static uint64_t probe_addsf3_exact(void) {
  return libcc___addsf3(0x3f800000u, 0x3f800000u); // 1 + 1
}
static uint64_t probe_adddf3_exact(void) {
  return libcc___adddf3(0x3ff0000000000000ull, 0x3ff0000000000000ull);
}
static uint64_t probe_addsf3_inexact(void) {
  return libcc___addsf3(0x3f800000u, 0x33800000u); // 1 + 2^-24
}
static uint64_t probe_adddf3_inexact(void) {
  return libcc___adddf3(0x3ff0000000000000ull, 0x3c30000000000000ull);
}

static void recursion_probe(void) {
  g_cur_cat = "recursion-probe";
  g_cur_variant = "libc-closed";
  probe("addsf3(1,1)", probe_addsf3_exact, 8);
  probe("adddf3(1,1)", probe_adddf3_exact, 16);
  probe("adddf3(1,2^-60)", probe_adddf3_inexact, 16);
  probe("addsf3(1,2^-24)", probe_addsf3_inexact, 8);
}

static void put_stats(const char *tag, const char *name, const char *cat,
                      const char *variant, struct Stats s) {
  put_str(tag);
  put_ch('\t');
  put_str(name);
  put_ch('\t');
  put_str(cat);
  put_ch('\t');
  put_str(variant);
  put_ch('\t');
  put_u32(s.med);
  put_ch('\t');
  put_u32(s.min);
  put_ch('\t');
  put_u32(s.max);
  put_ch('\n');
}

int main(void) {
  WATCHDOG_CTRL &= ~0x40000000u;

  SYST_RVR = 0xffffff;
  SYST_CVR = 0;
  SYST_CSR = 0x5; // CLKSOURCE = core clock, ENABLE, no interrupt

  put_str("I\tcpuid\t");
  put_hex(CPUID, 8);
  put_str("\tinputs\t");
  put_u32(N_INPUTS);
  put_str("\tpasses\t");
  put_u32(N_PASSES);
  put_ch('\n');

  // Loop + call overhead, per call shape. All variants share it, so the
  // host subtracts it to report the builtin's own cost.
  for (int k = K_F_FF; k <= K_64_64; ++k) {
    static const char *const kind_names[] = {"f_ff",  "d_dd",  "32_32",
                                             "64_32", "32_64", "64_64"};
    g_rng = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < N_INPUTS; ++i) {
      g_a[i] = rnd();
      g_b[i] = rnd();
    }
    put_stats("B", kind_names[k], "-", "nop", measure(k, (void *)bench_nop));
  }

  for (unsigned bidx = 0; bidx < N_BUILTINS; ++bidx) {
    const struct Builtin *bi = &builtins[bidx];
    g_cur_name = bi->name;
    for (const enum Cat *c = fam_cats[bi->family]; *c != N_CATS; ++c) {
      g_cur_cat = cat_names[*c];
      g_rng = 0xc0ffee00ull + bidx * 131 + *c;
      for (int i = 0; i < N_INPUTS; ++i)
        gen_one(bi, *c, i, &g_a[i], &g_b[i]);

      for (int v = 0; v < N_VARIANTS; ++v) {
        if (v == V_ASM && !bi->has_asm)
          continue;
        g_cur_variant = variant_names[v];
        put_stats("R", bi->name, cat_names[*c], variant_names[v],
                  measure(bi->kind, bi->fn[v]));
      }
      g_cur_variant = "check";
      cross_check(bi, *c, V_LEGACY);
      if (bi->has_asm)
        cross_check(bi, *c, V_ASM);
    }
  }

  put_str("END\n");
  recursion_probe();
  finish();
}
