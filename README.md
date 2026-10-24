# DoomCPU

DoomCPU is a small RV32I CPU emulator written in C. It executes 32-bit RISC-V guest programs using an instruction interpreter, with 64 MiB of guest RAM and a custom ECALL interface for host services.

The project started as a tiny CPU interpreter and grew into a little virtual computer that can run DoomGeneric with a Doom or Freedoom IWAD, plus a small Bad Apple video player. SDL2 provides the host window, keyboard input, and audio output. The game and video player are compiled as RISC-V guest ELF files, not as native Linux applications.

> DoomCPU is a software CPU implementation, not a physical processor. It implements a useful subset of RV32I instruction behavior, but it is not a complete, certified RISC-V platform and does not currently provide the privileged architecture needed to boot a standard Linux kernel.

## Features

- RV32I guest instruction interpreter
- 32 integer registers and a 32-bit program counter
- 64 MiB of emulated RAM
- ELF32 little-endian RISC-V guest loader
- Host ECALL bridge for console output, file operations, timing, keyboard input, video, and audio
- SDL2 display using a 320 x 200 DoomGeneric framebuffer
- DoomGeneric support with Freedoom and compatible Doom IWADs
- A guest Bad Apple player with grayscale video and PCM soundtrack support

## Requirements

These instructions are written for Debian 13 (Trixie), including Debian in WSL2 or a container. Run the command blocks in Bash, not PowerShell. Other distributions may use different package names or install the RISC-V toolchain files in different locations.

Use `sudo` for package installation when working as a normal user. If you are already root (for example, inside a Docker container), omit `sudo` from the package commands.

### Install host dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential gcc git pkg-config ca-certificates curl wget unzip \
  libsdl2-dev ffmpeg python3 pipx \
  gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
  picolibc-riscv64-unknown-elf
```

Use `pipx`, not `python3-pipx`, for Debian 13. The `python3-pipx` package name does not exist in the Debian 13 package archive.

SDL_mixer and a General MIDI SoundFont are useful for the optional MIDI music bridge:

```bash
sudo apt install -y libsdl2-mixer-dev timidity freepats fluid-soundfont-gm
```

The guest toolchain needs a RISC-V bare-metal compiler, binutils, and Picolibc. Confirm the compiler and libraries are installed before building:

```bash
command -v riscv64-unknown-elf-gcc
command -v riscv64-unknown-elf-size
test -f /usr/lib/picolibc/riscv64-unknown-elf/picolibc.specs && echo "Picolibc specs found" || { echo "Picolibc specs file not found; locate it and update the --specs path below." >&2; exit 1; }
```

The package names and specs path above are for Debian 13 (Trixie). On another distribution, install its equivalent packages and update the `--specs` argument in the guest build command if Picolibc installs the specs file somewhere else.

### Expected project layout

The commands below assume the repository is at `~/doomcpu` and includes the following files:

```text
~/doomcpu/
  src/cpu.c
  doomcpu.ld
  libc-start.S
  doomcpu-libc.c
  doomcpu-platform.c
  doomcpu-sound.c
  doomcpu-main.c
  badapple.c
  third_party/doomgeneric/doomgeneric/
```

Depending on which optional music changes you have applied, you may also have `doomcpu-music.c` and a MIDI-enabled host bridge in `src/cpu.c`.

## Build the DoomCPU host emulator

The host emulator uses SDL2. From the repository root:

```bash
cd ~/doomcpu

gcc -D_POSIX_C_SOURCE=200809L -DDOOMCPU_SDL \
  -std=c11 -O2 -Wall -Wextra \
  $(pkg-config --cflags sdl2) \
  -o doomcpu src/cpu.c \
  $(pkg-config --libs sdl2)
```

If you applied the optional SDL_mixer MIDI bridge in `src/cpu.c`, compile with its feature define and SDL_mixer flags instead:

```bash
cd ~/doomcpu

gcc -D_POSIX_C_SOURCE=200809L \
  -DDOOMCPU_SDL -DDOOMCPU_SDL_MUSIC \
  -std=c11 -O2 -Wall -Wextra \
  $(pkg-config --cflags sdl2 SDL2_mixer) \
  -o doomcpu src/cpu.c \
  $(pkg-config --libs sdl2 SDL2_mixer)
```

Use only the second command if your `src/cpu.c` contains the MIDI ECALL implementation and SDL_mixer music code. The ordinary SDL2 audio bridge is sufficient for PCM playback and Doom sound effects.

## Run Doom or Freedoom with sound

### 1. Get an IWAD

Freedoom is a free replacement for Doom's game data. Obtain a Freedoom IWAD from the official Freedoom site and copy it into `~/doomcpu`. For example, the Phase 1 file used during development was named `freedoom1.wad`.

You can also use a legally obtained Doom IWAD. DoomCPU does not distribute commercial Doom game data.

Check that your IWAD is in the current directory:

```bash
cd ~/doomcpu
ls -lh freedoom1.wad
```

### 2. Build the guest Doom executable

The DoomGeneric sources are in `third_party/doomgeneric/doomgeneric`. This command obtains the upstream source list, excludes its native X11 backend, and compiles the engine for RV32I.

```bash
cd ~/doomcpu
D="$PWD/third_party/doomgeneric/doomgeneric"
cd "$D"

