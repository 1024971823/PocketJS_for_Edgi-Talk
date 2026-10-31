# 集成指南：把大棚管家合入官方 Edgi Talk 小智工程

本目录（`overlay/`）的所有内容都是为官方 BSP 工程
[`RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk`](https://github.com/RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk)
中的 `projects/Edgi_Talk_M55_XiaoZhi` 准备的增量文件。

**当前版本（2026-11-01）是一次整目录覆盖，不再有 `xiaozhi_ui.c` 补丁。**
overlay 携带三个目录，全部拷进 `applications/xiaozhi/` 即可：

| 目录 | 内容 |
|---|---|
| `greenhouse/` | 传感器采样/报警 + IoT Thing + MCP 工具 |
| `ui/` | 横屏多页面 LVGL：主页/网络/日历/作物/设置 + 手绘管家 + NTP 校时接入 |
| `sensors/` | `sensor_monitor`（真实 AHT20/IMU/电池/WiFi）+ `gh_ntp` 网络校时 |

`ui/`、`sensors/` 与官方目录同名：官方这两个 `SConscript` 本身就
`Glob('*.c')`，整目录拷过去即被自动编译，**不删不改任何官方构建文件**。
旧版 4 处 `xiaozhi_ui_set_greenhouse` 补丁已废弃（见文末「历史版本」），勿再打。

---

## 前置条件

- 已按官方文档搭建环境并烧录过原版小智固件（RT-Thread Studio ≥ 2.2.9 或
  scons + arm-none-eabi 工具链均可），见 `docs/02-环境搭建与烧录.md`。
- 以下假设 BSP 仓库在 `<BSP>`，小智工程即 `<BSP>/projects/Edgi_Talk_M55_XiaoZhi`。

---

## 步骤 ①：拷贝 `greenhouse/`（传感器 + IoT + MCP）

把 `applications/xiaozhi/greenhouse/` 整个目录拷到
`<BSP>/projects/Edgi_Talk_M55_XiaoZhi/applications/xiaozhi/greenhouse/`。
含 5 个文件：`SConscript`、`greenhouse_sensor.h/.cpp`（裸 I2C AHT20 驱动，
自动探测 i2c1/i2c2/i2c0 + 采样线程 + 报警状态机 + 红灯 + MSH 命令）、
`greenhouse_thing.cpp`（IoT Thing，属性随 states 上报）、
`greenhouse_mcp.cpp`（`self.greenhouse.*` 工具）。

> 官方 `applications/xiaozhi/SConscript` 会自动发现带 `SConscript` 的子目录，
> 不需要改任何官方文件。

## 步骤 ②：拷贝 `ui/` 与 `sensors/`（横屏界面 + 校时）

把 `applications/xiaozhi/ui/` 与 `applications/xiaozhi/sensors/` 整目录拷到
`<BSP>/projects/Edgi_Talk_M55_XiaoZhi/applications/xiaozhi/` 下（覆盖/合并官方
同名目录里的 `*.c`）。这两个官方 SConscript 本来就是 `Glob('*.c')`，拷入即编译，
无需 SConscript 改动：

```
ui/       gh_ui_layout.h(设计令牌+布局)  gh_ui_pages.h(页面引擎接口)
          xiaozhi_ui.c(页面管理/顶栏/告警条/气泡/标签)  gh_butler.c/.h(手绘管家)
          gh_page_main/net/cal/crop/set.c(5 个页面)
sensors/  sensor_monitor.c/.h(AHT20/IMU/电池/WiFi 真实快照 + 报警状态机)
          gh_ntp.c/.h(SNTP v4 校时，联网自动 + 设置页手动 + msh `ntp_sync`)
```

> 需要横屏：`rtconfig.h` 里 `BSP_LCD_ROTATION_DEGREES` 设为 90（官方 Kconfig
> 默认就是 90° landscape），且 `lv_conf.h` 关闭 `LV_USE_PERF_MONITOR`。

---

## 历史版本（已废弃，勿用）

以下是旧单页版本的 4 处 `xiaozhi_ui_set_greenhouse` 补丁；当前多页面 UI 已自带
温室显示，**不要**打进 `ui/xiaozhi_ui.c`。保留仅作对照。

### 补丁 1/4：`ui/xiaozhi_ui.h` 增加函数声明

在 `void xiaozhi_ui_set_adc(const char *adc_str);` 附近（任意 public 函数声明区）加入：

```c
/**
 * @brief Update greenhouse environment display (temperature/humidity/alarm)
 * @param text Text to display, e.g. "大棚 温度25.3℃ 湿度60.5% 正常"
 */
void xiaozhi_ui_set_greenhouse(const char *text);
```

### 补丁 2/4：`ui/xiaozhi_ui.c` 增加 UI 命令与标签变量

2a. 在 `ui_cmd_t` 枚举中，`UI_CMD_UPDATE_CHARGE_STATUS` 之后加一行：

```c
    ,UI_CMD_SET_GREENHOUSE
```

2b. 在 `static lv_obj_t *s_label_output;` 附近加入：

```c
static lv_obj_t *s_label_greenhouse;  /* Greenhouse env label */
```

### 补丁 3/4：`ui/xiaozhi_ui.c` 的 `ui_objects_init()` 末尾创建标签

在函数末尾（`#endif` 之后、`return RT_EOK;` 之前）插入：

```c
    /* Greenhouse environment label - below the header row */
    s_label_greenhouse = lv_label_create(screen);
    lv_label_set_long_mode(s_label_greenhouse, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(s_label_greenhouse, &s_style_24, 0);
    lv_obj_set_width(s_label_greenhouse, LV_PCT(80));
    lv_obj_set_style_text_color(s_label_greenhouse, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_label_greenhouse, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(s_label_greenhouse, LV_ALIGN_TOP_MID, 0, SCALE_DPX(HEADER_HEIGHT + 8));
    lv_label_set_text(s_label_greenhouse, " ");
```

### 补丁 4/4：`ui/xiaozhi_ui.c` 消息处理 + 公共函数

4a. 在 `ui_process_message()` 的 `switch` 中，`case UI_CMD_UPDATE_CHARGE_STATUS:` 分支之后加入：

```c
    case UI_CMD_SET_GREENHOUSE:
        if (s_label_greenhouse)
        {
            lv_label_set_text(s_label_greenhouse, msg->data[0] ? msg->data : " ");
            /* 报警文本以 "⚠" 开头时显示为红色（UTF-8 "⚠" 首字节 0xE2） */
            lv_obj_set_style_text_color(s_label_greenhouse,
                ((unsigned char)msg->data[0] == 0xE2) ? lv_color_hex(0xff4040) : lv_color_hex(0xffffff),
                LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        break;
```

4b. 在文件 Public Functions 区（如 `xiaozhi_ui_set_adc` 之后）加入：

```c
void xiaozhi_ui_set_greenhouse(const char *text)
{
    ui_send_message(UI_CMD_SET_GREENHOUSE, text, " ");
}
```

---

## 步骤 ③：编译、烧录、验证

1. 编译 `Edgi_Talk_M55_XiaoZhi`（Studio 一键编译，或 scons），确认无错误
   （会多编译一个 `xiaozhi_greenhouse` 组）。
2. 按既有流程烧录 M55 固件。
3. 串口验证（115200-8N1）：

```
msh /> greenhouse_status
sensor found      : yes
data valid       : yes
temperature      : 25.3 C
humidity         : 60.5 %RH
alarm            : normal
thresholds       : T[5,35]C H[30,90]%
```

启动日志应出现 `AHT20 found on bus 'i2cX'`（X 为实际总线）。
若 `sensor found : no`：这块板可能未焊载 AHT20 或它在拓展板上（早期/海外
宣传口径有差异），此时可外接 AHT20 模块到 40-pin 的 I2C 引脚。

4. 配网绑定（首次见 `docs/02-环境搭建与烧录.md`），然后对小智说：
   "现在大棚的温度是多少？"、"有什么报警吗？"。人设配置见
   `docs/03-智能体人设配置.md`。

---

## 已知行为说明

- **报警红灯**使用 P16_7（红 LED，与官方 Led Thing 同引脚）：报警时常亮、解除后熄灭。
  若语音让 AI "打开红灯"，下一次采样刷新报警状态时可能把它改回去——属预期行为。
- **阈值掉电不保存**：语音设置的新阈值保存在内存中，重启后回到默认 温度 5~35℃ / 湿度 30~90%。
  持久化（easyflash/fal）列入二期。
- **温度/湿度属性为 0.1 精度整数**（253 = 25.3℃），属性描述里已向大模型说明，属正常现象。
- 若屏幕温湿度标签显示不出 "℃" 字形（字体子集不含该符号），把
  `greenhouse_sensor.cpp` 中 `℃` 改成 `C`、`⚠` 改成 `!` 即可。
- 传感器读失败会自动重新探测总线（热插拔 AHT20 模块可恢复）。
