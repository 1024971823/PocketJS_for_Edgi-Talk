/*
 * gh_butler.c - 手绘农场管家（LVGL 矢量，右下角）
 *
 * 设计稿 128x140，放进 144x164 的角标里，居中留呼吸边距。
 * 官方 3D emoji 缩进这个尺寸不渲染，全屏又占半块屏，所以自己画。
 *
 * 持续在动：呼吸浮动、眨眼、说话时嘴型张合、高兴时挥手。
 * 配色跟随 gh_ui_layout.h 的深苔绿主题，只做点缀暖色。
 * 只允许 xz_ui 线程调用。
 *
 * Change Logs:
 * 2026-10-28     Greenhouse     First version
 * 2026-11-01     Greenhouse     重绘：更干净的比例与新配色
 */

#include <rtthread.h>
#include <stdlib.h>
#include "gh_butler.h"

#define B_W             128
#define B_H             140

/* 配色：暖黄草帽 + 肤色 + 翠绿背带 + 深底 */
#define C_HAT           0xE7C66B
#define C_HAT_DK        0xC9A44A
#define C_HAT_BAND      0x7A5A2C
#define C_SKIN          0xF6CFA9
#define C_SKIN_SHADE    0xE9B48A
#define C_EYE           0x2B2620
#define C_CHEEK         0xF09B8B
#define C_SHIRT         0x77BFA4
#define C_OVERALL       0x3E8C6E
#define C_OVERALL_DK    0x2F6E55
#define C_BTN           0xE7C66B
#define C_SHADOW        0x000000
#define C_MOUTH         0x8C5340

#define HEAD_CX         64
#define HEAD_CY         52
#define EYE_CY          48
#define MOUTH_CY        62

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *grp;
    lv_obj_t *eye_l, *eye_r;
    lv_obj_t *m_smile, *m_sad, *m_open;
    lv_obj_t *arm_l, *arm_r;
    lv_timer_t *blink_timer;
    lv_timer_t *talk_timer;
    rt_bool_t talking;
} gh_butler_t;

static gh_butler_t s_butler;

static lv_obj_t *make_circle(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy,
                             lv_coord_t d, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    return o;
}

static lv_obj_t *make_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h, lv_coord_t radius,
                           uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(o, x, y);
    return o;
}

