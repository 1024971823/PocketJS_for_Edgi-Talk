/*
 * gh_ui_layout.h - 菜园管家横屏布局契约（800x480）
 *
 * 所有页面共用这一张图，改尺寸只改这里：
 *
 *   y=0                         y=42        y=76
 *   ┌──────────────────────────────────────────────┐
 *   │ 菜园管家          小智状态            电池   │ 头部
 *   ├──────────────────────────────────────────────┤
 *   │ 告警栏（无报警时隐藏，坑位仍留着，避免跳动） │
 *   ├────────────────────────────────────┬─────────┤
 *   │ 页面主体 GH_BODY_W                 │         │
 *   │ 主页：温度/湿度/作物/阈值/提醒      │  气泡   │ y=GH_BUBBLE_Y
 *   │ 网络 / 日历 / 作物库               │ 200x52  │ 只盖右缘，不盖参数
 *   │                                    ├─────────┤
 *   │ 下排参数停在 GH_BUBBLE_X 左侧       │ 管家    │ 136x150，右下角
 *   ├────────────────────────────────────┴─────────┤
 *   │ [主页] [网络] [日历] [作物]                  │ y=GH_TAB_Y
 *   └──────────────────────────────────────────────┘
 *
 * 气泡挂在 lv_layer_top()，不说话就隐藏。管家是 LVGL 矢量小人，
 * 不是官方 3D emoji（那个缩进小容器后不渲染，全屏时又占半屏）。
 *
 * 土壤 / 光照 / CO2 板上没有传感器：界面写「未接」，禁止填仿真值。
 */
#ifndef __GH_UI_LAYOUT_H__
#define __GH_UI_LAYOUT_H__

#define GH_SCR_W            800
#define GH_SCR_H            480

#define GH_HDR_H            42
#define GH_BANNER_H         28
#define GH_BODY_X           12
#define GH_BODY_Y           76      /* 头部 + 告警坑位 */

#define GH_TAB_Y            422
#define GH_TAB_H            50
#define GH_TAB_W            108

#define GH_AVA_W            136
#define GH_AVA_H            150
#define GH_AVA_X            (GH_SCR_W - 8 - GH_AVA_W)          /* 656 */
#define GH_AVA_Y            (GH_SCR_H - 8 - GH_AVA_H)          /* 322 */

#define GH_BODY_W           (GH_AVA_X - GH_BODY_X - 8)        /* 636 */
#define GH_BODY_H           (GH_TAB_Y - GH_BODY_Y - 8)        /* 338 */

/* 气泡固定在管家正上方，窄而矮，两行截断 */
#define GH_BUBBLE_W         200
#define GH_BUBBLE_H         52
#define GH_BUBBLE_X         (GH_SCR_W - 12 - GH_BUBBLE_W)     /* 588 */
#define GH_BUBBLE_Y         (GH_AVA_Y - GH_BUBBLE_H - 8)      /* 262 */
#define GH_BUBBLE_HIDE_MS   4000

/* 主页下排（提醒 + 未接传感器）必须停在气泡左边 */
#define GH_LOWER_W          (GH_BUBBLE_X - GH_BODY_X - 12)    /* 564 */

#define GH_PAGE_NUM         4

#endif /* __GH_UI_LAYOUT_H__ */
