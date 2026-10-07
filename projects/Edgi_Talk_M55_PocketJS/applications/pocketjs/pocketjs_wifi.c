#include "pocketjs_wifi.h"
#include "pocketjs_game.h"
#include "pocketjs_dashboard.h"

#include <rthw.h>
#include <drivers/wlan.h>
#include <netdev_ipaddr.h>
#include <netdev.h>
#include <cy_syslib.h>
#include <socket/sys_socket/sys/socket.h>
#include <socket/netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define WIFI_CONFIG_PATH "/flash/pocketjs_wifi.bin"
#define WIFI_CONFIG_TEMP "/flash/pocketjs_wifi.tmp"
#define WIFI_CONFIG_MAGIC 0x504A5749U
#define WIFI_AP_IP "192.168.169.1"
#define WIFI_MANAGER_STACK 3072U
#define WIFI_HTTP_STACK 6144U
#define WIFI_THREAD_PRIORITY 25U
#define WIFI_HTTP_PORT 80U
#define WIFI_CLIENT_LIMIT 4U
#define WIFI_CLIENT_STACK 8192U
#define WIFI_DISCOVERY_PORT 47808U
#define WIFI_DISCOVERY_STACK 2048U
#define WIFI_UPLOAD_CHUNK 768U
#define WIFI_UPLOAD_TIME_MS 30000U
#define WIFI_PC_BODY_MAX 512U
#define WIFI_AUTH_MAX_FAILURES 5U
#define WIFI_AUTH_LOCKOUT_MS 30000U
#define WIFI_SCAN_LIMIT 24U
#define WIFI_BODY_LIMIT 384U
#define WIFI_PAGE_CAPACITY 12288U
#define WIFI_HEADER_CAPACITY 200U

typedef struct
{
    rt_uint32_t magic;
    char ssid[33];
    char password[65];
    rt_uint32_t checksum;
} wifi_config_t;

static wifi_config_t s_config;
static wifi_config_t s_pending;
static struct rt_mutex s_lock;
static rt_bool_t s_started;
static rt_bool_t s_pending_ready;
static volatile rt_bool_t s_ap_active;
static volatile rt_uint8_t s_connect_state;
static rt_uint8_t s_client_count;
static volatile rt_bool_t s_scan_requested;
static volatile rt_bool_t s_scan_busy;
static char s_ap_ssid[33];
static char s_ap_password[17];
static rt_uint8_t s_auth_failures;
static rt_tick_t s_auth_locked_until;

typedef struct
{
    char ssid[33];
    rt_int16_t rssi;
} wifi_scan_entry_t;

static wifi_scan_entry_t s_scan[WIFI_SCAN_LIMIT];
static volatile rt_uint8_t s_scan_count;

static void wifi_scan_report(int event, struct rt_wlan_buff *buffer, void *parameter);

static void wifi_refresh_scan(void)
{
    s_scan_busy = RT_TRUE;
    s_scan_count = 0U;
    if (rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION) == RT_EOK)
    {
        (void)rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT,
                                             wifi_scan_report, RT_NULL);
        (void)rt_wlan_scan_with_info(RT_NULL);
        (void)rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_REPORT);
    }
    s_scan_busy = RT_FALSE;
    rt_kprintf("[pjswifi] scan found %u networks\n", s_scan_count);
}

static rt_uint32_t wifi_checksum(const wifi_config_t *config)
{
    const rt_uint8_t *bytes = (const rt_uint8_t *)config;
    rt_uint32_t hash = 2166136261U;
    rt_size_t i;

    for (i = 0; i < offsetof(wifi_config_t, checksum); ++i)
    {
        hash = (hash ^ bytes[i]) * 16777619U;
    }
    return hash;
}

static rt_bool_t wifi_config_load(wifi_config_t *config)
{
    int fd = open(WIFI_CONFIG_PATH, O_RDONLY);
    int count;

    if (fd < 0)
    {
        return RT_FALSE;
    }
    count = read(fd, config, sizeof(*config));
    close(fd);
    return count == sizeof(*config) && config->magic == WIFI_CONFIG_MAGIC &&
           config->ssid[0] != '\0' && config->ssid[32] == '\0' &&
           config->password[64] == '\0' &&
           config->checksum == wifi_checksum(config);
}

static rt_bool_t wifi_config_save(wifi_config_t *config)
{
    int fd;
    int count;

    config->magic = WIFI_CONFIG_MAGIC;
    config->checksum = wifi_checksum(config);
    fd = open(WIFI_CONFIG_TEMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        return RT_FALSE;
    }
    count = write(fd, config, sizeof(*config));
    close(fd);
    if (count != sizeof(*config))
    {
        unlink(WIFI_CONFIG_TEMP);
        return RT_FALSE;
    }
    if (rename(WIFI_CONFIG_TEMP, WIFI_CONFIG_PATH) != 0)
    {
        unlink(WIFI_CONFIG_TEMP);
        return RT_FALSE;
    }
    return RT_TRUE;
}

static rt_bool_t wifi_connect(const wifi_config_t *config)
{
    int wait;

    if (rt_wlan_set_mode(RT_WLAN_DEVICE_STA_NAME, RT_WLAN_STATION) != RT_EOK)
    {
        return RT_FALSE;
    }
    if (rt_wlan_connect(config->ssid, config->password[0] ? config->password : RT_NULL) != RT_EOK)
    {
        return RT_FALSE;
    }
    for (wait = 0; wait < 30; ++wait)
    {
        if (rt_wlan_is_ready())
        {
            return RT_TRUE;
        }
        rt_thread_mdelay(500);
    }
    (void)rt_wlan_disconnect();
    return RT_FALSE;
}

