# FH6 Radio Rework — 工程交接

更新时间：2026-08-14 02:20（Asia/Shanghai）  
仓库：`https://github.com/shinji1100/fh6-radio-rework`  
本地：`C:\Users\f1140\WorkBuddy\2026-08-13-08-01-31\fh6-radio-rework`  
游戏：`D:\Forza Horizon 6`  
分支：`sig-scout`

## 0. 接手者先读这里

当前主线不是继续盲扫 FMOD，也不是直接把 18 个 Cockpit IR 卷到音乐上，而是：

1. 保持已经能工作的 `WASAPI -> ring -> FH6 FMOD DSP` 外部音乐链路。
2. 用插件自己的虚拟扬声器和座舱 DSP 给 Streamer Mode 音乐做车内声学。
3. 把过去的“车内/车外二态”升级为六个驾驶视角。
4. 再做实时 A/B、响度校准、实测 HRTF、车辆类别拟合和敞篷状态。

当前代码 HEAD 是 `56228414938d058d03b39e8587ca5e229dbcffa9`，提交名：

```text
track six camera views from input
```

对应 GitHub Actions：

- run：`31729807364`
- `portable-tests`：success
- `windows-x64`：success
- artifact：`9192770287`
- artifact ZIP SHA-256：`64CA1BAC97DE312162826E7F1A65569BB1CB600EEB0326741425DC264D3AD875`
- DLL SHA-256：`2D58D4F38118BFA437359A4B1E1AA6554B4F6A3B1626B375AED2CE89873E5189`
- 已暂存：`D:\Forza Horizon 6\fh6-radio-rework\staging\5622841\version.dll`

写本文档时游戏仍在运行，因此 `5622841` 尚未覆盖到游戏根目录。当前游戏加载的是上一版 `db8f26b`，DLL SHA-256：

```text
DD56B646CE88553F85C02C01996B3E86B53B7174DCB771D5EA631AB64CB648B7
```

接手后的第一件事：让用户正常退出游戏，备份根目录 `version.dll`，安装暂存的 `5622841`，启动游戏，然后按本文第 8 节验证六态计数。不要在游戏进程运行时强制覆盖 DLL。

## 1. 用户最终目标和已确认偏好

用户要的是 FH6 Streamer Mode 的完整自定义车载电台体验：

- QQ 音乐等外部播放器的音频进入 FH6 原生 FMOD 链路。
- HUD Now Playing 显示外部媒体标题和艺术家。
- 参数化虚拟车载扬声器布局。
- 座舱反射、衰减、EQ、敞篷开放度。
- 可选 binaural/HRTF，尽可能有立体空间感。
- 不同车辆通过测得数据和公式拟合，不为每辆车手工制作一套硬编码。
- 六个驾驶视角必须能区分：
  - Dashboard / 仪表盘（Driver）
  - Cockpit / 驾驶舱（可见方向盘）
  - Chase Near
  - Chase Far
  - Hood
  - Bumper / 车头低位
- 换车会经过黑屏，换车时允许直接重置，不要求淡化。
- 敞篷开合没有同样的黑屏，开放度必须平滑变化。
- 用户已明确觉得旧版“能听出效果，但很简陋”，并指出第三人称和第一人称曾经听起来一样、音量偏小。

交流要求：把“代码写了”“CI 通过”“DLL 已安装”“实机验证通过”分开报告，不能混为一谈。

## 2. 当前已验证能力

### 2.1 外部音频注入

已验证：

```text
Windows 播放设备 loopback -> WASAPI capture -> SPSC ring -> FMOD custom DSP -> FH6 Radio Stream
```

关键文件：

- `src/wasapi_capture.cpp`
- `include/fh6r/audio_ring.hpp`
- `src/fmod/dsp_bridge.cpp`
- `src/fmod/controller.cpp`

安全约束：

- 只在选中 `Streamer Mode` 时接管。
- 唯一 live RadioStreamFmod 候选才 attach；多候选时拒绝猜测。
- FMOD packed handle 的 resolver/unlock 是必需条件。
- 普通 FH6 电台不应受影响。

### 2.2 元数据

已验证：Windows Media Session 能读取外部播放器标题/艺术家，并写入 FH6 RadioStreamFmod 元数据体：

- `body + 0x30`：标题
- `body + 0x50`：艺术家

关键文件：

