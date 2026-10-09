/*
 * Beat Dash native game core: a tiny polyphonic synthesizer that renders the
 * built-in (or PC-supplied) songs straight into the sound0 device, a clock that
 * follows the audio stream, and flash-backed scores / settings / custom song.
 */
#ifndef POCKETJS_GAME_H
#define POCKETJS_GAME_H

#include <rtthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pocketjs_synth.h"

#define POCKETJS_GAME_SLOTS      8U
#define POCKETJS_GAME_SONG_LIMIT (96U * 1024U)


/* Create the audio/storage worker and load saved scores. Safe to call twice. */
rt_err_t pocketjs_game_start_service(void);

/*
 * Queue a song for playback. Returns false when the arguments are unusable or
 * the previous song could not be stopped. The audio device is opened
 * asynchronously; pocketjs_game_clock_ms() reports -1 until sound is flowing.
 */
bool pocketjs_game_begin(uint32_t bpm, const pocketjs_game_event_t *events, uint32_t count);
void pocketjs_game_end(void);

/* True while a song is playing or about to (uploads and flash writes wait). */
bool pocketjs_game_running(void);

/* Song position in milliseconds following the audio stream, or -1. */
int32_t pocketjs_game_clock_ms(void);

/* 0 perfect, 1 great, 2 miss. */
void pocketjs_game_sfx(int id);

/* Scores. rank codes: 0 none, 1 C, 2 B, 3 A, 4 S. */
void pocketjs_game_scores(uint32_t best[POCKETJS_GAME_SLOTS], char rank[POCKETJS_GAME_SLOTS + 1U]);
bool pocketjs_game_save_score(int slot, uint32_t score, int rank);
int pocketjs_game_offset(void);
/* Six-digit pairing code shown on the device; false until storage is loaded. */
bool pocketjs_game_token(char text[8]);
void pocketjs_game_set_offset(int ms);

/* Custom song stored on /flash (uploaded by the PC companion). */
int pocketjs_game_custom_open(void);
/* Read the staged upload through its open fd without replacing the saved song. */
char *pocketjs_game_custom_staged_read(int fd, size_t *length);
bool pocketjs_game_custom_commit(int fd, size_t total);
void pocketjs_game_custom_abort(int fd);
bool pocketjs_game_custom_clear(void);
size_t pocketjs_game_custom_size(void);
/* Returns a malloc'ed, NUL-terminated copy (caller frees) or NULL. */
char *pocketjs_game_custom_read(size_t *length);

#endif
