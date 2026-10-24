#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#ifdef DOOMCPU_SDL
#include <SDL2/SDL.h>
#ifdef DOOMCPU_SDL_MUSIC
#include <SDL2/SDL_mixer.h>
#endif
#endif


#define RAM_SIZE (64u * 1024u * 1024u)
#define DEFAULT_MAX_STEPS 100000000000000ull

static int trace_enabled;
static struct CPU *active_cpu;
static uint64_t max_steps = DEFAULT_MAX_STEPS;

typedef struct CPU {
    uint32_t x[32];
    uint32_t pc;
    uint8_t ram[RAM_SIZE];
    uint64_t steps;
    int halted;
} CPU;

static uint32_t sext(uint32_t v, unsigned bits) {
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

static void fault(const char *msg, uint32_t addr) {
    fprintf(stderr, "CPU fault: %s at 0x%08X\n", msg, addr);
    exit(1);
}

static uint32_t read_mem(CPU *c, uint32_t addr, unsigned n) {
    if (addr > RAM_SIZE || n > RAM_SIZE - addr)
        fault("memory read out of range", addr);

    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint32_t)c->ram[addr + i] << (8 * i);
    return v;
}

static void write_mem(CPU *c, uint32_t addr, uint32_t v, unsigned n) {
    if (addr > RAM_SIZE || n > RAM_SIZE - addr)
        fault("memory write out of range", addr);

    for (unsigned i = 0; i < n; i++)
        c->ram[addr + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t fetch32(CPU *c) {
    if ((c->pc & 3) != 0)
        fault("misaligned instruction address", c->pc);

    uint32_t v = read_mem(c, c->pc, 4);
    c->pc += 4;
    return v;
}

/* ELF32 loader for little-endian RISC-V executables. */
static uint32_t load_elf(CPU *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }

    Elf32_Ehdr eh;
    if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh))
        fault("cannot read ELF header", 0);

    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS32 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_ident[EI_VERSION] != EV_CURRENT ||
        eh.e_type != ET_EXEC ||
        eh.e_version != EV_CURRENT ||
        eh.e_ehsize != sizeof(Elf32_Ehdr) ||
        eh.e_machine != EM_RISCV ||
        eh.e_phentsize != sizeof(Elf32_Phdr)) {
        fprintf(stderr, "Not a supported ELF32 little-endian RISC-V file\n");
        exit(1);
    }

    if (fseek(f, 0, SEEK_END) != 0)
        fault("cannot seek ELF file", 0);
    long file_size = ftell(f);
    if (file_size < 0)
        fault("cannot determine ELF file size", 0);

    if ((uint64_t)eh.e_phoff +
        (uint64_t)eh.e_phnum * eh.e_phentsize > (uint64_t)file_size)
        fault("ELF program header table outside file", eh.e_phoff);

    int entry_is_executable = 0;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        uint64_t off = (uint64_t)eh.e_phoff +
                       (uint64_t)i * eh.e_phentsize;

        if (off + sizeof(Elf32_Phdr) > (uint64_t)file_size)
            fault("invalid ELF program header", (uint32_t)off);

        if (fseek(f, (long)off, SEEK_SET) != 0)
            fault("cannot seek ELF program header", (uint32_t)off);

        Elf32_Phdr ph;
        if (fread(&ph, 1, sizeof(ph), f) != sizeof(ph))
            fault("cannot read ELF program header", (uint32_t)off);

        if (ph.p_type != PT_LOAD)
            continue;

        if ((ph.p_flags & PF_X) &&
            eh.e_entry >= ph.p_vaddr &&
            (uint64_t)eh.e_entry < (uint64_t)ph.p_vaddr + ph.p_memsz)
            entry_is_executable = 1;

        if (ph.p_filesz > ph.p_memsz ||
            (uint64_t)ph.p_offset + ph.p_filesz > (uint64_t)file_size ||
            ph.p_vaddr > RAM_SIZE ||
            ph.p_memsz > RAM_SIZE - ph.p_vaddr) {
            fault("ELF segment outside virtual RAM", ph.p_vaddr);
        }

        if (fseek(f, ph.p_offset, SEEK_SET) != 0)
            fault("cannot seek ELF segment", ph.p_offset);

        if (fread(&c->ram[ph.p_vaddr], 1, ph.p_filesz, f)
            != ph.p_filesz)
            fault("cannot load ELF segment", ph.p_vaddr);

        /* RAM is initially zero, so the remainder is BSS. */
    }

    fclose(f);

    if ((eh.e_entry & 3) != 0 || eh.e_entry >= RAM_SIZE ||
        !entry_is_executable)
        fault("invalid or non-executable ELF entry point", eh.e_entry);

    return eh.e_entry;
}


