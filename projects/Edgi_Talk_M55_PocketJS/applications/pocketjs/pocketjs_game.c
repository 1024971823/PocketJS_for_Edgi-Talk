/*
 * Beat Dash game core.
 *
 * One worker thread owns everything slow: it renders the synthesizer into the
 * sound0 device (blocking writes pace it), keeps the song clock, and persists
 * scores/settings to /flash. The UI thread only flips flags and reads cached
 * values, so a 30 Hz frame never waits on audio or flash.
 */

#include "pocketjs_game.h"
#include "pocketjs_music.h"
#include "pocketjs_synth.h"

#include <rtdevice.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GAME_BLOCK_SAMPLES   320U  /* 20 ms per write */
#define GAME_STACK_SIZE      6144U
#define GAME_PRIORITY        17U
/*
 * Time between a block being accepted by the audio queue and it becoming
 * audible. Measured against the drv_i2s double buffer; players fine-tune the
 * remainder with the audio offset in Settings.
 */
#define GAME_PIPELINE_MS     100
#define GAME_OFFSET_LIMIT    300

#define GAME_STORE_PATH      "/flash/pocketjs_game.bin"
#define GAME_STORE_TEMP      "/flash/pocketjs_game.tmp"
#define GAME_STORE_MAGIC     0x42454154U /* "BEAT" */
#define GAME_SONG_PATH       "/flash/pocketjs_song.json"
#define GAME_SONG_TEMP       "/flash/pocketjs_song.tmp"

enum
{
    REQUEST_NONE,
    REQUEST_START,
    REQUEST_STOP,
};

typedef struct
{
    uint32_t magic;
    uint32_t best[POCKETJS_GAME_SLOTS];
    uint8_t rank[POCKETJS_GAME_SLOTS];
    int16_t offset;
    uint16_t reserved;
    uint32_t token; /* 6-digit pairing code for the PC companion */
    uint32_t checksum;
} game_store_t;

extern bool music_player_pause;

static game_store_t s_store;
static struct rt_mutex s_store_lock;
static rt_sem_t s_wake;
static rt_bool_t s_started;
static volatile rt_bool_t s_dirty;
static volatile rt_bool_t s_loaded;

static rt_device_t s_device;
static volatile uint8_t s_request;
static volatile rt_bool_t s_running;
static volatile rt_bool_t s_clock_valid;
static volatile rt_uint32_t s_clock_origin;

static int16_t s_block[GAME_BLOCK_SAMPLES];

