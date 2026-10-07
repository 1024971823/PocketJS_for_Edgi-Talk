/*
 * Small native status source for the PocketJS home screen. Network work stays
 * on a low-priority worker; the UI task only copies an already cached value.
 */

#include "pocketjs_dashboard.h"
#include "pocketjs_game.h"
#include "pocketjs_music.h"
#include "pocketjs_wifi.h"
#include "pocketjs_bt.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/wlan.h>
#include <socket/netdb.h>
#include <socket/sys_socket/sys/socket.h>
#include <socket/netinet/in.h>
#include <string.h>
#include <time.h>

#define DASHBOARD_UNKNOWN_TEMP       (-9999)
#define DASHBOARD_WEATHER_STACK_SIZE (8192U)
#define DASHBOARD_WEATHER_PRIORITY   26U
#define DASHBOARD_POLL_OFFLINE_MS    (15000U)
#define DASHBOARD_POLL_ONLINE_MS     (900000U)
#define DASHBOARD_RESPONSE_CAPACITY  (1536U)
#define DASHBOARD_SENSOR_STACK_SIZE  (2048U)
#define DASHBOARD_SENSOR_PERIOD_MS   (5000U)
#define AHT20_ADDRESS                (0x38U)
#define LSM6DS3_ID_REGISTER          (0x0FU)
#define LSM6DS3_TEMP_REGISTER        (0x20U)

enum dashboard_temp_source
{
    DASHBOARD_TEMP_NONE,
    DASHBOARD_TEMP_AHT20,
    DASHBOARD_TEMP_IMU,
};

typedef struct
{
    rt_uint8_t wifi;
    rt_int16_t weather_temp_tenths;
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1U];
    char weather[12];
} dashboard_network_t;

static dashboard_network_t s_network = {
    .weather_temp_tenths = DASHBOARD_UNKNOWN_TEMP,
    .ssid = "OFFLINE",
    .weather = "SYNCING",
};
static dashboard_network_t s_network_read_cache = {
    .weather_temp_tenths = DASHBOARD_UNKNOWN_TEMP,
    .ssid = "OFFLINE",
    .weather = "SYNCING",
};
static struct rt_mutex s_network_lock;
static rt_bool_t s_started;
static rt_thread_t s_weather_thread;
static rt_thread_t s_sensor_thread;
static volatile rt_int16_t s_board_temp_tenths = DASHBOARD_UNKNOWN_TEMP;
static volatile rt_uint8_t s_board_temp_source = DASHBOARD_TEMP_NONE;
static volatile rt_uint8_t s_aht20_probe_result;
static volatile rt_uint8_t s_imu_probe_result;

static struct rt_timer s_cpu_timer;
static rt_thread_t s_idle_thread;
static volatile rt_tick_t s_cpu_idle_ticks;
static rt_tick_t s_cpu_last_idle_ticks;
static rt_tick_t s_cpu_sample_ticks;
static volatile rt_uint8_t s_cpu_percent;

static rt_tick_t s_fps_started_at;
static uint32_t s_fps_presented;
static volatile rt_uint8_t s_fps;

static rt_bool_t dashboard_i2c_write(struct rt_i2c_bus_device *bus,
                                      rt_uint16_t address, rt_uint8_t *data,
                                      rt_uint16_t length)
{
    struct rt_i2c_msg message = {0};

    message.addr = address;
    message.flags = RT_I2C_WR;
    message.buf = data;
    message.len = length;
    return rt_i2c_transfer(bus, &message, 1) == 1;
}

static rt_bool_t dashboard_i2c_read(struct rt_i2c_bus_device *bus,
                                     rt_uint16_t address, rt_uint8_t *data,
                                     rt_uint16_t length)
{
    struct rt_i2c_msg message = {0};

    message.addr = address;
    message.flags = RT_I2C_RD;
    message.buf = data;
    message.len = length;
    return rt_i2c_transfer(bus, &message, 1) == 1;
}

static rt_bool_t dashboard_i2c_read_register(struct rt_i2c_bus_device *bus,
                                              rt_uint16_t address, rt_uint8_t reg,
                                              rt_uint8_t *data, rt_uint16_t length)
{
    struct rt_i2c_msg messages[2] = {0};

    messages[0].addr = address;
    messages[0].flags = RT_I2C_WR;
    messages[0].buf = &reg;
    messages[0].len = 1;
    messages[1].addr = address;
    messages[1].flags = RT_I2C_RD;
    messages[1].buf = data;
    messages[1].len = length;
    return rt_i2c_transfer(bus, messages, 2) == 2;
}

