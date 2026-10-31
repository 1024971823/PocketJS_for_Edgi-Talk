/*
 * gh_ntp.h - 菜园管家 NTP 网络校时
 *
 * 作用：开机联网后自动把系统 RTC 校准到网络时间（UTC，配合 libc 的
 * RT_LIBC_USING_LIGHT_TZ_DST +8 时区显示为北京时间），让日历页显示正确。
 * 纯后台线程实现，不阻塞 UI；状态通过 gh_ntp_get_status() 供 UI 轮询显示。
 *
 * Change Logs:
 * 2026-11-01 新增：SNTP v4 极简实现（UDP/123），多服务器轮询，自动+手动触发。
 */
#ifndef GH_NTP_H
#define GH_NTP_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 同步状态机 */
typedef enum
{
    GH_NTP_IDLE = 0,    /* 空闲/从未同步 */
    GH_NTP_RUNNING,     /* 正在同步 */
    GH_NTP_OK,          /* 上次同步成功 */
    GH_NTP_FAIL         /* 上次同步失败 */
} gh_ntp_state_t;

/* 当前状态（供 UI 轮询） */
gh_ntp_state_t gh_ntp_get_state(void);

/* 是否曾经成功同步过（true 则 RTC 可信） */
rt_bool_t gh_ntp_synced(void);

/* 触发一次异步同步（若在运行中则直接返回）。返回 RT_EOK 表示已受理。 */
rt_err_t gh_ntp_sync_async(void);

/* 模块初始化（在 wifi 可用后由 settings/日历页调用；内部幂等） */
void gh_ntp_init(void);

#ifdef __cplusplus
}
#endif

#endif /* GH_NTP_H */
