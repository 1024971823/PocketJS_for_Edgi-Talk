/*
 * gh_page_main.c - 主页：实时农作物环境监测 + 小智提醒
 *
 * 数据全部来自板上真实模块，没有仿真通道：
 *   温度  AHT20 空气温度；没接到时用 LSM6DS3 管芯温度，并标明来源
 *   湿度  只有 AHT20 在线才有数，否则显示 --
 *   作物  作物库当前项 + 正在生效的报警阈值（语音改阈值后 1 秒内回读）
 *   提醒  报警原文，或「环境正常」+ 小智是否在线
 *   土壤 / 光照 / CO2  板上没装，固定「未接」，不填假数
 *
 * 下排停在气泡左侧，见 gh_ui_layout.h 的 GH_LOWER_W。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     设备状态六宫格
 * 2026-10-31     Greenhouse     改回作物环境监测，未接通道不再仿真
 */

#include <rtthread.h>
#include <string.h>
#include <lvgl.h>
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "../sensors/sensor_monitor.h"
#include "../xiaozhi.h"

/* 定义在 xiaozhi.cpp；xiaozhi.h 没有把 g_app 暴露成 extern */
extern xiaozhi_app_t g_app;

#define C_CARD        0x11161D
#define C_CARD_ALERT  0x2A1214
#define C_TEXT        0xF2F4F8
#define C_TEXT_DIM    0x7C8698
#define C_ACCENT      0x35C4A0
#define C_ALERT       0xFF5A52

#define CARD_GAP      10
#define CARD_W        ((GH_BODY_W - CARD_GAP) / 2)
#define CARD_H        108
#define CROP_Y        (CARD_H + 8)
#define CROP_H        62
#define LOWER_Y       186

typedef struct
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *sub;
    lv_obj_t *tag;
} gh_card_t;

static gh_card_t s_temp;
static gh_card_t s_humi;
static lv_obj_t *s_crop_name;
static lv_obj_t *s_crop_range;
static lv_obj_t *s_remind;
static lv_obj_t *s_remind_sub;

static void fmt_deci(char *buf, int len, int deci)
{
    int neg = deci < 0;
    int abs_v = neg ? -deci : deci;
    rt_snprintf(buf, len, "%s%d.%d", neg ? "-" : "", abs_v / 10, abs_v % 10);
}

