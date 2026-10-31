/*
 * Greenhouse environment sensor monitor (real sensors only).
 * Ported from projects/Edgi_Talk_M55_GitHub_Profile/applications/xiaozhi/sensors/.
 * 2026-10-28: simulated channels (soil/light/co2 and the temp/humi fallback
 * random-walk) REMOVED per product decision - only real on-board modules are
 * reported: AHT20 air temperature and humidity on soft i2c2 (P9.3/P9.2),
 * with i2c1 and i2c0 probed as fallbacks. LSM6DS3 die temperature is kept
 * for diagnostics only and is not published as the greenhouse temperature.
 * Humidity and temperature are both invalid when the AHT20 is absent.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <netdev_ipaddr.h>
#include <netdev.h>
#include <sys/statfs.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "sensor_monitor.h"
#include "wifi_manager.h"

#define SENSOR_THREAD_STACK       2048
#define SENSOR_THREAD_PRIORITY    19
#define SENSOR_UPDATE_PERIOD_MS   1000
#define SENSOR_SUMMARY_PERIOD_S   5    /* serial summary line every 5 cycles */

/* Red LED (P16_7), same pin as iot/led.cpp but driven directly so alerts do
 * not depend on the IoT singletons. */
#define ALERT_LED_PIN             GET_PIN(16, 7)

/* Same alert is re-logged at most once per this period (30s dedup, F5) */
#define ALERT_DEDUP_S             30

/* Invalid humidity marker when no AHT20 is on board (shown as 未接传感器) */
#define HUMI_INVALID_DECI         (-1)

#define AHT20_I2C_ADDRESS         0x38U
#define AHT20_CMD_INIT            0xBEU
#define AHT20_CMD_TRIGGER         0xACU

#define LSM6DS3_ADDR_LOW          0x6AU
#define LSM6DS3_ADDR_HIGH         0x6BU
#define LSM6DS3_WHO_AM_I          0x0FU
#define LSM6DS3_WHO_AM_I_VALUE    0x6AU
#define LSM6DS3_CTRL1_XL          0x10U
#define LSM6DS3_CTRL2_G           0x11U
#define LSM6DS3_OUT_TEMP_L        0x20U

#define BATTERY_ADC_DEVICE         "adc1"
#define BATTERY_ADC_CHANNEL        1
#define BATTERY_ADC_CONTROL_PIN    GET_PIN(8, 4)
#define BATTERY_ADC_FULL_SCALE_MV  4200U
#define BATTERY_ADC_FULL_SCALE_RAW 1800U
/* Plausibility window for a real battery/USB rail reading; outside it the
 * channel is floating (BUG#3: board read 21mV) and is reported as n/a
 * instead of a fake near-zero value. */
#define BATTERY_VALID_MIN_MV       2500U
#define BATTERY_VALID_MAX_MV       5000U

static rt_mutex_t s_snapshot_lock = RT_NULL;
static edgi_sensor_snapshot_t s_snapshot;
static rt_bool_t s_monitor_started = RT_FALSE;
static struct rt_i2c_bus_device *s_aht20_bus = RT_NULL;
static rt_bool_t s_aht20_initialized = RT_FALSE;
static rt_int8_t s_aht20_bus_id = -1;
static struct rt_i2c_bus_device *s_lsm6ds3_bus = RT_NULL;
static rt_uint16_t s_lsm6ds3_address;
static rt_bool_t s_lsm6ds3_initialized = RT_FALSE;
static rt_int8_t s_lsm6ds3_bus_id = -1;
static rt_adc_device_t s_battery_adc = RT_NULL;
static rt_bool_t s_battery_adc_enabled = RT_FALSE;
static rt_uint8_t s_battery_adc_channel = BATTERY_ADC_CHANNEL;
static rt_bool_t s_battery_ch0_probed = RT_FALSE;

/* i2c2 is the AHT20 header (P9.3 SCL / P9.2 SDA). i2c1 is the touch bus
 * and i2c0 is the codec/IMU bus; both are probed because some boards move
 * the AHT20, but a bus is accepted only after a valid measurement. */
static const char *const s_aht20_bus_names[] = {"i2c2", "i2c1", "i2c0"};
static const rt_int8_t s_aht20_bus_ids[] = {2, 1, 0};
static const char *const s_lsm6ds3_bus_names[] = {"i2c0", "i2c1", "i2c2"};
static const rt_int8_t s_lsm6ds3_bus_ids[] = {0, 1, 2};

/*****************************************************************************
 * F5: alert thresholds and state
 *****************************************************************************/
static greenhouse_thresh_t s_thresh =
{
    .temp_min_deci = 80,   /* 8.0 C    */
    .temp_max_deci = 350,  /* 35.0 C   */
    .humi_min_deci = 300,  /* 30.0 %RH */
    .humi_max_deci = 900,  /* 90.0 %RH */
    .soil_min_pct  = 25,   /* 25 %     */
    .co2_max_ppm   = 1500, /* 1500 ppm */
};

typedef struct
{
    rt_bool_t active;
    rt_tick_t last_tick;
} alert_state_t;

static volatile rt_bool_t s_alert_temp_active;
static volatile rt_bool_t s_alert_humi_active;
static volatile rt_bool_t s_alert_soil_active;
static volatile rt_bool_t s_alert_co2_active;
static greenhouse_alert_cb_t s_alert_cb = RT_NULL; /* F5+: UI notify hook */
static rt_bool_t s_last_notified_active = RT_FALSE;
static rt_tick_t s_alert_temp_tick;
static rt_tick_t s_alert_humi_tick;
static rt_tick_t s_alert_soil_tick;
static rt_tick_t s_alert_co2_tick;
static rt_bool_t s_alert_led_on = RT_FALSE;