static rt_bool_t dashboard_read_aht20(rt_int16_t *temperature)
{
    static const char *const buses[] = {"i2c2", "i2c1"};
    rt_uint8_t initialize[] = {0xBEU, 0x08U, 0x00U};
    rt_uint8_t trigger[] = {0xACU, 0x33U, 0x00U};
    rt_uint8_t data[6];
    rt_uint32_t raw;
    rt_size_t index;

    s_aht20_probe_result = 1U;
    for (index = 0; index < sizeof(buses) / sizeof(buses[0]); ++index)
    {
        struct rt_i2c_bus_device *bus =
            (struct rt_i2c_bus_device *)rt_device_find(buses[index]);

        if (bus == RT_NULL ||
            !dashboard_i2c_write(bus, AHT20_ADDRESS, initialize, sizeof(initialize)))
        {
            continue;
        }
        rt_thread_mdelay(10);
        if (!dashboard_i2c_write(bus, AHT20_ADDRESS, trigger, sizeof(trigger)))
        {
            s_aht20_probe_result = 2U;
            continue;
        }
        rt_thread_mdelay(85);
        if (!dashboard_i2c_read(bus, AHT20_ADDRESS, data, sizeof(data)))
        {
            s_aht20_probe_result = 3U;
            continue;
        }
        if ((data[0] & 0x80U) != 0U || (data[0] & 0x08U) == 0U)
        {
            s_aht20_probe_result = 4U;
            continue;
        }
        raw = ((rt_uint32_t)(data[3] & 0x0FU) << 16) |
              ((rt_uint32_t)data[4] << 8) | data[5];
        *temperature = (rt_int16_t)((rt_int32_t)((raw * 2000U) >> 20) - 500);
        s_aht20_probe_result = 0U;
        return RT_TRUE;
    }
    return RT_FALSE;
}