- `src/media_session.cpp`
- `src/fmod/metadata_injector.cpp`

### 2.3 Radio 声学身份

已验证：当前 active Radio 的 Core Sound 是 2D stream，直接 Studio Event 也不是 3D Event。不要再把“复用现成 Radio 3D event”当作主线。

关键运行证据曾出现：

```text
[radio-acoustics] Sound::getMode ... is2D=true is3D=false createStream=true
[radio-acoustics] direct Radio Event identity -> CONFIRMED is3D=false isStream=false [PROVEN]
```

所以现行架构决定是自建 cabin DSP。

### 2.4 Cabin DSP

`CabinDSP` 已接入热路径并经过用户实机听感确认。当前包含：

- 2/4/6/8 虚拟扬声器布局。
- 解析式 ITD、头影低通和可选 binaural。
- FH6 IR 数据拟合出的稀疏早期反射时序。
- 四线 Householder FDN 尾响。
- 车辆声学类别 EQ。
- 40 ms 左右视角参数过渡。
- 敞篷开放度约 50 ms 平滑。
- 软限幅器。

关键文件：

- `include/fh6r/cabin_dsp.hpp`
- `src/cabin_dsp.cpp`
- `tests/cabin_dsp_test.cpp`

## 3. 18 个 IR 数据结论

数据目录：

```text
C:\Users\f1140\WorkBuddy\2026-08-13-08-01-31\cockpit_ir_extracted
```

仓库分析工具和结果：

- `tools/analyze_cockpit_irs.py`
- `analysis/cockpit_ir_metrics.csv`
- `analysis/cockpit_ir_metrics.json`
- `analysis/README.md`

结论：

- 18 个 IR 中有 4 声道和 8 声道文件。
- 8 声道文件的声道能量跨度约 58–81 dB，矩阵语义未知，不能直接当音乐卷积核。
- 选取 10 个较平衡的 4 声道 IR 做时序/衰减拟合。
- 当前拟合值：
  - decay：约 `0.167644 s`
  - wet：约 `0.154019`
  - reflection：约 `0.633395`
- IR 低频传递增益经常超过 20 dB，更像发动机、底盘、碰撞等外部 SFX 进入驾驶舱的传递函数，不是“车载扬声器到耳朵”的音乐响应。

因此当前只借用反射到达时序和衰减，不复制其大幅低频增益。除非先证明声道矩阵和物理路径，否则不要把 18 个 IR 全带宽直接卷到音乐上。

## 4. 六视角调查：哪些路失败了

### 4.1 旧二态

提交 `68b0162` 使用 `.data + 0x397C0` 做车内/车外门控。用户实机听到切换，但它只得到两个声音：两个车内视角一组，四个车外视角一组。

这个地址是 build-specific，而且后续检查显示附近还有大量瞬态状态，因此 `5622841` 已不再依赖该静态地址。

### 4.2 六视角静态 `.data` 映射器

提交 `db8f26b` 加入两轮六标签映射器：

- 第一轮抓到 15 个看起来像多态的候选。
- 第二轮相同六标签重复采集后，稳定候选为 0。

日志证据：

```text
[viewmap] round 1: 15 candidate(s) retained for repeat verification
[viewmap] round 2: 0 stable multi-view candidate(s)
```

这证明标签操作没有问题，但真正的视角枚举不在当前扫描的静态 `.data` 小整数范围内。不要再次让用户重复同样的 12 次采集。

### 4.3 Dashboard -> Cockpit -> Dashboard 三次差分

曾出现 `+0x397A8` 等回归候选，但连续实时读取后发现它们会在不改变最终视角时自行翻转，是切换/输入过程瞬态，不能固化。

### 4.4 FMOD listener

连续采样证明 listener 的位置/俯仰能形成五个稳定几何簇，足够区分大多数外部视角；但 Dashboard 和 Cockpit 会共享相同 listener 位置和方向，因此 listener 本身无法完成六态。

### 4.5 对已验证静态标志的反向追踪

新增 `tools/find_rip_xrefs.py`，用 PE 头解析、NumPy 候选扫描和 Capstone 验证 RIP-relative xref。

对 `.data + 0x397C0` 只找到直接初始化写入：

```text
RVA 0x208C88: mov byte ptr [rip + ...], 0
```

没有得到可靠的动态相机枚举写入点。这也是放弃静态标志主线的原因。

