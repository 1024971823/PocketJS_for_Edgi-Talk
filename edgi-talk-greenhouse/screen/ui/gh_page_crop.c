/*
 * gh_page_crop.c - 作物库页：常见温室作物适宜温度一键设阈值
 *
 * 职责：展示 8 种常见温室作物卡片，点击即把该作物适宜温度区间
 * 写入温度报警阈值（greenhouse_set_thresh "temp"），气泡确认。
 * 温度是本板当前唯一真实的环境告警通道（AHT20 空气温湿度）；
 * 湿度阈值保留在配置中（接上 AHT20 即生效），可由语音
 * （AI SetThresholds）或 msh `xz_thresh` 修改。
 *
 * 布局（GH_BODY 636x350，局部坐标）：
 *   y=0    提示行 18pt 暗色「点击作物，一键设置温度报警区间」
 *   y=30   2 列 x 4 行作物卡片，卡 312x64，列距 12，行距 8
 *          卡：GH_COL_CARD 底、圆角 14；选中 → GH_COL_TAB_ACT 底
 *          + 左缘 3px GH_COL_ACCENT 竖条；名 22pt、适温 18pt 暗右对齐
 *   y=318  1px 分隔线
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 * 2026-11-01     Greenhouse     重写为深色设计令牌版，作物库扩到 8 种
 */

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "../sensors/sensor_monitor.h"

/* 作物适宜温度区间（白天适温, 与 docs/03 人设提示词保持一致） */
typedef struct
{
    const char *name;
    int t_min, t_max;   /* ℃ */
} crop_item_t;

static const crop_item_t s_crops[] =
{
    { "通用", 18, 30 },
    { "番茄", 22, 28 },
    { "黄瓜", 25, 30 },
    { "生菜", 15, 22 },
    { "草莓", 20, 25 },
    { "辣椒", 25, 30 },
    { "茄子", 24, 30 },
    { "西瓜", 25, 32 },
};
#define CROP_NUM   (sizeof(s_crops) / sizeof(s_crops[0]))
#define CROP_DEFAULT 0   /* 默认"通用" */

/* 卡片网格几何：整宽两列，行距 8 */
#define CROP_COL_GAP  12
#define CROP_ROW_GAP  8
#define CROP_W        ((GH_BODY_W - CROP_COL_GAP) / 2)   /* 312 */
#define CROP_H        64
#define CROP_GRID_Y   30
#define CROP_PITCH    (CROP_H + CROP_ROW_GAP)            /* 72 */
#define CROP_BAR_W    3

static lv_obj_t *s_btn[CROP_NUM];
static lv_obj_t *s_bar[CROP_NUM];
static int s_current = CROP_DEFAULT;
static lv_obj_t *s_thresh_label;

/* 当前作物名 + 适宜温度区间文本（气泡/日志用） */
const char *gh_crop_get_current(char *range_buf, int range_len)
{
    const crop_item_t *c = &s_crops[s_current];
    if (range_buf != RT_NULL && range_len > 0)
        rt_snprintf(range_buf, range_len, "适温%d~%d℃", c->t_min, c->t_max);
    return c->name;
}

/* 页脚阈值回显 */
static void refresh_thresh_label(void)
{
    greenhouse_thresh_t th;
    greenhouse_get_thresh(&th);
    lv_label_set_text_fmt(s_thresh_label,
        "当前报警阈值  温度%d.%d~%d.%d℃  湿度%d.%d~%d.%d%%（接AHT20后生效）",
        th.temp_min_deci / 10, th.temp_min_deci % 10,
        th.temp_max_deci / 10, th.temp_max_deci % 10,
        th.humi_min_deci / 10, th.humi_min_deci % 10,
        th.humi_max_deci / 10, th.humi_max_deci % 10);
}

