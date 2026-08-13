#pragma once
#include <mutex>
#include <string>
#include <thread>

namespace fh6r {

// Reads the Windows "media control center" (SMTC) title/artist — the source QQ
// Music (and other players) publish to — so the injected radio can show the real
// now-playing text. Runs on its own thread with a WinRT MTA apartment.
//
// Degrades to a no-op (info() returns false) when the C++/WinRT headers are not
// available at build time, so it never breaks the build. Read-only with respect
// to the game.
class MediaSessionProvider {
public:
    MediaSessionProvider();
    ~MediaSessionProvider();
    MediaSessionProvider(const MediaSessionProvider&) = delete;
    MediaSessionProvider& operator=(const MediaSessionProvider&) = delete;

    struct Info {
        std::string title;
        std::string artist;
    };

    // Latest snapshot; false until the first successful read.
    bool info(Info& out) const;

private:
    void run(std::stop_token st);

    std::jthread thread_;
    mutable std::mutex mu_;
    Info info_;
    bool has_info_ = false;
};

} // namespace fh6r