/*****************************************************************************
 * Formatting helpers (no float printf on this platform)
 *****************************************************************************/
/* "24.5" / "-3.2" from a deci value */
static int fmt_deci(char *buf, rt_size_t len, rt_int16_t deci)
{
    rt_int16_t abs_val = (deci < 0) ? (rt_int16_t)(-deci) : deci;
    return rt_snprintf(buf, len, "%s%d.%d", (deci < 0) ? "-" : "",
                       abs_val / 10, abs_val % 10);
}

/* tiny "[-]d[.d]" parser, avoids pulling atof/strtod from libc */
static float parse_simple_float(const char *s)
{
    float result = 0.0f;
    float scale = 0.1f;
    int neg = 0;
    int dot_seen = 0;

    if (s == RT_NULL)
    {
        return 0.0f;
    }
    if (*s == '-')
    {
        neg = 1;
        s++;
    }
    else if (*s == '+')
    {
        s++;
    }
    for (; *s != '\0'; s++)
    {
        if (*s == '.')
        {
            if (dot_seen)
            {
                break;
            }
            dot_seen = 1;
            continue;
        }
        if ((*s < '0') || (*s > '9'))
        {
            break;
        }
        if (!dot_seen)
        {
            result = result * 10.0f + (float)(*s - '0');
        }
        else
        {
            result += (float)(*s - '0') * scale;
            scale *= 0.1f;
        }
    }
    return neg ? -result : result;
}

/*****************************************************************************
 * I2C helpers (ported as-is from GitHub_Profile)
 *****************************************************************************/
static rt_err_t i2c_write(struct rt_i2c_bus_device *bus, rt_uint16_t address,
                          const rt_uint8_t *data, rt_uint16_t length)
{
    struct rt_i2c_msg message;

    message.addr = address;
    message.flags = RT_I2C_WR;
    message.buf = (rt_uint8_t *)data;
    message.len = length;

    return (rt_i2c_transfer(bus, &message, 1) == 1) ? RT_EOK : -RT_ERROR;
}

static rt_err_t i2c_read_registers(struct rt_i2c_bus_device *bus, rt_uint16_t address,
                                   rt_uint8_t reg, rt_uint8_t *data, rt_uint16_t length)
{
    struct rt_i2c_msg messages[2];

    messages[0].addr = address;
    messages[0].flags = RT_I2C_WR;
    messages[0].buf = &reg;
    messages[0].len = 1;
    messages[1].addr = address;
    messages[1].flags = RT_I2C_RD;
    messages[1].buf = data;
    messages[1].len = length;

    return (rt_i2c_transfer(bus, messages, 2) == 2) ? RT_EOK : -RT_ERROR;
}

static rt_int16_t clamp_int16(rt_int32_t value)
{
    if (value > 32767)
    {
        return 32767;
    }
    if (value < -32768)
    {
        return -32768;
    }
    return (rt_int16_t)value;
}

static rt_uint8_t battery_percent_from_mv(rt_uint16_t millivolts)
{
    if (millivolts >= 4200U)
    {
        return 100U;
    }
    if (millivolts >= 4100U)
    {
        return (rt_uint8_t)(96U + (millivolts - 4100U) * 4U / 100U);
    }
    if (millivolts >= 4000U)
    {
        return (rt_uint8_t)(88U + (millivolts - 4000U) * 8U / 100U);
    }
    if (millivolts >= 3900U)
    {
        return (rt_uint8_t)(75U + (millivolts - 3900U) * 13U / 100U);
    }
    if (millivolts >= 3800U)
    {
        return (rt_uint8_t)(60U + (millivolts - 3800U) * 15U / 100U);
    }
    if (millivolts >= 3700U)
    {
        return (rt_uint8_t)(40U + (millivolts - 3700U) * 20U / 100U);
    }
    if (millivolts >= 3600U)
    {
        return (rt_uint8_t)(20U + (millivolts - 3600U) * 20U / 100U);
    }
    if (millivolts >= 3500U)
    {
        return (rt_uint8_t)(10U + (millivolts - 3500U) * 10U / 100U);
    }
    if (millivolts >= 3300U)
    {
        return (rt_uint8_t)((millivolts - 3300U) * 10U / 200U);
    }
    return 0U;
}

/*
 * BUG#3: read the battery ADC with a plausibility check. The XiaoZhi board
 * has no battery on this channel (reads ~21mV); a value outside the window
 * means "no battery here" and is reported as n/a (batt_mv stays 0) instead
 * of a fake voltage. The alternate channel is probed once before giving up.
 */
