#!/usr/bin/env python3
"""Render the firmware's results blob as a markdown report.

Usage: report.py results.bin sizes.tsv [--llvm-src DIR] [--raw-out FILE]

All timings are net cycles/op: the median of N_PASSES passes minus the
median of the same loop calling a `bx lr` stub with the same call shape.
"""

import argparse
import datetime
import math
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import BUILTINS  # noqa: E402

MAGIC = 0x52324250
KIND_TAG = {"K_F_FF": "f_ff", "K_D_DD": "d_dd", "K_32_32": "32_32",
            "K_64_32": "64_32", "K_32_64": "32_64", "K_64_64": "64_64"}
FAMILY_TITLE = [
    ("Arithmetic", {"FAM_ADDSUB", "FAM_MUL", "FAM_DIV", "FAM_NEG"}),
    ("Extend / truncate", {"FAM_EXTEND", "FAM_TRUNC"}),
    ("Float -> int", {"FAM_FIX"}),
    ("Int -> float", {"FAM_FLOAT"}),
]


def git(src, *args):
    try:
        return subprocess.run(["git", "-C", src, *args], text=True,
                              capture_output=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "?"


def parse(blob):
    magic, length, sink, _ = struct.unpack_from("<4I", blob)
    if magic != MAGIC:
        sys.exit(f"ERROR: bad magic {magic:#x} - firmware did not finish?")
    text = blob[16:16 + length].decode()
    return text, sink


def pct(new, old):
    if old <= 0:
        return "n/a"
    return f"{(new - old) * 100 / old:+.0f}%"


def geomean(xs):
    xs = [x for x in xs if x > 0]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("sizes")
    ap.add_argument("--llvm-src", default=".")
    ap.add_argument("--raw-out")
    a = ap.parse_args()

    text, sink = parse(Path(a.results).read_bytes())
    if a.raw_out:
        Path(a.raw_out).write_text(text)

    info, base, runs, checks, probes, faults = {}, {}, {}, [], [], []
    ended = False
    for line in text.splitlines():
        f = line.split("\t")
        if f[0] == "I":
            info = dict(zip(f[1::2], f[2::2]))
        elif f[0] == "B":
            base[f[1]] = int(f[4]) / 100
        elif f[0] == "R":
            runs[(f[1], f[2], f[3])] = (int(f[4]) / 100, int(f[5]) / 100,
                                        int(f[6]) / 100)
        elif f[0] == "M":
            checks.append(f[1:])
        elif f[0] == "P":
            probes.append(f[1:])
        elif f[0] == "FAULT":
            faults.append(f[1:])
        elif f[0] == "END":
            ended = True

    sizes = defaultdict(dict)
    for line in Path(a.sizes).read_text().splitlines():
        name, mode, size, member = line.split("\t")
        sizes[name][mode] = (int(size), member)

    kind = {b[0]: b[1] for b in BUILTINS}
    fam = {b[0]: b[4] for b in BUILTINS}

    def net(name, cat, var):
        r = runs.get((name, cat, var))
        if r is None:
            return None
        return max(r[0] - base[KIND_TAG[kind[name]]], 0.0)

    cats = defaultdict(list)
    for (name, cat, _var) in runs:
        if cat not in cats[name]:
            cats[name].append(cat)

    out = []
    w = out.append
    sha = git(a.llvm_src, "rev-parse", "--short=12", "HEAD")
    branch = git(a.llvm_src, "rev-parse", "--abbrev-ref", "HEAD")
    clang = subprocess.run(["clang", "--version"], text=True,
                           capture_output=True).stdout.splitlines()[0]

    w("# compiler-rt soft-float builtins on a Raspberry Pi Pico 2\n")
    w(f"- date: {datetime.date.today()}")
    w(f"- llvm-project: `{sha}` ({branch})")
    w(f"- compiler: {clang}")
    w("- target: RP2350 Cortex-M33 core 0, `thumbv8m.main-none-eabi "
      "-mcpu=cortex-m33 -mfloat-abi=soft`, builtins at `-O3` "
      "(compiler-rt's own flags), code + data in SRAM")
    w(f"- method: {info.get('inputs', '?')} inputs per category, median of "
      f"{info.get('passes', '?')} passes, SysTick on the core clock")
    w("- numbers: **net cycles/op** (loop + call overhead subtracted). "
      "Divide by 150 for µs at the Pico 2's default 150 MHz.\n")
    w("Variants:\n")
    w("- **asm**: what the armv8m.main archive ships today. For these "
      "builtins `filter_builtin_sources` picks `arm/*.S` over both the `.c` "
      "and the `.cpp`, so `COMPILER_RT_USE_LIBC_MATH=ON` does not change it.")
    w("- **legacy**: generic compiler-rt C (`<name>.c`).")
    w("- **libc**: LLVM-libc backed (`<name>.cpp`).\n")

    if not ended:
        w("> **The run did not reach END** - see the fault section below.\n")

    # Summary: geometric mean over categories, per builtin.
    w("## Summary (geomean over input categories)\n")
    w("| builtin | asm | legacy | libc | libc vs legacy | libc vs asm |")
    w("|---|---:|---:|---:|---:|---:|")
    for name, *_ in BUILTINS:
        if name not in cats:
            continue
        g = {}
        for var in ("asm", "legacy", "libc"):
            vals = [net(name, c, var) for c in cats[name]]
            g[var] = geomean([v for v in vals if v is not None]) \
                if any(v is not None for v in vals) else None
        fmt = lambda v: "-" if v is None else f"{v:.1f}"
        w(f"| `__{name}` | {fmt(g['asm'])} | {fmt(g['legacy'])} | "
          f"{fmt(g['libc'])} | {pct(g['libc'], g['legacy'])} | "
          f"{pct(g['libc'], g['asm']) if g['asm'] else '-'} |")
    w("")

    for title, fams in FAMILY_TITLE:
        w(f"## {title} (net cycles/op)\n")
        w("| builtin | category | asm | legacy | libc | libc vs legacy "
          "| libc vs asm |")
        w("|---|---|---:|---:|---:|---:|---:|")
        for name, *_ in BUILTINS:
            if fam[name] not in fams or name not in cats:
                continue
            for c in cats[name]:
                v = {var: net(name, c, var) for var in ("asm", "legacy", "libc")}
                fmt = lambda x: "-" if x is None else f"{x:.1f}"
                w(f"| `__{name}` | {c} | {fmt(v['asm'])} | {fmt(v['legacy'])} "
                  f"| {fmt(v['libc'])} | {pct(v['libc'], v['legacy'])} | "
                  f"{pct(v['libc'], v['asm']) if v['asm'] is not None else '-'} |")
        w("")

    w("## Bit-exactness: libc vs legacy / asm\n")
    w("Results compared per input after masking to the result width and "
      "collapsing every NaN to one pattern.\n")
    bad = [c for c in checks if c[3] != "0"]
    if not bad:
        w(f"All {len(checks)} (builtin, category, reference) checks match.\n")
    else:
        w(f"{len(checks) - len(bad)} of {len(checks)} checks match. "
          "Mismatches:\n")
        w("| builtin | category | vs | mismatches | first input a | b "
          "| reference | libc |")
        w("|---|---|---|---:|---|---|---|---|")
        for c in bad:
            name, cat, ref, mm, n, *ex = c
            ex += ["-"] * (4 - len(ex))
            w(f"| `__{name}` | {cat} | {ref} | {mm}/{n} | `{ex[0]}` | "
              f"`{ex[1]}` | `{ex[2]}` | `{ex[3]}` |")
        w("")

    w("## Closed-world recursion probe\n")
    w("libc objects relinked so their internal soft-float calls "
      "(`__aeabi_fadd`, `__aeabi_dmul`, ...) bind to the libc builtins "
      "themselves, as on a soft-float target with no `arm/*.S` "
      "(e.g. riscv32 without F). `MSPLIM` turns unbounded recursion into a "
      "stack-overflow fault.\n")
    started = [p[0] for p in probes if p[1] == "start"]
    ok = {p[0]: p[2] for p in probes if p[1] == "ok"}
    w("| call | outcome |")
    w("|---|---|")
    for p in started:
        if p in ok:
            w(f"| `{p}` | returned `{ok[p]}` |")
        else:
            fault = next((f for f in faults if f[3] == p), None)
            stkof = fault and int(fault[2], 16) & (1 << 20)
            w(f"| `{p}` | **fault** (IPSR {fault[0] if fault else '?'}, CFSR "
              f"`{fault[2] if fault else '?'}`{', STKOF: stack overflow' if stkof else ''}) |")
    other = [f for f in faults if f[3] not in started]
    if other:
        w("\nUnexpected faults during the benchmark:\n")
        for f in other:
            w(f"- IPSR {f[0]}, LR `{f[1]}`, CFSR `{f[2]}` in `{f[3]}` / "
              f"{f[4]} / {f[5]}")
    w("")

    w("## Code size (.text bytes of the object)\n")
    w("asm objects can hold more than one builtin (e.g. `addsf3.S` has add "
      "and sub); legacy `sub*` is a thin wrapper that tail-calls `add*`. "
      "libc objects include their inlined/instantiated helpers.\n")
    w("| builtin | asm | legacy | libc |")
    w("|---|---:|---:|---:|")
    for name, *_ in BUILTINS:
        s = sizes.get(name, {})
        g = lambda m: str(s[m][0]) if m in s else "-"
        w(f"| `__{name}` | {g('asm')} | {g('legacy')} | {g('libc')} |")
    w("")
    w(f"<details><summary>Raw output</summary>\n\n```\n{text.rstrip()}\n```\n"
      f"\nchecksum sink: `{sink:#010x}`\n</details>")
    print("\n".join(out))


if __name__ == "__main__":
    main()
