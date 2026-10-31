/*
 * gh_page_set.c - 菜园管家「设置」页（第5个标签，横屏 800x480）
 *
 * 内容（自上而下，全部真实生效）：
 *   卡片1 屏幕亮度 —— LVGL 滑条 0-100，拖动实时调 PWM 背光(set_brightness)
 *   卡片2 网络校时 —— 「立即校时」按钮触发 gh_ntp_sync_async()，状态行
 *                      实时显示 校时中/成功 HH:MM/失败/未联网
 *   卡片3 系统信息 —— 堆内存剩余、运行时长、温度来源(AHT20/未接)
 *   卡片4 重启设备 —— 两段式确认（点一下变成「再点确认重启」，3s 后恢复）
 *
 * 数据更新：update() 每 1s 由引擎调用，刷新 NTP 状态/内存/时间标签。
 *
 * Change Logs:
 * 2026-11-01 新增：设置页（亮度/校时/系统信息/重启）。
 */
#include <rtthread.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "../sensors/gh_ntp.h"
#include "../sensors/sensor_monitor.h"

/* 背光 PWM（pwm.c 实现，main 启动即注册） */
extern void       set_brightness(rt_uint8_t percent);
extern rt_uint8_t get_brightness(void);

/* ---- 布局：两行两列卡片，下方一行宽卡片（重启） ---- */
#define SET_GAP     14
#define SET_CARD_W  ((GH_BODY_W - SET_GAP) / 2)          /* ~311 */
#define SET_CARD_H  118
#define SET_ROW2_Y  (SET_CARD_H + SET_GAP)               /* 132 */
#define SET_RBT_Y   (SET_ROW2_Y + SET_CARD_H + SET_GAP)  /* 264 */
#define SET_RBT_H   (GH_BODY_H - SET_RBT_Y)              /* ~86 */

static lv_obj_t *s_bright_val;   /* 亮度百分比标签 */
static lv_obj_t *s_slider;
static lv_obj_t *s_ntp_val;      /* 校时状态 */
static lv_obj_t *s_ntp_btn_lbl;
static lv_obj_t *s_mem_val;      /* 内存/运行时长 */
static lv_obj_t *s_src_val;      /* 温度来源 */
static lv_obj_t *s_rbt_lbl;      /* 重启按钮文字（两段式） */

static rt_bool_t s_confirm_reboot = RT_FALSE;
static rt_tick_t s_confirm_tick; /* 确认窗口超时 */

