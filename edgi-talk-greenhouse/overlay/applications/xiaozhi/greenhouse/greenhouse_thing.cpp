/*
 * greenhouse_thing.cpp - 把大棚环境暴露为小智 IoT Thing
 *
 * 机制（与官方 iot/led.cpp、speaker.cpp 同一套框架）：
 *   - Thing 的 properties 会随 iot states 消息上报服务端，
 *     注入到大模型上下文，用户问"大棚温度多少"AI 可直接回答；
 *   - methods 可由 AI 下发调用（SetThresholds 语音改阈值）；
 *   - DECLARE_THING 宏静态注册，INIT_APP_EXPORT 时挂入 ThingManager，
 *     与官方 iot_c_api.cpp 注册 Speaker/Screen/Led 的方式互补、互不冲突。
 *
 * Change Logs:
 * 2026-10-26     Greenhouse     First version
 */

#include <rtthread.h>
#include "thing.h"
#include "thing_manager.h"
#include "greenhouse_sensor.h"

#define TAG "Greenhouse"

namespace iot
{

class Greenhouse : public Thing
{
public:
    Greenhouse() : Thing("Greenhouse", "蔬菜大棚环境监测(空气温湿度)")
    {
        /* ---- 属性：随 states 上报，注入大模型上下文 ---- */
        properties_.AddNumberProperty("temperature", "大棚空气温度,单位0.1摄氏度,253表示25.3度", [this]() -> int {
            return greenhouse_get_data().temp_dc;
        });
        properties_.AddNumberProperty("humidity", "大棚空气相对湿度,单位0.1百分比,605表示60.5%", [this]() -> int {
            return greenhouse_get_data().hum_dpct;
        });
        properties_.AddBooleanProperty("alarm", "环境是否超阈值报警(true=有报警)", [this]() -> bool {
            return greenhouse_is_alarm() == RT_TRUE;
        });
        properties_.AddStringProperty("alarm_description", "报警详情,正常时为:正常", [this]() -> std::string {
            return greenhouse_get_alarm_text();
        });

        /* ---- 方法：AI 可调用 ---- */
        methods_.AddMethod("SetThresholds", "设置大棚温湿度报警阈值(整数度/整数百分比)",
            ParameterList({
                Parameter("temp_min", "温度下限(摄氏度)", kValueTypeNumber, true),
                Parameter("temp_max", "温度上限(摄氏度)", kValueTypeNumber, true),
                Parameter("hum_min",  "湿度下限(百分比)", kValueTypeNumber, true),
                Parameter("hum_max",  "湿度上限(百分比)", kValueTypeNumber, true)
            }), [this](const ParameterList &p) {
                int tmin = p["temp_min"].number();
                int tmax = p["temp_max"].number();
                int hmin = p["hum_min"].number();
                int hmax = p["hum_max"].number();
                greenhouse_set_thresholds(tmin, tmax, hmin, hmax);
                rt_kprintf("[%s] SetThresholds T[%d,%d] H[%d,%d]\n",
                           TAG, tmin, tmax, hmin, hmax);
            });

        rt_kprintf("[%s] thing created\n", TAG);
    }
};

} // namespace iot

DECLARE_THING(Greenhouse);

/* 在应用初始化阶段挂入 ThingManager。
 * 官方 iot_initialize()（WebSocket 连上后调用）稍后注册 Speaker/Screen/Led，
 * 两边都只是向单例追加，顺序无关。 */
static int greenhouse_thing_init(void)
{
    iot::ThingManager::GetInstance().AddThing(iot::CreateThing("Greenhouse"));
    return RT_EOK;
}
INIT_APP_EXPORT(greenhouse_thing_init);