static rt_bool_t read_battery(edgi_sensor_snapshot_t *snapshot)
{
    rt_uint32_t raw_value;
    rt_uint16_t millivolts;

    if (s_battery_adc == RT_NULL)
    {
        rt_pin_mode(BATTERY_ADC_CONTROL_PIN, PIN_MODE_OUTPUT);
        rt_pin_write(BATTERY_ADC_CONTROL_PIN, PIN_HIGH);
        s_battery_adc = (rt_adc_device_t)rt_device_find(BATTERY_ADC_DEVICE);
        if (s_battery_adc == RT_NULL)
        {
            return RT_FALSE;
        }
    }
    if (!s_battery_adc_enabled)
    {
        s_battery_adc_enabled =
            (rt_adc_enable(s_battery_adc, s_battery_adc_channel) == RT_EOK);
        if (!s_battery_adc_enabled)
        {
            return RT_FALSE;
        }
    }

    raw_value = rt_adc_read(s_battery_adc, s_battery_adc_channel);
    millivolts = (rt_uint16_t)((raw_value * BATTERY_ADC_FULL_SCALE_MV +
                                BATTERY_ADC_FULL_SCALE_RAW / 2U) /
                               BATTERY_ADC_FULL_SCALE_RAW);
    if ((millivolts >= BATTERY_VALID_MIN_MV) && (millivolts <= BATTERY_VALID_MAX_MV))
    {
        snapshot->battery_voltage_mv = millivolts;
        snapshot->battery_percent = battery_percent_from_mv(millivolts);
        snapshot->power_online = RT_TRUE;
        return RT_TRUE;
    }

    /* implausible reading: probe channel 0 once (XiaoZhi adc demo uses it),
     * then give up silently on later cycles */
    if (!s_battery_ch0_probed && (s_battery_adc_channel != 0))
    {
        s_battery_ch0_probed = RT_TRUE;
        rt_adc_disable(s_battery_adc, s_battery_adc_channel);
        s_battery_adc_enabled = RT_FALSE;

        if (rt_adc_enable(s_battery_adc, 0) == RT_EOK)
        {
            raw_value = rt_adc_read(s_battery_adc, 0);
            millivolts = (rt_uint16_t)((raw_value * BATTERY_ADC_FULL_SCALE_MV +
                                        BATTERY_ADC_FULL_SCALE_RAW / 2U) /
                                       BATTERY_ADC_FULL_SCALE_RAW);
            if ((millivolts >= BATTERY_VALID_MIN_MV) &&
                (millivolts <= BATTERY_VALID_MAX_MV))
            {
                s_battery_adc_channel = 0;
                s_battery_adc_enabled = RT_TRUE;
                rt_kprintf("[SENSOR] battery ADC switched to channel 0 (%dmV)\n",
                           millivolts);
                snapshot->battery_voltage_mv = millivolts;
                snapshot->battery_percent = battery_percent_from_mv(millivolts);
                snapshot->power_online = RT_TRUE;
                return RT_TRUE;
            }
            rt_adc_disable(s_battery_adc, 0);
        }

        /* neither channel carries a plausible rail: fall back to the
         * configured channel and keep reporting n/a */
        s_battery_adc_channel = BATTERY_ADC_CHANNEL;
        s_battery_adc_enabled = RT_FALSE;
        rt_kprintf("[SENSOR] battery ADC not available (ch%d=%dmV), reporting n/a\n",
                   BATTERY_ADC_CHANNEL, millivolts);
    }

    return RT_FALSE;
}

static void read_system_status(edgi_sensor_snapshot_t *snapshot)
{
    struct statfs filesystem;
    struct netdev *netdev;

    (void)read_battery(snapshot);

    rt_memset(&filesystem, 0, sizeof(filesystem));
    if ((statfs("/sdcard", &filesystem) == 0) && (filesystem.f_bsize > 0U) &&
        (filesystem.f_blocks > 0U))
    {
        rt_uint64_t total_bytes = (rt_uint64_t)filesystem.f_bsize * filesystem.f_blocks;
        rt_uint64_t free_bytes = (rt_uint64_t)filesystem.f_bsize * filesystem.f_bfree;

        snapshot->sdcard_mounted = RT_TRUE;
        snapshot->sdcard_total_mb = (rt_uint32_t)(total_bytes / (1024U * 1024U));
        snapshot->sdcard_free_mb = (rt_uint32_t)(free_bytes / (1024U * 1024U));
        snapshot->sdcard_used_percent =
            (rt_uint8_t)(100U - (free_bytes * 100U) / total_bytes);
    }

    snapshot->wifi_station_connected = wifi_manager_is_connected();
    netdev = netdev_default;
    if ((netdev != RT_NULL) && netdev_is_up(netdev) && !ip_addr_isany(&netdev->ip_addr))
    {
        snapshot->wifi_network_ready = RT_TRUE;
        (void)inet_ntoa_r(netdev->ip_addr, snapshot->wifi_ip, sizeof(snapshot->wifi_ip));
    }
}

/* Decode one AHT20 sample. Busy or uncalibrated status, or a value outside
 * the datasheet range, is rejected so a touch/codec ACK cannot be published. */
static rt_err_t aht20_decode(const rt_uint8_t data[6],
                             rt_int16_t *temp_deci, rt_int16_t *humi_deci)
{
    rt_uint32_t humidity_raw;
    rt_uint32_t temperature_raw;
    rt_int16_t humidity;
    rt_int16_t temperature;

    if (((data[0] & 0x80U) != 0U) || ((data[0] & 0x08U) == 0U))
    {
        return -RT_ERROR;
    }

    humidity_raw = ((rt_uint32_t)data[1] << 12) |
                   ((rt_uint32_t)data[2] << 4) |
                   ((rt_uint32_t)data[3] >> 4);
    temperature_raw = ((rt_uint32_t)(data[3] & 0x0FU) << 16) |
                      ((rt_uint32_t)data[4] << 8) |
                      data[5];
    humidity = (rt_int16_t)((humidity_raw * 1000U) >> 20);
    temperature = (rt_int16_t)(((temperature_raw * 2000U) >> 20) - 500);
    if ((humidity < 0) || (humidity > 1000) ||
        (temperature < -400) || (temperature > 850))
    {
        return -RT_ERROR;
    }

    *temp_deci = temperature;
    *humi_deci = humidity;
    return RT_EOK;
}

