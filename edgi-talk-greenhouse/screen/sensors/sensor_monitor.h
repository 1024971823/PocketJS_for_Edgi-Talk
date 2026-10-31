#ifndef EDGI_SENSOR_MONITOR_H
#define EDGI_SENSOR_MONITOR_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Greenhouse environment snapshot (real sensors only since 2026-10-28).
 * Channels without a real on-board sensor (soil/light/co2) are reported as 0
 * with their *_sim flag set and are excluded from the IoT Thing, MCP tool
 * and UI. Temperature and humidity both come from the AHT20. The LSM6DS3
 * die temperature stays in imu_temp_deci and is not shown as air temperature.
 * humi_deci == -1 and temp_sim set means the AHT20 is not attached.
 */
typedef struct
{
    /* Greenhouse channels */
    rt_int16_t temp_deci;       /* AHT20 air temperature, 0.1 degree C */
    rt_int16_t humi_deci;       /* air relative humidity 0.1 %RH, -1 = no sensor */
    rt_uint8_t soil_pct;        /* 0 (no real sensor on board) */
    rt_uint32_t light_lux;      /* 0 (no real sensor on board) */
    rt_uint16_t co2_ppm;        /* 0 (no real sensor on board) */
    rt_bool_t temp_sim;         /* RT_TRUE = AHT20 absent, temp_deci is not air temperature */
    rt_bool_t humi_sim;         /* RT_TRUE = no AHT20, humidity invalid */
    rt_bool_t soil_sim;
    rt_bool_t light_sim;
    rt_bool_t co2_sim;
    rt_int16_t imu_temp_deci;   /* LSM6DS3 die temperature, 0.1 degree C */
    rt_uint16_t batt_mv;        /* battery voltage, mV */

    /* Raw sensor status (kept from the original GitHub_Profile module) */
    rt_bool_t environment_online;   /* AHT20温湿度 */
    rt_bool_t motion_online;        /* LSM6DS3 IMU */
    rt_int8_t environment_bus;
    rt_int8_t motion_bus;
    rt_int16_t environment_error;
    rt_int16_t motion_error;
    rt_int16_t environment_temperature_deci_c;
    rt_int16_t humidity_deci_percent;
    rt_int16_t acceleration_mg[3];
    rt_int16_t angular_rate_deci_dps[3];
    rt_int16_t motion_temperature_deci_c;
    rt_bool_t power_online;
    rt_uint16_t battery_voltage_mv;
    rt_uint8_t battery_percent;
    rt_bool_t sdcard_mounted;
    rt_uint8_t sdcard_used_percent;
    rt_uint32_t sdcard_total_mb;
    rt_uint32_t sdcard_free_mb;
    rt_bool_t wifi_station_connected;
    rt_bool_t wifi_network_ready;
    char wifi_ip[16];
} edgi_sensor_snapshot_t;

/* Alert thresholds (F5). Values in the same unit as the channels above. */
typedef struct
{
    rt_int16_t temp_min_deci;
    rt_int16_t temp_max_deci;
    rt_int16_t humi_min_deci;
    rt_int16_t humi_max_deci;
    rt_uint8_t soil_min_pct;
    rt_uint16_t co2_max_ppm;
} greenhouse_thresh_t;

void edgi_sensor_monitor_init(void);
rt_err_t edgi_sensor_monitor_get_snapshot(edgi_sensor_snapshot_t *snapshot);

/* F5: threshold query / modify. name: "temp" "humi" "soil" "co2" */
void greenhouse_get_thresh(greenhouse_thresh_t *thresh);
rt_err_t greenhouse_set_thresh(const char *name, float min_value, float max_value);
rt_bool_t greenhouse_any_alert_active(void);

/*
 * F5+: per-channel alert flag query for UI card coloring / IoT Thing.
 * Any output pointer may be RT_NULL. Values are the live alert state
 * (not debounced beyond the monitor's own 1s cycle).
 */
void greenhouse_get_alert_flags(rt_bool_t *temp, rt_bool_t *humi,
                                rt_bool_t *soil, rt_bool_t *co2);

/*
 * F5+: render the active alerts as one human-readable Chinese line, e.g.
 * "温度过高(36.2℃); 土壤偏干(22%,下限45)". Returns length; 0 when all clear.
 */
int greenhouse_build_alert_text(char *buf, rt_size_t len);

/*
 * F5+: alert state-change callback for UI (bubble/banner). Fired from the
 * sensor monitor thread on every active->clear / clear->active transition,
 * with the rendered alert text ("" when clear). Keep the callback light:
 * post to a queue, never block.
 */
typedef void (*greenhouse_alert_cb_t)(rt_bool_t any_active, const char *text);
void greenhouse_set_alert_callback(greenhouse_alert_cb_t cb);

/* F6: render the full greenhouse status (values + sim flags + alerts +
 * thresholds) as one JSON string. Returns string length, <0 on error. */
int greenhouse_get_status_json(char *buf, rt_size_t len);

#ifdef __cplusplus
}
#endif

#endif /* EDGI_SENSOR_MONITOR_H */