该工具运行需要 `numpy` 和 `capstone`；本轮把 Capstone 临时装在：

```text
%TEMP%\fh6r-capstone
```

## 5. 当前六态方案（闭环几何跟踪器，取代 5622841 的开环计数）

### 5.1 视角跟踪（CameraTracker）

`include/fh6r/camera_tracker.hpp` + `src/camera_tracker.cpp`，由 `src/fmod/controller.cpp` 的 100 Hz 相机线程驱动。真值来源是游戏自己的 FMOD listener 位姿（`System::get3DListenerAttributes`，启动时特征码/锚点解析，无硬编码地址），输入边沿（Tab/RB）只是辅助信号：

- 切换事件 = 几何跳变（位置/朝向瞬时突变，超出平滑驾驶运动量）或输入边沿。
- 在线聚类：运动补偿（慢 EMA 基线跟踪车身）后的相对位置 + fwd/up 方向。六个视角占五个几何簇（Dashboard 与 Cockpit 共享 listener 位姿，已实测）。
- 环序标注：已确认循环 Dashboard→Hood→Bumper→ChaseNear→ChaseFar→Cockpit 映射到簇转移 I→H→B→N→F→I 加唯一自环 I→I（Cockpit→Dashboard）。因此观测转移序列即可绝对标注簇：自环证明 interior；进入 interior 证明 Cockpit；离开证明 Hood；外部跳变沿环传播。
- 到达已标注簇即 snap 到绝对视角（自动纠错）。未收敛时退化为边沿计数，不劣于旧方案。
- 自环 bootstrap：移动中 |rel| 半径先验（停车时失效，有速度门控）或跨两次访问的"无边沿跳变"确认；同一次访问内的菜单乱按不会误标注。
- 矛盾防御：interior 的合法前驱只有 ChaseFar；自环只合法于 interior。违反即降级标签并退回计数，防止错误标注永久污染。
- 换车（新 channel handle）时 reset：丢弃已学簇，重新收敛（约两个完整循环）。

手动 `POST /api/camera/sync` 保留为调试兜底，并会立即把当前簇标注为所选视角（加速收敛），但正常运行不依赖它。

遥测（`GET /api/state`）：
- `controller.camera_view / camera_anchored / camera_labeled / camera_clusters / camera_events`
- `dsp.applied.{mode,mix,gain,width,cutoff_hz}`：当前实际作用于音频路径的平滑参数，用于证明视角切换确实生效。

### 5.2 每视角声学参数

当前参数在 `src/cabin_dsp.cpp` 的 `view_render()`：

| 视角 | cabin_mix | wet_scale | stereo width | gain | low-pass |
|---|---:|---:|---:|---:|---:|
| Cockpit | 1.00 | 1.00 | 1.00 | 1.00 | 18 kHz |
| Dashboard | 1.00 | 0.66 | 0.82 | 1.02 | 15.5 kHz |
| Chase Near | 0.00 | 0.00 | 0.58 | 1.00 | 15 kHz |
| Chase Far | 0.00 | 0.00 | 0.40 | 0.94 | 10.5 kHz |
| Hood | 0.00 | 0.00 | 0.74 | 1.03 | 16.5 kHz |
| Bumper | 0.00 | 0.00 | 0.62 | 1.00 | 13.5 kHz |

Dashboard 对比度已提高到可闻阈以上（原 0.84/0.92 与 Cockpit 几乎不可区分，是"车内一套"听感的直接原因）。最终数值以 P1 等响度 A/B 为准。

## 6. 当前方案的风险边界

闭环跟踪器已通过便携单元测试（合成几何下的完整循环、错误起点自愈、菜单按键安全、改键恢复），但写本文档时尚未完成实机验证。必须明确标为“实现完成、单测通过、运行时待验证”。

已知风险：

1. 聚类容差（`cluster_pos_m=1.6`、`cluster_dir=0.30`）是按估计设定的；Hood/Bumper 的真实几何间距若小于容差会被并簇（两者声学接近、相邻，影响有限，矛盾防御会阻止标注污染）。实机后用日志实测值校准。
2. 菜单/回放等非驾驶相机几何会进入聚类（容量 8，最少观测回收）。若菜单相机恰好满足自环确认条件可能误标注；矛盾防御负责降级。
3. 收敛前（约两个完整视角循环）退化为计数，仍受改键/手柄后台读取影响；收敛后几何锚定自动覆盖这些误差。
4. 起始视角不固定（用户 2026-08-14 明确）：reset 默认 Dashboard 只是占位，首次几何接触/首个切换事件即开始纠正。
5. 游戏若在不重建 radio channel 的情况下换车，簇重置可能漏掉；listener 几何仍在，重新聚类会自动恢复。