/* ---------- 小部件工厂 ---------- */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title,
                           int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(c, lv_color_hex(GH_COL_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(c, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(c, 14, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *t = lv_label_create(c);
    lv_obj_add_style(t, gh_ui_style(18), 0);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    return c;
}

/* ---------- 亮度 ---------- */
static void bright_changed_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (v < 5) v = 5;
    set_brightness((rt_uint8_t)v);
    lv_label_set_text_fmt(s_bright_val, "%d %%", v);
}

static void build_brightness(lv_obj_t *parent)
{
    lv_obj_t *c = make_card(parent, "屏幕亮度", 0, 0, SET_CARD_W, SET_CARD_H);

    s_bright_val = lv_label_create(c);
    lv_obj_add_style(s_bright_val, gh_ui_style(22), 0);
    lv_label_set_text_fmt(s_bright_val, "%d %%", get_brightness());
    lv_obj_set_style_text_color(s_bright_val, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_bright_val, LV_ALIGN_TOP_RIGHT, 0, -2);

    s_slider = lv_slider_create(c);
    lv_slider_set_range(s_slider, 5, 100);
    lv_slider_set_value(s_slider, get_brightness(), LV_ANIM_OFF);
    lv_obj_set_size(s_slider, SET_CARD_W - 28, 14);
    lv_obj_align(s_slider, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(GH_COL_CARD_HI), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(GH_COL_ACCENT), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_slider, lv_color_hex(GH_COL_ACCENT2), LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_slider, bright_changed_cb, LV_EVENT_VALUE_CHANGED, RT_NULL);
}

/* ---------- 网络校时 ---------- */
static void ntp_btn_cb(lv_event_t *e)
{
    (void)e;
    gh_ntp_init();
    gh_ntp_sync_async();
}

static void build_ntp(lv_obj_t *parent)
{
    lv_obj_t *c = make_card(parent, "网络校时", SET_CARD_W + SET_GAP, 0, SET_CARD_W, SET_CARD_H);

    lv_obj_t *btn = lv_button_create(c);
    lv_obj_set_size(btn, 108, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_obj_set_style_radius(btn, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(GH_COL_TAB_ACT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, ntp_btn_cb, LV_EVENT_CLICKED, RT_NULL);
    s_ntp_btn_lbl = lv_label_create(btn);
    lv_obj_add_style(s_ntp_btn_lbl, gh_ui_style(18), 0);
    lv_label_set_text(s_ntp_btn_lbl, "立即校时");
    lv_obj_set_style_text_color(s_ntp_btn_lbl, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_ntp_btn_lbl);

    s_ntp_val = lv_label_create(c);
    lv_obj_add_style(s_ntp_val, gh_ui_style(18), 0);
    lv_label_set_text(s_ntp_val, "未校时");
    lv_obj_set_style_text_color(s_ntp_val, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_ntp_val, LV_ALIGN_BOTTOM_RIGHT, 0, -10);
}

/* ---------- 系统信息 ---------- */
static void build_sysinfo(lv_obj_t *parent)
{
    lv_obj_t *c = make_card(parent, "系统信息", 0, SET_ROW2_Y, SET_CARD_W, SET_CARD_H);

    s_mem_val = lv_label_create(c);
    lv_obj_add_style(s_mem_val, gh_ui_style(20), 0);
    lv_label_set_text(s_mem_val, "--");
    lv_obj_set_style_text_color(s_mem_val, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_mem_val, LV_ALIGN_BOTTOM_LEFT, 0, -2);

    s_src_val = lv_label_create(c);
    lv_obj_add_style(s_src_val, gh_ui_style(18), 0);
    lv_label_set_text(s_src_val, "");
    lv_obj_set_style_text_color(s_src_val, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_src_val, LV_ALIGN_BOTTOM_LEFT, 0, -34);
}

/* ---------- 重启 ---------- */
static void rbt_cb(lv_event_t *e)
{
    (void)e;
    if (!s_confirm_reboot)
    {
        s_confirm_reboot = RT_TRUE;
        s_confirm_tick = rt_tick_get();
        lv_label_set_text(s_rbt_lbl, "再点一次确认重启");
        return;
    }
    /* 第二次点击：真正重启 */
    lv_label_set_text(s_rbt_lbl, "正在重启…");
    gh_ui_bubble_show("正在重启…");
    rt_hw_cpu_reset();
}

static void build_reboot(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, SET_CARD_W, SET_RBT_H);
    lv_obj_set_pos(btn, 0, SET_RBT_Y);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(GH_COL_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(btn, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, rbt_cb, LV_EVENT_CLICKED, RT_NULL);

    s_rbt_lbl = lv_label_create(btn);
    lv_obj_add_style(s_rbt_lbl, gh_ui_style(20), 0);
    lv_label_set_text(s_rbt_lbl, "重启设备");
    lv_obj_set_style_text_color(s_rbt_lbl, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(s_rbt_lbl);
}

/* ---------- gh_page_t 接口 ---------- */
static void set_create(lv_obj_t *parent)
{
    gh_ntp_init(); /* 确保线程在（幂等） */
    build_brightness(parent);
    build_ntp(parent);
    build_sysinfo(parent);
    build_reboot(parent);
}

static void set_update(void)
{
    /* 确认重启 3s 超时恢复 */
    if (s_confirm_reboot &&
        rt_tick_get() - s_confirm_tick > rt_tick_from_millisecond(3000))
    {
        s_confirm_reboot = RT_FALSE;
        lv_label_set_text(s_rbt_lbl, "重启设备");
    }

    /* NTP 状态 */
    {
        gh_ntp_state_t st = gh_ntp_get_state();
        edgi_sensor_snapshot_t snap;
        edgi_sensor_monitor_get_snapshot(&snap);
        rt_bool_t wifi_up = (snap.wifi_ip[0] != '\0' && snap.wifi_ip[0] != '-');
        const char *txt; rt_uint32_t col = GH_COL_TEXT_DIM;
        switch (st)
        {
        case GH_NTP_RUNNING: txt = "校时中…";                 col = GH_COL_ACCENT;  break;
        case GH_NTP_OK:      txt = "已同步网络时间";           col = GH_COL_ACCENT;  break;
        case GH_NTP_FAIL:    txt = "校时失败，点左侧重试";      col = GH_COL_ALERT;   break;
        case GH_NTP_IDLE:
        default:
            txt = wifi_up ? "未校时" : "未联网";
            col = GH_COL_TEXT_DIM;
            break;
        }
        lv_label_set_text(s_ntp_val, txt);
        lv_obj_set_style_text_color(s_ntp_val, lv_color_hex(col), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    /* 系统信息：内存 + 运行时长 */
    {
        rt_size_t total = 0, used = 0, max_used = 0;
        rt_memory_info(&total, &used, &max_used);
        rt_uint32_t up_s = rt_tick_get() / RT_TICK_PER_SECOND;
        lv_label_set_text_fmt(s_mem_val,
                              "剩余内存 %u KB · 已运行 %u h %u m",
                              (unsigned)((total - used) / 1024),
                              up_s / 3600, (up_s % 3600) / 60);

        edgi_sensor_snapshot_t snap;
        edgi_sensor_monitor_get_snapshot(&snap);
        lv_label_set_text(s_src_val,
                          snap.environment_online ? "温度来源 AHT20 · 湿度已接"
                                                  : "温度来源 未接 · 湿度未接");
    }
}

static const gh_page_t s_page =
{
    .tab     = "设置",
    .create  = set_create,
    .update  = set_update,
    .on_show = RT_NULL,
    .on_hide = RT_NULL,
};

const gh_page_t *gh_page_set(void)
{
    return &s_page;
}
