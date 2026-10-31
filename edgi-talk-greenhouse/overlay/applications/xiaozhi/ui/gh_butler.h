/*
 * gh_butler.h - 手绘"农场管家"小形象（LVGL 矢量图形 + 实时动画）
 *
 * 用 LVGL 基础对象（圆/圆角矩形/弧）画出的草帽+背带裤管家。
 * 画布 120x128，放在屏幕右下角，不占用参数区。官方 3D emoji
 * 缩到这个尺寸不渲染，全屏时又占半屏，所以这里不用它。持续动画：
 *   - 呼吸浮动（身体上下 3px，1.8s 循环）
 *   - 眨眼（约 3.2s 一次）
 *   - 说话嘴型（talking 状态下张合，随 AI 语音气泡出现/结束）
 * 表情（mood）：neutral / happy(挥手) / sad / surprised / sleepy / talking。
 *
 * 仅限 UI(xz_ui) 线程调用。
 */
#ifndef __GH_BUTLER_H__
#define __GH_BUTLER_H__

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 在 parent 内创建管家形象（120x128，在父容器里居中） */
lv_obj_t *gh_butler_create(lv_obj_t *parent);

/* 切换表情；emoji_name 为官方 18 个表情名，内部映射到 5 种 mood。
 * 传 "talking" 进入说话状态（嘴部张合动画），传其他值离开。 */
void gh_butler_set_mood(const char *emoji_name);

#ifdef __cplusplus
}
#endif

#endif /* __GH_BUTLER_H__ */
