// Oyun Araçları: ekrandaki crosshair ve FPS göstergesi. Çarpıyla kapatılınca saat yanındaki simgelere iner,
// göstergeler çalışmaya devam eder. Kayıt Stüdyosu'ndan bağımsız ayrı bir programdır.
#include "common.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // Hata kurtarma; ui.h'deki ImGui kısaltmalarından önce gelmeli
#include "ui.h"
#include "fps_meter.h"
#include "now_playing.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <powrprof.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>

#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

void apply_gamma(int brightness, int contrast, int gamma);      // Ekran rengi (aşağıda)

const wchar_t* kWindowClass = L"OyunAraclari";
const wchar_t* kTaskName = L"Game Tools";
const wchar_t* kMutexName = L"Local\\OyunAraclari";
const char* kAppName = "Oyun Araçları";
const char* kVersion = build_version();      // Derleme anı: yyyy.aa.gg.ss.dd.ss
const char* kCodePrefix = "CH2-";
const float kFpsPreviewW = 460, kFpsPreviewH = 300;
const UINT WM_HOTKEY_ACTION = WM_APP + 1, WM_TRAY = WM_APP + 2;
enum { MENU_OPEN = 1, MENU_CROSSHAIR, MENU_FPS, MENU_EXIT };
// Windows'un Segoe MDL2 simge yazı tipindeki karakterler
#define ICON_CROSSHAIR "\xEE\x9C\x90"
#define ICON_FPS "\xEE\xB1\x8A"
#define ICON_KEYS "\xEE\x9D\xA5"
#define ICON_SETTINGS "\xEE\x9C\x93"
#define ICON_PEOPLE "\xEE\x9C\x96"
#define ICON_EXIT "\xEE\x9F\xA8"
#define ICON_BENCH "\xEE\xA7\x99"
#define ICON_GAMES "\xEE\x9F\xBC"
#define ICON_AIM "\xEE\xA5\xA2"
#define ICON_EXTRAS "\xEE\x9C\x94"

struct Action { const char* key; const char* name; Hotkey fallback; };
const Action kActions[] = {
    {"crosshair", "Crosshair göster / gizle", {VK_F9}}, {"crosshair_off", "Crosshair'i kapat", {VK_F10}},
    {"fps", "FPS Ölçer göster / gizle", {VK_F7}}, {"fps_off", "FPS Ölçer'i kapat", {VK_F8}},
    {"bench", "Performans testini başlat / durdur", {VK_F6}},
    {"keys", "Tuş göstergesi göster / gizle", {}}, {"replay_save", "Tekrarı kaydet (Kayıt Stüdyosu)", {}},
};

const std::pair<const char*, const char*> kArms[] = {{"arm_up", "Üst"}, {"arm_down", "Alt"}, {"arm_left", "Sol"}, {"arm_right", "Sağ"}};

json default_crosshair() {
    json d = {{"length", 10}, {"gap", 4}, {"thickness", 2}, {"t_shape", false}, {"outer", false}, {"outer_length", 4},
              {"outer_gap", 18}, {"outer_thickness", 2}, {"dot", true}, {"dot_size", 2}, {"dot_shape", "square"},
              {"ring", false}, {"ring_radius", 12}, {"ring_thickness", 1}, {"outline", false}, {"outline_thickness", 1},
              {"outline_color", "#000000"}, {"color", "#00ff00"}, {"opacity", 100}, {"rotate", 0}, {"outer_rotate", 0},
              {"arms_separate", false}, {"arms_ready", false},
              // Faz C: resimden crosshair, zemine göre renk
              {"image", ""}, {"image_scale", 100}, {"adaptive", false}, {"adaptive_color", "#ff00ff"}};
    for (auto& [key, name] : kArms) {
        const std::string k = key;
        d[k + "_length"] = 10;
        d[k + "_thickness"] = 2;
        d[k + "_gap"] = 4;
        d[k + "_angle"] = 0;
        d[k + "_spin"] = 0;
        d["outer_" + k + "_angle"] = 0;
        d["outer_" + k + "_spin"] = 0;
    }
    return d;
}

json default_options() {
    return {{"crosshair_autostart", false}, {"fps_autostart", false}, {"crosshair_hide", false}, {"fps_hide", false},
            {"note_shown", false}, {"theme_light", false}, {"theme_mode", -1}, {"theme_style", 0}, {"bundled_profiles", ""},
            // Faz B: oyuna özel profiller, oyun modu, oyun süresi, mola
            {"games", json::array()}, {"gm_power", true}, {"gm_close", json::array()}, {"gm_reopen", true},
            {"playtime_fullscreen", true}, {"break_on", false}, {"break_minutes", 60}, {"gamma_applied", false},
            // Faz C: crosshair konumu, hassasiyet çevirici, nişan antrenmanı
            {"crosshair_center", 1}, {"crosshair_offset_x", 0}, {"crosshair_offset_y", 0},
            {"sens_from", "cs2"}, {"sens_to", "valorant"}, {"sens_value", 1.0}, {"sens_dpi", 800}, {"sens_new_dpi", 1600},
            {"sens_yaw_from", 0.022}, {"sens_yaw_to", 0.022}, {"dpi_distance", 10.0},
            {"aim_mode", "click"}, {"aim_seconds", 30}, {"aim_size", 26}, {"aim_targets", 3}, {"aim_color", "#ff4d4d"},
            // Faz D: tuş göstergesi, çalan şarkı, Kayıt Stüdyosu bağlantısı
            {"keys_on", false}, {"keys_layout", "wasd"}, {"keys_mods", true}, {"keys_mouse", true}, {"keys_extra", "Q E R F"},
            {"keys_corner", "bottom_left"}, {"keys_size", 44}, {"keys_color", "#ffffff"}, {"keys_active", "#2fbf71"}, {"keys_bg", 55},
            {"keys_hide", false},
            {"song_on", false}, {"song_corner", "top_left"}, {"song_size", 20}, {"song_color", "#ffffff"}, {"song_bg", true},
            {"song_paused", false}, {"song_hide", false},
            {"rs_badge", true}, {"rs_badge_corner", "top_right"}, {"rs_badge_hide", true}, {"rs_replay_auto", false}, {"rs_launch", true}};
}

json preset_profiles() {
    const json d = default_crosshair();
    auto with = [&](json changes) { json s = d; s.merge_patch(changes); return s; };
    return {{"Klasik", d},
            {"Sadece Nokta", with({{"length", 0}, {"dot_size", 4}, {"dot_shape", "round"}, {"outline", true}})},
            {"Halka", with({{"length", 0}, {"dot_shape", "round"}, {"ring", true}, {"ring_radius", 10}})},
            {"T Şekli", with({{"length", 8}, {"gap", 3}, {"t_shape", true}, {"dot", false}, {"color", "#00ffff"}, {"outline", true}})},
            {"Dış Çizgili", with({{"length", 6}, {"gap", 3}, {"dot", false}, {"outer", true}, {"outer_length", 4},
                                  {"outer_gap", 14}, {"color", "#ffff00"}, {"outline", true}})}};
}

// --- Paylaşım kodu: sadece varsayılandan farklı ayarları taşır, kısa kalır ---

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    int bits = 0;
    unsigned value = 0;
    for (unsigned char c : in) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 6) { bits -= 6; out += kBase64[(value >> bits) & 63]; }
    }
    if (bits) out += kBase64[(value << (6 - bits)) & 63];
    return out;
}

std::string base64_decode(const std::string& in) {
    std::string out;
    int bits = 0;
    unsigned value = 0;
    for (char c : in) {
        const char* p = strchr(kBase64, c);
        if (!c || !p) continue;
        value = (value << 6) | unsigned(p - kBase64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out += char((value >> bits) & 255); }
    }
    return out;
}

std::string encode_code(const json& s) {
    const json d = default_crosshair();
    json diff = json::object();
    for (auto& [key, value] : s.items())
        if (!d.contains(key) || d[key] != value) diff[key] = value;
    return kCodePrefix + base64_encode(diff.dump());
}

// --- Başka oyunların crosshair kodları (yaklaşık karşılık; oyunların çizimi birebir aynı değildir) ---

std::string hex_color(int r, int g, int b) {
    char text[8];
    sprintf_s(text, "#%02x%02x%02x", std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
    return text;
}

// CS paylaşım kodu: 57 harfli alfabe, karakterler sondan başa okunan büyük bir sayı, büyük uçlu bayt dizisi
bool cs_bytes(const std::string& chars, size_t count, std::vector<uint8_t>& out) {
    static const char kDict[] = "ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789";
    std::vector<uint32_t> big(count + 2, 0);       // Büyük uçlu, bayt başına bir hane
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
        const char* p = strchr(kDict, *it);
        if (!*it || !p) return false;
        uint32_t carry = uint32_t(p - kDict);
        for (size_t i = big.size(); i-- > 0;) {
            const uint32_t v = big[i] * 57 + carry;
            big[i] = v & 255;
            carry = v >> 8;
        }
        if (carry) return false;
    }
    if (big[0] || big[1]) return false;
    out.assign(big.begin() + 2, big.end());
    uint32_t sum = 0;
    for (size_t i = 1; i < out.size(); i++) sum += out[i];
    return out[0] == (sum & 255);
}

const char* kCsColors[] = {"#ff0000", "#00ff00", "#ffff00", "#0000ff", "#00ffff"};

bool decode_cs(std::string code, json& s, int screen_h) {
    std::string chars;
    for (char c : code) if (c != '-') chars += c;
    std::vector<uint8_t> b;
    const double unit = screen_h / 480.0;     // CS çizgi ölçüleri 480 satırlık ekrana göredir
    if (chars.rfind("CSGO", 0) == 0 && chars.size() == 29) {
        if (!cs_bytes(chars.substr(4), 18, b) || b[1] != 1) return false;
        const double gap = int8_t(b[2]) / 10.0, outline = b[3] / 2.0, thickness = b[12] / 10.0, length = b[14] / 10.0;
        const int color = b[10] & 7;
        s["color"] = color < 5 ? std::string(kCsColors[color]) : hex_color(b[4], b[5], b[6]);
        s["opacity"] = (b[13] & 0x40) ? std::clamp(int(lround(b[7] / 2.55)), 10, 100) : 100;
        s["outline"] = (b[10] & 8) != 0;
        s["outline_thickness"] = std::clamp(int(lround(outline)), 1, 3);
        s["outline_color"] = "#000000";
        s["dot"] = (b[13] & 0x10) != 0;
        s["t_shape"] = (b[13] & 0x80) != 0;
        s["thickness"] = std::clamp(int(lround(thickness * unit)), 1, 8);
        s["length"] = std::clamp(int(lround(length * unit)), 0, 30);
        s["gap"] = std::clamp(int(lround((gap + 4.5) * unit / 2)), 0, 20);
    } else if (chars.rfind("CS", 0) == 0 && chars.size() == 46) {
        // CS2'nin yeni kodu: ölçüler, kodda yazan ekran yüksekliğindeki piksel cinsinden
        if (!cs_bytes(chars.substr(2), 32, b) || b[1] != 1) return false;
        const int code_h = std::max(240, b[2] | (b[3] << 8));
        const double k = double(screen_h) / code_h;
        const int gap = int16_t(b[14] | (b[15] << 8)), mode = std::min(b[13] >> 6, 2);
        s["color"] = hex_color(b[5], b[6], b[7]);
        s["opacity"] = std::clamp(int(lround(b[8] / 2.55)), 10, 100);
        s["outline"] = mode != 0;
        s["outline_thickness"] = 1;
        s["outline_color"] = hex_color(b[9], b[10], b[11]);
        s["dot"] = (b[4] & 0x40) != 0;
        s["t_shape"] = (b[4] & 0x80) != 0;
        s["thickness"] = std::clamp(int(lround(std::min(b[13] & 0x3f, 32) * k)), 1, 8);
        s["length"] = std::clamp(int(lround(b[16] * k)), 0, 30);
        s["gap"] = std::clamp(int(lround(gap * k)), 0, 20);
    } else {
        return false;
    }
    s["dot_size"] = s["thickness"];
    s["dot_shape"] = "square";
    s["outer"] = s["ring"] = false;
    return true;
}

// Valorant profil kodu: "0;P;c;5;h;0;0l;4;..." (P birincil crosshair; A nişan alma ve S dürbün bölümleri atlanır)
bool decode_valorant(const std::string& code, json& s, int screen_h) {
    std::vector<std::string> parts;
    size_t from = 0;
    for (size_t at; (at = code.find(';', from)) != std::string::npos; from = at + 1) parts.push_back(code.substr(from, at - from));
    parts.push_back(code.substr(from));
    if (parts.empty() || parts[0] != "0") return false;
    std::map<std::string, std::string> v;
    bool primary = true;
    for (size_t i = 1; i < parts.size(); i++) {
        const std::string& key = parts[i];
        if (key == "P") { primary = true; continue; }
        if (key == "A" || key == "S") { primary = false; continue; }
        if (i + 1 >= parts.size()) return false;
        if (primary) v[key] = parts[i + 1];
        i++;
    }
    auto num = [&](const char* key, double fallback) { auto it = v.find(key); return it == v.end() ? fallback : atof(it->second.c_str()); };
    static const char* kColors[] = {"#ffffff", "#00ff00", "#7fff00", "#dfff00", "#ffff00", "#00ffff", "#ff00ff", "#ff0000"};
    const int color = int(num("c", 0));
    std::string hex = color >= 0 && color < 8 ? kColors[color] : "#ffffff";
    if (color == 8 && v.count("u") && v["u"].size() >= 6) hex = "#" + v["u"].substr(0, 6);
    for (auto& ch : hex) ch = char(tolower(ch));
    const double k = screen_h / 1080.0;     // Valorant ölçüleri 1080p'de piksele denk
    auto px = [&](double value, int lo, int hi) { return std::clamp(int(lround(value * k)), lo, hi); };
    s["color"] = hex;
    s["outline"] = num("h", 1) != 0;
    s["outline_thickness"] = std::clamp(int(num("t", 1)), 1, 3);
    s["outline_color"] = "#000000";
    s["dot"] = num("d", 0) != 0;
    s["dot_size"] = px(num("z", 2), 1, 12);
    s["dot_shape"] = "square";
    const bool inner = num("0b", 1) != 0;
    s["length"] = inner ? px(num("0l", 6), 0, 30) : 0;
    s["thickness"] = px(num("0t", 2), 1, 8);
    s["gap"] = px(num("0o", 3), 0, 20);
    s["opacity"] = std::clamp(int(lround(num("0a", 0.8) * 100)), 10, 100);
    s["outer"] = num("1b", 1) != 0 && num("1l", 2) > 0;
    s["outer_length"] = px(num("1l", 2), 0, 30);
    s["outer_thickness"] = px(num("1t", 2), 1, 8);
    s["outer_gap"] = px(num("1o", 10), 0, 40);
    s["t_shape"] = s["ring"] = false;
    return true;
}

// source: bulunan kodun türü ("" kendi kodumuz, "CS2", "Valorant")
bool decode_code(std::string code, json& out, std::string* source = nullptr) {
    const size_t start = code.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return false;
    code = code.substr(start);
    code = code.substr(0, code.find_last_not_of(" \t\r\n") + 1);
    const int screen_h = GetSystemMetrics(SM_CYSCREEN);
    json s = default_crosshair();
    if (code.rfind("CS", 0) == 0 && code.rfind(kCodePrefix, 0) != 0) {
        if (!decode_cs(code, s, screen_h)) return false;
        if (source) *source = "CS2";
        out = s;
        return true;
    }
    if (code.rfind("0;", 0) == 0 || code == "0") {
        if (!decode_valorant(code, s, screen_h)) return false;
        if (source) *source = "Valorant";
        out = s;
        return true;
    }
    if (code.rfind(kCodePrefix, 0) != 0) return false;
    const json data = json::parse(base64_decode(code.substr(strlen(kCodePrefix))), nullptr, false);
    if (!data.is_object()) return false;
    out = merge_known(default_crosshair(), data);
    if (source) source->clear();
    return true;
}

// --- Otomatik başlatma: Görev Zamanlayıcı ile yönetici olarak, UAC sormadan açılır ---
// (Sıcaklık sensörleri yönetici yetkisi ister; Başlangıç klasöründen bu yetkiyle açılamaz.)

int run_hidden(std::wstring command) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
}

bool task_exists() {
    return run_hidden(std::wstring(L"schtasks /Query /TN \"") + kTaskName + L"\"") == 0;
}

std::wstring xml_escape(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (c == L'&') out += L"&amp;";
        else if (c == L'<') out += L"&lt;";
        else if (c == L'>') out += L"&gt;";
        else out += c;
    }
    return out;
}

std::wstring task_xml() {
    wchar_t path[MAX_PATH], domain[256] = L"", user[256] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    const std::wstring who = xml_escape(std::wstring(domain) + L"\\" + user);
    return L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           L"  <RegistrationInfo><Description>Oyun Araçları göstergelerini oturum açılınca başlatır</Description></RegistrationInfo>\n"
           L"  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + who + L"</UserId><Delay>PT10S</Delay></LogonTrigger></Triggers>\n"
           L"  <Principals><Principal id=\"Author\"><UserId>" + who + L"</UserId><LogonType>InteractiveToken</LogonType>"
           L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
           L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
           L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
           L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\n"
           L"  <Actions Context=\"Author\"><Exec><Command>" + xml_escape(path) + L"</Command><Arguments>--baslangic</Arguments>"
           L"<WorkingDirectory>" + xml_escape(exe_dir()) + L"</WorkingDirectory></Exec></Actions>\n"
           L"</Task>\n";
}

// Yönetici olarak çalışan kopya bunu yapar
bool write_task(bool enable) {
    if (!enable) return run_hidden(std::wstring(L"schtasks /Delete /TN \"") + kTaskName + L"\" /F") == 0;
    wchar_t folder[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    const std::wstring path = std::wstring(folder) + L"OyunAraclariGorev.xml";
    {
        const std::wstring xml = wchar_t(0xFEFF) + task_xml();
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(xml.data()), xml.size() * sizeof(wchar_t));
    }
    const int code = run_hidden(std::wstring(L"schtasks /Create /TN \"") + kTaskName + L"\" /XML \"" + path + L"\" /F");
    DeleteFileW(path.c_str());
    return code == 0;
}

