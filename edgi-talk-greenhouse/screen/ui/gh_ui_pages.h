/*
 * gh_ui_pages.h - 菜园管家多页面 UI：页面插件接口
 *
 * 页面框架（engine 在 xiaozhi_ui.c）：
 *   几何尺寸在 gh_ui_layout.h，不要在页面里再写一套 800x480。
 *   气泡挂在 lv_layer_top()，固定 200x52，停在管家正上方，
 *   约 4 秒无新内容自动消失（配网信息常驻）。下排参数宽度
 *   用 GH_LOWER_W，停在气泡左边。管家是 gh_butler.c 的矢量小人。
 *
 * 新增页面步骤：
 *   1. 新建 ui/gh_page_xxx.c，实现 gh_page_t 的 create/update（可选
 *      on_show/on_hide），并提供 const gh_page_t *gh_page_xxx(void);
 *   2. 在 xiaozhi_ui.c 的 s_pages[] 注册表中加一行。
 *   create 只在首次切到该页时调用一次；update 每秒调用（仅活动页）；
 *   所有页面代码都运行在 LVGL 线程（xz_ui）上下文，直接操作 LVGL 安全。
 */
#ifndef __GH_UI_PAGES_H__
#define __GH_UI_PAGES_H__

#include <rtthread.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 页面描述符：引擎据此创建/刷新页面 */
typedef struct gh_page
{
    const char *tab;              /* 标签按钮文字 */
    void (*create)(lv_obj_t *parent);  /* 首次进入时创建控件(仅一次) */
    void (*update)(void);         /* 活动页每秒刷新一次(可为 NULL) */
    void (*on_show)(void);        /* 每次切入时回调(可为 NULL) */
    void (*on_hide)(void);        /* 每次切出时回调(可为 NULL) */
} gh_page_t;

/* 四个内置页面（各由 gh_page_*.c 提供） */
const gh_page_t *gh_page_main(void);
const gh_page_t *gh_page_net(void);
const gh_page_t *gh_page_cal(void);
const gh_page_t *gh_page_crop(void);

/* ---- 引擎提供的服务（实现在 xiaozhi_ui.c） ---- */

/* 取引擎预建的字体样式（px: 18/22/28/44，其余返回 NULL）。
 * 仅限 UI(xz_ui) 线程使用。 */
lv_style_t *gh_ui_style(int px);

/* 弹出对话气泡（非常驻，约 4 秒无新内容自动消失），线程安全 */
void gh_ui_bubble_show(const char *text);

/* 配网 AP 信息（webnet_wifi.c 上报，可能为空串），网络页展示用 */
const char *gh_ui_get_ap_info(void);

/* 当前作物名与其适宜温度区间文本（"适温22~28℃"），作物页写、气泡/日志用 */
const char *gh_crop_get_current(char *range_buf, int range_len);

#ifdef __cplusplus
}
#endif

#endif /* __GH_UI_PAGES_H__ */
