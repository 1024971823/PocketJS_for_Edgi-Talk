/*
 * gh_page_crop.c - 作物库页：常见蔬菜适宜温度一键设阈值
 *
 * 交互：点击作物卡片 → 调 greenhouse_set_thresh("temp", ...) 把该作物的
 * 适宜温度区间写入报警阈值 → 气泡确认。温度是本板当前唯一真实的
 * 环境告警通道（AHT20 空气温度 / IMU 板载温度）；湿度阈值保留在
 * 配置中（接上 AHT20 即生效），可由语音（AI SetThresholds）或 msh
 * `xz_thresh` 修改。
 *
 * 阈值也可被语音反向修改（MCP/Thing），本页 update() 每秒回读显示，
 * 保证 UI 与实际阈值一致。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 */

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <lvgl.h>
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "../sensors/sensor_monitor.h"

#define C_TEXT      0xF2F4F8
#define C_TEXT_DIM  0x7C8698
#define C_ACCENT    0x35C4A0
#define C_CARD      0x11161D
#define C_CARD_SEL  0x12385A

/* 作物适宜温度区间（白天适温, 与 docs/03 人设提示词保持一致） */
typedef struct
{
    const char *name;
    int t_min, t_max;   /* ℃ */
} crop_item_t;

static const crop_item_t s_crops[] =
{
    { "番茄", 22, 28 },
    { "黄瓜", 25, 30 },
    { "辣椒", 25, 30 },
    { "叶菜", 15, 22 },
    { "草莓", 20, 25 },
    { "通用", 18, 30 },
};
#define CROP_NUM   (sizeof(s_crops) / sizeof(s_crops[0]))
#define CROP_DEFAULT 5   /* 默认"通用" */

/* 右列停在气泡左边，说话时不会盖住作物名 */
#define CROP_W  ((GH_LOWER_W - 12) / 2)
#define CROP_H  74

static lv_obj_t *s_btn[CROP_NUM];
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
        "当前报警阈值  温度%d.%d~%d.%d℃  湿度%d.%d~%d.%d%%(接AHT20后生效)",
        th.temp_min_deci / 10, th.temp_min_deci % 10,
        th.temp_max_deci / 10, th.temp_max_deci % 10,
        th.humi_min_deci / 10, th.humi_min_deci % 10,
        th.humi_max_deci / 10, th.humi_max_deci % 10);
}

/* 选中态样式 */
static void crop_style_refresh(void)
{
    for (rt_size_t i = 0; i < CROP_NUM; i++)
    {
        lv_obj_set_style_bg_color(s_btn[i],
            (i == (rt_size_t)s_current) ? lv_color_hex(C_CARD_SEL) : lv_color_hex(C_CARD),
            LV_PART_MAIN | LV_STATE_DEFAULT);
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

static void crop_create(lv_obj_t *parent)
{
    lv_obj_t *tip = lv_label_create(parent);
    lv_obj_add_style(tip, gh_ui_style(18), 0);
    lv_label_set_text(tip, "点击作物，一键设置温度报警区间");
    lv_obj_set_style_text_color(tip, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(tip, 4, 0);

    for (rt_size_t i = 0; i < CROP_NUM; i++)
    {
        int col = (int)(i % 2);
        int row = (int)(i / 2);
        char sub[32];

        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(btn, CROP_W, CROP_H);
        lv_obj_set_pos(btn, col * (CROP_W + 12), 26 + row * (CROP_H + 8));
        lv_obj_set_style_radius(btn, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_CARD), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(btn, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, crop_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(btn);
        lv_obj_add_style(name, gh_ui_style(22), 0);
        lv_label_set_text(name, s_crops[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *range = lv_label_create(btn);
        lv_obj_add_style(range, gh_ui_style(18), 0);
        rt_snprintf(sub, sizeof(sub), "适温 %d~%d℃", s_crops[i].t_min, s_crops[i].t_max);
        lv_label_set_text(range, sub);
        lv_obj_set_style_text_color(range, lv_color_hex(C_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(range, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        s_btn[i] = btn;
    }

    s_thresh_label = lv_label_create(parent);
    lv_obj_add_style(s_thresh_label, gh_ui_style(18), 0);
    lv_label_set_text(s_thresh_label, "");
    lv_obj_set_style_text_color(s_thresh_label, lv_color_hex(C_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_thresh_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

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
