# Edgi-Talk M55 PocketJS（Beat Dash）

Edgi-Talk 的 M55 固件。PocketJS 画一个 400×240 的界面，放大到 800×480。主页是仪表盘，里面有双轨节奏游戏 Beat Dash、日历，以及电脑推过来的 CPU / 内存 / 磁盘 / 温度。三首歌在板上合成。玩游戏不需要电脑；传自定义曲和看电脑状态需要上位机。

完整用法在 [docs/](docs/README.md)：

| 文档 | 内容 |
| --- | --- |
| [板上使用](docs/板上使用.md) | 主页、打歌规则、日历、电脑监控页、设置、SD 卡音乐、配网 |
| [电脑上位机](docs/电脑上位机.md) | 发现和配对、推送本机状态、窗口、谱面格式、HTTP 接口 |
| [编译与烧录](docs/编译与烧录.md) | 打 `.pocket`、编固件、只烧 M55 |
| [桌面预览](docs/桌面预览.md) | 在 x86 上跑同一份界面 |
| [性能测量](docs/性能测量.md) | 用 KitProg3 测帧率和截屏 |

## 目录

| 路径 | 内容 |
| --- | --- |
| 本目录 | RT-Thread 工程、`applications/pocketjs/`、上位机、预览和测量脚本 |
| `b/work/pocketjs/apps/edgitalk-m55-smoke/` | 界面和游戏的 TypeScript，打包后嵌进固件 |
| `b/work/quickjs-ng/` | QuickJS |
| `b/work/build-edgi-talk.sh`、`flash-xiaozhi-daplink.sh` | 编译和烧录 |

## 板上，最短的一条路

- PLAY（或主页上滑）进选歌，点一张卡，再点一下屏幕开始。上半屏打蓝色空气轨，下半屏打红色地面轨。EXIT 退出。
- 日期胶囊是日历，今天是深青色实心圆。右侧四环点进去是电脑监控页。Wi-Fi 胶囊或菜单是设置，里面有音频偏移和六位配对码。
- 没保存过 Wi-Fi 时，板子开热点 `EdgiTalk-……`，手机连上后打开 `http://192.168.169.1` 选网络。连上后设置页显示局域网 IP。

判定、计分、偏移和时间从哪来，见 [板上使用](docs/板上使用.md)。

## 电脑，最短的一条路

```sh
cd tools/edgitalk-companion
python3 edgitalk.py pair <设置页里的 IP> <六位配对码>
python3 edgitalk.py stats --watch          # 板上的 PC MONITOR 开始刷新
python3 edgitalk.py console                # 浏览器页面：只填配对码，地址自己找
python3 edgitalk.py push chart.json        # 选歌页的 PC 卡
```

请求不走 `http_proxy`。假板子：`python3 edgitalk.py mock`（`127.0.0.1:8080`，配对码 `123456`）。细节和谱面格式见 [电脑上位机](docs/电脑上位机.md)。

## 改完再烧

```sh
cd /home/Srakoul/Documents/Codex/2026-09-27/b/work/pocketjs
~/.bun/bin/bun tools/pocket.ts build \
  --host-profile apps/edgitalk-m55-smoke/pocket.host.json \
  --manifest apps/edgitalk-m55-smoke/pocket.json

cd /home/Srakoul/Documents/Codex/2026-09-27/b/work
./build-edgi-talk.sh Edgi_Talk_M55_PocketJS -j8
M55_PROJECT=Edgi_Talk_M55_PocketJS ./flash-xiaozhi-daplink.sh m55
```

`m55` 只写 M55，不动 M33。不设 `M55_PROJECT` 会烧成另一个工程。只改了 C、没改界面时，可以跳过第一条。步骤说明见 [编译与烧录](docs/编译与烧录.md)。
