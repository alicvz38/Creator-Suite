// Çalan şarkı (bkz. now_playing.h). WinRT başlıkları burada kalır: arayüz dosyalarındaki ImGui kısaltmalarıyla karışmasın.
#include "now_playing.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

using namespace winrt::Windows::Media::Control;

namespace {
std::string utf8(const winrt::hstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
}  // namespace

struct MediaWatcher::Impl {
    std::thread thread;
    std::atomic<bool> halt{false};
    std::mutex lock;
    NowPlaying value;

    void run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        {
            GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
            while (!halt) {
                NowPlaying now;
                try {
                    if (!manager) manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                    if (auto session = manager.GetCurrentSession()) {
                        now.playing = session.GetPlaybackInfo().PlaybackStatus() ==
                                      GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                        auto props = session.TryGetMediaPropertiesAsync().get();
                        now.title = utf8(props.Title());
                        now.artist = utf8(props.Artist());
                    }
                } catch (...) {
                    manager = nullptr;      // Bir sonraki turda yeniden bağlanır
                }
                {
                    std::lock_guard<std::mutex> g(lock);
                    value = now;
                }
                for (int i = 0; i < 15 && !halt; i++) Sleep(100);
            }
        }
        winrt::uninit_apartment();
    }
};

MediaWatcher::MediaWatcher() = default;
MediaWatcher::~MediaWatcher() { stop(); }

void MediaWatcher::start() {
    if (impl_) return;
    impl_ = std::make_unique<Impl>();
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
}

void MediaWatcher::stop() {
    if (!impl_) return;
    impl_->halt = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_.reset();
}

NowPlaying MediaWatcher::get() {
    if (!impl_) return {};
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->value;
}
