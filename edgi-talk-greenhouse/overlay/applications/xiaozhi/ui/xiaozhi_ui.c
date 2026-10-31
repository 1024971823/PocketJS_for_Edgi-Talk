/*
 * xiaozhi_ui.c - 菜园管家 UI 引擎（多页面版）
 *
 * 职责（本文件只做"引擎"，页面内容在 gh_page_*.c）：
 *   1. LVGL 初始化 / UI 线程 / 消息队列（沿用官方 xiaozhi_ui 的线程模型，
 *      所有 LVGL 操作都发生在 xz_ui 线程，其他线程通过消息队列投递）；
 *   2. 页面管理器：主页仪表盘 / 网络 / 日历 / 作物库 四页切换；
 *   3. 跨页面常驻元素：头部(标题/小智状态/电池)、全局告警栏、
 *      右下角手绘农场管家、上层小对话气泡（几何见 gh_ui_layout.h）；
 *   4. 每秒一次数据心跳：读 sensor_monitor 快照 → 电池/活动页刷新；
 *   5. 告警事件（sensor_monitor 回调 → 消息队列）→ 告警栏 + 气泡 + 表情；
 *   6. 向下兼容官方公共 API（xiaozhi_ui.h）：xiaozhi.cpp / main.c /
 *      webnet_wifi.c 无需任何改动。
 *
 * 数据流：
 *   sensor 线程 --alert_cb--> mq(UI_CMD_ALERT) --> 告警栏+气泡+表情
 *   sensor 线程 --snapshot(互斥锁保护)--> UI 心跳定时器(1s) --> 卡片/电池
 *   xiaozhi 线程 --chat_status/chat_output/update_emoji--> mq --> 头部/气泡/表情
 *
 * Change Logs:
 * 2024-01-01     RT-Thread    官方原版（单屏 emoji 布局）
 * 2026-10-28     Greenhouse   重写为多页面菜园管家 UI（API 向下兼容）
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include <lvgl.h>
#include "xiaozhi_ui.h"
#include "gh_ui_pages.h"
#include "gh_ui_layout.h"
#include "gh_butler.h"
#include "../sensors/sensor_monitor.h"
#include "../sensors/gh_ntp.h"

/*****************************************************************************
 * 宏定义
 *****************************************************************************/
#define DBG_TAG    "xz.ui"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* 外部符号（官方字库, 免去隐式声明告警） */
extern const unsigned char xiaozhi_font[];
extern const int xiaozhi_font_size;
extern void lv_port_disp_init(void);
extern void lv_port_indev_init(void);

#define AP_INFO_DEFAULT_TEXT "SSID: RT-Thread-AP 密码: 123456789 IP:192.168.169.1"

#define UI_MSG_DATA_SIZE    200
#define UI_MSG_POOL_SIZE    12
#define UI_THREAD_STACK     (1024 * 14)
#define UI_THREAD_PRIORITY  25
#define UI_THREAD_TICK      10

/* 布局数字只在 gh_ui_layout.h。这里留旧名字，避免改下面每一处调用。 */
#define SCR_W_DEF           GH_SCR_W
#define SCR_H_DEF           GH_SCR_H
#define HDR_H               GH_HDR_H
#define BANNER_H            GH_BANNER_H
#define BODY_X              GH_BODY_X
#define BODY_Y              GH_BODY_Y
#define BODY_W              GH_BODY_W
#define BODY_H              GH_BODY_H
#define TAB_Y               GH_TAB_Y
#define TAB_H               GH_TAB_H
#define TAB_W               GH_TAB_W
#define AVA_X               GH_AVA_X
#define AVA_Y               GH_AVA_Y
#define AVA_W               GH_AVA_W
#define AVA_H               GH_AVA_H
#define BUBBLE_W            GH_BUBBLE_W
#define BUBBLE_HIDE_MS      GH_BUBBLE_HIDE_MS

/* GH_BODY 可延伸到管家列（x 至 656)：页面下排内容统一收在 GH_LOWER_W(540) 内，
 * 与管家/气泡的 y 区间错开，不遮挡。仅断言真正会相撞的两项。 */
