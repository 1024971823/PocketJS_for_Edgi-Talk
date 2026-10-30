/*
 * greenhouse_sensor.cpp - 蔬菜大棚环境监测核心模块（板载 AHT20）
 *
 * 数据链路：
 *   AHT20 --(裸 I2C, 自动探测 i2c1/i2c2/i2c0)--> 采样线程 gh_sens
 *       --缓存+阈值去抖-->
 *   ├─ Thing/MCP 查询接口（greenhouse_thing.cpp / greenhouse_mcp.cpp）
 *   ├─ 屏幕温湿度标签（xiaozhi_ui_set_greenhouse，见 INTEGRATION.md）
 *   └─ 报警红灯（P16_7，与 iot/led.cpp 的 Led Thing 共用引脚，
 *      报警时常亮、恢复后熄灭；语音控制红灯会被下一次采样覆盖）
 *
 * AHT20 读法参考官方 SDK 的
 * projects/Edgi_Talk_M55_GitHub_Profile/applications/xiaozhi/sensors/sensor_monitor.c
 * （0x38 地址、init 0xBE、trigger 0xAC、85ms、20bit 原始值换算），不依赖
 * aht10 软件包——不同批次/拓展板的 AHT20 所在 I2C 总线可能不同，因此做
 * 上电自动探测，探测顺序 i2c1 → i2c2 → i2c0。
 *
 * Change Logs:
 * 2026-10-26     Greenhouse     First version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "greenhouse_sensor.h"

#define DBG_TAG    "greenhouse"
#define DBG_LVL    DBG_INFO
#include <rtdbg.h>

/* ============================ 可调配置 ============================ */

/* 采样周期（毫秒） */
#ifndef GH_SAMPLE_PERIOD_MS
#define GH_SAMPLE_PERIOD_MS      10000
#endif

/* 报警去抖：连续 N 次采样越界才置位报警，连续 N 次恢复才解除 */
#ifndef GH_ALARM_DEBOUNCE
#define GH_ALARM_DEBOUNCE        3
#endif

/* 报警红灯引脚（P16_7，红），引脚号来源与 iot/led.cpp 保持一致 */
#include "drv_gpio.h"
#define GH_ALARM_LED_PIN         GET_PIN(16, 7)

/* ============================ AHT20 裸 I2C ============================ */

#define AHT20_I2C_ADDRESS        0x38U
#define AHT20_CMD_INIT           0xBEU
#define AHT20_CMD_TRIGGER        0xACU
#define AHT20_MEASURE_DELAY_MS   85

/* 探测顺序：i2c1(BSP AHT20 默认) → i2c2 → i2c0(ES8388/IMU 共线, I2C 总线可共享) */
static const char *const s_bus_candidates[] = {"i2c1", "i2c2", "i2c0"};

static struct rt_i2c_bus_device *s_aht_bus = RT_NULL;
static rt_bool_t s_aht_found = RT_FALSE;

static rt_err_t gh_i2c_write(struct rt_i2c_bus_device *bus, rt_uint8_t addr,
                             const rt_uint8_t *data, rt_uint16_t len)
{
    struct rt_i2c_msg msg;
    msg.addr = addr;
    msg.flags = RT_I2C_WR;
    msg.buf = (rt_uint8_t *)data;
    msg.len = len;
    return (rt_i2c_transfer(bus, &msg, 1) == 1) ? RT_EOK : -RT_ERROR;
}

/* 探测：向候选总线发 AHT20 init 命令，ACK 即认为传感器在该总线 */
static void gh_aht20_probe(void)
{
    rt_uint8_t init_cmd[] = {AHT20_CMD_INIT, 0x08U, 0x00U};
    struct rt_i2c_bus_device *bus;

    s_aht_bus = RT_NULL;
    for (rt_size_t i = 0; i < sizeof(s_bus_candidates) / sizeof(s_bus_candidates[0]); i++)
    {
        bus = (struct rt_i2c_bus_device *)rt_device_find(s_bus_candidates[i]);
        if (bus == RT_NULL)
            continue;
        if (gh_i2c_write(bus, AHT20_I2C_ADDRESS, init_cmd, sizeof(init_cmd)) == RT_EOK)
        {
            s_aht_bus = bus;
            s_aht_found = RT_TRUE;
            LOG_I("AHT20 found on bus '%s'", s_bus_candidates[i]);
            rt_thread_mdelay(10);
            return;
        }
    }
    LOG_W("AHT20 not found on i2c1/i2c2/i2c0 (not soldered or on expansion board?)");
}

