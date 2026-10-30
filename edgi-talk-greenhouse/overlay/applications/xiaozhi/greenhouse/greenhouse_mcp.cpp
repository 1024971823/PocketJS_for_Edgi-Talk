/*
 * greenhouse_mcp.cpp - 把大棚环境注册为小智 MCP 工具
 *
 * 机制（与官方 mcp/mcp_server.cpp 的 AddCommonTools() 同一套框架）：
 *   小智 v2.x 服务端是 MCP client，握手后 tools/list 拉取工具清单，
 *   对话中按需 tools/call 现场取值。本文件在应用初始化阶段向
 *   McpServer 单例追加 5 个工具，与 AddCommonTools() 的内置工具
 *   （self.audio_speaker.* / self.screen.* / self.led.set）并存。
 *
 * 工具命名遵循官方规范 self.模块.功能。
 *
 * Change Logs:
 * 2026-10-26     Greenhouse     First version
 */

#include <rtthread.h>
#include <string>
#include "mcp_server.h"
#include "greenhouse_sensor.h"

#define TAG "GH_MCP"

static std::string gh_report_string(void)
{
    gh_data_t d = greenhouse_get_data();

    if (!greenhouse_sensor_available())
        return std::string("错误: 未找到AHT20传感器(i2c1/i2c2/i2c0均无应答,请检查板上是否焊载)");
    if (!d.valid)
        return std::string("传感器数据尚未就绪,请稍后再试");

    char buf[128];
    rt_snprintf(buf, sizeof(buf), "大棚温度%d.%d℃,湿度%d.%d%%,%s",
                d.temp_dc / 10, d.temp_dc % 10,
                d.hum_dpct / 10, d.hum_dpct % 10,
                greenhouse_get_alarm_text());
    return std::string(buf);
}

static int greenhouse_mcp_init(void)
{
    auto &mcp = McpServer::GetInstance();

    mcp.AddTool("self.greenhouse.get_report",
        "获取蔬菜大棚环境报告(温度/湿度/报警状态一次性返回),推荐优先调用",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            return gh_report_string();
        });

    mcp.AddTool("self.greenhouse.get_temperature",
        "获取大棚空气温度,返回值单位0.1摄氏度,253表示25.3摄氏度",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            return greenhouse_get_data().temp_dc;
        });

    mcp.AddTool("self.greenhouse.get_humidity",
        "获取大棚空气相对湿度,返回值单位0.1百分比,605表示60.5%",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            return greenhouse_get_data().hum_dpct;
        });

    mcp.AddTool("self.greenhouse.get_alarm_status",
        "获取大棚报警状态,正常返回:正常,报警时返回具体原因",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            return std::string(greenhouse_get_alarm_text());
        });

    mcp.AddTool("self.greenhouse.set_thresholds",
        "设置大棚温湿度报警阈值(单位:摄氏度/百分比)。参数可省略,省略时使用默认值",
        PropertyList({
            Property("temp_min", kPropertyTypeInteger, 5,  -40, 85),
            Property("temp_max", kPropertyTypeInteger, 35, -40, 85),
            Property("hum_min",  kPropertyTypeInteger, 30,   0, 100),
            Property("hum_max",  kPropertyTypeInteger, 90,   0, 100)
        }), [](const PropertyList &p) -> ReturnValue {
            int tmin = p["temp_min"].value<int>();
            int tmax = p["temp_max"].value<int>();
            int hmin = p["hum_min"].value<int>();
            int hmax = p["hum_max"].value<int>();
            greenhouse_set_thresholds(tmin, tmax, hmin, hmax);
            return std::string("阈值已设置:温度" + std::to_string(tmin) + "~" +
                               std::to_string(tmax) + "℃,湿度" +
                               std::to_string(hmin) + "~" + std::to_string(hmax) + "%");
        });

    rt_kprintf("[%s] 5 greenhouse MCP tools registered\n", TAG);
    return RT_EOK;
}
INIT_APP_EXPORT(greenhouse_mcp_init);
