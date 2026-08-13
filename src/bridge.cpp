#include "fh6r/audio_ring.hpp"
#include "fh6r/config.hpp"
#include "fh6r/fmod/controller.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/fmod/sig_scout.hpp"
#include "fh6r/http/http_server.hpp"
#include "fh6r/log.hpp"
#include "fh6r/wasapi_capture.hpp"

#include <windows.h>
#include <chrono>
#include <exception>
#include <filesystem>
#include <thread>

namespace fh6r {
namespace {
std::filesystem::path module_directory(HMODULE module) {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return std::filesystem::current_path();
    buf.resize(n);
    return std::filesystem::path{buf}.parent_path();
}
}

void run_bridge(HMODULE self) noexcept {
    try {
        const auto root = module_directory(self);
        const auto data_dir = root / L"fh6-radio-rework";
        std::error_code ec;
        std::filesystem::create_directories(data_dir, ec);
        log::init(data_dir / L"fh6-radio-rework.log");
        log::info("FH6 Radio Rework v0.1 starting");

        ConfigStore config{data_dir / L"config.ini"};
        const bool config_existed = std::filesystem::exists(config.path(), ec);
        auto cfg = config.load();
        if (!config_existed) config.save();

        AudioRing ring{16384};
        WasapiCapture capture{ring};
        capture.set_endpoint(cfg.endpoint_id);
        capture.start();

        auto* host = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
        const auto image = fmod::parse(host);
        fmod::FMODFns fns{};
        if (!image.valid()) log::error("[bridge] could not parse game PE image");
        else if (!fmod::resolve_fmod_signatures(image, fns))
            log::error("[bridge] one or more mandatory FMOD signatures were not resolved");

        // Phase 1 audio-state probe: scout the wider FMOD API surface so the
        // next iteration gets confirmed byte patterns for the cabin-acoustics
        // state enumerator. Read-only, runs once at startup.
        if (image.valid()) fmod::scout_apis(image);

        fmod::DSPBridge dsp{ring, fns};
        dsp.set_gain(cfg.gain);
        dsp.set_native_stereo(cfg.native_stereo);
        fmod::Controller controller{dsp, image};
        http::HttpServer http{cfg.dashboard_port, config, ring, capture, dsp, controller};

        // version.dll stays resident for the lifetime of the game. Keep all
        // services owned by this bootstrap thread; process teardown will end it.
        while (true) std::this_thread::sleep_for(std::chrono::seconds(30));
    } catch (const std::exception& e) {
        log::error("[bridge] fatal exception: {}", e.what());
    } catch (...) {
        log::error("[bridge] fatal unknown exception");
    }
}
} // namespace fh6r