static rt_bool_t dashboard_read_imu_temp(rt_int16_t *temperature)
{
    static const char *const buses[] = {"i2c2", "i2c0", "i2c1"};
    static const rt_uint16_t addresses[] = {0x6AU, 0x6BU};
    rt_size_t bus_index;
    rt_size_t address_index;

    s_imu_probe_result = 1U;
    for (bus_index = 0; bus_index < sizeof(buses) / sizeof(buses[0]); ++bus_index)
    {
        struct rt_i2c_bus_device *bus =
            (struct rt_i2c_bus_device *)rt_device_find(buses[bus_index]);
        if (bus == RT_NULL)
        {
            continue;
        }
        for (address_index = 0; address_index < sizeof(addresses) / sizeof(addresses[0]); ++address_index)
        {
            rt_uint8_t id;
            rt_uint8_t accel_control;
            rt_uint8_t data[2];
            rt_int16_t raw;

            if (!dashboard_i2c_read_register(bus, addresses[address_index],
                                              LSM6DS3_ID_REGISTER, &id, 1) || id != 0x6AU)
            {
                continue;
            }
            s_imu_probe_result = 2U;
            if (!dashboard_i2c_read_register(bus, addresses[address_index],
                                              0x10U, &accel_control, 1))
            {
                continue;
            }
            if ((accel_control & 0xF0U) == 0U)
            {
                rt_uint8_t enable_accel[] = {0x10U, 0x30U};
                if (!dashboard_i2c_write(bus, addresses[address_index],
                                          enable_accel, sizeof(enable_accel)))
                {
                    continue;
                }
                rt_thread_mdelay(45);
            }
            if (!dashboard_i2c_read_register(bus, addresses[address_index],
                                              LSM6DS3_TEMP_REGISTER, data, sizeof(data)))
            {
                continue;
            }
            raw = (rt_int16_t)(((rt_uint16_t)data[1] << 8) | data[0]);
            *temperature = (rt_int16_t)(250 + ((rt_int32_t)raw * 10) / 256);
            s_imu_probe_result = 0U;
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

static void dashboard_sensor_task(void *parameter)
{
    rt_int16_t temperature;

    (void)parameter;
    while (1)
    {
        if (dashboard_read_aht20(&temperature))
        {
            s_board_temp_tenths = temperature;
            s_board_temp_source = DASHBOARD_TEMP_AHT20;
        }
        else if (dashboard_read_imu_temp(&temperature))
        {
            s_board_temp_tenths = temperature;
            s_board_temp_source = DASHBOARD_TEMP_IMU;
        }
        else
        {
            s_board_temp_tenths = DASHBOARD_UNKNOWN_TEMP;
            s_board_temp_source = DASHBOARD_TEMP_NONE;
        }
        rt_thread_mdelay(DASHBOARD_SENSOR_PERIOD_MS);
    }
}

static void dashboard_copy_network(dashboard_network_t *out)
{
    if (out == RT_NULL)
    {
        return;
    }
    if (rt_mutex_take(&s_network_lock, RT_WAITING_NO) == RT_EOK)
    {
        *out = s_network;
        s_network_read_cache = s_network;
        rt_mutex_release(&s_network_lock);
    }
    else
    {
        *out = s_network_read_cache;
    }
}

static void dashboard_store_network(const dashboard_network_t *next)
{
    if (next == RT_NULL)
    {
        return;
    }
    if (rt_mutex_take(&s_network_lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        s_network = *next;
        rt_mutex_release(&s_network_lock);
    }
}

static void dashboard_update_wifi(dashboard_network_t *next)
{
    struct rt_wlan_info info;

    if (next == RT_NULL)
    {
        return;
    }
    rt_memset(&info, 0, sizeof(info));
    next->wifi = RT_FALSE;
    rt_strncpy(next->ssid, "OFFLINE", sizeof(next->ssid));

    if (rt_wlan_get_info(&info) != RT_EOK)
    {
        return;
    }
    next->wifi = RT_TRUE;
    if (info.ssid.len != 0U)
    {
        rt_size_t length = info.ssid.len;
        if (length >= sizeof(next->ssid))
        {
            length = sizeof(next->ssid) - 1U;
        }
        rt_memcpy(next->ssid, info.ssid.val, length);
        next->ssid[length] = '\0';
    }
    else
    {
        rt_strncpy(next->ssid, "CONNECTED", sizeof(next->ssid));
    }
}

static rt_bool_t dashboard_parse_tenths(const char *json, const char *key,
                                        rt_int16_t *out)
{
    const char *value;
    int sign = 1;
    int whole = 0;
    int tenths = 0;

    if (json == RT_NULL || key == RT_NULL || out == RT_NULL)
    {
        return RT_FALSE;
    }
    value = rt_strstr(json, key);
    if (value == RT_NULL)
    {
        return RT_FALSE;
    }
    value += rt_strlen(key);
    while (*value == ' ' || *value == ':')
    {
        ++value;
    }
    if (*value == '-')
    {
        sign = -1;
        ++value;
    }
    if (*value < '0' || *value > '9')
    {
        return RT_FALSE;
    }
    while (*value >= '0' && *value <= '9')
    {
        whole = whole * 10 + (*value - '0');
        ++value;
    }
    if (*value == '.')
    {
        ++value;
        if (*value >= '0' && *value <= '9')
        {
            tenths = *value - '0';
        }
    }
    *out = (rt_int16_t)(sign * (whole * 10 + tenths));
    return RT_TRUE;
}

static rt_bool_t dashboard_parse_integer(const char *json, const char *key,
                                         int *out)
{
    const char *value;
    int sign = 1;
    int result = 0;

    if (json == RT_NULL || key == RT_NULL || out == RT_NULL)
    {
        return RT_FALSE;
    }
    value = rt_strstr(json, key);
    if (value == RT_NULL)
    {
        return RT_FALSE;
    }
    value += rt_strlen(key);
    while (*value == ' ' || *value == ':')
    {
        ++value;
    }
    if (*value == '-')
    {
        sign = -1;
        ++value;
    }
    if (*value < '0' || *value > '9')
    {
        return RT_FALSE;
    }
    while (*value >= '0' && *value <= '9')
    {
        result = result * 10 + (*value - '0');
        ++value;
    }
    *out = sign * result;
    return RT_TRUE;
}

static const char *dashboard_weather_name(int code)
{
    if (code == 0)
    {
        return "CLEAR";
    }
    if (code <= 3)
    {
        return "CLOUDY";
    }
    if (code == 45 || code == 48)
    {
        return "FOG";
    }
    if (code >= 71 && code <= 77)
    {
        return "SNOW";
    }
    if (code >= 80)
    {
        return "SHOWERS";
    }
    return "RAIN";
}

#define DASHBOARD_DEFAULT_TZ_MINUTES 480 /* UTC+8; the weather location is Shanghai */
#define DASHBOARD_PC_TIME_HOLD_MS    (10U * 60U * 1000U)
#define DASHBOARD_VALID_YEAR         2024

static rt_tick_t s_pc_time_tick;
static rt_bool_t s_pc_time_seen;
static rt_bool_t s_clock_valid;

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm). */
static int64_t dashboard_days_from_civil(int year, int month, int day)
{
    int64_t y = year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

bool pocketjs_dashboard_set_time(int64_t unix_utc, int tz_minutes, bool from_pc)
{
    /* Newlib has no zone database here, so the soft RTC simply holds local time. */
    int64_t local = unix_utc + (int64_t)tz_minutes * 60;

    if (unix_utc < 1704067200 || tz_minutes < -720 || tz_minutes > 840)
    {
        return false;
    }
    if (from_pc)
    {
        s_pc_time_tick = rt_tick_get();
        s_pc_time_seen = RT_TRUE;
    }
    else if (s_pc_time_seen &&
             (rt_tick_get() - s_pc_time_tick) < rt_tick_from_millisecond(DASHBOARD_PC_TIME_HOLD_MS))
    {
        return true;
    }
    /* Only step the clock when it is really off, so the calendar never flickers. */
    {
        time_t now = time(RT_NULL);
        int64_t drift = (int64_t)now - local;

        if (s_clock_valid && drift > -2 && drift < 2)
        {
            return true;
        }
    }
    if (set_timestamp((time_t)local) != RT_EOK)
    {
        return false;
    }
    s_clock_valid = RT_TRUE;
    return true;
}

/* "Date: Wed, 07 Oct 2026 06:14:05 GMT" anywhere in the response headers. */
static void dashboard_sync_clock_from_http(const char *response)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *line = response;
    int day;
    int year;
    int hour;
    int minute;
    int second;
    int month = 0;
    int i;

    while (line != RT_NULL && *line != '\0')
    {
        if ((line[0] == 'D' || line[0] == 'd') && (line[1] == 'a' || line[1] == 'A') &&
            (line[2] == 't' || line[2] == 'T') && (line[3] == 'e' || line[3] == 'E') &&
            line[4] == ':')
        {
            break;
        }
        line = rt_strstr(line, "\r\n");
        if (line != RT_NULL)
        {
            line += 2;
            if (line[0] == '\r')
            {
                return; /* end of headers */
            }
        }
    }
    if (line == RT_NULL || *line == '\0')
    {
        return;
    }
    line += 5;
    while (*line == ' ')
    {
        ++line;
    }
    /* Skip the weekday name and comma. */
    while (*line != '\0' && *line != ',')
    {
        ++line;
    }
    if (*line != ',')
    {
        return;
    }
    ++line;
    while (*line == ' ')
    {
        ++line;
    }
    day = 0;
    while (*line >= '0' && *line <= '9')
    {
        day = day * 10 + (*line++ - '0');
    }
    while (*line == ' ')
    {
        ++line;
    }
    for (i = 0; i < 12; ++i)
    {
        if (rt_strncmp(line, months[i], 3) == 0)
        {
            month = i + 1;
            break;
        }
    }
    if (month == 0)
    {
        return;
    }
    line += 3;
    year = 0;
    while (*line == ' ')
    {
        ++line;
    }
    while (*line >= '0' && *line <= '9')
    {
        year = year * 10 + (*line++ - '0');
    }
    while (*line == ' ')
    {
        ++line;
    }
    hour = (line[0] - '0') * 10 + (line[1] - '0');
    minute = (line[3] - '0') * 10 + (line[4] - '0');
    second = (line[6] - '0') * 10 + (line[7] - '0');
    if (day < 1 || day > 31 || year < DASHBOARD_VALID_YEAR || hour > 23 || minute > 59 || second > 60)
    {
        return;
    }
    (void)pocketjs_dashboard_set_time(
        dashboard_days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second,
        DASHBOARD_DEFAULT_TZ_MINUTES, false);
}

static rt_bool_t dashboard_fetch_weather(dashboard_network_t *next)
{
    static const char request[] =
        "GET /v1/forecast?latitude=31.23&longitude=121.47&current=temperature_2m,weather_code HTTP/1.0\r\n"
        "Host: api.open-meteo.com\r\n"
        "Connection: close\r\n\r\n";
    struct hostent *host;
    struct sockaddr_in address;
    char response[DASHBOARD_RESPONSE_CAPACITY + 1U];
    int socket_fd = -1;
    int timeout = 5000;
    int received;
    rt_size_t used = 0U;
    rt_int16_t temperature;
    int code;

    if (next == RT_NULL)
    {
        return RT_FALSE;
    }
    host = gethostbyname("api.open-meteo.com");
    if (host == RT_NULL || host->h_addr_list == RT_NULL ||
        host->h_addr_list[0] == RT_NULL)
    {
        return RT_FALSE;
    }

    rt_memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(80U);
    rt_memcpy(&address.sin_addr, host->h_addr_list[0], host->h_length);

    socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0)
    {
        return RT_FALSE;
    }
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    if (connect(socket_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        send(socket_fd, request, sizeof(request) - 1U, 0) < 0)
    {
        closesocket(socket_fd);
        return RT_FALSE;
    }

    while (used < DASHBOARD_RESPONSE_CAPACITY)
    {
        received = recv(socket_fd, response + used,
                        DASHBOARD_RESPONSE_CAPACITY - used, 0);
        if (received <= 0)
        {
            break;
        }
        used += (rt_size_t)received;
    }
    closesocket(socket_fd);
    response[used] = '\0';
    dashboard_sync_clock_from_http(response);

    if (!dashboard_parse_tenths(response, "\"temperature_2m\"", &temperature) ||
        !dashboard_parse_integer(response, "\"weather_code\"", &code))
    {
        return RT_FALSE;
    }
    next->weather_temp_tenths = temperature;
    rt_strncpy(next->weather, dashboard_weather_name(code), sizeof(next->weather));
    return RT_TRUE;
}

static void dashboard_weather_task(void *parameter)
{
    dashboard_network_t next;

    (void)parameter;
    while (1)
    {
        dashboard_copy_network(&next);
        dashboard_update_wifi(&next);
        if (next.wifi)
        {
            if (!dashboard_fetch_weather(&next) &&
                next.weather_temp_tenths == DASHBOARD_UNKNOWN_TEMP)
            {
                rt_strncpy(next.weather, "SYNCING", sizeof(next.weather));
            }
            dashboard_store_network(&next);
            rt_thread_mdelay(s_clock_valid ? DASHBOARD_POLL_ONLINE_MS : DASHBOARD_POLL_OFFLINE_MS);
        }
        else
        {
            next.weather_temp_tenths = DASHBOARD_UNKNOWN_TEMP;
            rt_strncpy(next.weather, "OFFLINE", sizeof(next.weather));
            dashboard_store_network(&next);
            rt_thread_mdelay(DASHBOARD_POLL_OFFLINE_MS);
        }
    }
}

static void dashboard_cpu_timer(void *parameter)
{
    rt_tick_t idle_delta;

    (void)parameter;
    if (rt_thread_self() == s_idle_thread)
    {
        ++s_cpu_idle_ticks;
    }
    if (++s_cpu_sample_ticks < RT_TICK_PER_SECOND)
    {
        return;
    }
    s_cpu_sample_ticks = 0U;
    idle_delta = s_cpu_idle_ticks - s_cpu_last_idle_ticks;
    s_cpu_last_idle_ticks = s_cpu_idle_ticks;
    if (idle_delta > RT_TICK_PER_SECOND)
    {
        idle_delta = RT_TICK_PER_SECOND;
    }
    s_cpu_percent = (rt_uint8_t)(RT_TICK_PER_SECOND - idle_delta) * 100U /
                    RT_TICK_PER_SECOND;
}

void pocketjs_dashboard_note_frame(void)
{
    rt_tick_t now = rt_tick_get();
    rt_tick_t elapsed;

    if (!s_started)
    {
        return;
    }
    ++s_fps_presented;
    elapsed = now - s_fps_started_at;
    if (elapsed < RT_TICK_PER_SECOND)
    {
        return;
    }
    s_fps = (rt_uint8_t)((s_fps_presented * RT_TICK_PER_SECOND) / elapsed);
    s_fps_presented = 0U;
    s_fps_started_at = now;
}

static void dashboard_calendar(JSContext *context, JSValue object)
{
    time_t now = time(RT_NULL);
    struct tm *calendar = localtime(&now);
    rt_bool_t valid = (calendar != RT_NULL) && (calendar->tm_year + 1900 >= DASHBOARD_VALID_YEAR);

    /* month 0 means "clock not set yet": the UI shows dashes instead of a made-up date. */
    JS_SetPropertyStr(context, object, "year", JS_NewInt32(context, valid ? calendar->tm_year + 1900 : 0));
    JS_SetPropertyStr(context, object, "month", JS_NewInt32(context, valid ? calendar->tm_mon + 1 : 0));
    JS_SetPropertyStr(context, object, "day", JS_NewInt32(context, valid ? calendar->tm_mday : 0));
    JS_SetPropertyStr(context, object, "weekday", JS_NewInt32(context, valid ? calendar->tm_wday : 0));
}

/* -- PC stats pushed over Wi-Fi ----------------------------------------------------------- */

typedef struct
{
    rt_bool_t seen;
    rt_tick_t tick;
    char host[17];
    int cpu;
    int ram_pct;
    int disk_pct;
    int temp; /* degrees C, -1 when the PC has no sensor */
    int ram_used_mb;
    int ram_total_mb;
    int disk_used_gb;
    int disk_total_gb;
} dashboard_pc_t;

static dashboard_pc_t s_pc;

static int dashboard_clamp(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

static rt_bool_t dashboard_parse_string(const char *json, const char *key, char *out, rt_size_t capacity)
{
    const char *value = rt_strstr(json, key);
    rt_size_t used = 0U;

    if (value == RT_NULL || capacity == 0U)
    {
        return RT_FALSE;
    }
    value += rt_strlen(key);
    while (*value == ' ' || *value == ':')
    {
        ++value;
    }
    if (*value != '"')
    {
        return RT_FALSE;
    }
    ++value;
    /* The baked fonts only cover printable ASCII. */
    while (*value != '\0' && *value != '"' && used + 1U < capacity)
    {
        out[used++] = (*value >= 32 && *value < 127 && *value != '\\') ? *value : '?';
        ++value;
    }
    out[used] = '\0';
    return RT_TRUE;
}

bool pocketjs_dashboard_pc_ingest(const char *json)
{
    dashboard_pc_t next = {0};
    int value;
    int unix_time = 0;
    int zone = DASHBOARD_DEFAULT_TZ_MINUTES;

    if (json == RT_NULL || !dashboard_parse_integer(json, "\"cpu\"", &value))
    {
        return false;
    }
    next.cpu = dashboard_clamp(value, 0, 100);
    next.ram_pct = dashboard_parse_integer(json, "\"ramPct\"", &value) ? dashboard_clamp(value, 0, 100) : 0;
    next.disk_pct = dashboard_parse_integer(json, "\"diskPct\"", &value) ? dashboard_clamp(value, 0, 100) : 0;
    next.temp = dashboard_parse_integer(json, "\"temp\"", &value) ? dashboard_clamp(value, -1, 150) : -1;
    next.ram_used_mb = dashboard_parse_integer(json, "\"ramUsedMb\"", &value) ? dashboard_clamp(value, 0, 9999999) : 0;
    next.ram_total_mb = dashboard_parse_integer(json, "\"ramTotalMb\"", &value) ? dashboard_clamp(value, 0, 9999999) : 0;
    next.disk_used_gb = dashboard_parse_integer(json, "\"diskUsedGb\"", &value) ? dashboard_clamp(value, 0, 9999999) : 0;
    next.disk_total_gb = dashboard_parse_integer(json, "\"diskTotalGb\"", &value) ? dashboard_clamp(value, 0, 9999999) : 0;
    if (!dashboard_parse_string(json, "\"host\"", next.host, sizeof(next.host)))
    {
        rt_strncpy(next.host, "PC", sizeof(next.host));
    }
    next.seen = RT_TRUE;
    next.tick = rt_tick_get();

    rt_enter_critical();
    s_pc = next;
    rt_exit_critical();

    if (dashboard_parse_integer(json, "\"t\"", &unix_time))
    {
        (void)dashboard_parse_integer(json, "\"tz\"", &zone);
        (void)pocketjs_dashboard_set_time((int64_t)(unsigned int)unix_time, zone, true);
    }
    return true;
}

static rt_uint32_t dashboard_pc_age_ms(const dashboard_pc_t *pc)
{
    return pc->seen ? (rt_uint32_t)((rt_tick_get() - pc->tick) * 1000U / RT_TICK_PER_SECOND) : 0xFFFFFFFFU;
}

size_t pocketjs_dashboard_pc_json(char *out, size_t capacity)
{
    dashboard_pc_t pc;

    rt_enter_critical();
    pc = s_pc;
    rt_exit_critical();
    return (size_t)rt_snprintf(out, capacity,
                               "{\"seen\":%s,\"ageMs\":%d,\"host\":\"%s\",\"cpu\":%d,\"ramPct\":%d,"
                               "\"ramUsedMb\":%d,\"ramTotalMb\":%d,\"diskPct\":%d,\"diskUsedGb\":%d,"
                               "\"diskTotalGb\":%d,\"temp\":%d}",
                               pc.seen ? "true" : "false", pc.seen ? (int)dashboard_pc_age_ms(&pc) : -1,
                               pc.host, pc.cpu, pc.ram_pct, pc.ram_used_mb, pc.ram_total_mb, pc.disk_pct,
                               pc.disk_used_gb, pc.disk_total_gb, pc.temp);
}

static JSValue dashboard_pc(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv)
{
    dashboard_pc_t pc;
    JSValue object;

    (void)this_value;
    (void)argc;
    (void)argv;
    rt_enter_critical();
    pc = s_pc;
    rt_exit_critical();
    object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "seen", JS_NewBool(context, pc.seen));
    JS_SetPropertyStr(context, object, "ageMs",
                      JS_NewInt32(context, pc.seen ? (int)dashboard_pc_age_ms(&pc) : -1));
    JS_SetPropertyStr(context, object, "host", JS_NewString(context, pc.host));
    JS_SetPropertyStr(context, object, "cpu", JS_NewInt32(context, pc.cpu));
    JS_SetPropertyStr(context, object, "ram", JS_NewInt32(context, pc.ram_pct));
    JS_SetPropertyStr(context, object, "ramUsedMb", JS_NewInt32(context, pc.ram_used_mb));
    JS_SetPropertyStr(context, object, "ramTotalMb", JS_NewInt32(context, pc.ram_total_mb));
    JS_SetPropertyStr(context, object, "disk", JS_NewInt32(context, pc.disk_pct));
    JS_SetPropertyStr(context, object, "diskUsedGb", JS_NewInt32(context, pc.disk_used_gb));
    JS_SetPropertyStr(context, object, "diskTotalGb", JS_NewInt32(context, pc.disk_total_gb));
    JS_SetPropertyStr(context, object, "temp", JS_NewInt32(context, pc.temp));
    return object;
}

static JSValue dashboard_status(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    dashboard_network_t network;
    pocketjs_wifi_status_t wifi_status;
    rt_size_t total = 0U;
    rt_size_t used = 0U;
    JSValue object;

    (void)this_value;
    (void)argc;
    (void)argv;
    dashboard_copy_network(&network);
    pocketjs_wifi_status(&wifi_status);
    rt_memory_info(&total, &used, RT_NULL);
    object = JS_NewObject(context);
    if (JS_IsException(object))
    {
        return object;
    }
    JS_SetPropertyStr(context, object, "cpu", JS_NewInt32(context, s_cpu_percent));
    JS_SetPropertyStr(context, object, "ram",
                      JS_NewInt32(context, total == 0U ? 0 :
                                  (int)((used * 100U + total / 2U) / total)));
    JS_SetPropertyStr(context, object, "fps", JS_NewInt32(context, s_fps));
    JS_SetPropertyStr(context, object, "temp",
                      JS_NewInt32(context, s_board_temp_tenths));
    JS_SetPropertyStr(context, object, "tempSource",
                      JS_NewInt32(context, s_board_temp_source));
    JS_SetPropertyStr(context, object, "wifi", JS_NewBool(context, network.wifi));
    JS_SetPropertyStr(context, object, "bt", JS_NewBool(context, pocketjs_bt_advertising()));
    JS_SetPropertyStr(context, object, "ssid", JS_NewString(context, network.ssid));
    {
        char token[8];

        (void)pocketjs_game_token(token);
        JS_SetPropertyStr(context, object, "ip", JS_NewString(context, wifi_status.sta_ip));
        JS_SetPropertyStr(context, object, "token", JS_NewString(context, token));
    }
    JS_SetPropertyStr(context, object, "ap", JS_NewBool(context, wifi_status.ap_active));
    JS_SetPropertyStr(context, object, "apSsid", JS_NewString(context, wifi_status.ap_ssid));
    JS_SetPropertyStr(context, object, "apPassword", JS_NewString(context, wifi_status.ap_password));
    JS_SetPropertyStr(context, object, "weather", JS_NewString(context, network.weather));
    JS_SetPropertyStr(context, object, "weatherTemp",
                      JS_NewInt32(context, network.weather_temp_tenths));
    dashboard_calendar(context, object);
    return object;
}

static JSValue dashboard_music_status(JSContext *context, JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    pocketjs_music_status_t status;
    JSValue object;

    (void)this_value;
    (void)argc;
    (void)argv;
    pocketjs_music_status(&status);
    object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "track", JS_NewString(context, status.track));
    JS_SetPropertyStr(context, object, "index", JS_NewInt32(context, status.index));
    JS_SetPropertyStr(context, object, "count", JS_NewInt32(context, status.count));
    JS_SetPropertyStr(context, object, "volume", JS_NewInt32(context, status.volume));
    JS_SetPropertyStr(context, object, "state", JS_NewInt32(context, status.state));
    JS_SetPropertyStr(context, object, "sd", JS_NewBool(context, status.sd_present));
    return object;
}

static JSValue dashboard_music_command(JSContext *context, JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    int32_t command;

    (void)this_value;
    if (argc < 1 || JS_ToInt32(context, &command, argv[0]) < 0)
    {
        return JS_FALSE;
    }
    if (command < POCKETJS_MUSIC_TOGGLE || command > POCKETJS_MUSIC_SCAN)
    {
        return JS_FALSE;
    }
    return JS_NewBool(context, pocketjs_music_command(command));
}

/* -- Beat Dash bridge ---------------------------------------------------------- */

static int32_t dashboard_int_arg(JSContext *context, JSValueConst value, int32_t fallback)
{
    int32_t number;

    return JS_ToInt32(context, &number, value) < 0 ? fallback : number;
}

/* gameStart(bpm, [step, voice, note, length, volume, ...]) */
static JSValue dashboard_game_start(JSContext *context, JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    pocketjs_game_event_t *events;
    JSValue length_value;
    uint32_t length = 0U;
    uint32_t count;
    uint32_t i;
    int32_t bpm;
    bool started;

    (void)this_value;
    if (argc < 2 || !JS_IsArray(argv[1]))
    {
        return JS_FALSE;
    }
    bpm = dashboard_int_arg(context, argv[0], 0);
    length_value = JS_GetPropertyStr(context, argv[1], "length");
    if (JS_ToUint32(context, &length, length_value) < 0)
    {
        length = 0U;
    }
    JS_FreeValue(context, length_value);
    count = length / 5U;
    if (count == 0U || count > POCKETJS_GAME_MAX_EVENTS || bpm < 60 || bpm > 240)
    {
        return JS_FALSE;
    }
    events = (pocketjs_game_event_t *)rt_malloc(count * sizeof(events[0]));
    if (events == RT_NULL)
    {
        return JS_FALSE;
    }
    for (i = 0U; i < count; ++i)
    {
        int32_t field[5];
        uint32_t k;

        for (k = 0U; k < 5U; ++k)
        {
            JSValue item = JS_GetPropertyUint32(context, argv[1], i * 5U + k);
            field[k] = dashboard_int_arg(context, item, 0);
            JS_FreeValue(context, item);
        }
        events[i].step = (uint16_t)(field[0] < 0 ? 0 : (field[0] > 65535 ? 65535 : field[0]));
        events[i].voice = (uint8_t)(field[1] & 0xFF);
        events[i].note = (uint8_t)(field[2] & 0xFF);
        events[i].length = (uint8_t)(field[3] < 1 ? 1 : (field[3] > 255 ? 255 : field[3]));
        events[i].volume = (uint8_t)(field[4] < 0 ? 0 : (field[4] > 127 ? 127 : field[4]));
    }
    started = pocketjs_game_begin((uint32_t)bpm, events, count);
    rt_free(events);
    return JS_NewBool(context, started);
}

static JSValue dashboard_game_stop(JSContext *context, JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void)context;
    (void)this_value;
    (void)argc;
    (void)argv;
    pocketjs_game_end();
    return JS_UNDEFINED;
}

static JSValue dashboard_game_clock(JSContext *context, JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_NewInt32(context, pocketjs_game_clock_ms());
}

static JSValue dashboard_game_sfx(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc >= 1)
    {
        pocketjs_game_sfx(dashboard_int_arg(context, argv[0], -1));
    }
    return JS_UNDEFINED;
}

