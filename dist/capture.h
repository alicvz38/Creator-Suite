// Oyun ve pencere yakalama (Windows Graphics Capture). Oyuna hiçbir şey enjekte edilmez, oyun belleğine dokunulmaz:
// bu yüzden hile koruması olan oyunlarda da güvenlidir.
#pragma once
#include "common.h"

// FPS göstergesi için: yakalanan pencereden gelen kare sayısı
inline std::atomic<long long> g_capture_frames{0};
inline std::atomic<bool> g_capture_active{false};

struct GameWindow { std::string exe, title; };
std::vector<GameWindow> list_game_windows();

class WindowCapture {
public:
    // exe boşsa tam ekran çalışan uygulama kendiliğinden bulunur, doluysa o programın penceresi yakalanır
    // title doluysa o başlıklı pencere tercih edilir. client: başlık çubuğu ve kenarlar alınmaz. game: FPS göstergesi bu yakalamadan sayar
    WindowCapture(IDXGIAdapter1* adapter, const std::string& exe, bool cursor, const std::string& title = {}, bool client = false, bool game = true);
    ~WindowCapture();

    HANDLE handle();                        // Son karenin paylaşımlı dokusu
    bool take_fresh() { return fresh_.exchange(false); }
    void mark_fresh() { fresh_ = true; }
    bool has_target() const { return target_found_; }
    bool stalled() const { return stalled_; }   // Pencere var ama görüntü gelmiyor (özel tam ekran)

private:
    void loop();
    ComPtr<IDXGIAdapter1> adapter_;
    std::string exe_, title_;
    bool cursor_, client_, game_;
    std::thread thread_;
    std::atomic<bool> halt_{false}, fresh_{false}, target_found_{false}, stalled_{false};
    std::mutex lock_;
    HANDLE handle_ = nullptr;
};
