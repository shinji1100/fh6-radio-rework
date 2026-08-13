#include "fh6r/audio_ring.hpp"
#include "fh6r/audio_state_probe.hpp"
#include "fh6r/config.hpp"
#include "fh6r/fmod/controller.hpp"
#include "fh6r/fmod/dsp_bridge.hpp"
#include "fh6r/fmod/pe_image.hpp"
#include "fh6r/fmod/sig_scout.hpp"
#include "fh6r/fmod/studio_api_scout.hpp"
#include "fh6r/fmod/studio_param_hook.hpp"
#include "fh6r/fmod/studio_probe.hpp"
#include "fh6r/fmod/studio_system_scout.hpp"
#include "fh6r/http/http_server.hpp"
#include "fh6r/log.hpp"
#include "fh6r/memory_diff.hpp"
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

        // Phase 1 (Studio path, superseded): the old vtable-reverse-lookup
        // locate_studio_system is retired — Studio objects are packed handles,
        // not vtable objects, so that approach was premised wrong.
        // if (image.valid()) fmod::studio_scout(image);

        // Studio enumeration route, Step 1: resolve the 13 Studio APIs and report
        // MISSING / RESOLVED / AMBIGUOUS per anchor. Read-only; no API is called
        // and no object memory is inspected (only address reliability).
        if (image.valid()) fmod::scout_studio_api(image);

        // Studio enumeration route, Step 2: resolve the Studio factory/lifecycle
        // methods and scan .text for FH6 call xrefs, dumping the RCX source (the
        // Studio System out-param / handle storage). Read-only.
        if (image.valid()) fmod::scout_studio_system(image);

        // Phase 1 (interior/exterior state): observe the FMOD Studio parameter
        // setters (read-only detour) to confirm the "Cockpit" global parameter
        // flips on cockpit<->chase. Fail-safe; original functions always run.
        if (image.valid()) fmod::install_studio_param_hooks(image);

        fmod::DSPBridge dsp{ring, fns};
        dsp.set_gain(cfg.gain);
        dsp.set_native_stereo(cfg.native_stereo);
        fmod::Controller controller{dsp, image};
        AudioStateProbe probe{dsp, data_dir.string()};
        MemoryDiff memdiff{image};
        http::HttpServer http{cfg.dashboard_port, config, ring, capture, dsp, controller, probe, memdiff};

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
