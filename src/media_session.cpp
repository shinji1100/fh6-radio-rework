#include "fh6r/media_session.hpp"
#include "fh6r/log.hpp"

#if __has_include(<winrt/Windows.Foundation.h>) && \
    __has_include(<winrt/Windows.Media.Control.h>)
#define FH6R_HAS_WINRT 1
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#else
#define FH6R_HAS_WINRT 0
#endif

#include <chrono>
#include <thread>
#include <utility>

namespace fh6r {
namespace {
using namespace std::chrono_literals;
}

MediaSessionProvider::MediaSessionProvider()
    : thread_{[this](std::stop_token st) { run(st); }} {}

MediaSessionProvider::~MediaSessionProvider() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
}

bool MediaSessionProvider::info(Info& out) const {
    std::scoped_lock lk{mu_};
    if (!has_info_) return false;
    out = info_;
    return true;
}

void MediaSessionProvider::run(std::stop_token st) {
#if FH6R_HAS_WINRT
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        log::warn("[media] winrt init failed; Now Playing metadata disabled");
        return;
    }

    while (!st.stop_requested()) {
        try {
            using namespace winrt::Windows::Media::Control;
            auto manager =
                GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (session) {
                auto props = session.TryGetMediaPropertiesAsync().get();
                if (props) {
                    Info i;
                    i.title  = winrt::to_string(props.Title());
                    i.artist = winrt::to_string(props.Artist());
                    {
                        std::scoped_lock lk{mu_};
                        info_ = std::move(i);
                        has_info_ = true;
                    }
                }
            }
        } catch (...) {
            // Session churn / transient failures: keep polling.
        }
        std::this_thread::sleep_for(1s);
    }
    winrt::uninit_apartment();
#else
    (void)st;
    log::warn("[media] C++/WinRT headers not available -- Now Playing metadata source disabled");
#endif
}

} // namespace fh6r
