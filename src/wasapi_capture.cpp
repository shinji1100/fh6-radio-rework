#include "fh6r/wasapi_capture.hpp"
#include "fh6r/log.hpp"
#include "fh6r/drift_control.hpp"

#include <windows.h>
#include <propidl.h>
#include <propkeydef.h>
#include <propsys.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string_view>

namespace fh6r {
namespace {
constexpr std::uint32_t kOutputRate = 48000;

template <class T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_{o.p_} { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T** put() noexcept { reset(); return &p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset(T* p = nullptr) noexcept { if (p_) p_->Release(); p_ = p; }
private:
    T* p_ = nullptr;
};

struct CoMemFormat {
    WAVEFORMATEX* p = nullptr;
    ~CoMemFormat() { if (p) CoTaskMemFree(p); }
    WAVEFORMATEX** put() noexcept { if (p) CoTaskMemFree(p); p = nullptr; return &p; }
};
struct CoMemString {
    wchar_t* p = nullptr;
    ~CoMemString() { if (p) CoTaskMemFree(p); }
    wchar_t** put() noexcept { if (p) CoTaskMemFree(p); p = nullptr; return &p; }
};

std::string hr_text(const char* where, HRESULT hr) {
    char buf[192]{};
    std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", where,
                  static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return buf;
}
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string wide_to_utf8(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring lower(std::wstring s) {
    std::ranges::transform(s, s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}
std::string friendly_name(IMMDevice* d) {
    if (!d) return {};
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, ps.put())) || !ps) return {};
    PROPVARIANT v{};
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out = wide_to_utf8(v.pwszVal);
    PropVariantClear(&v);
    return out;
}
std::string device_id(IMMDevice* d) {
    CoMemString id;
    if (!d || FAILED(d->GetId(id.put())) || !id.p) return {};
    return wide_to_utf8(id.p);
}
HRESULT select_device(IMMDeviceEnumerator* e, const std::string& endpoint, IMMDevice** out) {
    if (endpoint.empty()) return e->GetDefaultAudioEndpoint(eRender, eConsole, out);
    const auto wanted = utf8_to_wide(endpoint);
    HRESULT hr = e->GetDevice(wanted.c_str(), out);
    if (SUCCEEDED(hr)) return hr;
    ComPtr<IMMDeviceCollection> coll;
    hr = e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, coll.put());
    if (FAILED(hr) || !coll) return hr;
    UINT count = 0;
    if (FAILED(coll->GetCount(&count))) return E_NOTFOUND;
    const auto needle = lower(wanted);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(coll->Item(i, d.put())) || !d) continue;
        auto n = lower(utf8_to_wide(friendly_name(d.get())));
        auto id = lower(utf8_to_wide(device_id(d.get())));
        if ((!n.empty() && n.find(needle) != std::wstring::npos) ||
            (!id.empty() && id.find(needle) != std::wstring::npos)) {
            *out = d.get(); (*out)->AddRef(); return S_OK;
        }
    }
    return E_NOTFOUND;
}

struct InputFormat {
    std::uint32_t rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t container_bits = 0;
    std::uint16_t valid_bits = 0;
    std::uint16_t block_align = 0;
    bool is_float = false;
};

