#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomgeneric.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define OUT_RATE       22050u
#define CHANNELS       16
#define MAX_MIX_FRAMES 2048u

/* These are used by i_sound.c when FEATURE_SOUND is enabled. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static int audio_ready;
static int sfx_prefix;
static uint32_t last_update_ms;
static uint32_t frame_remainder;
static int16_t mixbuf[MAX_MIX_FRAMES * 2];

typedef struct {
    uint8_t *samples;
    uint32_t count;
    uint64_t phase;
    uint32_t step;
    int volume;
    int separation;
} active_sound_t;

static active_sound_t active[CHANNELS];

/* DoomCPU host-call ABI. */
static int32_t host_call(uint32_t service,
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

static void stop_channel(int channel)
{
    if (channel < 0 || channel >= CHANNELS)
        return;

    free(active[channel].samples);
    memset(&active[channel], 0, sizeof(active[channel]));
}

static boolean sound_init(boolean use_sfx_prefix)
{
    memset(active, 0, sizeof(active));
    sfx_prefix = use_sfx_prefix;

    if (host_call(25, OUT_RATE, 2, 0) < 0)
        return false;

    audio_ready = 1;
    last_update_ms = DG_GetTicksMs();
    frame_remainder = 0;

    return true;
}

static void sound_shutdown(void)
{
    for (int i = 0; i < CHANNELS; ++i)
        stop_channel(i);

    if (audio_ready)
        (void)host_call(27, 0, 0, 0);

    audio_ready = 0;
}

static int sound_get_lump(sfxinfo_t *sfx)
{
    char name[9] = {0};
    unsigned int pos = 0;

    if (sfx->link != NULL)
        sfx = sfx->link;

    if (sfx_prefix) {
        name[pos++] = 'D';
        name[pos++] = 'S';
    }

    while (pos < 8 && sfx->name[pos - (sfx_prefix ? 2 : 0)] != '\0') {
        unsigned int index = pos - (sfx_prefix ? 2 : 0);
        name[pos++] = sfx->name[index];
    }

    return W_CheckNumForName(name);
}

static void sound_update_params(int channel, int volume, int separation)
{
    if (channel < 0 || channel >= CHANNELS)
        return;

    active[channel].volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    active[channel].separation =
        separation < 0 ? 0 : separation > 254 ? 254 : separation;
}

static int sound_start(sfxinfo_t *sfx, int channel, int volume, int separation)
{
    if (!audio_ready || channel < 0 || channel >= CHANNELS)
        return -1;

    stop_channel(channel);

    int lump = sfx->lumpnum;
    if (lump < 0)
        return -1;

    uint8_t *wad = (uint8_t *)W_CacheLumpNum(lump, PU_STATIC);
    int lump_length = W_LumpLength(lump);

    if (wad == NULL || lump_length < 8) {
        W_ReleaseLumpNum(lump);
        return -1;
    }

    uint32_t rate = (uint32_t)wad[2] | ((uint32_t)wad[3] << 8);
    uint32_t count = (uint32_t)wad[4]
                   | ((uint32_t)wad[5] << 8)
                   | ((uint32_t)wad[6] << 16)
                   | ((uint32_t)wad[7] << 24);

    if (wad[0] != 3 || wad[1] != 0 ||
        count > (uint32_t)(lump_length - 8) ||
        count <= 48 || rate < 4000 || rate > 48000) {
        W_ReleaseLumpNum(lump);
        return -1;
    }

    /* Skip the short leading/trailing portions used by Doom's DMX sounds. */
    count -= 32;

    uint8_t *copy = malloc(count);
    if (copy == NULL) {
        W_ReleaseLumpNum(lump);
        return -1;
    }

    memcpy(copy, wad + 24, count);
    W_ReleaseLumpNum(lump);

    active[channel].samples = copy;
    active[channel].count = count;
    active[channel].phase = 0;
    active[channel].step = (rate << 16) / OUT_RATE;
    active[channel].volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    active[channel].separation =
        separation < 0 ? 0 : separation > 254 ? 254 : separation;

    return channel;
}

static void sound_update(void)
{
    if (!audio_ready)
        return;

    uint32_t now = DG_GetTicksMs();
    uint32_t elapsed = now - last_update_ms;
    last_update_ms = now;

    if (elapsed == 0)
        return;

    /* Bound work if the emulator pauses for a while. */
    if (elapsed > 80)
        elapsed = 80;

    uint32_t total = elapsed * OUT_RATE + frame_remainder;
    uint32_t frames = total / 1000u;
    frame_remainder = total % 1000u;

    if (frames == 0)
        return;

    if (frames > MAX_MIX_FRAMES)
        frames = MAX_MIX_FRAMES;

    for (uint32_t frame = 0; frame < frames; ++frame) {
        int32_t left = 0;
        int32_t right = 0;

        for (int ch = 0; ch < CHANNELS; ++ch) {
            active_sound_t *sound = &active[ch];

            if (sound->samples == NULL)
                continue;

            uint32_t index = (uint32_t)(sound->phase >> 16);
            if (index >= sound->count) {
                stop_channel(ch);
                continue;
            }

            int sample = ((int)sound->samples[index] - 128) * 256;
            int sep = sound->separation;

            int left_pan  = sep < 127 ? 127 : 254 - sep;
            int right_pan = sep > 127 ? 127 : sep;

            left  += sample * sound->volume * left_pan / 16129;
            right += sample * sound->volume * right_pan / 16129;

            sound->phase += sound->step;
        }

        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;

        mixbuf[frame * 2] = (int16_t)left;
        mixbuf[frame * 2 + 1] = (int16_t)right;
    }

    (void)host_call(26, (uint32_t)(uintptr_t)mixbuf,
                    frames * 4u, 0);
}

static void sound_stop(int channel)
{
    stop_channel(channel);
}

static boolean sound_is_playing(int channel)
{
    return channel >= 0 && channel < CHANNELS &&
           active[channel].samples != NULL;
}

static void sound_cache(sfxinfo_t *sounds, int count)
{
    (void)sounds;
    (void)count;
}

static snddevice_t sound_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32
};

sound_module_t DG_sound_module = {
    sound_devices,
    sizeof(sound_devices) / sizeof(sound_devices[0]),
    sound_init,
    sound_shutdown,
    sound_get_lump,
    sound_update,
    sound_update_params,
    sound_start,
    sound_stop,
    sound_is_playing,
    sound_cache
};

/* DG_music_module is implemented in doomcpu-music.c. */