mapfile -t DOOM_SOURCES < <(
  sed -n 's/^SRC_DOOM[[:space:]]*=[[:space:]]*//p' Makefile |
  tr ' ' '\n' |
  sed '/^[[:space:]]*$/d; s/\.o$/.c/' |
  grep -v '^doomgeneric_xlib\.c$'
)

if [ "${#DOOM_SOURCES[@]}" -eq 0 ]; then
  echo "Could not extract SRC_DOOM from the DoomGeneric Makefile. Check that upstream Makefile before continuing." >&2
  exit 1
fi

cd ~/doomcpu

# If the optional guest MIDI module exists, include it and Doom's MUS converter.
MUSIC_SOURCES=()
if [ -f doomcpu-music.c ]; then
  MUSIC_SOURCES=(doomcpu-music.c "$D/mus2mid.c")
fi

riscv64-unknown-elf-gcc \
  -march=rv32i -mabi=ilp32 -Os \
  -ffreestanding -fno-builtin -msmall-data-limit=0 \
  -ffunction-sections -fdata-sections \
  -DNORMALUNIX -D_DEFAULT_SOURCE \
  -DFEATURE_SOUND -DDOOMCPU_SOUND \
  -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
  -Ithird_party/doomgeneric/doomgeneric \
  -nostartfiles \
  --specs=/usr/lib/picolibc/riscv64-unknown-elf/picolibc.specs \
  -T doomcpu.ld -Wl,-e,_start -Wl,--gc-sections \
  -Wl,--defsym=__flash=0x00010000 \
  -Wl,--defsym=__flash_size=0x03FF0000 \
  -Wl,--defsym=__ram=0x00800000 \
  -Wl,--defsym=__ram_size=0x03600000 \
  -Wl,--defsym=__heap_start=0x00800000 \
  -Wl,--defsym=__heap_end=0x03E00000 \
  -Wl,--build-id=none \
  -Wl,-Map=doomcpu.map \
  -o doomcpu.elf \
  libc-start.S doomcpu-libc.c doomcpu-platform.c \
  doomcpu-sound.c "${MUSIC_SOURCES[@]}" doomcpu-main.c \
  "${DOOM_SOURCES[@]/#/$D/}" -lm -lgcc
```

This enables Doom's sound-effects path. It sends mixed PCM to the host SDL audio device. Background MIDI music is optional. If `doomcpu-music.c` is present and you want music, ensure `doomcpu-main.c` does not pass `-nomusic`, and build the host emulator with the SDL_mixer MIDI bridge described above. MIDI playback is an optional experimental extension, so it may need debugging on some systems.

### 3. Configure the WAD name

The example `doomcpu-main.c` passes `freedoom1.wad` to DoomGeneric. Ensure that file exists in the current directory. To use a different WAD, change the filename in `doomcpu-main.c`, then rebuild `doomcpu.elf`.

### 4. Launch

```bash
cd ~/doomcpu
./doomcpu doomcpu.elf
```

The SDL window should appear. Use the keyboard mappings implemented in `src/cpu.c`. The current mapping includes arrows for movement/turning, Ctrl for fire, Space for use, Alt for strafe, Shift for running, and Esc for the menu.

DoomCPU's default instruction limit is configured in `src/cpu.c`. You can override it for one run:

```bash
./doomcpu --max-steps 100000000000000 doomcpu.elf
```

If you use a different IWAD, confirm that its filename matches the one in `doomcpu-main.c`.

## Run Bad Apple with sound

The Bad Apple player is a separate guest RISC-V program. It reads raw grayscale video frames and PCM audio from host files through DoomCPU's ECALL bridge. FFmpeg prepares the data before playback. The player does not decode MP4 inside the guest CPU.

### 1. Install yt-dlp and Deno

```bash
pipx install yt-dlp || pipx upgrade yt-dlp
pipx inject yt-dlp yt-dlp-ejs
curl -fsSL https://deno.land/install.sh | sh
export PATH="$HOME/.deno/bin:$HOME/.local/bin:$PATH"
```

If you open a new shell later, run the `export PATH` command again, or add it to `~/.bashrc`.

### 2. Download a source video

```bash
cd ~/doomcpu

yt-dlp \
  --js-runtimes deno \
  --remote-components ejs:npm \
  -f 'bv*[height<=480]+ba/b[height<=480]' \
  --merge-output-format mp4 \
  -o 'bad_apple.%(ext)s' \
  'https://www.youtube.com/watch?v=FtutLA63Cp8'
