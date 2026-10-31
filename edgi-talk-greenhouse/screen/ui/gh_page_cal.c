/*
 * gh_page_cal.c - 日历页：时钟 + 当月月历
 *
 * 职责：显示 RTC 时钟（HH:MM:SS）、日期（年月日 星期X）、
 * 未同步提示，以及当月 7 列月历（今天高亮）。
 *
 * 布局（GH_BODY 636x350 内，局部坐标）：
 *   左侧时钟卡 300x350 @ (0,0)：44pt 翠绿时间居中，22pt 日期，
 *   18pt 灰提示（未同步时提示 msh date 命令，二期接 NTP）。
 *   右侧月历卡 320x350 @ (316,0)：18pt 灰星期表头 + 40x32 日期格，
 *   今天 GH_COL_TAB_ACT 底 + 翠绿字，其余 GH_COL_PANEL 底。
 *
 * 时间源：RT-Thread RTC（RT_USING_RTC, time()/localtime_r）。
 * 年份 < 2026 视为 RTC 未设置：时钟 --:--:--，月历清空。
 * 月历只在年/月/日变化时重建（lv_obj_clean 后重画），不随秒刷新。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 * 2026-11-01     Greenhouse     重写为新暗色设计，全量使用 GH_COL_* 令牌
 */

#include <rtthread.h>
#include <string.h>
/* <time.h> 直接包含会在 newlib 里先走到 sys/types.h，再撞上
 * RT-Thread 的 sys/time.h，此时 time_t 还没定义。先走 sys/time.h，
 * 它会在用到 time_t 之前把 time.h 包含完。 */
#include <sys/time.h>
#include <lvgl.h>
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"

/* ---- 布局常量（GH_BODY 内局部坐标） ---- */
#define CAL_CLOCK_W     300     /* 左时钟卡宽 */
#define CAL_GRID_X      316     /* 右月历卡位置 */
#define CAL_GRID_W      320
#define CAL_PANEL_H     350

#define CELL_W          40      /* 日期格 */
#define CELL_H          32
#define CELL_PITCH      44      /* 列间距: 8 + 7*44 = 316 居卡内 */
#define ROW_PITCH       48      /* 行间距: 52 + 5*48 + 32 = 324 */
#define GRID_X0         8
#define GRID_HDR_Y      14
#define GRID_ROW0_Y     52

static const char *s_week_hdr[7] = {"一", "二", "三", "四", "五", "六", "日"};
static const char *s_weekday_cn[7] = {"一", "二", "三", "四", "五", "六", "日"};

static lv_obj_t *s_clock;       /* 大号时钟 */
static lv_obj_t *s_date;        /* 日期行: 2026年10月28日 星期三 */
static lv_obj_t *s_hint;        /* 同步状态提示 */
static lv_obj_t *s_grid;        /* 月历卡片容器 */
static int s_grid_year = -1;    /* 当前月历渲染的年月日, 变化时重建 */
static int s_grid_mon  = -1;
static int s_grid_day  = -1;    /* 跨天要重画“今天”高亮 */

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