static void wifi_start_ap(void)
{
    int attempt;

    if (rt_wlan_ap_is_active())
    {
        s_ap_active = RT_TRUE;
        return;
    }
    s_ap_active = RT_FALSE;
    wifi_refresh_scan();
    for (attempt = 0; attempt < 20; ++attempt)
    {
        if (rt_wlan_set_mode(RT_WLAN_DEVICE_AP_NAME, RT_WLAN_AP) == RT_EOK)
        {
            break;
        }
        rt_thread_mdelay(500);
    }
    if (attempt == 20 || rt_wlan_start_ap(s_ap_ssid, s_ap_password) != RT_EOK)
    {
        rt_kprintf("[pjswifi] AP start failed\n");
        return;
    }
    s_ap_active = RT_TRUE;
    rt_kprintf("[pjswifi] AP %s at http://%s\n", s_ap_ssid, WIFI_AP_IP);
}

static void wifi_manager_task(void *parameter)
{
    wifi_config_t candidate;
    int attempt;
    int offline_ticks = 0;

    (void)parameter;
    for (attempt = 0; attempt < 40; ++attempt)
    {
        struct stat filesystem;
        if (stat("/flash", &filesystem) == 0)
        {
            break;
        }
        rt_thread_mdelay(500);
    }
    rt_wlan_config_autoreconnect(RT_FALSE);
    if (wifi_config_load(&s_config))
    {
        for (attempt = 0; attempt < 2 && !rt_wlan_is_ready(); ++attempt)
        {
            (void)wifi_connect(&s_config);
        }
    }
    if (!rt_wlan_is_ready())
    {
        wifi_start_ap();
    }

    while (1)
    {
        rt_bool_t have_pending = RT_FALSE;

        if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK)
        {
            if (s_pending_ready)
            {
                candidate = s_pending;
                s_pending_ready = RT_FALSE;
                have_pending = RT_TRUE;
            }
            rt_mutex_release(&s_lock);
        }
        if (have_pending)
        {
            s_connect_state = 1U;
            if (wifi_connect(&candidate))
            {
                s_connect_state = 2U;
                s_config = candidate;
                if (!wifi_config_save(&s_config))
                {
                    rt_kprintf("[pjswifi] connected, but /flash save failed\n");
                }
                rt_thread_mdelay(5000);
                if (s_ap_active)
                {
                    (void)rt_wlan_ap_stop();
                    s_ap_active = RT_FALSE;
                }
            }
            else
            {
                s_connect_state = 3U;
                rt_kprintf("[pjswifi] connection failed for %s\n", candidate.ssid);
                wifi_start_ap();
            }
        }
        else if (s_scan_requested)
        {
            s_scan_requested = RT_FALSE;
            wifi_refresh_scan();
        }
        else if (!rt_wlan_is_ready())
        {
            if (++offline_ticks >= 5)
            {
                offline_ticks = 0;
                if (s_config.ssid[0] && !wifi_connect(&s_config))
                {
                    wifi_start_ap();
                }
                else if (!s_config.ssid[0])
                {
                    wifi_start_ap();
                }
            }
        }
        else
        {
            offline_ticks = 0;
        }
        rt_thread_mdelay(1000);
    }
}

static void wifi_send_bytes(int fd, const char *bytes, rt_size_t length)
{
    while (length != 0U)
    {
        int sent = send(fd, bytes, length, 0);
        if (sent <= 0) return;
        bytes += sent;
        length -= (rt_size_t)sent;
    }
}

