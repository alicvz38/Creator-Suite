// Tüm modüllerin paylaştığı küçük yardımcılar.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Hata kaydı ve çökme koruması (common.cpp). Kayıt: data_dir()\error-log.txt; çökme dökümü: data_dir()\Crash\*.dmp
void log_error(const std::string& text);        // Aynı metin 10 saniyede bir kez yazılır
void install_crash_guard();                     // wWinMain başında: yakalanmayan hata ve abort öncesi döküm + kayıt
// json'ın iç denetimi başarısız olursa "Assertion failed" kutusu çıkıp program kapanmasın: kaydedilir, istisna atılır
[[noreturn]] void json_check_failed(const char* expr, const char* file, int line);
#define JSON_ASSERT(x) ((x) ? (void)0 : json_check_failed(#x, __FILE__, __LINE__))

#include "json.hpp"

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

// İş parçacığı ve tur gövdeleri için: hata programı kapatmaz, kaydedilir
template <typename F>
void guarded(const char* where, F&& body) {
    try {
        body();
    } catch (const std::exception& e) {
        log_error(std::string(where) + ": " + e.what());
    } catch (...) {
        log_error(std::string(where) + ": bilinmeyen hata");
    }
}

std::wstring widen(const std::string& text);
std::string narrow(const std::wstring& text);
double now();                   // Saniye (yüksek çözünürlüklü saat)
std::wstring exe_dir();         // Exe'nin bulunduğu klasör
std::wstring data_dir();        // Ayar dosyalarının klasörü (kuruluma göre, bkz. ortak.cpp)
std::wstring videos_dir();      // Kullanıcının Videolar klasörü
json read_json(const std::wstring& path);       // Dosya yoksa null
const char* build_version();    // Sürüm = derleme anı, yyyy.aa.gg.ss.dd.ss (elle sürüm numarası tutulmaz)
void write_json(const std::wstring& path, const json& data);    // Yarım dosya kalmasın diye geçici dosyadan

// Önceden çarpılmış BGRA görüntü (ekran kartına ve katmanlı pencereye doğrudan gider)
struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool empty() const { return w == 0 || h == 0; }
};

// gorsel.cpp: resim yükleme ve Direct2D ile metin / crosshair / sosyal medya çizimi
bool load_image(const std::wstring& path, Bitmap& out);
bool save_png(const Bitmap& b, const std::wstring& path);
Bitmap draw_text(const json& s, float k);
Bitmap draw_shape(const json& s, float k);      // Dikdörtgen, elips, ok, çizgi, balon, rozet
Bitmap draw_crosshair(const json& s);
// Tuş göstergesi: konum ve boyut tuş birimi cinsinden; basılı tuş vurgulu
struct KeyCap { float x, y, w, h; std::string label; bool down; };
Bitmap draw_keys(const std::vector<KeyCap>& caps, float unit, uint32_t text, uint32_t active, float bg_alpha);
Bitmap draw_social(const json& s, float k);
Bitmap make_icon(const std::string& slug, int size, bool brand, uint32_t rgb);
uint32_t parse_color(const std::string& hex);   // "#rrggbb" -> 0xRRGGBB

// Açılış ekranı (acilis.cpp): program hazırlanırken ayrı iş parçacığında gösterilir
struct SplashInfo { std::wstring glyph; std::string name, tagline, version, note; uint32_t accent = 0x0d6efd; };
Bitmap draw_splash(const SplashInfo& info, float k, float progress, const std::string& status);
void splash_start(const SplashInfo& info);      // Ayarlarda kapalıysa bir şey yapmaz
void splash_finish(double minimum_seconds);     // En az bu kadar gösterilir, solarak kapanır; ana pencere bundan sonra açılmalı

// FPS göstergesinin satırları. Parça türü: 'l' başlık, 'v' değer, 'u' birim
struct FpsPart { std::string text; char kind; uint32_t color; };
struct FpsCell { std::vector<FpsPart> parts; std::string tmpl; };
struct FpsRow { std::string label; std::vector<std::pair<int, FpsCell>> cells; bool align; };
// graph: kare süreleri (ms), s["show_graph"] açıksa satırların altına çubuk grafik olarak çizilir
Bitmap draw_fps(const json& s, const std::vector<FpsRow>& rows, float k, const std::vector<float>& graph = {});

struct Platform { const char* key; const char* name; uint32_t color; };
extern const Platform kPlatforms[8];

// Ayarların varsayılanları (ana.cpp)
json default_social();