推荐后续加固：

- 配置项：`camera_key_vk`、`camera_xinput_mask`。
- 控制台按钮/API：`POST /api/camera/reset`，一键重新同步 Dashboard。
- 控制台显示实时 `camera_view`。
- 如果能找到可靠的“可驾驶状态/黑屏/换车”信号，用它代替仅靠 channel handle 重置。
- 可把 FMOD listener 几何簇作为外部四视角的校验信号，而不是唯一状态源；检测计数器与几何簇冲突时提示重新同步。

## 7. 配置和运行环境

当前游戏配置：

```ini
endpoint_id={0.0.0.00000000}.{c6cb2c27-8397-44a1-b941-1f15ef792594}
gain=1
native_stereo=true
spatial_audio=true
binaural=true
cabin_profile=sportscar
speaker_layout=premium6
cabin_wet=0.15
driver_offset=0.35
head_width_m=0.18
cabin_openness=0
dashboard_port=8420
```

文件：

```text
D:\Forza Horizon 6\fh6-radio-rework\config.ini
```

日志：

```text
D:\Forza Horizon 6\fh6-radio-rework\fh6-radio-rework.log
```

控制台：

```text
http://127.0.0.1:8420/
```

API：

- `GET /api/state`
- `GET /api/probe/state`
- `GET /api/memdiff/result`
- `GET /api/viewmap/result`
- `POST /api/camera/sync`（body 为视角名，如 `dashboard`；控制台有六个同步按钮）

## 8. 安装和实机验证 `5622841`

### 8.1 安装

先确认游戏退出：

```powershell
Get-Process forzahorizon6 -ErrorAction SilentlyContinue
```

源 DLL：

```text
D:\Forza Horizon 6\fh6-radio-rework\staging\5622841\version.dll
```

安装前把当前根目录 DLL 备份到：

```text
D:\Forza Horizon 6\fh6-radio-rework\backups\version.dll.before-5622841.<timestamp>.bak
```

安装后确认根目录 DLL SHA-256 为：

```text
2D58D4F38118BFA437359A4B1E1AA6554B4F6A3B1626B375AED2CE89873E5189
```

不要覆盖 `config.ini`。

### 8.2 验证步骤

1. 启动游戏并进入可驾驶场景。
2. 选中 Streamer Mode，确认外部音乐正常。
3. 确认起始实际视角是 Dashboard。
4. 请求 `GET http://127.0.0.1:8420/api/state`，应显示：

```json
"camera_view":"dashboard"
```

5. 每按一次相机切换键后读取 API/日志，期望顺序（已按用户确认修正）：

```text
dashboard
hood
bumper
chase_near
chase_far
cockpit
dashboard
```

起始视角不固定：进游戏后先由用户报告画面真实视角，用 `POST /api/camera/sync` 同步，再开始按键验证。

6. 如果顺序与画面不一致，先记录“画面视角 -> API camera_view”，不要凭感觉改枚举；优先核实 FH6 的实际循环顺序。
7. 听感检查：
   - Cockpit 与 Dashboard 都应有座舱声学，但 Dashboard 略更前、更宽度收窄、wet 更低。
   - Chase Far 应比 Chase Near 更窄、更暗、略低。
   - Hood 应更直接、更亮。
   - Bumper 应比 Hood 稍窄、稍暗。
8. 换一次车，黑屏结束后 API 应重置到 Dashboard。

## 9. 构建、推送和下载

本机没有可用 MSVC/CMake，Windows DLL 通过 GitHub Actions 编译。

CI：`.github/workflows/build.yml`

本地便携测试命令（有 CMake 时）：

```powershell
cmake -S . -B build-portable -DFH6R_PORTABLE_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-portable --parallel
ctest --test-dir build-portable --output-on-failure
python tests/static_audit.py
```

本机 Git HTTPS 通过 `http://127.0.0.1:7897` 会 TLS 失败，但同一 mixed 端口用 SOCKS 正常：