bool parse_format(const WAVEFORMATEX& f, InputFormat& o) {
    o.rate = f.nSamplesPerSec;
    o.channels = f.nChannels;
    o.container_bits = f.wBitsPerSample;
    o.valid_bits = f.wBitsPerSample;
    o.block_align = f.nBlockAlign;
    if (!o.rate || !o.channels || o.channels > 32 || !o.container_bits || !o.block_align) return false;
    const auto valid_container = [](std::uint16_t bits, bool fp) {
        return fp ? (bits == 32 || bits == 64)
                  : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    };
    if (f.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        o.is_float = true;
        return valid_container(o.container_bits, true) &&
               o.block_align >= o.channels * ((o.container_bits + 7u) / 8u);
    }
    if (f.wFormatTag == WAVE_FORMAT_PCM) {
        o.is_float = false;
        return valid_container(o.container_bits, false) &&
               o.block_align >= o.channels * ((o.container_bits + 7u) / 8u);
    }
    if (f.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        f.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& x = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(f);
        o.container_bits = x.Format.wBitsPerSample;
        o.valid_bits = x.Samples.wValidBitsPerSample ? x.Samples.wValidBitsPerSample : o.container_bits;
        o.block_align = x.Format.nBlockAlign;
        if (o.valid_bits > o.container_bits) o.valid_bits = o.container_bits;
        if (IsEqualGUID(x.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            o.is_float = true;
            return valid_container(o.container_bits, true) &&
                   o.block_align >= o.channels * ((o.container_bits + 7u) / 8u);
        }
        if (IsEqualGUID(x.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            o.is_float = false;
            return valid_container(o.container_bits, false) &&
                   o.block_align >= o.channels * ((o.container_bits + 7u) / 8u);
        }
    }
    return false;
}

float clampf(float x) noexcept { return std::isfinite(x) ? std::clamp(x, -1.0f, 1.0f) : 0.0f; }
std::int32_t read24(const BYTE* p) noexcept {
    std::int32_t v = static_cast<std::int32_t>(p[0]) |
                     (static_cast<std::int32_t>(p[1]) << 8) |
                     (static_cast<std::int32_t>(p[2]) << 16);
    if (v & 0x00800000) v |= static_cast<std::int32_t>(0xFF000000u);
    return v;
}
float read_sample(const BYTE* p, std::uint16_t bits, std::uint16_t valid, bool fp) noexcept {
    if (fp) {
        if (bits == 32) { float v{}; std::memcpy(&v, p, 4); return clampf(v); }
        if (bits == 64) { double v{}; std::memcpy(&v, p, 8); return clampf(static_cast<float>(v)); }
        return 0.0f;
    }
    if (!valid || valid > bits || valid > 32) valid = bits;
    if (bits == 8) return clampf((static_cast<int>(*p) - 128) / 128.0f);
    std::int32_t v = 0;
    if (bits == 16) { std::int16_t q{}; std::memcpy(&q, p, 2); v = q; }
    else if (bits == 24) v = read24(p);
    else if (bits == 32) std::memcpy(&v, p, 4);
    else return 0.0f;
    if (valid < bits) v >>= (bits - valid);
    const double denom = static_cast<double>(std::uint64_t{1} << (valid - 1));
    return clampf(static_cast<float>(static_cast<double>(v) / denom));
}
std::int16_t to_s16(float x) noexcept {
    x = clampf(x);
    if (x >= 0.9999695f) return std::numeric_limits<std::int16_t>::max();
    if (x <= -1.0f) return std::numeric_limits<std::int16_t>::min();
    return static_cast<std::int16_t>(std::lrintf(x * 32767.0f));
}

struct FloatFrame { float l = 0; float r = 0; };

class AdaptiveResampler {
public:
    AdaptiveResampler(InputFormat fmt, AudioRing& ring) : fmt_{fmt}, ring_{ring} {}

    void push(const BYTE* data, UINT32 frames, DWORD flags) noexcept {
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            have_prev_ = false;
            phase_ = 0.0;
            ring_.request_reset();
        }
        if (frames == 0) return;

        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const std::size_t total = static_cast<std::size_t>(frames) + (have_prev_ ? 1u : 0u);
        if (total < 2) {
            prev_ = decode_frame(data, frames - 1, silent);
            have_prev_ = true;
            return;
        }

        // Input and FMOD may both advertise 48 kHz but still run from slightly
        // different physical clocks. The bounded ratio correction prevents a
        // slow multi-minute drift into overflow/underrun.
        const double step = drift::resample_step(fmt_.rate, kOutputRate,
                                                 ring_.readable_frames(), ring_.capacity_frames());

        std::array<StereoFrame, 1024> out{};
        std::size_t out_count = 0;
        auto flush = [&]() noexcept {
            if (out_count) {
                ring_.push(out.data(), out_count);
                out_count = 0;
            }
        };
        auto at = [&](std::size_t index) noexcept -> FloatFrame {
            if (have_prev_) {
                if (index == 0) return prev_;
                return decode_frame(data, static_cast<UINT32>(index - 1), silent);
            }
            return decode_frame(data, static_cast<UINT32>(index), silent);
        };

        while (phase_ + 1.0 < static_cast<double>(total)) {
            const auto i = static_cast<std::size_t>(phase_);
            const float a = static_cast<float>(phase_ - static_cast<double>(i));
            const auto x = at(i);
            const auto y = at(i + 1);
            out[out_count++] = {
                to_s16(x.l + (y.l - x.l) * a),
                to_s16(x.r + (y.r - x.r) * a)
            };
            if (out_count == out.size()) flush();
            phase_ += step;
        }
        flush();

        prev_ = decode_frame(data, frames - 1, silent);
        have_prev_ = true;
        phase_ -= static_cast<double>(total - 1u);
        if (phase_ < 0.0 || phase_ > step + 1.0) phase_ = 0.0; // defensive recovery
    }

private:
    FloatFrame decode_frame(const BYTE* data, UINT32 index, bool silent) const noexcept {
        if (silent || !data) return {};
        const auto bytes = static_cast<std::uint16_t>((fmt_.container_bits + 7u) / 8u);
        const BYTE* frame = data + static_cast<std::size_t>(index) * fmt_.block_align;
        FloatFrame f{};
        f.l = read_sample(frame, fmt_.container_bits, fmt_.valid_bits, fmt_.is_float);
        f.r = fmt_.channels > 1
            ? read_sample(frame + bytes, fmt_.container_bits, fmt_.valid_bits, fmt_.is_float)
            : f.l;
        return f;
    }

    InputFormat fmt_;
    AudioRing& ring_;
    FloatFrame prev_{};
    bool have_prev_ = false;
    double phase_ = 0.0;
};
} // namespace

