/*
 * PocketJS launcher for the Edgi-Talk Cortex-M55.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>
#include <board.h>

#include "pocketjs/pocketjs_app.h"
#include "pocketjs/pocketjs_wifi.h"
#include "pocketjs/pocketjs_bt.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define LCD_BL_GPIO_NUM GET_PIN(15, 7)
#define BL_PWM_DISP_CTRL GET_PIN(20, 6)

#ifndef BSP_LCD_STARTUP_STABILIZE_MS
#define BSP_LCD_STARTUP_STABILIZE_MS 1500U
#endif

static void m55_cpu_cache_enable(void)
{
#if defined(BSP_LVGL_ENABLE_CPU_CACHE) && defined(RT_USING_CACHE)
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    if (!rt_hw_cpu_icache_status())
    {
        rt_hw_cpu_icache_enable();
    }
#endif

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    if (!rt_hw_cpu_dcache_status())
    {
        rt_hw_cpu_dcache_enable();
    }
#endif

    LOG_I("M55 cache enabled: I=%d D=%d",
          rt_hw_cpu_icache_status(),
          rt_hw_cpu_dcache_status());
#endif
}

static void m55_lcd_backlight_enable(void)
{
    rt_pin_mode(LCD_BL_GPIO_NUM, PIN_MODE_OUTPUT);
    rt_pin_mode(BL_PWM_DISP_CTRL, PIN_MODE_OUTPUT);
    rt_pin_write(LCD_BL_GPIO_NUM, PIN_HIGH);
    rt_pin_write(BL_PWM_DISP_CTRL, PIN_HIGH);
}

int main(void)
{
    rt_err_t result;

    LOG_I("Edgi-Talk PocketJS for Cortex-M55");
    m55_cpu_cache_enable();
    rt_thread_mdelay(BSP_LCD_STARTUP_STABILIZE_MS);

    result = pocketjs_app_start();
    if (result != RT_EOK)
    {
        LOG_E("PocketJS task start failed: %d", result);
        return result;
    }

    result = pocketjs_app_wait_ready(rt_tick_from_millisecond(8000));
    if (result != RT_EOK)
    {
        LOG_E("PocketJS initialization failed: %d", result);
        return result;
    }

    m55_lcd_backlight_enable();
#ifndef POCKETJS_BT_ONLY
    pocketjs_wifi_start();
#endif
    pocketjs_bt_start();
    return 0;
}