/* 触发一次测量并读取。成功输出 0.1℃ / 0.1%RH。读失败时重置总线，下轮重新探测。 */
static rt_err_t gh_aht20_read(int *temp_dc, int *hum_dpct)
{
    rt_uint8_t trigger[] = {AHT20_CMD_TRIGGER, 0x33U, 0x00U};
    rt_uint8_t data[6];
    rt_uint32_t hum_raw, temp_raw;
    struct rt_i2c_msg msg;

    if (s_aht_bus == RT_NULL)
        return -RT_ERROR;

    if (gh_i2c_write(s_aht_bus, AHT20_I2C_ADDRESS, trigger, sizeof(trigger)) != RT_EOK)
        goto lost;

    rt_thread_mdelay(AHT20_MEASURE_DELAY_MS);

    msg.addr = AHT20_I2C_ADDRESS;
    msg.flags = RT_I2C_RD;
    msg.buf = data;
    msg.len = sizeof(data);
    if (rt_i2c_transfer(s_aht_bus, &msg, 1) != 1)
        goto lost;

    /* bit7=busy, bit3=calibrated */
    if (((data[0] & 0x80U) != 0U) || ((data[0] & 0x68U) != 0x08U))
        goto lost;

    hum_raw   = ((rt_uint32_t)data[1] << 12) | ((rt_uint32_t)data[2] << 4) | (data[3] >> 4);
    temp_raw  = ((rt_uint32_t)(data[3] & 0x0FU) << 16) | ((rt_uint32_t)data[4] << 8) | data[5];
    *hum_dpct = (int)((hum_raw * 1000U) >> 20);
    *temp_dc  = (int)(((temp_raw * 2000U) >> 20) - 500);
    return RT_EOK;

lost:
    s_aht_bus = RT_NULL;   /* 下轮采样自动重新探测 */
    return -RT_ERROR;
}

/* ============================ 内部状态 ============================ */

static struct rt_mutex s_lock;
static rt_bool_t s_inited = RT_FALSE;

static gh_data_t       s_data;             /* 最新数据快照 */
static gh_thresholds_t s_thresholds;       /* 当前阈值 */
static rt_bool_t       s_alarm = RT_FALSE; /* 去抖后的报警稳态 */
static char            s_alarm_text[96];   /* 报警描述 */

static rt_int32_t s_over_cnt  = 0;         /* 连续越界计数 */
static rt_int32_t s_ok_cnt    = 0;         /* 连续正常计数 */

/* UI 接口由 INTEGRATION.md 中的 xiaozhi_ui.c 补丁提供 */
extern "C" void xiaozhi_ui_set_greenhouse(const char *text);

/* ============================ 工具函数 ============================ */