std::vector<AudioDeviceInfo> enumerate_audio_devices() {
    std::vector<AudioDeviceInfo> out;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return out;
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put()))) || !e) {
        if (uninit) CoUninitialize(); return out;
    }
    std::string def_id;
    { ComPtr<IMMDevice> d; if (SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, d.put())) && d) def_id = device_id(d.get()); }
    ComPtr<IMMDeviceCollection> c;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, c.put())) || !c) {
        if (uninit) CoUninitialize(); return out;
    }
    UINT count = 0; c->GetCount(&count);
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(c->Item(i, d.put())) || !d) continue;
        AudioDeviceInfo x{device_id(d.get()), friendly_name(d.get()), false};
        if (x.name.empty()) x.name = x.id;
        x.is_default = !def_id.empty() && x.id == def_id;
        if (!x.id.empty()) out.push_back(std::move(x));
    }
    std::ranges::stable_sort(out, [](const auto& a, const auto& b) {
        if (a.is_default != b.is_default) return a.is_default;
        return a.name < b.name;
    });
    if (uninit) CoUninitialize();
    return out;
}

WasapiCapture::WasapiCapture(AudioRing& ring) : ring_{ring} {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
WasapiCapture::~WasapiCapture() {
    stop();
    if (stop_event_) CloseHandle(static_cast<HANDLE>(stop_event_));
}
void WasapiCapture::set_endpoint(std::string endpoint) {
    std::scoped_lock lk{mu_}; endpoint_ = std::move(endpoint);
}
std::string WasapiCapture::endpoint() const { std::scoped_lock lk{mu_}; return endpoint_; }
void WasapiCapture::set_error(std::string msg) noexcept {
    { std::scoped_lock lk{mu_}; last_error_ = std::move(msg); }
    log::warn("[capture] {}", last_error_);
}
bool WasapiCapture::start() {
    if (!stop_event_) return false;
    if (thread_.joinable()) {
        if (worker_alive_.load(std::memory_order_acquire)) return true;
        try { thread_.join(); } catch (...) { return false; }
    }
    ResetEvent(static_cast<HANDLE>(stop_event_));
    worker_alive_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    input_rate_.store(0, std::memory_order_relaxed);
    input_channels_.store(0, std::memory_order_relaxed);
    { std::scoped_lock lk{mu_}; last_error_.clear(); device_name_.clear(); }
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        worker_alive_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}
void WasapiCapture::stop() noexcept {
    if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
    if (thread_.joinable()) { try { thread_.join(); } catch (...) {} }
    running_.store(false, std::memory_order_release);
}
bool WasapiCapture::restart() { stop(); ring_.request_reset(); return start(); }
CaptureStats WasapiCapture::stats() const {
    CaptureStats s;
    s.worker_alive = worker_alive_.load(std::memory_order_acquire);
    s.running = running_.load(std::memory_order_acquire);
    s.input_rate = input_rate_.load(std::memory_order_relaxed);
    s.input_channels = input_channels_.load(std::memory_order_relaxed);
    s.packets = packets_.load(std::memory_order_relaxed);
    s.frames_captured = frames_captured_.load(std::memory_order_relaxed);
    s.discontinuities = discontinuities_.load(std::memory_order_relaxed);
    s.event_timeouts = event_timeouts_.load(std::memory_order_relaxed);
    { std::scoped_lock lk{mu_}; s.device_name = device_name_; s.last_error = last_error_; }
    return s;
}

void WasapiCapture::run() noexcept {
    struct ExitMark { std::atomic<bool>& alive; ~ExitMark() { alive.store(false, std::memory_order_release); } } exit_mark{worker_alive_};
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_uninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { set_error(hr_text("CoInitializeEx", hr)); return; }

    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    ComPtr<IMMDeviceEnumerator> e;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(e.put()));
    if (FAILED(hr) || !e) { set_error(hr_text("MMDeviceEnumerator", hr)); goto cleanup; }

    {
        ComPtr<IMMDevice> d;
        hr = select_device(e.get(), endpoint(), d.put());
        if (FAILED(hr) || !d) { set_error(hr_text("Select playback device", hr)); goto cleanup; }
        { std::scoped_lock lk{mu_}; device_name_ = friendly_name(d.get()); }

        ComPtr<IAudioClient> client;
        hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.put()));
        if (FAILED(hr) || !client) { set_error(hr_text("Activate IAudioClient", hr)); goto cleanup; }

        CoMemFormat mix;
        hr = client->GetMixFormat(mix.put());
        if (FAILED(hr) || !mix.p) { set_error(hr_text("GetMixFormat", hr)); goto cleanup; }
        InputFormat fmt;
        if (!parse_format(*mix.p, fmt)) { set_error("Unsupported WASAPI mix format"); goto cleanup; }
        input_rate_.store(fmt.rate, std::memory_order_release);
        input_channels_.store(fmt.channels, std::memory_order_release);

        HANDLE sample_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sample_event) { set_error("CreateEvent(sample) failed"); goto cleanup; }

        constexpr DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                AUDCLNT_STREAMFLAGS_NOPERSIST;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix.p, nullptr);
        if (FAILED(hr)) {
            CloseHandle(sample_event);
            set_error(hr_text("IAudioClient::Initialize(loopback,event)", hr));
            goto cleanup;
        }
        hr = client->SetEventHandle(sample_event);
        if (FAILED(hr)) {
            CloseHandle(sample_event);
            set_error(hr_text("IAudioClient::SetEventHandle", hr));
            goto cleanup;
        }

        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.put()));
        if (FAILED(hr) || !capture) {
            CloseHandle(sample_event);
            set_error(hr_text("GetService(IAudioCaptureClient)", hr));
            goto cleanup;
        }

        AdaptiveResampler resampler{fmt, ring_};
        hr = client->Start();
        if (FAILED(hr)) {
            CloseHandle(sample_event);
            set_error(hr_text("IAudioClient::Start", hr));
            goto cleanup;
        }
        running_.store(true, std::memory_order_release);
        log::info("[capture] event-driven loopback: {} ch @ {} Hz from '{}'",
                  fmt.channels, fmt.rate, friendly_name(d.get()));

        HANDLE waits[2] = {static_cast<HANDLE>(stop_event_), sample_event};
        bool done = false;
        while (!done) {
            const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 2000);
            if (wr == WAIT_OBJECT_0) break;
            if (wr == WAIT_TIMEOUT) { event_timeouts_.fetch_add(1, std::memory_order_relaxed); continue; }
            if (wr != WAIT_OBJECT_0 + 1) { set_error("WaitForMultipleObjects failed"); break; }

            for (;;) {
                UINT32 next = 0;
                hr = capture->GetNextPacketSize(&next);
                if (FAILED(hr)) { set_error(hr_text("GetNextPacketSize", hr)); done = true; break; }
                if (!next) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD packet_flags = 0;
                hr = capture->GetBuffer(&data, &frames, &packet_flags, nullptr, nullptr);
                if (FAILED(hr)) { set_error(hr_text("GetBuffer", hr)); done = true; break; }
                if (packet_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                    discontinuities_.fetch_add(1, std::memory_order_relaxed);
                resampler.push(data, frames, packet_flags);
                packets_.fetch_add(1, std::memory_order_relaxed);
                frames_captured_.fetch_add(frames, std::memory_order_relaxed);
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        running_.store(false, std::memory_order_release);
        CloseHandle(sample_event);
    }

cleanup:
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (co_uninit) CoUninitialize();
    running_.store(false, std::memory_order_release);
}
} // namespace fh6r