static JSValue dashboard_game_scores(JSContext *context, JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    uint32_t best[POCKETJS_GAME_SLOTS];
    char rank[POCKETJS_GAME_SLOTS + 1U];
    JSValue object = JS_NewObject(context);
    JSValue list = JS_NewArray(context);
    uint32_t i;

    (void)this_value;
    (void)argc;
    (void)argv;
    pocketjs_game_scores(best, rank);
    for (i = 0U; i < POCKETJS_GAME_SLOTS; ++i)
    {
        JS_SetPropertyUint32(context, list, i, JS_NewUint32(context, best[i]));
    }
    JS_SetPropertyStr(context, object, "best", list);
    JS_SetPropertyStr(context, object, "rank", JS_NewString(context, rank));
    return object;
}

static JSValue dashboard_game_save(JSContext *context, JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc < 3)
    {
        return JS_FALSE;
    }
    return JS_NewBool(context, pocketjs_game_save_score(
                                   dashboard_int_arg(context, argv[0], -1),
                                   (uint32_t)dashboard_int_arg(context, argv[1], 0),
                                   dashboard_int_arg(context, argv[2], 0)));
}

static JSValue dashboard_game_offset(JSContext *context, JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_NewInt32(context, pocketjs_game_offset());
}

static JSValue dashboard_game_set_offset(JSContext *context, JSValueConst this_value,
                                         int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc >= 1)
    {
        pocketjs_game_set_offset(dashboard_int_arg(context, argv[0], 0));
    }
    return JS_UNDEFINED;
}

