# DoomCPU File Manager

A keyboard-controlled RV32I guest program for DoomCPU. It browses the host-side `filemanager/` directory, opens subdirectories, and launches `.elf` files as nested guest programs.

## Build

Run these commands from the DoomCPU repository root (Debian/Ubuntu dependencies: `gcc-riscv64-unknown-elf`, binutils, and Picolibc are needed):

```bash

gcc -D_POSIX_C_SOURCE=200809L \
  -DDOOMCPU_SDL -DDOOMCPU_SDL_MUSIC \
  -std=c11 -O2 -Wall -Wextra \
  $(pkg-config --cflags sdl2 SDL2_mixer) \
  -o doomcpu src/cpu.c \
  $(pkg-config --libs sdl2 SDL2_mixer)

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