static rt_size_t wifi_format_header(char *header, rt_size_t capacity,
                                    rt_size_t length)
{
    rt_snprintf(header, capacity,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %u\r\nCache-Control: no-store\r\n"
                "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                (unsigned int)length);
    return rt_strlen(header);
}

static void wifi_send(int fd, const char *body)
{
    char response[1792];
    rt_size_t body_length = rt_strlen(body);
    rt_size_t header_length = wifi_format_header(response, sizeof(response), body_length);

    if (header_length + body_length > sizeof(response)) return;
    rt_memcpy(response + header_length, body, body_length);
    wifi_send_bytes(fd, response, header_length + body_length);
}

static rt_bool_t wifi_append(char *buffer, rt_size_t capacity,
                             rt_size_t *used, const char *text)
{
    rt_size_t length = rt_strlen(text);

    if (length > capacity - *used) return RT_FALSE;
    rt_memcpy(buffer + *used, text, length);
    *used += length;
    return RT_TRUE;
}

static void wifi_html_escape(char *out, rt_size_t capacity, const char *input,
                             rt_size_t length)
{
    rt_size_t used = 0;
    rt_size_t i;

    for (i = 0; i < length && used + 6 < capacity; ++i)
    {
        const char *replacement = RT_NULL;
        if (input[i] == '&') replacement = "&amp;";
        if (input[i] == '<') replacement = "&lt;";
        if (input[i] == '>') replacement = "&gt;";
        if (input[i] == '"') replacement = "&quot;";
        if (input[i] == '\'') replacement = "&#39;";
        if (replacement)
        {
            rt_size_t size = rt_strlen(replacement);
            rt_memcpy(out + used, replacement, size);
            used += size;
        }
        else if ((rt_uint8_t)input[i] >= 32U)
        {
            out[used++] = input[i];
        }
    }
    out[used] = '\0';
}

static void wifi_scan_report(int event, struct rt_wlan_buff *buffer, void *parameter)
{
    const struct rt_wlan_info *info = (const struct rt_wlan_info *)buffer->data;
    rt_uint8_t index;

    (void)event;
    (void)parameter;
    if (info == RT_NULL || info->ssid.len == 0U || info->ssid.len > 32U)
    {
        return;
    }
    for (index = 0; index < s_scan_count; ++index)
    {
        if (rt_strlen(s_scan[index].ssid) == info->ssid.len &&
            rt_memcmp(s_scan[index].ssid, info->ssid.val, info->ssid.len) == 0)
        {
            if (info->rssi > s_scan[index].rssi) s_scan[index].rssi = info->rssi;
            return;
        }
    }
    if (s_scan_count < WIFI_SCAN_LIMIT)
    {
        index = s_scan_count++;
        rt_memcpy(s_scan[index].ssid, info->ssid.val, info->ssid.len);
        s_scan[index].ssid[info->ssid.len] = '\0';
        s_scan[index].rssi = info->rssi;
    }
}

static void wifi_send_page(int fd)
{
    static const char prefix[] =
        "<!doctype html><html lang='zh'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Edgi Talk Wi-Fi</title><style>"
        "*{box-sizing:border-box}body{margin:0;background:#e6eff3;color:#12304a;font:16px system-ui,sans-serif}"
        "main{max-width:440px;margin:0 auto;padding:28px 18px}.brand{display:flex;align-items:center;gap:10px}"
        ".dot{width:14px;height:14px;border-radius:50%;background:#ffc94a}h1{font-size:24px;margin:0}"
        "p{line-height:1.5;color:#4a6470}.card{background:#fff;border:1px solid #cbdde3;border-radius:16px;padding:6px 18px 20px;margin-top:18px}"
        "label{display:block;margin:16px 0 6px;font-weight:600;font-size:14px}"
        "input,select,button{width:100%;padding:13px;font:inherit;border-radius:12px}"
        "input,select{border:1px solid #b9cdd4;background:#f7fafb}"
        "button{border:0;background:#ffc94a;color:#12304a;margin-top:22px;font-weight:800}"
        "a{color:#1d7974}</style><main><div class='brand'><span class='dot'></span><h1>Edgi Talk</h1></div>"
        "<p>Beat Dash &middot; 选择附近的 Wi-Fi，输入密码后连接。</p><form class='card' method='post' action='/connect'>"
        "<label for='ssid'>Wi-Fi 网络</label><select id='ssid' name='ssid'>";
    static const char suffix[] =
        "</select><label for='manual'>或手动输入网络名称</label>"
        "<input id='manual' name='manual' maxlength='32' autocomplete='off'>"
        "<label for='password'>密码</label><input id='password' name='password' type='password' maxlength='64' autocomplete='off'>"
        "<button type='submit'>连接 Wi-Fi</button></form><p><a href='/scan'>重新扫描</a></p></main></html>";
    static const char scanning[] = "<option value=''>正在扫描，可先手动输入</option>";
    static const char empty[] = "<option value=''>未找到网络，请手动输入</option>";
    wifi_scan_entry_t entries[WIFI_SCAN_LIMIT];
    char *response;
    char *body;
    char header[WIFI_HEADER_CAPACITY];
    rt_uint8_t count;
    rt_bool_t scanning_now;
    rt_size_t used = 0U;
    rt_size_t header_length;
    rt_base_t level;
    rt_uint8_t i;

    level = rt_hw_interrupt_disable();
    scanning_now = s_scan_busy;
    count = scanning_now ? 0U : s_scan_count;
    rt_memcpy(entries, s_scan, count * sizeof(entries[0]));
    rt_hw_interrupt_enable(level);

    response = rt_malloc(WIFI_HEADER_CAPACITY + WIFI_PAGE_CAPACITY);
    if (response == RT_NULL)
    {
        wifi_send(fd, "<h2>页面暂时不可用，请稍后重试</h2>");
        return;
    }
    body = response + WIFI_HEADER_CAPACITY;
    if (!wifi_append(body, WIFI_PAGE_CAPACITY, &used, prefix)) goto page_too_large;
    if (scanning_now && !wifi_append(body, WIFI_PAGE_CAPACITY, &used, scanning)) goto page_too_large;
    if (!scanning_now && count == 0U &&
        !wifi_append(body, WIFI_PAGE_CAPACITY, &used, empty)) goto page_too_large;
    for (i = 0; i < count; ++i)
    {
        char escaped[200];
        char option[512];
        wifi_html_escape(escaped, sizeof(escaped), entries[i].ssid,
                         rt_strlen(entries[i].ssid));
        rt_snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>",
                    escaped, escaped, entries[i].rssi);
        if (!wifi_append(body, WIFI_PAGE_CAPACITY, &used, option)) goto page_too_large;
    }
    if (!wifi_append(body, WIFI_PAGE_CAPACITY, &used, suffix)) goto page_too_large;

    header_length = wifi_format_header(header, sizeof(header), used);
    rt_memmove(response + header_length, body, used);
    rt_memcpy(response, header, header_length);
    wifi_send_bytes(fd, response, header_length + used);
    rt_free(response);
    return;

page_too_large:
    rt_free(response);
    wifi_send(fd, "<h2>网络列表过长，请手动输入网络名称</h2>");
}

