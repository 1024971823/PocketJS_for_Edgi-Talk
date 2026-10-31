/*
 * gh_page_cal.c - 日历页：时钟 + 当月月历
 *
 * 时间源：RT-Thread RTC（RT_USING_RTC, time()/localtime_r）。
 * 时钟未同步（年份 < 2026，RTC 未设置）时给出提示，可用 msh 的
 * date 命令设置；联网后可接入 NTP（二期）。
 *
 * 月历：手绘 7 列网格（一~日），今天格子红底高亮；月份切换时重建。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 */

#include <rtthread.h>
#include <string.h>
/* <time.h> 直接包含会在 newlib 里先走到 sys/types.h，再撞上
 * RT-Thread 的 sys/time.h，此时 time_t 还没定义。先走 sys/time.h，
 * 它会在用到 time_t 之前把 time.h 包含完。 */
#include <sys/time.h>
#include <lvgl.h>
#include "gh_ui_pages.h"

#define C_TEXT      0xFFFFFF
#define C_TEXT_DIM  0x8A93A6
#define C_ACCENT    0x35C4A0
#define C_ALERT     0xFF5050
#define C_CAL_CELL  0x1C2430

static const char *s_week_hdr[7] = {"一", "二", "三", "四", "五", "六", "日"};
static const char *s_weekday_cn[7] = {"一", "二", "三", "四", "五", "六", "日"};

static lv_obj_t *s_clock;       /* 大号时钟 */
static lv_obj_t *s_date;        /* 日期行: 2026年10月28日 星期三 */
static lv_obj_t *s_hint;        /* 未同步提示 */
static lv_obj_t *s_grid;        /* 月历容器 */
static int s_grid_year = -1;    /* 当前月历渲染的年月, 变化时重建 */
static int s_grid_mon = -1;
static lv_obj_t *s_today_cell = RT_NULL;

/* 该月天数（公历） */
static int days_in_month(int year, int mon)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int n = d[mon - 1];
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        n = 29;
    return n;
}

/* 1900-01-01 是星期一; 计算某年某月 1 日是星期几(0=周一) */
static int first_weekday(int year, int mon)
{
    struct tm t;
    time_t base;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = 1;
    base = mktime(&t);
    if (base < 0)
        return 0;
    localtime_r(&base, &t);
    return (t.tm_wday + 6) % 7;    /* tm_wday: 0=周日 → 转 0=周一 */
}

/* 渲染某年某月的月历网格 */
static void calendar_render(int year, int mon)
{
    int first, days, row, col, day;
    char buf[8];

    /* 清掉旧格子 */
    lv_obj_clean(s_grid);
    s_today_cell = RT_NULL;

    /* 星期表头 */
    for (col = 0; col < 7; col++)
    {
        lv_obj_t *w = lv_label_create(s_grid);
        lv_obj_add_style(w, gh_ui_style(18), 0);
        lv_label_set_text(w, s_week_hdr[col]);
        lv_obj_set_style_text_color(w, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_pos(w, 8 + col * 64, 0);
    }

    first = first_weekday(year, mon);
    days = days_in_month(year, mon);
    for (day = 1; day <= days; day++)
    {
        int pos = first + day - 1;
        row = pos / 7;
        col = pos % 7;

        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cell, 56, 30);
        lv_obj_set_pos(cell, 8 + col * 64, 30 + row * 34);
        lv_obj_set_style_radius(cell, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(cell, lv_color_hex(C_CAL_CELL), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *num = lv_label_create(cell);
        lv_obj_add_style(num, gh_ui_style(18), 0);
        rt_snprintf(buf, sizeof(buf), "%d", day);
        lv_label_set_text(num, buf);
        lv_obj_set_style_text_color(num, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(num);
    }

    /* 今天格子：创建后按 day 反查（重新遍历一次标记, 简单直接） */
    {
        time_t now = time(RT_NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        if (tm_now.tm_year + 1900 == year && tm_now.tm_mon + 1 == mon)
        {
            int pos = first + tm_now.tm_mday - 1;
            int idx = pos;    /* 含表头行前偏移: 格子从 0 开始编号 */
            /* grid 子对象: 0..6 是表头, 之后是格子 */
            if (idx + 7 < (int)lv_obj_get_child_cnt(s_grid))
            {
                s_today_cell = lv_obj_get_child(s_grid, idx + 7);
                lv_obj_set_style_bg_color(s_today_cell, lv_color_hex(0x8C2B2B), LV_PART_MAIN | LV_STATE_DEFAULT);
            }
        }
    }

    s_grid_year = year;
    s_grid_mon = mon;
}

static void cal_create(lv_obj_t *parent)
{
    s_clock = lv_label_create(parent);
    lv_obj_add_style(s_clock, gh_ui_style(44), 0);
    lv_label_set_text(s_clock, "--:--:--");
    lv_obj_set_style_text_color(s_clock, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_clock, 8, 4);

    s_date = lv_label_create(parent);
    lv_obj_add_style(s_date, gh_ui_style(22), 0);
    lv_label_set_text(s_date, "");
    lv_obj_set_style_text_color(s_date, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_date, 8, 58);

    s_hint = lv_label_create(parent);
    lv_obj_add_style(s_hint, gh_ui_style(18), 0);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xE0B040), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_hint, 8, 92);

    /* 月历占页面右侧 */
    s_grid = lv_obj_create(parent);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_grid, 470, 210);
    lv_obj_set_pos(s_grid, 8, 96);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void cal_update(void)
{
    time_t now = time(RT_NULL);
    struct tm tm_now;
    char buf[64];

    localtime_r(&now, &tm_now);

    if (tm_now.tm_year + 1900 < 2026)
    {
        /* RTC 未设置 */
        lv_label_set_text(s_clock, "--:--:--");
        lv_label_set_text(s_date, "时钟未同步");
        lv_label_set_text(s_hint, "串口 msh 输入 date 命令设置时间(二期接入NTP)");
        if (s_grid_year >= 0)
        {
            lv_obj_clean(s_grid);
            s_grid_year = -1;
        }
        return;
    }

    lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    rt_snprintf(buf, sizeof(buf), "%d年%d月%d日 星期%s",
                tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                s_weekday_cn[(tm_now.tm_wday + 6) % 7]);
    lv_label_set_text(s_date, buf);
    lv_label_set_text(s_hint, "");

    if (tm_now.tm_year + 1900 != s_grid_year || tm_now.tm_mon + 1 != s_grid_mon)
        calendar_render(tm_now.tm_year + 1900, tm_now.tm_mon + 1);
}

static const gh_page_t s_page = {
    .tab     = "日历",
    .create  = cal_create,
    .update  = cal_update,
    .on_show = RT_NULL,
    .on_hide = RT_NULL,
};

const gh_page_t *gh_page_cal(void)
{
    return &s_page;
}
