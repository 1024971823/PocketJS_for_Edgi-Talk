# 桌面预览

用固件里的 PocketJS C、QuickJS 和 Rust 渲染器，在 x86 上跑已经打好的 `.pocket`，脚本点击后把 PPM 转成 PNG。完整步骤在 [docs/桌面预览.md](../../docs/桌面预览.md)。

```sh
tools/preview/build.sh [perfect|idle] [歌曲序号] [帧数]
```

默认 `perfect`、歌曲 2、2000 帧。截图和损伤统计写到 `/tmp/pjs-preview`（`PREVIEW_OUT` 可改）。

先要有 gcc、bun、python3（Pillow），以及编好的主机库：

- `$PREVIEW_OUT/cargo/ui-core/release/libpocketjs_idf_ui_core.a`
- `$PREVIEW_OUT/cargo/render-rgb565/release/libpocketjs_idf_render_rgb565.a`

这是 x86 库，不是烧到板子上的那份。桌面上的毫秒数不能当板子帧率；看区域数和有没有整屏重绘。`synth_test.c` 在主机上单独跑合成器。