```

The download requires working network access. YouTube may change its delivery requirements and yt-dlp may need updating. This project does not include or redistribute the source video.

### 3. Convert video to guest-readable frames

This creates 320 x 200 grayscale frames at 15 FPS. The raw frame file can be around 200 MB, depending on the video duration.

```bash
cd ~/doomcpu

ffmpeg -y -hide_banner -loglevel warning \
  -i bad_apple.mp4 -an \
  -vf 'fps=15,scale=320:200:force_original_aspect_ratio=decrease:force_divisible_by=2,pad=320:200:(ow-iw)/2:(oh-ih)/2,format=gray' \
  -f rawvideo -pix_fmt gray \
  badapple.gray
```

### 4. Convert audio to PCM

```bash
cd ~/doomcpu

ffmpeg -y -hide_banner -loglevel warning \
  -i bad_apple.mp4 -vn \
  -ac 2 -ar 22050 -c:a pcm_s16le -f s16le \
  badapple.pcm
```

### 5. Compile the player

Make sure `badapple.c` uses the DoomCPU host-call service IDs from your current `src/cpu.c`, including file access (services 11 to 13), video (service 20), audio initialization and queueing (services 25 and 26), and timing (services 22 and 23).

```bash
cd ~/doomcpu

riscv64-unknown-elf-gcc \
  -march=rv32i -mabi=ilp32 -Os \
  -ffreestanding -fno-builtin -msmall-data-limit=0 \
  -ffunction-sections -fdata-sections \
  -nostdlib -nostartfiles \
  -T doomcpu.ld -Wl,-e,_start \
  -Wl,--gc-sections -Wl,--build-id=none \
  -o badapple.elf \
  libc-start.S badapple.c -lgcc
```

RV32I has no hardware integer divide instruction. The build command explicitly links `-lgcc` so GCC runtime helpers such as `__udivsi3` are available if the compiler emits calls to them.

### 6. Play

```bash
cd ~/doomcpu
./doomcpu badapple.elf
```

A window titled `Bad Apple - DoomCPU` should display the black-and-white animation and play the PCM soundtrack through SDL audio. Press `q` or Esc if your current `badapple.c` includes the quit-key handling.
## Run simple snake game
### 1. Compile
```bash
cd ~/doomcpu

riscv64-unknown-elf-gcc \
  -march=rv32i -mabi=ilp32 -Os \
  -ffreestanding -fno-builtin -msmall-data-limit=0 \
  -ffunction-sections -fdata-sections \
  -nostdlib -nostartfiles \
  -T doomcpu.ld -Wl,-e,_start \
  -Wl,--gc-sections -Wl,--build-id=none \
  -o snake.elf \
  libc-start.S snake.c -lgcc
```
### 2. Play
```bash
./doomcpu snake.elf
```
## A file-manager for easy program/games opening
### 1. Compile
```bash
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
### 2. Run
```bash
./doomcpu filemanager/filemanager.elf
```
## Troubleshooting

### SDL window or audio device does not open

Run DoomCPU from a graphical WSL session with SDL2 support. Check the host audio device and make sure the emulator was rebuilt after changes to `src/cpu.c`.

### `bad_apple.gray` or `badapple.pcm` cannot be opened

Run the emulator from `~/doomcpu`, and verify both files exist there. The guest player uses relative paths.

### Doom cannot find the IWAD

Run from `~/doomcpu`, check the WAD filename, and make sure `doomcpu-main.c` uses that exact name.

### `undefined reference to __udivsi3`

The guest is using RV32I, which has no `DIV` instruction. The Bad Apple build command includes `-lgcc` for compiler runtime helpers. If this error persists, verify that `riscv64-unknown-elf-gcc` is the compiler being used and that its matching runtime library is installed.

### MIDI music is silent

PCM sound effects and MIDI music use different paths. Confirm that the host binary was compiled with `DOOMCPU_SDL_MUSIC`, SDL_mixer reports MIDI support, a General MIDI SoundFont is installed, `doomcpu-music.c` is linked into the guest, and `-nomusic` is not present in `doomcpu-main.c`.

## Project status

DoomCPU is an experimental hobby project. Doom/Freedoom gameplay and Bad Apple video/audio playback have been tested in the described setup. Support for the full RISC-V privileged architecture, standard Linux boot protocols, and complete ISA compliance is not implemented. MIDI music support is an optional extension and may require further debugging on some systems.

## Credits

- [DoomGeneric](https://github.com/ozkl/doomgeneric) for the portable Doom engine wrapper
- [Freedoom](https://freedoom.github.io/) for free Doom-compatible game data
- [RISC-V International](https://riscv.org/) for the RISC-V instruction set architecture
- SDL2 for host-side video, input, and audio facilities
