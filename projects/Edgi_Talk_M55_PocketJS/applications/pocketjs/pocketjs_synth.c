#include "pocketjs_synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 14
/* 160 Hz and 42 Hz as phase increments at 16 kHz. */
#define KICK_HIGH_INC 42949673U
#define KICK_LOW_INC 11274289U
#define STEP_SAMPLES_NUMERATOR (POCKETJS_SYNTH_RATE * 15U) /* samples/step = 240000 / bpm */

enum
{
    KIND_LEAD,
    KIND_BASS,
    KIND_KICK,
    KIND_SNARE,
    KIND_HAT,
    KIND_SFX_PERFECT,
    KIND_SFX_GREAT,
    KIND_SFX_MISS,
};

typedef struct
{
    bool active;
    uint8_t kind;
    uint32_t phase;
    uint32_t inc;
    int32_t age;
    int32_t length;
    int32_t amp;
    int32_t aux;
    int32_t env;   /* envelope, refreshed at control rate */
    int32_t extra; /* kick sweep increment, refreshed with the envelope */
} voice_t;

/* Envelopes are recomputed every 16 samples (1 ms): they are smooth, and the
 * per-sample divides were the bulk of the synth's CPU time. */
#define CONTROL_MASK 15

typedef struct
{
    uint32_t start;
    uint16_t length;
    uint8_t voice;
    uint8_t note;
    uint8_t volume;
} scheduled_t;

static scheduled_t s_events[POCKETJS_GAME_MAX_EVENTS];
static uint32_t s_count;
static uint32_t s_next;
static uint32_t s_position;
static voice_t s_voices[MAX_VOICES];
static uint32_t s_pitch[128];
static uint32_t s_noise = 0x2545F491U;
static uint32_t s_sfx_pending;
static bool s_tables_ready;

/* Convert hertz to a 32-bit phase increment at the synth rate. */
static uint32_t hz_to_inc(float hz)
{
    return (uint32_t)(hz * (4294967296.0f / (float)POCKETJS_SYNTH_RATE));
}

void pocketjs_synth_init(void)
{
    int note;

    if (s_tables_ready)
    {
        return;
    }
    for (note = 0; note < 128; ++note)
    {
        s_pitch[note] = hz_to_inc(440.0f * powf(2.0f, (float)(note - 69) / 12.0f));
    }
    s_tables_ready = true;
}

static int32_t noise(void)
{
    s_noise ^= s_noise << 13;
    s_noise ^= s_noise >> 17;
    s_noise ^= s_noise << 5;
    return (int32_t)(s_noise >> 16) - 32768;
}

/* Triangle in -32768..32766 from a 32-bit phase. */
static int32_t triangle(uint32_t phase)
{
    int32_t p = (int32_t)(phase >> 16);
    return (p < 32768 ? p : 65535 - p) * 2 - 32768;
}

static int compare_events(const void *left, const void *right)
{
    const scheduled_t *a = (const scheduled_t *)left;
    const scheduled_t *b = (const scheduled_t *)right;

    if (a->start != b->start)
    {
        return a->start < b->start ? -1 : 1;
    }
    return (int)a->voice - (int)b->voice;
}

uint32_t pocketjs_synth_load(uint32_t bpm, const pocketjs_game_event_t *events, uint32_t count)
{
    uint32_t i;
    uint32_t kept = 0U;

    pocketjs_synth_init();
    s_count = 0U;
    s_next = 0U;
    if (events == NULL || count == 0U || bpm < POCKETJS_GAME_BPM_MIN || bpm > POCKETJS_GAME_BPM_MAX)
    {
        return 0U;
    }
    if (count > POCKETJS_GAME_MAX_EVENTS)
    {
        count = POCKETJS_GAME_MAX_EVENTS;
    }
    for (i = 0U; i < count; ++i)
    {
        const pocketjs_game_event_t *in = &events[i];
        scheduled_t *out = &s_events[kept];
        uint32_t length = in->length == 0U ? 1U : (in->length > 64U ? 64U : in->length);

        if (in->voice > 2U)
        {
            continue;
        }
        if (in->voice == 2U ? in->note > 2U : (in->note < 12U || in->note > 108U))
        {
            continue;
        }
        out->start = (uint32_t)(((uint64_t)in->step * STEP_SAMPLES_NUMERATOR) / bpm);
        out->length = (uint16_t)(((uint64_t)length * STEP_SAMPLES_NUMERATOR) / bpm);
        out->voice = in->voice;
        out->note = in->note;
        out->volume = in->volume == 0U ? 90U : (in->volume > 127U ? 127U : in->volume);
        ++kept;
    }
    qsort(s_events, kept, sizeof(s_events[0]), compare_events);
    s_count = kept;
    return kept;
}

