# FH6 Radio 声学自动检测器

这个检测器用于回答一个具体问题：插件接管的 Streamer Mode 电台，当前到底走的是 2D 还是 3D FMOD 路径，以及是否能直接依赖原生 Radio Event 做车内/车外声学。

检测完全自动、只运行一次，不需要打开调试器，也不会修改 FMOD bank。它在每次游戏进程首次找到 active Streamer Mode stream 时触发，并把结论写入插件日志。

## 如何使用

当前构建已经安装到：

```text
D:\Forza Horizon 6\version.dll
```

1. 用 `D:\Forza Horizon 6\forzahorizon6_loader.exe` 启动游戏。
2. 进入自由驾驶。
3. 把电台切换到 **Streamer Mode**。
4. 等待约 5～15 秒，让 active radio stream 开始播放。
5. 打开日志：

   ```text
   D:\Forza Horizon 6\fh6-radio-rework\fh6-radio-rework.log
   ```

6. 搜索 `[radio-acoustics]`；最重要的是带 `DECISION:` 的一行。

检测器每次游戏启动只运行一次。要重新检测，请正常退出游戏，再通过 loader 重新启动。

## 正常结果

当前 FH6 build 的实机结果是：

```text
[radio-acoustics] Sound::getMode rc=0 mode=0x0003008A is2D=true is3D=false createStream=true
[radio-acoustics] direct Radio Event identity -> CONFIRMED is3D=false isStream=false [PROVEN]
[radio-acoustics] DECISION: active Core Sound=2D STREAM [PROVEN]; direct Studio Event is3D=false isStream=false [PROVEN]; use the custom cabin DSP for interior/exterior acoustics
```

含义：

- 实际播放的 FMOD Core Sound 是 **2D + 流式**。
- 与该 Radio wrapper 直接关联的 Studio Event 是 **非 3D、非流式 Event**。
- 所以车内低通、箱体共振、车外衰减等效果应由本项目自己的 cabin DSP 完成，不能指望这个 Event 自带 3D 声学。

这个结论只覆盖插件实际接管的 active Radio stream 和它的直接 Studio Event；它不是“FH6 中不存在其他 3D Music/Radio 资源”的全局证明。

## 其他关键行

```text
[radio-acoustics] wrapper Studio handle ... -> MATCH [PROVEN]
```

说明 Radio wrapper 与此前验证的 Studio System 属于同一个 FMOD 身份链。

```text
[controller] active Streamer Mode stream='HZ6_...'
```

说明插件已经锁定当前载体曲目。

如果只有 `waiting for Streamer Mode`，请确认已经进入自由驾驶并切换到 Streamer Mode；必要时切到其他电台再切回来。

如果没有任何 `[radio-acoustics]`，先确认游戏目录中的 `version.dll` 是当前构建，并查看日志中是否出现 `[controller] active Streamer Mode`。

