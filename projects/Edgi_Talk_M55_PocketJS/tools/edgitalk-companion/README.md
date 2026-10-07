# Edgi Talk 上位机

Python 3 标准库。和板子在同一局域网，配对码在板子的设置页。完整说明（窗口、谱面格式、每个 HTTP 接口）在 [docs/电脑上位机.md](../../docs/电脑上位机.md)。

```sh
python3 edgitalk.py discover
python3 edgitalk.py pair 192.168.1.23 123456
python3 edgitalk.py status
python3 edgitalk.py scores

python3 edgitalk.py stats                      # 发一次本机 CPU/内存/磁盘/温度和时钟
python3 edgitalk.py stats --watch --interval 2
python3 edgitalk.py console                    # 浏览器页面。gui 是同一个页面。只填配对码，地址自己找

python3 edgitalk.py demo -o chart.json
python3 edgitalk.py validate chart.json
python3 edgitalk.py push chart.json            # 选歌页的 PC 卡
python3 edgitalk.py pull out.json
python3 edgitalk.py clear
python3 edgitalk.py make-chart in.mid -o chart.json --title "My Song" --level 2

python3 edgitalk.py mock                       # 127.0.0.1:8080，配对码 123456
```

`pair` 写到 `~/.edgitalk.json`。`--host`、`--token`、`--port` 临时覆盖。请求不走 `http_proxy`。

`stats` 在装了 `psutil` 时用它，否则 Linux 读 `/proc` 和 `/sys`，Windows 用系统 API。读不到温度时板子显示 N/A。自定义曲最大 96 KiB、2000 个音频事件。

测试：`python3 -m unittest test_edgitalk.py`。