void pocketjs_synth_rewind(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    s_next = 0U;
    s_position = 0U;
    __atomic_store_n(&s_sfx_pending, 0U, __ATOMIC_RELAXED);
}

static voice_t *voice_alloc(void)
{
    voice_t *oldest = &s_voices[0];
    int i;

    for (i = 0; i < MAX_VOICES; ++i)
    {
        if (!s_voices[i].active)
        {
            return &s_voices[i];
        }
        if (s_voices[i].age > oldest->age)
        {
            oldest = &s_voices[i];
        }
    }
    return oldest;
}

static void voice_start(uint8_t kind, uint32_t inc, int32_t length, int32_t amp)
{
    voice_t *voice = voice_alloc();

    memset(voice, 0, sizeof(*voice));
    voice->active = true;
    voice->kind = kind;
    voice->inc = inc;
    voice->length = length < 1 ? 1 : length;
    voice->amp = amp;
}

static void trigger(const scheduled_t *event)
{
    int32_t length = (int32_t)event->length * 15 / 16;
    int32_t volume = event->volume;

    if (event->voice == 0U)
    {
        voice_start(KIND_LEAD, s_pitch[event->note], length, volume * 46);
    }
    else if (event->voice == 1U)
    {
        voice_start(KIND_BASS, s_pitch[event->note], length, volume * 56);
    }
    else if (event->note == 0U)
    {
        voice_start(KIND_KICK, 0U, 4800, volume * 70);
    }
    else if (event->note == 1U)
    {
        voice_start(KIND_SNARE, s_pitch[54], 2600, volume * 54);
    }
    else
    {
        voice_start(KIND_HAT, 0U, 900, volume * 30);
    }
}

void pocketjs_synth_sfx(int id)
{
    if (id >= 0 && id <= 2)
    {
        __atomic_fetch_or(&s_sfx_pending, 1U << id, __ATOMIC_RELAXED);
    }
}

static void trigger_sfx(uint32_t mask)
{
    if (mask & 1U)
    {
        voice_start(KIND_SFX_PERFECT, hz_to_inc(1760.0f), 1280, 2300);
    }
    if (mask & 2U)
    {
        voice_start(KIND_SFX_GREAT, hz_to_inc(1319.0f), 960, 2400);
    }
    if (mask & 4U)
    {
        voice_start(KIND_SFX_MISS, hz_to_inc(130.0f), 3400, 2600);
    }
}

