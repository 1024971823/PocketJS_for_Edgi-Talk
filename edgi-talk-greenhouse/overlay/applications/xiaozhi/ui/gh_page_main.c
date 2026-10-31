/*
 * gh_page_main.c - 主页：实时环境监测仪表盘 + 小智提醒
 *
 * 布局（GH_BODY 636x350，本地坐标）：
 *   第一行 y=0    空气温度 / 空气湿度两张英雄卡（310x170，间隔16），
 *                 左缘 3px 色条，右上来源标（AHT20/未接），报警时卡底与数值转红
 *   第二行 y=186  土壤 / 光照 / CO2 三个参数 chip（172x76，间隔12），
 *                 右缘停在 GH_LOWER_W（气泡左侧）；板上无传感器，固定「未接」不仿真
 *   第三行 y=282  上方 1px 分隔线；左：当前作物 + 适温/报警阈值，
 *                 右：提醒（报警原文 / 环境正常）+ 小智在线状态与 WiFi IP
 *
 * 数据全部来自板上真实模块：AHT20 离线显示 --，土壤/光照/CO2 一律「未接」。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     设备状态六宫格
 * 2026-10-31     Greenhouse     改回作物环境监测，未接通道不再仿真
 * 2026-11-01     Greenhouse     深色现代重设计：英雄卡 + 参数 chip + 底栏提醒
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

#define HERO_GAP      16
#define HERO_W        ((GH_BODY_W - HERO_GAP) / 2)        /* 310 */
#define HERO_H        170

#define CHIP_Y        186
#define CHIP_H        76
#define CHIP_GAP      12
#define CHIP_W        ((GH_LOWER_W - 2 * CHIP_GAP) / 3)   /* 172 */

#define ROW3_DIV_Y    272                                 /* 分隔线 */
#define ROW3_Y        282
#define ROW3_CAP_X    104                                 /* 作物名起点（留「当前作物」标） */
#define ROW3_LW       300                                 /* 左侧作物块宽 */
#define ROW3_RX       316                                 /* 右侧提醒起点 */
#define ROW3_RW       (GH_LOWER_W - ROW3_RX)              /* 224 */

typedef struct
{
    lv_obj_t *card;
    lv_obj_t *bar;     /* 左缘 3px 色条 */
    lv_obj_t *title;
    lv_obj_t *tag;     /* 右上来源标 AHT20/未接 */
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *sub;
} gh_card_t;

static gh_card_t s_temp;
static gh_card_t s_humi;
static lv_obj_t *s_crop_name;
static lv_obj_t *s_crop_info;
static lv_obj_t *s_remind;
static lv_obj_t *s_remind_sub;

static void fmt_deci(char *buf, int len, int deci)
{
    int neg = deci < 0;
    int abs_v = neg ? -deci : deci;
    rt_snprintf(buf, len, "%s%d.%d", neg ? "-" : "", abs_v / 10, abs_v % 10);
}