static int wifi_hex(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
}

static rt_bool_t wifi_form_field(const char *body, const char *key,
                                 char *value, rt_size_t capacity)
{
    const char *cursor = body;
    rt_size_t key_length = rt_strlen(key);

    while (*cursor)
    {
        const char *end = strchr(cursor, '&');
        rt_size_t length = end ? (rt_size_t)(end - cursor) : rt_strlen(cursor);
        if (length > key_length && rt_memcmp(cursor, key, key_length) == 0 &&
            cursor[key_length] == '=')
        {
            rt_size_t used = 0;
            rt_size_t i;
            for (i = key_length + 1; i < length; ++i)
            {
                char decoded = cursor[i];
                if (decoded == '+') decoded = ' ';
                else if (decoded == '%' && i + 2 < length)
                {
                    int high = wifi_hex(cursor[i + 1]);
                    int low = wifi_hex(cursor[i + 2]);
                    if (high < 0 || low < 0) return RT_FALSE;
                    decoded = (char)((high << 4) | low);
                    i += 2;
                }
                if (decoded == '\0' || used + 1 >= capacity) return RT_FALSE;
                value[used++] = decoded;
            }
            value[used] = '\0';
            return RT_TRUE;
        }
        if (!end) break;
        cursor = end + 1;
    }
    return RT_FALSE;
}

/* -- PC companion API ---------------------------------------------------------- */

static rt_bool_t wifi_lan_reachable(void)
{
    return s_ap_active || rt_wlan_is_ready();
}

static void wifi_station_ip(char *out, rt_size_t capacity)
{
    struct netdev *device = netdev_get_by_name("w0");
    rt_uint32_t address;

    out[0] = '\0';
    if (device == RT_NULL)
    {
        device = netdev_default;
    }
    if (device == RT_NULL || !netdev_is_link_up(device))
    {
        return;
    }
    address = device->ip_addr.addr;
    if (address == 0U)
    {
        return;
    }
    rt_snprintf(out, capacity, "%u.%u.%u.%u", (unsigned int)(address & 0xFFU),
                (unsigned int)((address >> 8) & 0xFFU),
                (unsigned int)((address >> 16) & 0xFFU),
                (unsigned int)((address >> 24) & 0xFFU));
}

static void wifi_send_json(int fd, const char *status, const char *json)
{
    char header[192];
    rt_size_t length = rt_strlen(json);

    rt_snprintf(header, sizeof(header),
                "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                status, (unsigned int)length);
    wifi_send_bytes(fd, header, rt_strlen(header));
    wifi_send_bytes(fd, json, length);
}

static char wifi_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive header lookup inside the request head; false when absent. */
static rt_bool_t wifi_header_value(const char *head, const char *name,
                                   char *value, rt_size_t capacity)
{
    rt_size_t name_length = rt_strlen(name);
    const char *line = rt_strstr(head, "\r\n");

    while (line != RT_NULL)
    {
        rt_size_t i;
        rt_bool_t match = RT_TRUE;

        line += 2;
        if (line[0] == '\r' || line[0] == '\0')
        {
            break;
        }
        for (i = 0U; i < name_length; ++i)
        {
            if (wifi_lower(line[i]) != wifi_lower(name[i]))
            {
                match = RT_FALSE;
                break;
            }
        }
        if (match && line[name_length] == ':')
        {
            const char *text = line + name_length + 1U;
            rt_size_t used = 0U;

            while (*text == ' ' || *text == '\t')
            {
                ++text;
            }
            while (*text != '\0' && *text != '\r' && *text != '\n' && used + 1U < capacity)
            {
                value[used++] = *text++;
            }
            value[used] = '\0';
            return RT_TRUE;
        }
        line = rt_strstr(line, "\r\n");
    }
    return RT_FALSE;
}

/* 0 = ok, 401 = wrong/missing token, 429 = locked out after repeated failures. */
static int wifi_authorize(const char *head)
{
    char supplied[16] = {0};
    char expected[8];
    rt_uint8_t difference = 0U;
    rt_size_t i;

    if (s_auth_failures >= WIFI_AUTH_MAX_FAILURES)
    {
        if ((rt_tick_get() - s_auth_locked_until) > 0x7FFFFFFFU)
        {
            return 429;
        }
        s_auth_failures = 0U;
    }
    if (!pocketjs_game_token(expected))
    {
        return 401;
    }
    (void)wifi_header_value(head, "X-Token", supplied, sizeof(supplied));
    for (i = 0U; i < 6U; ++i)
    {
        difference |= (rt_uint8_t)(supplied[i] ^ expected[i]);
    }
    if (difference == 0U && supplied[6] == '\0')
    {
        s_auth_failures = 0U;
        return 0;
    }
    if (++s_auth_failures >= WIFI_AUTH_MAX_FAILURES)
    {
        s_auth_locked_until = rt_tick_get() + rt_tick_from_millisecond(WIFI_AUTH_LOCKOUT_MS);
    }
    return 401;
}

static void wifi_api_reject(int fd, int code)
{
    if (code == 429)
    {
        wifi_send_json(fd, "429 Too Many Requests", "{\"ok\":false,\"error\":\"locked, retry later\"}");
    }
    else
    {
        wifi_send_json(fd, "401 Unauthorized", "{\"ok\":false,\"error\":\"bad token\"}");
    }
}

static void wifi_api_status(int fd)
{
    char address[16];
    char json[320];

    wifi_station_ip(address, sizeof(address));
    rt_snprintf(json, sizeof(json),
                "{\"device\":\"edgi-talk\",\"app\":\"beat-dash\",\"api\":1,\"name\":\"%s\","
                "\"ip\":\"%s\",\"ap\":%s,\"song\":{\"present\":%s,\"size\":%u,\"limit\":%u},"
                "\"playing\":%s}",
                s_ap_ssid, address, s_ap_active ? "true" : "false",
                pocketjs_game_custom_size() != 0U ? "true" : "false",
                (unsigned int)pocketjs_game_custom_size(),
                (unsigned int)POCKETJS_GAME_SONG_LIMIT,
                pocketjs_game_running() ? "true" : "false");
    wifi_send_json(fd, "200 OK", json);
}

static void wifi_api_scores(int fd)
{
    uint32_t best[POCKETJS_GAME_SLOTS];
    char rank[POCKETJS_GAME_SLOTS + 1U];
    char json[256];
    rt_size_t used;
    rt_size_t i;

    pocketjs_game_scores(best, rank);
    used = (rt_size_t)rt_snprintf(json, sizeof(json), "{\"best\":[");
    for (i = 0U; i < POCKETJS_GAME_SLOTS; ++i)
    {
        used += (rt_size_t)rt_snprintf(json + used, sizeof(json) - used, "%s%u",
                                       i ? "," : "", (unsigned int)best[i]);
    }
    rt_snprintf(json + used, sizeof(json) - used, "],\"rank\":\"%s\"}", rank);
    wifi_send_json(fd, "200 OK", json);
}

static void wifi_api_get_song(int fd)
{
    size_t length = 0U;
    char *text = pocketjs_game_custom_read(&length);
    char header[160];

    if (text == RT_NULL)
    {
        wifi_send_json(fd, "404 Not Found", "{\"ok\":false,\"error\":\"no custom song\"}");
        return;
    }
    rt_snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
                "Connection: close\r\n\r\n", (unsigned int)length);
    wifi_send_bytes(fd, header, rt_strlen(header));
    wifi_send_bytes(fd, text, length);
    rt_free(text);
}