```powershell
git -c http.proxy=socks5h://127.0.0.1:7897 `
    -c https.proxy=socks5h://127.0.0.1:7897 `
    push origin sig-scout
```

GitHub MCP 当前对本仓库可读，但 Git data 写接口返回 403；不要把 MCP 可读误报为可写。

## 10. 回滚

现有备份目录：

```text
D:\Forza Horizon 6\fh6-radio-rework\backups
```

关键备份：

- `version.dll.before-db8f26b.20260814-015516.bak`
  - 对应上一版 `68b0162`
  - SHA-256：`A63D061B27C18781376B3489162C9E036A44EF17E447831720825A13A93FCE4F`
- `version.dll.before-68b0162.20260814-0140.bak`
  - 更早基线

回滚前必须退出游戏。复制备份回 `D:\Forza Horizon 6\version.dll` 后验证哈希，再启动游戏。

## 11. 下一阶段执行计划

### P0：完成 `5622841` 实机闭环

- 安装暂存 DLL。
- 核对六次按键与 `camera_view` 顺序。
- 验证换车后 Dashboard 重置。
- 如果顺序错，只修正状态顺序；如果完全不计数，检查实际输入设备和键位。
- 增加手动 resync Dashboard API/按钮，作为长期保险。

### P1：实时 A/B 和响度

- 控制台加入：
  - DSP 总开关（原始 vs processed）
  - cabin wet
  - view width/gain/cutoff
  - HRTF 开关
  - 当前 camera_view
- 做等响度 A/B，不用“更小声”伪装距离。
- 记录峰值和短时 RMS，给每视角做补偿，保留 limiter headroom。

### P2：实测 HRTF

当前 binaural 是解析式 ITD + 头影低通，不是测得 HRIR。

候选数据：SADIE II，University of York，Apache 2.0；有 48 kHz、256 tap HRIR 和 SOFA 数据。接入前：

- 只选一个公开许可、可再分发的中性头模/测量对象。
- 在 `NOTICE.md` 和文档中写清引用与许可证。
- 做分区卷积或短 FIR，不能在音频回调分配内存。
- 保留 `binaural=false` 的扬声器立体声路径。
- 做响度匹配，避免用户把“更响”误判为更空间化。

参考：

- `https://www.york.ac.uk/sadie-project/database.html`
- `https://zenodo.org/records/10886409`
- `https://sofacoustics.org/data/database/sadie/`

### P3：车辆和敞篷拟合

目标不是每辆车一份手工预设，而是把车辆映射到少量物理/声学参数：

- cabin volume / decay
- damping
- speaker layout
- driver offset
- openness
- EQ 类别

已有 acoustic class：Generic、Luxury、Race、Saloon、SportsCar、Van。

后续需要找可靠的车辆身份/类别和 roof state 来源。换车时直接 reset；roof openness 继续平滑。

## 12. 不要重复或不要误判的事项

- 不要再次让用户做同样的两轮六视角 `.data` 采集；结果已经是 15 -> 0 稳定候选。
- 不要使用 `+0x397A8`；它是瞬态。
- 不要把 `.data + 0x397C0` 当作已证明的六态枚举。
- 不要把 18 个 Cockpit IR 全带宽直接卷到音乐上。
- 不要把 anchor 字符串命中当成 FMOD API ABI 已验证。
- 不要恢复手写 inline detour；此前出现过两次崩溃，当前参数 setter scout 是 read-only。
- 不要恢复纯输入边沿开环计数作为唯一视角来源；它是本次失同步 bug 的根因，已被 CameraTracker 闭环取代。
- 不要在游戏运行时覆盖 `version.dll`。
- 不要把 CI 成功写成实机成功；闭环跟踪器仍需运行时闭环。

## 13. 接手文件顺序

按顺序阅读：

1. 本文 `HANDOFF.md`
2. `include/fh6r/cabin_dsp.hpp`
3. `src/cabin_dsp.cpp`
4. `include/fh6r/fmod/controller.hpp`
5. `src/fmod/controller.cpp`
6. `src/fmod/dsp_bridge.cpp`
7. `src/bridge.cpp`
8. `analysis/README.md`
9. `tests/cabin_dsp_test.cpp`
10. `tests/static_audit.py`

如果只剩很少上下文，直接执行第 8 节；不要从头逆向整个 FMOD 系统。