static void card_paint(gh_card_t *c, rt_bool_t alert)
{
    lv_obj_set_style_bg_color(c->card, lv_color_hex(alert ? C_CARD_ALERT : C_CARD),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(c->title, lv_color_hex(alert ? C_ALERT : C_TEXT_DIM),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(c->value, lv_color_hex(alert ? C_ALERT : C_TEXT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void card_create(gh_card_t *c, lv_obj_t *parent, int col, const char *title)
{
    c->card = lv_obj_create(parent);
    lv_obj_remove_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c->card, CARD_W, CARD_H);
    lv_obj_set_pos(c->card, col * (CARD_W + CARD_GAP), 0);
    lv_obj_set_style_radius(c->card, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c->card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(c->card, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(c->card, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    c->title = lv_label_create(c->card);
    lv_obj_add_style(c->title, gh_ui_style(18), 0);
    lv_label_set_text(c->title, title);
    lv_obj_set_style_text_color(c->title, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->title, LV_ALIGN_TOP_LEFT, 0, 0);

    c->tag = lv_label_create(c->card);
    lv_obj_add_style(c->tag, gh_ui_style(18), 0);
    lv_label_set_text(c->tag, "");
    lv_obj_set_style_text_color(c->tag, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->tag, LV_ALIGN_TOP_RIGHT, 0, 0);

    c->value = lv_label_create(c->card);
    lv_obj_add_style(c->value, gh_ui_style(44), 0);
    lv_label_set_text(c->value, "--");
    lv_obj_set_style_text_color(c->value, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->value, LV_ALIGN_LEFT_MID, 0, 8);

    c->sub = lv_label_create(c->card);
    lv_obj_add_style(c->sub, gh_ui_style(18), 0);
    lv_label_set_text(c->sub, "");
    lv_obj_set_style_text_color(c->sub, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* 未接传感器：只说明缺失，不放数值 */
static void absent_chip(lv_obj_t *parent, int index, const char *name)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_t *title;
    lv_obj_t *sub;
    int w = (GH_LOWER_W - 16) / 3;

    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chip, w, 52);
    lv_obj_set_pos(chip, index * (w + 8), LOWER_Y + 50);
    lv_obj_set_style_radius(chip, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(chip, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(chip, 6, LV_PART_MAIN | LV_STATE_DEFAULT);

    title = lv_label_create(chip);
    lv_obj_add_style(title, gh_ui_style(18), 0);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    sub = lv_label_create(chip);
    lv_obj_add_style(sub, gh_ui_style(18), 0);
    lv_label_set_text(sub, "未接");
    lv_obj_set_style_text_color(sub, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void main_create(lv_obj_t *parent)
{
    lv_obj_t *crop;
    lv_obj_t *crop_title;

    card_create(&s_temp, parent, 0, "空气温度 ℃");
    card_create(&s_humi, parent, 1, "空气湿度 %");

    crop = lv_obj_create(parent);
    lv_obj_remove_flag(crop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(crop, GH_BODY_W, CROP_H);
    lv_obj_set_pos(crop, 0, CROP_Y);
    lv_obj_set_style_radius(crop, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(crop, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(crop, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(crop, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    crop_title = lv_label_create(crop);
    lv_obj_add_style(crop_title, gh_ui_style(18), 0);
    lv_label_set_text(crop_title, "当前作物");
    lv_obj_set_style_text_color(crop_title, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(crop_title, LV_ALIGN_LEFT_MID, 0, 0);

    s_crop_name = lv_label_create(crop);
    lv_obj_add_style(s_crop_name, gh_ui_style(28), 0);
    lv_label_set_text(s_crop_name, "通用");
    lv_obj_set_style_text_color(s_crop_name, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_crop_name, LV_ALIGN_LEFT_MID, 96, 0);

    s_crop_range = lv_label_create(crop);
    lv_obj_add_style(s_crop_range, gh_ui_style(18), 0);
    lv_label_set_text(s_crop_range, "");
    lv_obj_set_style_text_color(s_crop_range, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_crop_range, LV_ALIGN_RIGHT_MID, 0, 0);

    s_remind = lv_label_create(parent);
    lv_obj_add_style(s_remind, gh_ui_style(22), 0);
    lv_label_set_long_mode(s_remind, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_remind, GH_LOWER_W);
    lv_label_set_text(s_remind, "环境正常");
    lv_obj_set_style_text_color(s_remind, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_remind, 4, LOWER_Y);

    s_remind_sub = lv_label_create(parent);
    lv_obj_add_style(s_remind_sub, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_remind_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_remind_sub, GH_LOWER_W);
    lv_label_set_text(s_remind_sub, "");
    lv_obj_set_style_text_color(s_remind_sub, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_remind_sub, 4, LOWER_Y + 26);

    absent_chip(parent, 0, "土壤");
    absent_chip(parent, 1, "光照");
    absent_chip(parent, 2, "CO2");
}

static void main_update(void)
{
    edgi_sensor_snapshot_t snap;
    greenhouse_thresh_t th;
    rt_bool_t a_temp = RT_FALSE;
    rt_bool_t a_humi = RT_FALSE;
    char tbuf[16];
    char hbuf[16];
    char range[24];
    char line[96];
    const char *crop;
    int alert_len;

    if (edgi_sensor_monitor_get_snapshot(&snap) != RT_EOK)
        return;

    greenhouse_get_alert_flags(&a_temp, &a_humi, RT_NULL, RT_NULL);
    greenhouse_get_thresh(&th);

    fmt_deci(tbuf, sizeof(tbuf), snap.temp_deci);
    lv_label_set_text(s_temp.value, tbuf);
    lv_label_set_text(s_temp.tag, snap.environment_online ? "AHT20" : "IMU");
    lv_label_set_text(s_temp.sub, snap.environment_online ? "空气温度" : "板载温度，略高于室温");
    card_paint(&s_temp, a_temp);

    if (snap.humi_deci >= 0 && snap.environment_online)
    {
        fmt_deci(hbuf, sizeof(hbuf), snap.humi_deci);
        lv_label_set_text(s_humi.value, hbuf);
        lv_label_set_text(s_humi.tag, "AHT20");
        lv_label_set_text(s_humi.sub, "空气湿度");
        card_paint(&s_humi, a_humi);
    }
    else
    {
        lv_label_set_text(s_humi.value, "--");
        lv_label_set_text(s_humi.tag, "未接");
        lv_label_set_text(s_humi.sub, "没有 AHT20");
        card_paint(&s_humi, RT_FALSE);
        lv_obj_set_style_text_color(s_humi.value, lv_color_hex(C_TEXT_DIM),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    crop = gh_crop_get_current(range, sizeof(range));
    lv_label_set_text(s_crop_name, crop);
    fmt_deci(tbuf, sizeof(tbuf), th.temp_min_deci);
    fmt_deci(hbuf, sizeof(hbuf), th.temp_max_deci);
    rt_snprintf(line, sizeof(line), "%s  报警 %s~%s℃", range, tbuf, hbuf);
    lv_label_set_text(s_crop_range, line);

    alert_len = greenhouse_build_alert_text(line, sizeof(line));
    if (alert_len > 0)
    {
        lv_label_set_text(s_remind, line);
        lv_obj_set_style_text_color(s_remind, lv_color_hex(C_ALERT),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(s_remind, "环境正常");
        lv_obj_set_style_text_color(s_remind, lv_color_hex(C_ACCENT),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (g_app.ws.is_connected)
        lv_label_set_text(s_remind_sub, snap.wifi_ip[0] ? "小智在线，可以直接问大棚情况" : "小智在线");
    else if (snap.wifi_ip[0])
        lv_label_set_text_fmt(s_remind_sub, "小智未连接 · WiFi %s", snap.wifi_ip);
    else
        lv_label_set_text(s_remind_sub, "小智未连接 · WiFi 离线");
}

static const gh_page_t s_page = {
    .tab     = "主页",
    .create  = main_create,
    .update  = main_update,
    .on_show = RT_NULL,
    .on_hide = RT_NULL,
};

const gh_page_t *gh_page_main(void)
{
    return &s_page;
}