static JSValue dashboard_custom_song(JSContext *context, JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    size_t length = 0U;
    char *text;
    JSValue result;

    (void)this_value;
    (void)argc;
    (void)argv;
    text = pocketjs_game_custom_read(&length);
    if (text == RT_NULL)
    {
        return JS_NewString(context, "");
    }
    result = JS_NewStringLen(context, text, length);
    rt_free(text);
    return result;
}

static void dashboard_add_function(JSContext *context, JSValue object, const char *name,
                                   JSCFunction *function, int length)
{
    JS_SetPropertyStr(context, object, name, JS_NewCFunction(context, function, name, length));
}

esp_err_t pocketjs_dashboard_install(JSContext *context, void *user_data)
{
    JSValue global;
    JSValue dashboard;

    (void)user_data;
    global = JS_GetGlobalObject(context);
    dashboard = JS_NewObject(context);
    if (JS_IsException(global) || JS_IsException(dashboard))
    {
        JS_FreeValue(context, global);
        JS_FreeValue(context, dashboard);
        return ESP_ERR_NO_MEM;
    }
    JS_SetPropertyStr(context, dashboard, "status",
                      JS_NewCFunction(context, dashboard_status, "status", 0));
    dashboard_add_function(context, dashboard, "pc", dashboard_pc, 0);
    JS_SetPropertyStr(context, dashboard, "musicStatus",
                      JS_NewCFunction(context, dashboard_music_status, "musicStatus", 0));
    JS_SetPropertyStr(context, dashboard, "musicCommand",
                      JS_NewCFunction(context, dashboard_music_command, "musicCommand", 1));
    dashboard_add_function(context, dashboard, "gameStart", dashboard_game_start, 2);
    dashboard_add_function(context, dashboard, "gameStop", dashboard_game_stop, 0);
    dashboard_add_function(context, dashboard, "gameClock", dashboard_game_clock, 0);
    dashboard_add_function(context, dashboard, "gameSfx", dashboard_game_sfx, 1);
    dashboard_add_function(context, dashboard, "gameScores", dashboard_game_scores, 0);
    dashboard_add_function(context, dashboard, "gameSave", dashboard_game_save, 3);
    dashboard_add_function(context, dashboard, "gameOffset", dashboard_game_offset, 0);
    dashboard_add_function(context, dashboard, "gameSetOffset", dashboard_game_set_offset, 1);
    dashboard_add_function(context, dashboard, "customSong", dashboard_custom_song, 0);
    JS_SetPropertyStr(context, global, "__edgi", dashboard);
    JS_FreeValue(context, global);
    return JS_HasException(context) ? ESP_FAIL : ESP_OK;
}