static rt_err_t aht20_measure(struct rt_i2c_bus_device *bus,
                              rt_int16_t *temp_deci, rt_int16_t *humi_deci)
{
    rt_uint8_t trigger[] = {AHT20_CMD_TRIGGER, 0x33U, 0x00U};
    rt_uint8_t data[6];
    rt_uint8_t attempt;
    struct rt_i2c_msg message;

    for (attempt = 0; attempt < 2U; attempt++)
    {
        if (i2c_write(bus, AHT20_I2C_ADDRESS, trigger, sizeof(trigger)) != RT_EOK)
        {
            return -RT_ERROR;
        }
        rt_thread_mdelay(80);

        message.addr = AHT20_I2C_ADDRESS;
        message.flags = RT_I2C_RD;
        message.buf = data;
        message.len = sizeof(data);
        if (rt_i2c_transfer(bus, &message, 1) != 1)
        {
            return -RT_ERROR;
        }
        if ((data[0] & 0x80U) == 0U)
        {
            return aht20_decode(data, temp_deci, humi_deci);
        }
    }

    return -RT_ETIMEOUT;
}

static rt_err_t read_aht20(edgi_sensor_snapshot_t *snapshot)
{
    struct rt_i2c_bus_device *bus;
    rt_uint8_t initialize[] = {AHT20_CMD_INIT, 0x08U, 0x00U};
    rt_int16_t temperature;
    rt_int16_t humidity;
    rt_err_t result = -RT_ENOSYS;
    rt_size_t index;

    bus = s_aht20_bus;
    if ((bus == RT_NULL) || !s_aht20_initialized)
    {
        s_aht20_bus = RT_NULL;
        s_aht20_initialized = RT_FALSE;
        s_aht20_bus_id = -1;
        for (index = 0; index < sizeof(s_aht20_bus_names) / sizeof(s_aht20_bus_names[0]); index++)
        {
            bus = (struct rt_i2c_bus_device *)rt_device_find(s_aht20_bus_names[index]);
            if (bus == RT_NULL)
            {
                continue;
            }
            /* 0xBE is required only while the calibration bit is clear.
             * A missing chip NACKs both this and the following trigger. */
            if (i2c_write(bus, AHT20_I2C_ADDRESS, initialize, sizeof(initialize)) == RT_EOK)
            {
                rt_thread_mdelay(10);
            }
            if (aht20_measure(bus, &temperature, &humidity) != RT_EOK)
            {
                continue;
            }
            s_aht20_bus = bus;
            s_aht20_initialized = RT_TRUE;
            s_aht20_bus_id = s_aht20_bus_ids[index];
            snapshot->humidity_deci_percent = humidity;
            snapshot->environment_temperature_deci_c = temperature;
            snapshot->environment_online = RT_TRUE;
            snapshot->environment_bus = s_aht20_bus_id;
            return RT_EOK;
        }
        return result;
    }

    result = aht20_measure(bus, &temperature, &humidity);
    if (result != RT_EOK)
    {
        s_aht20_initialized = RT_FALSE;
        s_aht20_bus = RT_NULL;
        return result;
    }

    snapshot->humidity_deci_percent = humidity;
    snapshot->environment_temperature_deci_c = temperature;
    snapshot->environment_online = RT_TRUE;
    snapshot->environment_bus = s_aht20_bus_id;
    return RT_EOK;
}

static rt_err_t lsm6ds3_find(struct rt_i2c_bus_device **bus_out, rt_uint16_t *address_out)
{
    struct rt_i2c_bus_device *bus;
    rt_uint8_t id;
    rt_uint16_t addresses[] = {LSM6DS3_ADDR_LOW, LSM6DS3_ADDR_HIGH};
    rt_size_t bus_index;
    rt_size_t address_index;

    for (bus_index = 0; bus_index < sizeof(s_lsm6ds3_bus_names) / sizeof(s_lsm6ds3_bus_names[0]); bus_index++)
    {
        bus = (struct rt_i2c_bus_device *)rt_device_find(s_lsm6ds3_bus_names[bus_index]);
        if (bus == RT_NULL)
        {
            continue;
        }
        for (address_index = 0; address_index < sizeof(addresses) / sizeof(addresses[0]); address_index++)
        {
            if ((i2c_read_registers(bus, addresses[address_index], LSM6DS3_WHO_AM_I, &id, 1) == RT_EOK) &&
                (id == LSM6DS3_WHO_AM_I_VALUE))
            {
                *bus_out = bus;
                *address_out = addresses[address_index];
                s_lsm6ds3_bus_id = s_lsm6ds3_bus_ids[bus_index];
                return RT_EOK;
            }
        }
    }

    return -RT_ERROR;
}

