#!/usr/bin/env python3
"""Build the RP2350 benchmark image.

1. Configure compiler-rt's builtins standalone for thumbv8m.main soft-float
   twice: COMPILER_RT_USE_LIBC_MATH=OFF (built: the shipped archive, which is
   also the "asm" variant) and =ON (configured only, for its compile flags).
2. Compile every benchmarked builtin's generic <name>.c and libc <name>.cpp
   with exactly the flags those builds use, and rename their global "__*"
   symbols to legacy___* / libc___* so all three variants link side by side.
   (On Arm, filter_builtin_sources drops both the .c and the .cpp whenever an
   arm/<name>.S exists, so neither CMake archive alone has all variants.)
3. Link harness + variant objects + shipped archive into a RAM-only ELF/UF2.

Writes build/fw.elf, build/fw.uf2 and build/sizes.tsv.
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
BUILD = HERE / "build"
HARNESS = HERE / "harness"

TARGET = "thumbv8m.main-none-eabi"
CPU_FLAGS = "-mcpu=cortex-m33 -mfloat-abi=soft -mfpu=none"

# name, call shape, input fmt, output fmt, family (see harness/bench.c)
BUILTINS = [
    ("addsf3", "K_F_FF", "F32", "F32", "FAM_ADDSUB"),
    ("subsf3", "K_F_FF", "F32", "F32", "FAM_ADDSUB"),
    ("mulsf3", "K_F_FF", "F32", "F32", "FAM_MUL"),
    ("divsf3", "K_F_FF", "F32", "F32", "FAM_DIV"),
    ("adddf3", "K_D_DD", "F64", "F64", "FAM_ADDSUB"),
    ("subdf3", "K_D_DD", "F64", "F64", "FAM_ADDSUB"),
    ("muldf3", "K_D_DD", "F64", "F64", "FAM_MUL"),
    ("divdf3", "K_D_DD", "F64", "F64", "FAM_DIV"),
    ("negsf2", "K_32_32", "F32", "F32", "FAM_NEG"),
    ("negdf2", "K_64_64", "F64", "F64", "FAM_NEG"),
    ("extendhfsf2", "K_32_32", "F16", "F32", "FAM_EXTEND"),
    ("extendhfdf2", "K_64_32", "F16", "F64", "FAM_EXTEND"),
    ("extendsfdf2", "K_64_32", "F32", "F64", "FAM_EXTEND"),
    ("truncsfhf2", "K_32_32", "F32", "F16", "FAM_TRUNC"),
    ("truncdfhf2", "K_32_64", "F64", "F16", "FAM_TRUNC"),
    ("truncdfsf2", "K_32_64", "F64", "F32", "FAM_TRUNC"),
    ("fixsfsi", "K_32_32", "F32", "I32", "FAM_FIX"),
    ("fixsfdi", "K_64_32", "F32", "I64", "FAM_FIX"),
    ("fixdfsi", "K_32_64", "F64", "I32", "FAM_FIX"),
    ("fixdfdi", "K_64_64", "F64", "I64", "FAM_FIX"),
    ("fixunssfsi", "K_32_32", "F32", "U32", "FAM_FIX"),
    ("fixunssfdi", "K_64_32", "F32", "U64", "FAM_FIX"),
    ("fixunsdfsi", "K_32_64", "F64", "U32", "FAM_FIX"),
    ("fixunsdfdi", "K_64_64", "F64", "U64", "FAM_FIX"),
    ("floatsisf", "K_32_32", "I32", "F32", "FAM_FLOAT"),
    ("floatdisf", "K_32_64", "I64", "F32", "FAM_FLOAT"),
    ("floatsidf", "K_64_32", "I32", "F64", "FAM_FLOAT"),
    ("floatdidf", "K_64_64", "I64", "F64", "FAM_FLOAT"),
    ("floatunsisf", "K_32_32", "U32", "F32", "FAM_FLOAT"),
    ("floatundisf", "K_32_64", "U64", "F32", "FAM_FLOAT"),
    ("floatunsidf", "K_64_32", "U32", "F64", "FAM_FLOAT"),
    ("floatundidf", "K_64_64", "U64", "F64", "FAM_FLOAT"),
]

PREFIX = {"legacy": "legacy_", "libc": "libc_"}

# RTABI helpers the libc objects call, and the builtin each one aliases in
# the legacy .c sources (the libc .cpp sources do not define these aliases).
AEABI_TO_BUILTIN = {
    "__aeabi_fadd": "addsf3", "__aeabi_fsub": "subsf3",
    "__aeabi_fmul": "mulsf3", "__aeabi_fdiv": "divsf3",
    "__aeabi_dadd": "adddf3", "__aeabi_dsub": "subdf3",
    "__aeabi_dmul": "muldf3", "__aeabi_ddiv": "divdf3",
    "__aeabi_ui2f": "floatunsisf", "__aeabi_ui2d": "floatunsidf",
    "__aeabi_i2f": "floatsisf", "__aeabi_i2d": "floatsidf",
    "__aeabi_f2d": "extendsfdf2", "__aeabi_d2f": "truncdfsf2",
}


def log(msg):
    print(f"== {msg}", file=sys.stderr)


def run(cmd, **kw):
    if isinstance(cmd, str):
        cmd = shlex.split(cmd)
    r = subprocess.run(cmd, text=True, capture_output=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(f"FAILED: {shlex.join(cmd)}\n{r.stdout}{r.stderr}")
        sys.exit(1)
    return r.stdout


def find_llvm_src():
    env = os.environ.get("LLVM_SRC")
    if env:
        return Path(env).resolve()
    for p in HERE.parents:
        if (p / "compiler-rt").is_dir() and (p / "libc").is_dir():
            return p
    sys.exit("ERROR: set LLVM_SRC to an llvm-project checkout")


def configure(src, mode):
    d = BUILD / f"crt-{mode}"
    if (d / "build.ninja").exists():
        return d
    flag = "ON" if mode == "libc" else "OFF"
    log(f"configure compiler-rt builtins ({mode}, USE_LIBC_MATH={flag})")
    tool = lambda t: shutil.which(t) or t
    args = [
        "cmake", "-S", str(src / "compiler-rt/lib/builtins"), "-B", str(d),
        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_SYSTEM_NAME=Generic", "-DCMAKE_SYSTEM_PROCESSOR=arm",
        "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
        "-DCMAKE_ASM_COMPILER=clang",
        f"-DCMAKE_AR={tool('llvm-ar')}", f"-DCMAKE_RANLIB={tool('llvm-ranlib')}",
        f"-DCMAKE_NM={tool('llvm-nm')}",
        "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
        "-DCOMPILER_RT_BAREMETAL_BUILD=ON",
        "-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON",
        "-DCOMPILER_RT_OS_DIR=baremetal",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        f"-DCOMPILER_RT_USE_LIBC_MATH={flag}",
    ]
    for lang in ("C", "CXX", "ASM"):
        args += [f"-DCMAKE_{lang}_COMPILER_TARGET={TARGET}",
                 f"-DCMAKE_{lang}_FLAGS={CPU_FLAGS}"]
    run(args)
    return d


def template(builddir, suffix):
    """Compile command (argv) of a generic builtin source with this suffix."""
    for e in json.loads((builddir / "compile_commands.json").read_text()):
        f = e["file"]
        if f.endswith("/negsf2" + suffix):
            return shlex.split(e["command"]), f
    sys.exit(f"ERROR: no negsf2{suffix} entry in {builddir}")


def compile_variant(argv, tmpl_src, src, out, extra=()):
    argv = list(argv)
    argv[argv.index(tmpl_src)] = str(src)
    argv[argv.index("-o") + 1] = str(out)
    argv[1:1] = list(extra)
    run(argv)


def defined_globals(obj, prefix="__"):
    out = run(["llvm-nm", "--defined-only", "-g", "-j", str(obj)])
    return {s for s in out.split() if s.startswith(prefix)}


def text_size(obj):
    total = 0
    for line in run(["llvm-size", "-A", "-d", str(obj)]).splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".text"):
            total += int(parts[1])
    return total


def archive_members(archive):
    """symbol -> defining member name, for the shipped archive."""
    owner = {}
    member = None
    for line in run(["llvm-nm", "--defined-only", "-g", str(archive)]).splitlines():
        if line.endswith(":"):
            member = line[:-1]
        elif line.strip():
            owner[line.split()[-1]] = member
    return owner


def main():
    src = find_llvm_src()
    BUILD.mkdir(exist_ok=True)

    legacy_dir = configure(src, "legacy")
    libc_dir = configure(src, "libc")
    log("build shipped archive (legacy)")
    run(["cmake", "--build", str(legacy_dir), "--target", "clang_rt.builtins-armv8m.main"])
    archive = next(legacy_dir.rglob("libclang_rt.builtins*.a"))

    c_argv, c_tmpl = template(legacy_dir, ".c")
    cpp_argv, cpp_tmpl = template(libc_dir, ".cpp")
    builtins_dir = src / "compiler-rt/lib/builtins"
    # Only builtins that have a libc-backed .cpp in this checkout.
    active = [b for b in BUILTINS if (builtins_dir / f"{b[0]}.cpp").exists()]
    for b in BUILTINS:
        if b not in active:
            log(f"skip {b[0]}: no {b[0]}.cpp in this checkout")

    objs = {"legacy": [], "libc": []}
    sizes = {}
    owner = archive_members(archive)
    extract = BUILD / "asm-members"
    extract.mkdir(exist_ok=True)

    for name, *_ in active:
        for mode, ext, argv, tmpl, extra in (
            ("legacy", ".c", c_argv, c_tmpl, ()),
            # stub-include: bare metal has no <assert.h>/<fenv.h>/... .
            # -include: extendsfdf2.cpp/truncdfsf2.cpp include the libc
            # header before fp_libc_config.h, so force the intended config.
            ("libc", ".cpp", cpp_argv, cpp_tmpl,
             ("-isystem", str(HARNESS / "stub-include"),
              "-include", str(builtins_dir / "fp_libc_config.h"))),
        ):
            d = BUILD / mode
            d.mkdir(exist_ok=True)
            obj = d / f"{name}.o"
            log(f"compile {mode} {name}")
            compile_variant(argv, tmpl, builtins_dir / f"{name}{ext}", obj, extra)
            objs[mode].append(obj)
            sizes[(name, mode)] = text_size(obj)
        member = owner.get(f"__{name}")
        if member and member.endswith(".S.obj"):
            run(["llvm-ar", "x", f"--output={extract}", str(archive), member])
            sizes[(name, "asm")] = text_size(extract / member)
            sizes[(name, "asm-member")] = member

    # Closed-world copy of the libc objects: their own soft-float calls
    # (__aeabi_fadd, ...) bind back to the libc builtins instead of the
    # shipped asm, as on a target with no arm/*.S. Packed in an archive so
    # only what the recursion probe reaches gets linked.
    closed = BUILD / "libc-closed"
    closed.mkdir(exist_ok=True)
    for o in objs["libc"]:
        shutil.copy(o, closed / o.name)
    closed_objs = sorted(closed.glob("*.o"))
    # All globals, including the weak libc template instances: otherwise the
    # linker keeps the libc variant's copies, which call the shipped asm.
    syms = set().union(*(defined_globals(o, "") for o in closed_objs))
    remap = {s: f"libcc_{s}" for s in syms}
    remap.update({a: f"libcc___{b}" for a, b in AEABI_TO_BUILTIN.items()})
    mapfile = BUILD / "libc-closed.redefine"
    mapfile.write_text("".join(f"{k} {v}\n" for k, v in sorted(remap.items())))
    for o in closed_objs:
        run(["llvm-objcopy", f"--redefine-syms={mapfile}", str(o)])
    closed_ar = BUILD / "libc-closed.a"
    closed_ar.unlink(missing_ok=True)
    run(["llvm-ar", "rcs", str(closed_ar), *map(str, closed_objs)])

    # Rename every "__*" global a variant defines, in all of that variant's
    # objects, so e.g. legacy subdf3 still calls legacy adddf3.
    for mode, files in objs.items():
        syms = set().union(*(defined_globals(o) for o in files))
        mapfile = BUILD / f"{mode}.redefine"
        mapfile.write_text("".join(f"{s} {PREFIX[mode]}{s}\n" for s in sorted(syms)))
        for o in files:
            run(["llvm-objcopy", f"--redefine-syms={mapfile}", str(o)])

    gen = ["// Generated by scripts/build.py.", "#define BUILTINS \\"]
    for name, kind, fin, fout, fam in active:
        has_asm = int((name, "asm") in sizes)
        gen.append(f"  BUILTIN({name}, {kind}, {fin}, {fout}, {fam}, {has_asm}) \\")
    gen.append("")
    (BUILD / "builtins_gen.h").write_text("\n".join(gen) + "\n")

    log("build harness")
    cflags = (f"--target={TARGET} {CPU_FLAGS} -O2 -ffreestanding -fno-builtin "
              f"-Wall -Wextra -I{BUILD}")
    run(f"clang {cflags} -c {HARNESS / 'start.S'} -o {BUILD / 'start.o'}")
    run(f"clang {cflags} -std=c11 -c {HARNESS / 'bench.c'} -o {BUILD / 'bench.o'}")
    elf = BUILD / "fw.elf"
    run(["ld.lld", "-T", str(HARNESS / "link.ld"),
         str(BUILD / "start.o"), str(BUILD / "bench.o"),
         *map(str, objs["legacy"]), *map(str, objs["libc"]),
         str(closed_ar), str(archive),
         "-o", str(elf), "-Map", str(BUILD / "fw.map")])

    picotool = os.environ.get(
        "PICOTOOL", str(HERE / "third_party/picotool-build/picotool"))
    if shutil.which(picotool):
        run([picotool, "uf2", "convert", str(elf), str(BUILD / "fw.uf2"),
             "--family", "rp2350-arm-s"])

    with open(BUILD / "sizes.tsv", "w") as f:
        for name, *_ in active:
            for mode in ("asm", "legacy", "libc"):
                if (name, mode) in sizes:
                    f.write(f"{name}\t{mode}\t{sizes[(name, mode)]}\t"
                            f"{sizes.get((name, 'asm-member'), '-') if mode == 'asm' else '-'}\n")

    log(f"image: {elf}")
    print(run(["llvm-size", str(elf)]), file=sys.stderr)


if __name__ == "__main__":
    main()
