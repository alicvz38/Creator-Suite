// FPS Ölçer: ekrana gelen kareleri sayar, sensörleri okur, gösterge satırlarını hazırlar.
#pragma once
#include "common.h"

#include <deque>

// Kare süresi örneği: Windows her ekran yenilemesinde, o arada çizilen kare sayısını verir.
// Kare başına süre = iki yenileme arasındaki süre / kare sayısı (yenileme hızından yüksek FPS'de ortalama olur).
struct FrameSample { double t, ms; int frames; };
struct FrameStats { double avg = -1, low1 = -1, low01 = -1, min = -1, max = -1; };
FrameStats frame_stats(const std::vector<FrameSample>& samples);    // %1 / %0,1 en düşük: en yavaş karelerin FPS'i

class FpsMeter {
public:
    void start();
    void stop();
    bool running() const { return thread_.joinable(); }
    double read();      // Son okumadan bu yana saniyedeki kare, okunamıyorsa -1
    std::vector<FrameSample> recent(double seconds);    // Son N saniyenin kare süreleri (en çok 120 sn tutulur)
    void session_begin();                               // Performans testi: bütün kareler toplanır
    std::vector<FrameSample> session_end();

private:
    void run();
    void add(double t, double ms, int frames);
    std::thread thread_;
    std::atomic<bool> halt_{false}, ok_{false};
    std::atomic<long long> frames_{0};
    long long last_frames_ = 0;
    double last_time_ = 0;
    std::mutex lock_;
    std::deque<FrameSample> recent_;
    std::vector<FrameSample> session_;
    bool session_on_ = false;
};

// Ping: seçilen sunucuya saniyede bir ICMP yankısı. Son 20 denemenin kaybı ve son gecikme.
class Pinger {
public:
    ~Pinger() { stop(); }
    void start(const std::string& host);    // Aynı sunucuysa bir şey yapmaz
    void stop();
    bool running() const { return thread_.joinable(); }
    json data();                            // {"ms": son gecikme, "loss": %, "host": ...}; ulaşılamıyorsa ms yok

private:
    void run(std::string host);
    std::thread thread_;
    std::atomic<bool> halt_{false};
    std::mutex lock_;
    std::string host_;
    std::deque<int> results_;               // -1 kayıp
    std::string error_;
};

// lib\sensor.exe yardımcısını yönetir; açılamazsa Windows'tan kullanım değerlerini okur
class Sensors {
public:
    ~Sensors() { stop(); }
    void start();
    void stop();
    bool running() const { return thread_.joinable(); }
    void configure(const std::string& wanted, int interval_ms);     // "cpu gpu ram ..."
    json data();
    std::string status();       // loading | ok | fallback
    std::string error();

private:
    void read_helper();
    void fallback();
    void send(const std::string& line);

    std::thread thread_;
    std::atomic<bool> halt_{false};
    std::atomic<int> interval_{1000};
    HANDLE process_ = nullptr, stdin_ = nullptr, stdout_ = nullptr;
    std::mutex lock_;
    json data_ = json::object();
    std::string status_ = "loading", error_, sent_;
};

json default_fps();
json merge_known(const json& defaults, const json& data);    // Bilinmeyen ve yanlış tipteki değerleri atar
std::string fps_wanted(const json& s);                       // Okunması gereken donanımlar
json fps_preview_data(const json& live, const std::string& gpu);
const json* fps_pick_gpu(const json& data, const std::string& name);
bool fps_missing(const json& live, const std::string& gpu, const std::string& key);   // Canlı değer okunamıyor mu
std::vector<FpsRow> fps_rows(const json& s, double fps, int hz, const json& data);
std::string fps_signature(const std::vector<FpsRow>& rows);
void fps_place(const json& s, int w, int h, int area_w, int area_h, float k, int& x, int& y);
int refresh_rate();
bool is_admin();
bool pawnio_installed();
