/*
 * Desktop check for the native synthesizer: renders a song (events read from a
 * text file, one "step voice note length volume" per line) to a WAV file and
 * prints level statistics so clipping or silence shows up without hardware.
 *
 *   synth_test <bpm> <events.txt> <out.wav> [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pocketjs_synth.h"

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    static pocketjs_game_event_t events[POCKETJS_GAME_MAX_EVENTS];
    uint32_t count = 0;
    unsigned step, voice, note, length, volume;
    FILE *in, *out;
    uint32_t bpm, total;
    int16_t block[320];
    long peak = 0, clipped = 0;
    double sum = 0;
    uint32_t written = 0;
    int sfx_at = 0;

    if (argc < 4) { fprintf(stderr, "usage: synth_test bpm events.txt out.wav [seconds]\n"); return 1; }
    bpm = (uint32_t)atoi(argv[1]);
    in = fopen(argv[2], "r");
    if (!in) { perror("events"); return 1; }
    while (count < POCKETJS_GAME_MAX_EVENTS && fscanf(in, "%u %u %u %u %u", &step, &voice, &note, &length, &volume) == 5) {
        events[count++] = (pocketjs_game_event_t){(uint16_t)step, (uint8_t)voice, (uint8_t)note, (uint8_t)length, (uint8_t)volume};
    }
    fclose(in);
    printf("read %u events\n", count);
    if (pocketjs_synth_load(bpm, events, count) == 0) { fprintf(stderr, "load rejected\n"); return 1; }
    pocketjs_synth_rewind();

    total = (uint32_t)((argc > 4 ? atof(argv[4]) : 30.0) * POCKETJS_SYNTH_RATE);
    out = fopen(argv[3], "wb");
    fwrite("RIFF", 1, 4, out); put_u32(out, 36 + total * 2); fwrite("WAVEfmt ", 1, 8, out);
    put_u32(out, 16); put_u16(out, 1); put_u16(out, 1); put_u32(out, POCKETJS_SYNTH_RATE);
    put_u32(out, POCKETJS_SYNTH_RATE * 2); put_u16(out, 2); put_u16(out, 16);
    fwrite("data", 1, 4, out); put_u32(out, total * 2);
    while (written < total) {
        size_t n = total - written < 320 ? total - written : 320;
        if ((written / 320) % 100 == 50) { pocketjs_synth_sfx(sfx_at % 3); ++sfx_at; }
        pocketjs_synth_render(block, n);
        for (size_t i = 0; i < n; ++i) {
            long a = block[i] < 0 ? -(long)block[i] : block[i];
            if (a > peak) peak = a;
            if (a >= 32767) ++clipped;
            sum += (double)block[i] * block[i];
        }
        fwrite(block, 2, n, out);
        written += (uint32_t)n;
    }
    fclose(out);
    printf("rendered %.1fs peak=%ld clipped=%ld rms=%.0f done=%d\n", total / (double)POCKETJS_SYNTH_RATE,
           peak, clipped, __builtin_sqrt(sum / total), pocketjs_synth_song_done());
    return 0;
}