void pocketjs_dashboard_start(void)
{
    if (s_started)
    {
        return;
    }
    s_started = RT_TRUE;
    s_fps_started_at = rt_tick_get();
    s_idle_thread = rt_thread_find("tidle0");
    if (s_idle_thread == RT_NULL)
    {
        s_idle_thread = rt_thread_find("tidle");
    }
    (void)rt_mutex_init(&s_network_lock, "pjsnet", RT_IPC_FLAG_PRIO);

    rt_timer_init(&s_cpu_timer, "pjscpu", dashboard_cpu_timer, RT_NULL, 1U,
                  RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_HARD_TIMER);
    rt_timer_start(&s_cpu_timer);

    s_weather_thread = rt_thread_create("pjswx", dashboard_weather_task,
                                        RT_NULL, DASHBOARD_WEATHER_STACK_SIZE,
                                        DASHBOARD_WEATHER_PRIORITY, 10U);
    if (s_weather_thread != RT_NULL)
    {
        rt_thread_startup(s_weather_thread);
    }
    s_sensor_thread = rt_thread_create("pjstmp", dashboard_sensor_task,
                                       RT_NULL, DASHBOARD_SENSOR_STACK_SIZE,
                                       DASHBOARD_WEATHER_PRIORITY, 10U);
    if (s_sensor_thread != RT_NULL)
    {
        rt_thread_startup(s_sensor_thread);
    }
}