/* Guest addresses are offsets into the emulated RAM array. */
static int guest_span(uint32_t addr, uint32_t n)
{
    return addr <= RAM_SIZE && n <= RAM_SIZE - addr;
}

static int guest_string(CPU *c, uint32_t addr, char *dst, size_t cap)
{
    size_t i;

    if (addr >= RAM_SIZE || cap == 0)
        return 0;

    for (i = 0; i < cap && (uint64_t)addr + i < RAM_SIZE; ++i) {
        dst[i] = (char)c->ram[addr + i];
        if (dst[i] == '\0')
            return 1;
    }

    return 0;
}

#ifdef DOOMCPU_SDL

#ifdef DOOMCPU_SDL_MUSIC
#define DOOM_MUSIC_SLOTS 64

typedef struct {
    Mix_Music *music;
    unsigned char *buffer;
} DoomMusicSlot;

static DoomMusicSlot doom_music_slots[DOOM_MUSIC_SLOTS];
static int doom_music_ready;
static int doom_mixer_initialized;
static int doom_mixer_audio_open;
static int doom_current_music = -1;

static void doom_music_cleanup(void)
{
    if (doom_mixer_audio_open)
        Mix_HaltMusic();

    doom_current_music = -1;

    for (int i = 0; i < DOOM_MUSIC_SLOTS; ++i) {
        if (doom_music_slots[i].music != NULL)
            Mix_FreeMusic(doom_music_slots[i].music);

        free(doom_music_slots[i].buffer);
        doom_music_slots[i].music = NULL;
        doom_music_slots[i].buffer = NULL;
    }

    if (doom_mixer_audio_open) {
        Mix_CloseAudio();
        doom_mixer_audio_open = 0;
    }

    if (doom_mixer_initialized) {
        Mix_Quit();
        doom_mixer_initialized = 0;
    }

    doom_music_ready = 0;
}
#endif

static SDL_Window *doom_window;
static SDL_AudioDeviceID doom_audio_device;
static SDL_Renderer *doom_renderer;
static SDL_Texture *doom_texture;
static int doom_video_w;
static int doom_video_h;

struct DoomKeyEvent {
    int pressed;
    unsigned char key;
};

static struct DoomKeyEvent doom_keys[128];
static unsigned doom_key_read;
static unsigned doom_key_write;

static int doom_map_key(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:        return 0xad;
    case SDLK_DOWN:      return 0xaf;
    case SDLK_LEFT:      return 0xac;
    case SDLK_RIGHT:     return 0xae;
    case SDLK_ESCAPE:    return 27;
    case SDLK_RETURN:    return 13;
    case SDLK_KP_ENTER:  return 13;
    case SDLK_SPACE:     return 0xa2; /* Doom KEY_USE */
    case SDLK_TAB:       return 9;
    case SDLK_BACKSPACE: return 127;
    case SDLK_LCTRL:
    case SDLK_RCTRL:     return 0xa3; /* Doom KEY_FIRE */
    case SDLK_LALT:
    case SDLK_RALT:      return 0xb8;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:    return 0xb6;
    default:
        if ((sym >= SDLK_a && sym <= SDLK_z) ||
            (sym >= SDLK_0 && sym <= SDLK_9))
            return (int)sym;
        return 0;
    }
}

