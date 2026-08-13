# FH6 Radio Rework — 工程交接文档 (HANDOFF)

> 本文档面向一个**未参与此前对话、但可直接读取本仓库**的高级编程 AI。
> 目标:读完本文档后,无需任何历史背景即可继续当前工作。
> 所有结论以仓库代码 + 最新运行日志为准;凡聊天记忆与代码/日志冲突处,以代码和日志为准,并在本文中显式标注冲突。

---

## 1. 项目最终目标

**最终目标**:让《Forza Horizon 6》(FH6) 的 Streamer Mode 电台成为玩家自己的"自定义车载电台",使外部播放器(QQ 音乐等)的音乐在**主观上无法与原生电台区分**。这包含三个独立维度:

1. **Audio Integration(音频)**:外部音乐经 FH6 的 FMOD 音频链播放,并具有可信的车载扬声器空间感。
2. **Now Playing Integration(元数据)**:游戏 HUD 的歌曲名/艺术家同步显示当前外部播放器的曲目。
3. **Cabin Acoustics(车载声学)**:驾驶舱视角下音乐听起来像从车厢内音响发出,车外视角听感不同,且普通 FH6 电台完全不受影响。

**当前正在解决的是第 3 部分(车载声学)的"状态逆向"子阶段**,具体是:定位 FMOD **Studio System 对象**,以便通过官方 Studio API 枚举正在播放的 Radio 事件、确认它是否为 3D event,进而拿到 interior/exterior 状态。

**为什么选择 FMOD / 内存运行时分析 / signature 方案**:
- FH6 的音频完全由 **FMOD Studio**(静态链入 `forzahorizon6.exe`,无独立 fmod dll)驱动。要"融入原生链路"而不是"另起 overlay",必须操作 FMOD 对象。
- FH6 的 **FMOD 代码段在磁盘上加密、运行时由 loader 解密**,因此静态分析 `.exe` 拿不到 FMOD 函数体/签名;只能用**运行时**的 `resolve`(anchor 字符串 + LEA 引用定位)在 live 进程里解析函数地址。
- 插件本身通过 `version.dll` proxy 注入,是唯一的注入点(游戏会加载同目录的 `version.dll`)。

**已被排除的方案及原因**:
| 方案 | 排除原因 |
|---|---|
| 静态分析 `forzahorizon6.exe` 提取 FMOD 签名 | FMOD 代码磁盘加密,`.text` 里 FMOD 名串无 LEA 引用,`resolver`/`createDSP` pattern 全文 0 命中 |
| vtable 反查定位 Studio System(旧 `locate_studio_system`) | Studio 对象是 **packed handle**,不是 vtable 多态对象;该前提错误,已作废 |
| 手写 inline detour 改函数序言 | 崩了两次(见 §9),已禁用 hook,当前全 read-only |
| 运行时读回 FMOD convolution reverb 的 IR | `getParameterData` 返回 `FMOD_ERR_UNSUPPORTED`(rc=68),IR 只写不读 |
| 把 Radio 直接塞进原生 CockpitReverb | CockpitReverb 是"车体/结构 SFX → cockpit listener"的传递函数,不是"speaker→listener";且无 `CockpitReverb_Music`/`_Radio` 类,塞进去会"响但不物理正确" |
| 用 Core `ChannelGroup::getChannel` 枚举定位 radio channel | Core 枚举不暴露 Studio 事件管理的 channel(groups=549 只枚举到 2 个 channel) |

---

## 2. 当前项目状态

| 模块 | 状态 | 依据 |
|---|---|---|
| 音频注入(WASAPI→ring→DSP) | **已验证** | 游戏运行日志 + 用户实机确认 |
| Now Playing 元数据(+0x30/+0x50 写侧 + 媒体会话源) | **已验证** | 日志 `+30="挪威的森林" +50="伍佰"` 实证 |
| Studio API 锚点候选(13 个 API) | **仅地址候选,不可直接调用** | 旧日志 12 RESOLVED / 1 AMBIGUOUS / 1 MISSING;本轮证明至少 `getBankCount/getBankList` 是误命中的诊断/格式化 helper |
| Cockpit IR 资产提取(18 个 IR) | **已验证** | 离线 FSB5 解析产出 18 个 WAV |
| Studio System handle 定位 + 身份闭环 | **已验证** | 2026-08-13 17:56:56: `getCoreSystem rc=0`;Studio core 与 Radio core 完全相等,日志 `MATCH [PROVEN]` |
| cabin DSP 核心 | **已实现,未接入音频链** | `cabin_dsp.cpp` 已写,Python 数值验证通过,但尚未接进 `read_callback` |
| 专辑封面(texture injector) | **尚未开始** | 属独立重活(D3D12 hook + build-specific 指纹),未搬 |

> 明确区分:**"已验证"** 指有日志/运行证据;**"已实现"** 指代码已写但未跑通;**"尚未验证"** 指写了没测。不要把推测写成事实。

---

## 3. 已验证事实

> 每条的验证方式用 `[DOC]`(官方文档) / `[BINDING]`(Rust/C 绑定) / `[STATIC]`(静态反汇编) / `[RUNTIME]`(游戏运行日志) / `[TEST]`(代码测试) 标注。

### 3.1 x86-64 指令解码规则(极易被"误修",务必遵守)

