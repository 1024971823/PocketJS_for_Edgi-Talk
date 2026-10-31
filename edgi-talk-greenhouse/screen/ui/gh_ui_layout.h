/*
 * gh_ui_layout.h - 菜园管家横屏设计系统（800x480，全部页面共用）
 *
 * 布局骨架（改动只改这里，页面里不要再写裸坐标）：
 *
 *   y=0                       y=52              y=80
 *   ┌───────────────────────────────────────────────────┐
 *   │ GH_HDR  菜园管家 · 状态 · 电池                     │  顶栏
 *   ├───────────────────────────────────────────────────┤
 *   │ GH_BANNER 全局告警条（无报警隐藏，坑位保留不跳动） │
 *   ├──────────────────────────────────────────┬────────┤
 *   │  GH_BODY 页面主体 636x350                │  气泡   │ y=GH_BUBBLE_Y
 *   │  主页：4 张环境卡 + 土壤/光照/CO2 参数行  │ 208x56  │
 *   │  网络 / 日历 / 作物 / 设置               ├────────┤
 *   │  参数区右缘停在 GH_BUBBLE_X 左侧          │ 管家    │ 144x164
 *   ├──────────────────────────────────────────┴────────┤
 *   │  GH_TAB  [主页][网络][日历][作物][设置]            │  y=GH_TAB_Y
 *   └───────────────────────────────────────────────────┘
 *
 * 管家是 gh_butler.c 画的 LVGL 矢量小人，固定在右下角，不盖参数。
 * 气泡挂在 lv_layer_top()，约 4 秒无新内容自动隐藏；配网信息常驻。
 * 土壤 / 光照 / CO2 板上暂无传感器，页面统一显示「未接」，禁止仿真值。
 */
#ifndef __GH_UI_LAYOUT_H__
#define __GH_UI_LAYOUT_H__

/* ---- 屏幕 ---- */
#define GH_SCR_W            800
#define GH_SCR_H            480

/* ---- 设计令牌：配色（深苔绿暗色主题） ---- */
#define GH_COL_BG          0x0A0D10   /* 全局底 */
#define GH_COL_PANEL       0x131A1F   /* 次级面板 */
#define GH_COL_CARD        0x182126   /* 卡片底 */
#define GH_COL_CARD_HI     0x1F2B33   /* 卡片描边/悬停 */
#define GH_COL_TEXT        0xEDF2F0   /* 主文字 */
#define GH_COL_TEXT_DIM    0x7E8B90   /* 次文字 */
#define GH_COL_ACCENT      0x3ECF8E   /* 翠绿：主数值/正常/选中 */
#define GH_COL_ACCENT2     0x2BA98C   /* 次绿：辅助强调 */
#define GH_COL_WARN        0xE4B33C   /* 黄：注意 */
#define GH_COL_ALERT       0xE85D5D   /* 红：报警 */
#define GH_COL_ALERT_BG    0x2E1417   /* 报警卡片底 */
#define GH_COL_TAB_BG      0x151C21   /* 标签底 */
#define GH_COL_TAB_ACT     0x1E4636   /* 标签选中 */
#define GH_COL_DIVIDER     0x223037   /* 分隔线 */

/* ---- 顶栏 ---- */
#define GH_HDR_H            52
#define GH_HDR_PAD_X        20

/* ---- 告警条（顶栏下方整宽，占坑不跳动） ---- */
#define GH_BANNER_H         28
#define GH_BANNER_Y         (GH_HDR_H)

/* ---- 页面主体 ---- */
#define GH_BODY_X           20
#define GH_BODY_Y           (GH_BANNER_Y + GH_BANNER_H)   /* 80 */
#define GH_BODY_W           636
#define GH_BODY_H           350

/* ---- 右下角管家 ---- */
#define GH_AVA_W            144
#define GH_AVA_H            164
#define GH_AVA_X            (GH_SCR_W - 20 - GH_AVA_W)          /* 636 */
#define GH_AVA_Y            (GH_TAB_Y - 8 - GH_AVA_H)           /* 258 见下 GH_TAB_Y */

/* ---- 气泡（管家正上方，固定两行，不盖参数行） ---- */
#define GH_BUBBLE_W         208
#define GH_BUBBLE_H         56
#define GH_BUBBLE_X         (GH_SCR_W - 20 - GH_BUBBLE_W)       /* 572 */
#define GH_BUBBLE_Y         (GH_AVA_Y - GH_BUBBLE_H - 6)        /* 196 */
#define GH_BUBBLE_HIDE_MS   4000

/* ---- 底部标签 ---- */
#define GH_TAB_Y            428
#define GH_TAB_H            40
#define GH_TAB_W            140
#define GH_TAB_GAP          12
#define GH_TAB_X0           GH_BODY_X

/* ---- 下排参数区右缘（停在气泡左边） ---- */
#define GH_LOWER_W          (GH_BUBBLE_X - GH_BODY_X - 12)      /* 540 */

#define GH_PAGE_NUM         5

#endif /* __GH_UI_LAYOUT_H__ */
