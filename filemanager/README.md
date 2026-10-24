# DoomCPU File Manager

A keyboard-controlled RV32I guest program for DoomCPU. It browses the host-side `filemanager/` directory, opens subdirectories, and launches `.elf` files as nested guest programs.

## Host bridge requirement

The host emulator needs two extra ECALL services. From the DoomCPU repository root, run:

```bash
python3 filemanager/patch_host.py
```

This edits `src/cpu.c` and creates `src/cpu.c.before-filemanager` as a backup. The added services are:

- `a7=38`: `a0=directory path`, `a1=output buffer`, `a2=capacity`; returns the number of bytes written as NUL-terminated records. Each record begins with a type byte (`D` directory, `E` ELF, `F` ordinary file, `X` other), followed by the name and a NUL byte.
- `a7=39`: `a0=relative guest ELF path`; executes that guest in separate 64 MiB RAM and returns when it exits.

## Build

Run these commands from the DoomCPU repository root (Debian/Ubuntu dependencies: `gcc-riscv64-unknown-elf`, binutils, and Picolibc are needed):

```bash
mkdir -p filemanager
python3 filemanager/patch_host.py

gcc -D_POSIX_C_SOURCE=200809L -DDOOMCPU_SDL \
  -std=c11 -O2 -Wall -Wextra \
  $(pkg-config --cflags sdl2) -o doomcpu src/cpu.c \
  $(pkg-config --libs sdl2)

riscv64-unknown-elf-gcc \
  -march=rv32i -mabi=ilp32 -Os \
  -ffreestanding -fno-builtin -msmall-data-limit=0 \
  -ffunction-sections -fdata-sections \
  -nostdlib -nostartfiles \
  -T doomcpu.ld -Wl,-e,_start \
  -Wl,--gc-sections -Wl,--build-id=none \
  -o filemanager/filemanager.elf \
  libc-start.S filemanager/filemanager.c -lgcc
```

Put any DoomCPU-compatible guest ELF files into `filemanager/`. Launch the manager from the repository root:

```bash
./doomcpu filemanager/filemanager.elf
```

## Controls

- Up/Down or W/S: choose an entry
- Enter or Right: open a directory or run an `.elf`
- Left: go to the parent directory
- Esc or Q: exit the manager

Only trusted RV32I guest programs should be launched. A guest program with unsupported instructions or a CPU fault can still stop the host emulator; the nested-launch feature is a prototype, not a security sandbox. The emulator's current working directory must be the DoomCPU repository root so paths under `filemanager/` resolve correctly.