static rt_err_t read_lsm6ds3(edgi_sensor_snapshot_t *snapshot)
{
    struct rt_i2c_bus_device *bus;
    rt_uint16_t address;
    rt_uint8_t ctrl1[] = {LSM6DS3_CTRL1_XL, 0x30U}; /* 26 Hz, +/-2 g */
    rt_uint8_t ctrl2[] = {LSM6DS3_CTRL2_G, 0x3CU};  /* 26 Hz, +/-2000 dps */
    rt_uint8_t data[14];
    rt_int16_t raw_temperature;
    rt_size_t axis;

    if (!s_lsm6ds3_initialized)
    {
        if (lsm6ds3_find(&bus, &address) != RT_EOK)
        {
            return -RT_ERROR;
        }

        if ((i2c_write(bus, address, ctrl1, sizeof(ctrl1)) != RT_EOK) ||
            (i2c_write(bus, address, ctrl2, sizeof(ctrl2)) != RT_EOK))
        {
            return -RT_ERROR;
        }
        s_lsm6ds3_bus = bus;
        s_lsm6ds3_address = address;
        s_lsm6ds3_initialized = RT_TRUE;
        rt_thread_mdelay(45);
    }
    else
    {
        bus = s_lsm6ds3_bus;
        address = s_lsm6ds3_address;
    }

    if (i2c_read_registers(bus, address, LSM6DS3_OUT_TEMP_L, data, sizeof(data)) != RT_EOK)
    {
        s_lsm6ds3_initialized = RT_FALSE;
        return -RT_ERROR;
    }

    raw_temperature = (rt_int16_t)(((rt_uint16_t)data[1] << 8) | data[0]);
    snapshot->motion_temperature_deci_c =
        clamp_int16(250 + ((rt_int32_t)raw_temperature * 10) / 256);

    for (axis = 0; axis < 3; axis++)
    {
        rt_int16_t raw_gyro = (rt_int16_t)(((rt_uint16_t)data[3 + axis * 2] << 8) |
                                            data[2 + axis * 2]);
        rt_int16_t raw_accel = (rt_int16_t)(((rt_uint16_t)data[9 + axis * 2] << 8) |
                                             data[8 + axis * 2]);

        /* +/-2000 dps is 70 mdps/LSB; +/-2 g is 0.061 mg/LSB. */
        snapshot->angular_rate_deci_dps[axis] =
            clamp_int16(((rt_int32_t)raw_gyro * 7) / 10);
        snapshot->acceleration_mg[axis] =
            clamp_int16(((rt_int32_t)raw_accel * 61) / 1000);
    }

    snapshot->motion_online = RT_TRUE;
    snapshot->motion_bus = s_lsm6ds3_bus_id;
    return RT_EOK;
}

/*****************************************************************************
 * Merge real channels into the greenhouse fields (real sensors only)
 *****************************************************************************/
static void apply_greenhouse_channels(edgi_sensor_snapshot_t *next)
{
    /* IMU die temperature is diagnostic only. The greenhouse temperature
     * and humidity are the AHT20 air reading, or invalid when it is absent. */
    next->imu_temp_deci = next->motion_online ? next->motion_temperature_deci_c : 0;

    if (next->environment_online)
    {
        next->temp_deci = next->environment_temperature_deci_c;
        next->humi_deci = next->humidity_deci_percent;
        next->temp_sim = RT_FALSE;
        next->humi_sim = RT_FALSE;
    }
    else
    {
        next->temp_deci = 0;
        next->humi_deci = HUMI_INVALID_DECI;
        next->temp_sim = RT_TRUE;
        next->humi_sim = RT_TRUE;
    }

    /* soil / light / co2: no real sensor on board - reported as 0 and
     * excluded from the Thing / MCP / UI (no simulated values). */
    next->soil_pct = 0;
    next->soil_sim = RT_TRUE;
    next->light_lux = 0;
    next->light_sim = RT_TRUE;
    next->co2_ppm = 0;
    next->co2_sim = RT_TRUE;

    next->batt_mv = next->battery_voltage_mv;
}

/*****************************************************************************
 * F5: threshold alerts
 *****************************************************************************/
static rt_bool_t alert_should_log(rt_bool_t active, rt_tick_t last_tick)
{
    if (!active)
    {
        return RT_TRUE; /* fresh violation */
    }
    /* already active: periodic re-alert with 30s dedup */
    return ((rt_tick_get() - last_tick) >=
            (rt_tick_t)rt_tick_from_millisecond(ALERT_DEDUP_S * 1000)) ? RT_TRUE : RT_FALSE;
}

static void alert_update(const char *name, rt_bool_t violating, volatile rt_bool_t *active,
                         rt_tick_t *last_tick, const char *value_text, const char *range_text)
{
    if (violating)
    {
        if (alert_should_log(*active, *last_tick))
        {
            rt_kprintf("[ALERT] %s=%s out of range %s\n", name, value_text, range_text);
            *last_tick = rt_tick_get();
        }
        *active = RT_TRUE;
    }
    else if (*active)
    {
        rt_kprintf("[RECOVER] %s=%s back in range %s\n", name, value_text, range_text);
        *active = RT_FALSE;
    }
}