/* 拼接报警文本，例如 "温度过高(36.2℃); 湿度偏低(25.1%)"；无越界返回 0 */
static rt_bool_t gh_build_alarm_text(char *buf, rt_size_t size)
{
    rt_size_t off = 0;
    int t = s_data.temp_dc / 10;
    int h = s_data.hum_dpct / 10;

    buf[0] = '\0';
#define GH_APPEND(fmt, ...)                                     \
    do                                                          \
    {                                                           \
        int n = rt_snprintf(buf + off, size - off, fmt, ##__VA_ARGS__); \
        if (n > 0 && (rt_size_t)n < size - off) off += n;       \
        else { buf[size - 1] = '\0'; }                          \
    } while (0)

    if (t > s_thresholds.temp_max)
        GH_APPEND("温度过高(%d.%d℃)", t, (s_data.temp_dc % 10));
    else if (t < s_thresholds.temp_min)
        GH_APPEND("温度过低(%d.%d℃)", t, (s_data.temp_dc % 10));

    if (h > s_thresholds.hum_max)
    {
        if (off) GH_APPEND("; ");
        GH_APPEND("湿度过高(%d.%d%%)", h, (s_data.hum_dpct % 10));
    }
    else if (h < s_thresholds.hum_min)
    {
        if (off) GH_APPEND("; ");
        GH_APPEND("湿度偏低(%d.%d%%)", h, (s_data.hum_dpct % 10));
    }
#undef GH_APPEND
    return off > 0 ? RT_TRUE : RT_FALSE;
}

/* 推送温湿度到屏幕（在采样线程上下文调用，消息队列内部线程安全） */
static void gh_push_ui(void)
{
    char buf[64];
    if (!s_data.valid)
    {
        rt_snprintf(buf, sizeof(buf), "大棚传感器未就绪");
    }
    else if (s_alarm)
    {
        /* 报警文本可能较长，屏幕标签会换行 */
        rt_snprintf(buf, sizeof(buf), "⚠ %s", s_alarm_text);
    }
    else
    {
        rt_snprintf(buf, sizeof(buf), "大棚 温度%d.%d℃ 湿度%d.%d%% 正常",
                    s_data.temp_dc / 10, s_data.temp_dc % 10,
                    s_data.hum_dpct / 10, s_data.hum_dpct % 10);
    }
    xiaozhi_ui_set_greenhouse(buf);
}

/* ============================ 采样线程 ============================ */

static void gh_sample_entry(void *param)
{
    (void)param;
    int t_dc, h_dpct;
    rt_err_t err;
    rt_bool_t over;

    /* 上电先等系统稳定（WiFi 初始化/外设上电完成） */
    rt_thread_mdelay(3000);

    while (1)
    {
        if (s_aht_bus == RT_NULL)
            gh_aht20_probe();

        err = (s_aht_bus == RT_NULL) ? -RT_ERROR : gh_aht20_read(&t_dc, &h_dpct);

        /* 合理性过滤：AHT20 量程 温度 -40~85℃，湿度 0~100% */
        if (err == RT_EOK &&
            t_dc >= -400 && t_dc <= 850 && h_dpct >= 0 && h_dpct <= 1000)
        {
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_data.valid    = RT_TRUE;
            s_data.temp_dc  = t_dc;
            s_data.hum_dpct = h_dpct;

            /* 阈值判断 + 去抖状态机 */
            over = gh_build_alarm_text(s_alarm_text, sizeof(s_alarm_text));
            if (over)
            {
                s_ok_cnt = 0;
                if (s_over_cnt < GH_ALARM_DEBOUNCE) s_over_cnt++;
                if (s_over_cnt >= GH_ALARM_DEBOUNCE && !s_alarm)
                {
                    s_alarm = RT_TRUE;
                    LOG_W("ALARM ON: %s", s_alarm_text);
                    rt_pin_write(GH_ALARM_LED_PIN, PIN_HIGH);
                }
            }
            else
            {
                s_over_cnt = 0;
                if (s_ok_cnt < GH_ALARM_DEBOUNCE) s_ok_cnt++;
                if (s_ok_cnt >= GH_ALARM_DEBOUNCE && s_alarm)
                {
                    s_alarm = RT_FALSE;
                    LOG_I("ALARM OFF, back to normal");
                    rt_pin_write(GH_ALARM_LED_PIN, PIN_LOW);
                }
            }

            gh_push_ui();
            rt_mutex_release(&s_lock);
        }
        else
        {
            LOG_D("AHT20 read failed/skipped (err=%d t=%d h=%d)", err, t_dc, h_dpct);
        }

        rt_thread_mdelay(GH_SAMPLE_PERIOD_MS);
    }
}

/* ============================ 对外接口 ============================ */

int greenhouse_sensor_init(void)
{
    rt_thread_t tid;

    if (s_inited)
        return RT_EOK;

    rt_mutex_init(&s_lock, "gh_lock", RT_IPC_FLAG_PRIO);

    s_data.valid    = RT_FALSE;
    s_data.temp_dc  = 0;
    s_data.hum_dpct = 0;
    s_thresholds.temp_min = 5;
    s_thresholds.temp_max = 35;
    s_thresholds.hum_min  = 30;
    s_thresholds.hum_max  = 90;
    rt_strncpy(s_alarm_text, "正常", sizeof(s_alarm_text) - 1);

    /* 报警红灯初始化为熄灭 */
    rt_pin_mode(GH_ALARM_LED_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(GH_ALARM_LED_PIN, PIN_LOW);

    tid = rt_thread_create("gh_sens", gh_sample_entry, RT_NULL,
                           2048, 25, 10);
    if (tid == RT_NULL)
    {
        LOG_E("create gh_sens thread failed");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    s_inited = RT_TRUE;

    LOG_I("greenhouse sensor started (AHT20 auto-probe i2c1/i2c2/i2c0, period %d ms)",
          GH_SAMPLE_PERIOD_MS);
    return RT_EOK;
}

gh_data_t greenhouse_get_data(void)
{
    gh_data_t snap;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    snap = s_data;
    rt_mutex_release(&s_lock);
    return snap;
}

rt_bool_t greenhouse_is_alarm(void)
{
    rt_bool_t alarm;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    alarm = s_alarm;
    rt_mutex_release(&s_lock);
    return alarm;
}

const char *greenhouse_get_alarm_text(void)
{
    const char *text;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (!s_alarm)
        text = "正常";
    else
        text = s_alarm_text;
    rt_mutex_release(&s_lock);
    return text;
}

gh_thresholds_t greenhouse_get_thresholds(void)
{
    gh_thresholds_t th;
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    th = s_thresholds;
    rt_mutex_release(&s_lock);
    return th;
}

void greenhouse_set_thresholds(int temp_min, int temp_max,
                               int hum_min, int hum_max)
{
    /* 输入规整：上下限不颠倒，并限制在传感器量程内 */
    if (temp_min > temp_max)
    {
        int t = temp_min; temp_min = temp_max; temp_max = t;
    }
    if (hum_min > hum_max)
    {
        int t = hum_min; hum_min = hum_max; hum_max = t;
    }
    if (temp_min < -40) temp_min = -40;
    if (temp_max > 85)  temp_max = 85;
    if (hum_min < 0)    hum_min = 0;
    if (hum_max > 100)  hum_max = 100;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_thresholds.temp_min = temp_min;
    s_thresholds.temp_max = temp_max;
    s_thresholds.hum_min  = hum_min;
    s_thresholds.hum_max  = hum_max;
    /* 阈值变化后立刻重新评估，重置去抖计数 */
    s_over_cnt = 0;
    s_ok_cnt   = 0;
    rt_mutex_release(&s_lock);
    LOG_I("thresholds updated: T[%d,%d]℃ H[%d,%d]%%",
          temp_min, temp_max, hum_min, hum_max);
}

rt_bool_t greenhouse_sensor_available(void)
{
    return s_aht_found;
}

/* 随应用自动初始化（在 main() 之前由 RT-Thread 组件初始化调用） */
INIT_APP_EXPORT(greenhouse_sensor_init);

/* MSH 调试命令：msh /> greenhouse_status */
static int greenhouse_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    gh_data_t d = greenhouse_get_data();
    gh_thresholds_t th = greenhouse_get_thresholds();

    rt_kprintf("sensor found      : %s\n",
               greenhouse_sensor_available() == RT_TRUE ? "yes" : "no (i2c1/i2c2/i2c0 no ACK)");
    rt_kprintf("data valid       : %s\n", d.valid == RT_TRUE ? "yes" : "no");
    rt_kprintf("temperature      : %d.%d C\n", d.temp_dc / 10, d.temp_dc % 10);
    rt_kprintf("humidity         : %d.%d %%RH\n", d.hum_dpct / 10, d.hum_dpct % 10);
    rt_kprintf("alarm            : %s\n", greenhouse_is_alarm() == RT_TRUE ? "ALARM" : "normal");
    rt_kprintf("alarm text       : %s\n", greenhouse_get_alarm_text());
    rt_kprintf("thresholds       : T[%d,%d]C H[%d,%d]%%\n",
               th.temp_min, th.temp_max, th.hum_min, th.hum_max);
    return RT_EOK;
}
MSH_CMD_EXPORT(greenhouse_status, show greenhouse sensor status);
