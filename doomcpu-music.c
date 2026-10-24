#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"

#define MAX_MIDI_SIZE (1024u * 1024u)

typedef struct {
    int32_t host_id;
    uint8_t *midi;
    uint32_t midi_length;
} doomcpu_song_t;

static int music_ready;

static int32_t music_call(uint32_t service,
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

static boolean music_init(void)
{
    int32_t r = music_call(28, 0, 0, 0);
    music_ready = (r == 0);

    if (!music_ready)
        printf("DoomCPU: MIDI initialization failed: %d\n", (int)r);

    return music_ready;
}

static void music_shutdown(void)
{
    if (music_ready)
        (void)music_call(37, 0, 0, 0);

    music_ready = 0;
}

static void music_volume(int volume)
{
    if (music_ready)
        (void)music_call(33, (uint32_t)volume, 0, 0);
}

static void music_pause(void)
{
    if (music_ready)
        (void)music_call(34, 0, 0, 0);
}

static void music_resume(void)
{
    if (music_ready)
        (void)music_call(35, 0, 0, 0);
}

static void *music_register(void *data, int length)
{
    if (!music_ready || data == NULL || length <= 0 ||
        (uint32_t)length > MAX_MIDI_SIZE)
        return NULL;

    uint8_t *midi = NULL;
    size_t midi_length = 0;

    if (length >= 14 && memcmp(data, "MThd", 4) == 0) {
        midi_length = (size_t)length;
        midi = malloc(midi_length);

        if (midi != NULL)
            memcpy(midi, data, midi_length);
    } else {
        /* Doom WAD music is MUS; convert it to a standard MIDI stream. */
        MEMFILE *input = mem_fopen_read(data, (size_t)length);
        MEMFILE *output = mem_fopen_write();

        if (input == NULL || output == NULL) {
            if (input != NULL) mem_fclose(input);
            if (output != NULL) mem_fclose(output);
            return NULL;
        }

        int result = mus2mid(input, output);
        void *converted = NULL;

        if (result == 0)
            mem_get_buf(output, &converted, &midi_length);

        if (result == 0 && converted != NULL &&
            midi_length >= 14 && midi_length <= MAX_MIDI_SIZE) {
            midi = malloc(midi_length);
            if (midi != NULL)
                memcpy(midi, converted, midi_length);
        }

        mem_fclose(input);
        mem_fclose(output);
    }

    if (midi == NULL || midi_length == 0 ||
        midi_length > MAX_MIDI_SIZE) {
        free(midi);
        return NULL;
    }

    doomcpu_song_t *song = malloc(sizeof(*song));
    if (song == NULL) {
        free(midi);
        return NULL;
    }

    int32_t id = music_call(
        29, (uint32_t)(uintptr_t)midi, (uint32_t)midi_length, 0);

    if (id <= 0) {
        printf("DoomCPU: MIDI registration failed: %d\n", (int)id);
        free(midi);
        free(song);
        return NULL;
    }

    song->host_id = id;
    song->midi = midi;
    song->midi_length = (uint32_t)midi_length;

    return song;
}

static void music_unregister(void *handle)
{
    doomcpu_song_t *song = handle;

    if (song == NULL)
        return;

    if (music_ready && song->host_id > 0)
        (void)music_call(32, (uint32_t)song->host_id, 0, 0);

    free(song->midi);
    free(song);
}

static void music_play(void *handle, boolean looping)
{
    doomcpu_song_t *song = handle;

    if (music_ready && song != NULL)
        (void)music_call(30, (uint32_t)song->host_id,
                         looping ? 1u : 0u, 0);
}

static void music_stop(void)
{
    if (music_ready)
        (void)music_call(31, 0, 0, 0);
}

static boolean music_playing(void)
{
    return music_ready && music_call(36, 0, 0, 0) > 0;
}

static void music_poll(void)
{
    /* SDL_mixer advances MIDI playback asynchronously on the host. */
}

static snddevice_t music_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32
};

music_module_t DG_music_module = {
    music_devices,
    sizeof(music_devices) / sizeof(music_devices[0]),
    music_init,
    music_shutdown,
    music_volume,
    music_pause,
    music_resume,
    music_register,
    music_unregister,
    music_play,
    music_stop,
    music_playing,
    music_poll
};