1. **RIP-relative 特殊编码**:`ModRM.mod == 00 && r/m == 101` 在 64-bit 模式下**永远是 `[rip+disp32]`**。`REX.B=1` **不会**把它变成 `[r13]`。[DOC] Intel SDM。
   - 常见误区:误以为 `mod=00,rm=101` 加 REX.B 应该解成 `[r13]`。**错**。`r13`/`rbp` 作为 base 时不能用 mod=00 的"无 displacement"形式,必须编码成 `mod=01 disp8=0` 或 `mod=10 disp32`。
   - 独立字节验证:`49 8B 0D 00 00 00 00` = `mov rcx,[rip+0]`,不是 `mov rcx,[r13]`。
   - 代码含义:`decode()` 里 `mod==0 && rm==5` 分支用**原始未扩展的 `rm`** 判断 RIP-relative,这是**正确的**,不要改成 `m`(扩展后)。

2. **REX 位对寄存器扩展**:`r = reg | (rex_r ? 8 : 0)`、`m = rm | (rex_b ? 8 : 0)`——这是 `r8-r15` 扩展的**统一规则**,已在 `decode()` 中实现。[DOC] Intel SDM。

3. **deref 边界**:`mov reg,[base+disp]` 是**内存加载**,`*(base+disp) ≠ base+disp`,不能像 `lea` 一样把 disp 压平累加。反例:`mov rax,[rsi+0x20]; lea rcx,[rax+0x30]` ⇒ `RCX = *(RSI+0x20)+0x30`,**不是** `RSI+0x50`。[STATIC] 代码已按此修正(`deref_boundary` 标志)。

4. **逐字节扫描要跳过 REX 前缀之后的位置**:从 call 点向前逐字节反扫时,若 `q[-1]` 是 REX 前缀(`0x40-0x4F`),则 `q` 落在指令中间,必须跳过;否则 `49 8B CE`(mov rcx,r14)会被误读成 `8B CE`(mov rcx,rsi),丢失 r8-r15 扩展。[RUNTIME] 日志 `rcx=rsi` 曾是此 bug,已修复(commit 142b149)。

### 3.2 FMOD Studio / Core ABI

5. **FMOD Studio 类型表示**:`FMOD_STUDIO_EVENTDESCRIPTION`/`EVENTINSTANCE`/`BANK`/`BUS`/`VCA` 是 **packed handle**(8 字节值)。C ABI 把 `FMOD_STUDIO_SYSTEM*` 声明成指针类型,但 FH6 实机传入的 system 值必须视为**不可解引用的 opaque handle token**;本轮值曾为 `0x1FFF1F`,仍被真实 `getCoreSystem` 接受并返回 FMOD_OK。[DOC][BINDING][RUNTIME]

6. **Studio::System::create**:`FMOD_Studio_System_Create(FMOD_STUDIO_SYSTEM **system, unsigned headerversion)`。x64 下 RCX=输出槽地址,EDX=版本号(16:8:8 编码)。[DOC][RUNTIME] 实机 ctx:`mov edx, 0x20308` = 2.03.08。

7. **Studio System 方法 ABI**(x64 调用约定,首参在 RCX):[DOC][BINDING]
   - `FMOD_Studio_System_GetCoreSystem(FMOD_STUDIO_SYSTEM *system, FMOD_SYSTEM *coresystem)` — RCX=system, RDX=&core
   - `FMOD_Studio_System_GetBankCount(FMOD_STUDIO_SYSTEM *system, int *count)` — RCX=system, RDX=&count
   - `FMOD_Studio_System_GetBankList(FMOD_STUDIO_SYSTEM *system, FMOD_STUDIO_BANK *array, int capacity, int *count)` — RCX/RDX/R8/R9
   - `FMOD_Studio_System_GetEvent(FMOD_STUDIO_SYSTEM *system, const char *path, FMOD_STUDIO_EVENTDESCRIPTION *event)` — RCX/RDX/R8

8. **FMOD Core DSP type(FMOD 2.03,非 FMOD Ex)**:`ConvolutionReverb=28, ChannelMix=29, Transceiver=30, ObjectPan=31, MultibandEq=32`。[DOC] 见 `fmod_dsp_type.hpp`。
   - **关键坑**:早期代码曾把 32 当 ConvolutionReverb(那是 FMOD Ex 旧枚举)。实际 32=MultibandEq。此错误导致对 type=32 的 IR/WET/DRY 读取全部作废。

9. **DSP 参数类型**:`Float=0, Int=1, Bool=2, Data=3`。[DOC][RUNTIME] 已由 `getParameterInfo` 运行时坐实。

10. **ConvolutionReverb 参数布局**:`IR=0(Data)`, `Wet=1(Float, dB [-80,+10])`, `Dry=2(Float, dB)`, `Linked=3(Bool, 默认 true)`。[DOC][RUNTIME] getParameterInfo 运行时确认 type=3/0/0/2。

11. **`getParameterFloat` 有 5 个参数**:`(dsp, index, float* value, char* valuestr, int valuestrlen)`。早期漏了第 5 参数导致读到垃圾值(40/660/21999)。[DOC][RUNTIME] 已修正。

### 3.3 已找到的 FMOD 运行时锚点(当前 build)

12. **唯一真 Convolution Reverb DSP**:depth=5,链为 `Fader(7)→Distortion(9)→MultibandEQ(32)→ConvolutionReverb(28)`,ch=8。[RUNTIME] graph dump。全图仅 1 个 type=28;type=32 有 161 个(Multiband EQ)。

13. **该卷积 reverb 的 wet/dry**:`wet=-21.0 dB, dry=-80.0 dB`(rc=0)。[RUNTIME] dry≈-80 表明它是 wet-only return(符合 send→return→wet 结构,但 SEND connection getter 被裁,未升级为 routing 已证明)。