/*
 * PUT /api/song: stream the JSON body straight into flash so a 96 KB chart never
 * needs a RAM copy. The song only replaces the old one after the whole body
 * arrived and looks like a JSON object.
 */
static void wifi_api_put_song(int fd, const char *head, const char *body_start, int body_have)
{
    char value[16] = {0};
    char chunk[WIFI_UPLOAD_CHUNK];
    size_t expected;
    size_t remaining;
    size_t stored = 0U;
    char first = '\0';
    char last = '\0';
    rt_tick_t started = rt_tick_get_millisecond();
    int file;
    int received = body_have;
    const char *source = body_start;

    if (!wifi_header_value(head, "Content-Length", value, sizeof(value)) || atoi(value) <= 0)
    {
        wifi_send_json(fd, "411 Length Required", "{\"ok\":false,\"error\":\"content-length required\"}");
        return;
    }
    expected = (size_t)atoi(value);
    if (expected < 8U || expected > POCKETJS_GAME_SONG_LIMIT)
    {
        wifi_send_json(fd, "413 Payload Too Large", "{\"ok\":false,\"error\":\"song size out of range\"}");
        return;
    }
    if (pocketjs_game_running())
    {
        wifi_send_json(fd, "409 Conflict", "{\"ok\":false,\"error\":\"game is running\"}");
        return;
    }
    file = pocketjs_game_custom_open();
    if (file < 0)
    {
        wifi_send_json(fd, "500 Internal Server Error", "{\"ok\":false,\"error\":\"storage unavailable\"}");
        return;
    }
    remaining = expected;
    while (remaining != 0U)
    {
        size_t take;
        size_t i;

        if (received <= 0)
        {
            take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
            received = recv(fd, chunk, take, 0);
            if (received <= 0 || (rt_tick_get_millisecond() - started) > WIFI_UPLOAD_TIME_MS)
            {
                pocketjs_game_custom_abort(file);
                wifi_send_json(fd, "408 Request Timeout", "{\"ok\":false,\"error\":\"upload interrupted\"}");
                return;
            }
            source = chunk;
        }
        take = (size_t)received < remaining ? (size_t)received : remaining;
        for (i = 0U; i < take; ++i)
        {
            char c = source[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            {
                if (first == '\0')
                {
                    first = c;
                }
                last = c;
            }
        }
        if (write(file, source, take) != (int)take)
        {
            pocketjs_game_custom_abort(file);
            wifi_send_json(fd, "507 Insufficient Storage", "{\"ok\":false,\"error\":\"flash write failed\"}");
            return;
        }
        stored += take;
        remaining -= take;
        received = 0;
    }
    if (first != '{' || last != '}' || !pocketjs_game_custom_commit(file, stored))
    {
        if (first != '{' || last != '}')
        {
            pocketjs_game_custom_abort(file);
        }
        wifi_send_json(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"not a song object\"}");
        return;
    }
    rt_snprintf(chunk, sizeof(chunk), "{\"ok\":true,\"size\":%u}", (unsigned int)stored);
    wifi_send_json(fd, "200 OK", chunk);
}

/* PUT /api/pc: a short JSON object of PC stats, small enough to sit in one buffer. */
static void wifi_api_put_pc(int fd, const char *head, const char *body_start, int body_have)
{
    char value[16] = {0};
    char json[WIFI_PC_BODY_MAX + 1U];
    size_t expected;
    size_t have;

    if (!wifi_header_value(head, "Content-Length", value, sizeof(value)) || atoi(value) <= 0)
    {
        wifi_send_json(fd, "411 Length Required", "{\"ok\":false,\"error\":\"content-length required\"}");
        return;
    }
    expected = (size_t)atoi(value);
    if (expected > WIFI_PC_BODY_MAX)
    {
        wifi_send_json(fd, "413 Payload Too Large", "{\"ok\":false,\"error\":\"stats too large\"}");
        return;
    }
    have = body_have > 0 ? (size_t)body_have : 0U;
    if (have > expected)
    {
        have = expected;
    }
    rt_memcpy(json, body_start, have);
    while (have < expected)
    {
        int received = recv(fd, json + have, expected - have, 0);

        if (received <= 0)
        {
            wifi_send_json(fd, "408 Request Timeout", "{\"ok\":false,\"error\":\"body interrupted\"}");
            return;
        }
        have += (size_t)received;
    }
    json[have] = '\0';
    if (!pocketjs_dashboard_pc_ingest(json))
    {
        wifi_send_json(fd, "400 Bad Request", "{\"ok\":false,\"error\":\"no cpu value\"}");
        return;
    }
    wifi_send_json(fd, "200 OK", "{\"ok\":true}");
}

static void wifi_api_get_pc(int fd)
{
    char json[320];

    (void)pocketjs_dashboard_pc_json(json, sizeof(json));
    wifi_send_json(fd, "200 OK", json);
}

static void wifi_serve_api(int fd, const char *request, const char *body, int body_have)
{
    int denied;

    if (rt_strncmp(request, "GET /api/status ", 16) == 0)
    {
        wifi_api_status(fd);
    }
    else if (rt_strncmp(request, "GET /api/scores ", 16) == 0)
    {
        wifi_api_scores(fd);
    }
    else if (rt_strncmp(request, "GET /api/song ", 14) == 0)
    {
        wifi_api_get_song(fd);
    }
    else if (rt_strncmp(request, "GET /api/pc ", 12) == 0)
    {
        wifi_api_get_pc(fd);
    }
    else if (rt_strncmp(request, "PUT /api/pc ", 12) == 0)
    {
        denied = wifi_authorize(request);
        if (denied != 0)
        {
            wifi_api_reject(fd, denied);
            return;
        }
        wifi_api_put_pc(fd, request, body, body_have);
    }
    else if (rt_strncmp(request, "PUT /api/song ", 14) == 0)
    {
        denied = wifi_authorize(request);
        if (denied != 0)
        {
            wifi_api_reject(fd, denied);
            return;
        }
        wifi_api_put_song(fd, request, body, body_have);
    }
    else if (rt_strncmp(request, "DELETE /api/song ", 17) == 0)
    {
        denied = wifi_authorize(request);
        if (denied != 0)
        {
            wifi_api_reject(fd, denied);
            return;
        }
        if (pocketjs_game_running())
        {
            wifi_send_json(fd, "409 Conflict", "{\"ok\":false,\"error\":\"game is running\"}");
            return;
        }
        (void)pocketjs_game_custom_clear();
        wifi_send_json(fd, "200 OK", "{\"ok\":true}");
    }
    else
    {
        wifi_send_json(fd, "404 Not Found", "{\"ok\":false,\"error\":\"unknown endpoint\"}");
    }
}

static void wifi_send_device_page(int fd)
{
    wifi_send(fd,
              "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>Edgi Talk</title><body style='margin:0;background:#e6eff3;color:#12304a;font:16px system-ui,sans-serif'>"
              "<main style='max-width:440px;margin:0 auto;padding:28px 18px'>"
              "<h1 style='margin:0 0 8px'>Edgi Talk &middot; Beat Dash</h1>"
              "<p style='color:#4a6470;line-height:1.5'>设备已联网。用电脑上的 <b>edgitalk</b> 伴侣工具即可推送自制谱面、查看成绩。</p>"
              "<p style='background:#fff;border:1px solid #cbdde3;border-radius:16px;padding:14px 18px;line-height:1.6'>"
              "<code>python edgitalk.py status</code><br><code>python edgitalk.py push chart.json</code></p>"
              "<p style='color:#4a6470'>配对码显示在设备的 Settings 页面。</p></main>");
}

static void wifi_serve_client(int fd)
{
    char request[1024];
    int total = 0;
    int received;
    char *body;
    int content_length = 0;

    while (total < (int)sizeof(request) - 1)
    {
        received = recv(fd, request + total, sizeof(request) - 1U - total, 0);
        if (received <= 0) return;
        total += received;
        request[total] = '\0';
        body = rt_strstr(request, "\r\n\r\n");
        if (body) break;
    }
    body = rt_strstr(request, "\r\n\r\n");
    if (!body)
    {
        wifi_send_json(fd, "431 Request Header Fields Too Large", "{\"ok\":false}");
        return;
    }
    body += 4;
    if (rt_strncmp(request, "GET /api/", 9) == 0 || rt_strncmp(request, "PUT /api/", 9) == 0 ||
        rt_strncmp(request, "DELETE /api/", 12) == 0)
    {
        wifi_serve_api(fd, request, body, total - (int)(body - request));
        return;
    }
    if (!s_ap_active)
    {
        /* Wi-Fi provisioning is only offered on the device's own hotspot. */
        wifi_send_device_page(fd);
        return;
    }
    if (rt_strncmp(request, "GET / ", 6) == 0 ||
        rt_strncmp(request, "GET /index.html ", 16) == 0)
    {
        wifi_send_page(fd);
        return;
    }
    if (rt_strncmp(request, "GET /scan ", 10) == 0)
    {
        s_scan_requested = RT_TRUE;
        wifi_send(fd, "<meta name='viewport' content='width=device-width'><meta http-equiv='refresh' content='5;url=/'><h2>正在扫描附近网络...</h2><p>热点可能短暂断开，请等待自动重新连接。</p>");
        return;
    }
    if (rt_strncmp(request, "GET /status ", 12) == 0)
    {
        wifi_send(fd, s_connect_state == 3U ?
                  "<meta name='viewport' content='width=device-width'><h2>连接失败</h2><p>请检查密码或选择其他网络。</p><a href='/'>重新配网</a>" :
                  rt_wlan_is_ready() ?
                  "<meta name='viewport' content='width=device-width'><h2>Wi-Fi 已连接</h2><p>可以返回设备继续使用。</p>" :
                  "<meta name='viewport' content='width=device-width'><meta http-equiv='refresh' content='3'><h2>正在连接...</h2><p>若连接失败，请返回配网页重试。</p><a href='/'>返回配网</a>");
        return;
    }
    if (rt_strncmp(request, "POST /connect ", 14) != 0)
    {
        wifi_send(fd, "<meta http-equiv='refresh' content='0;url=/'>");
        return;
    }
    {
        const char *header = rt_strstr(request, "Content-Length:");
        if (!header) header = rt_strstr(request, "content-length:");
        if (header) content_length = atoi(strchr(header, ':') + 1);
    }
    if (content_length <= 0 || content_length > WIFI_BODY_LIMIT ||
        (int)(body - request) + content_length >= (int)sizeof(request))
    {
        wifi_send(fd, "<h2>请求内容无效</h2><a href='/'>返回</a>");
        return;
    }
    while (total - (int)(body - request) < content_length)
    {
        received = recv(fd, request + total, sizeof(request) - 1U - total, 0);
        if (received <= 0) return;
        total += received;
    }
    body[content_length] = '\0';
    {
        wifi_config_t next = {0};
        char manual[33] = {0};
        (void)wifi_form_field(body, "ssid", next.ssid, sizeof(next.ssid));
        (void)wifi_form_field(body, "manual", manual, sizeof(manual));
        if (manual[0]) rt_strncpy(next.ssid, manual, sizeof(next.ssid));
        if (!wifi_form_field(body, "password", next.password, sizeof(next.password)) ||
            !next.ssid[0])
        {
            wifi_send(fd, "<h2>网络名称或密码无效</h2><a href='/'>返回</a>");
            return;
        }
        if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK)
        {
            s_pending = next;
            s_pending_ready = RT_TRUE;
            s_connect_state = 1U;
            rt_mutex_release(&s_lock);
        }
    }
    wifi_send(fd, "<meta name='viewport' content='width=device-width'><meta http-equiv='refresh' content='3;url=/status'><h2>正在连接 Wi-Fi...</h2><p>请稍候，连接成功后热点会自动关闭。</p>");
}