// Yetki yoksa kendini bir kez yönetici olarak açtırır: kullanıcı UAC onayı verir, görev kurulur
bool set_task(HWND owner, bool enable) {
    if (is_admin()) return write_task(enable);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.lpParameters = enable ? L"--gorev-kur" : L"--gorev-sil";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return false;
    WaitForSingleObject(info.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0;
}

// --- Ekrandaki gösterge (tıklamalar içinden geçen, hep önde katmanlı pencere) ---

class Overlay {
public:
    void show(const Bitmap& b, float opacity, int x, int y) {
        if (!hwnd_) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"OyunAraclariGosterge";
            RegisterClassW(&wc);
            hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                    wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), b.w, -b.h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, dib);
        memcpy(bits, b.px.data(), b.px.size());
        POINT pos{x, y}, origin{};
        SIZE size{b.w, b.h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(opacity * 255), AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd_, screen, &pos, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
        DeleteObject(dib);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
        keep_on_top();
        apply_capture();
    }
    void hide() {
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
    bool visible() const { return visible_; }
    void move(int x, int y) {
        if (hwnd_ && visible_) SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
    void keep_on_top() {
        if (visible_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // Ekranda görünür ama kayıt ve yayın programlarının yakaladığı görüntüye girmez
    void hide_from_capture(bool hide) {
        want_hidden_ = hide;
        apply_capture();
    }

private:
    void apply_capture() {
        if (hwnd_ && want_hidden_ != hidden_) {
            SetWindowDisplayAffinity(hwnd_, want_hidden_ ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
            hidden_ = want_hidden_;
        }
    }
    HWND hwnd_ = nullptr;
    bool visible_ = false, hidden_ = false, want_hidden_ = false;
};

// --- Uygulama ---

class App {
public:
    int run(bool startup);

    void on_resize(UINT w, UINT h) {
        if (swap_ && w && h) { rtv_.Reset(); swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0); create_rtv(); }
    }
    void on_hotkey(int action);
    void show_window();
    void hide_to_tray();
    void add_tray_icon();
    void tray_menu();

    HWND hwnd = nullptr;

private:
    void create_rtv();
    void load();
    void save();
    void mark_dirty() { save_dirty_ = true; save_at_ = now() + 0.3; }
    void hotkey_thread();
    void note(const std::string& text, bool error = false) { message_ = text; message_error_ = error; }
    ComPtr<ID3D11ShaderResourceView> make_texture(const Bitmap& b);

    void sidebar();
    void page_crosshair();
    void profiles_panel(float height);
    void apply_crosshair(const json& s);
    void save_profiles();
    void update_crosshair();
    void set_crosshair_on(bool on);
    void page_fps();
    void fps_check(const char* label, const char* key);
    void fps_status_panel();
    void fps_tab_info();
    void fps_tab_look();
    void fps_tab_alerts();
    void fps_tab_position();
    void fps_tab_extra();
    void tick();
    // Performans testi ve uyarılar
    void bench_toggle();
    void bench_finish();
    void bench_tick(double t);
    void alerts_tick(double t);
    void show_alert(const std::string& text);
    void page_bench();
    // Oyunlar: oyuna özel profil, oyun modu, ekran rengi, oyun süresi, mola
    void games_tick(double t);
    void game_enter(int rule);
    void game_leave();
    void game_mode_start(const json& rule);
    void game_mode_stop();
    void page_games();
    void games_tab_rules();
    void games_tab_mode();
    void games_tab_time();
    void games_tab_break();
    void bench_export(const std::vector<int>& which);
    // Crosshair: resim, zemine göre renk, oyun penceresinin ortası
    POINT crosshair_point();
    void crosshair_follow();
    void pick_crosshair_image();
    // Nişan: hassasiyet çevirici, DPI ölçümü, nişan antrenmanı
    void page_aim();
    void aim_tab_sens();
    void aim_tab_dpi();
    void aim_tab_train();
    void aim_arena(float w, float h);
    void aim_spawn(int index, float w, float h);
    void aim_finish();
public:
    void on_raw_input(HRAWINPUT input);
    bool keys_fast() const { return opt_.value("keys_on", false); }
private:
    // Yayın araçları: tuş göstergesi, çalan şarkı, Kayıt Stüdyosu bağlantısı
    std::vector<KeyCap> key_caps();
    void extras_tick(double t);
    void rs_tick(double t);
    int rs_send(int command);           // -1: Kayıt Stüdyosu açık değil
    void rs_replay_save();
    void page_extras();
    void extras_tab_keys();
    void extras_tab_song();
    void extras_tab_rs();
    void set_fps_on(bool on);
    bool fps_page() const { return page_ == "fps" && IsWindowVisible(hwnd) && !IsIconic(hwnd); }
    void autostart_changed(const char* key);
    void page_hotkeys();
    void page_help();
    void page_settings();
    void page_developers();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11ShaderResourceView> crosshair_srv_, fps_srv_;

    Overlay overlay_, fps_overlay_, alert_overlay_;
    FpsMeter meter_;
    Sensors sensors_;
    Pinger pinger_;
    // Performans testi: süre boyunca saniyelik örnekler; bitince sonuç benchmarks.json'a eklenir
    bool bench_on_ = false;
    double bench_start_ = 0, bench_next_ = 0;
    std::string bench_game_;
    json bench_series_ = json::array();
    json benches_ = json::array();
    std::map<int, bool> bench_pick_;
    // Uyarılar: eşik birkaç saniye aşılırsa ekranın üstünde kısa yazı, aynı uyarı 2 dakikada bir
    std::map<std::string, double> alert_since_, alert_last_;
    // Oyunlar
    double games_next_ = 0, games_left_at_ = 0;
    int game_rule_ = -1;                // Öndeki oyunun kuralı
    std::string game_mode_exe_;         // Oyun modu bu oyun çalışırken açık
    json game_saved_;                   // Oyun bitince geri dönülecek crosshair ve gösterge durumu
    bool gm_power_set_ = false;
    GUID gm_old_scheme_{};
    std::vector<std::wstring> gm_closed_;
    json playtime_ = json::object();
    std::string playing_;               // Şu an süresi sayılan oyun
    double play_saved_at_ = 0, break_play_ = 0, break_idle_since_ = 0;
    std::vector<std::string> running_apps_;
    double gamma_test_until_ = 0;
    double alert_hide_at_ = 0, alert_next_check_ = 0;
    json crosshair_, fps_, opt_, profiles_ = json::object(), sensor_data_ = json::object();
    bool crosshair_on_ = false, fps_on_ = false, fps_visible_ = true, fps_dirty_ = false;
    bool task_installed_ = false, save_dirty_ = false, message_error_ = false;
    json fps_frame_ = json::object();
    std::vector<float> fps_graph_;
    double fps_value_ = -1, fps_next_read_ = 0, fps_next_draw_ = 0, fps_last_need_ = 0, next_top_ = 0, save_at_ = 0;
    int screen_w_ = 0, screen_h_ = 0;
    int fps_hz_ = 60, fps_preview_w_ = 0, fps_preview_h_ = 0, fps_sample_ = 144, fps_preview_bg_ = 0;
    std::string fps_overlay_sig_, fps_preview_sig_, page_ = "crosshair", message_;
    char profile_name_[64] = "";
    std::string pending_profile_, pending_action_, capturing_;
    std::thread keys_thread_;
    std::atomic<bool> keys_halt_{false};
    std::map<std::string, Hotkey> hotkeys_;
    std::mutex hotkeys_lock_;
    bool want_exit_ = false;
    int help_request_ = -1;
    // Crosshair
    std::string image_path_;            // Yüklü resmin yolu (her çizimde diskten okunmasın)
    Bitmap image_;
    bool alt_color_ = false;            // Zemin, ana renge yakın: ikinci renk kullanılıyor
    POINT ch_pos_{-1, -1};
    int ch_w_ = 0, ch_h_ = 0;
    double ch_next_ = 0;
    // DPI ölçümü: ham fare hareketi (Windows imleç hızından etkilenmez)
    bool dpi_listen_ = false, dpi_drag_ = false;
    long long dpi_dx_ = 0, dpi_dy_ = 0;
    double dpi_result_ = 0;
    // Nişan antrenmanı
    struct Target { float x, y, vx, vy; double born; };
    std::vector<Target> targets_;
    bool aim_on_ = false, aim_armed_ = false;
    double aim_start_ = 0, aim_last_ = 0, aim_on_time_ = 0, aim_react_sum_ = 0;
    int aim_hits_ = 0, aim_shots_ = 0, aim_react_n_ = 0;
    json aim_ = json::object(), aim_result_;
    unsigned rng_ = 12345;
    // Yayın araçları
    Overlay keys_overlay_, song_overlay_, badge_overlay_;
    std::string keys_sig_, song_sig_, badge_sig_;
    ComPtr<ID3D11ShaderResourceView> keys_srv_;
    std::string keys_preview_sig_;
    int keys_preview_w_ = 0, keys_preview_h_ = 0;
    MediaWatcher media_;
    NowPlaying song_;
    double song_next_ = 0, rs_next_ = 0, rs_game_gone_ = 0, rs_launched_at_ = -100;
    int rs_state_ = -1;                 // Kayıt Stüdyosu durum bitleri: 1 kayıt, 2 yayın, 4 tekrar, 8 duraklatıldı
    bool rs_replay_started_ = false;
    float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return (rng_ >> 8) / 16777216.f; }
};

App* g_app = nullptr;
UINT g_show_message = 0, g_taskbar_created = 0;
std::string g_test_page;

}  // namespace

// --- Ayarlar ---

void App::load() {
    json data = read_json(data_dir() + L"\\game-settings.json");
    if (!data.is_object()) data = json::object();
    benches_ = read_json(data_dir() + L"\\benchmarks.json");
    if (!benches_.is_array()) benches_ = json::array();
    playtime_ = read_json(data_dir() + L"\\playtime.json");
    if (!playtime_.is_object()) playtime_ = json::object();
    aim_ = read_json(data_dir() + L"\\aim.json");
    if (!aim_.is_object()) aim_ = json::object();
    opt_ = merge_known(default_options(), data);
    crosshair_ = merge_known(default_crosshair(), data.value("crosshair", json::object()));
    fps_ = merge_known(default_fps(), read_json(data_dir() + L"\\fps-settings.json"));
    const json saved = read_json(data_dir() + L"\\crosshair-profiles.json");
    if (saved.is_object()) {
        for (auto& [name, s] : saved.items()) profiles_[name] = merge_known(default_crosshair(), s);
    }
    // Kurulumla gelen profiller: var olanın üstüne yazılmaz, aynı paket bir kez eklenir (silinen geri gelmesin)
    const json bundled = read_json(exe_dir() + L"\\preset-profiles.json");
    const bool has_bundled = bundled.is_object() && !bundled.empty();
    if (saved.is_null() && !has_bundled) {
        profiles_ = preset_profiles();
        save_profiles();
    }
    if (has_bundled) {
        const std::string stamp = std::to_string(std::hash<std::string>{}(bundled.dump()));
        if (opt_.value("bundled_profiles", std::string()) != stamp) {
            for (auto& [name, s] : bundled.items())
                if (!profiles_.contains(name)) profiles_[name] = merge_known(default_crosshair(), s);
            save_profiles();
            opt_["bundled_profiles"] = stamp;
            mark_dirty();
        }
    }
    const json keys = data.value("hotkeys", json::object());
    std::lock_guard<std::mutex> g(hotkeys_lock_);
    for (auto& a : kActions) {
        Hotkey h = a.fallback;
        if (keys.contains(a.key)) {
            const auto& k = keys[a.key];
            h = {k.value("vk", 0), k.value("ctrl", false), k.value("shift", false), k.value("alt", false)};
        }
        hotkeys_[a.key] = h;
    }
}

void App::save() {
    json data = opt_;
    data["crosshair"] = crosshair_;
    json keys = json::object();
    {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        for (auto& [name, h] : hotkeys_)
            keys[name] = {{"vk", h.vk}, {"ctrl", h.ctrl}, {"shift", h.shift}, {"alt", h.alt}};
    }
    data["hotkeys"] = keys;
    write_json(data_dir() + L"\\game-settings.json", data);
    write_json(data_dir() + L"\\fps-settings.json", fps_);
}

void App::save_profiles() {
    write_json(data_dir() + L"\\crosshair-profiles.json", profiles_);
}

void App::hotkey_thread() {
    std::map<std::string, bool> held;
    while (!keys_halt_) {
        Sleep(20);
        std::map<std::string, Hotkey> copy;
        {
            std::lock_guard<std::mutex> g(hotkeys_lock_);
            copy = hotkeys_;
        }
        const bool ctrl = key_down(VK_CONTROL), shift = key_down(VK_SHIFT), alt = key_down(VK_MENU);
        int index = 0;
        for (auto& a : kActions) {
            const Hotkey& h = copy[a.key];
            const bool on = h.vk && key_down(h.vk) && h.ctrl == ctrl && h.shift == shift && h.alt == alt;
            if (on && !held[a.key]) PostMessageW(hwnd, WM_HOTKEY_ACTION, index, 0);
            held[a.key] = on;
            index++;
        }
    }
}

void App::on_hotkey(int action) {
    if (!capturing_.empty()) return;
    const std::string key = kActions[action].key;
    if (key == "crosshair") set_crosshair_on(!crosshair_on_);
    if (key == "crosshair_off") set_crosshair_on(false);
    if (key == "fps") {
        if (!fps_on_) set_fps_on(true);
        else fps_visible_ = !fps_visible_;
    }
    if (key == "fps_off") set_fps_on(false);
    if (key == "bench") bench_toggle();
    if (key == "keys") { opt_["keys_on"] = !opt_.value("keys_on", false); keys_sig_.clear(); mark_dirty(); }
    if (key == "replay_save") rs_replay_save();
}

// --- Pencere ve simge ---

void App::show_window() {
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd);
}

void App::hide_to_tray() {
    ShowWindow(hwnd, SW_HIDE);
    if (opt_.value("note_shown", false)) return;
    opt_["note_shown"] = true;
    mark_dirty();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfoTitle, TW("Oyun Araçları çalışmaya devam ediyor").c_str());
    wcscpy_s(nid.szInfo, TW("Göstergeler açık kaldı. Pencereyi açmak ya da programı kapatmak için saatin yanındaki simgeyi kullanın.").c_str());
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::add_tray_icon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wcscpy_s(nid.szTip, TW(kAppName).c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void App::tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_OPEN, TW("Aç").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (crosshair_on_ ? MF_CHECKED : 0), MENU_CROSSHAIR, L"Crosshair");
    AppendMenuW(menu, MF_STRING | (fps_on_ ? MF_CHECKED : 0), MENU_FPS, TW("FPS Ölçer").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, MENU_EXIT, TW("Çıkış").c_str());
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);      // Dışarı tıklanınca menü kapansın diye gerekli
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (choice == MENU_OPEN) show_window();
    if (choice == MENU_CROSSHAIR) set_crosshair_on(!crosshair_on_);
    if (choice == MENU_FPS) set_fps_on(!fps_on_);
    if (choice == MENU_EXIT) PostQuitMessage(0);
}

ComPtr<ID3D11ShaderResourceView> App::make_texture(const Bitmap& b) {
    ComPtr<ID3D11ShaderResourceView> srv;
    if (b.empty()) return srv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = b.w; td.Height = b.h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{b.px.data(), UINT(b.w * 4), 0};
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(device_->CreateTexture2D(&td, &init, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

void App::autostart_changed(const char* key) {
    const bool want = opt_.value("crosshair_autostart", false) || opt_.value("fps_autostart", false);
    if (want != task_installed_) {
        if (set_task(hwnd, want)) {
            task_installed_ = want;
        } else {
            opt_[key] = !opt_.value(key, false);    // Onay verilmediyse tik geri alınır
            note("Otomatik başlatma ayarlanamadı. Windows'un yönetici onayı gerekiyor.", true);
            return;
        }
    }
    mark_dirty();
    note(want ? "Bilgisayar açılınca işaretli göstergeler başlayacak" : "Otomatik başlatma kapatıldı");
}

// --- Göstergeler ---

void App::set_crosshair_on(bool on) {
    crosshair_on_ = on;
    update_crosshair();
}

// Resmi büyütür ya da küçültür (önceden çarpılmış BGRA, çift doğrusal). Tam katlarda pikseller keskin kalır.
Bitmap scale_bitmap(const Bitmap& src, float k) {
    Bitmap out;
    if (src.empty()) return out;
    out.w = std::clamp(int(lround(src.w * k)), 1, 801);
    out.h = std::clamp(int(lround(src.h * k)), 1, 801);
    out.px.resize(size_t(out.w) * out.h * 4);
    const bool sharp = k >= 1 && fabsf(k - roundf(k)) < 0.001f;
    for (int y = 0; y < out.h; y++) {
        for (int x = 0; x < out.w; x++) {
            uint8_t* d = &out.px[(size_t(y) * out.w + x) * 4];
            const float sx = (x + 0.5f) * src.w / out.w - 0.5f, sy = (y + 0.5f) * src.h / out.h - 0.5f;
            if (sharp) {
                const int ix = std::clamp(int(sx + 0.5f), 0, src.w - 1), iy = std::clamp(int(sy + 0.5f), 0, src.h - 1);
                memcpy(d, &src.px[(size_t(iy) * src.w + ix) * 4], 4);
                continue;
            }
            const int x0 = std::clamp(int(floorf(sx)), 0, src.w - 1), y0 = std::clamp(int(floorf(sy)), 0, src.h - 1);
            const int x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
            const float fx = std::clamp(sx - x0, 0.f, 1.f), fy = std::clamp(sy - y0, 0.f, 1.f);
            for (int c = 0; c < 4; c++) {
                auto at = [&](int xx, int yy) { return float(src.px[(size_t(yy) * src.w + xx) * 4 + c]); };
                const float top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx, bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
                d[c] = uint8_t(std::clamp(top * (1 - fy) + bottom * fy + 0.5f, 0.f, 255.f));
            }
        }
    }
    return out;
}

void App::update_crosshair() {
    json s = crosshair_;
    if (alt_color_ && s.value("adaptive", false)) s["color"] = s.value("adaptive_color", std::string("#ff00ff"));
    Bitmap b;
    const std::string image = s.value("image", std::string());
    if (!image.empty()) {
        if (image != image_path_) {
            image_path_ = image;
            image_ = {};
            load_image(widen(image), image_);
        }
        if (!image_.empty()) b = scale_bitmap(image_, std::clamp(s.value("image_scale", 100), 10, 800) / 100.f);
    }
    if (b.empty()) b = draw_crosshair(s);
    overlay_.hide_from_capture(opt_.value("crosshair_hide", false));
    ch_w_ = b.w;
    ch_h_ = b.h;
    if (crosshair_on_) {
        const POINT p = crosshair_point();
        ch_pos_ = p;
        overlay_.show(b, crosshair_.value("opacity", 100) / 100.f, p.x - b.w / 2, p.y - b.h / 2);
    } else {
        overlay_.hide();
    }
    crosshair_srv_ = make_texture(b);
}

void App::set_fps_on(bool on) {
    fps_on_ = on;
    fps_visible_ = true;
    fps_overlay_sig_.clear();
    if (!on) fps_overlay_.hide();
}

void App::tick() {
    const double t = now();
    // Oyun çözünürlüğü değiştirince ekran da değişir: göstergeler eski koordinatta kalmasın
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (sw != screen_w_ || sh != screen_h_) {
        const bool first = screen_w_ == 0;
        screen_w_ = sw;
        screen_h_ = sh;
        if (!first) {
            if (crosshair_on_) update_crosshair();
            fps_overlay_sig_.clear();
            fps_next_draw_ = 0;
        }
    }
    if (crosshair_on_ && t >= ch_next_) { ch_next_ = t + 0.1; crosshair_follow(); }
    if (t >= next_top_) {   // Kenarlıksız pencere oyunlar öne geçse de göstergeler üstte kalsın
        next_top_ = t + 0.25;
        overlay_.keep_on_top();
        fps_overlay_.keep_on_top();
        keys_overlay_.keep_on_top();
        song_overlay_.keep_on_top();
        badge_overlay_.keep_on_top();
    }
    if (fps_dirty_) {
        fps_dirty_ = false;
        mark_dirty();
        fps_overlay_sig_.clear(); fps_preview_sig_.clear();
    }
    if (save_dirty_ && t >= save_at_) { save(); save_dirty_ = false; }

    alerts_tick(t);
    extras_tick(t);
    bench_tick(t);
    games_tick(t);
    if (gamma_test_until_ && t >= gamma_test_until_ && game_rule_ < 0) {     // "5 saniye dene" bitti
        gamma_test_until_ = 0;
        apply_gamma(0, 0, 0);
        opt_["gamma_applied"] = false;
    }
    const bool need = fps_on_ || fps_page() || bench_on_ || fps_.value("alert_on", false) || page_ == "bench";
    if (need) fps_last_need_ = t;
    if (!need) {
        if (sensors_.running() && t - fps_last_need_ > 30) {   // Sayfadan çıkınca hemen kapanmasın: açılışı birkaç saniye sürer
            sensors_.stop();
            meter_.stop();
            pinger_.stop();
        }
        return;
    }
    if (fps_.value("show_ping", false)) pinger_.start(fps_.value("ping_host", std::string("1.1.1.1")));
    else pinger_.stop();
    if (!meter_.running()) { meter_.start(); fps_next_read_ = t + 0.5; }
    if (!sensors_.running()) sensors_.start();
    if (t < fps_next_draw_) return;
    fps_next_draw_ = t + 0.1;

    sensors_.configure(fps_wanted(fps_) + (fps_.value("alert_on", false) || bench_on_ ? " gpu cpu battery" : ""), fps_.value("sensor_interval", 1000));
    sensor_data_ = sensors_.data();
    bool fresh_read = false;
    if (t >= fps_next_read_) {
        fps_next_read_ = t + std::max(fps_.value("interval", 500), 50) / 1000.0;
        fps_value_ = meter_.read();
        fps_hz_ = refresh_rate();
        fresh_read = true;
        // Ortalama ve en düşük FPS, son N saniyeden; grafik için kare süreleri
        const FrameStats st = frame_stats(meter_.recent(std::clamp(fps_.value("lows_window", 10), 2, 120)));
        fps_frame_ = {{"avg", st.avg}, {"low1", st.low1}, {"low01", st.low01}};
        fps_graph_.clear();
        if (fps_.value("show_graph", false)) {
            const auto samples = meter_.recent(std::clamp(fps_.value("graph_seconds", 5), 1, 30));
            const size_t buckets = 120;     // Çok kare varsa gruplanır; takılma kaybolmasın diye grubun en büyüğü alınır
            const size_t per = std::max<size_t>(1, (samples.size() + buckets - 1) / buckets);
            for (size_t i = 0; i < samples.size(); i += per) {
                double top = 0;
                for (size_t k = i; k < std::min(samples.size(), i + per); k++) top = std::max(top, samples[k].ms);
                fps_graph_.push_back(float(top));
            }
        }
    }
    json live = sensor_data_;
    live["frame"] = fps_frame_;
    if (pinger_.running()) live["ping"] = pinger_.data();
    // Sensörler ilk ölçümü yapana kadar bekle: satırlar sonradan eklenip yerleşimi kaydırmasın
    const bool ready = !sensor_data_.empty() || fps_wanted(fps_).empty() || sensors_.status() != "loading";
    const bool scale = fps_.value("scale_with_resolution", true);

    fps_overlay_.hide_from_capture(opt_.value("fps_hide", false));
    if (fps_on_ && fps_visible_) {
        auto rows = ready ? fps_rows(fps_, fps_value_, fps_hz_, live) : std::vector<FpsRow>{};
        if (bench_on_) {    // Test sürüyor: göstergede süre
            const int sec = int(t - bench_start_);
            char text[16];
            sprintf_s(text, "%02d:%02d", sec / 60, sec % 60);
            FpsCell c;
            c.tmpl = "00:00";
            c.parts.push_back({text, 'v', 0xff5050});
            rows.push_back({T("TEST"), {{0, c}}, true});
        }
        std::string sig = fps_signature(rows);
        if (fps_.value("show_graph", false) && fresh_read) sig += std::to_string(t);     // Grafik her okumada yenilenir
        if (sig != fps_overlay_sig_) {   // Aynıysa yeniden çizme: çizim de ekrana kare olarak sayılır
            fps_overlay_sig_ = sig;
            const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
            const float k = scale ? sh / 1080.f : 1.f;
            const Bitmap b = draw_fps(fps_, rows, k, fps_graph_);
            if (b.empty()) {
                fps_overlay_.hide();
            } else {
                int x, y;
                fps_place(fps_, b.w, b.h, sw, sh, k, x, y);
                fps_overlay_.show(b, fps_.value("opacity", 100) / 100.f, x, y);
            }
        }
    } else if (fps_overlay_.visible()) {
        fps_overlay_.hide();
        fps_overlay_sig_.clear();
    }
    if (fps_page()) {
        json preview_data = fps_preview_data(sensor_data_, fps_.value("gpu", std::string()));
        // Önizleme: canlı değer yoksa örnek değerler
        const double sample_fps = fps_sample_;
        preview_data["frame"] = {{"avg", fps_frame_.value("avg", -1.0) > 0 ? fps_frame_["avg"].get<double>() : sample_fps * 0.97},
                                 {"low1", fps_frame_.value("low1", -1.0) > 0 ? fps_frame_["low1"].get<double>() : sample_fps * 0.7},
                                 {"low01", fps_frame_.value("low01", -1.0) > 0 ? fps_frame_["low01"].get<double>() : sample_fps * 0.55}};
        json ping = pinger_.running() ? pinger_.data() : json::object();
        if (!ping.contains("ms")) ping = {{"ms", 24}, {"loss", 0}};
        preview_data["ping"] = ping;
        std::vector<float> graph = fps_graph_;
        if (graph.size() < 2 && sample_fps > 0)     // Örnek grafik
            for (int i = 0; i < 90; i++) graph.push_back(float(1000 / sample_fps * (1 + 0.15 * sin(i * 0.7)) * (i % 37 == 0 ? 2.6 : 1)));
        const auto sample = fps_rows(fps_, fps_sample_, fps_hz_, preview_data);
        std::string preview = fps_signature(sample);
        if (fps_.value("show_graph", false) && fresh_read) preview += std::to_string(t);
        if (preview != fps_preview_sig_) {
            fps_preview_sig_ = preview;
            const Bitmap b = draw_fps(fps_, sample, scale ? GetSystemMetrics(SM_CYSCREEN) / 1080.f : 1.f, graph);
            fps_preview_w_ = b.w;
            fps_preview_h_ = b.h;
            fps_srv_ = make_texture(b);
        }
    }
}

// --- Performans testi ---

namespace {
std::string foreground_game() {     // Testi başlatan an öndeki program (oyun)
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId()) return T("Masaüstü");
    std::string name;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &size)) {
            std::wstring w = path;
            w = w.substr(w.find_last_of(L'\\') + 1);
            if (w.size() > 4 && _wcsicmp(w.c_str() + w.size() - 4, L".exe") == 0) w.resize(w.size() - 4);
            name = narrow(w);
        }
        CloseHandle(p);
    }
    return name.empty() ? T("Bilinmiyor") : name;
}

