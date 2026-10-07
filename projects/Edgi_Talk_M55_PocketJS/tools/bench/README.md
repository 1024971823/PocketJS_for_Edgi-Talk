# 板上测量

通过 KitProg3 读内存。点击需要 `POCKETJS_DEBUG_TOUCH=1` 编的固件。参数和读数怎么看在 [docs/性能测量.md](../../docs/性能测量.md)。

```sh
python3 stage_bench.py [--song 0] [--home-only | --transition] [--window 3]
python3 shot.py [--no-reset] "wait:40000" "tap:330,20" "wait:2500" "shot:/tmp/a.png"
python3 scanout_check.py [--no-reset] [--stage]
python3 pc_profile.py [--script transition|stage|none] [--samples 1500]
```

`stage_bench.py` 打每帧耗时。`shot.py` 的坐标是 400×240 的界面坐标。`scanout_check.py` 对照局部送显和整帧旋转。`pc_profile.py` 不暂停 CPU，统计函数热点。
