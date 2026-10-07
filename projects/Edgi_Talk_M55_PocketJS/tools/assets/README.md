# 界面素材

`make_rings.py` 生成主页和电脑监控页用的仪表环：`ring_track.png`，以及 `ring_00.png` 到 `ring_20.png`（0% 到 100%，每 5% 一张）。每张都有 `@2x`。图片写到 PocketJS 应用目录 `b/work/pocketjs/apps/edgitalk-m55-smoke/`。

```sh
python3 make_rings.py
```

需要 Pillow。生成之后要重新打 `.pocket` 并烧录，见 [docs/编译与烧录.md](../../docs/编译与烧录.md)。环的尺寸和颜色要和 `ui/kit.tsx` 里的 `Ring` 一致，改一边就要改另一边。
