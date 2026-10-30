/*
 * greenhouse_sensor.h - 蔬菜大棚环境监测核心模块（板载 AHT20）
 *
 * 负责：周期采样、数据缓存、阈值报警状态机（带去抖）、
 *       报警红灯控制、屏幕温湿度信息推送。
 *
 * 依赖：
 *   - 无外部依赖：AHT20 用裸 I2C 驱动（参考官方 SDK sensor_monitor.c 的
 *     实测读法），上电自动探测 i2c1/i2c2/i2c0 总线，无需 menuconfig、
 *     无需 aht10 软件包。
 *   - xiaozhi_ui_set_greenhouse()：需要对 ui/xiaozhi_ui.c/.h 打的
 *     4 处小补丁（见 INTEGRATION.md）。
 *
 * 线程模型：
 *   - 内部创建 "gh_sens" 采样线程（默认 10s 周期）；
 *   - 所有对外接口线程安全（内部互斥锁）。
 */

#ifndef __GREENHOUSE_SENSOR_H__
#define __GREENHOUSE_SENSOR_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一次采样的快照。temp_dc 单位 0.1℃，hum_dpct 单位 0.1%RH */
typedef struct
{
    rt_bool_t valid;   /* 是否已成功取得过传感器数据 */
    int temp_dc;       /* 0.1℃，例如 253 = 25.3℃ */
    int hum_dpct;      /* 0.1%RH，例如 605 = 60.5% */
} gh_data_t;

/* 报警阈值（整数℃，整数 %RH） */
typedef struct
{
    int temp_min;      /* 温度下限，默认 5 */
    int temp_max;      /* 温度上限，默认 35 */
    int hum_min;       /* 湿度下限，默认 30 */
    int hum_max;       /* 湿度上限，默认 90 */
} gh_thresholds_t;

/* 初始化并启动采样线程（由 INIT_APP_EXPORT 自动执行，无需手动调用） */
int greenhouse_sensor_init(void);

/* 取当前数据快照（线程安全） */
gh_data_t greenhouse_get_data(void);

/* 是否处于报警状态（去抖后的稳态） */
rt_bool_t greenhouse_is_alarm(void);

/* 报警描述文本，例如 "正常" / "温度过高(36.2℃); 湿度偏低(25.1%)"
 * 返回内部静态缓冲区指针，调用方应立即使用/拷贝，不要长期持有 */
const char *greenhouse_get_alarm_text(void);

/* 读取/设置阈值（线程安全；设置后立即生效，掉电恢复默认值） */
gh_thresholds_t greenhouse_get_thresholds(void);
void greenhouse_set_thresholds(int temp_min, int temp_max,
                               int hum_min, int hum_max);

/* 传感器是否可用（PKG_USING_AHT10 未开启时返回 RT_FALSE） */
rt_bool_t greenhouse_sensor_available(void);

#ifdef __cplusplus
}
#endif

#endif /* __GREENHOUSE_SENSOR_H__ */
