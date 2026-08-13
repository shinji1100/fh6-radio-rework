# FH6 Radio Rework

一个针对《Forza Horizon 6》PC 版的轻量电台注入器，第一版专注于 **External Audio / QQ 音乐**。

本项目不是在原版 Universal Radio 上继续堆补丁，而是把实时音频链路重新拆分：WASAPI 捕获线程直接向 SPSC 音频环形缓冲写入，FMOD mixer 回调直接读取；控制台、设备枚举和诊断全部放在实时音频路径之外。

> 非 Playground Games / Microsoft 官方项目。修改游戏进程始终存在兼容性与线上模式风险，请自行决定是否使用。

## v0.1 目标

- QQ 音乐 / Apple Music / 浏览器等 Windows 音频通过虚拟播放设备进入 FH6 电台。
- WASAPI loopback 使用 event callback，而不是 `Sleep(5)` 轮询。
- 捕获线程加入 MMCSS `Pro Audio` 调度。
- 48 kHz 输出带轻微异步时钟漂移补偿，避免两个“名义 48 kHz”设备长期累积缓冲误差。
- 音频数据：`WASAPI -> adaptive resampler -> SPSC frame ring -> FMOD DSP`，无中间 `vector + mutex + 20 ms pump`。
- SPSC reset 由 mixer consumer 独占执行；控制线程只能请求 reset，避免并发改写 read index。
- ring 满时丢弃整段旧 backlog 并重新预缓冲，而不是暂停后继续播放过时音频。
- FMOD 回调**绝不增加**游戏分配的输出声道数。
- 默认使用 mono 内容复制到 FMOD 已分配的全部声道，规避 3D radio panner 的相位问题。
- 只绑定 `127.0.0.1` 的中文控制台，不向局域网暴露配置接口。
- 通过 RadioState 确认当前选择的是 Streamer Mode，再自动锁定唯一 active `RadioStreamFmod`；不再写死某一首 carrier。

## 暂时移除

第一版刻意不包含以下功能：Spotify Connect、YouTube Music 下载、Jellyfin、在线电台、本地文件播放、D3D12 游戏内封面替换、全局热键、比赛开始自动切歌。

这些功能以后可以作为独立模块加回，前提是不进入实时 mixer/capture 热路径。

## 安装

1. 从 GitHub Actions 的 `fh6-radio-rework` artifact 取得 `version.dll`。
2. 将 `version.dll` 放到 `forzahorizon6.exe` 同目录。
3. 启动游戏后打开 `http://127.0.0.1:8420`。
4. FH6 中启用 **Streamer Mode**，关闭 **Radio DJ**，并选择 Streamer Mode 电台；插件会自动锁定当前 active radio stream，直到控制台显示 `FH6 DSP 已接入`。
5. 在 Windows 音量混合器中，把 QQ 音乐输出设为一个独立播放设备，例如 `CABLE Input (VB-Audio Virtual Cable)`。
6. 在控制台选择同一个播放设备并点击“应用设备并重启捕获”。

信号路径：

```text
QQ 音乐
  -> CABLE Input（Windows 播放设备）
  -> WASAPI loopback
  -> 自适应重采样 / 时钟漂移补偿
  -> SPSC frame ring
  -> FH6 FMOD DSP
  -> 游戏 Radio Bus
  -> 耳机 / 音箱
```

## 配置

首次启动会生成：

```text
fh6-radio-rework/config.ini
```

示例：

```ini
endpoint_id=
gain=1.0
native_stereo=false
dashboard_port=8420
```

`native_stereo=false` 是推荐默认值。它不会把 FMOD 的 mono buffer 强制改成 stereo；而是在 FMOD 已分配的通道布局内写入相同的 mono 内容。

## 诊断

中文控制台实时显示：

- WASAPI 是否运行、输入采样率/声道数
- 捕获包数量、discontinuity、event timeout
- SPSC ring 当前填充与 overflow
- 当前 Station 是否为 Streamer Mode，以及 active radio stream 是否找到
- DSP 是否成功挂到 FMOD channel
- mixer callback 次数与 underrun frame 数

如果“捕获运行中”但 DSP 未接入，先在 FH6 中切换一次电台。如果 DSP 已接入但 buffer 长期 0，检查 QQ 音乐和控制台是否选择了同一个播放设备。

要自动确认当前 Radio 的 FMOD 2D/3D、stream 和直接 Studio Event 身份，请看 [RADIO_ACOUSTICS_DIAGNOSTIC.md](RADIO_ACOUSTICS_DIAGNOSTIC.md)。检测器会在每次游戏进程首次找到 active Streamer Mode stream 时运行一次。

## 构建

需要 Windows x64、CMake 3.24+、Visual Studio 2022 C++ 工具链。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物：

```text
build/Release/version.dll
```

仓库包含 GitHub Actions 工作流，可在 `windows-latest` 上自动构建。

## 安全设计

- Dashboard 仅监听 `127.0.0.1`。
- 不发送 `Access-Control-Allow-Origin: *`，并校验 `Host`；有 `Origin` 的 POST 只接受本机同源。
- HTTP header 最大 16 KiB、body 最大 64 KiB，socket 收发超时 2 秒。
- v0.1 不包含外部下载器、浏览器 cookies、Jellyfin API key 或 Spotify 凭据。
- 不包含 D3D12 texture hook。

## 来源与许可

本项目使用并重构了 `g0ldyy/fh6-universal-radio` 中与 FH6/FM​OD 发现相关的低层实现思路及部分派生代码，包括 PE 解析、签名扫描、`RadioStreamFmod` RTTI/heap discovery 与 `version.dll` proxy exports。原项目采用 GPL-3.0，因此本项目同样以 **GPL-3.0** 发布。

音频捕获、SPSC frame ring、event-driven WASAPI 管线、自适应时钟漂移补偿、重新设计的 DSP buffer contract、localhost 控制台与 v0.1 架构为本分支重写。

## 免责声明

这个项目通过 `version.dll` proxy 进入游戏进程并使用运行时扫描定位 FMOD 内部对象，游戏更新可能导致失效。不要把“能够启动”理解为线上模式零风险保证。