_Static_assert(GH_BUBBLE_X >= GH_BODY_X + GH_LOWER_W, "bubble covers the lower parameter row");
_Static_assert(GH_BUBBLE_Y + GH_BUBBLE_H <= GH_AVA_Y, "bubble overlaps the butler");
_Static_assert(GH_AVA_X + GH_AVA_W <= GH_SCR_W && GH_AVA_Y + GH_AVA_H <= GH_SCR_H,
               "butler is off the 800x480 screen");

/* 配色：统一引用 gh_ui_layout.h 的设计令牌（深苔绿暗色主题） */
#define C_BG                GH_COL_BG
#define C_CARD              GH_COL_CARD
#define C_CARD_ALERT        GH_COL_ALERT_BG
#define C_TEXT              GH_COL_TEXT
#define C_TEXT_DIM          GH_COL_TEXT_DIM
#define C_ACCENT            GH_COL_ACCENT
#define C_ALERT             GH_COL_ALERT
#define C_TAB_BG            GH_COL_TAB_BG
#define C_TAB_ACTIVE        GH_COL_TAB_ACT
#define C_BUBBLE_BG         GH_COL_PANEL

/*****************************************************************************
 * 类型定义
 *****************************************************************************/
typedef enum
{
    UI_CMD_SET_STATUS = 0,      /* 头部小智状态 */
    UI_CMD_SET_OUTPUT,          /* AI 说话内容 → 气泡 */
    UI_CMD_SET_EMOJI,           /* 表情名 → 3D 头像 */
    UI_CMD_SET_ADC,             /* 兼容保留(调试面板已弃用) */
    UI_CMD_CLEAR_INFO,          /* 清除气泡 */
    UI_CMD_SHOW_AP_INFO,        /* 配网模式提示(常驻气泡) */
    UI_CMD_SHOW_CONNECTING,     /* 正在连接已保存 WiFi */
    UI_CMD_UPDATE_BATTERY,      /* 电池(旧接口) */
    UI_CMD_UPDATE_CHARGE_STATUS,/* 兼容保留 */
    UI_CMD_ALERT                /* 报警事件: data[0]='1'报警/'0'恢复 + 文本 */
} ui_cmd_t;

typedef struct
{
    ui_cmd_t cmd;
    char data[UI_MSG_DATA_SIZE];
} ui_msg_t;

/*****************************************************************************
 * 静态变量
 *****************************************************************************/
/* 同步原语 */
static struct rt_semaphore s_ui_init_sem;
static struct rt_messagequeue s_ui_mq;
static char s_mq_pool[UI_MSG_POOL_SIZE * sizeof(ui_msg_t)];

/* 字体样式（tiny_ttf, 全量 CJK 字库 xiaozhi_font） */
static lv_style_t s_st18;
static lv_style_t s_st22;
static lv_style_t s_st28;
static lv_style_t s_st44;

/* 头部 */
static lv_obj_t *s_hdr_title;
static lv_obj_t *s_label_status;
static lv_obj_t *s_battery_bar;
static lv_obj_t *s_battery_label;

/* 全局告警栏 */
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_label;
static rt_bool_t s_banner_on = RT_FALSE;

/* 页面管理 */
static lv_obj_t *s_body;                    /* 页面主体容器 */
static lv_obj_t *s_page_roots[GH_PAGE_NUM];    /* 各页根容器(懒创建) */
static const gh_page_t *s_pages[GH_PAGE_NUM];  /* 页面注册表 */
static lv_obj_t *s_tab_btns[GH_PAGE_NUM];
static int s_page_cur = -1;                 /* 当前页索引, -1=未初始化 */

/* 头像面板（跨页面常驻, 右下角） */
static lv_obj_t *s_avatar_panel;

/* 气泡（lv_layer_top 上层） */
static lv_obj_t *s_bubble;
static lv_obj_t *s_bubble_label;
static lv_timer_t *s_bubble_timer;
static rt_bool_t s_bubble_sticky = RT_FALSE;

/* 配网信息（webnet_wifi 上报，网络页展示） */
static char s_ap_info[UI_MSG_DATA_SIZE] = {0};

/* 电池显示兜底值 */
static int s_battery_level = -1;            /* -1=无数据 */

/* 这些状态短语只更新头部/无需弹气泡（避免"就绪/聆听中"刷屏） */
static const char *const s_output_ignored[] =
{
    " ", "就绪", "聆听中...", "等待唤醒",
    "正在连接...", "仍在连接中...", "重新连接中...", "请稍后再试",
};