static uint32_t game_checksum(const game_store_t *store)
{
    const uint8_t *bytes = (const uint8_t *)store;
    uint32_t hash = 2166136261U;
    size_t i;

    for (i = 0; i < offsetof(game_store_t, checksum); ++i)
    {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

static rt_bool_t game_filesystem_ready(void)
{
    struct stat info;

    return stat("/flash", &info) == 0;
}

static void game_store_load(void)
{
    game_store_t loaded;
    int fd = open(GAME_STORE_PATH, O_RDONLY);
    int count;

    if (fd < 0)
    {
        return;
    }
    count = read(fd, &loaded, sizeof(loaded));
    close(fd);
    if (count != (int)sizeof(loaded) || loaded.magic != GAME_STORE_MAGIC ||
        loaded.checksum != game_checksum(&loaded))
    {
        return;
    }
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    s_store = loaded;
    rt_mutex_release(&s_store_lock);
}

/* Write the store atomically (temp file + rename; FAT needs the unlink). */
static void game_store_flush(void)
{
    game_store_t snapshot;
    int fd;
    int count;

    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    s_store.magic = GAME_STORE_MAGIC;
    s_store.checksum = game_checksum(&s_store);
    snapshot = s_store;
    s_dirty = RT_FALSE;
    rt_mutex_release(&s_store_lock);

    fd = open(GAME_STORE_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        s_dirty = RT_TRUE;
        return;
    }
    count = write(fd, &snapshot, sizeof(snapshot));
    close(fd);
    if (count != (int)sizeof(snapshot))
    {
        unlink(GAME_STORE_TEMP);
        s_dirty = RT_TRUE;
        return;
    }
    (void)unlink(GAME_STORE_PATH);
    if (rename(GAME_STORE_TEMP, GAME_STORE_PATH) != 0)
    {
        unlink(GAME_STORE_TEMP);
        s_dirty = RT_TRUE;
    }
}

/* Create the pairing code on first boot; it then stays stable across boots. */
static void game_ensure_token(void)
{
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    if (s_store.token < 100000U || s_store.token > 999999U)
    {
        uint32_t seed = rt_tick_get() * 2654435761U;

        srand(seed ^ (uint32_t)(uintptr_t)&seed);
        s_store.token = 100000U + ((uint32_t)rand() ^ (seed >> 7)) % 900000U;
        s_dirty = RT_TRUE;
    }
    s_loaded = RT_TRUE;
    rt_mutex_release(&s_store_lock);
}

static rt_bool_t game_audio_open(void)
{
    struct rt_audio_caps caps = {0};

    (void)pocketjs_music_suspend();
    s_device = rt_device_find("sound0");
    if (s_device == RT_NULL || rt_device_open(s_device, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
    {
        s_device = RT_NULL;
        pocketjs_music_resume();
        rt_kprintf("[pjsgame] sound0 unavailable\n");
        return RT_FALSE;
    }
    caps.main_type = AUDIO_TYPE_OUTPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = POCKETJS_SYNTH_RATE;
    caps.udata.config.channels = 1;
    caps.udata.config.samplebits = 16;
    (void)rt_device_control(s_device, AUDIO_CTL_CONFIGURE, &caps);

    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_VOLUME;
    caps.udata.value = pocketjs_music_volume();
    (void)rt_device_control(s_device, AUDIO_CTL_CONFIGURE, &caps);

    music_player_pause = false;
    return RT_TRUE;
}

static void game_audio_close(void)
{
    if (s_device != RT_NULL)
    {
        rt_device_close(s_device);
        s_device = RT_NULL;
    }
    music_player_pause = true;
    pocketjs_music_resume();
}

static void game_song_start(void)
{
    pocketjs_synth_rewind();
    s_clock_valid = RT_FALSE;
    if (!game_audio_open())
    {
        return;
    }
    s_running = RT_TRUE;
}

static void game_song_stop(void)
{
    s_running = RT_FALSE;
    s_clock_valid = RT_FALSE;
    game_audio_close();
}

static void game_render_block(rt_bool_t *first)
{
    rt_uint32_t now;
    int32_t audio_ms;
    int32_t lead;

    pocketjs_synth_render(s_block, GAME_BLOCK_SAMPLES);
    (void)rt_device_write(s_device, 0, s_block, sizeof(s_block));
    now = rt_tick_get_millisecond();
    if (*first)
    {
        *first = RT_FALSE;
        s_clock_origin = now;
        s_clock_valid = RT_TRUE;
        return;
    }
    /*
     * The writer normally runs ahead of playback by the queue depth. If it ever
     * falls behind (lead < 0) the audio stream stalled; hold the clock back by
     * the same amount so notes keep matching what is heard.
     */
    audio_ms = (int32_t)((uint64_t)pocketjs_synth_position() * 1000U / POCKETJS_SYNTH_RATE);
    lead = audio_ms - (int32_t)(now - s_clock_origin);
    if (lead < 0)
    {
        s_clock_origin += (rt_uint32_t)(-lead);
    }
}

static void game_worker(void *parameter)
{
    rt_bool_t first = RT_TRUE;
    int wait;

    (void)parameter;
    for (wait = 0; wait < 60 && !game_filesystem_ready(); ++wait)
    {
        rt_thread_mdelay(500);
    }
    game_store_load();
    game_ensure_token();

    while (1)
    {
        if (s_running)
        {
            if (s_request != REQUEST_NONE)
            {
                game_song_stop();
                if (s_request == REQUEST_STOP)
                {
                    s_request = REQUEST_NONE;
                }
                first = RT_TRUE;
                continue;
            }
            game_render_block(&first);
            continue;
        }

        if (s_request == REQUEST_NONE)
        {
            (void)rt_sem_take(s_wake, s_dirty ? 200 : 1000);
        }
        if (s_request == REQUEST_START)
        {
            s_request = REQUEST_NONE;
            first = RT_TRUE;
            game_song_start();
        }
        else if (s_request == REQUEST_STOP)
        {
            s_request = REQUEST_NONE;
        }
        else if (s_dirty && game_filesystem_ready())
        {
            game_store_flush();
        }
    }
}

rt_err_t pocketjs_game_start_service(void)
{
    rt_thread_t thread;

    if (s_started)
    {
        return RT_EOK;
    }
    pocketjs_synth_init();
    memset(&s_store, 0, sizeof(s_store));
    if (rt_mutex_init(&s_store_lock, "pjsgame", RT_IPC_FLAG_PRIO) != RT_EOK)
    {
        return -RT_ENOMEM;
    }
    s_wake = rt_sem_create("pjsgame", 0, RT_IPC_FLAG_FIFO);
    thread = rt_thread_create("pjsgame", game_worker, RT_NULL, GAME_STACK_SIZE,
                              GAME_PRIORITY, 10U);
    if (s_wake == RT_NULL || thread == RT_NULL)
    {
        return -RT_ENOMEM;
    }
    s_started = RT_TRUE;
    return rt_thread_startup(thread);
}

static void game_wake(void)
{
    if (s_wake != RT_NULL)
    {
        (void)rt_sem_release(s_wake);
    }
}

void pocketjs_game_end(void)
{
    int wait;

    if (!s_started || (!s_running && s_request != REQUEST_START))
    {
        return;
    }
    s_request = REQUEST_STOP;
    game_wake();
    for (wait = 0; wait < 120 && (s_running || s_request == REQUEST_STOP); ++wait)
    {
        rt_thread_mdelay(5);
    }
}

bool pocketjs_game_begin(uint32_t bpm, const pocketjs_game_event_t *events, uint32_t count)
{
    if (!s_started)
    {
        return false;
    }
    pocketjs_game_end();
    if (s_running || s_request != REQUEST_NONE)
    {
        return false;
    }
    if (pocketjs_synth_load(bpm, events, count) == 0U)
    {
        return false;
    }
    s_request = REQUEST_START;
    game_wake();
    return true;
}

bool pocketjs_game_running(void)
{
    return s_running || s_request == REQUEST_START;
}

int32_t pocketjs_game_clock_ms(void)
{
    int32_t ms;

    if (!s_running || !s_clock_valid)
    {
        return -1;
    }
    ms = (int32_t)(rt_tick_get_millisecond() - s_clock_origin) - GAME_PIPELINE_MS;
    return ms < 0 ? 0 : ms;
}

void pocketjs_game_sfx(int id)
{
    if (s_running)
    {
        pocketjs_synth_sfx(id);
    }
}

void pocketjs_game_scores(uint32_t best[POCKETJS_GAME_SLOTS], char rank[POCKETJS_GAME_SLOTS + 1U])
{
    static const char letters[] = "-CBAS";
    size_t i;

    if (!s_started)
    {
        memset(best, 0, POCKETJS_GAME_SLOTS * sizeof(best[0]));
        memset(rank, '-', POCKETJS_GAME_SLOTS);
        rank[POCKETJS_GAME_SLOTS] = '\0';
        return;
    }
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    for (i = 0U; i < POCKETJS_GAME_SLOTS; ++i)
    {
        best[i] = s_store.best[i];
        rank[i] = letters[s_store.rank[i] > 4U ? 0U : s_store.rank[i]];
    }
    rank[POCKETJS_GAME_SLOTS] = '\0';
    rt_mutex_release(&s_store_lock);
}

bool pocketjs_game_save_score(int slot, uint32_t score, int rank)
{
    if (!s_started || slot < 0 || slot >= (int)POCKETJS_GAME_SLOTS)
    {
        return false;
    }
    if (rank < 0 || rank > 4)
    {
        rank = 0;
    }
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    if (score > s_store.best[slot])
    {
        s_store.best[slot] = score;
        s_store.rank[slot] = (uint8_t)rank;
        s_dirty = RT_TRUE;
    }
    rt_mutex_release(&s_store_lock);
    game_wake();
    return true;
}

bool pocketjs_game_token(char text[8])
{
    uint32_t token;

    text[0] = '\0';
    if (!s_started || !s_loaded)
    {
        return false;
    }
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    token = s_store.token;
    rt_mutex_release(&s_store_lock);
    rt_snprintf(text, 8, "%06u", (unsigned int)token);
    return true;
}

int pocketjs_game_offset(void)
{
    return s_store.offset;
}

void pocketjs_game_set_offset(int ms)
{
    if (!s_started)
    {
        return;
    }
    if (ms > GAME_OFFSET_LIMIT)
    {
        ms = GAME_OFFSET_LIMIT;
    }
    if (ms < -GAME_OFFSET_LIMIT)
    {
        ms = -GAME_OFFSET_LIMIT;
    }
    rt_mutex_take(&s_store_lock, RT_WAITING_FOREVER);
    if (s_store.offset != (int16_t)ms)
    {
        s_store.offset = (int16_t)ms;
        s_dirty = RT_TRUE;
    }
    rt_mutex_release(&s_store_lock);
    game_wake();
}

/* -- Custom song storage ------------------------------------------------------ */

int pocketjs_game_custom_open(void)
{
    if (!game_filesystem_ready())
    {
        return -1;
    }
    return open(GAME_SONG_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
}

bool pocketjs_game_custom_commit(int fd, size_t total)
{
    if (fd < 0)
    {
        return false;
    }
    close(fd);
    if (total == 0U || total > POCKETJS_GAME_SONG_LIMIT)
    {
        unlink(GAME_SONG_TEMP);
        return false;
    }
    (void)unlink(GAME_SONG_PATH);
    if (rename(GAME_SONG_TEMP, GAME_SONG_PATH) != 0)
    {
        unlink(GAME_SONG_TEMP);
        return false;
    }
    return true;
}

void pocketjs_game_custom_abort(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
    unlink(GAME_SONG_TEMP);
}

bool pocketjs_game_custom_clear(void)
{
    return unlink(GAME_SONG_PATH) == 0;
}

size_t pocketjs_game_custom_size(void)
{
    struct stat info;

    if (stat(GAME_SONG_PATH, &info) != 0 || info.st_size < 0)
    {
        return 0U;
    }
    return (size_t)info.st_size;
}

char *pocketjs_game_custom_read(size_t *length)
{
    size_t size = pocketjs_game_custom_size();
    size_t used = 0U;
    char *text;
    int fd;

    if (length != NULL)
    {
        *length = 0U;
    }
    if (size == 0U || size > POCKETJS_GAME_SONG_LIMIT)
    {
        return NULL;
    }
    text = (char *)rt_malloc(size + 1U);
    if (text == NULL)
    {
        return NULL;
    }
    fd = open(GAME_SONG_PATH, O_RDONLY);
    if (fd < 0)
    {
        rt_free(text);
        return NULL;
    }
    while (used < size)
    {
        int count = read(fd, text + used, size - used);
        if (count <= 0)
        {
            break;
        }
        used += (size_t)count;
    }
    close(fd);
    if (used != size)
    {
        rt_free(text);
        return NULL;
    }
    text[size] = '\0';
    if (length != NULL)
    {
        *length = size;
    }
    return text;
}
