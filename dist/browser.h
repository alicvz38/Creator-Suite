// Tarayıcı kaynağı: web sayfası (bağış / takipçi uyarıları, sohbet, sayaç...) görünmeyen bir yüzeye çizilir ve sahneye
// saydamlığıyla girer. Sayfa Microsoft Edge WebView2 ile açılır; ayrı tarayıcı gerekmez.
#pragma once
#include "common.h"

struct BrowserOptions {
    std::string url;            // https://... ya da yerel dosya yolu
    std::string css;            // Sayfaya eklenecek stil (arka planı saydam yapmak gibi)
    int width = 1280, height = 720, fps = 30;
    bool muted = false;
};

class BrowserCapture {
public:
    BrowserCapture(IDXGIAdapter1* adapter, const BrowserOptions& options);
    ~BrowserCapture();
    HANDLE handle();                    // Son karenin paylaşımlı dokusu (önceden çarpılmış BGRA, saydam)
    bool take_fresh() { return fresh_.exchange(false); }
    void mark_fresh() { fresh_ = true; }
    std::string error();
    void reload() { reload_ = true; }

private:
    void loop();
    ComPtr<IDXGIAdapter1> adapter_;
    BrowserOptions options_;
    std::thread thread_;
    std::atomic<bool> halt_{false}, fresh_{false}, reload_{false};
    std::mutex lock_;
    HANDLE handle_ = nullptr;
    std::string error_;
};

bool browser_available();               // WebView2 çalışma ortamı kurulu mu

// Sohbet ve bildirim pencereleri: sayfa kendi penceresinde açılır, yalnızca kullanıcı içindir (sahneye girmez).
// hidden: ekran yakalamadan hariç tutulur (kayıtta ve yayında görünmez). Giriş bilgileri pencereler arasında ortaktır.
class BrowserPanel {
public:
    BrowserPanel(const std::wstring& title, const std::string& url, RECT rect, bool topmost, bool hidden);
    ~BrowserPanel();
    bool closed() const { return closed_; }     // Kullanıcı pencereyi kapattı
    RECT rect();                                // Son konum ve boyut (ekran pikseli)
    void set_flags(bool topmost, bool hidden);
    void navigate(const std::string& url);
    void show();                                // Öne getirir
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

private:
    void loop();
    void apply_flags();
    std::wstring title_;
    std::string url_;
    RECT rect_;
    std::atomic<bool> halt_{false}, closed_{false}, topmost_{false}, hidden_{true}, flags_changed_{false}, show_{false};
    std::mutex lock_;
    std::string pending_url_, error_;
    HWND hwnd_ = nullptr;
    void* controller_ = nullptr;                // ICoreWebView2Controller (yalnızca pencerenin iş parçacığında kullanılır)
    std::thread thread_;
};
