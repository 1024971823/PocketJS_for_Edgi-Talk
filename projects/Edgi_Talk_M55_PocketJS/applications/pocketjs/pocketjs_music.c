#include "pocketjs_music.h"

#include <rtdevice.h>
#include <dirent.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

#define MUSIC_MAX_TRACKS 17
#define MUSIC_BLOCK_SAMPLES 512
#define MUSIC_THREAD_STACK 4096
#define MUSIC_FOLDER "/sdcard/music"

typedef struct
{
    char name[32];
    char path[128];
} music_track_t;

typedef struct
{
    FILE *file;
    uint32_t remaining;
    uint32_t sample_rate;
    uint16_t channels;
} music_wav_t;

static music_track_t s_tracks[MUSIC_MAX_TRACKS] = {{"Demo melody", ""}};
static pocketjs_music_status_t s_status = {
    .track = "Demo melody", .count = 1, .volume = 50,
};
static rt_mutex_t s_lock;
static rt_mq_t s_commands;
static rt_bool_t s_started;
/* Set once the worker has released sound0 for the game synthesizer. */
static volatile rt_bool_t s_suspended;
static rt_bool_t s_resume_playing;
extern bool music_player_pause;

static uint16_t music_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t music_u32(const uint8_t *data)
{
    return (uint32_t)music_u16(data) | ((uint32_t)music_u16(data + 2) << 16);
}

static bool music_is_wav(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && name[0] != '.' &&
           name[length - 4] == '.' &&
           tolower((unsigned char)name[length - 3]) == 'w' &&
           tolower((unsigned char)name[length - 2]) == 'a' &&
           tolower((unsigned char)name[length - 1]) == 'v';
}