/* 报警转发函数（sensor 线程回调 → 队列），先声明供线程入口注册 */
static void greenhouse_ui_alert_forwarder(rt_bool_t any_active, const char *text);

/*****************************************************************************
 * 气泡
 *****************************************************************************/
/* 说完回到表情：报警还在就保持惊讶，否则平静 */
static void butler_after_speech(void)
{
    gh_butler_set_mood(s_banner_on ? "surprised" : "neutral");
}

/* 气泡隐藏定时器回调（仅 UI 线程触达） */
static void bubble_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    s_bubble_sticky = RT_FALSE;
    butler_after_speech();
}

/* 气泡位置固定，不跟着文本变高，避免盖住左侧作物参数 */
static void bubble_reposition(void)
{
    lv_obj_set_pos(s_bubble, GH_BUBBLE_X, GH_BUBBLE_Y);
}

/* 内部：UI 线程内弹气泡 */
static void bubble_show_internal(const char *text, rt_bool_t sticky)
{
    if (s_bubble == RT_NULL || text == RT_NULL || text[0] == '\0')
        return;

    lv_label_set_text(s_bubble_label, text);
    s_bubble_sticky = sticky;
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bubble);
    bubble_reposition();
    if (!sticky)
        gh_butler_set_mood("talking");   /* AI 说话 → 管家嘴型动画 */

    if (sticky)
    {
        lv_timer_pause(s_bubble_timer);
    }
    else
    {
        lv_timer_reset(s_bubble_timer);
        lv_timer_resume(s_bubble_timer);
    }
}

/* 对外：其他模块(UI 线程内, 如作物页)弹气泡 */
void gh_ui_bubble_show(const char *text)
{
    bubble_show_internal(text, RT_FALSE);
}

const char *gh_ui_get_ap_info(void)
{
    return (s_ap_info[0] != '\0') ? s_ap_info : AP_INFO_DEFAULT_TEXT;
}

/* 页面共用字体样式 */
lv_style_t *gh_ui_style(int px)
{
    switch (px)
    {
    case 18: return &s_st18;
    case 22: return &s_st22;
    case 28: return &s_st28;
    case 44: return &s_st44;
    default: return RT_NULL;
    }
}

/* 判断是否为无需弹泡的状态短语 */
static rt_bool_t output_is_ignored(const char *text)
{
    for (rt_size_t i = 0; i < sizeof(s_output_ignored) / sizeof(s_output_ignored[0]); i++)
    {
        if (rt_strcmp(text, s_output_ignored[i]) == 0)
            return RT_TRUE;
    }
    return RT_FALSE;
}

/*****************************************************************************
 * 页面管理
 *****************************************************************************/