double field(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : -1;
}
}  // namespace

void App::bench_toggle() {
    if (bench_on_) { bench_finish(); return; }
    if (!meter_.running()) meter_.start();
    if (!sensors_.running()) sensors_.start();
    meter_.session_begin();
    bench_on_ = true;
    bench_start_ = bench_next_ = now();
    bench_game_ = foreground_game();
    bench_series_ = json::array();
    show_alert(TF("Performans testi başladı: %s", bench_game_.c_str()));
    note(TF("Performans testi başladı: %s", bench_game_.c_str()));
}

void App::bench_tick(double t) {
    if (!bench_on_ || t < bench_next_) return;
    bench_next_ += 1;
    const FrameStats st = frame_stats(meter_.recent(1.0));
    json point = {{"t", int(t - bench_start_)}, {"fps", st.avg}};
    if (const json* gpu = fps_pick_gpu(sensor_data_, fps_.value("gpu", std::string()))) {
        point["gpu_temp"] = field(*gpu, "temp");
        point["gpu_load"] = field(*gpu, "load");
    }
    if (auto c = sensor_data_.find("cpu"); c != sensor_data_.end() && c->is_object()) {
        point["cpu_temp"] = field(*c, "temp");
        point["cpu_load"] = field(*c, "load");
    }
    bench_series_.push_back(point);
    const int limit = fps_.value("bench_seconds", 60);
    if (limit > 0 && t - bench_start_ >= limit) bench_finish();
}

void App::bench_finish() {
    if (!bench_on_) return;
    bench_on_ = false;
    const auto samples = meter_.session_end();
    const FrameStats st = frame_stats(samples);
    auto summary = [&](const char* key, bool max) {     // Saniyelik örneklerden en yüksek ya da ortalama
        double total = 0, top = -1;
        int n = 0;
        for (auto& p : bench_series_) {
            const double v = field(p, key);
            if (v < 0) continue;
            total += v; n++;
            top = std::max(top, v);
        }
        return max ? top : n ? total / n : -1;
    };
    SYSTEMTIME now_time;
    GetLocalTime(&now_time);
    char date[32];
    sprintf_s(date, "%04d-%02d-%02d %02d:%02d", now_time.wYear, now_time.wMonth, now_time.wDay, now_time.wHour, now_time.wMinute);
    json r = {{"game", bench_game_}, {"date", date}, {"seconds", int(now() - bench_start_)},
              {"avg", st.avg}, {"low1", st.low1}, {"low01", st.low01}, {"min", st.min}, {"max", st.max},
              {"gpu_temp_max", summary("gpu_temp", true)}, {"gpu_temp_avg", summary("gpu_temp", false)},
              {"cpu_temp_max", summary("cpu_temp", true)}, {"cpu_temp_avg", summary("cpu_temp", false)},
              {"gpu_load_avg", summary("gpu_load", false)}, {"cpu_load_avg", summary("cpu_load", false)}, {"series", bench_series_}};
    benches_.insert(benches_.begin(), r);
    while (benches_.size() > 100) benches_.erase(benches_.size() - 1);
    write_json(data_dir() + L"\\benchmarks.json", benches_);
    bench_pick_.clear();
    const std::string text = st.avg > 0 ? TF("Test bitti: ortalama %.0f FPS, %%1 en düşük %.0f FPS", st.avg, st.low1) : std::string(T("Test bitti: kare ölçülemedi"));
    show_alert(text);
    note(text);
}

// --- Uyarılar: eşik birkaç saniye aşılınca ekranın üst ortasında kısa yazı ---

void App::show_alert(const std::string& text) {
    const float k = GetSystemMetrics(SM_CYSCREEN) / 1080.f;
    const json s = {{"text", text}, {"font", "Segoe UI"}, {"font_size", 26}, {"bold", true}, {"color", "#ffffff"}, {"outline", false},
                    {"background", true}, {"bg_color", "#b02828"}, {"bg_opacity", 88}};
    const Bitmap b = draw_text(s, k);
    if (b.empty()) return;
    alert_overlay_.hide_from_capture(opt_.value("fps_hide", false));
    alert_overlay_.show(b, 1.f, GetSystemMetrics(SM_CXSCREEN) / 2 - b.w / 2, int(40 * k));
    alert_hide_at_ = now() + 6;
}

void App::alerts_tick(double t) {
    if (alert_hide_at_ && t >= alert_hide_at_) { alert_overlay_.hide(); alert_hide_at_ = 0; }
    if (!fps_.value("alert_on", false) || t < alert_next_check_) return;
    alert_next_check_ = t + 1;
    auto check = [&](const std::string& key, bool over, const std::string& text) {
        if (!over) { alert_since_.erase(key); return; }
        if (!alert_since_.count(key)) alert_since_[key] = t;
        if (t - alert_since_[key] >= 5 && t - alert_last_[key] >= 120) {   // 5 sn sürerse, aynı uyarı en sık 2 dakikada bir
            alert_last_[key] = t;
            show_alert(text);
            if (fps_.value("alert_sound", true)) MessageBeep(MB_ICONWARNING);
        }
    };
    if (const json* gpu = fps_pick_gpu(sensor_data_, fps_.value("gpu", std::string()))) {
        const double v = field(*gpu, "temp");
        check("gpu", v >= fps_.value("alert_gpu_temp", 85), TF("Ekran kartı çok ısındı: %.0f °C", v));
    }
    if (auto c = sensor_data_.find("cpu"); c != sensor_data_.end() && c->is_object()) {
        const double v = field(*c, "temp");
        check("cpu", v >= fps_.value("alert_cpu_temp", 90), TF("İşlemci çok ısındı: %.0f °C", v));
    }
    if (auto b = sensor_data_.find("battery"); b != sensor_data_.end() && b->is_object()) {
        const double level = field(*b, "level"), rate = b->value("rate", 0.0);
        check("battery", level >= 0 && level <= fps_.value("alert_battery", 15) && rate <= 0, TF("Pil azaldı: %%%.0f", level));
    }
}

void App::bench_export(const std::vector<int>& which) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"CSV", L"*.csv"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"csv");
    dlg->SetFileName(L"Performance tests.csv");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::ofstream out(path, std::ios::binary);
    out << "\xEF\xBB\xBF";      // Excel Türkçe harfleri doğru açsın
    auto num = [](double v, int d = 1) { if (v < 0) return std::string(); char b[32]; sprintf_s(b, "%.*f", d, v); return std::string(b); };
    out << T("Oyun") << ';' << T("Tarih") << ';' << T("Süre (sn)") << ";Ort FPS;1% low;0.1% low;Min FPS;Max FPS;GPU max C;GPU ort C;CPU max C;CPU ort C;GPU %;CPU %\r\n";
    for (int i : which) {
        const json& r = benches_[i];
        out << r.value("game", std::string()) << ';' << r.value("date", std::string()) << ';' << r.value("seconds", 0) << ';'
            << num(field(r, "avg")) << ';' << num(field(r, "low1")) << ';' << num(field(r, "low01")) << ';' << num(field(r, "min")) << ';'
            << num(field(r, "max")) << ';' << num(field(r, "gpu_temp_max"), 0) << ';' << num(field(r, "gpu_temp_avg"), 0) << ';'
            << num(field(r, "cpu_temp_max"), 0) << ';' << num(field(r, "cpu_temp_avg"), 0) << ';' << num(field(r, "gpu_load_avg"), 0) << ';'
            << num(field(r, "cpu_load_avg"), 0) << "\r\n";
    }
    out << "\r\n";
    for (int i : which) {       // Saniye saniye ölçümler
        const json& r = benches_[i];
        out << r.value("game", std::string()) << " - " << r.value("date", std::string()) << "\r\n" << T("Saniye") << ";FPS;GPU C;GPU %;CPU C;CPU %\r\n";
        for (auto& p : r.value("series", json::array()))
            out << p.value("t", 0) << ';' << num(field(p, "fps")) << ';' << num(field(p, "gpu_temp"), 0) << ';' << num(field(p, "gpu_load"), 0) << ';'
                << num(field(p, "cpu_temp"), 0) << ';' << num(field(p, "cpu_load"), 0) << "\r\n";
        out << "\r\n";
    }
    note(TF("Kaydedildi: %s", narrow(path).c_str()));
    CoTaskMemFree(path);
}