14. **IR readback 不可用**:`getParameterData(IR)` 返回 `FMOD_ERR_UNSUPPORTED`(rc=68)。[RUNTIME] 措辞精确为:"该实例的 IR readback 返回 UNSUPPORTED,故不能通过公共 getter 读当前 IR",而非"IR 天生只写不读"(官方只说明 IR 可 setParameterData 设置)。

15. **LINKED 读不到**:`getParameterInt(Linked)` 返回 rc=68(用 int getter 读 bool 参数被拒);`getParameterBool` 名串被死代码消除裁掉。**LINKED 维持 UNVERIFIED**。[RUNTIME]

16. **FMOD 函数解析机制**:`resolve_studio_anchor` = anchor 字符串 + LEA 引用 + 过滤共享 dispatch 函数(prologue `48 89 5C 24 18 55 56 57`)。[RUNTIME] 它只能产出**地址候选**,不能证明 callable ABI。旧 scout 的 12 RESOLVED / 1 AMBIGUOUS / 1 MISSING 统计现降级为观察结果;本轮 `getBankCount/getBankList` 候选均无真实 call xref,反汇编形状与官方 ABI 不符。

### 3.4 Cockpit 声学资产(盘上明文)

17. **车内外状态源 = 全局参数 `Cockpit`**:在 `MasterBank.strings.bank` 中,与 `ImpactSpeed`/`Wetness`/`Doppler` 等并列。[STATIC]

18. **FH6 车内声学 = 卷积 IR + 混响总线**:`bus:/MasterVolume/1_SFX/CockpitInterior/CockpitReverb`,IR 有 `CockpitIR_Luxury/Race/Saloon/SportsCar/Van`,还有 `CockpitGainReduction`。[STATIC]

19. **CockpitReverb 分类总线无 Music/Radio**:`CockpitReverb_{Car,Collisions,Foley,Chassis,Engine,Exhaust,GearShifts,Horns,Suspension,Transmission,VO,WorldCollisions,WeatherOnCar...}`,**没有 `_Music`/`_Radio`**。[STATIC] 这证实 FH6 不给电台做车内混响,需自建 cabin DSP。

20. **18 个 CockpitIR 已提取**:从 `Reverb_CockpitIRs.assets.bank`(5.1MB,FEV/RIFF 容器,SND chunk 内嵌 FSB5,FSB5 魔数在 offset 768)提取。48kHz int16,时长 254ms~1s,**11 个 4ch quad + 7 个 8ch**。[STATIC] 产出在 `cockpit_ir_extracted/`(18 个 wav)。多声道 + 声道能量不均 ⇒ 是含空间信息的 true-stereo IR,不是 mono kernel。

21. **IRMappings.xml:17 类里仅 5 类启用**:`Buggy/Classic/EV/HotHatch/HotRod/Luxury/Muscle/Offroad/Race/Rally/Saloon/SportsCar/Supercar/SUV/TrackToy/Truck/Van`,仅 `Luxury/Race/Saloon/SportsCar/Van` 未被 XML 注释掉。[STATIC]

### 3.5 RadioStreamFmod 对象与元数据

22. **RadioStreamFmod RTTI 发现**:通过 RTTI 串 `"RadioStreamFmod"` + `"_Ref_count_obj2"` 定位 type descriptor → Complete Object Locator → vtable → 堆扫描实例。[RUNTIME] 缓存于 `g_cache`。

23. **关键偏移**:[RUNTIME]
   - `rc + 0x10` = stream(内联对象,首 8 字节是 vtable = FH6 自己的 wrapper,**不是** EventInstance)
   - `rc + 0x18` = fmod_sound
   - `stream + 0x08 → x → x + 0xC0` = FMOD System(`resolve_fmod_system`)
   - 元数据链 Layout A(legacy):`+0x48 → first, +0x18 → body`;Layout B(modern):`+0x50 → first, +0x08 → body`
   - `body + 0x10` = SoundName(HZ6_ 前缀内部名), `body + 0x30` = 标题, `body + 0x50` = 艺术家

24. **Studio System 槽位 = `RSI + 0x4C918`**:[RUNTIME] create 调用点 ctx `lea r14,[rsi+0x4C918]`。**RSI 是函数参数/this**(不是全局),因此 create 的 trace 无法直接定位,需 getCoreSystem 的两层 deref(见 §10)。

25. **getCoreSystem 调用点存在自包含两层 deref**:[RUNTIME] 旧版日志 RVA `0x31D2AD4` 的 ctx:
   ```
   48 8B 0D [disp32]   mov rcx,[rip+G]      ; rcx = *(G) = AudioManager
   48 8B 89 [disp32]   mov rcx,[rcx+0x4C918] ; rcx = *(AudioManager+0x4C918) = Studio System
   ```
   即 `Studio System = *( *(G) + 0x4C918 )`。这是**更直接**的定位入口(不需追 rsi 来源)。

26. **Studio System 已由真实 Core 身份闭环证明**:[RUNTIME] 2026-08-13 17:52:46 `getCoreSystem rc=0 core=0x2391DC78028`;进入 Streamer Mode 后,17:56:56 `resolve_fmod_system()` 得到同一地址,日志为 `identity closure: studio_core=... radio_core=... -> MATCH [PROVEN]`。

27. **旧 `getBankCount/getBankList` 候选不可调用**:[STATIC][RUNTIME] `getBankCount` 候选按 `rcx+0x10` 等字段工作、没有真实 E8 call xref,不符合二参 C ABI;旧版强行调用返回 `4294967295`,且被 SEH 捕获。本轮已移除调用,只记录 `NOT CALLED (ABI unverified)`。