static void doom_queue_key(int pressed, unsigned char key)
{
    unsigned next = (doom_key_write + 1) % 128;

    if (next == doom_key_read)
        return;

    doom_keys[doom_key_write].pressed = pressed;
    doom_keys[doom_key_write].key = key;
    doom_key_write = next;
}

static int doom_video_init(int w, int h)
{
    if (doom_window != NULL)
        return doom_video_w == w && doom_video_h == h;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "DoomCPU: SDL_Init: %s\n", SDL_GetError());
        return 0;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    doom_window = SDL_CreateWindow(
        "DoomCPU - RV32I",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w * 3, h * 3, SDL_WINDOW_RESIZABLE);

    if (doom_window == NULL) {
        fprintf(stderr, "DoomCPU: SDL window: %s\n", SDL_GetError());
        return 0;
    }

    doom_renderer = SDL_CreateRenderer(
        doom_window, -1, SDL_RENDERER_ACCELERATED);

    if (doom_renderer == NULL)
        doom_renderer = SDL_CreateRenderer(
            doom_window, -1, SDL_RENDERER_SOFTWARE);

    if (doom_renderer == NULL) {
        fprintf(stderr, "DoomCPU: SDL renderer: %s\n", SDL_GetError());
        return 0;
    }

    doom_texture = SDL_CreateTexture(
        doom_renderer, SDL_PIXELFORMAT_RGB888,
        SDL_TEXTUREACCESS_STREAMING, w, h);

    if (doom_texture == NULL) {
        fprintf(stderr, "DoomCPU: SDL texture: %s\n", SDL_GetError());
        return 0;
    }

    SDL_SetTextureBlendMode(doom_texture, SDL_BLENDMODE_NONE);
    SDL_RenderSetLogicalSize(doom_renderer, w, h);

    doom_video_w = w;
    doom_video_h = h;

    fprintf(stderr, "DoomCPU: SDL display ready (%dx%d guest framebuffer)\n",
            w, h);
    return 1;
}

static void doom_poll_events(CPU *c)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            c->halted = 1;
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            int key;

            if (ev.type == SDL_KEYDOWN && ev.key.repeat)
                continue;

            key = doom_map_key(ev.key.keysym.sym);
            if (key != 0)
                doom_queue_key(ev.type == SDL_KEYDOWN, (unsigned char)key);
        }
    }
}

static void doom_video_cleanup(void)
{
    if (doom_audio_device != 0) {
        SDL_CloseAudioDevice(doom_audio_device);
        doom_audio_device = 0;
    }

    if (doom_texture != NULL)
        SDL_DestroyTexture(doom_texture);
    if (doom_renderer != NULL)
        SDL_DestroyRenderer(doom_renderer);
    if (doom_window != NULL)
        SDL_DestroyWindow(doom_window);

    doom_texture = NULL;
    doom_renderer = NULL;
    doom_window = NULL;
#ifdef DOOMCPU_SDL_MUSIC
    doom_music_cleanup();
#endif
    SDL_Quit();
}
#endif


