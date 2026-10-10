# 贡献指南

感谢参与 PocketJS for Edgi-Talk。仓库是上游 BSP 的 overlay，开发目标是让每个改动都能在没有板子的环境中先得到清晰反馈，再进入完整固件验证。

## 开发前准备

- Python 3.10+、GCC、GNU Make。
- 完整固件构建还需要 `POCKETJS_ROOT`、`QUICKJS_ROOT`、Arm 工具链、Bun 和 RT-Thread BSP；变量默认值和覆盖方式见 [`projects/Edgi_Talk_M55_PocketJS/docs/编译与烧录.md`](projects/Edgi_Talk_M55_PocketJS/docs/编译与烧录.md)。
- 只改本仓库源码时，不需要安装外部 BSP，直接运行 `make source-check`。

## 常用命令

```sh
make help
make test                 # Python 上位机测试 + host synth
make source-check         # 源码-only CI
make check                # 自动选择 source-only 或完整检查
make build ARGS=-j8       # 重新构建 .pocket 和固件
make build-no-ui ARGS=-j8 # 复用已有 .pocket，只编固件
make preview              # 桌面预览
make release-check        # 发布前检查
make clean
```

## 修改边界

- 固件 C 和 RT-Thread 适配放在 `projects/Edgi_Talk_M55_PocketJS/applications/pocketjs/`。
- 板上 TypeScript/资源放在 `pocketjs-app/edgitalk-m55-smoke/`；`applications/pocketjs/generated/` 是生成物，不要手工编辑。
- 电脑端命令行、HTTP/BLE 客户端、mock 板放在 `tools/edgitalk-companion/`。
- 任何会改变协议大小、谱面限制或播放器限制的改动，都要更新 `contracts/chart-contract.json`，运行 `python3 projects/Edgi_Talk_M55_PocketJS/tools/check_contracts.py`，并同步 Python/C 两端。
- 上游 BSP 才需要的改动放 `bsp-patches/`，保持补丁最小且可重放。

## 变更流程

1. 从最新 `main` 开分支，改动保持单一主题。
2. 先运行 `make test` 或 `make source-check`，再运行 `make check`。
3. 修改外部依赖或预编译库时，确认哈希变化是预期的，再运行 `check_dependencies.py --update`，把锁文件和来源写入提交说明。
4. 完整环境至少执行一次 `make build`；如果修改了烧录、启动或硬件路径，再用 `make flash` 做板上验证。
5. 更新 `CHANGELOG.md` 的 `Unreleased` 或目标版本条目；发布版本同步修改 `VERSION`。
6. 提交前执行 `make release-check`，不要提交 `build/`、`Debug/`、缓存或个人绝对路径。

## 质量门槛

- 新的协议字段、资源上限或错误路径必须有 Python 单测或 host synth 测试。
- C 代码保持资源和输入边界明确；外部 JSON、HTTP、BLE 数据必须在板端再次校验。
- 不在源码、配置、文档或脚本中写个人 home 目录、用户目录或个人盘符路径。
- 提交信息建议使用 `feat:`、`fix:`、`test:`、`docs:`、`build:`、`chore:` 前缀，例如 `feat: add chart contract checks`。