/* 英雄卡：标题 + 来源标 + 大数值 + 单位 + 底部说明，左缘色条 */
static void hero_create(gh_card_t *c, lv_obj_t *parent, int col,
                        const char *title, const char *unit)
{
    c->card = lv_obj_create(parent);
    lv_obj_remove_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c->card, HERO_W, HERO_H);
    lv_obj_set_pos(c->card, col * (HERO_W + HERO_GAP), 0);
    lv_obj_set_style_radius(c->card, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c->card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(c->card, lv_color_hex(GH_COL_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(c->card, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 左缘 3px 竖色条，上下各留 8px */
    c->bar = lv_obj_create(c->card);
    lv_obj_remove_flag(c->bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c->bar, 3, HERO_H - 16);
    lv_obj_set_pos(c->bar, 0, 8);
    lv_obj_set_style_radius(c->bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c->bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(c->bar, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    c->title = lv_label_create(c->card);
    lv_obj_add_style(c->title, gh_ui_style(18), 0);
    lv_label_set_long_mode(c->title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c->title, HERO_W - 140);
    lv_label_set_text(c->title, title);
    lv_obj_set_style_text_color(c->title, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(c->title, 18, 14);

    c->tag = lv_label_create(c->card);
    lv_obj_add_style(c->tag, gh_ui_style(18), 0);
    lv_label_set_text(c->tag, "");
    lv_obj_set_style_text_color(c->tag, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->tag, LV_ALIGN_TOP_RIGHT, -14, 14);

    c->value = lv_label_create(c->card);
    lv_obj_add_style(c->value, gh_ui_style(44), 0);
    lv_label_set_text(c->value, "--");
    lv_obj_set_style_text_color(c->value, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->value, LV_ALIGN_LEFT_MID, 18, 10);

    /* 单位贴数值右下基线，update 里按文本宽度重新对齐 */
    c->unit = lv_label_create(c->card);
    lv_obj_add_style(c->unit, gh_ui_style(18), 0);
    lv_label_set_text(c->unit, unit);
    lv_obj_set_style_text_color(c->unit, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align_to(c->unit, c->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);

    c->sub = lv_label_create(c->card);
    lv_obj_add_style(c->sub, gh_ui_style(18), 0);
    lv_label_set_long_mode(c->sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c->sub, HERO_W - 36);
    lv_label_set_text(c->sub, "");
    lv_obj_set_style_text_color(c->sub, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(c->sub, LV_ALIGN_BOTTOM_LEFT, 18, -14);
}

/* 英雄卡每秒刷新：数值/来源/说明 + 报警配色 */
static void hero_set(gh_card_t *c, rt_bool_t online, int deci, rt_bool_t alert,
                     const char *sub_on, const char *sub_off)
{
    char buf[16];
    rt_bool_t hot = online && alert;

    if (online)
    {
        fmt_deci(buf, sizeof(buf), deci);
        lv_label_set_text(c->value, buf);
        lv_label_set_text(c->tag, "AHT20");
        lv_label_set_text(c->sub, sub_on);
        lv_obj_remove_flag(c->unit, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text(c->value, "--");
        lv_label_set_text(c->tag, "未接");
        lv_label_set_text(c->sub, sub_off);
        lv_obj_add_flag(c->unit, LV_OBJ_FLAG_HIDDEN);
    }

    /* 数值宽度会变，单位跟随对齐 */
    lv_obj_align_to(c->unit, c->value, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);

    lv_obj_set_style_bg_color(c->card, lv_color_hex(hot ? GH_COL_ALERT_BG : GH_COL_CARD),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(c->bar, lv_color_hex(hot ? GH_COL_ALERT : GH_COL_ACCENT),
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(c->value,
                                lv_color_hex(hot ? GH_COL_ALERT : (online ? GH_COL_TEXT : GH_COL_TEXT_DIM)),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* 未接参数 chip：左色点 + 名称 + 「未接」，不放仿真值 */
static void chip_create(lv_obj_t *parent, int index, const char *name)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_t *dot;
    lv_obj_t *nm;
    lv_obj_t *val;

    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chip, CHIP_W, CHIP_H);
    lv_obj_set_pos(chip, index * (CHIP_W + CHIP_GAP), CHIP_Y);
    lv_obj_set_style_radius(chip, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(chip, lv_color_hex(GH_COL_PANEL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    dot = lv_obj_create(chip);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dot, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    nm = lv_label_create(chip);
    lv_obj_add_style(nm, gh_ui_style(18), 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, CHIP_W - 44);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 32, 12);

    val = lv_label_create(chip);
    lv_obj_add_style(val, gh_ui_style(22), 0);
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_width(val, CHIP_W - 44);
    lv_label_set_text(val, "未接");
    lv_obj_set_style_text_color(val, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 32, -12);
}

static void main_create(lv_obj_t *parent)
{
    lv_obj_t *div;
    lv_obj_t *cap;

    hero_create(&s_temp, parent, 0, "空气温度", "℃");
    hero_create(&s_humi, parent, 1, "空气湿度", "%RH");

    chip_create(parent, 0, "土壤");
    chip_create(parent, 1, "光照");
    chip_create(parent, 2, "CO2");

    /* 底栏分隔线 */
    div = lv_obj_create(parent);
    lv_obj_remove_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(div, GH_LOWER_W, 1);
    lv_obj_set_pos(div, 0, ROW3_DIV_Y);
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(div, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 左：当前作物 + 适温/报警阈值 */
    cap = lv_label_create(parent);
    lv_obj_add_style(cap, gh_ui_style(18), 0);
    lv_label_set_text(cap, "当前作物");
    lv_obj_set_style_text_color(cap, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(cap, 0, ROW3_Y + 10);

    s_crop_name = lv_label_create(parent);
    lv_obj_add_style(s_crop_name, gh_ui_style(28), 0);
    lv_label_set_long_mode(s_crop_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_crop_name, ROW3_LW - ROW3_CAP_X);
    lv_label_set_text(s_crop_name, "通用");
    lv_obj_set_style_text_color(s_crop_name, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_crop_name, ROW3_CAP_X, ROW3_Y);

    s_crop_info = lv_label_create(parent);
    lv_obj_add_style(s_crop_info, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_crop_info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_crop_info, ROW3_LW);
    lv_label_set_text(s_crop_info, "");
    lv_obj_set_style_text_color(s_crop_info, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_crop_info, 0, ROW3_Y + 40);

    /* 右：提醒 + 小智/WiFi 状态 */
    s_remind = lv_label_create(parent);
    lv_obj_add_style(s_remind, gh_ui_style(22), 0);
    lv_label_set_long_mode(s_remind, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_remind, ROW3_RW);
    lv_label_set_text(s_remind, "环境正常");
    lv_obj_set_style_text_color(s_remind, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_remind, ROW3_RX, ROW3_Y + 2);

    s_remind_sub = lv_label_create(parent);
    lv_obj_add_style(s_remind_sub, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_remind_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_remind_sub, ROW3_RW);
    lv_label_set_text(s_remind_sub, "");
    lv_obj_set_style_text_color(s_remind_sub, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_remind_sub, ROW3_RX, ROW3_Y + 36);
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

    hero_set(&s_temp, snap.environment_online, snap.temp_deci, a_temp,
             "空气温度", "没有 AHT20");
    hero_set(&s_humi, snap.environment_online && snap.humi_deci >= 0,
             snap.humi_deci, a_humi, "空气湿度", "没有 AHT20");

    /* 当前作物：适温区间 + 正在生效的报警阈值 */
    crop = gh_crop_get_current(range, sizeof(range));
    lv_label_set_text(s_crop_name, crop);
    fmt_deci(tbuf, sizeof(tbuf), th.temp_min_deci);
    fmt_deci(hbuf, sizeof(hbuf), th.temp_max_deci);
    rt_snprintf(line, sizeof(line), "%s · 报警 %s~%s℃", range, tbuf, hbuf);
    lv_label_set_text(s_crop_info, line);

    /* 提醒：有报警显示原文，否则「环境正常」 */
    alert_len = greenhouse_build_alert_text(line, sizeof(line));
    if (alert_len > 0)
    {
        lv_label_set_text(s_remind, line);
        lv_obj_set_style_text_color(s_remind, lv_color_hex(GH_COL_ALERT),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        lv_label_set_text(s_remind, "环境正常");
        lv_obj_set_style_text_color(s_remind, lv_color_hex(GH_COL_ACCENT),
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