void App::page_bench() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Performans testi");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Oyundayken kısayola (varsayılan F6) basın: FPS, en düşük FPS, sıcaklık ve kullanım belirlediğiniz süre boyunca ölçülür. "
                       "Bitince sonuç buraya eklenir; ayarlarınızı değiştirip yeniden ölçerek karşılaştırın.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (bench_on_) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 60, 255));
    if (ImGui::Button(bench_on_ ? "Testi durdur" : "Testi başlat", ImVec2(180, 36))) bench_toggle();
    if (bench_on_) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    const int limit = fps_.value("bench_seconds", 60);
    if (ImGui::BeginCombo("Süre", limit ? TF("%d saniye", limit).c_str() : T("Elle durdurulur"))) {
        for (int v : {30, 60, 120, 300, 0})
            if (ImGui::Selectable(v ? TF("%d saniye", v).c_str() : T("Elle durdurulur"), v == limit)) { fps_["bench_seconds"] = v; fps_dirty_ = true; }
        ImGui::EndCombo();
    }
    if (bench_on_) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme_error(), "%s", TF("Ölçülüyor: %s  ·  %d sn", bench_game_.c_str(), int(now() - bench_start_)).c_str());
    }
    ImGui::TextDisabled("Uygulama penceresinden başlatılırsa oyun adı \"Masaüstü\" olur; en doğru sonuç için oyundayken kısayolu kullanın.");
    ImGui::Spacing();
    ImGui::SeparatorText("Sonuçlar");
    if (benches_.empty()) { ImGui::TextDisabled("Henüz test yok."); return; }
    std::vector<int> picked;
    for (int i = 0; i < int(benches_.size()); i++) if (bench_pick_[i]) picked.push_back(i);
    if (ImGui::Button("CSV olarak kaydet")) {
        std::vector<int> all;
        for (int i = 0; i < int(benches_.size()); i++) all.push_back(i);
        bench_export(picked.empty() ? all : picked);
    }
    ImGui::SetItemTooltip("Seçili testler, hiç seçim yoksa hepsi");
    ImGui::SameLine();
    ImGui::BeginDisabled(picked.empty());
    if (ImGui::Button("Seçilenleri sil")) {
        for (auto it = picked.rbegin(); it != picked.rend(); ++it) benches_.erase(*it);
        bench_pick_.clear();
        write_json(data_dir() + L"\\benchmarks.json", benches_);
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("İki test seçerseniz altta karşılaştırılır.");
    auto text = [](double v, const char* unit = "") { if (v < 0) return std::string("-"); char b[32]; sprintf_s(b, "%.0f%s", v, unit); return std::string(b); };
    const float table_h = std::min(320.f, 30.f + 26.f * benches_.size());
    if (ImGui::BeginTable("testler", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##sec", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Oyun");
        ImGui::TableSetupColumn("Tarih");
        ImGui::TableSetupColumn("Süre");
        ImGui::TableSetupColumn("Ort. FPS");
        ImGui::TableSetupColumn("%1 en düşük");
        ImGui::TableSetupColumn("%0,1 en düşük");
        ImGui::TableSetupColumn("GPU en sıcak");
        ImGui::TableSetupColumn("CPU en sıcak");
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(benches_.size()); i++) {
            const json& r = benches_[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = bench_pick_[i];
            if (ImGui::Checkbox("##s", &on)) bench_pick_[i] = on;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.value("game", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.value("date", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(TF("%d sn", r.value("seconds", 0)).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(theme_accent(), "%s", text(field(r, "avg")).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(text(field(r, "low1")).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(text(field(r, "low01")).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(text(field(r, "gpu_temp_max"), " °C").c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(text(field(r, "cpu_temp_max"), " °C").c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Seçilen testlerin saniyelik FPS çizgisi
    for (int i : picked) {
        std::vector<float> line;
        for (auto& p : benches_[i].value("series", json::array())) line.push_back(float(std::max(0.0, field(p, "fps"))));
        if (line.size() < 2) continue;
        const std::string title = benches_[i].value("game", std::string()) + "  " + benches_[i].value("date", std::string());
        ImGui::PlotLines(("##cizgi" + std::to_string(i)).c_str(), line.data(), int(line.size()), 0, title.c_str(), 0, FLT_MAX, ImVec2(-1, 70));
    }
    if (picked.size() == 2) {   // Karşılaştırma: ikinci seçilen birinciye göre
        const json& a = benches_[picked[1]];
        const json& b = benches_[picked[0]];
        ImGui::SeparatorText("Karşılaştırma");
        if (ImGui::BeginTable("kars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Ölçüm");
            ImGui::TableSetupColumn((a.value("game", std::string()) + "  " + a.value("date", std::string())).c_str());
            ImGui::TableSetupColumn((b.value("game", std::string()) + "  " + b.value("date", std::string())).c_str());
            ImGui::TableSetupColumn("Fark");
            ImGui::TableHeadersRow();
            struct M { const char* label; const char* key; bool higher_better; };
            for (auto m : {M{"Ortalama FPS", "avg", true}, M{"%1 en düşük", "low1", true}, M{"%0,1 en düşük", "low01", true},
                           M{"GPU en sıcak (°C)", "gpu_temp_max", false}, M{"CPU en sıcak (°C)", "cpu_temp_max", false},
                           M{"GPU kullanımı (%)", "gpu_load_avg", false}, M{"CPU kullanımı (%)", "cpu_load_avg", false}}) {
                const double va = field(a, m.key), vb = field(b, m.key);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.label);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(text(va).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(text(vb).c_str());
                ImGui::TableNextColumn();
                if (va > 0 && vb >= 0) {
                    const double pct = (vb - va) / va * 100;
                    const bool good = m.higher_better ? pct >= 0 : pct <= 0;
                    char buf[32];
                    sprintf_s(buf, "%+.1f%%", pct);
                    ImGui::TextColored(std::fabs(pct) < 1 ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : good ? theme_good() : theme_error(), "%s", buf);
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::EndTable();
        }
    }
}

// --- Oyunlar: oyun açılınca ayarlar kendiliğinden değişir, oyun bitince geri döner ---

namespace {
std::wstring process_path(DWORD pid) {
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &size)) out = path;
        CloseHandle(p);
    }
    return out;
}

std::string exe_name(const std::wstring& path) {
    std::wstring w = path.substr(path.find_last_of(L'\\') + 1);
    for (auto& c : w) c = wchar_t(towlower(c));
    return narrow(w);
}

// Öndeki pencerenin programı ve ekranı kaplayıp kaplamadığı
std::string foreground_exe(bool& fullscreen) {
    fullscreen = false;
    HWND h = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!h || !pid || pid == GetCurrentProcessId()) return {};
    RECT r;
    MONITORINFO mi{sizeof(mi)};
    if (GetWindowRect(h, &r) && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        fullscreen = r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
    return exe_name(process_path(pid));
}

// Çalışan programların her biri için bir yol (oyun modu kapatma ve yeniden açma için)
std::map<std::string, std::vector<std::pair<DWORD, std::wstring>>> running_processes() {
    std::map<std::string, std::vector<std::pair<DWORD, std::wstring>>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        std::wstring n = e.szExeFile;
        for (auto& c : n) c = wchar_t(towlower(c));
        out[narrow(n)].push_back({e.th32ProcessID, {}});
    }
    CloseHandle(snap);
    return out;
}

// Pencereli programlar (listeden oyun seçmek için)
std::vector<std::string> windowed_apps() {
    std::vector<std::string> out;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetWindowTextLengthW(h) == 0) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;
        const std::string n = exe_name(process_path(pid));
        if (n.empty() || n == "explorer.exe" || n == "applicationframehost.exe" || n == "textinputhost.exe") continue;
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Ekran rengi: parlaklık, kontrast, gama (-50..50). Hepsi 0 ise düz rampa (Windows varsayılanı).
void apply_gamma(int brightness, int contrast, int gamma) {
    WORD ramp[3][256];
    const double exponent = pow(2.0, -gamma / 50.0);
    for (int i = 0; i < 256; i++) {
        double v = pow(i / 255.0, exponent);
        v = (v - 0.5) * (1 + contrast / 100.0) + 0.5 + brightness / 200.0;
        const WORD w = WORD(std::clamp(v, 0.0, 1.0) * 65535);
        ramp[0][i] = ramp[1][i] = ramp[2][i] = w;
    }
    HDC dc = GetDC(nullptr);
    SetDeviceGammaRamp(dc, ramp);
    ReleaseDC(nullptr, dc);
}

const char* kNotGames[] = {"explorer.exe", "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe", "brave.exe", "vlc.exe",
                           "mpc-hc64.exe", "potplayermini64.exe", "video editor.exe", "recording studio.exe", "game tools.exe",
                           "code.exe", "devenv.exe", "powerpnt.exe", "applicationframehost.exe", "lockapp.exe"};
}  // namespace

// Crosshair'ın ekrandaki merkezi: ana ekranın ortası ya da öndeki oyun penceresinin iç alanının ortası
// (pencereli oyun, ikinci ekrandaki oyun). Oyun: listedeki program ya da ekranı kaplayan, tarayıcı vb. olmayan program.
POINT App::crosshair_point() {
    POINT c{GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
    if (opt_.value("crosshair_center", 1) == 1) {
        bool fullscreen = false;
        const std::string exe = foreground_exe(fullscreen);
        bool game = false;
        if (!exe.empty()) {
            for (auto& r : opt_["games"]) if (_stricmp(r.value("exe", std::string()).c_str(), exe.c_str()) == 0) game = true;
            if (!game && fullscreen) {
                game = true;
                for (const char* n : kNotGames) if (exe == n) game = false;
            }
        }
        HWND h = GetForegroundWindow();
        RECT r;
        if (game && GetClientRect(h, &r) && r.right > 0 && r.bottom > 0) {
            POINT p{r.right / 2, r.bottom / 2};
            ClientToScreen(h, &p);
            c = p;
        }
    }
    c.x += opt_.value("crosshair_offset_x", 0);
    c.y += opt_.value("crosshair_offset_y", 0);
    return c;
}

namespace {
float luminance(uint32_t rgb) {
    return (0.2126f * ((rgb >> 16) & 255) + 0.7152f * ((rgb >> 8) & 255) + 0.0722f * (rgb & 255)) / 255.f;
}

// Merkezin çevresindeki zeminin ortalama parlaklığı. CAPTUREBLT olmadan alınır: katmanlı göstergeler görüntüye girmez.
float background_luminance(POINT p, int radius) {
    const int n = radius * 2;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, dib);
    BitBlt(dc, 0, 0, n, n, screen, p.x - radius, p.y - radius, SRCCOPY);
    double sum = 0;
    const uint8_t* px = static_cast<const uint8_t*>(bits);
    for (int i = 0; px && i < n * n; i++) sum += 0.0722 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.2126 * px[i * 4 + 2];
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return float(sum / (n * n) / 255.0);
}
}  // namespace

void App::crosshair_follow() {
    const POINT p = crosshair_point();
    if (p.x != ch_pos_.x || p.y != ch_pos_.y) {
        ch_pos_ = p;
        overlay_.move(p.x - ch_w_ / 2, p.y - ch_h_ / 2);
    }
    // Zemine göre renk: hangi renk zeminden daha çok ayrışıyorsa o; gidip gelmesin diye belirgin fark gerekir
    if (crosshair_.value("adaptive", false) && crosshair_.value("image", std::string()).empty()) {
        const float bg = background_luminance(p, 30);
        const float main = fabsf(bg - luminance(parse_color(crosshair_.value("color", std::string("#00ff00")))));
        const float alt = fabsf(bg - luminance(parse_color(crosshair_.value("adaptive_color", std::string("#ff00ff")))));
        const bool want = alt_color_ ? !(main > alt + 0.12f) : alt > main + 0.12f;
        if (want != alt_color_) { alt_color_ = want; update_crosshair(); }
    } else if (alt_color_) {
        alt_color_ = false;
        update_crosshair();
    }
}

void App::pick_crosshair_image() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    const std::wstring images = TW("Resimler");
    COMDLG_FILTERSPEC spec[] = {{images.c_str(), L"*.png;*.bmp;*.gif;*.jpg;*.jpeg;*.webp"}};
    dlg->SetFileTypes(1, spec);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->Show(hwnd)) || FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    Bitmap test;
    if (!load_image(path, test)) {
        note("Resim açılamadı", true);
    } else {
        // Resim programın klasörüne kopyalanır: asıl dosya taşınsa da silinse de crosshair bozulmaz
        const std::wstring folder = data_dir() + L"\\Crosshairs";
        CreateDirectoryW(folder.c_str(), nullptr);
        std::wstring name = path;
        name = name.substr(name.find_last_of(L'\\') + 1);
        const std::wstring copy = folder + L"\\" + name;
        const bool same = _wcsicmp(copy.c_str(), path) == 0;
        if (!same) CopyFileW(path, copy.c_str(), FALSE);
        crosshair_["image"] = narrow(same || GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES ? copy : std::wstring(path));
        image_path_.clear();
        if (std::max(test.w, test.h) > 64 && crosshair_.value("image_scale", 100) == 100)    // Büyük resim ekranı kaplamasın
            crosshair_["image_scale"] = std::max(10, int(6400 / std::max(test.w, test.h)));
        mark_dirty();
        update_crosshair();
        note("Resim crosshair olarak ayarlandı");
    }
    CoTaskMemFree(path);
}

void App::games_tick(double t) {
    if (t < games_next_) return;
    games_next_ = t + 1;
    bool fullscreen = false;
    const std::string front = foreground_exe(fullscreen);
    const json& rules = opt_["games"];
    int rule = -1;
    for (int i = 0; i < int(rules.size()); i++)
        if (!front.empty() && _stricmp(rules[i].value("exe", std::string()).c_str(), front.c_str()) == 0) rule = i;

    // Oyuna özel profil: oyun öndeyken geçerli; başka pencereye geçince 2 sn sonra eski haline döner
    if (rule >= 0 && rule != game_rule_) { if (game_rule_ >= 0) game_leave(); game_enter(rule); }
    if (rule < 0 && game_rule_ >= 0) {
        if (!games_left_at_) games_left_at_ = t;
        if (t - games_left_at_ >= 2) game_leave();
    } else {
        games_left_at_ = 0;
    }

    // Oyun modu: oyun çalıştığı sürece (alt-tab ile çıkılsa da) açık kalır
    if (!game_mode_exe_.empty()) {
        const auto procs = running_processes();
        if (!procs.count(game_mode_exe_)) game_mode_stop();
    }
    if (rule >= 0 && rules[rule].value("game_mode", false) && game_mode_exe_.empty()) game_mode_start(rules[rule]);

    // Oyun süresi: listedeki oyunlar, isteğe bağlı olarak ekranı kaplayan diğer programlar
    bool is_game = rule >= 0;
    if (!is_game && fullscreen && opt_.value("playtime_fullscreen", true) && !front.empty()) {
        is_game = true;
        for (const char* n : kNotGames) if (front == n) is_game = false;
    }
    playing_ = is_game ? front : std::string();
    if (is_game) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char day[16];
        sprintf_s(day, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        json& g = playtime_[front];
        g["total"] = g.value("total", 0) + 1;
        g["days"][day] = g["days"].value(day, 0) + 1;
        if (t - play_saved_at_ > 60) { play_saved_at_ = t; write_json(data_dir() + L"\\playtime.json", playtime_); }
        // Mola: kesintisiz oyun süresi
        break_play_ += 1;
        break_idle_since_ = 0;
        const int minutes = std::max(10, opt_.value("break_minutes", 60));
        if (opt_.value("break_on", false) && break_play_ >= minutes * 60) {
            break_play_ = 0;
            show_alert(TF("Mola zamanı: %d dakikadır oynuyorsunuz. Gözlerinizi dinlendirin, biraz su için.", minutes));
            if (fps_.value("alert_sound", true)) MessageBeep(MB_ICONASTERISK);
        }
    } else {
        if (!break_idle_since_) break_idle_since_ = t;
        if (t - break_idle_since_ > 300) break_play_ = 0;       // 5 dakikadan uzun ara mola sayılır
    }
}

void App::game_enter(int rule) {
    const json& r = opt_["games"][rule];
    game_rule_ = rule;
    game_saved_ = {{"crosshair", crosshair_}, {"crosshair_on", crosshair_on_}, {"fps_on", fps_on_}};
    const std::string profile = r.value("crosshair_profile", std::string());
    if (!profile.empty() && profiles_.contains(profile)) { crosshair_ = profiles_[profile]; update_crosshair(); }
    const std::string ch = r.value("crosshair", std::string("keep")), fps = r.value("fps", std::string("keep"));
    if (ch != "keep") set_crosshair_on(ch == "on");
    if (fps != "keep") set_fps_on(fps == "on");
    if (r.value("color", false)) {
        apply_gamma(r.value("brightness", 0), r.value("contrast", 0), r.value("gamma", 0));
        opt_["gamma_applied"] = true;
        mark_dirty();
    }
    note(TF("Oyun profili: %s", r.value("name", r.value("exe", std::string())).c_str()));
}

void App::game_leave() {
    if (game_rule_ < 0) return;
    game_rule_ = -1;
    games_left_at_ = 0;
    if (game_saved_.is_object()) {
        crosshair_ = game_saved_.value("crosshair", crosshair_);
        set_crosshair_on(game_saved_.value("crosshair_on", false));
        if (game_saved_.value("fps_on", false) != fps_on_) set_fps_on(game_saved_.value("fps_on", false));
    }
    if (opt_.value("gamma_applied", false)) {
        apply_gamma(0, 0, 0);
        opt_["gamma_applied"] = false;
        mark_dirty();
    }
}

void App::game_mode_start(const json& rule) {
    game_mode_exe_ = rule.value("exe", std::string());
    for (auto& c : game_mode_exe_) c = char(tolower(uint8_t(c)));
    if (opt_.value("gm_power", true) && !gm_power_set_) {      // En yüksek performans güç planı
        GUID* active = nullptr;
        if (PowerGetActiveScheme(nullptr, &active) == ERROR_SUCCESS && active) {
            gm_old_scheme_ = *active;
            LocalFree(active);
            if (PowerSetActiveScheme(nullptr, &GUID_MIN_POWER_SAVINGS) == ERROR_SUCCESS) gm_power_set_ = true;
        }
    }
    gm_closed_.clear();     // Seçilen programlar kapatılır (pencerelerine "kapan" denir, zorla sonlandırılmaz)
    const auto procs = running_processes();
    for (auto& name : opt_.value("gm_close", json::array())) {
        std::string n = name.get<std::string>();
        for (auto& c : n) c = char(tolower(uint8_t(c)));
        auto it = procs.find(n);
        if (it == procs.end()) continue;
        std::wstring path;
        for (auto& [pid, unused] : it->second) {
            if (path.empty()) path = process_path(pid);
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                DWORD owner = 0;
                GetWindowThreadProcessId(h, &owner);
                if (owner == pid && !GetWindow(h, GW_OWNER)) PostMessageW(h, WM_CLOSE, 0, 0);
            }
        }
        if (!path.empty()) gm_closed_.push_back(path);
    }
    note(TF("Oyun modu açık: %s", game_mode_exe_.c_str()));
}

void App::game_mode_stop() {
    if (gm_power_set_) { PowerSetActiveScheme(nullptr, &gm_old_scheme_); gm_power_set_ = false; }
    if (opt_.value("gm_reopen", true)) {       // Kapatılan programlar yeniden açılır
        const auto procs = running_processes();
        for (auto& path : gm_closed_)
            if (!procs.count(exe_name(path))) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWMINNOACTIVE);
    }
    gm_closed_.clear();
    note(TF("Oyun modu kapandı: %s", game_mode_exe_.c_str()));
    game_mode_exe_.clear();
}

void App::page_games() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Oyunlar");
    ImGui::PopFont();
    ImGui::TextDisabled("Oyun açılınca ayarlar kendiliğinden değişir, oyun bitince eski haline döner.");
    ImGui::Spacing();
    if (ImGui::BeginTabBar("oyun_sekmeler")) {
        if (ImGui::BeginTabItem("Oyun profilleri")) { games_tab_rules(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Oyun modu")) { games_tab_mode(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Oyun süresi")) { games_tab_time(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Mola")) { games_tab_break(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void App::games_tab_rules() {
    json& rules = opt_["games"];
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Oyunu ekleyin ve ne olacağını seçin: oyun öndeyken crosshair profili, göstergeler ve ekran rengi uygulanır; "
                       "başka pencereye geçince ya da oyun kapanınca eski haline döner. Oyun modu oyun çalıştığı sürece açık kalır.");
    ImGui::PopStyleColor();
    if (ImGui::Button("\xEE\x9C\x90  Çalışan programlardan oyun ekle")) { running_apps_ = windowed_apps(); ImGui::OpenPopup("oyun_sec"); }
    if (ImGui::BeginPopup("oyun_sec")) {
        if (running_apps_.empty()) ImGui::TextDisabled("Pencereli program bulunamadı.");
        for (auto& a : running_apps_)
            if (ImGui::Selectable(a.c_str())) {
                bool known = false;
                for (auto& r : rules) known |= _stricmp(r.value("exe", std::string()).c_str(), a.c_str()) == 0;
                if (!known) {
                    rules.push_back({{"exe", a}, {"name", a.substr(0, a.rfind('.'))}, {"crosshair_profile", ""}, {"crosshair", "keep"}, {"fps", "keep"},
                                     {"game_mode", false}, {"color", false}, {"brightness", 0}, {"contrast", 0}, {"gamma", 0}});
                    mark_dirty();
                }
            }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Oyun açıkken ekleyin; listede exe adıyla görünür.");
    if (game_rule_ >= 0) ImGui::TextColored(theme_good(), "%s", TF("Şu an uygulanan: %s", rules[game_rule_].value("name", std::string()).c_str()).c_str());
    ImGui::Spacing();
    int remove = -1;
    const std::vector<std::pair<std::string, std::string>> modes{{"keep", "Değiştirme"}, {"on", "Aç"}, {"off", "Kapat"}};
    for (int i = 0; i < int(rules.size()); i++) {
        json& r = rules[i];
        ImGui::PushID(i);
        if (ImGui::CollapsingHeader((r.value("name", std::string()) + "  (" + r.value("exe", std::string()) + ")###oyun").c_str())) {
            ImGui::Indent(10);
            ImGui::PushItemWidth(260);
            bool edited = edit_text("Ad", r, "name");
            std::vector<std::pair<std::string, std::string>> names{{"", "Değiştirme"}};
            for (auto& [name, s] : profiles_.items()) names.push_back({name, name});
            edited |= edit_choice("Crosshair profili", r, "crosshair_profile", names);
            edited |= edit_choice("Crosshair", r, "crosshair", modes);
            edited |= edit_choice("FPS göstergesi", r, "fps", modes);
            edited |= edit_bool("Oyun modu", r, "game_mode");
            ImGui::SameLine();
            ImGui::TextDisabled("(ayarları \"Oyun modu\" sekmesinde)");
            edited |= edit_bool("Ekran rengini değiştir", r, "color");
            if (r.value("color", false)) {
                bool look = false;
                look |= edit_int("Parlaklık", r, "brightness", -50, 50);
                look |= edit_int("Kontrast", r, "contrast", -50, 50);
                look |= edit_int("Gama", r, "gamma", -50, 50);
                if (look && game_rule_ == i) apply_gamma(r.value("brightness", 0), r.value("contrast", 0), r.value("gamma", 0));
                if (ImGui::Button("5 saniye dene")) {
                    apply_gamma(r.value("brightness", 0), r.value("contrast", 0), r.value("gamma", 0));
                    opt_["gamma_applied"] = true;
                    gamma_test_until_ = now() + 5;
                }
                ImGui::TextDisabled("Karanlık köşeleri görmek için gamayı ya da parlaklığı artırın. Bazı ekran kartları aşırı değerleri kabul etmez.");
                edited |= look;
            }
            ImGui::PopItemWidth();
            if (ImGui::Button("Bu oyunu kaldır")) remove = i;
            ImGui::Unindent(10);
            if (edited) mark_dirty();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        if (game_rule_ == remove) game_leave();
        rules.erase(remove);
        game_rule_ = -1;
        mark_dirty();
    }
}

void App::games_tab_mode() {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Oyun profilinde \"Oyun modu\" işaretli oyun açılınca uygulanır, oyun kapanınca geri alınır.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (edit_bool("Güç planını \"En yüksek performans\" yap", opt_, "gm_power")) mark_dirty();
    ImGui::TextDisabled("Dizüstünde işlemci ve ekran kartı tam hızda çalışır; oyun bitince önceki plana dönülür.");
    ImGui::Spacing();
    ImGui::SeparatorText("Oyun açılınca kapatılacak programlar");
    json& list = opt_["gm_close"];
    int remove = -1;
    for (int i = 0; i < int(list.size()); i++) {
        ImGui::PushID(i);
        ImGui::BulletText("%s", list[i].get<std::string>().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Kaldır")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) { list.erase(remove); mark_dirty(); }
    if (ImGui::Button("\xEE\x9C\x90  Çalışan programlardan ekle")) { running_apps_ = windowed_apps(); ImGui::OpenPopup("kapat_sec"); }
    if (ImGui::BeginPopup("kapat_sec")) {
        for (auto& a : running_apps_)
            if (ImGui::Selectable(a.c_str())) {
                bool known = false;
                for (auto& n : list) known |= n.get<std::string>() == a;
                if (!known) { list.push_back(a); mark_dirty(); }
            }
        ImGui::EndPopup();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Programlara normal kapanma isteği gönderilir, zorla sonlandırılmaz; kaydedilmemiş işi olan program size sorabilir. "
                       "Bazı programlar (örneğin Discord) kapanmak yerine saatin yanına küçülür.");
    ImGui::PopStyleColor();
    if (edit_bool("Oyun bitince bu programları yeniden aç", opt_, "gm_reopen")) mark_dirty();
    if (!game_mode_exe_.empty()) ImGui::TextColored(theme_good(), "%s", TF("Oyun modu şu an açık: %s", game_mode_exe_.c_str()).c_str());
}

void App::games_tab_time() {
    if (edit_bool("Ekranı kaplayan diğer programları da oyun say", opt_, "playtime_fullscreen")) mark_dirty();
    ImGui::TextDisabled("Kapalıysa yalnızca Oyun profilleri listesindeki oyunlar sayılır. Tarayıcı ve video oynatıcılar sayılmaz.");
    if (!playing_.empty()) ImGui::TextColored(theme_good(), "%s", TF("Şu an sayılıyor: %s", playing_.c_str()).c_str());
    ImGui::Spacing();
    if (playtime_.empty()) { ImGui::TextDisabled("Henüz oyun süresi yok."); return; }
    // Son 7 gün
    std::vector<std::string> days;
    for (int back = 6; back >= 0; back--) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
        u.QuadPart -= ULONGLONG(back) * 864000000000ULL;
        ft = {u.LowPart, u.HighPart};
        FILETIME local;
        FileTimeToLocalFileTime(&ft, &local);
        SYSTEMTIME st;
        FileTimeToSystemTime(&local, &st);
        char d[16];
        sprintf_s(d, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        days.push_back(d);
    }
    auto hours = [](int seconds) {
        char b[32];
        if (seconds < 3600) sprintf_s(b, "%d dk", seconds / 60);
        else sprintf_s(b, "%d sa %02d dk", seconds / 3600, seconds / 60 % 60);
        return std::string(b);
    };
    std::vector<std::pair<int, std::string>> order;     // En çok oynanan üstte
    for (auto& [name, g] : playtime_.items()) order.push_back({g.value("total", 0), name});
    std::sort(order.rbegin(), order.rend());
    if (ImGui::BeginTable("sureler", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Oyun");
        ImGui::TableSetupColumn("Bugün");
        ImGui::TableSetupColumn("Son 7 gün");
        ImGui::TableSetupColumn("Toplam");
        ImGui::TableHeadersRow();
        for (auto& [total, name] : order) {
            const json& g = playtime_[name];
            int week = 0;
            for (auto& d : days) week += g["days"].value(d, 0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hours(g["days"].value(days.back(), 0)).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hours(week).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(theme_accent(), "%s", hours(total).c_str());
        }
        ImGui::EndTable();
    }
    std::vector<float> per_day;
    for (auto& d : days) {
        int sum = 0;
        for (auto& [name, g] : playtime_.items()) sum += g["days"].value(d, 0);
        per_day.push_back(sum / 3600.f);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Son 7 gün (saat): %s ... %s", days.front().c_str(), days.back().c_str());
    ImGui::PlotHistogram("##gunler", per_day.data(), int(per_day.size()), 0, nullptr, 0, FLT_MAX, ImVec2(-1, 90));
    if (ImGui::Button("Süreleri sıfırla")) ImGui::OpenPopup("sure_sil");
    if (ImGui::BeginPopup("sure_sil")) {
        ImGui::TextUnformatted("Bütün oyun süreleri silinsin mi?");
        if (ImGui::Button("Sil##onay")) { playtime_ = json::object(); write_json(data_dir() + L"\\playtime.json", playtime_); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void App::games_tab_break() {
    if (edit_bool("Mola hatırlatıcısı", opt_, "break_on")) mark_dirty();
    ImGui::SetNextItemWidth(280);
    if (edit_int("Kaç dakikada bir (kesintisiz oyun)", opt_, "break_minutes", 10, 240)) mark_dirty();
    ImGui::TextDisabled("Süre doldukça ekranın üstünde kısa bir hatırlatma çıkar. 5 dakikadan uzun ara verince sayaç sıfırlanır.");
    if (break_play_ > 0) ImGui::TextUnformatted(TF("Kesintisiz oyun: %d dakika", int(break_play_ / 60)).c_str());
    if (ImGui::Button("Örnek hatırlatmayı göster")) show_alert(TF("Mola zamanı: %d dakikadır oynuyorsunuz. Gözlerinizi dinlendirin, biraz su için.", opt_.value("break_minutes", 60)));
}
// --- Yayın araçları: tuş göstergesi, çalan şarkı, Kayıt Stüdyosu bağlantısı ---

namespace {
const std::vector<std::pair<std::string, std::string>> kCorners = {
    {"top_left", "Sol üst"}, {"top_center", "Üst orta"}, {"top_right", "Sağ üst"},
    {"bottom_left", "Sol alt"}, {"bottom_center", "Alt orta"}, {"bottom_right", "Sağ alt"}};

void corner_place(const std::string& corner, int w, int h, int& x, int& y) {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int m = std::max(8, sh / 60);
    x = corner.find("left") != std::string::npos ? m : corner.find("right") != std::string::npos ? sw - w - m : (sw - w) / 2;
    y = corner.rfind("top", 0) == 0 ? m : sh - h - m;
}

// Tek harf ya da rakam -> sanal tuş kodu
int letter_key(const std::string& token) {
    if (token.size() != 1) return 0;
    const char c = char(toupper(token[0]));
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : 0;
}
}  // namespace

std::vector<KeyCap> App::key_caps() {
    std::vector<KeyCap> caps;
    auto add = [&](float x, float y, float w, float h, const std::string& label, int vk) {
        caps.push_back({x, y, w, h, label, key_down(vk)});
    };
    if (opt_.value("keys_layout", std::string("wasd")) == "arrows") {
        add(1, 0, 1, 1, "\xE2\x86\x91", VK_UP);
        add(0, 1, 1, 1, "\xE2\x86\x90", VK_LEFT);
        add(1, 1, 1, 1, "\xE2\x86\x93", VK_DOWN);
        add(2, 1, 1, 1, "\xE2\x86\x92", VK_RIGHT);
    } else {
        add(1, 0, 1, 1, "W", 'W');
        add(0, 1, 1, 1, "A", 'A');
        add(1, 1, 1, 1, "S", 'S');
        add(2, 1, 1, 1, "D", 'D');
    }
    float row = 2;
    if (opt_.value("keys_mods", true)) {
        add(0, 2, 1.5f, 1, "Shift", VK_SHIFT);
        add(1.5f, 2, 1.5f, 1, "Ctrl", VK_CONTROL);
        add(0, 3, 3, 1, T("Boşluk"), VK_SPACE);
        row = 4;
    }
    // Ek tuşlar: fare tuşları da varsa beşli, yoksa üçlü satırlar
    const std::string extra = opt_.value("keys_extra", std::string());
    const int per_row = opt_.value("keys_mouse", true) ? 5 : 3;
    int n = 0;
    for (size_t at = 0; at < extra.size();) {
        const size_t end = std::min(extra.find(' ', at), extra.size());
        const std::string token = extra.substr(at, end - at);
        at = end + 1;
        const int vk = letter_key(token);
        if (!vk || n >= 12) continue;
        add(float(n % per_row), row + float(n / per_row), 1, 1, std::string(1, char(vk)), vk);
        n++;
    }
    if (opt_.value("keys_mouse", true)) {
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;      // Solak ayarında fiziksel tuşlar yer değiştirir
        add(3.25f, 0, 0.875f, 1.5f, T("SOL"), swapped ? VK_RBUTTON : VK_LBUTTON);
        add(4.125f, 0, 0.875f, 1.5f, T("SAĞ"), swapped ? VK_LBUTTON : VK_RBUTTON);
        add(3.25f, 1.5f, 1.75f, 0.5f, T("ORTA"), VK_MBUTTON);
    }
    return caps;
}

int rs_command(int command) {
    HWND rs = FindWindowW(L"KayitStudyosu", nullptr);
    if (!rs) return -1;
    static const UINT message = RegisterWindowMessageW(L"KayitStudyosuKomut");
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(rs, message, command, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 400, &result) || !(result & 0x100)) return -1;
    return int(result & 0xff);
}

int App::rs_send(int command) { return rs_command(command); }

void App::rs_replay_save() {
    const int state = rs_send(0);
    if (state < 0) { show_alert(T("Kayıt Stüdyosu açık değil")); return; }
    if (!(state & 4)) { show_alert(T("Kayıt Stüdyosu'nda tekrar arabelleği kapalı")); return; }
    rs_send(3);
    show_alert(T("Tekrar kaydedildi"));
}

void App::rs_tick(double t) {
    if (t < rs_next_) return;
    rs_next_ = t + 1;
    const bool want_badge = opt_.value("rs_badge", true), want_auto = opt_.value("rs_replay_auto", false);
    if (!want_badge && !want_auto && page_ != "extras") { rs_state_ = -1; return; }
    rs_state_ = rs_send(0);
    // Oyun başlayınca tekrar arabelleği açılır, oyun bittikten 10 sn sonra kapanır (yalnızca bizim açtığımız)
    if (want_auto) {
        if (!playing_.empty()) {
            rs_game_gone_ = 0;
            if (rs_state_ < 0 && opt_.value("rs_launch", true) && t - rs_launched_at_ > 30) {
                rs_launched_at_ = t;
                const std::wstring exe = exe_dir() + L"\\Recording Studio.exe";
                ShellExecuteW(nullptr, L"open", exe.c_str(), L"--baslangic", exe_dir().c_str(), SW_HIDE);
            }
            if (rs_state_ >= 0 && !(rs_state_ & 4) && !rs_replay_started_) {
                rs_send(1);
                rs_replay_started_ = true;
            }
        } else if (rs_replay_started_) {
            if (!rs_game_gone_) rs_game_gone_ = t;
            if (t - rs_game_gone_ >= 10) {
                if (rs_state_ >= 0 && (rs_state_ & 4)) rs_send(2);
                rs_replay_started_ = false;
                rs_game_gone_ = 0;
            }
        }
    }
}

void App::extras_tick(double t) {
    const float k = GetSystemMetrics(SM_CYSCREEN) / 1080.f;
    // Tuş göstergesi: yalnızca bir tuşun durumu değişince yeniden çizilir
    if (opt_.value("keys_on", false)) {
        const auto caps = key_caps();
        std::string sig = opt_.value("keys_corner", std::string()) + std::to_string(GetSystemMetrics(SM_CXSCREEN));
        for (auto& c : caps) sig += c.label + (c.down ? "1" : "0");
        sig += std::to_string(opt_.value("keys_size", 44)) + opt_.value("keys_color", std::string()) + opt_.value("keys_active", std::string()) +
               std::to_string(opt_.value("keys_bg", 55)) + (opt_.value("keys_hide", false) ? "h" : "");
        if (sig != keys_sig_) {
            keys_sig_ = sig;
            const Bitmap b = draw_keys(caps, std::clamp(opt_.value("keys_size", 44), 20, 120) * k, parse_color(opt_.value("keys_color", std::string("#ffffff"))),
                                       parse_color(opt_.value("keys_active", std::string("#2fbf71"))), std::clamp(opt_.value("keys_bg", 55), 0, 100) / 100.f);
            int x, y;
            corner_place(opt_.value("keys_corner", std::string("bottom_left")), b.w, b.h, x, y);
            keys_overlay_.hide_from_capture(opt_.value("keys_hide", false));
            keys_overlay_.show(b, 1, x, y);
        }
    } else if (keys_overlay_.visible()) {
        keys_overlay_.hide();
        keys_sig_.clear();
    }

    // Çalan şarkı
    const bool song_wanted = opt_.value("song_on", false) || page_ == "extras";
    if (song_wanted && !media_.running()) media_.start();
    if (!song_wanted && media_.running()) media_.stop();
    if (media_.running() && t >= song_next_) {
        song_next_ = t + 0.5;
        song_ = media_.get();
    }
    std::string text;
    if (opt_.value("song_on", false) && !song_.title.empty() && (song_.playing || opt_.value("song_paused", false)))
        text = "\xE2\x99\xAA  " + (song_.artist.empty() ? song_.title : song_.artist + " \xE2\x80\x93 " + song_.title);
    const std::string song_sig = text + opt_.value("song_corner", std::string()) + std::to_string(opt_.value("song_size", 20)) +
                                 opt_.value("song_color", std::string()) + (opt_.value("song_bg", true) ? "b" : "") + (opt_.value("song_hide", false) ? "h" : "");
    if (song_sig != song_sig_) {
        song_sig_ = song_sig;
        if (text.empty()) {
            song_overlay_.hide();
        } else {
            const json s = {{"text", text}, {"font", "Segoe UI"}, {"bold", true}, {"font_size", std::clamp(opt_.value("song_size", 20), 10, 60)},
                            {"color", opt_.value("song_color", std::string("#ffffff"))}, {"outline", !opt_.value("song_bg", true)}, {"outline_thickness", 2},
                            {"background", opt_.value("song_bg", true)}, {"bg_color", "#000000"}, {"bg_opacity", 55}};
            const Bitmap b = draw_text(s, k);
            int x, y;
            corner_place(opt_.value("song_corner", std::string("top_left")), b.w, b.h, x, y);
            song_overlay_.hide_from_capture(opt_.value("song_hide", false));
            song_overlay_.show(b, 1, x, y);
        }
    }

    // Kayıt Stüdyosu: durum rozeti
    rs_tick(t);
    std::string badge;
    if (opt_.value("rs_badge", true) && rs_state_ > 0) {
        if (rs_state_ & 1) badge += (rs_state_ & 8) ? T("\xE2\x9D\x9A\xE2\x9D\x9A REC") : "\xE2\x97\x8F REC";
        if (rs_state_ & 2) badge += std::string(badge.empty() ? "" : "   ") + "\xE2\x97\x8F " + T("CANLI");
    }
    const std::string badge_sig = badge + opt_.value("rs_badge_corner", std::string()) + (opt_.value("rs_badge_hide", true) ? "h" : "");
    if (badge_sig != badge_sig_) {
        badge_sig_ = badge_sig;
        if (badge.empty()) {
            badge_overlay_.hide();
        } else {
            const json s = {{"text", badge}, {"font", "Segoe UI"}, {"bold", true}, {"font_size", 18}, {"color", "#ff4545"}, {"outline", false},
                            {"background", true}, {"bg_color", "#000000"}, {"bg_opacity", 60}};
            const Bitmap b = draw_text(s, k);
            int x, y;
            corner_place(opt_.value("rs_badge_corner", std::string("top_right")), b.w, b.h, x, y);
            badge_overlay_.hide_from_capture(opt_.value("rs_badge_hide", true));
            badge_overlay_.show(b, 1, x, y);
        }
    }
}

void App::page_extras() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Yayın araçları");
    ImGui::PopFont();
    ImGui::TextDisabled("Ekranda basılan tuşlar, çalan şarkı ve Kayıt Stüdyosu ile birlikte çalışma");
    ImGui::Spacing();
    if (ImGui::BeginTabBar("ekler")) {
        if (ImGui::BeginTabItem("Tuş göstergesi")) { extras_tab_keys(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Çalan şarkı")) { extras_tab_song(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Kayıt Stüdyosu")) { extras_tab_rs(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void App::extras_tab_keys() {
    if (edit_bool("Ekranda göster##tus", opt_, "keys_on")) mark_dirty();
    ImGui::TextDisabled("Yayında izleyiciler hangi tuşlara bastığınızı görür. Tuşlar kaydedilmez, yalnızca o anki durum gösterilir.");
    ImGui::Spacing();
    ImGui::BeginChild("tus_ayar", ImVec2(420, 0));
    ImGui::PushItemWidth(220);
    bool e = edit_choice("Yön tuşları", opt_, "keys_layout", {{"wasd", "W A S D"}, {"arrows", "Ok tuşları"}});
    e |= edit_bool("Shift, Ctrl ve boşluk", opt_, "keys_mods");
    e |= edit_bool("Fare tuşları", opt_, "keys_mouse");
    e |= edit_text("Ek tuşlar", opt_, "keys_extra");
    ImGui::TextDisabled("Boşlukla ayrılmış harf ya da rakamlar (en çok 12), ör. Q E R F 1 2");
    e |= edit_choice("Konum##tus", opt_, "keys_corner", kCorners);
    e |= edit_int("Tuş boyutu", opt_, "keys_size", 20, 120);
    e |= edit_color("Yazı rengi", opt_, "keys_color");
    e |= edit_color("Basılı tuş rengi", opt_, "keys_active");
    e |= edit_int("Zemin opaklığı", opt_, "keys_bg", 0, 100);
    e |= edit_bool("Kayıt ve yayında gizle##tus", opt_, "keys_hide");
    ImGui::PopItemWidth();
    if (e) mark_dirty();
    ImGui::EndChild();
    ImGui::SameLine();
    // Önizleme: canlı, tuşlara basınca yanar
    const auto caps = key_caps();
    std::string sig = std::to_string(opt_.value("keys_size", 44)) + opt_.value("keys_color", std::string()) + opt_.value("keys_active", std::string()) +
                      std::to_string(opt_.value("keys_bg", 55));
    for (auto& c : caps) sig += c.label + (c.down ? "1" : "0");
    if (sig != keys_preview_sig_) {
        keys_preview_sig_ = sig;
        const Bitmap b = draw_keys(caps, float(std::clamp(opt_.value("keys_size", 44), 20, 120)), parse_color(opt_.value("keys_color", std::string("#ffffff"))),
                                   parse_color(opt_.value("keys_active", std::string("#2fbf71"))), std::clamp(opt_.value("keys_bg", 55), 0, 100) / 100.f);
        keys_preview_w_ = b.w;
        keys_preview_h_ = b.h;
        keys_srv_ = make_texture(b);
    }
    const ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    const ImVec2 box(avail.x, std::min(avail.y, 360.f));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), IM_COL32(70, 90, 110, 255), 6.f);
    if (keys_srv_) {
        const float fit = std::min({1.f, (box.x - 20) / std::max(1, keys_preview_w_), (box.y - 20) / std::max(1, keys_preview_h_)});
        const float w = keys_preview_w_ * fit, h = keys_preview_h_ * fit;
        const ImVec2 a(floorf(p.x + (box.x - w) / 2), floorf(p.y + (box.y - h) / 2));
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)keys_srv_.Get(), a, ImVec2(a.x + w, a.y + h));
    }
    ImGui::Dummy(box);
}

void App::extras_tab_song() {
    if (edit_bool("Ekranda göster##sarki", opt_, "song_on")) mark_dirty();
    ImGui::TextDisabled("Spotify, tarayıcı ya da medya oynatıcıda çalan şarkının adı. Windows'un medya denetimini kullanan her program olur.");
    ImGui::Spacing();
    ImGui::SeparatorText("Şu an");
    if (song_.title.empty()) ImGui::TextDisabled("Çalan bir şey bulunamadı");
    else ImGui::Text("%s%s%s  (%s)", song_.artist.c_str(), song_.artist.empty() ? "" : " - ", song_.title.c_str(), T(song_.playing ? "çalıyor" : "duraklatıldı"));
    ImGui::SeparatorText("Görünüm");
    ImGui::PushItemWidth(220);
    bool e = edit_choice("Konum##sarki", opt_, "song_corner", kCorners);
    e |= edit_int("Yazı boyutu", opt_, "song_size", 10, 60);
    e |= edit_color("Yazı rengi##sarki", opt_, "song_color");
    e |= edit_bool("Koyu zemin", opt_, "song_bg");
    e |= edit_bool("Duraklatılınca da göster", opt_, "song_paused");
    e |= edit_bool("Kayıt ve yayında gizle##sarki", opt_, "song_hide");
    ImGui::PopItemWidth();
    if (e) mark_dirty();
}

void App::extras_tab_rs() {
    ImGui::SeparatorText("Durum");
    if (rs_state_ < 0) {
        ImGui::TextDisabled("Kayıt Stüdyosu açık değil");
    } else {
        ImGui::Text("Kayıt: %s   Yayın: %s   Tekrar arabelleği: %s", T(rs_state_ & 1 ? ((rs_state_ & 8) ? "duraklatıldı" : "sürüyor") : "kapalı"),
                    T(rs_state_ & 2 ? "açık" : "kapalı"), T(rs_state_ & 4 ? "açık" : "kapalı"));
    }
    ImGui::SeparatorText("Durum rozeti");
    ImGui::PushItemWidth(220);
    bool e = edit_bool("Kayıt ya da yayın sürerken ekranda REC / CANLI yaz", opt_, "rs_badge");
    e |= edit_choice("Konum##rozet", opt_, "rs_badge_corner", kCorners);
    e |= edit_bool("Kayıt ve yayında gizle##rozet", opt_, "rs_badge_hide");
    ImGui::SeparatorText("Tekrar arabelleği");
    e |= edit_bool("Oyun başlayınca tekrar arabelleğini aç", opt_, "rs_replay_auto");
    ImGui::TextDisabled("Oyunlar listesindeki ya da ekranı kaplayan oyun öndeyken açılır, oyun bittikten 10 sn sonra kapanır.");
    e |= edit_bool("Kayıt Stüdyosu kapalıysa arka planda aç", opt_, "rs_launch");
    ImGui::PopItemWidth();
    if (e) mark_dirty();
    ImGui::Spacing();
    std::string key;
    {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        key = hotkeys_.count("replay_save") && hotkeys_["replay_save"].vk ? key_name(hotkeys_["replay_save"]) : T("atanmamış");
    }
    ImGui::Text("Oyun içinde tekrarı kaydetme kısayolu: %s", key.c_str());
    if (ImGui::Button("Kısayolu ayarla")) page_ = "hotkeys";
    ImGui::SameLine();
    if (ImGui::Button("Şimdi tekrarı kaydet")) rs_replay_save();
}

// --- Nişan: hassasiyet çevirici, DPI ölçümü, nişan antrenmanı ---
namespace {
// Oyunların fare sayımı başına dönüş açısı (derece): hassasiyet 1'de bir sayımlık hareket
struct SensGame { const char* key; const char* name; double yaw; };
const SensGame kSensGames[] = {
    {"cs2", "Counter-Strike 2", 0.022}, {"valorant", "Valorant", 0.07}, {"apex", "Apex Legends", 0.022},
    {"overwatch", "Overwatch 2", 0.0066}, {"cod", "Call of Duty", 0.0066}, {"source", "Source oyunları (TF2, HL2)", 0.022},
    {"quake", "Quake Champions", 0.022}, {"custom", "Özel (derece / sayım)", 0}};
const int kCommonDpi[] = {400, 800, 1000, 1200, 1600, 2000, 2400, 3200, 6400};

std::string today_text() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char day[32];
    sprintf_s(day, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return day;
}
}  // namespace

void App::on_raw_input(HRAWINPUT input) {
    if (!dpi_drag_) return;
    RAWINPUT ri{};
    UINT size = sizeof(ri);
    if (GetRawInputData(input, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE || (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
    dpi_dx_ += ri.data.mouse.lLastX;
    dpi_dy_ += ri.data.mouse.lLastY;
}

void App::page_aim() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Nişan");
    ImGui::PopFont();
    ImGui::TextDisabled("Oyunlar arası hassasiyet, farenin gerçek DPI değeri ve nişan antrenmanı");
    ImGui::Spacing();
    if (ImGui::BeginTabBar("nisan")) {
        if (ImGui::BeginTabItem("Hassasiyet çevirici")) { aim_tab_sens(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("DPI ölçümü")) { aim_tab_dpi(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Nişan antrenmanı")) { aim_tab_train(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void App::aim_tab_sens() {
    auto number = [&](const char* label, const char* key, double lo, double hi, const char* fmt) {
        double v = opt_.value(key, 0.0);
        if (ImGui::InputDouble(T(label), &v, 0, 0, fmt)) { opt_[key] = std::clamp(v, lo, hi); mark_dirty(); }
        return std::clamp(v, lo, hi);
    };
    auto game_combo = [&](const char* label, const char* key, const char* yaw_key) {
        const std::string current = opt_.value(key, std::string("cs2"));
        const SensGame* g = &kSensGames[0];
        for (auto& x : kSensGames) if (current == x.key) g = &x;
        if (ImGui::BeginCombo(label, g->name)) {
            for (auto& x : kSensGames)
                if (ImGui::Selectable(x.name, g == &x)) { opt_[key] = x.key; mark_dirty(); g = &x; }
            ImGui::EndCombo();
        }
        if (g->yaw > 0) return g->yaw;
        ImGui::PushID(key);
        const double yaw = number("Derece / sayım", yaw_key, 0.0001, 10, "%.5f");
        ImGui::PopID();
        return yaw;
    };
    ImGui::PushItemWidth(260);
    ImGui::SeparatorText("Oyundan oyuna");
    const double yaw_from = game_combo("Şu anki oyun", "sens_from", "sens_yaw_from");
    const double sens = number("Hassasiyet", "sens_value", 0.0001, 1000, "%.4f");
    const int dpi = std::clamp(opt_.value("sens_dpi", 800), 50, 64000);
    {
        int v = dpi;
        if (ImGui::InputInt(T("Fare DPI"), &v, 50, 400)) { opt_["sens_dpi"] = std::clamp(v, 50, 64000); mark_dirty(); }
    }
    const double yaw_to = game_combo("Hedef oyun", "sens_to", "sens_yaw_to");
    ImGui::PopItemWidth();
    ImGui::Spacing();
    const double result = yaw_to > 0 ? sens * yaw_from / yaw_to : 0;
    const double cm360 = sens > 0 && yaw_from > 0 ? 360.0 / (sens * yaw_from * dpi) * 2.54 : 0;
    char text[64];
    sprintf_s(text, "%.4g", result);
    ImGui::PushFont(nullptr, 30.f);
    ImGui::TextColored(theme_accent(), "%s", text);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (ImGui::SmallButton("Kopyala")) { ImGui::SetClipboardText(text); note("Hassasiyet panoya kopyalandı"); }
    ImGui::Text("Hedef oyunda bu hassasiyetle aynı hissi alırsınız (aynı DPI ile).");
    ImGui::Text("Bir tam tur (360°) için fareyi %.1f cm (%.1f inç) kaydırırsınız.", cm360, cm360 / 2.54);
    ImGui::TextDisabled("Görüş açısı (FOV) farklı oyunlarda ekrandaki hız biraz farklı hissedilebilir; tur mesafesi aynı kalır.");

    ImGui::SeparatorText("DPI değiştirince");
    ImGui::PushItemWidth(260);
    int nd = std::clamp(opt_.value("sens_new_dpi", 1600), 50, 64000);
    if (ImGui::InputInt(T("Yeni DPI"), &nd, 50, 400)) { opt_["sens_new_dpi"] = std::clamp(nd, 50, 64000); mark_dirty(); }
    ImGui::PopItemWidth();
    const double same = sens * dpi / std::max(nd, 1);
    ImGui::Text("Şu anki oyunda aynı hissi korumak için hassasiyet: %.4g", same);
    ImGui::TextDisabled("Ham giriş kullanan oyunları Windows imleç hızı etkilemez. Masaüstünde de aynı his için Windows'ta işaretçi hızını düşürün.");
}

void App::aim_tab_dpi() {
    ImGui::TextWrapped("Farenin gerçek DPI değeri, yazılımda yazandan farklı olabilir. Fareyi masada bir cetvel boyunca düz kaydırarak ölçün:");
    ImGui::BulletText("Aşağıdaki alanda sol tuşu basılı tutun.");
    ImGui::BulletText("Fareyi cetvel boyunca, yazan mesafe kadar sağa kaydırın.");
    ImGui::BulletText("Tuşu bırakın. Daha doğru sonuç için birkaç kez tekrarlayın.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(200);
    double cm = opt_.value("dpi_distance", 10.0);
    if (ImGui::InputDouble(T("Mesafe (cm)"), &cm, 1, 5, "%.1f")) { opt_["dpi_distance"] = std::clamp(cm, 1.0, 100.0); mark_dirty(); }
    cm = std::clamp(cm, 1.0, 100.0);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 170);
    ImGui::InvisibleButton("dpi_alan", size);
    if (ImGui::IsItemActivated()) {
        RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd};    // Fare, ham giriş
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        dpi_drag_ = true;
        dpi_dx_ = dpi_dy_ = 0;
    }
    if (ImGui::IsItemDeactivated() && dpi_drag_) {
        dpi_drag_ = false;
        RAWINPUTDEVICE rid{0x01, 0x02, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        const double counts = sqrt(double(dpi_dx_) * dpi_dx_ + double(dpi_dy_) * dpi_dy_);
        if (counts > 20) dpi_result_ = counts / (cm / 2.54);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 border = ImGui::GetColorU32(dpi_drag_ ? ImGuiCol_ButtonActive : ImGuiCol_Border);
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), 6.f);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), border, 6.f, ImDrawFlags_None, 2.f);
    char line[128];
    if (dpi_drag_) sprintf_s(line, "%s  %lld", T("Ölçülüyor... sayım:"), (long long)sqrt(double(dpi_dx_) * dpi_dx_ + double(dpi_dy_) * dpi_dy_));
    else sprintf_s(line, "%s", T("Sol tuşu burada basılı tutup fareyi kaydırın"));
    const ImVec2 ts = ImGui::CalcTextSize(line);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_Text), line);
    ImGui::Spacing();
    if (dpi_result_ > 0) {
        int nearest = kCommonDpi[0];
        for (int d : kCommonDpi) if (fabs(d - dpi_result_) < fabs(nearest - dpi_result_)) nearest = d;
        ImGui::PushFont(nullptr, 26.f);
        ImGui::TextColored(theme_accent(), "%.0f DPI", dpi_result_);
        ImGui::PopFont();
        ImGui::Text("En yakın yaygın değer: %d. Fark %%%.0f.", nearest, fabs(dpi_result_ - nearest) / nearest * 100);
        if (ImGui::Button("Hassasiyet çeviricide kullan")) { opt_["sens_dpi"] = int(lround(dpi_result_)); mark_dirty(); note("DPI hassasiyet çeviriciye yazıldı"); }
    }
}

void App::aim_spawn(int index, float w, float h) {
    const float r = float(std::clamp(opt_.value("aim_size", 26), 6, 80));
    Target t{};
    for (int attempt = 0; attempt < 20; attempt++) {
        t.x = r + rnd() * std::max(1.f, w - 2 * r);
        t.y = r + rnd() * std::max(1.f, h - 2 * r - 30) + 30;     // Üstteki yazı satırının altında
        bool clear = true;
        for (int i = 0; i < int(targets_.size()); i++)
            if (i != index && hypotf(targets_[i].x - t.x, targets_[i].y - t.y) < r * 2.5f) clear = false;
        if (clear) break;
    }
    const float angle = rnd() * 6.2832f, speed = 120 + rnd() * 160;
    t.vx = cosf(angle) * speed;
    t.vy = sinf(angle) * speed;
    t.born = now();
    if (index < int(targets_.size())) targets_[index] = t;
    else targets_.push_back(t);
}

void App::aim_finish() {
    aim_on_ = false;
    const std::string mode = opt_.value("aim_mode", std::string("click"));
    const int seconds = std::clamp(opt_.value("aim_seconds", 30), 10, 120), size = std::clamp(opt_.value("aim_size", 26), 6, 80);
    json r = {{"mode", mode}, {"date", today_text()}, {"seconds", seconds}, {"size", size}};
    if (mode == "track") {
        const double pct = std::min(100.0, aim_on_time_ / seconds * 100);
        r["accuracy"] = pct;
        r["score"] = int(lround(pct * 100));
    } else {
        const double acc = aim_shots_ ? 100.0 * aim_hits_ / aim_shots_ : 0;
        r["hits"] = aim_hits_;
        r["shots"] = aim_shots_;
        r["accuracy"] = acc;
        r["ms"] = mode == "click" ? (aim_react_n_ ? aim_react_sum_ / aim_react_n_ * 1000 : 0) : (aim_hits_ ? seconds * 1000.0 / aim_hits_ : 0);
        r["score"] = int(lround(aim_hits_ * acc));
    }
    // Rekor, aynı tür, süre ve hedef boyutuyla karşılaştırılır
    const std::string key = mode + "_" + std::to_string(seconds) + "_" + std::to_string(size);
    json& best = aim_["best"][key];
    r["record"] = !best.is_object() || r["score"].get<int>() > best.value("score", 0);
    if (r["record"].get<bool>()) best = r;
    json& history = aim_["history"];
    if (!history.is_array()) history = json::array();
    history.push_back(r);
    while (history.size() > 200) history.erase(history.begin());
    write_json(data_dir() + L"\\aim.json", aim_);
    aim_result_ = r;
}

void App::aim_arena(float w, float h) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("arena", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + h), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(18, 20, 26, 255), 6.f);
    const ImVec2 m(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
    const double t = now();
    const std::string mode = opt_.value("aim_mode", std::string("click"));
    const int seconds = std::clamp(opt_.value("aim_seconds", 30), 10, 120);
    const float r = float(std::clamp(opt_.value("aim_size", 26), 6, 80));
    const uint32_t rgb = parse_color(opt_.value("aim_color", std::string("#ff4d4d")));
    const ImU32 color = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
    auto centered = [&](float y, const char* text, ImU32 c, float font = 0) {
        ImFont* f = ImGui::GetFont();
        const float fs = font > 0 ? font : ImGui::GetFontSize();
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, text);
        dl->AddText(f, fs, ImVec2(p0.x + (w - ts.x) / 2, p0.y + y), c, text);
    };
    if (aim_on_ && t - aim_last_ > 0.5) aim_on_ = false;      // Sayfadan çıkıldıysa yarım tur sayılmaz
    aim_last_ = t;
    if (!aim_on_) {
        const ImVec2 c(p0.x + w / 2, p0.y + h / 2);
        const bool over = hypotf(io.MousePos.x - c.x, io.MousePos.y - c.y) <= 46;
        dl->AddCircleFilled(c, 46, over ? IM_COL32(60, 180, 110, 255) : IM_COL32(45, 140, 90, 255), 48);
        centered(h / 2 - 9, T("Başla"), IM_COL32(255, 255, 255, 255), 18.f);
        centered(h / 2 + 60, T("Başlamak için yeşil daireye tıklayın. Esc turu bitirir."), IM_COL32(170, 175, 185, 255));
        if (aim_result_.is_object()) {
            char text[160];
            const double acc = aim_result_.value("accuracy", 0.0);
            if (aim_result_.value("mode", std::string()) == "track") sprintf_s(text, T("Hedefte kalma: %%%.1f"), acc);
            else sprintf_s(text, T("İsabet: %d   İsabet oranı: %%%.0f   Ortalama: %.0f ms"), aim_result_.value("hits", 0), acc, aim_result_.value("ms", 0.0));
            centered(40, text, IM_COL32(235, 235, 240, 255), 20.f);
            char score[96];
            sprintf_s(score, T("Puan: %d"), aim_result_.value("score", 0));
            centered(72, score, ImGui::GetColorU32(theme_accent()), 26.f);
            if (aim_result_.value("record", false)) centered(108, T("Yeni rekor!"), IM_COL32(255, 200, 60, 255), 18.f);
        }
        if (hovered && over && ImGui::IsMouseClicked(0)) {
            aim_on_ = true;
            aim_start_ = t;
            aim_hits_ = aim_shots_ = aim_react_n_ = 0;
            aim_react_sum_ = aim_on_time_ = 0;
            aim_result_ = nullptr;
            rng_ = unsigned(GetTickCount64());
            targets_.clear();
            const int count = mode == "multi" ? std::clamp(opt_.value("aim_targets", 3), 2, 8) : 1;
            for (int i = 0; i < count; i++) aim_spawn(i, w, h);
        }
        dl->PopClipRect();
        return;
    }
    const double left = seconds - (t - aim_start_);
    if (left <= 0) { aim_finish(); dl->PopClipRect(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { aim_on_ = false; dl->PopClipRect(); return; }
    const float dt = std::min(io.DeltaTime, 0.05f);
    if (mode == "track") {
        Target& g = targets_[0];
        if (t - g.born > 0.8 + rnd() * 0.8) {     // Ara sıra yön değiştirir
            const float angle = rnd() * 6.2832f, speed = 120 + rnd() * 200;
            g.vx = cosf(angle) * speed;
            g.vy = sinf(angle) * speed;
            g.born = t;
        }
        g.x += g.vx * dt;
        g.y += g.vy * dt;
        if (g.x < r) { g.x = r; g.vx = fabsf(g.vx); }
        if (g.x > w - r) { g.x = w - r; g.vx = -fabsf(g.vx); }
        if (g.y < r + 30) { g.y = r + 30; g.vy = fabsf(g.vy); }
        if (g.y > h - r) { g.y = h - r; g.vy = -fabsf(g.vy); }
        if (hovered && hypotf(m.x - g.x, m.y - g.y) <= r) aim_on_time_ += dt;
    } else if (hovered && ImGui::IsMouseClicked(0)) {
        aim_shots_++;
        for (int i = 0; i < int(targets_.size()); i++) {
            if (hypotf(m.x - targets_[i].x, m.y - targets_[i].y) > r) continue;
            aim_hits_++;
            aim_react_sum_ += t - targets_[i].born;
            aim_react_n_++;
            aim_spawn(i, w, h);
            break;
        }
    }
    for (auto& g : targets_) {
        const bool on = mode == "track" && hovered && hypotf(m.x - g.x, m.y - g.y) <= r;
        dl->AddCircleFilled(ImVec2(p0.x + g.x, p0.y + g.y), r, on ? IM_COL32(80, 220, 120, 255) : color, 40);
        dl->AddCircle(ImVec2(p0.x + g.x, p0.y + g.y), r, IM_COL32(255, 255, 255, 120), 40, 1.5f);
    }
    char hud[160];
    if (mode == "track") sprintf_s(hud, T("Süre: %.0f   Hedefte: %%%.0f"), ceil(left), (t - aim_start_) > 0 ? aim_on_time_ / (t - aim_start_) * 100 : 0.0);
    else sprintf_s(hud, T("Süre: %.0f   İsabet: %d   Iska: %d"), ceil(left), aim_hits_, aim_shots_ - aim_hits_);
    centered(8, hud, IM_COL32(230, 230, 235, 255));
    if (hovered) {      // İmleç yerine küçük artı
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        const ImVec2 c = io.MousePos;
        dl->AddLine(ImVec2(c.x - 7, c.y), ImVec2(c.x + 8, c.y), IM_COL32(255, 255, 255, 255), 2.f);
        dl->AddLine(ImVec2(c.x, c.y - 7), ImVec2(c.x, c.y + 8), IM_COL32(255, 255, 255, 255), 2.f);
    }
    dl->PopClipRect();
}

void App::aim_tab_train() {
    ImGui::BeginChild("aim_ayar", ImVec2(270, 0));
    ImGui::BeginDisabled(aim_on_);
    ImGui::PushItemWidth(150);
    ImGui::SeparatorText("Tür");
    const std::string mode = opt_.value("aim_mode", std::string("click"));
    for (auto [key, name] : {std::pair<const char*, const char*>{"click", "Tek hedef"}, {"multi", "Çoklu hedef"}, {"track", "Takip"}})
        if (ImGui::RadioButton(name, mode == key)) { opt_["aim_mode"] = key; mark_dirty(); aim_result_ = nullptr; }
    ImGui::TextDisabled(mode == "click" ? "Hedef tek tek çıkar: tepki süresi ölçülür." :
                        mode == "multi" ? "Aynı anda birkaç hedef: hızlı geçiş." : "Hareketli hedefi imleçle izleyin.");
    ImGui::SeparatorText("Ayarlar");
    if (edit_int("Süre (sn)", opt_, "aim_seconds", 10, 120)) mark_dirty();
    if (edit_int("Hedef boyutu", opt_, "aim_size", 6, 80)) mark_dirty();
    if (mode == "multi" && edit_int("Hedef sayısı", opt_, "aim_targets", 2, 8)) mark_dirty();
    if (edit_color("Hedef rengi", opt_, "aim_color")) mark_dirty();
    ImGui::PopItemWidth();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Rekor");
    const int seconds = std::clamp(opt_.value("aim_seconds", 30), 10, 120), size = std::clamp(opt_.value("aim_size", 26), 6, 80);
    const std::string key = mode + "_" + std::to_string(seconds) + "_" + std::to_string(size);
    const json best = aim_.contains("best") && aim_["best"].contains(key) ? aim_["best"][key] : json();
    if (best.is_object()) {
        ImGui::Text("Puan: %d", best.value("score", 0));
        if (mode == "track") ImGui::Text("Hedefte kalma: %%%.1f", best.value("accuracy", 0.0));
        else ImGui::Text("İsabet %d, oran %%%.0f", best.value("hits", 0), best.value("accuracy", 0.0));
        ImGui::TextDisabled("%s", best.value("date", std::string()).c_str());
    } else {
        ImGui::TextDisabled("Bu ayarlarla henüz tur yok");
    }
    // Aynı ayarlardaki son turların puanı
    std::vector<float> scores;
    if (aim_.contains("history"))
        for (auto& h : aim_["history"])
            if (h.value("mode", std::string()) == mode && h.value("seconds", 0) == seconds && h.value("size", 0) == size)
                scores.push_back(float(h.value("score", 0)));
    if (scores.size() > 30) scores.erase(scores.begin(), scores.end() - 30);
    if (scores.size() >= 2) {
        ImGui::SeparatorText("Gelişim");
        ImGui::PlotLines("##gelisim", scores.data(), int(scores.size()), 0, nullptr, 0, FLT_MAX, ImVec2(-1, 70));
        ImGui::TextDisabled("Son %d tur", int(scores.size()));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    aim_arena(avail.x, std::max(200.f, avail.y));
}

// --- Sayfalar ---
void App::sidebar() {
    ImGui::PushFont(nullptr, 22.f);
    ImGui::TextUnformatted(kAppName);
    ImGui::PopFont();
    ImGui::TextDisabled("Crosshair ve FPS göstergesi");
    ImGui::Spacing(); ImGui::Spacing();
    struct Item { const char* key; const char* name; const char* icon; };
    for (auto it : {Item{"crosshair", "Crosshair", ICON_CROSSHAIR}, Item{"fps", "FPS Ölçer", ICON_FPS}, Item{"bench", "Performans testi", ICON_BENCH},
                    Item{"games", "Oyunlar", ICON_GAMES}, Item{"aim", "Nişan", ICON_AIM}, Item{"extras", "Yayın araçları", ICON_EXTRAS},
                    Item{"hotkeys", "Kısayollar", ICON_KEYS}, Item{"settings", "Ayarlar", ICON_SETTINGS}, Item{"help", "Yardım", ICON_HELP}})
        if (nav_button(it.icon, it.name, it.key, page_ == it.key)) page_ = it.key;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 36 - ImGui::GetStyle().ItemSpacing.y);
    if (nav_button(ICON_EXIT, "Çıkış", "cikis", false)) want_exit_ = true;
}

void App::apply_crosshair(const json& s) {
    crosshair_ = s;
    mark_dirty();
    update_crosshair();
}

void App::profiles_panel(float height) {
    ImGui::SeparatorText("Profiller");
    ImGui::BeginChild("profil_liste", ImVec2(0, height - 150), ImGuiChildFlags_Borders);
    for (auto& [name, s] : profiles_.items()) {
        const bool active = s == crosshair_;
        if (ImGui::Selectable(name.c_str(), active)) {
            strncpy_s(profile_name_, name.c_str(), _TRUNCATE);
            apply_crosshair(json(s));
        }
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(-82);
    const bool enter = ImGui::InputTextWithHint("##profil", "Profil adı", profile_name_, sizeof(profile_name_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const std::string name = profile_name_;
    const bool has_name = name.find_first_not_of(' ') != std::string::npos;
    ImGui::BeginDisabled(!has_name);
    if ((ImGui::Button("Kaydet", ImVec2(-1, 0)) || enter) && has_name) {
        if (profiles_.contains(name)) {
            pending_action_ = "overwrite";
            pending_profile_ = name;
        } else {
            profiles_[name] = crosshair_;
            save_profiles();
            note("\"" + name + "\" kaydedildi");
        }
    }
    ImGui::EndDisabled();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!profiles_.contains(name));
    if (ImGui::Button("Seçili profili sil", ImVec2(-1, 0))) {
        pending_action_ = "delete";
        pending_profile_ = name;
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Kodu kopyala", ImVec2(half, 0))) {
        ImGui::SetClipboardText(encode_code(crosshair_).c_str());
        note("Paylaşım kodu panoya kopyalandı");
    }
    ImGui::SameLine();
    const bool from_code = ImGui::Button("Koddan al", ImVec2(half, 0));
    ImGui::SetItemTooltip("Panodaki kodu uygular. Bu programın kodları, CS2 (CSGO-...) ve Valorant (0;P;...) kodları olur.");
    if (from_code) {
        const char* text = ImGui::GetClipboardText();
        json s;
        std::string source;
        if (text && decode_code(text, s, &source)) {
            apply_crosshair(s);
            if (source.empty()) note("Kod içe aktarıldı ve uygulandı");
            else note(TF("%s kodu yaklaşık olarak çevrildi ve uygulandı", source.c_str()));
        } else {
            note("Panodaki kod geçersiz", true);
        }
    }
    if (!pending_action_.empty()) ImGui::OpenPopup("Profil");
    if (ImGui::BeginPopupModal("Profil", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool remove = pending_action_ == "delete";
        ImGui::Text(remove ? "\"%s\" silinsin mi?" : "\"%s\" zaten var. Üzerine yazılsın mı?", pending_profile_.c_str());
        ImGui::Spacing();
        if (ImGui::Button(remove ? "Sil" : "Üzerine yaz", ImVec2(130, 32))) {
            if (remove) profiles_.erase(pending_profile_);
            else profiles_[pending_profile_] = crosshair_;
            save_profiles();
            pending_action_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Vazgeç", ImVec2(110, 32))) {
            pending_action_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::page_crosshair() {
    auto& s = crosshair_;
    bool edited = false;
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Crosshair");
    ImGui::PopFont();
    if (ImGui::Button(crosshair_on_ ? "Ekrandan kaldır" : "Ekranda göster", ImVec2(200, 36))) set_crosshair_on(!crosshair_on_);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (edit_bool("Bilgisayar açılınca otomatik göster##ch", opt_, "crosshair_autostart")) autostart_changed("crosshair_autostart");
    ImGui::SameLine();
    if (edit_bool("Kayıt ve yayında gizle##ch", opt_, "crosshair_hide")) { mark_dirty(); update_crosshair(); }
    ImGui::Spacing();
    ImGui::BeginChild("ch_sol", ImVec2(300, 0));
    if (crosshair_srv_) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 300, p.y + 240), IM_COL32(30, 30, 30, 255));
        // Gerçek boyutunda, kutunun ortasında (büyük resim kutuya sığdırılır)
        const float fit = std::min({1.f, 290.f / std::max(1, ch_w_), 230.f / std::max(1, ch_h_)});
        const float w = ch_w_ * fit, h = ch_h_ * fit;
        const float x0 = floorf(p.x + 150 - w / 2), y0 = floorf(p.y + 120 - h / 2);
        ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)crosshair_srv_.Get(), ImVec2(x0, y0), ImVec2(x0 + w, y0 + h));
    }
    ImGui::Dummy(ImVec2(300, 240));
    profiles_panel(ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ch", ImVec2(0, 0));
    ImGui::PushItemWidth(320);
    ImGui::SeparatorText("İç çizgiler");
    if (edit_bool("Her çizgiyi ayrı ayarla", s, "arms_separate")) {
        edited = true;
        if (!s.value("arms_ready", false)) {    // İlk açılışta şekil bozulmasın: ortak değerlerden başla
            s["arms_ready"] = true;
            for (auto& [key, name] : kArms) {
                const std::string k = key;
                s[k + "_length"] = k == "arm_up" && s.value("t_shape", false) ? 0 : s.value("length", 10);
                s[k + "_thickness"] = s.value("thickness", 2);
                s[k + "_gap"] = s.value("gap", 4);
            }
        }
    }
    if (s.value("arms_separate", false)) {
        if (ImGui::BeginTabBar("cizgiler")) {
            for (auto& [key, name] : kArms) {
                if (!ImGui::BeginTabItem(name)) continue;
                const std::string k = key;
                edited |= edit_int("Uzunluk##kol", s, (k + "_length").c_str(), 0, 50);
                edited |= edit_int("Kalınlık##kol", s, (k + "_thickness").c_str(), 1, 20);
                edited |= edit_int("Boşluk##kol", s, (k + "_gap").c_str(), 0, 40);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else {
        edited |= edit_int("Uzunluk", s, "length", 0, 30);
        edited |= edit_int("Kalınlık", s, "thickness", 1, 8);
        edited |= edit_int("Boşluk", s, "gap", 0, 20);
        edited |= edit_bool("T şekli", s, "t_shape");
    }
    // İç ve dış çizgilerin döndürme bölümü aynıdır, ayarları ayrı tutulur
    auto rotation = [&](const char* title, const std::string& prefix) {
        const std::string group_key = prefix + "rotate";
        ImGui::PushID(title);
        ImGui::SeparatorText(title);
        edited |= edit_int("Tümünü döndür", s, group_key.c_str(), -180, 180);
        if (ImGui::BeginTable("acilar", 3)) {
            ImGui::TableSetupColumn("Çizgi", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Kendi etrafında");
            ImGui::TableSetupColumn("Merkez etrafında");
            ImGui::TableHeadersRow();
            for (auto& [key, name] : kArms) {
                const std::string k = prefix + key;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                edited |= edit_int(("##" + k + "_spin").c_str(), s, (k + "_spin").c_str(), -180, 180);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                edited |= edit_int(("##" + k + "_angle").c_str(), s, (k + "_angle").c_str(), -180, 180);
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Açıları sıfırla")) {
            s[group_key] = 0;
            for (auto& [key, name] : kArms) {
                s[prefix + key + "_angle"] = 0;
                s[prefix + key + "_spin"] = 0;
            }
            edited = true;
        }
        ImGui::PopID();
    };
    rotation("İç çizgileri döndür", "");
    ImGui::SeparatorText("Dış çizgiler");
    edited |= edit_bool("Göster##dis", s, "outer");
    edited |= edit_int("Uzunluk##d", s, "outer_length", 0, 30);
    edited |= edit_int("Kalınlık##d", s, "outer_thickness", 1, 8);
    edited |= edit_int("Boşluk##d", s, "outer_gap", 0, 40);
    rotation("Dış çizgileri döndür", "outer_");
    ImGui::SeparatorText("Nokta");
    edited |= edit_bool("Göster##nokta", s, "dot");
    edited |= edit_int("Boyut##n", s, "dot_size", 1, 12);
    edited |= edit_choice("Şekil", s, "dot_shape", {{"square", "Kare"}, {"round", "Yuvarlak"}});
    ImGui::SeparatorText("Halka");
    edited |= edit_bool("Göster##halka", s, "ring");
    edited |= edit_int("Yarıçap", s, "ring_radius", 2, 40);
    edited |= edit_int("Kalınlık##h", s, "ring_thickness", 1, 6);
    ImGui::SeparatorText("Renk ve kenarlık");
    edited |= edit_color("Renk", s, "color");
    edited |= edit_int("Opaklık", s, "opacity", 10, 100);
    edited |= edit_bool("Kenarlık", s, "outline");
    edited |= edit_int("Kalınlık##k", s, "outline_thickness", 1, 3);
    edited |= edit_color("Kenarlık rengi", s, "outline_color");
    ImGui::SeparatorText("Zemine göre renk");
    edited |= edit_bool("Zemin renge yakınsa ikinci rengi kullan", s, "adaptive");
    edited |= edit_color("İkinci renk", s, "adaptive_color");
    ImGui::TextDisabled("Merkezin çevresindeki zemin ana renge benzeyince crosshair ikinci renge geçer (ör. yeşil çimen üstünde mor).");
    ImGui::SeparatorText("Resimden crosshair");
    const std::string image = s.value("image", std::string());
    if (ImGui::Button("Resim seç (PNG)")) pick_crosshair_image();
    if (!image.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Resmi kaldır")) { s["image"] = ""; edited = true; }
        ImGui::TextDisabled("%s", image.substr(image.find_last_of('\\') + 1).c_str());
        edited |= edit_int("Boyut (%)", s, "image_scale", 10, 800);
        ImGui::TextDisabled("Resim kullanılırken yukarıdaki çizim ayarları uygulanmaz; opaklık geçerlidir.");
    } else {
        ImGui::TextDisabled("Saydam arka planlı bir PNG seçin. Resmin ortası ekranın ortasına gelir.");
    }
    ImGui::SeparatorText("Konum");
    {
        int center = opt_.value("crosshair_center", 1);
        const char* modes[] = {T("Ana ekranın ortası"), T("Oyun penceresinin ortası")};
        if (ImGui::Combo(T("Merkez"), &center, modes, 2)) { opt_["crosshair_center"] = center; mark_dirty(); ch_next_ = 0; }
        ImGui::TextDisabled("Oyun penceresi: Oyunlar listesindeki ya da ekranı kaplayan program. Pencereli oyunda ve ikinci ekranda da ortalar.");
        bool moved = edit_int("Yatay kaydır", opt_, "crosshair_offset_x", -200, 200);
        moved |= edit_int("Dikey kaydır", opt_, "crosshair_offset_y", -200, 200);
        if (moved) { mark_dirty(); ch_next_ = 0; }
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (edited) { mark_dirty(); update_crosshair(); }
}

void App::fps_check(const char* label, const char* key) {
    if (edit_bool(label, fps_, key)) fps_dirty_ = true;
    if (key[0] && fps_missing(sensor_data_, fps_.value("gpu", std::string()), key)) {
        ImGui::SameLine();
        ImGui::TextColored(theme_warn(), "okunamıyor");
    }
}

void App::fps_status_panel() {
    ImGui::SeparatorText("Sensör durumu");
    const ImVec4 good = theme_good(), warn = theme_warn();
    const std::string state = sensors_.status();
    const bool admin = is_admin(), pawnio = pawnio_installed();
    ImGui::TextUnformatted("Sensörler");
    ImGui::SameLine(150);
    if (state == "loading") ImGui::TextDisabled("Yükleniyor...");
    else if (state == "ok") ImGui::TextColored(good, "Hazır");
    else ImGui::TextColored(warn, "Sınırlı (sadece kullanım değerleri)");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Yönetici yetkisi");
    ImGui::SameLine(150);
    ImGui::TextColored(admin ? good : warn, admin ? "Var" : "Yok");
    if (!admin) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Yönetici olarak aç")) {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            if (INT_PTR(ShellExecuteW(nullptr, L"runas", path, L"--yonetici", nullptr, SW_SHOWNORMAL)) > 32) PostQuitMessage(0);
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("PawnIO sürücüsü");
    ImGui::SameLine(150);
    ImGui::TextColored(pawnio ? good : warn, pawnio ? "Kurulu" : "Kurulu değil");
    if (!pawnio) {
        ImGui::SameLine();
        if (ImGui::SmallButton("İndir")) ShellExecuteW(nullptr, L"open", L"https://pawnio.eu", nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (state == "fallback" && !sensors_.error().empty()) ImGui::TextWrapped("Sensör kütüphanesi yüklenemedi: %s", sensors_.error().c_str());
    if (!admin) ImGui::TextDisabled("İşlemci ve disk sıcaklıkları için yönetici yetkisi gerekir.");
    if (!pawnio) ImGui::TextDisabled("İşlemci sıcaklıkları için ücretsiz PawnIO sürücüsünü bir kez kurun.");
}

void App::fps_tab_info() {
    ImGui::BeginChild("bilgi1", ImVec2(ImGui::GetContentRegionAvail().x / 2, 0));
    ImGui::SeparatorText("FPS");
    fps_check("FPS", "show_fps");
    fps_check("Kare süresi (ms)", "show_frametime");
    fps_check("Ekran yenileme hızı (Hz)", "show_refresh");
    ImGui::SeparatorText("Ekran kartı");
    std::vector<std::pair<std::string, std::string>> gpus{{"", "Otomatik"}};
    const std::string chosen = fps_.value("gpu", std::string());
    bool listed = chosen.empty();
    if (auto it = sensor_data_.find("gpus"); it != sensor_data_.end() && it->is_array())
        for (auto& g : *it) {
            const std::string name = g.value("name", std::string());
            gpus.push_back({name, name});
            listed |= name == chosen;
        }
    if (!listed) gpus.push_back({chosen, chosen});   // Kayıtlı kart şu an görünmese de listeden kaybolmasın
    ImGui::SetNextItemWidth(-40);
    if (edit_choice("Kart", fps_, "gpu", gpus)) fps_dirty_ = true;
    fps_check("Sıcaklık##g", "show_gpu_temp");
    fps_check("Hotspot sıcaklığı", "show_gpu_hotspot");
    fps_check("Kullanım (%)##g", "show_gpu_load");
    fps_check("Saat hızı (MHz)##g", "show_gpu_clock");
    fps_check("VRAM kullanımı (MB)", "show_gpu_vram");
    fps_check("Güç tüketimi (W)##g", "show_gpu_power");
    fps_check("Fan hızı (RPM)", "show_gpu_fan");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("bilgi2", ImVec2(0, 0));
    ImGui::SeparatorText("İşlemci");
    fps_check("Sıcaklık##c", "show_cpu_temp");
    fps_check("Çekirdek sıcaklıkları", "show_cpu_cores");
    fps_check("Kullanım (%)##c", "show_cpu_load");
    fps_check("Saat hızı (MHz)##c", "show_cpu_clock");
    fps_check("Güç tüketimi (W)##c", "show_cpu_power");
    ImGui::SeparatorText("Bellek");
    fps_check("Kullanılan RAM (GB)", "show_ram");
    fps_check("RAM kullanımı (%)", "show_ram_load");
    ImGui::SeparatorText("Diskler ve diğer");
    fps_check("SSD / HDD sıcaklıkları", "show_disk_temp");
    fps_check("Pil (% ve W)", "show_battery");
    fps_check("Saat", "show_time");
    ImGui::EndChild();
}

void App::fps_tab_look() {
    auto& s = fps_;
    ImGui::PushItemWidth(280);
    ImGui::SeparatorText("Yazı tipi");
    fps_dirty_ |= edit_font(s);
    fps_dirty_ |= edit_int("Boyut (px)", s, "font_size", 8, 72);
    fps_dirty_ |= edit_bool("Kalın", s, "bold");
    fps_dirty_ |= edit_bool("Her çözünürlükte aynı boyut (1080p'ye göre ölçekle)", s, "scale_with_resolution");
    ImGui::SeparatorText("Düzen");
    fps_dirty_ |= edit_choice("Yerleşim", s, "layout", {{"vertical", "Dikey (satır satır)"}, {"horizontal", "Yatay (tek satır)"}});
    fps_dirty_ |= edit_bool("Satır başlıkları (GPU, CPU, RAM...)", s, "show_labels");
    ImGui::SeparatorText("Renkler");
    fps_dirty_ |= edit_color("Başlık", s, "label_color");
    fps_dirty_ |= edit_color("Değer", s, "color");
    fps_dirty_ |= edit_color("Birim", s, "unit_color");
    fps_dirty_ |= edit_int("Opaklık (%)", s, "opacity", 10, 100);
    ImGui::PopItemWidth();
}

void App::fps_tab_alerts() {
    auto& s = fps_;
    ImGui::PushItemWidth(280);
    ImGui::SeparatorText("Uyarı renkleri");
    fps_dirty_ |= edit_bool("Düşük FPS ve yüksek sıcaklıkta renk değiştir", s, "dynamic_color");
    fps_dirty_ |= edit_color("Uyarı", s, "warn_color");
    fps_dirty_ |= edit_color("Kritik", s, "crit_color");
    ImGui::SeparatorText("FPS");
    fps_dirty_ |= edit_int("Uyarı altı", s, "mid_fps", 10, 240);
    fps_dirty_ |= edit_int("Kritik altı", s, "low_fps", 5, 144);
    ImGui::SeparatorText("Sıcaklık (°C)");
    fps_dirty_ |= edit_int("Uyarı üstü", s, "temp_warn", 40, 110);
    fps_dirty_ |= edit_int("Kritik üstü", s, "temp_crit", 50, 120);
    ImGui::SeparatorText("Ekran uyarıları");
    fps_dirty_ |= edit_bool("Sıcaklık ve pil uyarılarını göster", s, "alert_on");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Değer 5 saniye eşiği aşarsa ekranın üst ortasında kısa bir uyarı çıkar (gösterge kapalıyken de). Aynı uyarı en sık 2 dakikada bir gelir.");
    ImGui::PopStyleColor();
    fps_dirty_ |= edit_int("Ekran kartı (°C)", s, "alert_gpu_temp", 50, 110);
    fps_dirty_ |= edit_int("İşlemci (°C)", s, "alert_cpu_temp", 50, 110);
    fps_dirty_ |= edit_int("Pil (%) altına inince", s, "alert_battery", 5, 50);
    fps_dirty_ |= edit_bool("Uyarı sesi", s, "alert_sound");
    if (ImGui::Button("Örnek uyarıyı göster")) show_alert(T("Örnek uyarı: ekran kartı 87 °C"));
    ImGui::PopItemWidth();
}

void App::fps_tab_extra() {
    auto& s = fps_;
    ImGui::PushItemWidth(280);
    ImGui::SeparatorText("Ortalama ve en düşük FPS");
    fps_check("Ortalama FPS", "show_fps_avg");
    fps_check("%1 en düşük FPS", "show_low1");
    fps_check("%0,1 en düşük FPS", "show_low01");
    fps_dirty_ |= edit_int("Ölçüm süresi (saniye)", s, "lows_window", 2, 120);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("En düşük FPS, son saniyelerdeki en yavaş karelerden hesaplanır; takılmaları ortalamadan iyi gösterir. "
                       "FPS ekranın yenileme hızını aşınca kareler gruplar halinde ölçülür, değer yaklaşık olur.");
    ImGui::PopStyleColor();
    ImGui::SeparatorText("Kare süresi grafiği");
    fps_check("Grafiği göster", "show_graph");
    fps_dirty_ |= edit_int("Gösterilen süre (saniye)", s, "graph_seconds", 1, 30);
    ImGui::TextDisabled("Kırmızı çubuklar takılmadır (ortalamanın iki katından uzun kare).");
    ImGui::SeparatorText("Ping");
    fps_check("Ping göster", "show_ping");
    fps_dirty_ |= edit_text("Sunucu", s, "ping_host");
    ImGui::TextDisabled("Oyun sunucusunun adresi ya da örneğin 1.1.1.1, 8.8.8.8. Saniyede bir ölçülür; son 20 ölçümün kaybı da gösterilir.");
    fps_dirty_ |= edit_int("Uyarı üstü (ms)", s, "ping_warn", 20, 500);
    fps_dirty_ |= edit_int("Kritik üstü (ms)", s, "ping_crit", 30, 1000);
    if (pinger_.running()) {
        const json p = pinger_.data();
        if (p.contains("ms")) ImGui::TextColored(theme_good(), "%s", TF("Şu an: %d ms, kayıp %%%.0f", p.value("ms", 0), p.value("loss", 0.0)).c_str());
        else ImGui::TextColored(theme_warn(), "%s", T("Sunucuya ulaşılamıyor"));
    }
    ImGui::PopItemWidth();
}

void App::fps_tab_position() {
    auto& s = fps_;
    ImGui::PushItemWidth(280);
    ImGui::SeparatorText("Konum");
    fps_dirty_ |= edit_choice("Köşe", s, "position", {{"top-left", "Sol üst"}, {"top", "Üst orta"}, {"top-right", "Sağ üst"},
                                                      {"bottom-left", "Sol alt"}, {"bottom", "Alt orta"}, {"bottom-right", "Sağ alt"}});
    fps_dirty_ |= edit_int("Yatay boşluk", s, "offset_x", 0, 300);
    fps_dirty_ |= edit_int("Dikey boşluk", s, "offset_y", 0, 300);
    ImGui::SeparatorText("Kenarlık");
    fps_dirty_ |= edit_bool("Göster##kenar", s, "outline");
    fps_dirty_ |= edit_int("Kalınlık", s, "outline_thickness", 1, 3);
    fps_dirty_ |= edit_color("Renk##kenar", s, "outline_color");
    ImGui::SeparatorText("Arka plan");
    fps_dirty_ |= edit_bool("Göster##arka", s, "background");
    fps_dirty_ |= edit_color("Renk##arka", s, "bg_color");
    fps_dirty_ |= edit_int("İç boşluk", s, "padding", 0, 20);
    ImGui::SeparatorText("Güncelleme aralığı (ms)");
    fps_dirty_ |= edit_int("FPS", s, "interval", 100, 2000);
    fps_dirty_ |= edit_int("Sensörler", s, "sensor_interval", 250, 5000);
    ImGui::PopItemWidth();
}

void App::page_fps() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("FPS Ölçer");
    ImGui::PopFont();
    if (ImGui::Button(fps_on_ ? "Ekrandan kaldır" : "Ekranda göster", ImVec2(200, 36))) set_fps_on(!fps_on_);
    ImGui::SameLine();
    if (ImGui::Button("Varsayılan", ImVec2(110, 36))) { fps_ = default_fps(); fps_dirty_ = true; }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (edit_bool("Bilgisayar açılınca otomatik göster##fps", opt_, "fps_autostart")) autostart_changed("fps_autostart");
    ImGui::SameLine();
    if (edit_bool("Kayıt ve yayında gizle##fps", opt_, "fps_hide")) mark_dirty();
    ImGui::Spacing();

    ImGui::BeginChild("fps_sol", ImVec2(kFpsPreviewW, 0));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    static const ImU32 backgrounds[] = {IM_COL32(30, 30, 30, 255), IM_COL32(138, 138, 138, 255), IM_COL32(232, 232, 232, 255), IM_COL32(135, 206, 235, 255)};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + kFpsPreviewW, p.y + kFpsPreviewH), backgrounds[fps_preview_bg_]);
    if (fps_srv_ && fps_preview_w_ > 0) {
        const float fit = std::min({1.f, (kFpsPreviewW - 8) / fps_preview_w_, (kFpsPreviewH - 8) / fps_preview_h_});
        const float w = fps_preview_w_ * fit, h = fps_preview_h_ * fit;
        const float k = fit * (fps_.value("scale_with_resolution", true) ? GetSystemMetrics(SM_CYSCREEN) / 1080.f : 1.f);
        int x, y;
        fps_place(fps_, int(w), int(h), int(kFpsPreviewW), int(kFpsPreviewH), k, x, y);
        x = std::clamp(x, 0, int(kFpsPreviewW - w));    // Büyük boşluklarda önizlemeden taşmasın
        y = std::clamp(y, 0, int(kFpsPreviewH - h));
        draw->AddImage((ImTextureID)(intptr_t)fps_srv_.Get(), ImVec2(p.x + x, p.y + y), ImVec2(p.x + x + w, p.y + y + h));
    }
    ImGui::Dummy(ImVec2(kFpsPreviewW, kFpsPreviewH));
    ImGui::TextDisabled("Okunamayan değerler örnek olarak gösterilir");
    for (int i = 0; i < 4; i++) {
        static const char* names[] = {"Koyu", "Gri", "Açık", "Gökyüzü"};
        if (i) ImGui::SameLine();
        ImGui::RadioButton(names[i], &fps_preview_bg_, i);
    }
    ImGui::SetNextItemWidth(300);
    slider_int("Örnek FPS", &fps_sample_, 0, 300);
    ImGui::Spacing();
    fps_status_panel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("fps_sag", ImVec2(0, 0));
    if (ImGui::BeginTabBar("fps_sekmeler")) {
        if (ImGui::BeginTabItem("Bilgiler")) { fps_tab_info(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Görünüm")) { fps_tab_look(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Uyarılar")) { fps_tab_alerts(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("En düşük FPS / Ping")) { fps_tab_extra(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Konum / Arka plan")) { fps_tab_position(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void App::page_hotkeys() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Kısayollar");
    ImGui::PopFont();
    ImGui::TextDisabled("\"Değiştir\"e basıp yeni tuşa bas; istersen Ctrl, Shift ya da Alt ile birlikte. Esc vazgeçer.");
    ImGui::Spacing();
    std::lock_guard<std::mutex> g(hotkeys_lock_);
    for (auto& a : kActions) {
        ImGui::PushID(a.key);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.name);
        ImGui::SameLine(300);
        ImGui::TextColored(theme_accent(), "%s", capturing_ == a.key ? "Bir tuşa bas..." : key_name(hotkeys_[a.key]).c_str());
        ImGui::SameLine(560);
        if (ImGui::Button("Değiştir")) capturing_ = a.key;
        ImGui::SameLine();
        if (ImGui::Button("Varsayılan")) { hotkeys_[a.key] = a.fallback; mark_dirty(); }
        ImGui::SameLine();
        if (ImGui::Button("Kaldır")) { hotkeys_[a.key] = Hotkey{}; mark_dirty(); }
        ImGui::PopID();
    }
    if (capturing_.empty()) return;
    for (int vk = 8; vk < 255; vk++) {
        if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
        if (vk >= VK_LSHIFT && vk <= VK_RMENU) continue;
        if (vk == VK_LWIN || vk == VK_RWIN || !key_down(vk)) continue;
        const bool ctrl = key_down(VK_CONTROL), shift = key_down(VK_SHIFT), alt = key_down(VK_MENU);
        if (vk != VK_ESCAPE || ctrl || shift || alt) {
            const Hotkey h{vk, ctrl, shift, alt};
            bool used = false;
            for (auto& [name, other] : hotkeys_)
                used |= name != capturing_ && other.vk == h.vk && other.ctrl == h.ctrl && other.shift == h.shift && other.alt == h.alt;
            note(used ? key_name(h) + " zaten başka bir işe atanmış" : "", used);
            if (!used) { hotkeys_[capturing_] = h; mark_dirty(); }
        }
        capturing_.clear();
        break;
    }
}

void App::page_settings() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Ayarlar");
    ImGui::PopFont();
    ImGui::Spacing();
    if (theme_settings()) {
        opt_["theme_light"] = theme_is_light();
        opt_["theme_mode"] = theme_mode();
        opt_["theme_style"] = theme_style();
        mark_dirty();
    }
    ImGui::Spacing();
    language_settings();
}

void App::page_help() {
    help_tabs(kAppName, guide_game, [this] { page_developers(); }, help_request_);
}

void App::page_developers() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Geliştiriciler");
    ImGui::PopFont();
    ImGui::TextDisabled("Oyun Araçları'nı hazırlayan ekip");
    ImGui::Spacing();
    ImGui::SeparatorText("Ali Ceviz");
    ImGui::TextColored(theme_accent(), "Programlama");
    ImGui::TextUnformatted("Proje sahibi. Araçların fikri, kodlaması ve testleri.");
    ImGui::Spacing();
    ImGui::SeparatorText("Vahdet Göl");
    ImGui::TextColored(theme_accent(), "Arayüz Tasarımı");
    ImGui::TextUnformatted("Ekranların düzeni, renkler ve kullanım kolaylığı.");
    ImGui::Spacing();
    thanks_section();
    ImGui::SeparatorText("Kullanılan açık kaynak bileşenler");
    ImGui::BulletText("Dear ImGui - arayüz (MIT)");
    ImGui::BulletText("nlohmann/json - ayar dosyaları (MIT)");
    ImGui::BulletText("LibreHardwareMonitor - donanım sensörleri (MPL-2.0)");
    ImGui::Spacing();
    ImGui::TextDisabled("%s  ·  Sürüm %s", kAppName, kVersion);
}

void App::create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
}

int App::run(bool startup) {
    load();
    if (!g_test_page.empty()) page_ = g_test_page;
    if (opt_.value("gamma_applied", false)) {   // Önceki oturum oyunun ekran rengiyle kapandıysa düzelt
        apply_gamma(0, 0, 0);
        opt_["gamma_applied"] = false;
    }
    task_installed_ = task_exists();
    if (!task_installed_) opt_["crosshair_autostart"] = opt_["fps_autostart"] = false;   // Görev silindiyse tikler doğruyu göstersin

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
                                             &swap_, &device_, nullptr, &ctx_))) {
        MessageBoxW(hwnd, TW("Pencere çizimi başlatılamadı").c_str(), TW(kAppName).c_str(), MB_ICONERROR);
        return 1;
    }
    create_rtv();
    setup_imgui(hwnd);
    theme_start(hwnd, opt_.value("theme_style", 0), theme_mode_from(opt_));
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());

    add_tray_icon();
    keys_thread_ = std::thread([this] { guarded("kısayollar", [this] { hotkey_thread(); }); });
    if (startup) {      // Bilgisayar açılışı: pencere açılmaz, işaretli göstergeler başlar
        crosshair_on_ = opt_.value("crosshair_autostart", false);
        fps_on_ = opt_.value("fps_autostart", false);
    } else {
        splash_finish(4.0);
        show_window();
    }
    update_crosshair();

    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;   // Ardından başka mesaj gelse de kapanma isteği kaybolmasın
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        // Beklenmeyen bir hata (ör. oyun harita yüklerken sensör değerinin bir an bozuk gelmesi) programı kapatmasın:
        // kaydedilir, göstergeler çalışmaya devam eder
        guarded("tick", [&] { lang_tick(); tick(); });
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {   // Pencere görünmüyorsa çizim yok: kaynak harcanmaz
            MsgWaitForMultipleObjects(0, nullptr, FALSE, keys_fast() ? 15 : 50, QS_ALLINPUT);     // Tuş göstergesi gecikmesin
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuiErrorRecoveryState recovery;
        ImGui::ErrorRecoveryStoreState(&recovery);
        bool frame_ok = true;
        try {
        theme_tick();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("ana", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::BeginChild("menu", ImVec2(200, -28), 0, ImGuiWindowFlags_NoScrollbar);
        sidebar();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("icerik", ImVec2(0, -28));
        if (page_ == "crosshair") page_crosshair();
        else if (page_ == "fps") page_fps();
        else if (page_ == "bench") page_bench();
        else if (page_ == "games") page_games();
        else if (page_ == "aim") page_aim();
        else if (page_ == "extras") page_extras();
        else if (page_ == "hotkeys") page_hotkeys();
        else if (page_ == "settings") page_settings();
        else page_help();
        ImGui::EndChild();
        ImGui::AlignTextToFramePadding();
        if (message_error_) ImGui::TextColored(theme_error(), "%s", T(message_.c_str()));
        else ImGui::TextDisabled("%s", T(message_.c_str()));
        ImGui::End();
        manual_frame(2);
        } catch (const std::exception& e) {
            log_error(std::string("arayüz: ") + e.what());
            frame_ok = false;
        } catch (...) {
            log_error("arayüz: bilinmeyen hata");
            frame_ok = false;
        }
        if (!frame_ok) {     // Yarım kalan Begin/Push çağrıları kapatılır, bir sonraki kare temiz başlar
            ImGuiIO& eio = ImGui::GetIO();
            const bool asserts = eio.ConfigErrorRecoveryEnableAssert;
            eio.ConfigErrorRecoveryEnableAssert = false;
            ImGui::ErrorRecoveryTryToRecoverState(&recovery);
            eio.ConfigErrorRecoveryEnableAssert = asserts;
        }
        ImGui::Render();
        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        ctx_->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_->Present(1, 0);
        if (want_exit_) PostQuitMessage(0);
    }
    keys_halt_ = true;
    keys_thread_.join();
    game_leave();       // Ekran rengi ve göstergeler eski haline, güç planı ve kapatılan programlar geri
    if (!game_mode_exe_.empty()) game_mode_stop();
    if (bench_on_) bench_finish();
    write_json(data_dir() + L"\\playtime.json", playtime_);
    sensors_.stop();
    meter_.stop();
    pinger_.stop();
    media_.stop();
    save();
    overlay_.hide();
    fps_overlay_.hide();
    keys_overlay_.hide();
    song_overlay_.hide();
    badge_overlay_.hide();
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

static LRESULT wnd_proc_body(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Pencere iletisi işlenirken çıkan hata Windows'un içinden geçip programı kapatmasın
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        return wnd_proc_body(hwnd, msg, wp, lp);
    } catch (const std::exception& e) {
        log_error(std::string("ileti ") + std::to_string(msg) + ": " + e.what());
    } catch (...) {
        log_error("ileti " + std::to_string(msg) + ": bilinmeyen hata");
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT wnd_proc_body(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    mouse_position_fix(msg, lp);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (g_app && msg == g_show_message) { g_app->show_window(); return 0; }
    if (g_app && msg == g_taskbar_created) { g_app->add_tray_icon(); return 0; }    // Explorer yeniden başladı
    switch (msg) {
        case WM_SIZE:
            if (g_app && wp != SIZE_MINIMIZED) g_app->on_resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_HOTKEY_ACTION:
            if (g_app) g_app->on_hotkey(int(wp));
            return 0;
        case WM_INPUT:      // DPI ölçümü
            if (g_app) g_app->on_raw_input(reinterpret_cast<HRAWINPUT>(lp));
            break;
        case WM_TRAY:
            if (g_app && LOWORD(lp) == WM_LBUTTONUP) g_app->show_window();
            if (g_app && LOWORD(lp) == WM_RBUTTONUP) g_app->tray_menu();
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // F10 ve Alt pencere menüsünü açıp sonraki tıklamayı yutmasın
            break;
        case WM_CLOSE:      // Çarpı programı kapatmaz: göstergeler açık kalır
            if (g_app) g_app->hide_to_tray();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {1120, 720};
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    install_crash_guard();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool startup = false, elevated_restart = false;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--gorev-kur")) return write_task(true) ? 0 : 1;
        if (!wcscmp(argv[i], L"--gorev-sil")) return write_task(false) ? 0 : 1;
        if (!wcscmp(argv[i], L"--baslangic")) startup = true;
        if (!wcscmp(argv[i], L"--rs-testi") && i + 2 < argc) {      // Deneme: Kayıt Stüdyosu'na komut, dönen durum dosyaya
            write_json(argv[i + 2], json{{"state", rs_command(_wtoi(argv[i + 1]))}});
            return 0;
        }
        if (!wcscmp(argv[i], L"--kod-testi") && i + 2 < argc) {     // Deneme: kodu çözer, sonucu dosyaya yazar
            json s;
            std::string source;
            const bool ok = decode_code(narrow(argv[i + 1]), s, &source);
            write_json(argv[i + 2], json{{"ok", ok}, {"source", source}, {"crosshair", s}});
            return 0;
        }
        if (!wcscmp(argv[i], L"--sayfa") && i + 1 < argc) g_test_page = narrow(argv[i + 1]);     // Deneme: açılışta bu sayfa
        if (!wcscmp(argv[i], L"--yonetici")) elevated_restart = true;
    }
    LocalFree(argv);

    // Tek kopya çalışsın: ikinci açılış var olan pencereyi öne getirir
    g_show_message = RegisterWindowMessageW(L"OyunAraclariGoster");
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    for (int i = 0; elevated_restart && GetLastError() == ERROR_ALREADY_EXISTS && i < 30; i++) {   // Eski kopyanın kapanmasını bekle
        CloseHandle(mutex);
        Sleep(100);
        mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            if (!startup) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, g_show_message, 0, 0);
            }
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lang_init();
    if (!startup && !elevated_restart)      // Yönetici olarak yeniden açılışta tanıtım ikinci kez gösterilmez
        splash_start({L"\xE7FC", T(kAppName), T("Crosshair ve FPS göstergesi"), TF("Sürüm %s", kVersion), T("Açık kaynak · Ücretsiz"), 0x2fbf71});
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int w = std::min(1280, int(sw * 0.8)), h = std::min(860, int(sh * 0.85));
    App app;
    app.hwnd = CreateWindowW(kWindowClass, TW(kAppName).c_str(), WS_OVERLAPPEDWINDOW, (sw - w) / 2, (sh - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    ChangeWindowMessageFilterEx(app.hwnd, g_show_message, MSGFLT_ALLOW, nullptr);   // Yönetici kopyasına normal kopyadan ulaşılsın
    g_app = &app;
    const int code = app.run(startup);
    g_app = nullptr;
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
