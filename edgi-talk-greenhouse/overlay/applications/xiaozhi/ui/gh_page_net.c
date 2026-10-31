/*
 * gh_page_net.c - 网络页：连接状态 + 配网信息（左右双面板）
 *
 * 布局（GH_BODY 636x350，坐标相对页根容器）：
 *   左面板 309x350「网络状态」：WiFi 连接 / 已存热点 / IP 地址 / 小智服务器 / 设备状态
 *   右面板 309x350「配网」    ：配网热点 AP 信息（换行）+ 底部操作提示
 *   每面板左缘 3px 色条（左 accent，右 accent2），内嵌 1px 分隔线。
 *
 * 数据源：sensor_monitor 快照（IP / 网络就绪标志）+ wifi_manager_is_connected()
 *         + g_app（extern，定义在 xiaozhi.cpp）。
 *         已存热点读 /flash/wifi_config.json（wifi_manager 保存的配置）。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 * 2026-11-01     Greenhouse     重写为左右双面板暗色设计，统一 GH_COL_* 令牌
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include <cJSON.h>
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "../sensors/sensor_monitor.h"
#include "../webnet/wifi_manager.h"
#include "../xiaozhi.h"
/* 注：不 include netdev.h —— 与 xiaozhi.h 拉入的 lwip 头冲突；
 * IP 等网络信息直接读 sensor_monitor 快照（其内部已填充） */

#define PNL_W       309                 /* 单面板宽：309*2+18=636 */
#define PNL_H       GH_BODY_H           /* 350 */
#define PNL_GAP     18                  /* 两面板间距 */
#define PNL_R       14                  /* 圆角 */
#define PAD_X       22                  /* 面板内容左边距(避让左色条) */
#define VAL_W       (PNL_W - PAD_X - 14)/* 值文本区宽 273 */
#define ROW_Y0      52                  /* 首行 y */
#define ROW_DY      56                  /* 行距：键(18) + 值(22) 上下排 */

/* 小智应用全局状态（定义在 xiaozhi.cpp, xiaozhi.h 无 extern 声明） */
extern xiaozhi_app_t g_app;

/* 每行: 键(灰,18) 在上 + 值(亮,22) 在下 */
typedef struct
{
    lv_obj_t *key;
    lv_obj_t *val;
} net_row_t;

static net_row_t r_wifi, r_ssid, r_ip, r_server, r_state;
static lv_obj_t *s_ap_key, *s_ap_val, *s_hint;

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

/* 面板：圆角暗底 + 左缘 3px 色条 */
static lv_obj_t *panel_create(lv_obj_t *parent, int x, uint32_t bar_hex)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(p, PNL_W, PNL_H);
    lv_obj_set_pos(p, x, 0);
    lv_obj_set_style_radius(p, PNL_R, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(p, lv_color_hex(GH_COL_PANEL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *bar = lv_obj_create(p);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 3, PNL_H - 16);
    lv_obj_set_pos(bar, 8, 8);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(bar_hex), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    return p;
}

/* 面板标题(18pt dim) + 下方 1px 分隔线 */
static void title_create(lv_obj_t *panel, const char *text)
{
    lv_obj_t *t = lv_label_create(panel);
    lv_obj_add_style(t, gh_ui_style(18), 0);
    lv_label_set_text(t, text);
    lv_obj_set_style_text_color(t, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(t, PAD_X, 12);

    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(line, VAL_W, 1);
    lv_obj_set_pos(line, PAD_X, 40);
    lv_obj_set_style_bg_color(line, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* 状态行：键 18pt dim 在上，值 22pt 在下，防溢出打点 */
static void row_create(net_row_t *r, lv_obj_t *panel, int idx, const char *key)
{
    int y = ROW_Y0 + idx * ROW_DY;

    r->key = lv_label_create(panel);
    lv_obj_add_style(r->key, gh_ui_style(18), 0);
    lv_label_set_text(r->key, key);
    lv_obj_set_style_text_color(r->key, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r->key, PAD_X, y);

    r->val = lv_label_create(panel);
    lv_obj_add_style(r->val, gh_ui_style(22), 0);
    lv_label_set_long_mode(r->val, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r->val, VAL_W);
    lv_label_set_text(r->val, "--");
    lv_obj_set_style_text_color(r->val, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(r->val, PAD_X, y + 24);
}

static void net_create(lv_obj_t *parent)
{
    /* 左：网络状态 */
    lv_obj_t *lp = panel_create(parent, 0, GH_COL_ACCENT);
    title_create(lp, "网络状态");
    row_create(&r_wifi,   lp, 0, "WiFi 连接");
    row_create(&r_ssid,   lp, 1, "已存热点");
    row_create(&r_ip,     lp, 2, "IP 地址");
    row_create(&r_server, lp, 3, "小智服务器");
    row_create(&r_state,  lp, 4, "设备状态");

    /* 右：配网 */
    lv_obj_t *rp = panel_create(parent, PNL_W + PNL_GAP, GH_COL_ACCENT2);
    title_create(rp, "配网");

    s_ap_key = lv_label_create(rp);
    lv_obj_add_style(s_ap_key, gh_ui_style(18), 0);
    lv_label_set_text(s_ap_key, "配网热点");
    lv_obj_set_style_text_color(s_ap_key, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_ap_key, PAD_X, ROW_Y0);

    /* AP 信息可能较长（SSID/密码/IP 一串），允许换行 */
    s_ap_val = lv_label_create(rp);
    lv_obj_add_style(s_ap_val, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_ap_val, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ap_val, VAL_W);
    lv_label_set_text(s_ap_val, "--");
    lv_obj_set_style_text_color(s_ap_val, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_ap_val, PAD_X, ROW_Y0 + 26);

    /* 操作提示固定贴面板底部，两行以内 */
    s_hint = lv_label_create(rp);
    lv_obj_add_style(s_hint, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_hint, VAL_W);
    lv_label_set_text(s_hint, "配网：手机连接上方热点，浏览器打开 192.168.169.1");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_LEFT, PAD_X, -14);
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
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else if (sta_up)
    {
        lv_label_set_text(r_wifi.val, "已关联(获取IP中)");
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(GH_COL_WARN), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(r_wifi.val, "未连接");
        lv_obj_set_style_text_color(r_wifi.val, lv_color_hex(GH_COL_ALERT), LV_PART_MAIN | LV_STATE_DEFAULT);
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
        lv_label_set_text(r_server.val, "已连接 api.tenclass.net");
        lv_obj_set_style_text_color(r_server.val, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(r_server.val, "未连接");
        lv_obj_set_style_text_color(r_server.val, lv_color_hex(GH_COL_ALERT), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    lv_label_set_text(r_state.val, device_state_text());
    lv_label_set_text(s_ap_val, gh_ui_get_ap_info());
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
