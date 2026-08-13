#include "fh6r/http/http_server.hpp"
#include "fh6r/audio_ring.hpp"
#include "fh6r/audio_state_probe.hpp"
#include "fh6r/memory_diff.hpp"
#include "fh6r/config.hpp"
#include "fh6r/fmod/controller.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/log.hpp"
#include "fh6r/wasapi_capture.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fh6r::http {
namespace {
constexpr std::size_t kMaxHeaders = 16 * 1024;
constexpr std::size_t kMaxBody = 64 * 1024;

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[7];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string lower_ascii(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool send_all(SOCKET s, const char* data, std::size_t size) {
    while (size) {
        const int n = send(s, data, static_cast<int>(std::min<std::size_t>(size, 1u << 20)), 0);
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

const char* status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

void respond(SOCKET s, int code, std::string_view body,
             std::string_view type = "application/json; charset=utf-8") {
    std::ostringstream h;
    h << "HTTP/1.1 " << code << ' ' << status_text(code) << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << "X-Content-Type-Options: nosniff\r\n"
      << "Referrer-Policy: no-referrer\r\n"
      << "Connection: close\r\n\r\n";
    const auto header = h.str();
    send_all(s, header.data(), header.size());
    send_all(s, body.data(), body.size());
}

struct Request {
    std::string method;
    std::string path;
    std::string body;
    std::string host;
    std::string origin;
};

bool parse_headers(std::string_view headers, Request& req, std::size_t& content_length) {
    content_length = 0;
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const auto end = headers.find("\r\n", pos);
        const auto line = headers.substr(pos, end == std::string_view::npos ? headers.size() - pos : end - pos);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto key_view = line.substr(0, colon);
            std::string key(key_view);
            std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            if (key == "content-length") {
                std::size_t n = 0;
                const auto r = std::from_chars(value.data(), value.data() + value.size(), n);
                if (r.ec != std::errc{} || r.ptr != value.data() + value.size()) return false;
                content_length = n;
            } else if (key == "host") {
                req.host.assign(value);
            } else if (key == "origin") {
                req.origin.assign(value);
            }
        }
        if (end == std::string_view::npos) break;
        pos = end + 2;
    }
    return true;
}
bool read_request(SOCKET s, Request& req, int& error_code) {
    std::string raw;
    raw.reserve(4096);
    std::array<char, 4096> buf{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const int n = recv(s, buf.data(), static_cast<int>(buf.size()), 0);
        if (n <= 0) return false;
        raw.append(buf.data(), static_cast<std::size_t>(n));
        if (raw.size() > kMaxHeaders) { error_code = 413; return false; }
        header_end = raw.find("\r\n\r\n");
    }

    const auto first_end = raw.find("\r\n");
    if (first_end == std::string::npos) { error_code = 400; return false; }
    const std::string_view first{raw.data(), first_end};
    const auto sp1 = first.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? sp1 : first.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) { error_code = 400; return false; }
    req.method.assign(first.substr(0, sp1));
    req.path.assign(first.substr(sp1 + 1, sp2 - sp1 - 1));
    if (req.path.find('?') != std::string::npos) req.path.resize(req.path.find('?'));

    std::size_t length = 0;
    if (!parse_headers(std::string_view{raw}.substr(first_end + 2, header_end - first_end - 2), req, length)) {
        error_code = 400; return false;
    }
    if (length > kMaxBody) { error_code = 413; return false; }
    req.body.assign(raw.data() + header_end + 4, raw.size() - header_end - 4);
    while (req.body.size() < length) {
        const auto need = std::min<std::size_t>(buf.size(), length - req.body.size());
        const int n = recv(s, buf.data(), static_cast<int>(need), 0);
        if (n <= 0) { error_code = 400; return false; }
        req.body.append(buf.data(), static_cast<std::size_t>(n));
    }
    if (req.body.size() > length) req.body.resize(length);
    return true;
}

constexpr std::string_view kHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>FH6 Radio Rework</title>
<style>
:root{font-family:Inter,"Microsoft YaHei UI",system-ui,sans-serif;color-scheme:dark;background:#0d0f12;color:#e9edf1}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(circle at 20% 0,#202933 0,#0d0f12 46%);padding:32px}
main{max-width:900px;margin:auto}.brand{display:flex;justify-content:space-between;align-items:end;margin-bottom:22px}h1{font-size:28px;margin:0}.sub{color:#94a0ad;margin-top:5px}
.card{background:#15191e;border:1px solid #2a3139;border-radius:16px;padding:20px;margin:14px 0;box-shadow:0 18px 50px #0005}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.metric{background:#0f1216;border-radius:12px;padding:13px}.metric b{display:block;font-size:20px;margin-top:4px}.muted{color:#8f9aa6;font-size:13px}
label{display:block;margin:12px 0 6px;color:#abb5bf}select,input[type=range]{width:100%}select,button{background:#20262d;color:#eef2f5;border:1px solid #39434e;border-radius:10px;padding:10px 12px}button{cursor:pointer;margin-right:8px}button.primary{background:#e8edf2;color:#101214;border-color:#e8edf2}button:disabled{opacity:.55}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.error{color:#ff8f8f;white-space:pre-wrap}.ok{color:#9fe2b1}@media(max-width:650px){body{padding:16px}.grid{grid-template-columns:1fr}}
</style></head><body><main>
<div class="brand"><div><h1>FH6 Radio Rework</h1><div class="sub">External Audio / QQ 音乐优先 · 本机控制台</div></div><div class="muted">v0.1</div></div>
<section class="card"><h2>状态</h2><div class="grid">
<div class="metric"><span class="muted">外部音频捕获</span><b id="capture">—</b></div>
<div class="metric"><span class="muted">FH6 DSP</span><b id="dsp">—</b></div>
<div class="metric"><span class="muted">输入格式</span><b id="format">—</b></div>
<div class="metric"><span class="muted">缓冲</span><b id="buffer">—</b></div>
</div><p id="hint" class="muted"></p><div id="error" class="error"></div></section>
<section class="card"><h2>外部音频</h2><label for="device">捕获播放设备</label><select id="device"></select>
<p class="muted">QQ 音乐的 Windows 输出也应指定到同一个播放设备，例如 CABLE Input (VB-Audio Virtual Cable)。</p>
<div class="row"><button class="primary" id="apply">应用设备并重启捕获</button><button id="start">启动捕获</button><button id="stop">停止捕获</button><button id="refresh">刷新设备</button></div></section>
<section class="card"><h2>输出</h2><label>音量 <span id="gainText">100%</span></label><input id="gain" type="range" min="0" max="200" value="100">
<label class="row"><input id="stereo" type="checkbox"> 原生双声道（实验性）</label>
<p class="muted">默认关闭。关闭时保留 FMOD 原本的缓冲区声道结构，但向各声道写入同一个 mono 样本，避免 3D 电台通道的相位问题。</p></section>
<section class="card"><h2>诊断</h2><div id="diag" class="muted">—</div></section>
<section class="card"><h2>音频状态探测</h2>
<div class="row"><button id="saveCockpit">存为 cockpit</button><button id="saveChase">存为 chase</button><button id="compare">对比 cockpit/chase</button></div>
<div id="probe" class="muted">—</div><div id="cmp" class="muted"></div></section>
<section class="card"><h2>相机状态差分（只读）</h2>
<div class="row"><button id="mdCapture">capture</button><button id="mdReset">reset</button></div>
<p class="muted">停车、不换车：驾驶舱点 capture → 切到追尾点 capture → 切回驾驶舱点 capture（第 3 次自动分析，列出随视角翻转的内存位）。</p>
<div id="md" class="muted">—</div></section>
<section class="card"><h2>六视角映射（只读）</h2>
<p class="muted">停车且不换车。第一轮依次切到每个视角并点对应按钮；六个都完成后，再重复第二轮。每个按钮最终应显示 2/2。</p>
<div class="row">
<button data-view="dashboard">仪表盘</button><button data-view="cockpit">驾驶位</button>
<button data-view="chase_near">追尾 1</button><button data-view="chase_far">追尾 2</button>
<button data-view="hood">引擎盖</button><button data-view="bumper">保险杠/车头</button>
<button id="viewReset">重置六视角采集</button></div>
<div id="viewMap" class="muted">尚未采集</div></section>
</main><script>
const $=id=>document.getElementById(id);let state=null;
async function api(path,opt){const r=await fetch(path,opt);if(!r.ok)throw new Error(await r.text());const t=await r.text();return t?JSON.parse(t):{};}
async function devices(){const d=await api('/api/devices');const sel=$('device'),old=state?.config?.endpoint_id||sel.value;sel.textContent='';
 const def=document.createElement('option');def.value='';def.textContent='Windows 默认播放设备';sel.append(def);
 for(const x of d.devices){const o=document.createElement('option');o.value=x.id;o.textContent=(x.is_default?'★ ':'')+x.name;sel.append(o)}sel.value=old;}
async function refresh(){try{state=await api('/api/state');$('capture').textContent=state.capture.running?'运行中':(state.capture.worker_alive?'初始化中':(state.capture.last_error?'错误':'已停止'));$('capture').className=state.capture.running?'ok':'';
 $('dsp').textContent=state.dsp.attached?'已接入 FH6':'未接入';$('dsp').className=state.dsp.attached?'ok':'';$('format').textContent=state.capture.input_rate?`${state.capture.input_channels} ch / ${state.capture.input_rate} Hz`:'—';
 $('buffer').textContent=`${state.ring.readable_frames} / ${state.ring.capacity_frames} 帧`;$('error').textContent=state.capture.last_error||'';
 $('hint').textContent=!state.controller.streamer_mode?(state.controller.station_name?`当前电台：${state.controller.station_name}。请切换到 Streamer Mode。`:'尚未识别电台状态，请确认 FH6 已进入可驾驶场景并启用 Streamer Mode。'):(state.controller.target_found?(state.dsp.attached?`已自动锁定 ${state.controller.sound_name||'当前 Streamer Mode 音轨'} 并挂载 DSP。`:'已找到 active stream，等待有效 FMOD channel。'):'Streamer Mode 已识别，正在等待 active radio stream。');
 $('gain').value=Math.round(state.config.gain*100);$('gainText').textContent=Math.round(state.config.gain*100)+'%';$('stereo').checked=state.config.native_stereo;
 $('diag').textContent=`设备: ${state.capture.device_name||'—'} · 捕获包: ${state.capture.packets} · 捕获帧: ${state.capture.frames_captured} · discontinuity: ${state.capture.discontinuities} · ring overflow: ${state.ring.overflow_frames} · DSP underrun: ${state.dsp.underrun_frames} · rebuffer: ${state.dsp.rebuffer_events} · callbacks: ${state.dsp.callbacks}`;
 }catch(e){$('error').textContent=String(e)}}
$('refresh').onclick=async()=>{await devices();await refresh()};$('apply').onclick=async()=>{await api('/api/device',{method:'POST',headers:{'Content-Type':'text/plain;charset=utf-8'},body:$('device').value});await refresh()};
$('start').onclick=async()=>{await api('/api/capture/start',{method:'POST'});await refresh()};$('stop').onclick=async()=>{await api('/api/capture/stop',{method:'POST'});await refresh()};
$('gain').oninput=()=>{$('gainText').textContent=$('gain').value+'%'};$('gain').onchange=async()=>{await api('/api/gain',{method:'POST',headers:{'Content-Type':'text/plain'},body:String(Number($('gain').value)/100)});await refresh()};
$('stereo').onchange=async()=>{await api('/api/stereo',{method:'POST',headers:{'Content-Type':'text/plain'},body:$('stereo').checked?'1':'0'});await refresh()};
async function refreshProbe(){try{const p=await api('/api/probe/state');let t=p.attached?('已挂载 handle=0x'+p.channel_handle.toString(16)+' · listener '+(p.listener.valid?('fwd=('+p.listener.fwd.map(x=>x.toFixed(2)).join(',')+') up=('+p.listener.up.map(x=>x.toFixed(2)).join(',')+')'):'无效')):'未挂载';if(p.dsps&&p.dsps.length){t+=' · DSP:';p.dsps.forEach((d,i)=>{t+=' ['+i+']type='+d.type+' params=['+d.params.slice(0,d.num_params).map(x=>x.toFixed(2)).join(',')+']';});}$('probe').textContent=t;}catch(e){$('probe').textContent=String(e)}}
$('saveCockpit').onclick=async()=>{await api('/api/probe/snapshot',{method:'POST',headers:{'Content-Type':'text/plain'},body:'cockpit'});await refreshProbe()};
$('saveChase').onclick=async()=>{await api('/api/probe/snapshot',{method:'POST',headers:{'Content-Type':'text/plain'},body:'chase'});await refreshProbe()};
$('compare').onclick=async()=>{try{const c=await api('/api/probe/compare',{method:'POST',headers:{'Content-Type':'text/plain'},body:'cockpit,chase'});$('cmp').textContent=c.changes&&c.changes.length?('变化: '+JSON.stringify(c.changes)):(c.error||'无变化');}catch(e){$('cmp').textContent=String(e)}};
$('mdCapture').onclick=async()=>{try{const r=await api('/api/memdiff/capture',{method:'POST'});$('md').textContent='已捕获 '+r.captures+'/3'+(r.candidates&&r.candidates.length?(' · 候选 '+r.candidates.length+' 个：'+r.candidates.slice(0,20).map(c=>c.offset+'['+c.s1+'→'+c.s2+'→'+c.s3+']').join(' ')):'');}catch(e){$('md').textContent=String(e)}};
$('mdReset').onclick=async()=>{try{await api('/api/memdiff/reset',{method:'POST'});$('md').textContent='已重置';}catch(e){$('md').textContent=String(e)}};
function showViewMap(r){const labels={dashboard:'仪表盘',cockpit:'驾驶位',chase_near:'追尾1',chase_far:'追尾2',hood:'引擎盖',bumper:'保险杠'};let s='阶段: '+r.phase+' · ';for(const [k,v] of Object.entries(r.counts||{}))s+=labels[k]+': '+v+'/2  ';if(r.phase==='complete')s+=' · 稳定候选: '+(r.candidates||[]).length;$('viewMap').textContent=s;}
document.querySelectorAll('[data-view]').forEach(b=>b.onclick=async()=>{try{showViewMap(await api('/api/viewmap/capture',{method:'POST',headers:{'Content-Type':'text/plain'},body:b.dataset.view}))}catch(e){$('viewMap').textContent=String(e)}});
$('viewReset').onclick=async()=>{try{showViewMap(await api('/api/viewmap/reset',{method:'POST'}))}catch(e){$('viewMap').textContent=String(e)}};
(async()=>{await refresh();await devices();refreshProbe();setInterval(refresh,1000);setInterval(refreshProbe,1000)})();
</script></body></html>)HTML";
}

struct HttpServer::Impl {
    ConfigStore& config;
    AudioRing& ring;
    WasapiCapture& capture;
    fmod::DSPBridge& dsp;
    fmod::Controller& controller;
    AudioStateProbe& probe;
    MemoryDiff& memdiff;
    std::uint16_t requested_port;
    std::atomic<std::uint16_t>* published_port;
    std::atomic<bool> stopping{false};
    std::atomic<SOCKET> server{INVALID_SOCKET};
    std::thread thread;
    std::map<std::string, AudioStateSnapshot> saved;

    Impl(std::uint16_t p, ConfigStore& c, AudioRing& r, WasapiCapture& cap,
         fmod::DSPBridge& d, fmod::Controller& ctl, AudioStateProbe& pr, MemoryDiff& md,
         std::atomic<std::uint16_t>* pub)
        : config{c}, ring{r}, capture{cap}, dsp{d}, controller{ctl}, probe{pr}, memdiff{md},
          requested_port{p}, published_port{pub},
          thread{[this] { run(); }} {}
    ~Impl() {
        stopping.store(true, std::memory_order_release);
        const SOCKET s = server.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
        if (s != INVALID_SOCKET) closesocket(s);
        if (thread.joinable()) thread.join();
    }

    std::string state_json() {
        const auto c = config.snapshot();
        const auto cap = capture.stats();
        const auto ds = dsp.stats();
        const auto cs = controller.stats();
        std::ostringstream o;
        o << "{\"config\":{\"endpoint_id\":\"" << json_escape(c.endpoint_id)
          << "\",\"gain\":" << c.gain << ",\"native_stereo\":" << (c.native_stereo?"true":"false")
          << "},\"capture\":{\"worker_alive\":" << (cap.worker_alive?"true":"false")
          << ",\"running\":" << (cap.running?"true":"false")
          << ",\"input_rate\":" << cap.input_rate << ",\"input_channels\":" << cap.input_channels
          << ",\"packets\":" << cap.packets << ",\"frames_captured\":" << cap.frames_captured
          << ",\"discontinuities\":" << cap.discontinuities << ",\"event_timeouts\":" << cap.event_timeouts
          << ",\"device_name\":\"" << json_escape(cap.device_name) << "\",\"last_error\":\"" << json_escape(cap.last_error)
          << "\"},\"ring\":{\"readable_frames\":" << ring.readable_frames() << ",\"capacity_frames\":" << ring.capacity_frames()
          << ",\"overflow_frames\":" << ring.overflow_count() << "},\"dsp\":{\"attached\":" << (ds.attached?"true":"false")
          << ",\"channel_handle\":" << ds.channel_handle << ",\"callbacks\":" << ds.callbacks
          << ",\"underrun_frames\":" << ds.underrun_frames << ",\"rebuffer_events\":" << ds.rebuffer_events
          << ",\"primed\":" << (ds.primed?"true":"false") << ",\"last_frames\":" << ds.last_frames
          << ",\"last_channels\":" << ds.last_channels << "},\"controller\":{\"target_found\":" << (cs.target_found?"true":"false")
          << ",\"streamer_mode\":" << (cs.streamer_mode?"true":"false")
          << ",\"station_name\":\"" << json_escape(cs.station_name) << "\""
          << ",\"sound_name\":\"" << json_escape(cs.sound_name) << "\"}}";
        return o.str();
    }

    std::string devices_json() {
        const auto devices = enumerate_audio_devices();
        std::ostringstream o; o << "{\"devices\":[";
        for (std::size_t i=0;i<devices.size();++i) {
            if (i) o << ',';
            o << "{\"id\":\"" << json_escape(devices[i].id) << "\",\"name\":\"" << json_escape(devices[i].name)
              << "\",\"is_default\":" << (devices[i].is_default?"true":"false") << '}';
        }
        o << "]}"; return o.str();
    }

    std::string probe_state_json() {
        const auto s = probe.snapshot();
        std::ostringstream o;
        o << "{\"attached\":" << (s.attached ? "true" : "false")
          << ",\"channel_handle\":" << s.channel_handle
          << ",\"taken_ms\":" << s.taken_ms
          << ",\"listener\":{\"valid\":" << (s.listener.valid ? "true" : "false");
        if (s.listener.valid) {
            o << ",\"pos\":[" << s.listener.pos.x << ',' << s.listener.pos.y << ',' << s.listener.pos.z << ']'
              << ",\"fwd\":[" << s.listener.fwd.x << ',' << s.listener.fwd.y << ',' << s.listener.fwd.z << ']'
              << ",\"up\":[" << s.listener.up.x << ',' << s.listener.up.y << ',' << s.listener.up.z << ']';
        }
        o << "},\"dsps\":[";
        for (std::size_t i = 0; i < s.dsps.size(); ++i) {
            if (i) o << ',';
            const auto& d = s.dsps[i];
            o << "{\"type\":" << d.type << ",\"num_params\":" << d.num_params << ",\"params\":[";
            for (std::int32_t k = 0; k < d.num_params && k < 16; ++k) {
                if (k) o << ',';
                o << d.params[k];
            }
            o << "]}";
        }
        o << "]}";
        return o.str();
    }

    std::string save_probe_snapshot(const std::string& name) {
        if (name.empty() || name.find_first_of("/\\.") != std::string::npos)
            return "{\"error\":\"invalid name\"}";
        const auto s = probe.snapshot();
        saved[name] = s;
        std::error_code ec;
        std::ofstream f(config.path().parent_path() / (name + ".json"), std::ios::binary | std::ios::trunc);
        if (f) f << probe_state_json();
        return "{\"saved\":\"" + json_escape(name) + "\"}";
    }

    std::string compare_probe(const std::string& a, const std::string& b) {
        auto ia = saved.find(a), ib = saved.find(b);
        if (ia == saved.end() || ib == saved.end()) return "{\"error\":\"unknown snapshot\"}";
        const auto& A = ia->second;
        const auto& B = ib->second;
        std::ostringstream o;
        o << "{\"a\":\"" << json_escape(a) << "\",\"b\":\"" << json_escape(b) << "\",\"changes\":[";
        bool first = true;
        auto sep = [&] { if (!first) o << ','; first = false; };
        auto vec_changed = [](const fmod::FMOD_VEC& x, const fmod::FMOD_VEC& y) {
            return std::abs(x.x - y.x) > 0.001f || std::abs(x.y - y.y) > 0.001f || std::abs(x.z - y.z) > 0.001f;
        };
        auto dump_vec = [&](const fmod::FMOD_VEC& v) {
            o << '[' << v.x << ',' << v.y << ',' << v.z << ']';
        };
        if (A.listener.valid && B.listener.valid) {
            if (vec_changed(A.listener.fwd, B.listener.fwd)) {
                sep(); o << "{\"field\":\"listener.fwd\",\"a\":"; dump_vec(A.listener.fwd); o << ",\"b\":"; dump_vec(B.listener.fwd); o << '}';
            }
            if (vec_changed(A.listener.up, B.listener.up)) {
                sep(); o << "{\"field\":\"listener.up\",\"a\":"; dump_vec(A.listener.up); o << ",\"b\":"; dump_vec(B.listener.up); o << '}';
            }
        } else if (A.listener.valid != B.listener.valid) {
            sep(); o << "{\"field\":\"listener.valid\",\"a\":" << (A.listener.valid ? "true" : "false")
                     << ",\"b\":" << (B.listener.valid ? "true" : "false") << '}';
        }
        const std::size_t max_dsps = std::max(A.dsps.size(), B.dsps.size());
        for (std::size_t i = 0; i < max_dsps; ++i) {
            if (i >= A.dsps.size() || i >= B.dsps.size()) {
                sep(); o << "{\"field\":\"dsp[" << i << "].present\",\"a\":"
                         << (i < A.dsps.size() ? "true" : "false") << ",\"b\":"
                         << (i < B.dsps.size() ? "true" : "false") << '}';
                continue;
            }
            const auto& da = A.dsps[i];
            const auto& db = B.dsps[i];
            if (da.type != db.type) {
                sep(); o << "{\"field\":\"dsp[" << i << "].type\",\"a\":" << da.type << ",\"b\":" << db.type << '}';
            }
            const int np = std::min(da.num_params, db.num_params);
            for (int k = 0; k < np; ++k) {
                if (std::abs(da.params[k] - db.params[k]) > 0.001f) {
                    sep(); o << "{\"field\":\"dsp[" << i << "].param[" << k << "]\",\"a\":" << da.params[k]
                             << ",\"b\":" << db.params[k] << '}';
                }
            }
            if (da.num_params != db.num_params) {
                sep(); o << "{\"field\":\"dsp[" << i << "].num_params\",\"a\":" << da.num_params
                         << ",\"b\":" << db.num_params << '}';
            }
        }
        o << "]}";
        return o.str();
    }

    void handle(SOCKET client) {
        DWORD timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        Request req; int err=400;
        if (!read_request(client, req, err)) { respond(client, err, "{\"error\":\"bad request\"}"); return; }
        const std::string port = std::to_string(requested_port);
        const std::string host = lower_ascii(req.host);
        const bool host_ok = host == "127.0.0.1:" + port || host == "localhost:" + port;
        if (!host_ok) { respond(client,400,"{\"error\":\"invalid host\"}"); return; }
        if (req.method == "POST" && !req.origin.empty()) {
            const std::string origin = lower_ascii(req.origin);
            const bool origin_ok = origin == "http://127.0.0.1:" + port ||
                                   origin == "http://localhost:" + port;
            if (!origin_ok) { respond(client,400,"{\"error\":\"cross-origin request rejected\"}"); return; }
        }
        if (req.method == "GET" && req.path == "/") { respond(client, 200, kHtml, "text/html; charset=utf-8"); return; }
        if (req.method == "GET" && req.path == "/api/state") { respond(client, 200, state_json()); return; }
        if (req.method == "GET" && req.path == "/api/devices") { respond(client, 200, devices_json()); return; }
        if (req.method == "GET" && req.path == "/api/probe/state") { respond(client, 200, probe_state_json()); return; }
        if (req.method == "POST" && req.path == "/api/probe/snapshot") {
            respond(client, 200, save_probe_snapshot(req.body)); return;
        }
        if (req.method == "POST" && req.path == "/api/probe/compare") {
            const auto comma = req.body.find(',');
            if (comma == std::string::npos) { respond(client, 400, "{\"error\":\"need a,b\"}"); return; }
            respond(client, 200, compare_probe(req.body.substr(0, comma), req.body.substr(comma + 1)));
            return;
        }
        if (req.method == "POST" && req.path == "/api/memdiff/capture") {
            memdiff.capture(); respond(client, 200, memdiff.result_json()); return;
        }
        if (req.method == "POST" && req.path == "/api/memdiff/reset") {
            memdiff.reset(); respond(client, 200, memdiff.result_json()); return;
        }
        if (req.method == "GET" && req.path == "/api/memdiff/result") {
            respond(client, 200, memdiff.result_json()); return;
        }
        if (req.method == "POST" && req.path == "/api/viewmap/capture") {
            if (!memdiff.capture_view(req.body)) {
                respond(client, 400, "{\"error\":\"invalid view label, duplicate, or wrong round order\"}");
                return;
            }
            respond(client, 200, memdiff.view_map_json()); return;
        }
        if (req.method == "POST" && req.path == "/api/viewmap/reset") {
            memdiff.reset_view_map(); respond(client, 200, memdiff.view_map_json()); return;
        }
        if (req.method == "GET" && req.path == "/api/viewmap/result") {
            respond(client, 200, memdiff.view_map_json()); return;
        }
        if (req.method == "POST" && req.path == "/api/capture/start") {
            if (!capture.start()) { respond(client,400,"{\"error\":\"capture start failed\"}"); return; }
            respond(client,200,"{}"); return;
        }
        if (req.method == "POST" && req.path == "/api/capture/stop") { capture.stop(); ring.request_reset(); respond(client,200,"{}"); return; }
        if (req.method == "POST" && req.path == "/api/device") {
            if (req.body.size() > 2048 || req.body.find_first_of("\r\n") != std::string::npos) {
                respond(client,400,"{\"error\":\"invalid endpoint\"}"); return;
            }
            config.set_endpoint(req.body); config.save(); capture.set_endpoint(req.body); capture.restart();
            respond(client,200,"{}"); return;
        }
        if (req.method == "POST" && req.path == "/api/gain") {
            try {
                const float g = std::stof(req.body);
                if (!std::isfinite(g)) throw std::runtime_error("non-finite");
                config.set_gain(g); config.save(); dsp.set_gain(config.snapshot().gain);
            } catch (...) { respond(client,400,"{\"error\":\"invalid gain\"}"); return; }
            respond(client,200,"{}"); return;
        }
        if (req.method == "POST" && req.path == "/api/stereo") {
            const bool v = req.body == "1" || req.body == "true"; config.set_native_stereo(v); config.save(); dsp.set_native_stereo(v);
            respond(client,200,"{}"); return;
        }
        respond(client,404,"{\"error\":\"not found\"}");
    }

    void run() noexcept {
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2,2), &w) != 0) return;
        const SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv == INVALID_SOCKET) { WSACleanup(); return; }
        server.store(srv, std::memory_order_release);
        BOOL yes = TRUE;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(requested_port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); // localhost only by design
        if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(srv, 8) == SOCKET_ERROR) {
            log::error("[http] cannot bind 127.0.0.1:{} (WSA {})", requested_port, WSAGetLastError());
            const SOCKET owned = server.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
            if (owned != INVALID_SOCKET) closesocket(owned);
            WSACleanup(); return;
        }
        published_port->store(requested_port, std::memory_order_release);
        log::info("[http] dashboard http://127.0.0.1:{}", requested_port);
        while (!stopping.load(std::memory_order_acquire)) {
            fd_set set; FD_ZERO(&set); FD_SET(srv, &set); timeval tv{0, 250000};
            const int ready = select(0, &set, nullptr, nullptr, &tv);
            if (ready <= 0) continue;
            SOCKET client = accept(srv, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            handle(client);
            closesocket(client);
        }
        const SOCKET owned = server.exchange(INVALID_SOCKET, std::memory_order_acq_rel);
        if (owned != INVALID_SOCKET) closesocket(owned);
        WSACleanup();
    }
};

HttpServer::HttpServer(std::uint16_t p, ConfigStore& c, AudioRing& r, WasapiCapture& cap,
                       fmod::DSPBridge& d, fmod::Controller& ctl, AudioStateProbe& pr,
                       MemoryDiff& md) {
    impl_ = new Impl{p,c,r,cap,d,ctl,pr,md,&port_};
}
HttpServer::~HttpServer() { delete impl_; impl_ = nullptr; }
} // namespace fh6r::http