/* One sample of one voice; deactivates the voice when it has finished. */
static int32_t voice_sample(voice_t *voice)
{
    int32_t age = voice->age;
    int32_t length = voice->length;
    int32_t remaining = length - age;
    int32_t env;
    int32_t out;

    if (remaining <= 0)
    {
        voice->active = false;
        return 0;
    }
    voice->phase += voice->inc;
    switch (voice->kind)
    {
    case KIND_LEAD:
        if ((age & CONTROL_MASK) == 0)
        {
            env = age < 48 ? age * 256 / 48 : 256 - (age - 48) * 140 / length;
            if (env < 96)
            {
                env = 96;
            }
            if (remaining < 160)
            {
                env = env * remaining / 160;
            }
            voice->env = env;
        }
        env = voice->env;
        out = (voice->phase < 0x50000000U ? voice->amp : -voice->amp) * env >> 8;
        break;
    case KIND_BASS:
        if ((age & CONTROL_MASK) == 0)
        {
            env = age < 32 ? age * 256 / 32 : 256;
            if (remaining < 240)
            {
                env = env * remaining / 240;
            }
            voice->env = env;
        }
        env = voice->env;
        out = ((triangle(voice->phase) * voice->amp) >> 15) * env >> 8;
        break;
    case KIND_KICK:
    {
        if ((age & CONTROL_MASK) == 0)
        {
            int32_t k = remaining * 256 / length;
            int32_t sweep = (k * k) >> 8;

            voice->extra = (int32_t)(KICK_LOW_INC + ((KICK_HIGH_INC - KICK_LOW_INC) >> 8) * (uint32_t)sweep);
            voice->env = k;
        }
        voice->phase += (uint32_t)voice->extra;
        env = voice->env;
        out = ((triangle(voice->phase) * voice->amp) >> 15) * env >> 8;
        break;
    }
    case KIND_SNARE:
    {
        if ((age & CONTROL_MASK) == 0)
        {
            int32_t k = remaining * 256 / length;
            voice->env = (k * k) >> 8;
        }
        env = voice->env;
        out = (((noise() * 3 + triangle(voice->phase)) >> 2) * voice->amp >> 15) * env >> 8;
        break;
    }
    case KIND_HAT:
    {
        int32_t n = noise();

        if ((age & CONTROL_MASK) == 0)
        {
            int32_t k = remaining * 256 / length;
            voice->env = (k * k) >> 8;
        }
        env = voice->env;
        out = (((n - voice->aux) >> 1) * voice->amp >> 15) * env >> 8;
        voice->aux = n;
        break;
    }
    case KIND_SFX_PERFECT:
        if (age == 640)
        {
            voice->inc = hz_to_inc(2349.0f);
        }
        if ((age & CONTROL_MASK) == 0)
        {
            voice->env = remaining * 256 / length;
        }
        env = voice->env;
        out = (voice->phase < 0x80000000U ? voice->amp : -voice->amp) * env >> 8;
        break;
    case KIND_SFX_GREAT:
        if ((age & CONTROL_MASK) == 0)
        {
            voice->env = remaining * 256 / length;
        }
        env = voice->env;
        out = ((triangle(voice->phase) * voice->amp) >> 15) * env >> 8;
        break;
    default: /* KIND_SFX_MISS: falling saw */
        if ((age & CONTROL_MASK) == 0)
        {
            voice->env = remaining * 256 / length;
        }
        env = voice->env;
        if ((age & 7) == 0)
        {
            voice->inc -= 37900U;
        }
        out = ((((int32_t)(voice->phase >> 16) - 32768) * voice->amp) >> 15) * env >> 8;
        break;
    }
    voice->age = age + 1;
    return out;
}

void pocketjs_synth_render(int16_t *out, size_t count)
{
    size_t i;
    uint32_t pending = __atomic_exchange_n(&s_sfx_pending, 0U, __ATOMIC_RELAXED);

    if (pending != 0U)
    {
        trigger_sfx(pending);
    }
    for (i = 0U; i < count; ++i)
    {
        int32_t mix = 0;
        int v;

        while (s_next < s_count && s_events[s_next].start <= s_position)
        {
            trigger(&s_events[s_next++]);
        }
        for (v = 0; v < MAX_VOICES; ++v)
        {
            if (s_voices[v].active)
            {
                mix += voice_sample(&s_voices[v]);
            }
        }
        if (mix > 32767)
        {
            mix = 32767;
        }
        else if (mix < -32768)
        {
            mix = -32768;
        }
        out[i] = (int16_t)mix;
        ++s_position;
    }
}

uint32_t pocketjs_synth_position(void)
{
    return s_position;
}

bool pocketjs_synth_song_done(void)
{
    return s_next >= s_count;
}