---

## 4. 尚未确认的假设

| # | 假设内容 | 当前证据 | 可信度 | 最简单的验证方法 |
|---|---|---|---|---|
| H2 | 18 个 CockpitIR 由唯一 convolution DSP 按车型动态切换 | 18 IR + 唯一 DSP + IRMappings 5 类 | 中 | 换车前 hook `setParameterData` 记录 IR 指纹,或黑盒 impulse 测量(IR readback 已堵死) |
| H3 | LINKED=false(4ch 是 true-stereo matrix) | 4ch 能量不均、含左右差异 | 中 | 需读 LINKED,但 getter 被裁;只能 hook `setParameterBool(Linked)` 观察 |
| H4 | 正在播放的 Radio event 是 FMOD 3D event | 盘上有 `GLB_Radio_3D`/`EMT_3D_Music` 资产名 | 中 | 拿到 Studio System → 枚举 event → `is3D()`/`isStream()` |
| H6 | 该 convolution DSP 是 wet-only return | dry=-80dB | 高 | SEND connection getter 被裁,暂无直接法;impulse 黑盒测量输出 |

> 原 H1/H5 已由 `getCoreSystem` 成功调用及 Radio core 地址相等升级为事实;不再用未验证的 `getBankCount` 作为判据。

---

## 5. 当前代码结构

```
include/fh6r/                      # 头文件(全在 include/ 下,命名空间 fh6r)
  audio_ring.hpp                   # SPSC 无锁环形缓冲(热路径核心)
  wasapi_capture.hpp               # WASAPI loopback 捕获
  drift_control.hpp                # 时钟漂移修正(纯函数,可移植)
  dsp_contract.hpp                 # FMOD 声道契约(纯函数,可移植)
  cabin_dsp.hpp                    # cabin DSP 核心(尚未接入)
  config.hpp / log.hpp / safe_mem.hpp
  fmod/
    pe_image.hpp                   # PE 解析(text/rdata 段、pdata 函数边界)
    sig_scanner.hpp                # 签名扫描(find_by_anchor/pattern/resolve_studio_anchor)
    sig_scout.hpp                  # 运行时 FMOD API 侦察
    radio_discovery.hpp            # RadioStreamFmod RTTI + 堆发现 + resolve_fmod_system
    dsp_bridge.hpp                 # FMODFns 结构 + DSPBridge(注入 + read_callback)
    controller.hpp                 # Streamer Mode 闸门 + 目标发现 + 元数据接线
    fmod_dsp_type.hpp              # FMOD 2.03 DSP type/参数枚举(注明来源)
    cockpit_reverb_scout.hpp       # 遍历 group 树找卷积 DSP + dump IR
    studio_api_scout.hpp           # Step1: Studio API resolve-only scout
    studio_system_scout.hpp        # Step2: locate + getCoreSystem 验证 + 缓存 identity snapshot
    studio_param_hook.hpp          # 参数 setter hook(已禁用,read-only)
    studio_probe.hpp               # 旧 locate_studio_system(已作废)
    event_instance_probe.hpp       # radio_stream 观察工具(仅观察,不断言)
    metadata_injector.hpp          # Now Playing 写侧(+0x30/+0x50)
  http/http_server.hpp             # localhost 控制台
  media_session.hpp                # Windows 媒体会话(取 QQ 歌名)
  memory_diff.hpp                  # 相机状态内存差分探针
src/                               # 实现(与 include 一一对应)
  bridge.cpp                       # 总装:构造所有对象 + 启动时 scout
  proxy/dll_main.cpp               # version.dll 入口 + proxy exports
  proxy/version.def                # proxy 导出表
tests/                             # 便携测试(非 Windows 可跑)
tools/                             # 诊断脚本(不参与编译)
```

**核心数据流(音频热路径)**:
```
WASAPI 捕获线程(生产者) → SPSC ring → FMOD read_callback(消费者) → FH6 radio bus
```
**控制路径(非热路径)**:`bridge.cpp run_bridge` → 启动时依次调 `scout_apis` / `scout_studio_api` / `scout_studio_system` / `install_studio_param_hooks` → 构造 `DSPBridge`/`Controller`/`AudioStateProbe`/`MemoryDiff`/`HttpServer`。

**模块关系**:
- `sig_scanner` 提供 resolve 原语(`find_by_anchor`/`resolve_by_anchor_unique`/`resolve_studio_anchor`),供 `dsp_bridge`、`studio_system_scout`、`cockpit_reverb_scout` 使用。
- `dsp_bridge` 持有 `FMODFns`(已解析的函数指针表)+ `DSPBridge`(注入 + 回调)。
- `controller` 用 `radio_discovery` + `dsp_bridge` 做 Streamer Mode 闸门 + 目标发现 + 元数据接线。
- `studio_system_scout` 的 `locate_studio_system_handle` 是当前主攻点。

---

## 6. 关键代码位置(接手 AI 最应先看)

