# 蔬菜大棚 AI 管家（Edgi Talk + RT-Thread + 小智）

基于 **RT-Thread × 英飞凌 Edgi Talk 开发板**（PSoC™ Edge E84）官方小智工程的
**蔬菜大棚环境监测 AI 管家**：板载 AHT20 实时监测大棚空气温湿度，语音即问即答，
超阈值屏幕红色告警 + 红灯点亮，AI 能主动播报报警原因。

## 功能一览

| 能力 | 说明 |
|---|---|
| 实时监测 | 板载 AHT20 空气温湿度；没接时温度用 IMU 实测，湿度显示未接。土壤/光照/CO2 不仿真 |
| 横屏界面 | LVGL 800×480：主页环境、网络、日历、作物库。右下角小号农场管家会动，说话时右上小气泡，约 4 秒消失 |
| 语音问答 | "现在大棚温度多少？""湿度正常吗？""有什么报警吗？" |
| 语音设阈值 | "把温度上限改成 32 度"（AI 调用 SetThresholds / MCP 工具） |
| 本地报警 | 超阈值：屏幕红字 + 红灯常亮；连续 3 次采样去抖，避免误报 |
| 双通道 AI 感知 | IoT Thing 属性（states 注入上下文）+ MCP 工具（对话中按需拉取） |
| 串口调试 | `msh /> greenhouse_status` 一条命令看全部状态 |

## 仓库结构

```
edgi-talk-greenhouse/
├── README.md            ← 你在这里
├── overlay/             ← 拷入官方 BSP 工程的增量文件
│   ├── INTEGRATION.md   ★ 集成步骤（拷目录 → UI 补丁，共 2 步，已实测编译通过）
│   └── applications/xiaozhi/greenhouse/
│       ├── SConscript
│       ├── greenhouse_sensor.h/.cpp   采样线程+报警状态机+红灯+屏幕推送
│       ├── greenhouse_thing.cpp       小智 IoT Thing
│       └── greenhouse_mcp.cpp         小智 MCP 工具（self.greenhouse.*）
└── docs/
    ├── 01-方案设计.md          架构、数据流、扩展路线（二期接外传感器/控制/MQTT）
    ├── 05-界面框架.md          横屏多页面、气泡和管家的布局契约
    ├── 02-环境搭建与烧录.md    从零开始：Studio/多核烧写/WiFi blob/配网/绑定
    ├── 03-智能体人设配置.md    xiaozhi.me 控制台"大棚管家"角色 + 系统提示词全文
    └── 04-测试验收清单.md      语音测试问题、报警触发方法、预期结果
```

## 快速开始

1. 准备：Windows + RT-Thread Studio ≥ 2.2.9，克隆官方 BSP 仓库
   [`RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk`](https://github.com/RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk)，
   并确保板子已能跑原版小智固件（见 `docs/02-环境搭建与烧录.md`）。
2. 按 [`overlay/INTEGRATION.md`](overlay/INTEGRATION.md) 完成 2 步集成。
3. 到 [xiaozhi.me](https://xiaozhi.me) 控制台按 `docs/03-智能体人设配置.md` 建"大棚管家"智能体。
4. 按 `docs/04-测试验收清单.md` 逐项验收。

## 工作原理（一分钟版）

```
AHT20 ─i2c1─▶ 采样线程 gh_sens（10s，越界去抖 3 次）
                │
                ├─▶ 屏幕标签（温湿度/红色报警文本）
                ├─▶ 红灯 P16_7（报警常亮）
                │
                ├─▶ IoT Thing「Greenhouse」属性随 states 上报 ─┐
                └─▶ MCP 工具 self.greenhouse.* 对话中取值 ────┤
                                                                ▼
                          WSS(api.tenclass.net) ─▶ 小智官方云 LLM
                          （人设由 xiaozhi.me 控制台配置）
```

小智服务端在 WebSocket 握手后 `tools/list` 拉到本固件注册的工具、并收到 IoT
states，因此大模型"知道"大棚的存在；你提问时它调用 `self.greenhouse.get_report`
等工具拿到实时值，再用自己的话播报。

## 二期路线（本期未做，见 docs/01）

- 外接传感器：土壤湿度（ADC）、光照 BH1750、CO2（SCD4x / MH-Z19）——接口已预留
- 继电器控制（风机/水泵/补光灯）与阈值自动联动
- paho-mqtt 历史数据上报 + 自建 xiaozhi-esp32-server（人设/知识库/notify 主动播报）

## 相关链接

- 官方 BSP：https://github.com/RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk
- 小智工程文档：https://rt-thread-studio.github.io/sdk-bsp-psoc_e84-edgi-talk/latest/component/Edgi_Talk_M55_XiaoZhi/README.html
- 小智控制台：https://xiaozhi.me
- xiaozhi-esp32（协议上游）：https://github.com/78/xiaozhi-esp32