static void ecall(CPU *c) {
    /*
     * DoomCPU host-call convention:
     * a7=1: print signed integer in a0
     * a7=2: print character in a0
     * a7=10: exit
     *
     * These are emulator services, not RV32I instructions.
     */
    switch (c->x[17]) {
    case 1:
        printf("%d\n", (int32_t)c->x[10]);
        break;
    case 2:
        putchar((unsigned char)c->x[10]);
        fflush(stdout);
        break;
    case 10:
        printf("Program exited with code %d\n", (int32_t)c->x[10]);
        c->halted = 1;
        break;
    /*
     * POSIX-like host bridge used by Picolibc:
     * 11 open, 12 close, 13 read, 14 write, 15 lseek,
     * 16 clock/time, 17 mkdir, 18 unlink, 19 rename.
     * SDL bridge: 20 frame/init, 21 keyboard, 22 sleep,
     * 23 monotonic milliseconds, 24 window title.
     * Errors are returned as negative host errno values.
     */
    case 11: {
        char path[4096];
        if (!guest_string(c, c->x[10], path, sizeof(path))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        int fd = open(path, (int)c->x[11], (mode_t)c->x[12]);
        c->x[10] = (uint32_t)(fd < 0 ? -errno : fd);
        break;
    }
    case 12: {
        int r = close((int)c->x[10]);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
    case 13: {
        uint32_t addr = c->x[11];
        uint32_t n = c->x[12];
        if (!guest_span(addr, n)) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        ssize_t r = read((int)c->x[10], c->ram + addr, n);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
    case 14: {
        uint32_t addr = c->x[11];
        uint32_t n = c->x[12];
        if (!guest_span(addr, n)) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        ssize_t r = write((int)c->x[10], c->ram + addr, n);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
    case 15: {
        off_t r = lseek((int)c->x[10],
                        (off_t)(int32_t)c->x[11],
                        (int)c->x[12]);
        c->x[10] = (uint32_t)(r < 0 ? -errno : (int32_t)r);
        break;
    }
    case 16: {
        uint32_t addr = c->x[10];
        struct timespec ts;

        if (!guest_span(addr, 8)) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            c->x[10] = (uint32_t)-errno;
            break;
        }

        write_mem(c, addr, (uint32_t)ts.tv_sec, 4);
        if (c->x[11] == 1)
            write_mem(c, addr + 4, (uint32_t)ts.tv_nsec, 4);
        else
            write_mem(c, addr + 4, (uint32_t)(ts.tv_nsec / 1000), 4);

        c->x[10] = 0;
        break;
    }
    case 17: {
        char path[4096];
        if (!guest_string(c, c->x[10], path, sizeof(path))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        int r = mkdir(path, (mode_t)c->x[11]);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
    case 18: {
        char path[4096];
        if (!guest_string(c, c->x[10], path, sizeof(path))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        int r = unlink(path);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
    case 19: {
        char oldpath[4096], newpath[4096];
        if (!guest_string(c, c->x[10], oldpath, sizeof(oldpath)) ||
            !guest_string(c, c->x[11], newpath, sizeof(newpath))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        int r = rename(oldpath, newpath);
        c->x[10] = (uint32_t)(r < 0 ? -errno : r);
        break;
    }
#ifdef DOOMCPU_SDL
    case 20: {
        uint32_t addr = c->x[10];
        int w = (int)c->x[11], h = (int)c->x[12];

        if (w <= 0 || h <= 0 || w > 1920 || h > 1080 ||
            !doom_video_init(w, h)) {
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        /* addr == 0 means initialize the display without drawing. */
        if (addr != 0) {
            uint64_t bytes = (uint64_t)(unsigned)w *
                             (uint64_t)(unsigned)h * 4u;
            if (bytes > UINT32_MAX ||
                !guest_span(addr, (uint32_t)bytes)) {
                c->x[10] = (uint32_t)-EFAULT;
                break;
            }

            SDL_UpdateTexture(doom_texture, NULL, c->ram + addr, w * 4);
            SDL_RenderClear(doom_renderer);
            SDL_RenderCopy(doom_renderer, doom_texture, NULL, NULL);
            SDL_RenderPresent(doom_renderer);
        }
        c->x[10] = 0;
        break;
    }
    case 21: {
        uint32_t pressed_addr = c->x[10];
        uint32_t key_addr = c->x[11];

        if (!guest_span(pressed_addr, 4) || !guest_span(key_addr, 1)) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }

        doom_poll_events(c);

        if (doom_key_read != doom_key_write && !c->halted) {
            struct DoomKeyEvent ev = doom_keys[doom_key_read];
            doom_key_read = (doom_key_read + 1) % 128;
            write_mem(c, pressed_addr, (uint32_t)ev.pressed, 4);
            write_mem(c, key_addr, (uint32_t)ev.key, 1);
            c->x[10] = 1;
        } else {
            c->x[10] = 0;
        }
        break;
    }
    case 22: {
        struct timespec ts;
        uint32_t ms = c->x[10];

        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        c->x[10] = 0;
        break;
    }
    case 23: {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            c->x[10] = (uint32_t)-errno;
            break;
        }
        c->x[10] = (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                               (uint64_t)ts.tv_nsec / 1000000u);
        break;
    }
    case 24: {
        char title[1024];
        if (!guest_string(c, c->x[10], title, sizeof(title))) {
            c->x[10] = (uint32_t)-EFAULT;
            break;
        }
        if (doom_window != NULL)
            SDL_SetWindowTitle(doom_window, title);
        c->x[10] = 0;
        break;
    }

    case 25: {
        /* a0 = sample rate, a1 = channels; signed 16-bit little-endian PCM */
        int rate = (int)c->x[10];
        int channels = (int)c->x[11];
        SDL_AudioSpec wanted, obtained;

        if (rate < 8000 || rate > 48000 ||
            (channels != 1 && channels != 2)) {
            c->x[10] = (uint32_t)-EINVAL;
            break;
        }

        if (doom_audio_device != 0) {
            SDL_CloseAudioDevice(doom_audio_device);
            doom_audio_device = 0;
        }

        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
            SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "DoomCPU audio init: %s\n", SDL_GetError());
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        SDL_zero(wanted);
        wanted.freq = rate;
        wanted.format = AUDIO_S16LSB;
        wanted.channels = (Uint8)channels;
        wanted.samples = 1024;
        wanted.callback = NULL;

        doom_audio_device =
            SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);

        if (doom_audio_device == 0) {
            fprintf(stderr, "DoomCPU audio device: %s\n", SDL_GetError());
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        SDL_PauseAudioDevice(doom_audio_device, 0);
        fprintf(stderr, "DoomCPU audio ready: %d Hz, %d channels\n",
                rate, channels);
        c->x[10] = 0;
        break;
    }
    case 26: {
        /* a0 = guest RAM address, a1 = byte count */
        uint32_t addr = c->x[10];
        uint32_t bytes = c->x[11];

        if (doom_audio_device == 0) {
            c->x[10] = (uint32_t)-EIO;
        } else if (!guest_span(addr, bytes)) {
            c->x[10] = (uint32_t)-EFAULT;
        } else if (bytes != 0 &&
                   SDL_QueueAudio(doom_audio_device,
                                  c->ram + addr, bytes) != 0) {
            fprintf(stderr, "DoomCPU queue audio: %s\n", SDL_GetError());
            c->x[10] = (uint32_t)-EIO;
        } else {
            c->x[10] = 0;
        }
        break;
    }
    case 27: {
        if (doom_audio_device != 0) {
            SDL_ClearQueuedAudio(doom_audio_device);
            SDL_CloseAudioDevice(doom_audio_device);
            doom_audio_device = 0;
        }
        c->x[10] = 0;
        break;
    }


#ifdef DOOMCPU_SDL_MUSIC
    case 28: {
        /* Initialize SDL_mixer's MIDI backend. */
        if (doom_music_ready) {
            c->x[10] = 0;
            break;
        }

        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
            SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "DoomCPU MIDI: SDL audio init: %s\n",
                    SDL_GetError());
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        int flags = Mix_Init(MIX_INIT_MID);
        if ((flags & MIX_INIT_MID) == 0) {
            fprintf(stderr, "DoomCPU MIDI: SDL_mixer MIDI support unavailable: %s\n",
                    Mix_GetError());
            Mix_Quit();
            c->x[10] = (uint32_t)-ENOTSUP;
            break;
        }
        doom_mixer_initialized = 1;

        int freq, channels;
        Uint16 format;

        if (!Mix_QuerySpec(&freq, &format, &channels)) {
            if (Mix_OpenAudio(22050, AUDIO_S16SYS, 2, 1024) < 0) {
                fprintf(stderr, "DoomCPU MIDI: opening mixer audio: %s\n",
                        Mix_GetError());
                Mix_Quit();
                doom_mixer_initialized = 0;
                c->x[10] = (uint32_t)-EIO;
                break;
            }
            doom_mixer_audio_open = 1;
        }

        Mix_VolumeMusic(MIX_MAX_VOLUME);
        doom_music_ready = 1;

        fprintf(stderr, "DoomCPU: SDL_mixer MIDI ready\n");
        c->x[10] = 0;
        break;
    }
    case 29: {
        /* Register MIDI bytes from guest RAM. Return a 1-based slot ID. */
        uint32_t addr = c->x[10];
        uint32_t length = c->x[11];

        if (!doom_music_ready) {
            c->x[10] = (uint32_t)-EIO;
            break;
        }
        if (!guest_span(addr, length) ||
            length < 14 || length > 1024u * 1024u) {
            c->x[10] = (uint32_t)-EINVAL;
            break;
        }

        int slot = -1;
        for (int i = 0; i < DOOM_MUSIC_SLOTS; ++i) {
            if (doom_music_slots[i].music == NULL) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            c->x[10] = (uint32_t)-ENOMEM;
            break;
        }

        unsigned char *copy = malloc(length);
        if (copy == NULL) {
            c->x[10] = (uint32_t)-ENOMEM;
            break;
        }

        memcpy(copy, c->ram + addr, length);

        SDL_RWops *rw = SDL_RWFromConstMem(copy, (int)length);
        if (rw == NULL) {
            free(copy);
            c->x[10] = (uint32_t)-ENOMEM;
            break;
        }

        /* Keep copy alive until the song is unregistered. */
        Mix_Music *music = Mix_LoadMUS_RW(rw, 1);
        if (music == NULL) {
            fprintf(stderr, "DoomCPU MIDI: loading converted song: %s\n",
                    Mix_GetError());
            free(copy);
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        doom_music_slots[slot].music = music;
        doom_music_slots[slot].buffer = copy;
        c->x[10] = (uint32_t)(slot + 1);
        break;
    }
    case 30: {
        /* a0 = song ID, a1 = looping */
        int id = (int)c->x[10] - 1;

        if (id < 0 || id >= DOOM_MUSIC_SLOTS ||
            doom_music_slots[id].music == NULL) {
            c->x[10] = (uint32_t)-ENOENT;
            break;
        }

        if (Mix_PlayMusic(doom_music_slots[id].music,
                          c->x[11] ? -1 : 0) < 0) {
            fprintf(stderr, "DoomCPU MIDI: playback failed: %s\n",
                    Mix_GetError());
            c->x[10] = (uint32_t)-EIO;
            break;
        }

        doom_current_music = id;
        c->x[10] = 0;
        break;
    }
    case 31: {
        if (doom_music_ready)
            Mix_HaltMusic();

        doom_current_music = -1;
        c->x[10] = 0;
        break;
    }
    case 32: {
        int id = (int)c->x[10] - 1;

        if (id >= 0 && id < DOOM_MUSIC_SLOTS) {
            if (doom_current_music == id) {
                Mix_HaltMusic();
                doom_current_music = -1;
            }

            if (doom_music_slots[id].music != NULL)
                Mix_FreeMusic(doom_music_slots[id].music);

            free(doom_music_slots[id].buffer);
            doom_music_slots[id].music = NULL;
            doom_music_slots[id].buffer = NULL;
        }

        c->x[10] = 0;
        break;
    }
    case 33: {
        int volume = (int)c->x[10];
        if (volume < 0) volume = 0;
        if (volume > 127) volume = 127;

        if (doom_music_ready)
            Mix_VolumeMusic(volume * MIX_MAX_VOLUME / 127);

        c->x[10] = 0;
        break;
    }
    case 34:
        if (doom_music_ready)
            Mix_PauseMusic();
        c->x[10] = 0;
        break;
    case 35:
        if (doom_music_ready)
            Mix_ResumeMusic();
        c->x[10] = 0;
        break;
    case 36:
        c->x[10] = doom_music_ready && Mix_PlayingMusic() ? 1 : 0;
        break;
    case 37:
        doom_music_cleanup();
        c->x[10] = 0;
        break;
#endif

#endif
    default:
        fprintf(stderr, "Unsupported ECALL service: %u\n", c->x[17]);
        exit(1);
    }
}

static void step(CPU *c) {
    uint32_t ins = fetch32(c);
    uint32_t oldpc = c->pc - 4;
    if (trace_enabled)
        fprintf(stderr, "%08X: %08X\\n", oldpc, ins);

    uint32_t opcode = ins & 0x7f;
    unsigned rd = (ins >> 7) & 31;
    unsigned f3 = (ins >> 12) & 7;
    unsigned rs1 = (ins >> 15) & 31;
    unsigned rs2 = (ins >> 20) & 31;
    unsigned f7 = ins >> 25;
    uint32_t a = c->x[rs1], b = c->x[rs2];
    uint32_t imm_i = sext(ins >> 20, 12);
    uint32_t imm_s = sext(((ins >> 25) << 5) |
                          ((ins >> 7) & 31), 12);
    uint32_t imm_b = sext(((ins >> 31) << 12) |
                          (((ins >> 7) & 1) << 11) |
                          (((ins >> 25) & 63) << 5) |
                          (((ins >> 8) & 15) << 1), 13);

    switch (opcode) {
    case 0x37: /* LUI */
        c->x[rd] = ins & 0xfffff000u;
        break;

    case 0x17: /* AUIPC */
        c->x[rd] = oldpc + (ins & 0xfffff000u);
        break;

    case 0x6f: { /* JAL */
        uint32_t imm = sext(((ins >> 31) << 20) |
                            (((ins >> 12) & 255) << 12) |
                            (((ins >> 20) & 1) << 11) |
                            (((ins >> 21) & 1023) << 1), 21);
        c->x[rd] = c->pc;
        c->pc = oldpc + imm;
        break;
    }

    case 0x67: /* JALR */
        if (f3 != 0) fault("invalid JALR", oldpc);
        {
            uint32_t target = (a + imm_i) & ~1u;
            c->x[rd] = c->pc;
            c->pc = target;
        }
        break;

    case 0x63: /* Conditional branches */
        switch (f3) {
        case 0: if (a == b) c->pc = oldpc + imm_b; break;
        case 1: if (a != b) c->pc = oldpc + imm_b; break;
        case 4: if ((int32_t)a < (int32_t)b) c->pc = oldpc + imm_b; break;
        case 5: if ((int32_t)a >= (int32_t)b) c->pc = oldpc + imm_b; break;
        case 6: if (a < b) c->pc = oldpc + imm_b; break;
        case 7: if (a >= b) c->pc = oldpc + imm_b; break;
        default: fault("invalid branch instruction", oldpc);
        }
        break;

    case 0x03: { /* Loads */
        uint32_t addr = a + imm_i, v;
        switch (f3) {
        case 0: v = sext(read_mem(c, addr, 1), 8); break;  /* LB */
        case 1: v = sext(read_mem(c, addr, 2), 16); break; /* LH */
        case 2: v = read_mem(c, addr, 4); break;           /* LW */
        case 4: v = read_mem(c, addr, 1); break;           /* LBU */
        case 5: v = read_mem(c, addr, 2); break;           /* LHU */
        default: fault("invalid load instruction", oldpc);
        }
        c->x[rd] = v;
        break;
    }

    case 0x23: { /* Stores */
        uint32_t addr = a + imm_s;
        switch (f3) {
        case 0: write_mem(c, addr, b, 1); break;
        case 1: write_mem(c, addr, b, 2); break;
        case 2: write_mem(c, addr, b, 4); break;
        default: fault("invalid store instruction", oldpc);
        }
        break;
    }

    case 0x13: /* Immediate ALU */
        switch (f3) {
        case 0: c->x[rd] = a + imm_i; break;                 /* ADDI */
        case 2: c->x[rd] = (int32_t)a < (int32_t)imm_i; break; /* SLTI */
        case 3: c->x[rd] = a < imm_i; break;                 /* SLTIU */
        case 4: c->x[rd] = a ^ imm_i; break;                 /* XORI */
        case 6: c->x[rd] = a | imm_i; break;                 /* ORI */
        case 7: c->x[rd] = a & imm_i; break;                 /* ANDI */
        case 1:
            if (f7 != 0) fault("invalid SLLI", oldpc);
            c->x[rd] = a << rs2;
            break;
        case 5:
            if (f7 == 0) c->x[rd] = a >> rs2;                /* SRLI */
            else if (f7 == 0x20) c->x[rd] = (uint32_t)((int32_t)a >> rs2); /* SRAI */
            else fault("invalid shift immediate", oldpc);
            break;
        default:
            fault("invalid immediate ALU instruction", oldpc);
        }
        break;

    case 0x33: /* Register ALU */
        switch (f3) {
        case 0:
            if (f7 == 0) c->x[rd] = a + b;                   /* ADD */
            else if (f7 == 0x20) c->x[rd] = a - b;            /* SUB */
            else fault("invalid ADD/SUB", oldpc);
            break;
        case 1:
            if (f7 != 0) fault("invalid SLL", oldpc);
            c->x[rd] = a << (b & 31);
            break;
        case 2:
            if (f7 != 0) fault("invalid SLT", oldpc);
            c->x[rd] = (int32_t)a < (int32_t)b;
            break;
        case 3:
            if (f7 != 0) fault("invalid SLTU", oldpc);
            c->x[rd] = a < b;
            break;
        case 4:
            if (f7 != 0) fault("invalid XOR", oldpc);
            c->x[rd] = a ^ b;
            break;
        case 5:
            if (f7 == 0) c->x[rd] = a >> (b & 31);
            else if (f7 == 0x20) c->x[rd] = (uint32_t)((int32_t)a >> (b & 31));
            else fault("invalid SRL/SRA", oldpc);
            break;
        case 6:
            if (f7 != 0) fault("invalid OR", oldpc);
            c->x[rd] = a | b;
            break;
        case 7:
            if (f7 != 0) fault("invalid AND", oldpc);
            c->x[rd] = a & b;
            break;
        default:
            fault("invalid register ALU instruction", oldpc);
        }
        break;

    case 0x0f: /* FENCE: no-op in this single-hart emulator */
        if (f3 != 0)
            fault("unsupported FENCE variant (FENCE.I is not enabled)", oldpc);
        break;

    case 0x73: /* SYSTEM */
        if (ins == 0x00000073) ecall(c);
        else if (ins == 0x00100073) {
            printf("EBREAK at 0x%08X\n", oldpc);
            c->halted = 1;
        } else fault("unsupported SYSTEM instruction", oldpc);
        break;

    default:
        fault("unsupported opcode", oldpc);
    }

    c->x[0] = 0; /* x0 is hardwired to zero */
}

int main(int argc, char **argv) {
    const char *program = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            trace_enabled = 1;
        } else if (strcmp(argv[i], "--max-steps") == 0) {
            char *end = NULL;
            if (++i >= argc) {
                fprintf(stderr, "--max-steps requires a number\n");
                return 2;
            }
            max_steps = strtoull(argv[i], &end, 10);
            if (!argv[i][0] || !end || *end || max_steps == 0) {
                fprintf(stderr, "Invalid --max-steps value: %s\n", argv[i]);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        } else if (program) {
            fprintf(stderr, "Only one ELF program may be specified\n");
            return 2;
        } else {
            program = argv[i];
        }
    }

    if (!program) {
        fprintf(stderr,
                "Usage: %s [--trace] [--max-steps N] program.elf\n",
                argv[0]);
        return 2;
    }

    CPU *c = calloc(1, sizeof(*c));
    if (!c) {
        perror("calloc");
        return 1;
    }
    active_cpu = c;

    c->pc = load_elf(c, program);
    c->x[2] = RAM_SIZE - 16; /* initial stack pointer */

    printf("DoomCPU RV32I\nRAM: %u bytes\nEntry: 0x%08X\n",
           (unsigned)RAM_SIZE, c->pc);

    while (!c->halted) {
        if (c->steps++ >= max_steps)
            fault("instruction limit reached", c->pc);
        step(c);
    }

    printf("Instructions executed: %llu\n",
           (unsigned long long)c->steps);
#ifdef DOOMCPU_SDL
    doom_video_cleanup();
#endif
    active_cpu = NULL;
    free(c);
    return 0;
}