typedef struct
{
    int fd;
} wifi_client_arg_t;

static void wifi_client_task(void *parameter)
{
    wifi_client_arg_t *client = (wifi_client_arg_t *)parameter;
    struct timeval receive_timeout = {3, 0};
    struct timeval send_timeout = {10, 0};
    rt_tick_t started_at = rt_tick_get_millisecond();

    (void)setsockopt(client->fd, SOL_SOCKET, SO_RCVTIMEO,
                     &receive_timeout, sizeof(receive_timeout));
    (void)setsockopt(client->fd, SOL_SOCKET, SO_SNDTIMEO,
                     &send_timeout, sizeof(send_timeout));
    wifi_serve_client(client->fd);
    closesocket(client->fd);
    rt_kprintf("[pjswifi] HTTP request took %u ms\n",
               (unsigned int)(rt_tick_get_millisecond() - started_at));
    if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK)
    {
        --s_client_count;
        rt_mutex_release(&s_lock);
    }
    rt_free(client);
}

static void wifi_http_task(void *parameter)
{
    (void)parameter;
    while (1)
    {
        int server;
        struct timeval timeout = {2, 0};
        int reuse = 1;
        struct sockaddr_in address = {0};

        if (!wifi_lan_reachable())
        {
            rt_thread_mdelay(1000);
            continue;
        }
        server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server < 0)
        {
            rt_thread_mdelay(1000);
            continue;
        }
        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (setsockopt(server, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) != 0)
        {
            rt_kprintf("[pjswifi] server receive timeout setup failed\n");
        }
        address.sin_family = AF_INET;
        address.sin_port = htons(WIFI_HTTP_PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, (struct sockaddr *)&address, sizeof(address)) == 0 &&
            listen(server, 2) == 0)
        {
            rt_kprintf("[pjswifi] HTTP listening on port 80\n");
            while (wifi_lan_reachable())
            {
                int client = accept(server, RT_NULL, RT_NULL);
                if (client >= 0)
                {
                    wifi_client_arg_t *argument = RT_NULL;
                    rt_thread_t thread = RT_NULL;
                    rt_bool_t reserved = RT_FALSE;

                    if (rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK)
                    {
                        if (s_client_count < WIFI_CLIENT_LIMIT)
                        {
                            ++s_client_count;
                            reserved = RT_TRUE;
                        }
                        rt_mutex_release(&s_lock);
                    }
                    if (reserved)
                    {
                        argument = rt_malloc(sizeof(*argument));
                        if (argument != RT_NULL)
                        {
                            argument->fd = client;
                            thread = rt_thread_create("pjscli", wifi_client_task,
                                                      argument, WIFI_CLIENT_STACK,
                                                      WIFI_THREAD_PRIORITY + 1U, 10U);
                        }
                    }
                    if (thread == RT_NULL || rt_thread_startup(thread) != RT_EOK)
                    {
                        if (thread != RT_NULL) rt_thread_delete(thread);
                        if (argument != RT_NULL) rt_free(argument);
                        closesocket(client);
                        if (reserved && rt_mutex_take(&s_lock, RT_WAITING_FOREVER) == RT_EOK)
                        {
                            --s_client_count;
                            rt_mutex_release(&s_lock);
                        }
                    }
                }
            }
        }
        else
        {
            rt_kprintf("[pjswifi] HTTP bind failed\n");
            rt_thread_mdelay(2000);
        }
        closesocket(server);
    }
}

