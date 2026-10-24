#include <stdint.h>

#define WIDTH 320u
#define HEIGHT 200u
#define PIXELS (WIDTH * HEIGHT)
#define AUDIO_BYTES_PER_FRAME 5880u  /* 22,050 Hz / 15 FPS, stereo S16 */

static uint8_t gray[PIXELS];
static uint8_t audio[AUDIO_BYTES_PER_FRAME];
static uint32_t framebuffer[PIXELS];

static int32_t call3(uint32_t service,
                     uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    register uint32_t a0 asm("a0") = arg0;
    register uint32_t a1 asm("a1") = arg1;
    register uint32_t a2 asm("a2") = arg2;
    register uint32_t a7 asm("a7") = service;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a1), "r"(a2), "r"(a7)
                 : "memory");
    return (int32_t)a0;
}

static void print(const char *s)
{
    while (*s)
        (void)call3(2, (uint8_t)*s++, 0, 0);
}

static int read_exact_frame(int fd)
{
    uint32_t got = 0;

    while (got < PIXELS) {
        int32_t n = call3(13, (uint32_t)fd,
                          (uint32_t)(uintptr_t)(gray + got),
                          PIXELS - got);
        if (n < 0) return -1;
        if (n == 0) return got == 0 ? 0 : -1;
        got += (uint32_t)n;
    }
    return 1;
}

static int read_audio_chunk(int fd)
{
    uint32_t got = 0;

    while (got < AUDIO_BYTES_PER_FRAME) {
        int32_t n = call3(13, (uint32_t)fd,
                          (uint32_t)(uintptr_t)(audio + got),
                          AUDIO_BYTES_PER_FRAME - got);
        if (n < 0) return -1;
        if (n == 0) break;
        got += (uint32_t)n;
    }
    return (int)got;
}

int main(void)
{
    print("Bad Apple on DoomCPU - video + audio!\n");

    int32_t video_fd =
        call3(11, (uint32_t)(uintptr_t)"badapple.gray", 0, 0);
    int32_t audio_fd =
        call3(11, (uint32_t)(uintptr_t)"badapple.pcm", 0, 0);

    if (video_fd < 0 || audio_fd < 0) {
        print("ERROR: Cannot open video/audio file\n");
        if (video_fd >= 0) (void)call3(12, (uint32_t)video_fd, 0, 0);
        if (audio_fd >= 0) (void)call3(12, (uint32_t)audio_fd, 0, 0);
        return 1;
    }

    if (call3(20, 0, WIDTH, HEIGHT) < 0) {
        print("ERROR: Cannot initialize display\n");
        return 1;
    }

    if (call3(25, 22050, 2, 0) < 0) {
        print("ERROR: Cannot initialize audio device\n");
        return 1;
    }

    (void)call3(24, (uint32_t)(uintptr_t)"Bad Apple - DoomCPU", 0, 0);

    uint32_t deadline = (uint32_t)call3(23, 0, 0, 0);
    uint32_t fraction = 0;

    for (;;) {
        int vr = read_exact_frame((int)video_fd);
        if (vr == 0) break;
        if (vr < 0) {
            print("ERROR: Video read failed\n");
            break;
        }

        int audio_bytes = read_audio_chunk((int)audio_fd);
        if (audio_bytes < 0) {
            print("ERROR: Audio read failed\n");
            break;
        }

        for (uint32_t i = 0; i < PIXELS; ++i)
            framebuffer[i] =
                gray[i] >= 128 ? 0x00FFFFFFu : 0x00000000u;

        if (call3(20, (uint32_t)(uintptr_t)framebuffer,
                  WIDTH, HEIGHT) < 0) {
            print("ERROR: Framebuffer update failed\n");
            break;
        }

        if (audio_bytes > 0 &&
            call3(26, (uint32_t)(uintptr_t)audio,
                  (uint32_t)audio_bytes, 0) < 0) {
            print("ERROR: Audio queue failed\n");
            break;
        }

        uint32_t pressed = 0;
        uint8_t key = 0;
        if (call3(21, (uint32_t)(uintptr_t)&pressed,
                  (uint32_t)(uintptr_t)&key, 0) > 0 &&
            pressed && (key == 27 || key == 'q'))
            break;

        /* Alternate 66 and 67 ms intervals for approximately 15 FPS.
           This avoids division, which RV32I does not implement in hardware. */
        deadline += 66u;
        fraction += 2u;
        if (fraction >= 3u) {
            deadline += 1u;
            fraction -= 3u;
        }

        for (;;) {
            uint32_t now = (uint32_t)call3(23, 0, 0, 0);
            int32_t remaining = (int32_t)(deadline - now);
            if (remaining <= 0) break;
            if (remaining > 1)
                (void)call3(22, (uint32_t)remaining - 1u, 0, 0);
        }
    }

    (void)call3(27, 0, 0, 0);
    (void)call3(12, (uint32_t)video_fd, 0, 0);
    (void)call3(12, (uint32_t)audio_fd, 0, 0);
    print("Bad Apple finished!\n");
    return 0;
}
