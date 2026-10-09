#pragma once

#include <rtthread.h>
#include <stdbool.h>

typedef struct
{
    char track[32];
    int index;
    int count;
    int volume;
    int state;
    bool sd_present;
} pocketjs_music_status_t;

rt_err_t pocketjs_music_start(void);
bool pocketjs_music_command(int command);
void pocketjs_music_status(pocketjs_music_status_t *status);
/* Rescan and show this title (the wav name without ".wav") on the home bar. */
void pocketjs_music_focus(const char *name);

/*
 * The game synthesizer shares the sound0 device with the music player.
 * suspend() makes the player release the device and blocks (up to ~0.6 s)
 * until it has; resume() lets it continue where it was playing before.
 */
bool pocketjs_music_suspend(void);
void pocketjs_music_resume(void);
int pocketjs_music_volume(void);

enum
{
    POCKETJS_MUSIC_TOGGLE = 1,
    POCKETJS_MUSIC_PREVIOUS,
    POCKETJS_MUSIC_NEXT,
    POCKETJS_MUSIC_VOLUME_DOWN,
    POCKETJS_MUSIC_VOLUME_UP,
    POCKETJS_MUSIC_SCAN,
    POCKETJS_MUSIC_SUSPEND, /* internal: see pocketjs_music_suspend() */
    POCKETJS_MUSIC_RESUME,  /* internal */
};

enum
{
    POCKETJS_MUSIC_STOPPED = 0,
    POCKETJS_MUSIC_PLAYING,
    POCKETJS_MUSIC_PAUSED,
    POCKETJS_MUSIC_ERROR,
};
