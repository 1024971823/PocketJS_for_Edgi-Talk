/*
 * Portable (no RTOS) integer synthesizer used by the Beat Dash game core.
 * 16 kHz, mono, signed 16-bit. It is kept free of OS headers so it can be
 * exercised on a desktop (tools/preview/synth_test.c).
 */
#ifndef POCKETJS_SYNTH_H
#define POCKETJS_SYNTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POCKETJS_GAME_MAX_EVENTS 2000U
#define POCKETJS_GAME_MAX_NOTES  2000U
#define POCKETJS_GAME_BPM_MIN    60U
#define POCKETJS_GAME_BPM_MAX    240U
#define POCKETJS_GAME_STEP_MAX   65535U
#define POCKETJS_GAME_TITLE_MAX  64U
#define POCKETJS_GAME_LEVEL_MIN  1U
#define POCKETJS_GAME_LEVEL_MAX  3U

typedef struct
{
    uint16_t step;   /* sixteenth-note index from the start of the song */
    uint8_t voice;   /* 0 lead, 1 bass, 2 drum */
    uint8_t note;    /* MIDI note, or drum id (0 kick, 1 snare, 2 hat) */
    uint8_t length;  /* in steps */
    uint8_t volume;  /* 0..127 */
} pocketjs_game_event_t;

#define POCKETJS_SYNTH_RATE 16000U

/* Build the pitch table. Idempotent. */
void pocketjs_synth_init(void);

/*
 * Load a song. Events are copied, sorted and validated; returns the number of
 * usable events (0 = rejected).
 */
uint32_t pocketjs_synth_load(uint32_t bpm, const pocketjs_game_event_t *events, uint32_t count);

/* Rewind to the start of the loaded song and silence every voice. */
void pocketjs_synth_rewind(void);

/* Render `count` samples. Sound effects keep sounding after the song ends. */
void pocketjs_synth_render(int16_t *out, size_t count);

/* Trigger a hit effect: 0 perfect, 1 great, 2 miss. Thread-safe (lock-free). */
void pocketjs_synth_sfx(int id);

/* Samples rendered since the last rewind. */
uint32_t pocketjs_synth_position(void);

/* True once every scheduled event has been triggered. */
bool pocketjs_synth_song_done(void);

#endif