/* 渲染某年某月的月历网格（先 lv_obj_clean 清掉旧格子） */
static void calendar_render(int year, int mon, int today)
{
    int first, days, row, col, day;
    char buf[8];

    lv_obj_clean(s_grid);

    /* 星期表头 */
    for (col = 0; col < 7; col++)
    {
        lv_obj_t *w = lv_label_create(s_grid);
        lv_obj_add_style(w, gh_ui_style(18), 0);
        lv_label_set_text(w, s_week_hdr[col]);
        lv_obj_set_style_text_color(w, lv_color_hex(GH_COL_TEXT_DIM),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(w, CELL_W);
        lv_obj_set_pos(w, GRID_X0 + col * CELL_PITCH, GRID_HDR_Y);
    }

    first = first_weekday(year, mon);
    days  = days_in_month(year, mon);
    for (day = 1; day <= days; day++)
    {
        int pos = first + day - 1;
        rt_bool_t is_today;

        row = pos / 7;
        col = pos % 7;
        is_today = (day == today);

        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_pos(cell, GRID_X0 + col * CELL_PITCH,
                       GRID_ROW0_Y + row * ROW_PITCH);
        lv_obj_set_style_radius(cell, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        /* 今天：选中底 + 翠绿字；其余：面板底 + 主文字 */
        lv_obj_set_style_bg_color(cell,
                                  lv_color_hex(is_today ? GH_COL_TAB_ACT : GH_COL_PANEL),
                                  LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *num = lv_label_create(cell);
        lv_obj_add_style(num, gh_ui_style(18), 0);
        rt_snprintf(buf, sizeof(buf), "%d", day);
        lv_label_set_text(num, buf);
        lv_obj_set_style_text_color(num,
                                    lv_color_hex(is_today ? GH_COL_ACCENT : GH_COL_TEXT),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(num);
    }

    s_grid_year = year;
    s_grid_mon  = mon;
    s_grid_day  = today;
}

/* 卡片左缘 3px 色条（设计系统统一装饰） */
static void edge_bar(lv_obj_t *card, int color)
{
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 3, CAL_PANEL_H - 16);
    lv_obj_set_pos(bar, 8, 8);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void cal_create(lv_obj_t *parent)
{
    lv_obj_t *clock_panel;

    /* ---- 左：时钟卡 ---- */
    clock_panel = lv_obj_create(parent);
    lv_obj_remove_flag(clock_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clock_panel, CAL_CLOCK_W, CAL_PANEL_H);
    lv_obj_set_pos(clock_panel, 0, 0);
    lv_obj_set_style_radius(clock_panel, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(clock_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(clock_panel, lv_color_hex(GH_COL_CARD),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(clock_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    edge_bar(clock_panel, GH_COL_ACCENT);

    s_clock = lv_label_create(clock_panel);
    lv_obj_add_style(s_clock, gh_ui_style(44), 0);
    lv_label_set_text(s_clock, "--:--:--");
    lv_obj_set_style_text_color(s_clock, lv_color_hex(GH_COL_ACCENT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_clock, LV_ALIGN_CENTER, 0, -64);

    s_date = lv_label_create(clock_panel);
    lv_obj_add_style(s_date, gh_ui_style(22), 0);
    lv_label_set_text(s_date, "");
    lv_obj_set_style_text_color(s_date, lv_color_hex(GH_COL_TEXT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 4);

    s_hint = lv_label_create(clock_panel);
    lv_obj_add_style(s_hint, gh_ui_style(18), 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_hint, CAL_CLOCK_W - 28);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(GH_COL_TEXT_DIM),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 56);

    /* ---- 右：月历卡（格子在 calendar_render 里画） ---- */
    s_grid = lv_obj_create(parent);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_grid, CAL_GRID_W, CAL_PANEL_H);
    lv_obj_set_pos(s_grid, CAL_GRID_X, 0);
    lv_obj_set_style_radius(s_grid, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_grid, lv_color_hex(GH_COL_CARD),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_grid, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    edge_bar(s_grid, GH_COL_ACCENT2);
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
            s_grid_mon  = -1;
            s_grid_day  = -1;
        }
        return;
    }

    lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    rt_snprintf(buf, sizeof(buf), "%d年%d月%d日 星期%s",
                tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                s_weekday_cn[(tm_now.tm_wday + 6) % 7]);
    lv_label_set_text(s_date, buf);
    lv_label_set_text(s_hint, "已同步");

    /* 仅年/月/日变化时重建网格，不随秒刷新 */
    if (tm_now.tm_year + 1900 != s_grid_year ||
        tm_now.tm_mon + 1     != s_grid_mon  ||
        tm_now.tm_mday        != s_grid_day)
    {
        calendar_render(tm_now.tm_year + 1900, tm_now.tm_mon + 1,
                        tm_now.tm_mday);
    }
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