static void music_scan(void)
{
    struct dirent *entry;
    DIR *folder = opendir(MUSIC_FOLDER);
    int count = 1;

    if (folder != RT_NULL)
    {
        while ((entry = readdir(folder)) != RT_NULL && count < MUSIC_MAX_TRACKS)
        {
            music_track_t *track = &s_tracks[count];
            int length;

            if (!music_is_wav(entry->d_name))
            {
                continue;
            }
            length = snprintf(track->path, sizeof(track->path),
                              MUSIC_FOLDER "/%s", entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(track->path))
            {
                continue;
            }
            rt_strncpy(track->name, entry->d_name, sizeof(track->name) - 1);
            track->name[sizeof(track->name) - 1] = '\0';
            track->name[strlen(track->name) - 4] = '\0';
            ++count;
        }
        closedir(folder);
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_status.count = count;
    s_status.sd_present = folder != RT_NULL;
    if (s_status.index >= count)
    {
        s_status.index = 0;
    }
    rt_strncpy(s_status.track, s_tracks[s_status.index].name,
               sizeof(s_status.track) - 1);
    s_status.track[sizeof(s_status.track) - 1] = '\0';
    rt_mutex_release(s_lock);
}

static bool music_open_wav(const char *path, music_wav_t *wav)
{
    uint8_t header[12];
    bool have_format = false;
    bool have_data = false;
    uint32_t size;

    rt_memset(wav, 0, sizeof(*wav));
    wav->file = fopen(path, "rb");
    if (wav->file == RT_NULL || fread(header, 1, sizeof(header), wav->file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0)
    {
        goto fail;
    }
    while (fread(header, 1, 8, wav->file) == 8)
    {
        size = music_u32(header + 4);
        if (memcmp(header, "fmt ", 4) == 0)
        {
            uint8_t format[16];
            if (size < sizeof(format) || fread(format, 1, sizeof(format), wav->file) != sizeof(format))
            {
                goto fail;
            }
            wav->channels = music_u16(format + 2);
            wav->sample_rate = music_u32(format + 4);
            if (music_u16(format) != 1 || music_u16(format + 14) != 16 ||
                wav->channels != 1 ||
                (wav->sample_rate != 16000 && wav->sample_rate != 24000 &&
                 wav->sample_rate != 48000 && wav->sample_rate != 96000))
            {
                goto fail;
            }
            have_format = true;
            if (fseek(wav->file, (long)(size - sizeof(format) + (size & 1U)), SEEK_CUR) != 0)
            {
                goto fail;
            }
        }
        else if (memcmp(header, "data", 4) == 0)
        {
            wav->remaining = size;
            have_data = true;
            break;
        }
        else if (fseek(wav->file, (long)(size + (size & 1U)), SEEK_CUR) != 0)
        {
            goto fail;
        }
    }
    if (have_format && have_data && wav->remaining != 0)
    {
        return true;
    }
fail:
    if (wav->file != RT_NULL)
    {
        fclose(wav->file);
    }
    wav->file = RT_NULL;
    return false;
}

static void music_set_state(int state)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_status.state = state;
    music_player_pause = state != POCKETJS_MUSIC_PLAYING;
    rt_mutex_release(s_lock);
}

static int music_get_index(void)
{
    int index;
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    index = s_status.index;
    rt_mutex_release(s_lock);
    return index;
}

static void music_select(int delta)
{
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    s_status.index = (s_status.index + delta + s_status.count) % s_status.count;
    rt_strncpy(s_status.track, s_tracks[s_status.index].name,
               sizeof(s_status.track) - 1);
    s_status.track[sizeof(s_status.track) - 1] = '\0';
    rt_mutex_release(s_lock);
}

static void music_set_volume(rt_device_t device, int delta)
{
    struct rt_audio_caps caps = {0};
    int volume;

    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    volume = s_status.volume + delta;
    if (volume < 0) volume = 0;
    if (volume > 99) volume = 99;
    s_status.volume = volume;
    rt_mutex_release(s_lock);
    if (device != RT_NULL)
    {
        caps.main_type = AUDIO_TYPE_MIXER;
        caps.sub_type = AUDIO_MIXER_VOLUME;
        caps.udata.value = volume;
        (void)rt_device_control(device, AUDIO_CTL_CONFIGURE, &caps);
    }
}

static uint32_t music_demo_phase;
static uint32_t music_demo_samples;

static void music_fill_demo(int16_t *buffer)
{
    static const uint16_t notes[] = {262, 330, 392, 523, 440, 392, 330, 294,
                                     262, 330, 392, 659, 523, 440, 392, 330};
    for (int i = 0; i < MUSIC_BLOCK_SAMPLES; ++i)
    {
        uint32_t note = (music_demo_samples / 4000U) % 16U;
        uint32_t position = music_demo_samples % 4000U;
        uint32_t step = (uint32_t)notes[note] * 65536U / 16000U;
        int32_t triangle;
        int32_t envelope = position < 250U ? (int32_t)position :
                           position > 3500U ? (int32_t)(4000U - position) : 250;

        music_demo_phase += step;
        triangle = (int32_t)((music_demo_phase >> 7) & 511U);
        triangle = triangle < 256 ? triangle - 128 : 383 - triangle;
        buffer[i] = (int16_t)(triangle * envelope / 5);
        ++music_demo_samples;
    }
}

static void music_worker(void *parameter)
{
    rt_device_t device = RT_NULL;
    music_wav_t wav = {0};
    int16_t buffer[MUSIC_BLOCK_SAMPLES];
    rt_tick_t last_scan = 0;
    bool opened = false;
    int command;

    (void)parameter;
    music_scan();
    while (1)
    {
        int state = s_status.state;
        rt_int32_t timeout = state == POCKETJS_MUSIC_PLAYING ? RT_WAITING_NO :
                             rt_tick_from_millisecond(1000);
        if (rt_mq_recv(s_commands, &command, sizeof(command), timeout) > 0)
        {
            if (command == POCKETJS_MUSIC_SUSPEND)
            {
                s_resume_playing = state == POCKETJS_MUSIC_PLAYING;
                if (s_resume_playing)
                {
                    music_set_state(POCKETJS_MUSIC_PAUSED);
                }
                if (wav.file != RT_NULL) fclose(wav.file);
                wav.file = RT_NULL;
                if (opened) rt_device_close(device);
                opened = false;
                s_suspended = RT_TRUE;
            }
            else if (command == POCKETJS_MUSIC_RESUME)
            {
                s_suspended = RT_FALSE;
                if (s_resume_playing)
                {
                    music_set_state(POCKETJS_MUSIC_PLAYING);
                }
                s_resume_playing = false;
            }
            else if (s_suspended && command != POCKETJS_MUSIC_VOLUME_UP &&
                     command != POCKETJS_MUSIC_VOLUME_DOWN)
            {
                /* The game owns the audio device until it resumes us. */
            }
            else if (command == POCKETJS_MUSIC_TOGGLE)
            {
                music_set_state(state == POCKETJS_MUSIC_PLAYING ? POCKETJS_MUSIC_PAUSED :
                                POCKETJS_MUSIC_PLAYING);
            }
            else if (command == POCKETJS_MUSIC_NEXT || command == POCKETJS_MUSIC_PREVIOUS)
            {
                music_select(command == POCKETJS_MUSIC_NEXT ? 1 : -1);
                if (state != POCKETJS_MUSIC_STOPPED)
                {
                    music_set_state(POCKETJS_MUSIC_PLAYING);
                }
                if (wav.file != RT_NULL) fclose(wav.file);
                wav.file = RT_NULL;
                if (opened) rt_device_close(device);
                opened = false;
            }
            else if (command == POCKETJS_MUSIC_VOLUME_UP || command == POCKETJS_MUSIC_VOLUME_DOWN)
            {
                music_set_volume(opened ? device : RT_NULL,
                                 command == POCKETJS_MUSIC_VOLUME_UP ? 10 : -10);
            }
            else if (command == POCKETJS_MUSIC_SCAN)
            {
                music_scan();
            }
        }
        if (s_status.state != POCKETJS_MUSIC_PLAYING)
        {
            if ((rt_tick_get() - last_scan) > rt_tick_from_millisecond(10000) && !opened)
            {
                music_scan();
                last_scan = rt_tick_get();
            }
            continue;
        }
        if (!opened)
        {
            struct rt_audio_caps caps = {0};
            int index = music_get_index();

            if (index != 0 && !music_open_wav(s_tracks[index].path, &wav))
            {
                music_set_state(POCKETJS_MUSIC_ERROR);
                continue;
            }
            device = rt_device_find("sound0");
            if (device == RT_NULL || rt_device_open(device, RT_DEVICE_OFLAG_WRONLY) != RT_EOK)
            {
                if (wav.file != RT_NULL) fclose(wav.file);
                wav.file = RT_NULL;
                music_set_state(POCKETJS_MUSIC_ERROR);
                continue;
            }
            caps.main_type = AUDIO_TYPE_OUTPUT;
            caps.sub_type = AUDIO_DSP_PARAM;
            caps.udata.config.samplerate = index == 0 ? 16000 : wav.sample_rate;
            caps.udata.config.channels = index == 0 ? 1 : wav.channels;
            caps.udata.config.samplebits = 16;
            (void)rt_device_control(device, AUDIO_CTL_CONFIGURE, &caps);
            music_set_volume(device, 0);
            music_demo_phase = 0;
            music_demo_samples = 0;
            opened = true;
        }
        if (wav.file != RT_NULL)
        {
            size_t bytes = wav.remaining < sizeof(buffer) ? wav.remaining : sizeof(buffer);
            size_t read = fread(buffer, 1, bytes, wav.file);
            if (read == 0)
            {
                music_set_state(POCKETJS_MUSIC_STOPPED);
            }
            else
            {
                wav.remaining -= read;
                (void)rt_device_write(device, 0, buffer, read);
                if (wav.remaining == 0) music_set_state(POCKETJS_MUSIC_STOPPED);
            }
        }
        else
        {
            music_fill_demo(buffer);
            (void)rt_device_write(device, 0, buffer, sizeof(buffer));
        }
        if (s_status.state == POCKETJS_MUSIC_STOPPED || s_status.state == POCKETJS_MUSIC_ERROR)
        {
            if (wav.file != RT_NULL) fclose(wav.file);
            wav.file = RT_NULL;
            rt_device_close(device);
            opened = false;
        }
    }
}

rt_err_t pocketjs_music_start(void)
{
    rt_thread_t thread;

    if (s_started) return RT_EOK;
    s_lock = rt_mutex_create("pjsmusic", RT_IPC_FLAG_PRIO);
    s_commands = rt_mq_create("pjsmusic", sizeof(int), 8, RT_IPC_FLAG_FIFO);
    if (s_lock == RT_NULL || s_commands == RT_NULL) return -RT_ENOMEM;
    thread = rt_thread_create("pjsmusic", music_worker, RT_NULL,
                              MUSIC_THREAD_STACK, 18, 10);
    if (thread == RT_NULL) return -RT_ENOMEM;
    s_started = RT_TRUE;
    return rt_thread_startup(thread);
}

bool pocketjs_music_command(int command)
{
    return s_commands != RT_NULL &&
           rt_mq_send(s_commands, &command, sizeof(command)) == RT_EOK;
}

void pocketjs_music_status(pocketjs_music_status_t *status)
{
    if (status == RT_NULL) return;
    if (s_lock == RT_NULL)
    {
        *status = s_status;
        return;
    }
    rt_mutex_take(s_lock, RT_WAITING_FOREVER);
    *status = s_status;
    rt_mutex_release(s_lock);
}

bool pocketjs_music_suspend(void)
{
    int attempt;

    if (s_commands == RT_NULL || !s_started)
    {
        return true;
    }
    if (s_suspended)
    {
        return true;
    }
    for (attempt = 0; attempt < 20; ++attempt)
    {
        if (pocketjs_music_command(POCKETJS_MUSIC_SUSPEND))
        {
            break;
        }
        rt_thread_mdelay(10);
    }
    for (attempt = 0; attempt < 120 && !s_suspended; ++attempt)
    {
        rt_thread_mdelay(5);
    }
    return s_suspended;
}

void pocketjs_music_resume(void)
{
    int attempt;

    if (s_commands == RT_NULL || !s_started || !s_suspended)
    {
        return;
    }
    for (attempt = 0; attempt < 20; ++attempt)
    {
        if (pocketjs_music_command(POCKETJS_MUSIC_RESUME))
        {
            break;
        }
        rt_thread_mdelay(10);
    }
    /* Wait for the worker so an immediate suspend() cannot race the resume. */
    for (attempt = 0; attempt < 100 && s_suspended; ++attempt)
    {
        rt_thread_mdelay(5);
    }
}

int pocketjs_music_volume(void)
{
    return s_status.volume;
}