/* 选中态样式：选中卡换 TAB_ACT 底 + 显示左侧翠绿竖条 */
static void crop_style_refresh(void)
{
    for (rt_size_t i = 0; i < CROP_NUM; i++)
    {
        rt_bool_t sel = (i == (rt_size_t)s_current);
        lv_obj_set_style_bg_color(s_btn[i],
            sel ? lv_color_hex(GH_COL_TAB_ACT) : lv_color_hex(GH_COL_CARD),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        if (sel)
            lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void crop_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const crop_item_t *c;
    char range[24];
    char bubble[96];

    if (idx < 0 || idx >= (int)CROP_NUM)
        return;
    c = &s_crops[idx];

    greenhouse_set_thresh("temp", (float)c->t_min, (float)c->t_max);
    s_current = idx;

    crop_style_refresh();
    refresh_thresh_label();

    rt_snprintf(bubble, sizeof(bubble),
                "已按%s设定温度报警区间（%s），越界会提醒你",
                c->name, gh_crop_get_current(range, sizeof(range)));
    gh_ui_bubble_show(bubble);
}

/* 建一张作物卡片：底卡 + 左缘竖条 + 名 + 适温 */
static lv_obj_t *crop_card_create(lv_obj_t *parent, rt_size_t i, int x, int y)
{
    char sub[32];
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btn, CROP_W, CROP_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(GH_COL_CARD),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btn, crop_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);

    /* 左缘选中竖条（未选中隐藏），高=卡高-16，圆角 2 */
    lv_obj_t *bar = lv_obj_create(btn);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);   /* 竖条不吃点击 */
    lv_obj_set_size(bar, CROP_BAR_W, CROP_H - 16);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(GH_COL_ACCENT),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *name = lv_label_create(btn);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(name, gh_ui_style(22), 0);
    lv_label_set_text(name, s_crops[i].name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, CROP_W - 180);
    lv_obj_set_style_text_color(name, lv_color_hex(GH_COL_TEXT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *range = lv_label_create(btn);
    lv_obj_remove_flag(range, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(range, gh_ui_style(18), 0);
    rt_snprintf(sub, sizeof(sub), "适温 %d~%d℃", s_crops[i].t_min, s_crops[i].t_max);
    lv_label_set_text(range, sub);
    lv_label_set_long_mode(range, LV_LABEL_LONG_DOT);
    lv_obj_set_width(range, 140);
    lv_obj_set_style_text_color(range, lv_color_hex(GH_COL_TEXT_DIM),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(range, LV_ALIGN_RIGHT_MID, -16, 0);

    s_bar[i] = bar;
    return btn;
}

static void crop_create(lv_obj_t *parent)
{
    /* 顶部提示行 */
    lv_obj_t *tip = lv_label_create(parent);
    lv_obj_remove_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(tip, gh_ui_style(18), 0);
    lv_label_set_text(tip, "点击作物，一键设置温度报警区间");
    lv_label_set_long_mode(tip, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tip, GH_BODY_W - 8);
    lv_obj_set_style_text_color(tip, lv_color_hex(GH_COL_TEXT_DIM),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(tip, 4, 0);

    /* 2 列 x 4 行作物卡 */
    for (rt_size_t i = 0; i < CROP_NUM; i++)
    {
        int col = (int)(i % 2);
        int row = (int)(i / 2);
        s_btn[i] = crop_card_create(parent, i,
                                    col * (CROP_W + CROP_COL_GAP),
                                    CROP_GRID_Y + row * CROP_PITCH);
    }

    /* 分隔线 + 页脚阈值行 */
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_flag(div, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(div, GH_BODY_W, 1);
    lv_obj_set_pos(div, 0, CROP_GRID_Y + 4 * CROP_PITCH);   /* 318 */
    lv_obj_set_style_border_width(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(div, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(div, lv_color_hex(GH_COL_DIVIDER),
                              LV_PART_MAIN | LV_STATE_DEFAULT);

    s_thresh_label = lv_label_create(parent);
    lv_obj_remove_flag(s_thresh_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(s_thresh_label, gh_ui_style(18), 0);
    lv_label_set_text(s_thresh_label, "");
    lv_label_set_long_mode(s_thresh_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_thresh_label, GH_BODY_W - 8);
    lv_obj_set_style_text_color(s_thresh_label, lv_color_hex(GH_COL_ACCENT),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_thresh_label, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    crop_style_refresh();
    refresh_thresh_label();
}

static void crop_update(void)
{
    /* 阈值可能被语音/AI(MCP/Thing)或 msh 修改, 每秒回读保持一致 */
    refresh_thresh_label();
}

static const gh_page_t s_page = {
    .tab     = "作物",
    .create  = crop_create,
    .update  = crop_update,
    .on_show = RT_NULL,
    .on_hide = RT_NULL,
};

const gh_page_t *gh_page_crop(void)
{
    return &s_page;
}