/* UDP discovery: a "EDGI?" datagram on port 47808 is answered with a JSON card. */
static void wifi_discovery_task(void *parameter)
{
    (void)parameter;
    while (1)
    {
        int socket_fd;
        struct sockaddr_in address = {0};
        struct timeval timeout = {2, 0};

        if (!wifi_lan_reachable())
        {
            rt_thread_mdelay(1500);
            continue;
        }
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0)
        {
            rt_thread_mdelay(2000);
            continue;
        }
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        address.sin_family = AF_INET;
        address.sin_port = htons(WIFI_DISCOVERY_PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) == 0)
        {
            while (wifi_lan_reachable())
            {
                char query[16];
                char reply[128];
                struct sockaddr_in from;
                socklen_t from_length = sizeof(from);
                int count = recvfrom(socket_fd, query, sizeof(query) - 1U, 0,
                                     (struct sockaddr *)&from, &from_length);

                if (count >= 5 && rt_memcmp(query, "EDGI?", 5) == 0)
                {
                    rt_snprintf(reply, sizeof(reply),
                                "{\"device\":\"edgi-talk\",\"name\":\"%s\",\"port\":%u,\"api\":1}",
                                s_ap_ssid, (unsigned int)WIFI_HTTP_PORT);
                    (void)sendto(socket_fd, reply, rt_strlen(reply), 0,
                                 (struct sockaddr *)&from, from_length);
                }
            }
        }
        closesocket(socket_fd);
    }
}