static void check_alerts(const edgi_sensor_snapshot_t *snap)
{
    char value[24];
    char range[40];
    rt_bool_t any_active;

    fmt_deci(value, sizeof(value), snap->temp_deci);
    strcpy(range, "[");
    fmt_deci(range + strlen(range), sizeof(range) - strlen(range), s_thresh.temp_min_deci);
    strcat(range, ",");
    fmt_deci(range + strlen(range), sizeof(range) - strlen(range), s_thresh.temp_max_deci);
    strcat(range, "]");
    alert_update("temp",
                 (!snap->temp_sim) &&
                 ((snap->temp_deci < s_thresh.temp_min_deci) ||
                  (snap->temp_deci > s_thresh.temp_max_deci)),
                 &s_alert_temp_active, &s_alert_temp_tick, value, range);

    /* Humidity: only meaningful with a real AHT20 (humi_sim set = no sensor,
     * humi_deci == -1); the flag clears automatically when it disappears. */
    fmt_deci(value, sizeof(value), snap->humi_deci);
    rt_snprintf(range, sizeof(range), "[%d.%d,%d.%d]",
                s_thresh.humi_min_deci / 10, s_thresh.humi_min_deci % 10,
                s_thresh.humi_max_deci / 10, s_thresh.humi_max_deci % 10);
    alert_update("humi",
                 (!snap->humi_sim) &&
                 ((snap->humi_deci < s_thresh.humi_min_deci) ||
                  (snap->humi_deci > s_thresh.humi_max_deci)),
                 &s_alert_humi_active, &s_alert_humi_tick, value, range);

    /* soil / co2: no real sensor on board - alerts removed (no simulated
     * data is produced anymore). */

    /* red LED: blink (~1s toggle) while any alert active, off when clear */
    any_active = (s_alert_temp_active || s_alert_humi_active ||
                  s_alert_soil_active || s_alert_co2_active) ? RT_TRUE : RT_FALSE;
    if (any_active)
    {
        s_alert_led_on = !s_alert_led_on;
        rt_pin_write(ALERT_LED_PIN, s_alert_led_on ? PIN_HIGH : PIN_LOW);
    }
    else if (s_alert_led_on)
    {
        s_alert_led_on = RT_FALSE;
        rt_pin_write(ALERT_LED_PIN, PIN_LOW);
    }

    /* F5+: notify the UI once per active<->clear transition */
    if (any_active != s_last_notified_active)
    {
        s_last_notified_active = any_active;
        if (s_alert_cb != RT_NULL)
        {
            char text[96];
            greenhouse_build_alert_text(text, sizeof(text));
            s_alert_cb(any_active, text);
        }
    }
}

/*****************************************************************************
 * F1: serial summary + one-shot probe conclusion
 *****************************************************************************/
static void print_probe_conclusion(const edgi_sensor_snapshot_t *snap)
{
    if (snap->environment_online)
    {
        rt_kprintf("[SENSOR] AHT20 found on i2c%d (air temp/humi real)\n",
                   snap->environment_bus);
    }
    else
    {
        rt_kprintf("[SENSOR] AHT20 not found on i2c2/i2c1/i2c0, "
                   "air temperature and humidity unavailable\n");
        if (rt_device_find("i2c2") == RT_NULL)
        {
            rt_kprintf("[SENSOR] i2c2 bus is not registered\n");
        }
    }

    if (snap->motion_online)
    {
        rt_kprintf("[SENSOR] LSM6DS3 found on i2c%d (imu_temp real)\n", snap->motion_bus);
    }
    else
    {
        rt_kprintf("[SENSOR] LSM6DS3 not found on i2c0/i2c1/i2c2, imu_temp=0\n");
    }
}

static void print_summary(const edgi_sensor_snapshot_t *snap)
{
    char t[12];
    char h[12];
    char batt[12];

    if (snap->environment_online)
        fmt_deci(t, sizeof(t), snap->temp_deci);
    else
        rt_strncpy(t, "--", sizeof(t) - 1);
    if (snap->humi_deci < 0)
        rt_strncpy(h, "n/a", sizeof(h) - 1);
    else
        fmt_deci(h, sizeof(h), snap->humi_deci);

    if (snap->batt_mv > 0)
        rt_snprintf(batt, sizeof(batt), "%dmV", snap->batt_mv);
    else
        rt_strncpy(batt, "n/a", sizeof(batt) - 1);

    rt_kprintf("[SENSOR] t=%sC(src=%s) h=%s%% batt=%s sd=%d wifi=%d ip=%s\n",
               t,
               snap->environment_online ? "AHT20" : "none",
               h, batt,
               snap->sdcard_mounted ? 1 : 0,
               snap->wifi_station_connected ? 1 : 0,
               (snap->wifi_ip[0] != '\0') ? snap->wifi_ip : "-");
}

/*****************************************************************************
 * Monitor thread
 *****************************************************************************/
