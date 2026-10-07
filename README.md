# PocketJS for Edgi-Talk

在 RT-Thread 的 PSoC E84 Edgi-Talk BSP 上跑的 PocketJS 应用：节奏游戏、电脑监控，以及和 Wi-Fi 同时工作的蓝牙状态通道。

上游板级包是 [sdk-bsp-psoc_e84-edgi-talk](https://github.com/RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk)。这个仓库只放本工程新增的内容和需要打到上游上的补丁，不是完整 BSP。

## 目录

- `projects/Edgi_Talk_M55_PocketJS/`：M55 固件工程、使用文档、电脑上位机。
- `pocketjs-app/edgitalk-m55-smoke/`：板上 QuickJS 界面的源码。
- `libraries/components/bt-fw-ifx-cyw55500a1/`：CYW55513 蓝牙控制器补丁。固件用其中 `COMPONENT_wlbga_iPA_sLNA_ANT0_LHL_XTAL_IN`。
- `bsp-patches/`：打到上游 BSP 上的三处改动。LCD 局部刷新、蓝牙串口 `uart4`、Wi-Fi 侧再次设置 `btc_mode=1`。

把本仓库里的 `projects/` 和 `libraries/` 覆盖到上游 BSP 同名路径，再按 `bsp-patches/` 里的补丁改对应文件。界面源码在单独的 PocketJS 仓库里构建，产物已经放在固件工程的 `applications/pocketjs/generated/`。

## 使用

板上用法、编译烧录和上位机见 `projects/Edgi_Talk_M55_PocketJS/docs/`。

上电后蓝牙补丁下载大约需要 45 秒。设置页出现 `BT EdgiTalk` 之后，上位机只填六位配对码即可。状态走蓝牙，自定义曲走 Wi-Fi，两边同时开着。