void pocketjs_wifi_status(pocketjs_wifi_status_t *status)
{
    if (status == RT_NULL) return;
    status->ap_active = s_ap_active && rt_wlan_ap_is_active();
    rt_strncpy(status->ap_ssid, s_ap_ssid, sizeof(status->ap_ssid));
    rt_strncpy(status->ap_password, s_ap_password, sizeof(status->ap_password));
    wifi_station_ip(status->sta_ip, sizeof(status->sta_ip));
}

void pocketjs_wifi_start(void)
{
    rt_thread_t manager;
    rt_thread_t http;
    rt_thread_t discovery;
    rt_uint32_t id;

    if (s_started) return;
    /* Controller patch from bt-fw-ifx-cyw55500a1 release-v2.2.0. The HCI download
     * path is not in this firmware yet; the image is linked so the blob is present. */
    {
        extern const char brcm_patch_version[];
        extern const uint8_t brcm_patchram_buf[];
        rt_kprintf("[pjswifi] BT controller firmware linked: %s buf=%p\n",
                   brcm_patch_version, brcm_patchram_buf);
    }
    id = (rt_uint32_t)Cy_SysLib_GetUniqueId();
    rt_snprintf(s_ap_ssid, sizeof(s_ap_ssid), "EdgiTalk-%06X", id & 0xFFFFFFU);
    rt_snprintf(s_ap_password, sizeof(s_ap_password), "Edgi%08X", id);
    if (rt_mutex_init(&s_lock, "pjswifi", RT_IPC_FLAG_PRIO) != RT_EOK) return;
    manager = rt_thread_create("pjswifi", wifi_manager_task, RT_NULL,
                               WIFI_MANAGER_STACK, WIFI_THREAD_PRIORITY, 10U);
    http = rt_thread_create("pjshttp", wifi_http_task, RT_NULL,
                            WIFI_HTTP_STACK, WIFI_THREAD_PRIORITY + 1U, 10U);
    discovery = rt_thread_create("pjsdisc", wifi_discovery_task, RT_NULL,
                                 WIFI_DISCOVERY_STACK, WIFI_THREAD_PRIORITY + 2U, 10U);
    if (manager == RT_NULL || http == RT_NULL)
    {
        if (manager) rt_thread_delete(manager);
        if (http) rt_thread_delete(http);
        if (discovery) rt_thread_delete(discovery);
        return;
    }
    s_started = RT_TRUE;
    rt_thread_startup(manager);
    rt_thread_startup(http);
    if (discovery) rt_thread_startup(discovery);
}
