# compiler-rt soft-float builtins on a Raspberry Pi Pico 2

Benchmarks compiler-rt's float builtins on an RP2350 (Cortex-M33,
`-mfloat-abi=soft`). Each builtin is measured in three versions:

- `asm`: `arm/<name>.S`, what Arm ships today
- `legacy`: `<name>.c`
- `libc`: `<name>.cpp`, LLVM-libc backed

Reports cycles/op, libc vs legacy/asm result mismatches, and code size.

## Run

Needs `clang`, `lld`, LLVM binutils, `cmake`, `ninja`, `python3`, `libusb`.

Install picotool's udev rule once:

```sh
sudo cp third_party/picotool/udev/60-picotool.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Hold BOOTSEL, plug in the Pico 2, then:

```sh
./run-bench
```

Results go to `results/<date>-<llvm-sha>.md`. It benchmarks the llvm-project
checkout this directory lives in (override with `LLVM_SRC`).

## Notes

- The firmware runs from SRAM and stores results in the last 64K of flash.
  That area is restored afterwards.
- `harness/stub-include/` stands in for the libc headers bare metal lacks.
- To add a builtin, add a row to `BUILTINS` in `scripts/build.py`.