static void sensor_monitor_entry(void *parameter)
{
    rt_uint32_t cycle = 0;
    rt_bool_t probe_reported = RT_FALSE;

    (void)parameter;

    rt_pin_mode(ALERT_LED_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(ALERT_LED_PIN, PIN_LOW);

    while (1)
    {
        edgi_sensor_snapshot_t next;

        rt_memset(&next, 0, sizeof(next));
        next.environment_bus = -1;
        next.motion_bus = -1;
        next.environment_error = read_aht20(&next);
        next.motion_error = read_lsm6ds3(&next);
        read_system_status(&next);
        apply_greenhouse_channels(&next);

        rt_mutex_take(s_snapshot_lock, RT_WAITING_FOREVER);
        s_snapshot = next;
        rt_mutex_release(s_snapshot_lock);

        if (!probe_reported)
        {
            probe_reported = RT_TRUE;
            print_probe_conclusion(&next);
        }

        if ((cycle % SENSOR_SUMMARY_PERIOD_S) == 0)
        {
            print_summary(&next);
        }

        check_alerts(&next);

        cycle++;
        rt_thread_mdelay(SENSOR_UPDATE_PERIOD_MS);
    }
}

void edgi_sensor_monitor_init(void)
{
    rt_thread_t thread;

    if (s_monitor_started)
    {
        return;
    }

    s_snapshot_lock = rt_mutex_create("smon", RT_IPC_FLAG_PRIO);
    if (s_snapshot_lock == RT_NULL)
    {
        return;
    }

    thread = rt_thread_create("smon", sensor_monitor_entry, RT_NULL,
                              SENSOR_THREAD_STACK, SENSOR_THREAD_PRIORITY, 10);
    if (thread == RT_NULL)
    {
        rt_mutex_delete(s_snapshot_lock);
        s_snapshot_lock = RT_NULL;
        return;
    }

    s_monitor_started = RT_TRUE;
    rt_thread_startup(thread);
}

rt_err_t edgi_sensor_monitor_get_snapshot(edgi_sensor_snapshot_t *snapshot)
{
    if ((snapshot == RT_NULL) || (s_snapshot_lock == RT_NULL))
    {
        return -RT_ERROR;
    }

    rt_mutex_take(s_snapshot_lock, RT_WAITING_FOREVER);
    *snapshot = s_snapshot;
    rt_mutex_release(s_snapshot_lock);
    return RT_EOK;
}

/*****************************************************************************
 * F5 public API: thresholds
 *****************************************************************************/
void greenhouse_get_thresh(greenhouse_thresh_t *thresh)
{
    if (thresh == RT_NULL)
    {
        return;
    }
    rt_memcpy(thresh, &s_thresh, sizeof(s_thresh));
}

rt_err_t greenhouse_set_thresh(const char *name, float min_value, float max_value)
{
    if (name == RT_NULL)
    {
        return -RT_EINVAL;
    }

    if (rt_strcmp(name, "temp") == 0)
    {
        s_thresh.temp_min_deci = (rt_int16_t)(min_value * 10);
        s_thresh.temp_max_deci = (rt_int16_t)(max_value * 10);
    }
    else if (rt_strcmp(name, "humi") == 0)
    {
        s_thresh.humi_min_deci = (rt_int16_t)(min_value * 10);
        s_thresh.humi_max_deci = (rt_int16_t)(max_value * 10);
    }
    else if (rt_strcmp(name, "soil") == 0)
    {
        if ((min_value < 0.0f) || (min_value > 100.0f))
        {
            return -RT_EINVAL;
        }
        s_thresh.soil_min_pct = (rt_uint8_t)min_value;
    }
    else if (rt_strcmp(name, "co2") == 0)
    {
        if ((min_value < 0.0f) || (max_value > 65000.0f))
        {
            return -RT_EINVAL;
        }
        s_thresh.co2_max_ppm = (rt_uint16_t)max_value;
    }
    else
    {
        return -RT_EINVAL;
    }

    return RT_EOK;
}

rt_bool_t greenhouse_any_alert_active(void)
{
    return (s_alert_temp_active || s_alert_humi_active ||
            s_alert_soil_active || s_alert_co2_active) ? RT_TRUE : RT_FALSE;
}

/*****************************************************************************
 * F5+: per-channel alert flags / alert text / UI callback
 *****************************************************************************/
void greenhouse_get_alert_flags(rt_bool_t *temp, rt_bool_t *humi,
                                rt_bool_t *soil, rt_bool_t *co2)
{
    if (temp != RT_NULL) *temp = s_alert_temp_active;
    if (humi != RT_NULL) *humi = s_alert_humi_active;
    if (soil != RT_NULL) *soil = s_alert_soil_active;
    if (co2 != RT_NULL) *co2 = s_alert_co2_active;
}

int greenhouse_build_alert_text(char *buf, rt_size_t len)
{
    edgi_sensor_snapshot_t snap;
    rt_size_t off = 0;

    if ((buf == RT_NULL) || (len == 0))
    {
        return 0;
    }
    buf[0] = '\0';
    if (edgi_sensor_monitor_get_snapshot(&snap) != RT_EOK)
    {
        return 0;
    }

#define GH_APPEND(...)                                              \
    do                                                              \
    {                                                               \
        int n = rt_snprintf(buf + off, len - off, ##__VA_ARGS__);    \
        if ((n > 0) && ((rt_size_t)n < len - off)) off += n;         \
        else { buf[len - 1] = '\0'; return (int)off; }               \
    } while (0)

    if (s_alert_temp_active)
    {
        char v[12];
        fmt_deci(v, sizeof(v), snap.temp_deci);
        if (snap.temp_deci > s_thresh.temp_max_deci)
            GH_APPEND("温度过高(%s℃)", v);
        else
            GH_APPEND("温度过低(%s℃)", v);
    }
    if (s_alert_humi_active)
    {
        char v[12];
        fmt_deci(v, sizeof(v), snap.humi_deci);
        GH_APPEND("%s湿度过%s(%s%%)", (off ? "; " : ""),
                  (snap.humi_deci > s_thresh.humi_max_deci) ? "高" : "低", v);
    }
#undef GH_APPEND
    return (int)off;
}

void greenhouse_set_alert_callback(greenhouse_alert_cb_t cb)
{
    s_alert_cb = cb;
}

/*****************************************************************************
 * F6: full status JSON
 *****************************************************************************/
/* bounds-checked append, returns new offset or -1 on error/truncation */
static int json_append(char *buf, rt_size_t len, int off, const char *fmt, ...)
{
    va_list args;
    int n;

    if ((off < 0) || ((rt_size_t)off >= len))
    {
        return -1;
    }
    va_start(args, fmt);
    n = rt_vsnprintf(buf + off, len - (rt_size_t)off, fmt, args);
    va_end(args);
    if ((n < 0) || ((int)(len - 1) <= off + n))
    {
        return -1;
    }
    return off + n;
}

static int json_append_deci(char *buf, rt_size_t len, int off, rt_int16_t deci)
{
    char tmp[12];

    fmt_deci(tmp, sizeof(tmp), deci);
    return json_append(buf, len, off, "%s", tmp);
}

int greenhouse_get_status_json(char *buf, rt_size_t len)
{
    edgi_sensor_snapshot_t snap;
    greenhouse_thresh_t th;
    int off = 0;

    if ((buf == RT_NULL) || (len == 0))
    {
        return -RT_EINVAL;
    }

    if (edgi_sensor_monitor_get_snapshot(&snap) != RT_EOK)
    {
        buf[0] = '\0';
        return -RT_ERROR;
    }
    greenhouse_get_thresh(&th);

    off = json_append(buf, len, off, "{\"temperature\":");
    if (!snap.environment_online)
        off = json_append(buf, len, off, "null");
    else
        off = json_append_deci(buf, len, off, snap.temp_deci);
    off = json_append(buf, len, off,
                      ",\"temperature_source\":\"%s\"",
                      snap.environment_online ? "AHT20" : "none");
    off = json_append(buf, len, off, ",\"humidity\":");
    if (snap.humi_deci < 0)
        off = json_append(buf, len, off, "null");
    else
        off = json_append_deci(buf, len, off, snap.humi_deci);
    off = json_append(buf, len, off,
                      ",\"battery_mv\":%d,\"battery_valid\":%s,\"battery_pct\":%d,"
                      "\"sd_mounted\":%s,\"sd_used_pct\":%d,\"sd_free_mb\":%d,"
                      "\"wifi_connected\":%s,\"wifi_ip\":\"%s\"",
                      snap.batt_mv,
                      (snap.batt_mv > 0) ? "true" : "false",
                      snap.battery_percent,
                      snap.sdcard_mounted ? "true" : "false",
                      snap.sdcard_used_percent,
                      snap.sdcard_free_mb,
                      snap.wifi_station_connected ? "true" : "false",
                      (snap.wifi_ip[0] != '\0') ? snap.wifi_ip : "0.0.0.0");
    off = json_append(buf, len, off,
                      ",\"alerts\":{\"temperature\":%s,\"humidity\":%s}",
                      s_alert_temp_active ? "true" : "false",
                      s_alert_humi_active ? "true" : "false");
    off = json_append(buf, len, off, ",\"thresholds\":{\"temperature\":[");
    off = json_append_deci(buf, len, off, th.temp_min_deci);
    off = json_append(buf, len, off, ",");
    off = json_append_deci(buf, len, off, th.temp_max_deci);
    off = json_append(buf, len, off, "],\"humidity\":[");
    off = json_append_deci(buf, len, off, th.humi_min_deci);
    off = json_append(buf, len, off, ",");
    off = json_append_deci(buf, len, off, th.humi_max_deci);
    off = json_append(buf, len, off, "]}}");

    if (off < 0)
    {
        buf[0] = '\0';
        return -RT_ENOMEM;
    }
    return off;
}

/*****************************************************************************
 * F5 msh command: xz_thresh show | set <name> <min> <max>
 *****************************************************************************/
static void xz_thresh(int argc, char **argv)
{
    greenhouse_thresh_t th;
    char min_str[12];
    char max_str[12];

    if ((argc < 2) || (rt_strcmp(argv[1], "show") == 0))
    {
        greenhouse_get_thresh(&th);
        fmt_deci(min_str, sizeof(min_str), th.temp_min_deci);
        fmt_deci(max_str, sizeof(max_str), th.temp_max_deci);
        rt_kprintf("temp : [%s, %s] C\n", min_str, max_str);
        fmt_deci(min_str, sizeof(min_str), th.humi_min_deci);
        fmt_deci(max_str, sizeof(max_str), th.humi_max_deci);
        rt_kprintf("humi : [%s, %s] %%RH\n", min_str, max_str);
        rt_kprintf("soil : [%d, 100] %%\n", th.soil_min_pct);
        rt_kprintf("co2  : [0, %d] ppm\n", th.co2_max_ppm);
        rt_kprintf("usage: xz_thresh set <temp|humi|soil|co2> <min> <max>\n");
        return;
    }

    if ((rt_strcmp(argv[1], "set") == 0) && (argc == 5))
    {
        float min_value = parse_simple_float(argv[3]);
        float max_value = parse_simple_float(argv[4]);

        if (min_value > max_value)
        {
            rt_kprintf("[SENSOR] min > max, rejected\n");
            return;
        }
        if (greenhouse_set_thresh(argv[2], min_value, max_value) == RT_EOK)
        {
            rt_kprintf("[SENSOR] threshold '%s' set to [%s, %s]\n",
                       argv[2], argv[3], argv[4]);
        }
        else
        {
            rt_kprintf("[SENSOR] unknown channel '%s' (temp|humi|soil|co2)\n", argv[2]);
        }
        return;
    }

    rt_kprintf("usage: xz_thresh show | set <temp|humi|soil|co2> <min> <max>\n");
}
MSH_CMD_EXPORT(xz_thresh, show or set greenhouse alert thresholds);
