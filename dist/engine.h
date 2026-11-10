// Kayıt motoru: kendi iş parçacığında sahneyi ekran kartında çizer, önizlemeye ve kodlayıcıya verir.
// Arayüzden bağımsız çalışır: pencere sürüklense ya da küçültülse kayıt etkilenmez.
#pragma once
#include "common.h"

struct Rect { float x = 0, y = 0, w = 0, h = 0; };   // Tuval pikseli

struct Status {
    bool recording = false, paused = false;
    double elapsed = 0;
    long long frames = 0, repeated = 0;
    double frame_ms = 0;
    float level_desktop = 0, level_mic = 0, level_mic_input = 0;   // input: mikrofon filtrelerden önce (kazançtan sonra)
    std::wstring path, last_path;
    std::string error;
    bool camera_failed = false;
    bool game_fallback = false;     // Oyun yakalama ekran yakalamaya düştü
    // Yayın
    bool streaming = false;
    int stream_state = 0;       // 1 bağlanıyor, 2 yayında, 3 yeniden bağlanıyor
    double stream_elapsed = 0, stream_kbps = 0;
    long long stream_dropped = 0;
    std::string stream_error;
    // Ekran görüntüsü
    long long shots = 0;            // Her kaydedilen görüntüde artar
    std::wstring shot_path;
    std::string shot_error;
    // Ek ses girişlerinin düzeyi (rec["audio_inputs"] sırasıyla)
    float level_inputs[8] = {};
    // Tekrar arabelleği
    bool replay_on = false;
    int replay_seconds = 0;         // Şu an bellekte tutulan süre
    long long replay_saves = 0;     // Her kaydedişte artar
    std::wstring replay_path;
    std::string replay_error;
    // Ek yayın hedefleri: ad ve durum ("Yayında", "Bağlanıyor", hata metni)
    struct Target { std::string name; int state = 0; double kbps = 0; std::string error; };
    std::vector<Target> stream_targets;
    // Sanal kamera: 0 kapalı, 1 açık (henüz bir uygulama kullanmıyor), 2 bir uygulamaya görüntü gidiyor
    int vcam_state = 0;
    std::string vcam_error;
    // Kayıt bölme ve istatistik
    int record_part = 0;            // 0: bölünmüyor ya da ilk parça
    double record_kbps = 0, render_fps = 0;
    long long record_bytes = 0;
};

std::vector<std::string> list_cameras();

class Engine {
public:
    bool start(std::string& error);
    void stop();

    void set_document(const json& doc);     // scenes, current, social
    void set_record_settings(const json& s);
    void set_active(bool active) { active_ = active; }      // Önizleme gerekiyor mu

    void request_record(bool on) { record_request_ = on ? 1 : 2; }
    void toggle_pause() { pause_request_ = true; }
    void request_stream(bool on) { stream_request_ = on ? 1 : 2; }
    void request_screenshot() { shot_request_ = true; }      // Tuvalin o anki karesi PNG olarak kayıt klasörüne
    void set_push_to_talk(bool held) { ptt_held_ = held; }   // Bas-konuş tuşu basılı mı
    void request_replay(bool on) { replay_request_ = on ? 1 : 2; }     // Tekrar arabelleğini aç / kapat
    void request_replay_save() { replay_save_request_ = true; }         // Son N saniyeyi dosyaya yaz
    HANDLE program_handle();                // Stüdyo modunda yayındaki sahnenin önizlemesi
    void request_virtual_camera(bool on) { vcam_request_ = on ? 1 : 2; }

    Status status();
    std::map<std::string, Rect> boxes();
    void canvas_size(int& w, int& h);
    HANDLE preview_handle();                // Tuval değişince değişir
    IDXGIAdapter1* adapter() { return adapter_.Get(); }
    std::vector<std::string> monitors();
    std::vector<RECT> monitor_rects() { return monitor_rects_; }    // Masaüstü koordinatı, monitors() ile aynı sıra

    static json default_record();
    struct Impl;

private:
    void run();
    bool create_device(std::string& error);

    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D11Device> device_;
    std::shared_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> halt_{false}, active_{true}, pause_request_{false}, shot_request_{false}, ptt_held_{false};
    std::thread shot_thread_;
    std::vector<RECT> monitor_rects_;
    std::atomic<int> record_request_{0}, stream_request_{0}, replay_request_{0}, vcam_request_{0};
    std::atomic<bool> replay_save_request_{false};
    HANDLE program_preview_ = nullptr;
    std::mutex lock_;
    json doc_, record_;
    bool doc_dirty_ = false, record_dirty_ = false;
    Status status_;
    std::map<std::string, Rect> boxes_;
    HANDLE preview_ = nullptr;
    int width_ = 1920, height_ = 1080;
    std::vector<std::string> monitors_;
};