- **`src/fmod/studio_system_scout.cpp`** — **最重要,当前阻塞点**
  - `decode()` — x86-64 mov/lea 解码器(REX/RIP-relative 规则在此,**勿误修**)。稳定。
  - `trace_rcx_addr()` — 从 call 点追 RCX 来源(lea 累积 disp / reg move / 全局来源),已含 REX 对齐修复 + deref 边界。较新。
  - `read_studio_via_getcore()` — 匹配 `mov rcx,[rip+G]` + `mov rcx,[rcx+slot]` 两层 deref,读 opaque handle。**已验证命中**。
  - `locate_studio_system_handle()` — 首选 getCoreSystem 模式匹配,fallback create trace。**已验证**。
  - `scout_studio_system()` — 入口,解析一次函数后重试;用真实 `getCoreSystem` 验证并缓存 Studio/core snapshot。bank 候选只记录、不调用。
  - 临时/实验:整个 `studio_system_scout.cpp` 都是诊断 scout,非音频路径代码,可自由改。

- **`src/fmod/dsp_bridge.cpp`**
  - `resolve_fmod_signatures()` — 解析全部 FMOD 函数到 `FMODFns`。音频注入的核心依赖。**稳定,勿动**。
  - `kSigs`/`kResolver`/`kUnlock` — 已确认的 FMOD Core 签名 pattern(build-specific)。

- **`src/fmod/controller.cpp`**
  - `resolve_radio_state_slot()` / `read_station_name()` — RadioState 全局 + Streamer Mode 闸门。稳定。
  - `discover_target()` — active RadioStreamFmod 发现 + 唯一性校验 + event_probe 一次性触发。
  - 首次 attach 后对比 `studio_system_snapshot().core_system` 与 `resolve_fmod_system()`,相等时写 `MATCH [PROVEN]`。**已实机验证**。

- **`src/fmod/radio_discovery.cpp`**
  - `resolve_fmod_system()` — `stream+0x08→x→x+0xC0` 链。getCoreSystem 闭环的 core_B 基准来源。
  - `discover_radio_instances()` — RTTI + 堆扫描。

- **`src/bridge.cpp`**
  - `run_bridge()` — 总装 + 启动时 scout 调用顺序。若要加新的启动 scout,在这里接线。

- **`include/fh6r/fmod/fmod_dsp_type.hpp`** — FMOD 2.03 常量(注明来源),**勿凭记忆改**。

- **已作废/观察用**:`src/fmod/studio_probe.cpp`(vtable 反查,前提错误)、`src/fmod/event_instance_probe.cpp`(仅观察,不断言 EventInstance)、`src/fmod/studio_param_hook.cpp`(hook 已禁用)。

---

## 7. 最近修改历史

| commit | 内容 | 为什么 | 已验证? | 后续 |
|---|---|---|---|---|
| `fa665ca` | 用 getCoreSystem 验证并缓存 opaque Studio handle;禁止调用未证 ABI 的 bank 候选;attach 后做 Core identity closure | 修复 bank helper 误调用,完成 Studio/Radio 身份闭环 | **CI + 实机已验证** | P1 需重新建立 bank/event API 的可调用解析依据 |
| `142b149` | 修 trace REX 对齐(跳过 REX 前缀后位置)+ getCoreSystem xref 诊断日志 | `rcx=rsi` 是逐字节扫描丢 REX 前缀的 bug | **CI + 实机已验证** | 完成 |
| `788e358` | locate 首选 getCoreSystem 两层 deref 模式匹配 | 比追 create 的 rsi 来源更直接 | **CI + 实机已验证** | 完成 |
| `3c74520` | 修编译错误(read_disp32/to_integer 误用、const pos、返回类型) | 重写引入的类型错误 | 已验证(编译通过) | 完成 |
| `0f074fc` | 修 trace:deref 边界(mov [base+disp] 停止压平) | 避免算出假 slot | 已验证 | 完成 |
| `eba862a` | locate + 验证 Studio System handle(结构化 trace) | 主线推进 | 已验证 | 完成 |
| `3a2f73c` | trace_rcx 支持 REX.B + 多层追踪 | 追 rsi 来源 | 已验证 | 完成 |
| `9420068` | FMOD 2.03 纪律:统一 DSP type 枚举 + 三重验证 | 纠正 type=32 误判 | 已验证 | 完成 |
| `d67165b` | 修 DSP type:ConvolutionReverb=28(非 32) | 根本性纠错 | 已验证 | 完成 |
| `612ea00` | 修 getParameterFloat ABI(补 valuestrlen)+ LINKED 用 getParameterInt | 读 wet/dry 垃圾值 | 已验证 | 完成 |
| `aa5e72f` | 彻底禁用 hook(两次崩溃后) | 手写 detour 崩 | 已验证 | 保持 read-only |

> 更早的 commit 见 `git log --oneline`。关键主线:音频注入(原有)→ 元数据(g0ldyy 移植)→ 声学逆向(当前)。

---

## 8. 当前运行方式

**Build**:
```
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```
- toolchain: **Windows x64 + MSVC**(VS2022/VS2026,当前 CI 用 MSVC 14.51)。CMake 3.24+。
- 产物:`build/Release/version.dll`。
- 便携测试(非 Windows,无 FMOD 依赖):`cmake -S . -B build-portable -DFH6R_PORTABLE_TESTS_ONLY=ON` + `ctest`。
- 当前环境**无本地 MSVC/CMake**,Windows DLL 由 GitHub Actions `windows-x64` job 构建(artifact 名 `fh6-radio-rework-windows-x64`)。Git Credential Manager 可 push;`gh` CLI 仍不存在。最新成功 run=`31688140597`,artifact id=`9176304409`。