/* 圆弧形嘴：可见笔画是 indicator，用 bg 角度裁剪出一段弧 */
static lv_obj_t *make_mouth_arc(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy,
                                lv_coord_t d, uint32_t color)
{
    lv_obj_t *o = lv_arc_create(parent);
    lv_arc_set_rotation(o, 0);
    lv_arc_set_bg_angles(o, 0, 360);
    lv_arc_set_range(o, 0, 360);
    lv_arc_set_value(o, 360);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_remove_style(o, NULL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_style(o, NULL, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_remove_style(o, NULL, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(o, lv_color_hex(color), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(o, 3, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(o, true, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void bob_anim_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void bob_start(lv_obj_t *grp)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, grp);
    lv_anim_set_exec_cb(&a, bob_anim_cb);
    lv_anim_set_values(&a, 0, -3);
    lv_anim_set_time(&a, 1000);
    lv_anim_set_playback_time(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void eye_blink_cb(void *var, int32_t v)
{
    lv_obj_t *eye = (lv_obj_t *)var;
    lv_obj_set_height(eye, v);
    lv_obj_set_y(eye, EYE_CY - v / 2);
}

static void blink_once(lv_timer_t *timer)
{
    lv_anim_t a;
    (void)timer;
    if (s_butler.talking)
        return;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_butler.eye_l);
    lv_anim_set_exec_cb(&a, eye_blink_cb);
    lv_anim_set_values(&a, 9, 2);
    lv_anim_set_time(&a, 90);
    lv_anim_set_playback_time(&a, 90);
    lv_anim_start(&a);
    lv_anim_set_var(&a, s_butler.eye_r);
    lv_anim_start(&a);
}

static void talk_toggle_cb(lv_timer_t *timer)
{
    static rt_bool_t open = RT_FALSE;
    lv_coord_t h;
    (void)timer;
    open = !open;
    h = open ? 11 : 4;
    lv_obj_set_height(s_butler.m_open, h);
    lv_obj_set_y(s_butler.m_open, MOUTH_CY - h / 2);
}

static void wave_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)var, v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void wave_once(void)
{
    lv_anim_t a;
    lv_obj_set_style_transform_pivot_x(s_butler.arm_r, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_pivot_y(s_butler.arm_r, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_butler.arm_r);
    lv_anim_set_exec_cb(&a, wave_anim_cb);
    lv_anim_set_values(&a, 0, -560);    /* 0.1°单位: -56° */
    lv_anim_set_time(&a, 240);
    lv_anim_set_playback_time(&a, 240);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_start(&a);
}

static void mouth_show(lv_obj_t *m)
{
    lv_obj_remove_flag(s_butler.m_smile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_butler.m_sad, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_butler.m_open, LV_OBJ_FLAG_HIDDEN);
    if (m != s_butler.m_smile) lv_obj_add_flag(s_butler.m_smile, LV_OBJ_FLAG_HIDDEN);
    if (m != s_butler.m_sad)   lv_obj_add_flag(s_butler.m_sad, LV_OBJ_FLAG_HIDDEN);
    if (m != s_butler.m_open)  lv_obj_add_flag(s_butler.m_open, LV_OBJ_FLAG_HIDDEN);
}

static void eyes_set_height(lv_coord_t h)
{
    lv_obj_set_height(s_butler.eye_l, h);
    lv_obj_set_height(s_butler.eye_r, h);
    lv_obj_set_y(s_butler.eye_l, EYE_CY - h / 2);
    lv_obj_set_y(s_butler.eye_r, EYE_CY - h / 2);
}

/* 官方 18 种表情映射到这个角色能做的情绪 */
static const char *map_mood(const char *emoji)
{
    static const char *const happies[] =
    {
        "happy", "laughing", "funny", "loving", "winking", "cool",
        "relaxed", "delicious", "kissy", "confident"
    };
    static const char *const sads[] = { "sad", "angry", "crying" };
    rt_size_t i;

    if (emoji == RT_NULL)
        return "neutral";
    if (rt_strcmp(emoji, "talking") == 0 || rt_strcmp(emoji, "surprised") == 0 ||
        rt_strcmp(emoji, "shocked") == 0 || rt_strcmp(emoji, "sleepy") == 0 ||
        rt_strcmp(emoji, "neutral") == 0)
        return emoji;
    for (i = 0; i < sizeof(happies) / sizeof(happies[0]); i++)
        if (rt_strcmp(emoji, happies[i]) == 0) return "happy";
    for (i = 0; i < sizeof(sads) / sizeof(sads[0]); i++)
        if (rt_strcmp(emoji, sads[i]) == 0) return "sad";
    return "neutral";
}

void gh_butler_set_mood(const char *emoji_name)
{
    if (s_butler.root == RT_NULL)
        return;

    if (emoji_name == RT_NULL || rt_strcmp(emoji_name, "talking") != 0)
        emoji_name = map_mood(emoji_name);

    if (s_butler.talking && (emoji_name == RT_NULL || rt_strcmp(emoji_name, "talking") != 0))
    {
        s_butler.talking = RT_FALSE;
        if (s_butler.talk_timer)
            lv_timer_pause(s_butler.talk_timer);
    }

    if (emoji_name == RT_NULL) emoji_name = "neutral";

    if (rt_strcmp(emoji_name, "talking") == 0)
    {
        s_butler.talking = RT_TRUE;
        mouth_show(s_butler.m_open);
        if (s_butler.talk_timer)
            lv_timer_resume(s_butler.talk_timer);
    }
    else if (rt_strcmp(emoji_name, "happy") == 0)
    {
        mouth_show(s_butler.m_smile);
        eyes_set_height(9);
        wave_once();
    }
    else if (rt_strcmp(emoji_name, "sad") == 0)
    {
        mouth_show(s_butler.m_sad);
        eyes_set_height(6);
    }
    else if (rt_strcmp(emoji_name, "surprised") == 0)
    {
        mouth_show(s_butler.m_open);
        eyes_set_height(11);
    }
    else if (rt_strcmp(emoji_name, "sleepy") == 0)
    {
        mouth_show(s_butler.m_smile);
        eyes_set_height(3);
    }
    else
    {
        mouth_show(s_butler.m_smile);
        eyes_set_height(9);
    }
}

lv_obj_t *gh_butler_create(lv_obj_t *parent)
{
    lv_memset(&s_butler, 0, sizeof(s_butler));

    s_butler.root = lv_obj_create(parent);
    lv_obj_remove_flag(s_butler.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_butler.root, B_W, B_H);
    lv_obj_center(s_butler.root);
    lv_obj_set_style_bg_opa(s_butler.root, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_butler.root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_butler.root, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_butler.grp = lv_obj_create(s_butler.root);
    lv_obj_remove_flag(s_butler.grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_butler.grp, B_W, B_H);
    lv_obj_set_pos(s_butler.grp, 0, 0);
    lv_obj_set_style_bg_opa(s_butler.grp, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_butler.grp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_butler.grp, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* 落地软阴影 */
    make_rect(s_butler.grp, 26, 126, 76, 10, 5, C_SHADOW, LV_OPA_30);

    /* 手臂（右臂要挥手，单独保存） */
    s_butler.arm_l = make_rect(s_butler.grp, 16, 84, 12, 36, 6, C_SHIRT, LV_OPA_COVER);
    s_butler.arm_r = make_rect(s_butler.grp, 100, 84, 12, 36, 6, C_SHIRT, LV_OPA_COVER);

    /* 衬衫 + 背带裤（背带 + 扣子） */
    make_rect(s_butler.grp, 30, 78, 68, 54, 16, C_SHIRT, LV_OPA_COVER);
    make_rect(s_butler.grp, 30, 98, 68, 34, 10, C_OVERALL, LV_OPA_COVER);
    make_rect(s_butler.grp, 30, 98, 68, 10, 4, C_OVERALL_DK, LV_OPA_COVER);
    make_rect(s_butler.grp, 47, 80, 9, 24, 3, C_OVERALL, LV_OPA_COVER);
    make_rect(s_butler.grp, 72, 80, 9, 24, 3, C_OVERALL, LV_OPA_COVER);
    make_circle(s_butler.grp, 51, 100, 7, C_BTN, LV_OPA_COVER);
    make_circle(s_butler.grp, 77, 100, 7, C_BTN, LV_OPA_COVER);

    /* 头 */
    make_circle(s_butler.grp, HEAD_CX, HEAD_CY, 58, C_SKIN, LV_OPA_COVER);
    /* 耳朵 */
    make_circle(s_butler.grp, HEAD_CX - 27, HEAD_CY + 2, 9, C_SKIN_SHADE, LV_OPA_COVER);
    make_circle(s_butler.grp, HEAD_CX + 27, HEAD_CY + 2, 9, C_SKIN_SHADE, LV_OPA_COVER);

    /* 草帽：帽檐 + 帽冠 + 束带 */
    make_rect(s_butler.grp, 22, 27, 84, 11, 5, C_HAT_DK, LV_OPA_COVER);   /* 檐 */
    make_rect(s_butler.grp, 24, 25, 80, 11, 5, C_HAT, LV_OPA_COVER);     /* 檐亮面 */
    make_rect(s_butler.grp, 40, 6, 48, 24, 10, C_HAT, LV_OPA_COVER);     /* 帽冠 */
    make_rect(s_butler.grp, 40, 22, 48, 6, 3, C_HAT_BAND, LV_OPA_COVER); /* 束带 */

    /* 眼睛 + 腮红 */
    s_butler.eye_l = make_circle(s_butler.grp, HEAD_CX - 11, EYE_CY, 9, C_EYE, LV_OPA_COVER);
    s_butler.eye_r = make_circle(s_butler.grp, HEAD_CX + 11, EYE_CY, 9, C_EYE, LV_OPA_COVER);
    make_circle(s_butler.grp, HEAD_CX - 19, 58, 7, C_CHEEK, LV_OPA_60);
    make_circle(s_butler.grp, HEAD_CX + 19, 58, 7, C_CHEEK, LV_OPA_60);

    /* 嘴：微笑弧 / 难过弧 / 说话圆 */
    s_butler.m_smile = make_mouth_arc(s_butler.grp, HEAD_CX, 55, 24, C_MOUTH);
    lv_arc_set_bg_angles(s_butler.m_smile, 30, 150);
    s_butler.m_sad = make_mouth_arc(s_butler.grp, HEAD_CX, 66, 24, C_MOUTH);
    lv_arc_set_bg_angles(s_butler.m_sad, 210, 330);
    s_butler.m_open = make_circle(s_butler.grp, HEAD_CX, MOUTH_CY, 9, C_MOUTH, LV_OPA_COVER);
    mouth_show(s_butler.m_smile);

    bob_start(s_butler.grp);
    s_butler.blink_timer = lv_timer_create(blink_once, 3400, RT_NULL);
    s_butler.talk_timer = lv_timer_create(talk_toggle_cb, 170, RT_NULL);
    lv_timer_pause(s_butler.talk_timer);

    return s_butler.root;
}