static void tab_style_refresh(void)
{
    for (int i = 0; i < GH_PAGE_NUM; i++)
    {
        rt_bool_t sel = (i == s_page_cur);
        lv_obj_set_style_bg_color(s_tab_btns[i],
            sel ? lv_color_hex(GH_COL_TAB_ACT) : lv_color_hex(GH_COL_TAB_BG),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_tab_btns[i],
            sel ? lv_color_hex(GH_COL_ACCENT) : lv_color_hex(GH_COL_DIVIDER),
            LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *lb = lv_obj_get_child(s_tab_btns[i], 0);
        if (lb)
            lv_obj_set_style_text_color(lb,
                sel ? lv_color_hex(GH_COL_ACCENT) : lv_color_hex(GH_COL_TEXT_DIM),
                LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

static void page_switch(int idx)
{
    if (idx < 0 || idx >= GH_PAGE_NUM || idx == s_page_cur)
        return;

    /* 首次进入：创建该页控件 */
    if (s_page_roots[idx] == RT_NULL)
    {
        s_page_roots[idx] = lv_obj_create(s_body);
        lv_obj_remove_flag(s_page_roots[idx], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_page_roots[idx], BODY_W, BODY_H);
        lv_obj_set_pos(s_page_roots[idx], 0, 0);
        lv_obj_set_style_bg_opa(s_page_roots[idx], LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_page_roots[idx], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(s_page_roots[idx], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        s_pages[idx]->create(s_page_roots[idx]);
    }

    if (s_page_cur >= 0)
    {
        if (s_pages[s_page_cur]->on_hide)
            s_pages[s_page_cur]->on_hide();
        lv_obj_add_flag(s_page_roots[s_page_cur], LV_OBJ_FLAG_HIDDEN);
    }

    s_page_cur = idx;
    lv_obj_remove_flag(s_page_roots[idx], LV_OBJ_FLAG_HIDDEN);
    if (s_pages[idx]->on_show)
        s_pages[idx]->on_show();
    tab_style_refresh();
}

static void tab_clicked_cb(lv_event_t *e)
{
    page_switch((int)(intptr_t)lv_event_get_user_data(e));
}

/* 底部标签按钮行 */
static void tabs_create(lv_obj_t *parent)
{
    for (int i = 0; i < GH_PAGE_NUM; i++)
    {
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(btn, GH_TAB_W, GH_TAB_H);
        lv_obj_set_pos(btn, GH_TAB_X0 + i * (GH_TAB_W + GH_TAB_GAP), GH_TAB_Y);
        lv_obj_set_style_radius(btn, GH_TAB_H / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, lv_color_hex(GH_COL_TAB_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(btn, tab_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lb = lv_label_create(btn);
        lv_obj_add_style(lb, &s_st18, 0);
        lv_label_set_text(lb, s_pages[i]->tab);
        lv_obj_set_style_text_color(lb, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_center(lb);

        s_tab_btns[i] = btn;
    }
}

/*****************************************************************************
 * 跨页面常驻元素
 *****************************************************************************/
/* 头部：左=绿点+标题 | 中=小智状态 | 右=电池条+百分比，下方 1px 分隔线 */
static void header_create(lv_obj_t *parent)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dot, lv_color_hex(GH_COL_ACCENT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(dot, GH_HDR_PAD_X, GH_HDR_H / 2 - 5);

    s_hdr_title = lv_label_create(parent);
    lv_obj_add_style(s_hdr_title, &s_st22, 0);
    lv_label_set_text(s_hdr_title, "菜园管家");
    lv_obj_set_style_text_color(s_hdr_title, lv_color_hex(GH_COL_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_pos(s_hdr_title, GH_HDR_PAD_X + 20, GH_HDR_H / 2 - 15);

    s_label_status = lv_label_create(parent);
    lv_obj_add_style(s_label_status, &s_st22, 0);
    lv_label_set_text(s_label_status, "初始化中...");
    lv_obj_set_style_text_color(s_label_status, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_label_status, LV_ALIGN_TOP_MID, 0, GH_HDR_H / 2 - 15);

    s_battery_label = lv_label_create(parent);
    lv_obj_add_style(s_battery_label, &s_st18, 0);
    lv_label_set_text(s_battery_label, "--");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(GH_COL_TEXT_DIM), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -GH_HDR_PAD_X, GH_HDR_H / 2 - 12);

    s_battery_bar = lv_bar_create(parent);
    lv_bar_set_range(s_battery_bar, 0, 100);
    lv_bar_set_value(s_battery_bar, 100, LV_ANIM_OFF);
    lv_obj_set_size(s_battery_bar, 56, 10);
    lv_obj_align(s_battery_bar, LV_ALIGN_TOP_RIGHT, -(GH_HDR_PAD_X + 44), GH_HDR_H / 2 - 4);
    lv_obj_set_style_radius(s_battery_bar, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_battery_bar, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(GH_COL_CARD_HI), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_battery_bar, lv_color_hex(GH_COL_ACCENT), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(line, GH_SCR_W - 2 * GH_HDR_PAD_X, 1);
    lv_obj_set_pos(line, GH_HDR_PAD_X, GH_HDR_H - 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(GH_COL_DIVIDER), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* 全局告警条（有报警时红色常显, 恢复后隐藏；坑位保留不跳动） */
static void banner_create(lv_obj_t *parent)
{
    s_banner = lv_obj_create(parent);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_banner, GH_SCR_W - 2 * GH_BODY_X, GH_BANNER_H - 4);
    lv_obj_set_pos(s_banner, GH_BODY_X, GH_BANNER_Y + 2);
    lv_obj_set_style_radius(s_banner, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_banner, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_banner, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(GH_COL_ALERT_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bar = lv_obj_create(s_banner);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bar, 4, GH_BANNER_H - 12);
    lv_obj_set_pos(bar, 8, 4);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(GH_COL_ALERT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_banner_label = lv_label_create(s_banner);
    lv_obj_add_style(s_banner_label, &s_st18, 0);
    lv_label_set_long_mode(s_banner_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_banner_label, GH_SCR_W - 2 * GH_BODY_X - 40);
    lv_label_set_text(s_banner_label, "");
    lv_obj_set_style_text_color(s_banner_label, lv_color_hex(0xF0B8B8), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_banner_label, LV_ALIGN_LEFT_MID, 24, 0);
}

/* 右下角小号农场管家。尺寸来自 gh_ui_layout.h，形象在 gh_butler.c。 */
static void avatar_create(lv_obj_t *parent)
{
    s_avatar_panel = lv_obj_create(parent);
    lv_obj_remove_flag(s_avatar_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_avatar_panel, AVA_W, AVA_H);
    lv_obj_set_pos(s_avatar_panel, AVA_X, AVA_Y);
    lv_obj_set_style_bg_opa(s_avatar_panel, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_avatar_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_avatar_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    gh_butler_create(s_avatar_panel);
}

/* 上层气泡：固定两行，停在管家上方，不跟着文本长高 */
static void bubble_create(void)
{
    s_bubble = lv_obj_create(lv_layer_top());
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bubble, GH_BUBBLE_W, GH_BUBBLE_H);
    lv_obj_set_pos(s_bubble, GH_BUBBLE_X, GH_BUBBLE_Y);
    lv_obj_set_style_radius(s_bubble, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(GH_COL_PANEL), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_90, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_bubble, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(GH_COL_ACCENT2), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_bubble, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    s_bubble_label = lv_label_create(s_bubble);
    lv_obj_add_style(s_bubble_label, &s_st18, 0);
    lv_label_set_long_mode(s_bubble_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_bubble_label, GH_BUBBLE_W - 12, GH_BUBBLE_H - 12);
    lv_obj_set_style_text_color(s_bubble_label, lv_color_hex(C_TEXT), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(s_bubble_label, "");

    s_bubble_timer = lv_timer_create(bubble_timer_cb, BUBBLE_HIDE_MS, RT_NULL);
    lv_timer_pause(s_bubble_timer);
}

/*****************************************************************************
 * 消息处理（UI 线程）
 *****************************************************************************/
static void ui_process_message(const ui_msg_t *msg)
{
    switch (msg->cmd)
    {
    case UI_CMD_SET_STATUS:
        lv_label_set_text(s_label_status, msg->data);
        break;

    case UI_CMD_SET_OUTPUT:
        /* AI 说话内容 → 气泡；纯状态短语不弹泡只留头部 */
        if (!output_is_ignored(msg->data))
            bubble_show_internal(msg->data, RT_FALSE);
        break;

    case UI_CMD_SET_EMOJI:
        gh_butler_set_mood(msg->data);
        break;

    case UI_CMD_SET_ADC:
        break;  /* 旧调试面板, 已弃用 */

    case UI_CMD_CLEAR_INFO:
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        s_bubble_sticky = RT_FALSE;
        lv_timer_pause(s_bubble_timer);
        butler_after_speech();
        break;

    case UI_CMD_SHOW_AP_INFO:
        rt_strncpy(s_ap_info, msg->data, sizeof(s_ap_info) - 1);
        s_ap_info[sizeof(s_ap_info) - 1] = '\0';
        lv_label_set_text(s_label_status, "配网模式");
        /* 配网信息需要停留阅读 → 常驻气泡 */
        bubble_show_internal(msg->data[0] ? msg->data : AP_INFO_DEFAULT_TEXT, RT_TRUE);
        break;

    case UI_CMD_SHOW_CONNECTING:
        lv_label_set_text(s_label_status, "连接中...");
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        s_bubble_sticky = RT_FALSE;
        lv_timer_pause(s_bubble_timer);
        butler_after_speech();
        break;

    case UI_CMD_UPDATE_BATTERY:
        s_battery_level = atoi(msg->data);
        break;

    case UI_CMD_UPDATE_CHARGE_STATUS:
        break;  /* 充电状态不单独展示, 电池条已反映 */

    case UI_CMD_ALERT:
    {
        rt_bool_t active = (msg->data[0] == '1');
        const char *text = &msg->data[1];

        if (active)
        {
            lv_label_set_text(s_banner_label, text);
            lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
            s_banner_on = RT_TRUE;
            char buf[UI_MSG_DATA_SIZE];
            rt_snprintf(buf, sizeof(buf), "报警：%s", text);
            bubble_show_internal(buf, RT_FALSE);
            gh_butler_set_mood("surprised");
        }
        else
        {
            lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
            s_banner_on = RT_FALSE;
            bubble_show_internal("环境已恢复正常", RT_FALSE);
            gh_butler_set_mood("happy");
        }
    }
    break;

    default:
        LOG_W("Unknown UI command: %d", msg->cmd);
        break;
    }
}

/*****************************************************************************
 * 心跳（UI 线程, 1s）：电池 + 活动页刷新
 *****************************************************************************/
static void ui_tick_cb(lv_timer_t *timer)
{
    edgi_sensor_snapshot_t snap;
    (void)timer;

    if (edgi_sensor_monitor_get_snapshot(&snap) == RT_EOK)
    {
        /* 电池：优先取百分比；无效读数显示 -- */
        if (snap.batt_mv > 0 && snap.battery_percent > 0)
        {
            s_battery_level = snap.battery_percent;
        }
        if (s_battery_level >= 0)
        {
            lv_bar_set_value(s_battery_bar, s_battery_level, LV_ANIM_OFF);
            lv_label_set_text_fmt(s_battery_label, "%d%%", s_battery_level);
            lv_obj_set_style_bg_color(s_battery_bar,
                (s_battery_level <= 20) ? lv_color_hex(C_ALERT) : lv_color_hex(0x00C853),
                LV_PART_INDICATOR | LV_STATE_DEFAULT);
        }
    }

    /* 堆余量遥测：tiny_ttf 全量 CJK 字形缓存是最大可疑 OOM 源，
     * 每 60s 打一次 free/max_used，若一路走低逼近 0 即可定案 */
    {
        static int s_heap_log_div = 0;
        if (++s_heap_log_div >= 60)
        {
            rt_size_t total = 0, used = 0, max_used = 0;
            s_heap_log_div = 0;
            rt_memory_info(&total, &used, &max_used);
            LOG_I("heap free=%u/%u max_used=%u",
                  (unsigned)(total - used), (unsigned)total, (unsigned)max_used);
        }
    }
    /* 网络就绪自动校时一次（gh_ntp_init 幂等，仅首次触发） */
    if (edgi_sensor_monitor_get_snapshot(&snap) == RT_EOK)
    {
        static rt_bool_t s_ntp_kicked = RT_FALSE;
        if (!s_ntp_kicked && snap.wifi_network_ready)
        {
            s_ntp_kicked = RT_TRUE;
            gh_ntp_init();
        }
    }

    if (s_page_cur >= 0 && s_pages[s_page_cur]->update)
        s_pages[s_page_cur]->update();
}

/*****************************************************************************
 * UI 线程主体
 *****************************************************************************/
static rt_err_t ui_objects_init(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_coord_t scr_w = lv_disp_get_hor_res(NULL);
    lv_coord_t scr_h = lv_disp_get_ver_res(NULL);

    if (scr_w <= 0) scr_w = SCR_W_DEF;
    if (scr_h <= 0) scr_h = SCR_H_DEF;

    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
    /* 字体样式（官方全量 CJK 字库, 4 个字号）；
     * lv_tiny_ttf_create_data 失败返回 NULL，直接挂进 style 会
     * 在渲染时断言/崩溃 → 判空兜底，缺的字号显示为空但 UI 不死 */
    {
        lv_font_t *f;
        int sizes[4] = {18, 22, 28, 44};
        lv_style_t *styles[4] = {&s_st18, &s_st22, &s_st28, &s_st44};
        int i;
        for (i = 0; i < 4; i++)
        {
            lv_style_init(styles[i]);
            f = lv_tiny_ttf_create_data(xiaozhi_font, xiaozhi_font_size, sizes[i]);
            if (f != RT_NULL)
            {
                lv_style_set_text_font(styles[i], f);
            }
            else
            {
                LOG_E("tiny_ttf %dpt create failed (heap low?), text at this size hidden", sizes[i]);
            }
        }
    }

    /* 页面注册表（新增页面在此登记） */
    s_pages[0] = gh_page_main();
    s_pages[1] = gh_page_net();
    s_pages[2] = gh_page_cal();
    s_pages[3] = gh_page_crop();
    s_pages[4] = gh_page_set();

    header_create(screen);
    banner_create(screen);
    avatar_create(screen);

    /* 页面主体容器 */
    s_body = lv_obj_create(screen);
    lv_obj_remove_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_body, BODY_W, BODY_H);
    lv_obj_set_pos(s_body, BODY_X, BODY_Y);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_body, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    bubble_create();
    tabs_create(screen);

    /* 默认进入主页 */
    page_switch(0);

    return RT_EOK;
}

static void ui_thread_entry(void *args)
{
    ui_msg_t msg;
    rt_uint32_t period_ms;
    (void)args;

    LOG_I("ui thread start");

    /* Initialize LVGL */
    lv_init();
    lv_tick_set_cb(&rt_tick_get_millisecond);
    lv_port_disp_init();
    lv_port_indev_init();

    lv_coord_t scr_w = lv_disp_get_hor_res(NULL);
    lv_coord_t scr_h = lv_disp_get_ver_res(NULL);
    LOG_I("Screen resolution: %d x %d", scr_w, scr_h);

    if (ui_objects_init() != RT_EOK)
    {
        LOG_E("UI objects init failed");
        return;
    }
    LOG_I("ui objects ready");

    /* 报警事件订阅：sensor 线程回调里只投队列, 不做 UI 操作 */
    greenhouse_set_alert_callback(greenhouse_ui_alert_forwarder);

    /* 心跳：电池 + 活动页数据刷新 */
    lv_timer_create(ui_tick_cb, 1000, RT_NULL);

    lv_task_handler();

    /* 初始化完成, 放行 main() */
    rt_sem_release(&s_ui_init_sem);
    LOG_I("UI initialized (%dx%d, greenhouse pages)", scr_w, scr_h);

    while (1)
    {
        if (rt_mq_recv(&s_ui_mq, &msg, sizeof(msg), RT_WAITING_NO) > 0)
        {
            ui_process_message(&msg);
        }

        period_ms = lv_task_handler();
        if (period_ms == LV_NO_TIMER_READY || period_ms > 100)
            period_ms = 20;

        rt_thread_mdelay(period_ms);
    }
}

/*****************************************************************************
 * 报警转发（sensor 线程上下文, 只投队列）
 *****************************************************************************/
static void greenhouse_ui_alert_forwarder(rt_bool_t any_active, const char *text)
{
    ui_msg_t msg;
    msg.cmd = UI_CMD_ALERT;
    msg.data[0] = any_active ? '1' : '0';
    rt_strncpy(&msg.data[1], text ? text : "", sizeof(msg.data) - 2);
    msg.data[sizeof(msg.data) - 1] = '\0';
    rt_mq_send(&s_ui_mq, &msg, sizeof(msg));
}

/*****************************************************************************
 * 对外公共 API（xiaozhi_ui.h 兼容层, 线程安全, 经消息队列）
 *****************************************************************************/
static void ui_send_message(ui_cmd_t cmd, const char *data, const char *default_data)
{
    ui_msg_t msg;

    msg.cmd = cmd;
    if (data != RT_NULL)
    {
        rt_strncpy(msg.data, data, sizeof(msg.data) - 1);
    }
    else if (default_data != RT_NULL)
    {
        rt_strncpy(msg.data, default_data, sizeof(msg.data) - 1);
    }
    else
    {
        msg.data[0] = '\0';
    }
    msg.data[sizeof(msg.data) - 1] = '\0';

    rt_mq_send(&s_ui_mq, &msg, sizeof(msg));
}

/* 断言兜底：默认 rt_assert_handler 是 while(1) 死循环，LVGL/lv_conf 把
 * LV_ASSERT_HANDLER 也映射到 RT_ASSERT(0) → 任何一次 lv_malloc/RT_ASSERT
 * 失败都会把该线程（多半是 xz_ui）永久自旋，表现就是「页面砖掉」。
 * 改成只打印定位信息并让线程继续走，把死机变成可查的错误日志。
 * 注意：继续执行可能在 NULL/坏数据后再次崩，但日志先落地，便于定位真因。*/
static void ui_assert_hook(const char *ex, const char *func, rt_size_t line)
{
    rt_kprintf("\n[ASSERT-SURVIVE] (%s) at %s:%d, thread=%s — continuing\n",
               ex, func, line,
               rt_thread_self() ? rt_thread_self()->parent.name : "?");
    /* 顺手打印堆余量：若此刻堆已见底，基本可定 OOM */
    {
        rt_size_t total = 0, used = 0, max_used = 0;
        rt_memory_info(&total, &used, &max_used);
        rt_kprintf("[ASSERT-SURVIVE] heap total=%u used=%u free=%u max_used=%u\n",
                   (unsigned)total, (unsigned)used,
                   (unsigned)(total - used), (unsigned)max_used);
    }
}

void xiaozhi_ui_init(void)
{
    rt_thread_t tid;

    rt_assert_set_hook(ui_assert_hook);

    rt_sem_init(&s_ui_init_sem, "ui_sem", 0, RT_IPC_FLAG_PRIO);
    rt_mq_init(&s_ui_mq, "ui_mq", s_mq_pool, sizeof(ui_msg_t),
               sizeof(s_mq_pool), RT_IPC_FLAG_FIFO);

    tid = rt_thread_create("xz_ui", ui_thread_entry, RT_NULL,
                           UI_THREAD_STACK, UI_THREAD_PRIORITY, UI_THREAD_TICK);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }
    else
    {
        LOG_E("Create UI thread failed");
    }
}

rt_err_t xiaozhi_ui_wait_ready(rt_int32_t timeout)
{
    return rt_sem_take(&s_ui_init_sem, timeout);
}

void xiaozhi_ui_set_status(const char *status)
{
    ui_send_message(UI_CMD_SET_STATUS, status, "");
}

void xiaozhi_ui_set_output(const char *output)
{
    ui_send_message(UI_CMD_SET_OUTPUT, output, "");
}

void xiaozhi_ui_set_emoji(const char *emoji)
{
    ui_send_message(UI_CMD_SET_EMOJI, emoji, "neutral");
}

void xiaozhi_ui_set_adc(const char *adc_str)
{
    (void)adc_str;
}

void xiaozhi_ui_clear_info(void)
{
    ui_send_message(UI_CMD_CLEAR_INFO, RT_NULL, RT_NULL);
}

void xiaozhi_ui_show_ap_config(const char *ap_info)
{
    ui_send_message(UI_CMD_SHOW_AP_INFO, ap_info, AP_INFO_DEFAULT_TEXT);
}

void xiaozhi_ui_show_connecting(void)
{
    ui_send_message(UI_CMD_SHOW_CONNECTING, RT_NULL, RT_NULL);
}

void xiaozhi_ui_update_battery(int level)
{
    char battery_str[16];
    rt_snprintf(battery_str, sizeof(battery_str), "%d", level);
    ui_send_message(UI_CMD_UPDATE_BATTERY, battery_str, "100");
}

void xiaozhi_ui_update_charging_status(bool is_charging)
{
    ui_send_message(UI_CMD_UPDATE_CHARGE_STATUS,
                    is_charging ? "charging" : "discharging", "discharging");
}

void xiaozhi_ui_update_ble_status(bool connected)
{
    (void)connected;    /* 本板无蓝牙 */
}

/*****************************************************************************
 * 旧版 API 兼容（xiaozhi.cpp 等仍在使用, 保持函数名不变）
 *****************************************************************************/
void init_ui(void)
{
    xiaozhi_ui_init();
}

rt_err_t wait_ui_ready(rt_int32_t timeout)
{
    return xiaozhi_ui_wait_ready(timeout);
}

void clean_info(void)
{
    xiaozhi_ui_clear_info();
}

void xiaozhi_ui_chat_status(char *string)
{
    xiaozhi_ui_set_status(string);
}

void xiaozhi_ui_chat_output(char *string)
{
    xiaozhi_ui_set_output(string);
}

void xiaozhi_ui_update_emoji(char *string)
{
    xiaozhi_ui_set_emoji(string);
}

void xiaozhi_ui_update_adc(char *string)
{
    xiaozhi_ui_set_adc(string);
}