**注入/加载**:
1. 把 `version.dll` 放到 `forzahorizon6.exe` 同目录(本例 `D:\Forza Horizon 6\`)。
2. 用同目录 `forzahorizon6_loader.exe` 启动 FH6,`version.dll` 通过 proxy exports 被游戏自动加载。不要直接双击 `forzahorizon6.exe`;本机实测会弹 ForzaSupport 错误并退出。
3. 进自由驾驶 → 切 **Streamer Mode**(关 Radio DJ)→ 插件自动锁定 active radio stream。
4. Windows 音量混合器里把 QQ 音乐输出设为独立播放设备(如 VB-Audio Cable)。

**日志**:`D:\Forza Horizon 6\fh6-radio-rework\fh6-radio-rework.log`。控制台:`http://127.0.0.1:8420`。

**判断 hook/侦察成功**:日志里 `[fmod] create=0x... release=... add=... remove=... resolver=... unlock=...` 均非 0,即音频注入可用;`[controller] active Streamer Mode stream='HZ6_...'` 即已接入。

**正常输出示例**:
```
[studio-system] ===== locate + verify Studio System handle =====
[studio-system] create=0x... getCoreSystem=0x... getBankCount=0x... getBankList=0x...
[studio-system] get_core_fn=0x...
[studio-system] getCoreSystem E8 xref matches=N
[studio-system] STUDIO SYSTEM HANDLE = 0x...
[studio-system] getCoreSystem rc=0 core=0x... -> Studio System VALID (opaque handle)
[studio-system] getBankCount/getBankList anchor candidate=0x... -> NOT CALLED (ABI unverified)
[studio-system] identity closure: studio_core=0x... radio_core=0x... -> MATCH [PROVEN]
```

**权限/配置**:无管理员权限要求;首次启动生成 `fh6-radio-rework/config.ini`。

---

## 9. 已知故障和坑

| 坑 | 正确做法 |
|---|---|
| **FMOD 代码磁盘加密,静态分析 exe 走不通** | 一切 resolve/xref 都在 live 进程做;磁盘 PE 只信 section/RVA/非加密区结构 |
| **DSP type 32 vs 28 混淆** | FMOD 2.03:28=ConvolutionReverb,32=MultibandEq。识别走三重验证(type+name+参数布局),禁止只看数字 |
| **getParameterFloat 漏第 5 参数 valuestrlen** | 签名 `(dsp,index,float*,char*,int)` |
| **getParameterBool 被裁 / int 读 bool 返回 UNSUPPORTED** | LINKED 维持 UNVERIFIED;不要假设能读 |
| **IR readback 返回 UNSUPPORTED(rc=68)** | 运行时读 IR 路线堵死;改黑盒测量或 FEV GUID 映射或 hook setParameterData |
| **ASLR 地址变化** | 永远用 RVA(`p - img.base`),不用绝对地址 |
| **手写 detour 崩(两次)** | prologue 搬迁蹦床有栈错位;vtable patch 扫 .text 误伤代码。当前全 read-only,hook 禁用 |
| **逐字节反扫丢 REX 前缀** | 跳过 `q[-1]` 是 0x40-0x4F 的位置 |
| **mov [base+disp] 当 lea 压平 disp** | 它是 deref 边界,停止代数化 |
| **Studio vs Core API 混淆** | Studio 对象是 packed handle(除 SYSTEM 是真指针);Core 用真实指针/handle |
| **bank 资产 vs 运行时对象** | 盘上 `.bank`/`.xml` 是资产(明文),运行时对象是解密后的 FMOD 结构,两者不直接对应 |
| **`ChannelGroup::getChannel` 枚举不到 radio channel** | Core 枚举不暴露 Studio 事件的 channel;改走 Studio `EventInstance::getChannelGroup` |
| **getParentGroup 被裁(`ChannelControl::` 前缀)** | `ChannelGroup::getParentGroup` 存在(不同前缀);DFS 维护 group_stack 也可绕过 |
| **RSI(AudioManager)是函数参数不是全局** | create 的 trace 追不到;改用 getCoreSystem 的 `mov rcx,[rip+G]` |
| **anchor 名串命中不等于 API 可调用** | 必须再有真实 call xref、调用点寄存器形状/参数数目与官方 ABI 一致,最好先用无副作用方法做身份闭环;旧 bank 候选已证伪 |
| **把 opaque Studio handle 当 C++ 对象解引用** | 禁止直接解引用;仅传给已验证的 FMOD C ABI 入口。本 build 的有效值可能小到类似 `0x1FFF1F` |

---

## 10. 当前真正的阻塞点

**Studio System 定位已经完成。当前阻塞转为:为 bank/event 枚举 API 建立 ABI 可调用性证据。**

旧 `resolve_studio_anchor` 把包含 `FMOD Studio ...` 诊断名串的 owner 当 API 地址。对 `getCoreSystem` 恰好有效并有 7 个真实 E8 xref;对 `getBankCount/getBankList` 则命中无 call xref、参数形状不符的 helper。故不能沿用“名串唯一 = RESOLVED”的标准继续调用 13 个候选。

下一步必须从已验证的真实 FMOD wrapper/call sites 出发,对每个所需入口同时满足:
- 有真实调用点或稳定 thunk provenance;
- 调用前 RCX/RDX/R8/R9 形状符合官方 C ABI;
- 首次调用采用 read-only、边界受控的参数并记录 FMOD_RESULT;
- 未满足以上条件时只报告 candidate,禁止调用。

---

## 11. 下一阶段执行计划

### P0(已完成)
- `getCoreSystem` 路径命中,读到 opaque Studio handle并返回 FMOD_OK。
- Streamer Mode attach 后 Studio core == Radio core,日志 `MATCH [PROVEN]`。
- commit `fa665ca`;GitHub Actions run `31688140597` 两个 job 通过;artifact 已实机部署。

### P1(当前主线)
- **任务 A:已完成** —— `getCoreSystem(studio)` 与 `resolve_fmod_system()` 相等,已记录 [PROVEN]。
- **任务 B:当前阻塞** —— 先重建可调用 API provenance,再做 `getBankCount`/`getBankList` → `Bank::getEventCount/getEventList` → `EventDescription::getInstanceCount/getInstanceList` + `is3D`/`isStream`/`getPath`,列出 active 事件,找 Radio event。
- **任务 C**:确认 Radio event 是否 3D → 决定"复用原生 3D Music 链"还是"自建 cabin DSP"。
- **文件**:新模块(建议 `src/fmod/studio_enum_scout.cpp`),在 attach 时触发(仿 `cockpit_reverb_scout` 的接线方式)。

### P2(后续增强)
- cabin DSP 接入 `DSPBridge::read_callback`(ring pop 后、write_frame 前),mode 由 GameAudioState 驱动。
- 用 `setParameterData`/黑盒测量确认 CockpitIR 按车型切换(H2)。
- 专辑封面 texture injector(独立重活)。

---

## 12. Reverse Engineering Evidence Log

| ID | 发现 | 类型 | 证据来源 | 状态 | 备注 |
|---|---|---|---|---|---|
| E01 | FMOD 代码磁盘加密、运行时解密 | STATIC | 磁盘 PE 分析(resolver/createDSP 0 命中)+ 运行时解析成功 | 已证明 | 静态分析 exe 路线作废 |
| E02 | version.dll 是公开仓库版 | STATIC | 抽签名串与源码逐字一致 | 已证明 | |
| E03 | ConvolutionReverb=28, MultibandEq=32 | DOC+RUNTIME | FMOD 2.03 文档 + getInfo name | 已证明 | 早期误判 32 已纠正 |
| E04 | 卷积 reverb 参数 IR/Wet/Dry/Linked, type 3/0/0/2 | RUNTIME | getParameterInfo dump | 已证明 | |
| E05 | wet=-21dB, dry=-80dB | RUNTIME | getParameterFloat | 已证明 | wet-only return 是强推断 |
| E06 | IR readback UNSUPPORTED(rc=68) | RUNTIME | getParameterData | 已证明 | 措辞见 §3.3-14 |
| E07 | LINKED 读不到(getParameterBool 被裁,int 被拒) | RUNTIME | getParameterInt rc=68 | 已证明 | LINKED 维持 UNVERIFIED |
| E08 | 全局参数 `Cockpit` | STATIC | MasterBank.strings.bank | 已证明 | 状态源 |
| E09 | 18 个 CockpitIR(11×4ch + 7×8ch) | STATIC | FSB5 解析 | 已证明 | 见 cockpit_ir_extracted/ |
| E10 | IRMappings 17 类仅 5 启用 | STATIC | IRMappings.xml | 已证明 | |
| E11 | CockpitReverb 无 Music/Radio 类 | STATIC | strings.bank | 已证明 | 需自建 cabin DSP |
| E12 | Studio System 槽位 = RSI+0x4C918 | RUNTIME | create xref ctx | 已证明 | RSI 是函数参数 |
| E13 | headerversion 0x20308 = 2.03.08 | RUNTIME | mov edx,0x20308 | 已证明 | |
| E14 | getCoreSystem 两层 deref 链 | RUNTIME | 旧版日志 xref RVA 0x31D2AD4 | 已证明 | 主线定位入口 |
| E15 | Studio 类型是 packed/opaque handle | DOC+RUNTIME | FMOD 2.03 文档 + 实机 handle `0x1FFF1F` | 已证明 | SYSTEM 的 C 类型虽为指针,实机仍不可直接解引用 |
| E16 | `getCoreSystem` C ABI 在本 build 可调用 | DOC+BINDING+RUNTIME | 官方签名 + rc=0 + core identity closure | 已证明 | 这是当前唯一完成 ABI+身份双验证的 Studio API |
| E17 | 旧 scout 产出 12/13 地址候选 | RUNTIME+STATIC | scout 日志 + 候选反汇编 | **降级为候选** | 至少 bank count/list 是误命中 helper,不可按 RESOLVED 使用 |
| E18 | 元数据 +0x30 标题 / +0x50 艺术家 | RUNTIME | radio-probe 日志 | 已证明 | |
| E19 | mod=00 && rm=101 恒为 RIP-relative | DOC | Intel SDM + 字节验证 | 已证明 | 防误修 |
| E20 | 手写 detour 崩溃(两方案) | RUNTIME | 两次游戏崩溃 | 已证明 | hook 已禁用 |
| E21 | Studio System handle 定位成功 | RUNTIME | 两层 deref + `getCoreSystem rc=0` | 已证明 | handle 仅作 opaque token 传参 |
| E22 | Studio core == Radio core | RUNTIME | 2026-08-13 17:56:56 `MATCH [PROVEN]` | 已证明 | 完成身份闭环 |
| E23 | bank count/list 旧候选 ABI 错误 | STATIC+RUNTIME | 0 call xref + 指令形状不符 + 旧调用失败 | 已证明 | 当前代码不再调用 |

---

## 13. 不要重复做的工作

**已证伪、不用重新调查**:
- 静态分析 `forzahorizon6.exe` 拿 FMOD 签名(加密,走不通)。
- vtable 反查定位 Studio System(Studio 是 packed handle,前提错误)。
- 运行时读回 convolution reverb 的 IR(UNSUPPORTED)。
- 用 `ChannelGroup::getChannel` 枚举定位 radio channel(不覆盖 Studio channel)。
- 把 Radio 塞进原生 CockpitReverb(物理上接错对象)。

**不要随意重构**:
- `dsp_bridge.cpp` 的 `resolve_fmod_signatures` / `kSigs` / `kResolver` / `kUnlock`(音频注入核心,build-specific,已验证)。
- `dsp_contract.hpp` / `audio_ring.hpp`(热路径契约,有测试守护)。
- `fmod_dsp_type.hpp`(FMOD 2.03 常量,注明来源)。

**看起来奇怪但故意的**:
- `decode()` 里 `mod==0 && rm==5` 用原始 `rm`(不是扩展后的 `m`)——这是 RIP-relative 正确规则,**勿改**。
- `read_callback` 里 mono 复制到全部声道——规避 3D panner 相位问题。
- `studio_param_hook.cpp` 里 hook 全 disabled——两次崩溃后的安全决定。
- 启动时 `scout_apis` 很慢(扫 40+ anchor)——这是诊断构建的预期,不是 bug。

---

## 14. NEXT AGENT START HERE

1. **先读**(按顺序):
   - `HANDOFF.md`(本文)
   - `src/fmod/studio_system_scout.cpp`(当前阻塞点,完整读)
   - `include/fh6r/fmod/fmod_dsp_type.hpp`(FMOD 常量)
   - `src/fmod/dsp_bridge.cpp` + `src/bridge.cpp`(FMOD 解析 + 接线)
   - `src/fmod/controller.cpp` + `src/fmod/radio_discovery.cpp`(状态闸门 + 发现 + resolve_fmod_system)

2. **当前基线**:`sig-scout` HEAD=`fa665ca`;CI run=`31688140597`;部署 DLL SHA256=`ABC9770B50CB2BCAFA1598FEBCBBD66E81B1CDB56B614C310CDB14D19A071DBD`。通过 `forzahorizon6_loader.exe` 启动。

3. **看日志**:`D:\Forza Horizon 6\fh6-radio-rework\fh6-radio-rework.log`,当前基线应出现:
   - `getCoreSystem rc=0 ... -> Studio System VALID (opaque handle)`
   - `getBankCount/getBankList ... NOT CALLED (ABI unverified)`
   - Streamer Mode attach 后 `identity closure ... MATCH [PROVEN]`

4. **验证什么**:任何新增 Studio API 必须同时验证 call provenance、参数寄存器形状和 FMOD_RESULT;不要再把 anchor owner 直接当 API。

5. **然后修改**:围绕 §11 P1 重建 bank/event API 的 callable 解析,再枚举 active event 并判断 Radio 是否 3D。`locate_studio_system_handle` 已完成,除非新 build 漂移,不要重写。

> 完成后把新的逆向发现追加到 §12 证据表,更新 §2 状态。

---

## 15. 2026-08-13 Radio 声学身份闭环（最新状态）

旧的 P1 “必须先枚举全部 bank/event 才能判断 Radio 是否 3D”已被一条更短、身份更强的 active-stream 路径替代并完成：从唯一 active `RadioStreamFmod` 直接读取 packed handles，再用有真实调用 provenance 的 Studio/Core getter 验证。

### 已实现

- 新增 `radio_acoustics_probe`，在每个游戏进程首次发现 active Streamer Mode stream 时自动运行一次。
- `stream + 0x18` 的 Studio System handle 与 `studio_system_snapshot()` 相等：`MATCH [PROVEN]`。
- `stream + 0x20` 是可调用 `Channel::getCurrentSound` 的 Core Channel handle。
- `Channel::getCurrentSound` 返回当前 Core Sound；`Sound::getMode` 实测 `0x0003008A`，即 `is2D=true`、`is3D=false`、`createStream=true`。
- `stream + 0x10` 通过 `EventInstance::getPlaybackState` 和 getDescription thunk 双重验证为直接 EventInstance packed handle；其 EventDescription 返回 `is3D=false`、`isStream=false`。
- 所有新 getter 调用均有 SEH 保护；候选身份不唯一时拒绝下结论。

### 实机证据（2026-08-13 18:48:08）

```text
[radio-acoustics] wrapper Studio handle 0x1FFF1F vs verified 0x1FFF1F -> MATCH [PROVEN]
[radio-acoustics] Channel::getCurrentSound rc=0 sound=0x1724FE48E28 seh_ok=true ...
[radio-acoustics] Sound::getMode rc=0 mode=0x0003008A is2D=true is3D=false createStream=true
[radio-acoustics] EventInstance=0x37B000 description=0x37A800 desc_ok=true is3D_rc=0 is3D=0 ... isStream_rc=0 isStream=0 ...
[radio-acoustics] direct Radio Event identity -> CONFIRMED is3D=false isStream=false [PROVEN]
```

### 结论与下一步

**架构决定已完成**：插件接管的 active Core Sound 是 2D stream，直接 Radio Studio Event 也不是 3D Event。因此下一条实现主线是把现有 `cabin_dsp.cpp` 接入 `DSPBridge::read_callback`，用插件自己的车内/车外状态和滤波/卷积实现座舱声学；不再把全 bank 枚举作为该决定的前置条件。

仍须保持证据边界：这只证明当前 active Radio stream 及其直接 Event，不证明整个 FH6 bank 中不存在其他 3D Music/Radio 资源。

用户操作说明见 `RADIO_ACOUSTICS_DIAGNOSTIC.md`。
