/*
 * gh_page_net.c - 网络页：WiFi / 小智服务器 / 配网信息
 *
 * 展示内容（每秒刷新）：
 *   - WiFi STA 连接状态 + 已保存的热点名（/flash/wifi_config.json）
 *   - IP 地址（netdev 默认网卡）
 *   - 小智服务器连接状态（g_app.ws.is_connected）+ 设备状态机（g_app.state）
 *   - AP 配网热点信息（配网模式时引擎收到 webnet_wifi 上报）
 *
 * 数据源引用 xiaozhi.cpp 的 g_app（xiaozhi_app_t, 本文件 extern）。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include <cJSON.h>
#include "gh_ui_pages.h"
#include "../sensors/sensor_monitor.h"
#include "../webnet/wifi_manager.h"
#include "../xiaozhi.h"
/* 注：不 include netdev.h —— 与 xiaozhi.h 拉入的 lwip 头冲突；
 * IP 等网络信息直接读 sensor_monitor 快照（其内部已填充） */

#define C_TEXT      0xFFFFFF
#define C_TEXT_DIM  0x8A93A6
#define C_ACCENT    0x35C4A0
#define C_ALERT     0xFF5050

#define ROW_Y0      6
#define ROW_DY      44
#define NET_VAL_W   440     /* 值文本区宽度 */

/* 小智应用全局状态（定义在 xiaozhi.cpp, xiaozhi.h 无 extern 声明） */
extern xiaozhi_app_t g_app;

/* 每行: 键(灰) + 值(亮) */
typedef struct
{
    lv_obj_t *key;
    lv_obj_t *val;
} net_row_t;

static net_row_t r_wifi, r_ssid, r_ip, r_server, r_state, r_ap;
static lv_obj_t *s_hint;

/* 设备状态机 → 中文 */
static const char *device_state_text(void)
{
    switch (g_app.state)
    {
    case kDeviceStateIdle:           return "待机";
    case kDeviceStateConnecting:     return "连接服务器中";
    case kDeviceStateListening:      return "聆听中";
    case kDeviceStateSpeaking:       return "说话中";
    case kDeviceStateUpgrading:      return "升级中";
    case kDeviceStateActivating:     return "等待激活";
    case kDeviceStateWifiConfiguring:return "配网模式";
    case kDeviceStateFatalError:     return "故障";
    case kDeviceStateStarting:       return "启动中";
    default:                         return "未知";
    }
}

/* 读 /flash/wifi_config.json 里的 ssid（wifi_manager 保存的配置） */
static void load_saved_ssid(char *buf, int len)
{
    FILE *fp;
    long size;
    char *json;
    cJSON *root, *item;

    buf[0] = '\0';
    fp = fopen(WIFI_CONFIG_FILE, "rb");
    if (fp == RT_NULL)
        return;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0 || size > 4096)
    {
        fclose(fp);
        return;
    }
    json = (char *)rt_malloc(size + 1);
    if (json == RT_NULL)
    {
        fclose(fp);
        return;
    }
    if (fread(json, 1, size, fp) != (rt_size_t)size)
    {
        rt_free(json);
        fclose(fp);
        return;
    }
    json[size] = '\0';
    fclose(fp);

    root = cJSON_Parse(json);
    rt_free(json);
    if (root == RT_NULL)
        return;
    item = cJSON_GetObjectItem(root, "ssid");
    if (item && item->valuestring)
        rt_strncpy(buf, item->valuestring, len - 1);
    buf[len - 1] = '\0';
    cJSON_Delete(root);
}

static void row_create(net_row_t *r, lv_obj_t *parent, int idx, const char *key)
{
    r->key = lv_label_create(parent);
    lv_obj_add_style(r->key, gh_ui_style(18), 0);
    lv_label_set_text(r->key, key);
    lv_obj_set_style_text_color(r->key, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r->key, 4, ROW_Y0 + idx * ROW_DY);

    r->val = lv_label_create(parent);
    lv_obj_add_style(r->val, gh_ui_style(22), 0);
    lv_label_set_text(r->val, "--");
    lv_obj_set_style_text_color(r->val, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r->val, 120, ROW_Y0 + idx * ROW_DY - 2);
}

static void net_create(lv_obj_t *parent)
{
    row_create(&r_wifi,   parent, 0, "WiFi 连接");
    row_create(&r_ssid,   parent, 1, "已存热点");
    row_create(&r_ip,     parent, 2, "IP 地址");
    row_create(&r_server, parent, 3, "小智服务器");
    row_create(&r_state,  parent, 4, "设备状态");

    /* AP 配网信息（占两行高度） */
    r_ap.key = lv_label_create(parent);
    lv_obj_add_style(r_ap.key, gh_ui_style(18), 0);
    lv_label_set_text(r_ap.key, "配网热点");
    lv_obj_set_style_text_color(r_ap.key, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r_ap.key, 4, ROW_Y0 + 5 * ROW_DY);

    r_ap.val = lv_label_create(parent);
    lv_obj_add_style(r_ap.val, gh_ui_style(18), 0);
    lv_label_set_long_mode(r_ap.val, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(r_ap.val, NET_VAL_W);
    lv_label_set_text(r_ap.val, "--");
    lv_obj_set_style_text_color(r_ap.val, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r_ap.val, 120, ROW_Y0 + 5 * ROW_DY - 2);

    s_hint = lv_label_create(parent);
    lv_obj_add_style(s_hint, gh_ui_style(18), 0);
    lv_label_set_text(s_hint, "配网：手机连接上方热点, 浏览器打开 192.168.169.1");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, 4, 0);
}

static void net_update(void)
{
    char ssid[64];
    edgi_sensor_snapshot_t snap;
    rt_bool_t have_snap = (edgi_sensor_monitor_get_snapshot(&snap) == RT_EOK);
    rt_bool_t sta_up = wifi_manager_is_connected();

    /* WiFi 状态（快照里的网络就绪标志 = 拿到 IP 且网卡 up） */
    if (have_snap && snap.wifi_network_ready)
    {
        lv_label_set_text(r_wifi.val, "已连接");
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if (sta_up)
    {
        lv_label_set_text(r_wifi.val, "已关联(获取IP中)");
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(0xE0B040), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(r_wifi.val, "未连接");
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(C_ALERT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    load_saved_ssid(ssid, sizeof(ssid));
    lv_label_set_text(r_ssid.val, ssid[0] ? ssid : "无(未配置)");

    /* IP：来自快照（sensor 线程从 netdev 读取） */
    if (have_snap && snap.wifi_ip[0] != '\0')
        lv_label_set_text(r_ip.val, snap.wifi_ip);
    else
        lv_label_set_text(r_ip.val, "无");

    /* 小智 WebSocket 服务器 */
    if (g_app.ws.is_connected)
    {
        lv_label_set_text(r_server.val, "已连接 (api.tenclass.net)");
        lv_obj_set_style_text_color(r_server.val, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(r_server.val, "未连接");
        lv_obj_set_style_text_color(r_server.val, lv_color_hex(C_ALERT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_label_set_text(r_state.val, device_state_text());
    lv_label_set_text(r_ap.val, gh_ui_get_ap_info());
}

static const gh_page_t s_page = {
    .tab     = "网络",
    .create  = net_create,
    .update  = net_update,
    .on_show = RT_NULL,
    .on_hide = RT_NULL,
};

const gh_page_t *gh_page_net(void)
{
    return &s_page;
}
