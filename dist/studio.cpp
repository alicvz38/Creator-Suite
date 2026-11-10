// Kayıt Stüdyosu: pencere, arayüz (Dear ImGui) ve ayarlar. Kayıt işi motor.cpp'de ayrı iş parçacığında döner.
#include "ui.h"
#include "recorder.h"
#include "stream.h"
#include "engine.h"
#include "browser.h"
#include "capture.h"
#include "fps_meter.h"
#include "vcam_shared.h"

#include <dpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

json default_social() {
    json accounts = json::object();
    for (auto& p : kPlatforms) accounts[p.key] = {{"show", true}, {"handle", ""}};
    return {{"accounts", accounts}, {"layout", "vertical"}, {"spacing", 10}, {"font", "Segoe UI"}, {"font_size", 26},
            {"bold", true}, {"text_color", "#ffffff"}, {"icon_style", "brand"}, {"outline", true},
            {"outline_thickness", 2}, {"outline_color", "#000000"}, {"background", true}, {"bg_color", "#000000"},
            {"bg_opacity", 55}, {"bg_radius", 60}};
}

namespace {

const char* kAppName = "Kayıt Stüdyosu";
const char* kVersion = build_version();      // Derleme anı: yyyy.aa.gg.ss.dd.ss
const float kBottomHeight = 228;
const float kMenuOpen = 270, kMenuClosed = 52;
// Windows'un Segoe MDL2 simge yazı tipindeki karakterler
#define ICON_MENU ""
#define ICON_STREAM "\xEE\xA4\xBE"
#define ICON_VOLUME "\xEE\x9D\xA7"
#define ICON_MUTE "\xEE\x9D\x8F"
#define ICON_FILTER "\xEE\xA7\xA9"
#define ICON_CAMERA "\xEE\x9C\xA2"
#define ICON_RECORD ""
#define ICON_KEYS ""
#define ICON_PEOPLE ""
#define ICON_SETTINGS ""
#define ICON_ADD ""
#define ICON_DELETE ""
#define ICON_UP ""
#define ICON_DOWN ""
#define ICON_RENAME ""
#define ICON_BACK ""
const float kHandle = 7;
const float kSnap = 10;
const float kMinSize = 16;

struct SourceType { const char* key; const char* name; bool resizable; };
const SourceType kTypes[] = {
    {"screen", "Ekran", true}, {"region", "Ekran bölgesi", true}, {"window", "Pencere", true}, {"game", "Oyun yakalama", true}, {"fps_meter", "FPS göstergesi", false}, {"camera", "Kamera", true}, {"image", "Resim", true}, {"video", "Video", true},
    {"text", "Metin", false}, {"social", "Sosyal medya", false}, {"color", "Düz renk", true},
    {"browser", "Tarayıcı", true}, {"slideshow", "Slayt gösterisi", true}, {"scene", "Sahne", true},
};

const SourceType& type_of(const std::string& key) {
    for (auto& t : kTypes)
        if (key == t.key) return t;
    return kTypes[0];
}

std::string new_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char text[17];
    sprintf_s(text, "%016llx", (unsigned long long)rng());
    return text;
}

json source_defaults(const std::string& type) {
    if (type == "screen") return {{"monitor", 0}, {"cursor", true}};
    if (type == "region") return {{"monitor", 0}, {"cursor", true}, {"region", {{"x", 0}, {"y", 0}, {"w", 1280}, {"h", 720}}}};
    if (type == "window") return {{"window", ""}, {"title", ""}, {"cursor", true}, {"client", true}};
    if (type == "game") return {{"window", ""}, {"cursor", false}};
    // Oyun Araçları'ndaki görünümle başlar, sonra kaynağın kendi ayarı olur
    if (type == "fps_meter") return merge_known(default_fps(), read_json(data_dir() + L"\\fps-settings.json"));
    if (type == "camera") return {{"device", 0}, {"mirror", false}};
    if (type == "image") return {{"file", ""}};
    if (type == "video") return {{"file", ""}, {"loop", true}, {"playlist", json::array()}, {"shuffle", false}};
    if (type == "browser")
        return {{"url", ""}, {"width", 1280}, {"height", 720}, {"muted", false}, {"reload", 0},
                {"css", "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }"}};
    if (type == "slideshow") return {{"files", json::array()}, {"interval", 5}, {"fade", true}, {"shuffle", false}};
    if (type == "scene") return {{"scene", ""}};
    if (type == "text")
        return {{"text", "Yeni metin"}, {"font", "Segoe UI"}, {"font_size", 48}, {"bold", true}, {"color", "#ffffff"},
                {"outline", true}, {"outline_color", "#000000"}, {"outline_thickness", 3}, {"background", false},
                {"bg_color", "#000000"}, {"bg_opacity", 60}};
    if (type == "color") return {{"color", "#1e1e2e"}};
    return json::object();
}

json new_source(const std::string& type, float x, float y, float w, float h) {
    return {{"id", new_id()}, {"type", type}, {"name", type_of(type).name}, {"visible", true}, {"x", x}, {"y", y},
            {"w", w}, {"h", h}, {"opacity", 100}, {"settings", source_defaults(type)}};
}

struct Action { const char* key; const char* name; Hotkey fallback; };
const Action kActions[] = {
    {"record", "Kaydı başlat / durdur", {VK_F12, true}}, {"pause", "Duraklat / devam et", {VK_F11, true}},
    {"stream", "Yayını başlat / durdur", {VK_F10, true}}, {"screenshot", "Ekran görüntüsü al", {VK_F9, true}},
    {"mute_mic", "Mikrofonu sustur / aç", {}}, {"mute_desktop", "Bilgisayar sesini sustur / aç", {}},
    {"push_to_talk", "Bas-konuş (basılı tutunca mikrofon açılır)", {}},
    {"replay_toggle", "Tekrar arabelleğini başlat / durdur", {}}, {"replay_save", "Tekrarı kaydet (son saniyeler)", {VK_F8, true}},
    {"studio_transition", "Stüdyo modu: önizlemeyi yayına al", {}}, {"vcam_toggle", "Sanal kamerayı aç / kapat", {}},
};

// Sanal kamera sürücüsü kamera hizmetinin okuyabileceği yere kopyalanır ve Windows'a tanıtılır (yönetici olarak çalışır)
std::wstring vcam_target() {
    PWSTR pd = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &pd))) dir = std::wstring(pd) + L"\\Recording Studio";
    CoTaskMemFree(pd);
    return dir;
}

// Kurulu ve güncel mi (program güncellenip sürücü eski kaldıysa yeniden kurulum istenir)
bool vcam_registered() {
    DWORD version = 0, size = sizeof(version);
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVcamClsidText + L"\\InprocServer32";
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"RecordingStudioVersion", RRF_RT_REG_DWORD, nullptr, &version, &size) != ERROR_SUCCESS) return false;
    return version >= kVcamVersion;
}

int vcam_install(bool remove) {
    const std::wstring dir = vcam_target(), dll = dir + L"\\vcam.dll", key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVcamClsidText;
    if (remove) {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
        if (!DeleteFileW(dll.c_str())) MoveFileExW(dll.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);   // Kamera hizmeti kullanıyorsa yeniden başlatmada
        return 0;
    }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    const std::wstring source = exe_dir() + L"\\vcam.dll";
    if (!CopyFileW(source.c_str(), dll.c_str(), FALSE)) {     // Eski sürüm kamera hizmetinde açıksa adı değiştirilip yenisi yazılır
        const std::wstring old = dll + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (MoveFileExW(dll.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!CopyFileW(source.c_str(), dll.c_str(), FALSE)) return 2;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (key + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return 3;
    RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(dll.c_str()), DWORD((dll.size() + 1) * sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), DWORD((wcslen(model) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"RecordingStudioVersion", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&kVcamVersion), sizeof(DWORD));
    RegCloseKey(k);
    return 0;
}

// Kaynak filtrelerinin varsayılanları (motor.cpp parse_fx ile aynı anahtarlar)
json default_filters() {
    return {{"crop_l", 0}, {"crop_t", 0}, {"crop_r", 0}, {"crop_b", 0},
            {"brightness", 0}, {"contrast", 0}, {"saturation", 0}, {"gamma", 0}, {"hue", 0},
            {"chroma", false}, {"chroma_color", "#00ff00"}, {"similarity", 40}, {"smoothness", 8}, {"spill", 50},
            {"sharpen", 0}, {"mask", "none"}, {"radius", 24}};
}

// Ekranın bir bölgesini fareyle seçtirir: ekran kararır, sürüklenen dikdörtgen açık kalır. Esc ya da sağ tık vazgeçer.
// monitor: masaüstü koordinatı. out: ekranın kendi pikseli.
struct RegionPick { POINT a{}, b{}; bool dragging = false, done = false, ok = false; };

LRESULT CALLBACK region_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<RegionPick*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!p) return DefWindowProcW(h, msg, wp, lp);
    const POINT at{short(LOWORD(lp)), short(HIWORD(lp))};
    switch (msg) {
        case WM_LBUTTONDOWN: p->a = p->b = at; p->dragging = true; SetCapture(h); return 0;
        case WM_MOUSEMOVE: if (p->dragging) { p->b = at; InvalidateRect(h, nullptr, FALSE); } return 0;
        case WM_LBUTTONUP:
            if (p->dragging) { p->b = at; p->dragging = false; ReleaseCapture(); p->ok = abs(p->b.x - p->a.x) >= 16 && abs(p->b.y - p->a.y) >= 16; p->done = p->ok; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_RBUTTONDOWN: p->done = true; return 0;
        case WM_KEYDOWN: if (wp == VK_ESCAPE) p->done = true; return 0;
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            HBRUSH dark = CreateSolidBrush(RGB(0, 0, 0)), hole = CreateSolidBrush(RGB(255, 0, 255)), line = CreateSolidBrush(RGB(255, 71, 87));
            FillRect(mem, &rc, dark);
            const RECT sel{std::min(p->a.x, p->b.x), std::min(p->a.y, p->b.y), std::max(p->a.x, p->b.x), std::max(p->a.y, p->b.y)};
            if (sel.right > sel.left && sel.bottom > sel.top) {
                FillRect(mem, &sel, hole);      // Renk anahtarı: bu kısım tamamen saydam görünür
                const RECT edge{sel.left - 3, sel.top - 3, sel.right + 3, sel.bottom + 3};
                FrameRect(mem, &edge, line);
                const RECT edge2{sel.left - 2, sel.top - 2, sel.right + 2, sel.bottom + 2};
                FrameRect(mem, &edge2, line);
            }
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(255, 255, 255));
            HFONT font = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ old_font = SelectObject(mem, font);
            std::wstring hint = TW("Kaydedilecek bölgeyi fareyle sürükleyerek seçin. Esc ya da sağ tık: vazgeç");
            if (sel.right > sel.left) hint += L"     " + std::to_wstring(sel.right - sel.left) + L" x " + std::to_wstring(sel.bottom - sel.top);
            RECT text_rc{0, 30, rc.right, 80};
            DrawTextW(mem, hint.c_str(), -1, &text_rc, DT_CENTER | DT_SINGLELINE);
            SelectObject(mem, old_font);
            DeleteObject(font);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp); DeleteDC(mem);
            DeleteObject(dark); DeleteObject(hole); DeleteObject(line);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool pick_region(const RECT& monitor, RECT& out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = region_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"RecordingStudioRegion";
        RegisterClassExW(&wc);
        registered = true;
    }
    RegionPick pick;
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"RecordingStudioRegion", L"", WS_POPUP,
                             monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return false;
    SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(&pick));
    SetLayeredWindowAttributes(h, RGB(255, 0, 255), 120, LWA_COLORKEY | LWA_ALPHA);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    SetTimer(h, 1, 50, nullptr);    // Pencere odak almasa da Esc yakalansın
    MSG msg;
    while (!pick.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (key_down(VK_ESCAPE)) pick.done = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(h);
    if (!pick.ok) return false;
    out = {std::min(pick.a.x, pick.b.x), std::min(pick.a.y, pick.b.y), std::max(pick.a.x, pick.b.x), std::max(pick.a.y, pick.b.y)};
    return true;
}

// --- Uygulama ---

// Kayıt ve yayın süresi göstergesi: ana ekranın köşesinde küçük, her zaman üstte duran bir kutu. Kaynak değildir:
// ekran yakalamadan hariç tutulur (WDA_EXCLUDEFROMCAPTURE), kayda ve yayına girmez; tıklamalar arkadaki pencereye geçer.
// İki satır olabilir: üstte kayıt (kırmızı nokta), altta yayın ("CANLI" etiketi).
class TimerOverlay {
public:
    // Her turda çağrılır; iki satır da kapalıysa kutu gizlenir
    void update(bool rec, bool paused, double rec_seconds, bool live, double live_seconds, const std::string& corner) {
        if (!rec && !live) {
            if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
            return;
        }
        if (!hwnd_ && !create()) return;
        const std::string where = corner.size() == 2 ? corner : "tr";
        const int secs = rec ? int(rec_seconds) : -1, live_secs = live ? int(live_seconds) : -1;
        const bool blink = !paused && fmod(rec ? rec_seconds : live_seconds, 1.0) < 0.5;
        const bool rows_changed = (secs >= 0) != (secs_ >= 0) || (live_secs >= 0) != (live_secs_ >= 0);
        const bool redraw = secs != secs_ || live_secs != live_secs_ || paused != paused_ || blink != blink_;
        secs_ = secs; live_secs_ = live_secs; paused_ = paused; blink_ = blink;
        if (rows_changed) scale_ = 0;                       // Yükseklik değişti: bölge yeniden kurulur
        if (where != corner_ || redraw) place(where);       // Saniyede bir yeniden en üste alınır
        if (redraw) InvalidateRect(hwnd_, nullptr, FALSE);
        if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    void close() {
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (font_) DeleteObject(font_);
        if (small_font_) DeleteObject(small_font_);
        font_ = small_font_ = nullptr;
        scale_ = 0;
    }

private:
    bool create() {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"KayitSuresiGostergesi";
            RegisterClassExW(&wc);
            registered = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                                L"KayitSuresiGostergesi", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;
        SetLayeredWindowAttributes(hwnd_, 0, 230, LWA_ALPHA);
        SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);    // Kendi ekranınızda görünür, kayıtta görünmez
        corner_.clear();
        return true;
    }
    void place(const std::string& corner) {
        corner_ = corner;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const float scale = GetDpiForWindow(hwnd_) / 96.f;
        const int rows = (secs_ >= 0 ? 1 : 0) + (live_secs_ >= 0 ? 1 : 0);
        const int w = int((live_secs_ >= 0 ? 178 : 132) * scale), h = int((rows * 34 + 4) * scale), m = int(16 * scale);
        if (scale != scale_) {
            scale_ = scale;
            if (font_) DeleteObject(font_);
            if (small_font_) DeleteObject(small_font_);
            font_ = CreateFontW(-int(20 * scale), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            small_font_ = CreateFontW(-int(12 * scale), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, int(12 * scale), int(12 * scale)), TRUE);
        }
        const RECT& a = mi.rcWork;
        const int x = corner[1] == 'l' ? a.left + m : a.right - w - m;
        const int y = corner[0] == 'b' ? a.bottom - h - m : a.top + m;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    }
    void paint(HDC dc, const RECT& r) {
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        const int row_h = int(34 * scale_), top = int(2 * scale_);
        int y = top;
        if (secs_ >= 0) { paint_record(dc, RECT{0, y, r.right, y + row_h}); y += row_h; }
        if (live_secs_ >= 0) paint_live(dc, RECT{0, y, r.right, y + row_h});
    }
    static void paint_time(HDC dc, HFONT font, int secs, RECT t, COLORREF color) {
        wchar_t text[16];
        swprintf_s(text, L"%02d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
        HGDIOBJ old_f = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, text, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        SelectObject(dc, old_f);
    }
    // Yayın satırı: kırmızı "CANLI" etiketi ve süre
    void paint_live(HDC dc, const RECT& r) {
        const float s = scale_;
        const std::wstring label = TW("CANLI");
        HGDIOBJ old_f = SelectObject(dc, small_font_);
        SIZE size{};
        GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &size);
        const int cy = (r.top + r.bottom) / 2, x = int(10 * s);
        RECT badge{x, cy - int(10 * s), x + size.cx + int(12 * s), cy + int(10 * s)};
        HBRUSH b = CreateSolidBrush(RGB(220, 38, 38));
        HGDIOBJ old_b = SelectObject(dc, b), old_p = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, badge.left, badge.top, badge.right + 1, badge.bottom + 1, int(6 * s), int(6 * s));
        SelectObject(dc, old_b);
        SelectObject(dc, old_p);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, label.c_str(), -1, &badge, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        SelectObject(dc, old_f);
        paint_time(dc, font_, live_secs_, RECT{badge.right + int(6 * s), r.top, r.right - int(12 * s), r.bottom}, RGB(245, 245, 245));
    }
    // Kayıt satırı: yanıp sönen kırmızı nokta (duraklatılınca sarı çubuklar) ve süre
    void paint_record(HDC dc, const RECT& r) {
        const float s = scale_;
        const int cy = (r.top + r.bottom) / 2, cx = int(20 * s), rad = int(6 * s);
        if (paused_) {      // Duraklatıldı: iki sarı çubuk
            HBRUSH b = CreateSolidBrush(RGB(240, 180, 40));
            RECT l{cx - int(6 * s), cy - int(7 * s), cx - int(2 * s), cy + int(7 * s)}, rr{cx + int(2 * s), l.top, cx + int(6 * s), l.bottom};
            FillRect(dc, &l, b);
            FillRect(dc, &rr, b);
            DeleteObject(b);
        } else {            // Kayıt sürüyor: yanıp sönen kırmızı nokta
            HBRUSH b = CreateSolidBrush(blink_ ? RGB(232, 52, 52) : RGB(110, 30, 30));
            HGDIOBJ old_b = SelectObject(dc, b), old_p = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
            SelectObject(dc, old_b);
            SelectObject(dc, old_p);
            DeleteObject(b);
        }
        paint_time(dc, font_, secs_, RECT{int(34 * s), r.top, r.right - int(12 * s), r.bottom}, paused_ ? RGB(200, 200, 200) : RGB(245, 245, 245));
    }
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<TimerOverlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            HDC mem = CreateCompatibleDC(dc);       // Titremesin diye önce bellekte çizilir
            HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            self->paint(mem, r);
            BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(h, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, small_font_ = nullptr;
    float scale_ = 0;
    int secs_ = -1, live_secs_ = -1;        // -1: o satır gösterilmez
    bool paused_ = false, blink_ = false;
    std::string corner_;
};

class App {
public:
    int run(int test_seconds, int stream_test);

    bool want_close = false, start_minimized = false;
    int test_properties = -1;       // Deneme: açılışta bu sıradaki kaynağın Özellikler penceresi açılır
    void on_resize(UINT w, UINT h) {
        if (swap_ && w && h) { rtv_.Reset(); swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0); create_rtv(); }
    }
    void on_hotkeys();
    void toggle_fullscreen();
    void hide_to_tray();
    void on_tray(UINT event);
    void toggle_mute(bool mic);     // true mikrofon, false bilgisayar sesi
    // Kaynaklarda sağ tık: türe göre seçenekler (liste ve önizleme)
    void source_menu(const std::string& id);
    void rename_popup();
    void ungroup(const std::string& id);
    std::string rename_id_;
    char rename_buf_[128] = "";
    bool rename_open_ = false;
    // Oyun Araçları'ndan gelen komut: 0 durum sorgusu, 1 tekrar arabelleğini aç, 2 kapat, 3 tekrarı kaydet. Dönüş: durum bitleri.
    LRESULT remote(WPARAM cmd) {
        if (cmd == 1 && !status_.replay_on) engine_.request_replay(true);
        if (cmd == 2 && status_.replay_on) engine_.request_replay(false);
        if (cmd == 3 && status_.replay_on) engine_.request_replay_save();
        return 0x100 | (status_.recording ? 1 : 0) | (status_.streaming ? 2 : 0) | (status_.replay_on ? 4 : 0) | (status_.paused ? 8 : 0);
    }
    void show_window() { window_show(hwnd); }
    bool is_fullscreen() const { return fullscreen_; }

    HWND hwnd = nullptr;

private:
    void create_rtv();
    void load();
    void save();
    void changed() { doc_dirty_ = true; }
    void hotkey_thread();

    json& scene();
    json* source(const std::string& id);
    void add_source(const std::string& type);

    void main_menu();
    void toggle_stream();
    void status_bar();
    void sidebar();
    void open_settings(const std::string& tab);
    void open_record_folder();
    void open_editor(bool with_last);
    void page_record();
    void panel_transition();
    void tab_record();
    bool encoder_settings(json& v, bool stream);
    // Profiller: sahneler, kayıt, yayın, ses, tema ve panel düzeni bir arada (data_dir\Profiles\ad.json)
    std::wstring profile_file(const std::string& name);
    std::vector<std::string> profiles();
    json profile_capture();
    void profile_store();
    void profile_load(const json& p);
    void profile_switch(const std::string& name);
    void profiles_migrate();
    void profile_delete();
    void profile_export();
    void profile_import();
    void profiles_menu();
    void profile_popup();
    void page_profiles();
    void page_settings();
    void page_theme();
    void page_stream();
    void page_hotkeys();
    void page_developers();
    void panel_preview(ImVec2 size);
    void preview_area(ImVec2 size);
    void panel_scenes();
    void panel_sources();
    void panel_record();
    void panel_audio();
    void panel_properties();
    void source_filters(json& src);
    void region_now(const std::string& id);
    void mic_window();
    void take_screenshot();
    void studio_transition();
    void group_popup();
    void vcam_toggle();
    void stats_window();
    void audio_inputs_panel();
    void panel_program(ImVec2 size);
    void hotkey_row(const std::string& key, const char* label, const Hotkey* fallback);
    void social_settings();
    void popup_exit();
    void preview_input(ImVec2 origin, float f);
    void update_preview_texture();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11ShaderResourceView> preview_srv_;
    HANDLE preview_handle_ = nullptr;

    Engine engine_;
    Status status_;
    std::map<std::string, Rect> boxes_;
    bool sidebar_open_ = true, show_properties_ = false;
    float sidebar_w_ = kMenuOpen;
    json layout_ = layout_defaults();    // Bölme boyutları (sol menü, alt sıra, alt paneller)
    bool layout_dirty_ = false, sidebar_dragging_ = false;
    static json layout_defaults() { return {{"sidebar", double(kMenuOpen)}, {"bottom", 300.0}, {"transition", 200.0}, {"controls", 260.0}}; }
    static std::vector<LayoutPreset> layout_presets() {
        return {{T("Varsayılan"), layout_defaults()},
                {T("Geniş önizleme"), {{"sidebar", 220.0}, {"bottom", 300.0}, {"transition", 170.0}, {"controls", 240.0}}},
                {T("Geniş kontroller"), {{"sidebar", 320.0}, {"bottom", 320.0}, {"transition", 240.0}, {"controls", 340.0}}}};
    }
    float sidebar_open_w() const { return std::clamp(float(layout_.value("sidebar", double(kMenuOpen))), 200.f, 480.f); }
    std::string settings_tab_ = "record";
    int help_request_ = -1;
    int theme_mode_ = 0;
    int theme_style_ = 0;
    bool autostart_on_ = false;
    json doc_, rec_, keys_;
    bool doc_dirty_ = true, rec_dirty_ = true, save_dirty_ = false;
    double save_at_ = 0;
    std::string page_ = "record", selected_, message_;
    bool message_error_ = false, was_recording_ = false, was_streaming_ = false, fullscreen_ = false;
    std::string shown_stream_error_;
    bool camera_error_shown_ = false;
    WINDOWPLACEMENT placement_{sizeof(placement_)};
    struct Drag { int mode = 0; float gx = 0, gy = 0, w = 0, h = 0, ax = 0, ay = 0, aspect = 1; bool top = false; } drag_;
    char scene_name_[128] = "";
    std::vector<std::string> cameras_;
    std::vector<AudioDevice> speakers_, mics_;
    std::string capturing_;
    std::thread keys_thread_;
    std::atomic<bool> keys_halt_{false};
    std::map<std::string, Hotkey> hotkeys_;     // İşler (kActions), "scene:<id>" ve "source:<id>"
    std::mutex hotkeys_lock_;
    std::vector<std::string> fired_;            // Kısayol iş parçacığından gelen, işlenmeyi bekleyenler
    std::string pending_region_;                // Bölge seçimi bir sonraki turda (arayüz karesi dışında) açılır
    bool show_mic_ = false, tray_note_shown_ = false, show_stats_ = false;
    long long shots_seen_ = 0, replay_seen_ = 0;
    ComPtr<ID3D11ShaderResourceView> program_srv_;
    HANDLE program_handle_ = nullptr;
    std::vector<std::string> audio_apps_;
    std::string profile_ = "Varsayılan", profile_action_;
    char profile_name_[128] = "";
    std::map<std::string, bool> group_pick_;
    bool vcam_install_ask_ = false;
    std::string shown_vcam_error_;
    TimerOverlay timer_;        // Kayıt sırasında ekranda geçen süre (Ayarlar > Kayıt)
    // Sohbet ve bildirim pencereleri (Ayarlar > Sohbet). Anahtar: "chat:main", "chat:<hedef sırası>", "alerts"
    struct PanelEntry { std::string key, title, url, service; };
    std::vector<PanelEntry> panel_entries();
    std::map<std::string, std::unique_ptr<BrowserPanel>> panels_;
    void panels_tick();
    void panel_toggle(const std::string& key);
    void panels_restore();
    void panels_close_all();
    void panels_remember();
    void page_chat();
    void chat_menu();
};

App* g_app = nullptr;
int g_test_properties = -1;
UINT g_show_message = 0;

void begin_panel(const char* title, ImVec2 size) {
    ImGui::BeginChild(title, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(nullptr, 17.f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

bool icon_button(const char* icon, const char* tip) {
    const bool pressed = ImGui::Button(icon, ImVec2(36, 30));
    ImGui::SetItemTooltip("%s", T(tip));
    return pressed;
}

// Yayın anahtarı kanalınıza erişim sağlar: dosyada düz metin durmaz, Windows hesabınıza bağlı şifrelenir
std::string protect_text(const std::string& text) {
    if (text.empty()) return {};
    DATA_BLOB in{DWORD(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, out{};
    if (!CryptProtectData(&in, L"Recording Studio", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string hex;
    char part[3];
    for (DWORD i = 0; i < out.cbData; i++) {
        sprintf_s(part, "%02x", out.pbData[i]);
        hex += part;
    }
    LocalFree(out.pbData);
    return hex;
}

std::string unprotect_text(const std::string& hex) {
    if (hex.empty() || hex.size() % 2) return {};
    std::vector<BYTE> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = BYTE(strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string text(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return text;
}

// Windows oturumu açılınca başlatma: kullanıcının başlangıç listesi, yönetici onayı gerekmez
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunName = L"Recording Studio";

std::wstring autostart_command() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\" --baslangic";
}

std::wstring autostart_read() {
    wchar_t value[1024] = L"";
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}

bool autostart_write(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS status;
    if (enable) {
        const std::wstring command = autostart_command();
        status = RegSetValueExW(key, kRunName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunName);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

struct StreamService { const char* key; const char* name; const char* server; const char* hint; };
const StreamService kServices[] = {
    {"twitch", "Twitch", "rtmp://live.twitch.tv/app", "Yayın anahtarı: Twitch Yayıncı Paneli > Ayarlar > Yayın."},
    {"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "Yayın anahtarı: YouTube Studio > Canlı yayına geç > Yayın ayarları."},
    {"kick", "Kick", "", "Kick paneli > Channel > Stream URL and Key: sunucu adresini ve anahtarı oradan kopyalayın."},
    {"facebook", "Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", "Yayın anahtarı: Facebook Live Producer > Yayın yazılımı."},
    {"custom", "Özel sunucu", "", "Platformunuzun verdiği RTMP ya da RTMPS adresini yazın."},
};

const StreamService& find_service(const std::string& key) {
    for (auto& s : kServices)
        if (key == s.key) return s;
    return kServices[0];
}

// Sohbet penceresinin adresi. text: kanal adı, kanal bağlantısı, YouTube video bağlantısı ya da tam sohbet adresi.
// Boş dönerse adres yazılmamıştır (Facebook ve özel sunucuda tam adres gerekir).
std::string chat_url(const std::string& service, std::string text) {
    while (!text.empty() && isspace(uint8_t(text.back()))) text.pop_back();
    while (!text.empty() && isspace(uint8_t(text.front()))) text.erase(text.begin());
    if (text.empty()) return "";
    // "kick.com/ad" ya da "twitch.tv/ad" bağlantısından kanal adı; popout adresi olduğu gibi kalır
    auto channel = [&](const char* domain) -> std::string {
        const size_t at = text.find(domain);
        if (at == std::string::npos) return text.find('/') == std::string::npos && text.find('.') == std::string::npos ? text : "";
        std::string rest = text.substr(at + strlen(domain));
        if (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
        rest = rest.substr(0, rest.find_first_of("/?#"));
        return rest == "popout" ? "" : rest;
    };
    if (service == "kick" || service == "twitch") {
        std::string name = channel(service == "kick" ? "kick.com" : "twitch.tv");
        if (!name.empty() && name[0] == '@') name.erase(0, 1);
        if (name.empty()) return text.find("://") != std::string::npos ? text : "https://" + text;
        return service == "kick" ? "https://kick.com/popout/" + name + "/chat" : "https://www.twitch.tv/popout/" + name + "/chat?popout=";
    }
    if (service == "youtube") {     // Her canlı yayının kendi sohbeti var: video kimliği gerekir
        if (text.find("live_chat") != std::string::npos) return text;
        std::string id;
        for (const char* mark : {"v=", "youtu.be/", "/live/"}) {
            const size_t at = text.find(mark);
            if (at != std::string::npos) { id = text.substr(at + strlen(mark)); break; }
        }
        if (id.empty() && text.find('/') == std::string::npos) id = text;
        id = id.substr(0, id.find_first_of("&?/#"));
        return id.empty() ? "" : "https://www.youtube.com/live_chat?is_popout=1&v=" + id;
    }
    return text.find('.') == std::string::npos ? "" : text;     // Facebook ve özel: tam adres
}

std::wstring pick_file(HWND owner, const wchar_t* filter_name, const wchar_t* filter) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC spec[] = {{filter_name, filter}, {L"*.*", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// Birden fazla dosya seçimi (oynatma listesi, slayt gösterisi)
std::vector<std::wstring> pick_files(HWND owner, const wchar_t* filter_name, const wchar_t* filter) {
    std::vector<std::wstring> out;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_ALLOWMULTISELECT);
    COMDLG_FILTERSPEC spec[] = {{filter_name, filter}, {L"*.*", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    if (FAILED(dlg->Show(owner))) return out;
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dlg->GetResults(&items)) || FAILED(items->GetCount(&count))) return out;
    for (DWORD i = 0; i < count; i++) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out.push_back(path);
        CoTaskMemFree(path);
    }
    return out;
}

std::wstring pick_folder(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS);
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::string format_time(double seconds) {
    const int s = int(seconds);
    char text[32];
    sprintf_s(text, "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    return text;
}

}  // namespace

// --- Ayarlar ---

void App::load() {
    autostart_on_ = !autostart_read().empty();
    if (autostart_on_ && autostart_read() != autostart_command()) autostart_write(true);   // Klasör taşındıysa yeni yeri göstersin
    json data = read_json(data_dir() + L"\\settings.json");
    if (!data.is_object()) data = json::object();
    sidebar_open_ = data.value("sidebar_open", true);
    tray_note_shown_ = data.value("tray_note_shown", false);
    profile_ = data.value("profile", data.value("collection", std::string(T("Varsayılan"))));
    theme_mode_ = theme_mode_from(data);
    theme_style_ = data.value("theme_style", 0);
    layout_ = layout_load("studio", layout_defaults());
    sidebar_w_ = sidebar_open_ ? sidebar_open_w() : kMenuClosed;
    doc_ = data.value("doc", json::object());
    if (!doc_.contains("scenes") || !doc_["scenes"].is_array() || doc_["scenes"].empty()) {
        const std::string id = new_id();
        doc_["scenes"] = json::array({{{"id", id}, {"name", "Oyun"},
                                       {"sources", json::array({new_source("screen", 0, 0, 1, 1)})}}});
        doc_["current"] = id;
    }
    json social = default_social();
    social.merge_patch(doc_.value("social", json::object()));
    doc_["social"] = social;
    doc_.erase("crosshair");
    for (auto& s : doc_["scenes"]) {   // Crosshair ve FPS artık Oyun Araçları'nda; ekrandaki gösterge kayda zaten girer
        auto& list = s["sources"];
        for (size_t i = list.size(); i-- > 0;) {
            const std::string type = list[i].value("type", std::string());
            if (type == "crosshair" || type == "fps") list.erase(i);
        }
    }
    rec_ = Engine::default_record();
    rec_.merge_patch(data.value("record", json::object()));
    rec_["stream"]["key"] = unprotect_text(data.value("stream_key", std::string()));
    if (rec_.contains("stream_targets") && rec_["stream_targets"].is_array())
        for (auto& t : rec_["stream_targets"]) {
            t["key"] = unprotect_text(t.value("key_protected", std::string()));
            t.erase("key_protected");
        }
    json filters = default_mic_filters();
    filters.merge_patch(rec_.value("mic_filters", json::object()));
    rec_["mic_filters"] = filters;
    keys_ = data.value("hotkeys", json::object());
    std::lock_guard<std::mutex> g(hotkeys_lock_);
    for (auto& a : kActions) hotkeys_[a.key] = a.fallback;
    for (auto& [name, k] : keys_.items())       // Sahne ve kaynak kısayolları dahil
        if (k.is_object()) hotkeys_[name] = {k.value("vk", 0), k.value("ctrl", false), k.value("shift", false), k.value("alt", false)};
}

void App::save() {
    std::set<std::string> alive;    // Silinmiş sahne ve kaynakların kısayolları saklanmaz
    for (auto& s : doc_["scenes"]) {
        alive.insert("scene:" + s.value("id", std::string()));
        for (auto& src : s["sources"]) alive.insert("source:" + src.value("id", std::string()));
    }
    json keys = json::object();
    {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        for (auto& [name, h] : hotkeys_) {
            const bool dynamic = name.rfind("scene:", 0) == 0 || name.rfind("source:", 0) == 0;
            if (dynamic && (!alive.count(name) || !h.vk)) continue;
            keys[name] = {{"vk", h.vk}, {"ctrl", h.ctrl}, {"shift", h.shift}, {"alt", h.alt}};
        }
    }
    json record = rec_;
    record["stream"].erase("key");
    if (record.contains("stream_targets") && record["stream_targets"].is_array())     // Ek hedeflerin anahtarları da şifreli
        for (auto& t : record["stream_targets"]) {
            t["key_protected"] = protect_text(t.value("key", std::string()));
            t.erase("key");
        }
    write_json(data_dir() + L"\\settings.json", {{"doc", doc_}, {"record", record},
                                                {"stream_key", protect_text(rec_["stream"].value("key", std::string()))}, {"hotkeys", keys}, {"sidebar_open", sidebar_open_}, {"tray_note_shown", tray_note_shown_}, {"profile", profile_}, {"theme_light", theme_is_light()}, {"theme_mode", theme_mode()}, {"theme_style", theme_style()}});
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
        bool talk = false;
        for (auto& [name, h] : copy) {
            if (!h.vk) continue;
            if (name == "push_to_talk") {   // Basılı tutuldukça; oyunda Shift ile koşarken de çalışsın diye fazladan tuşa bakılmaz
                talk = key_down(h.vk) && (!h.ctrl || ctrl) && (!h.shift || shift) && (!h.alt || alt);
                continue;
            }
            const bool on = key_down(h.vk) && h.ctrl == ctrl && h.shift == shift && h.alt == alt;
            if (on && !held[name]) {
                {
                    std::lock_guard<std::mutex> g(hotkeys_lock_);
                    fired_.push_back(name);
                }
                PostMessageW(hwnd, WM_APP + 1, 0, 0);
            }
            held[name] = on;
        }
        engine_.set_push_to_talk(talk);
    }
}

void App::on_hotkeys() {
    std::vector<std::string> list;
    {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        list.swap(fired_);
    }
    if (!capturing_.empty()) return;
    for (const std::string& key : list) {
        if (key == "record") engine_.request_record(!status_.recording);
        else if (key == "pause") engine_.toggle_pause();
        else if (key == "stream") toggle_stream();
        else if (key == "screenshot") take_screenshot();
        else if (key == "replay_toggle") engine_.request_replay(!status_.replay_on);
        else if (key == "replay_save") engine_.request_replay_save();
        else if (key == "studio_transition") studio_transition();
        else if (key == "vcam_toggle") vcam_toggle();
        else if (key == "mute_mic" || key == "mute_desktop") toggle_mute(key == "mute_mic");
        else if (key.rfind("scene:", 0) == 0) {
            const std::string id = key.substr(6);
            for (auto& s : doc_["scenes"])
                if (s.value("id", std::string()) == id && doc_.value("current", std::string()) != id) {
                    doc_["current"] = id;
                    selected_.clear();
                    changed();
                }
        } else if (key.rfind("source:", 0) == 0) {
            const std::string id = key.substr(7);
            for (auto& s : doc_["scenes"])
                for (auto& src : s["sources"])
                    if (src.value("id", std::string()) == id) { src["visible"] = !src.value("visible", true); changed(); }
        }
    }
}

// Kayıt ve yayın sürerken de çalışır: ses yalnızca susturulur (karıştırıcı yumuşak geçer), kaynak kapanmaz
void App::toggle_mute(bool mic) {
    const char* field = mic ? "mic_muted" : "desktop_muted";
    rec_[field] = !rec_.value(field, false);
    rec_dirty_ = true;
    message_ = rec_[field].get<bool>() ? (mic ? "Mikrofon susturuldu" : "Bilgisayar sesi susturuldu")
                                       : (mic ? "Mikrofon açıldı" : "Bilgisayar sesi açıldı");
    message_error_ = false;
}

// Çarpı programı kapatmaz: kayıt, yayın ve kısayollar sürer; simge saatin yanındaki gizli simgelerde kalır
void App::hide_to_tray() {
    ShowWindow(hwnd, SW_HIDE);
    if (tray_note_shown_) return;
    tray_note_shown_ = true;
    save_dirty_ = true;
    save_at_ = now();
    tray_note(hwnd, "Kayıt Stüdyosu çalışmaya devam ediyor", "Kayıt, yayın ve kısayollar çalışır. Pencereyi açmak ya da programı kapatmak için saatin yanındaki simgeyi kullanın.");
}

void App::on_tray(UINT event) {
    if (event == WM_LBUTTONUP) { show_window(); return; }
    if (event != WM_RBUTTONUP) return;
    enum { kOpen = 1, kRecord, kStream, kShot, kDesktop, kMic, kExit };
    const int choice = tray_menu(hwnd, {{kOpen, "Aç"}, {0, ""},
                                        {kRecord, status_.recording ? "Kaydı durdur" : "Kaydı başlat"},
                                        {kStream, status_.streaming ? "Yayını durdur" : "Yayını başlat"},
                                        {kShot, "Ekran görüntüsü al"}, {0, ""},
                                        {kDesktop, rec_.value("desktop_muted", false) ? "Bilgisayar sesini aç" : "Bilgisayar sesini sustur"},
                                        {kMic, rec_.value("mic_muted", false) ? "Mikrofonu aç" : "Mikrofonu sustur"},
                                        {0, ""}, {kExit, "Çıkış"}});
    if (choice == kOpen) show_window();
    if (choice == kRecord) engine_.request_record(!status_.recording);
    if (choice == kStream) toggle_stream();
    if (choice == kShot) take_screenshot();
    if (choice == kDesktop) toggle_mute(false);
    if (choice == kMic) toggle_mute(true);
    if (choice == kExit) want_close = true;
}

void App::take_screenshot() {
    engine_.request_screenshot();
    message_ = "Ekran görüntüsü alınıyor...";
    message_error_ = false;
}

void App::toggle_fullscreen() {
    const DWORD style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO mi{sizeof(mi)};
        GetWindowPlacement(hwnd, &placement_);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &placement_);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
}

json& App::scene() {
    const std::string id = doc_.value("current", std::string());
    for (auto& s : doc_["scenes"])
        if (s.value("id", std::string()) == id) return s;
    return doc_["scenes"][0];
}

json* App::source(const std::string& id) {
    for (auto& s : scene()["sources"])
        if (s.value("id", std::string()) == id) return &s;
    return nullptr;
}

void App::add_source(const std::string& type) {
    int W, H;
    engine_.canvas_size(W, H);
    const float w = 0.3f, h = w * W * 9 / 16 / H;
    json s;
    if (type == "camera") s = new_source(type, 0.68f, 0.66f, w, h);
    else if (type == "image" || type == "video" || type == "slideshow") s = new_source(type, 0.35f, 0.3f, w, h);
    else if (type == "color") s = new_source(type, 0.35f, 0.4f, 0.3f, 0.2f);
    else if (type == "text") s = new_source(type, 0.05f, 0.05f, 0, 0);
    else if (type == "social") s = new_source(type, 0.02f, 0.75f, 0, 0);
    else if (type == "fps_meter") s = new_source(type, 0.01f, 0.02f, 0, 0);
    else s = new_source(type, 0, 0, 1, 1);
    if (type == "camera" && cameras_.empty()) cameras_ = list_cameras();
    selected_ = s["id"];
    scene()["sources"].push_back(s);
    changed();
    if (type == "region") pending_region_ = selected_;     // Eklenir eklenmez bölge seçtirilir
    if (type == "window") show_properties_ = true;          // Pencere seçilsin
}

// Bölge kaynağı için ekranda fareyle seçim; seçilen bölgenin oranı kaynağın kutusuna da uygulanır
void App::region_now(const std::string& id) {
    json* src = nullptr;
    for (auto& s : doc_["scenes"])
        for (auto& c : s["sources"])
            if (c.value("id", std::string()) == id) src = &c;
    if (!src) return;
    json& s = (*src)["settings"];
    const auto rects = engine_.monitor_rects();
    const int m = std::clamp(s.value("monitor", 0), 0, std::max(0, int(rects.size()) - 1));
    if (rects.empty()) return;
    const bool was_minimized = IsIconic(hwnd) != 0;
    ShowWindow(hwnd, SW_MINIMIZE);      // Kendi penceremiz seçilecek bölgenin önünde kalmasın
    Sleep(250);
    RECT r;
    const bool ok = pick_region(rects[m], r);
    if (!was_minimized) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
    if (!ok) return;
    s["region"] = {{"x", r.left}, {"y", r.top}, {"w", r.right - r.left}, {"h", r.bottom - r.top}};
    int W, H;
    engine_.canvas_size(W, H);
    float w = src->value("w", 1.f), h = w * W * (r.bottom - r.top) / (r.right - r.left) / H;
    if (h > 1) { w /= h; h = 1; }
    (*src)["w"] = w; (*src)["h"] = h;
    (*src)["x"] = std::min(src->value("x", 0.f), 1 - w);
    (*src)["y"] = std::min(src->value("y", 0.f), 1 - h);
    changed();
}

void App::open_settings(const std::string& tab) {
    speakers_ = list_audio_devices(false);
    mics_ = list_audio_devices(true);
    settings_tab_ = tab;
    page_ = "settings";
}

void App::open_record_folder() {
    const std::wstring folder = widen(rec_.value("folder", std::string(".")));
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Sayfalar ---

void App::sidebar() {
    // Sahneler ve kaynaklar; sütun daraltılınca önizlemeye yer açılır
    const bool wide = sidebar_w_ > 180;
    if (ImGui::Button(ICON_MENU "###daralt", ImVec2(kMenuClosed - 8, 32))) {
        sidebar_open_ = !sidebar_open_;
        save_dirty_ = true;
        save_at_ = now() + 0.3;
    }
    ImGui::SetItemTooltip("%s", sidebar_open_ ? "Sol paneli daralt" : "Sol paneli aç");
    if (wide) {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float h = ImGui::GetContentRegionAvail().y - 36 - 2 * spacing;
        begin_panel("Sahneler", ImVec2(0, h * 0.4f));
        panel_scenes();
        ImGui::EndChild();
        begin_panel("Kaynaklar", ImVec2(0, h * 0.6f));
        panel_sources();
        ImGui::EndChild();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 36);
    if (nav_button(ICON_SETTINGS, "Ayarlar", "ayarlar", false, wide)) open_settings("record");
}

void App::toggle_stream() {
    if (status_.streaming) {
        engine_.request_stream(false);
        return;
    }
    const json& st = rec_["stream"];
    if (st.value("server", std::string()).empty() || st.value("key", std::string()).empty()) {
        open_settings("stream");
        message_ = "Yayın için önce sunucu ve yayın anahtarını girin";
        message_error_ = true;
        return;
    }
    engine_.request_stream(true);
}


// Video Düzenleyici açıksa öne gelir (ve kaydı alır), değilse başlatılır
void App::open_editor(bool with_last) {
    const std::wstring file = with_last ? status_.last_path : std::wstring();
    if (HWND other = FindWindowW(L"VideoDuzenleyici", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        if (!file.empty()) {
            COPYDATASTRUCT data{1, DWORD((file.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(file.c_str())};
            SendMessageW(other, WM_COPYDATA, WPARAM(hwnd), LPARAM(&data));
        }
        PostMessageW(other, RegisterWindowMessageW(L"VideoDuzenleyiciGoster"), 0, 0);
        return;
    }
    const std::wstring exe = exe_dir() + L"\\Video Editor.exe", args = file.empty() ? L"" : L"\"" + file + L"\"";
    if (INT_PTR(ShellExecuteW(hwnd, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), exe_dir().c_str(), SW_SHOWNORMAL)) <= 32) {
        message_ = "Video Düzenleyici açılamadı";
        message_error_ = true;
    }
}

void App::main_menu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Dosya")) {
        if (ImGui::MenuItem("Kayıt klasörünü aç")) open_record_folder();
        if (ImGui::MenuItem("Ayarlar")) open_settings("record");
        ImGui::Separator();
        if (ImGui::MenuItem("Çıkış")) want_close = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Görünüm")) {
        if (ImGui::MenuItem("Tam ekran", "F11", fullscreen_)) toggle_fullscreen();
        if (ImGui::MenuItem("Tema seçimi...")) open_settings("theme");
        if (ImGui::MenuItem("Sol panel", nullptr, sidebar_open_)) {
            sidebar_open_ = !sidebar_open_;
            save_dirty_ = true;
            save_at_ = now() + 0.3;
        }
        if (ImGui::MenuItem("Panel boyutlarını sıfırla")) { layout_ = layout_defaults(); layout_save("studio", layout_); }
        ImGui::EndMenu();
    }
    profiles_menu();
    if (ImGui::BeginMenu("Araçlar")) {
        if (ImGui::MenuItem("Video Düzenleyici'yi aç")) open_editor(false);
        if (ImGui::MenuItem("Son kaydı Video Düzenleyici'de aç", nullptr, false, !status_.last_path.empty())) open_editor(true);
        if (ImGui::MenuItem("Ekran görüntüsü al")) take_screenshot();
        if (ImGui::MenuItem("Tekrarı kaydet", nullptr, false, status_.replay_on)) engine_.request_replay_save();
        if (ImGui::MenuItem("İstatistikler", nullptr, show_stats_)) show_stats_ = !show_stats_;
        if (ImGui::MenuItem("Sanal kamera", nullptr, status_.vcam_state != 0)) vcam_toggle();
        if (ImGui::MenuItem("Sanal kamerayı kaldır", nullptr, false, vcam_registered() && !status_.vcam_state)) {
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            ShellExecuteW(hwnd, L"runas", exe, L"--vcam-uninstall", nullptr, SW_HIDE);
        }
        if (ImGui::MenuItem("Mikrofon ayarları")) { show_mic_ = true; page_ = "record"; }
        chat_menu();
        ImGui::Separator();
        if (ImGui::MenuItem("Kısayollar")) open_settings("hotkeys");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Yardım")) {
        if (ImGui::MenuItem("Kullanım Kılavuzu", "F1")) manual_open();
        if (ImGui::MenuItem("İletişim")) { help_request_ = 1; open_settings("help"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Geliştiriciler")) { help_request_ = 2; open_settings("help"); }
        ImGui::Separator();
        if (ImGui::MenuItem("Lisans Sözleşmesi")) { help_request_ = 3; open_settings("help"); }
        if (ImGui::MenuItem("Gizlilik")) { help_request_ = 4; open_settings("help"); }
        if (ImGui::MenuItem("Telif Hakları")) { help_request_ = 5; open_settings("help"); }
        ImGui::EndMenu();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 190);
    if (ImGui::Button("\xEE\x9C\x8F  Video Düzenleyici", ImVec2(182, 0))) open_editor(false);
    ImGui::SetItemTooltip("Video Düzenleyici'ye geç. Son kaydı açmak için Araçlar menüsünü kullanın.");
    ImGui::EndMenuBar();
}

void App::status_bar() {
    ImGui::AlignTextToFramePadding();
    if (message_error_) ImGui::TextColored(theme_error(), "%s", T(message_.c_str()));
    else ImGui::TextDisabled("%s", T(message_.c_str()));
    int W, H;
    engine_.canvas_size(W, H);
    char text[128];
    sprintf_s(text, "%d x %d  ·  %d FPS", W, H, rec_.value("fps", 60));
    std::string right = text;
    if (status_.recording) {
        const double pct = status_.frames ? 100.0 * status_.repeated / status_.frames : 0;
        right = TF("Atlanan kare %lld (%%%.1f)  ·  ", status_.repeated, pct) + right;
    }
    if (status_.streaming) {
        right = TF("Yayın %.0f kbps  ·  Atlanan %lld  ·  ", status_.stream_kbps, status_.stream_dropped) + right;
    }
    if (status_.recording && status_.record_part) right = TF("Parça %d  ·  ", status_.record_part) + right;
    if (status_.replay_on) right = TF("Tekrar arabelleği %d sn  ·  ", status_.replay_seconds) + right;
    if (status_.vcam_state) right = std::string(T("Sanal kamera")) + "  ·  " + right;
    if (status_.streaming && !status_.stream_targets.empty()) right = TF("+%d hedef  ·  ", int(status_.stream_targets.size())) + right;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(right.c_str()).x);
    ImGui::TextDisabled("%s", right.c_str());
}

void App::panel_transition() {
    ImGui::SetNextItemWidth(-1);
    if (edit_choice("##gecis", rec_, "transition", {{"fade", "Soldur"}, {"cut", "Kes"}})) rec_dirty_ = true;
    ImGui::TextDisabled("Süre: 300 ms");
    ImGui::Spacing();
    bool studio = doc_.value("studio", false);
    if (ImGui::Checkbox("Stüdyo modu", &studio)) {
        doc_["studio"] = studio;
        doc_["program"] = doc_.value("current", std::string());
        changed();
    }
    ImGui::SetItemTooltip("Sahneyi önce solda hazırlayın, hazır olunca \"Yayına al\" ile kayda ve yayına geçirin.");
    if (studio) {
        const bool same = doc_.value("program", std::string()) == doc_.value("current", std::string());
        ImGui::BeginDisabled(same);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(220, 53, 69, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(235, 70, 85, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
        if (ImGui::Button("Yayına al  \xEE\x9C\xAA", ImVec2(-1, 36))) studio_transition();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
}

void App::update_preview_texture() {
    HANDLE handle = engine_.preview_handle();
    if (handle == preview_handle_) return;
    preview_handle_ = handle;
    preview_srv_.Reset();
    ComPtr<ID3D11Texture2D> tex;
    if (handle && SUCCEEDED(device_->OpenSharedResource(handle, IID_PPV_ARGS(&tex))))
        device_->CreateShaderResourceView(tex.Get(), nullptr, &preview_srv_);
}

void App::preview_input(ImVec2 origin, float f) {
    int W, H;
    engine_.canvas_size(W, H);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float cx = (mouse.x - origin.x) / f, cy = (mouse.y - origin.y) / f;
    json* sel = source(selected_);
    auto box_of = [&](const std::string& id) { auto it = boxes_.find(id); return it == boxes_.end() ? Rect{} : it->second; };

    auto corner_at = [&]() -> int {
        if (!sel || !type_of(sel->value("type", std::string())).resizable) return -1;
        const Rect b = box_of(selected_);
        const float xs[4] = {b.x, b.x + b.w, b.x, b.x + b.w}, ys[4] = {b.y, b.y, b.y + b.h, b.y + b.h};
        for (int i = 0; i < 4; i++)
            if (fabsf(origin.x + xs[i] * f - mouse.x) <= kHandle + 2 && fabsf(origin.y + ys[i] * f - mouse.y) <= kHandle + 2)
                return i;
        return -1;
    };

    if (ImGui::IsItemHovered()) {
        const int corner = corner_at();
        if (corner >= 0) ImGui::SetMouseCursor(corner == 0 || corner == 3 ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW);
    }
    if (ImGui::IsItemActivated()) {
        drag_.mode = 0;
        const int corner = corner_at();
        if (corner >= 0) {
            const Rect b = box_of(selected_);
            drag_.mode = 2;
            drag_.ax = corner == 0 || corner == 2 ? b.x + b.w : b.x;
            drag_.ay = corner == 0 || corner == 1 ? b.y + b.h : b.y;
            drag_.aspect = b.w / std::max(b.h, 1.f);
            drag_.top = corner < 2;
        } else {
            auto& list = scene()["sources"];
            for (int i = int(list.size()) - 1; i >= 0; i--) {   // Öndeki kaynak önce
                const std::string id = list[i].value("id", std::string());
                const Rect b = box_of(id);
                if (list[i].value("visible", true) && cx >= b.x && cx < b.x + b.w && cy >= b.y && cy < b.y + b.h) {
                    selected_ = id;
                    drag_ = {1, cx - b.x, cy - b.y, b.w, b.h};
                    break;
                }
            }
        }
    }
    // Sağ tık: tıklanan kaynak seçilir ve türüne göre menü açılır
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        auto& list = scene()["sources"];
        std::string hit;
        for (int i = int(list.size()) - 1; i >= 0 && hit.empty(); i--) {
            const std::string id = list[i].value("id", std::string());
            const Rect b = box_of(id);
            if (list[i].value("visible", true) && cx >= b.x && cx < b.x + b.w && cy >= b.y && cy < b.y + b.h) hit = id;
        }
        if (!hit.empty()) { selected_ = hit; ImGui::OpenPopup("onizleme_sag"); }
    }
    if (ImGui::BeginPopup("onizleme_sag")) {
        source_menu(selected_);
        ImGui::EndPopup();
    }
    sel = source(selected_);
    // Çift tıklama: tıklanan kaynağın özellikleri açılır (ilk tıklama onu zaten seçmiştir)
    if (sel && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const Rect b = box_of(selected_);
        if (cx >= b.x && cx < b.x + b.w && cy >= b.y && cy < b.y + b.h) {
            drag_.mode = 0;
            show_properties_ = true;
        }
    }
    if (ImGui::IsItemActive() && drag_.mode && sel && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
        const float snap = kSnap / f;
        if (drag_.mode == 1) {
            float x = cx - drag_.gx, y = cy - drag_.gy;
            for (float t : {0.f, W - drag_.w, (W - drag_.w) / 2})   // Kenarlara ve ortaya yapış
                if (fabsf(x - t) < snap) x = t;
            for (float t : {0.f, H - drag_.h, (H - drag_.h) / 2})
                if (fabsf(y - t) < snap) y = t;
            (*sel)["x"] = x / W;
            (*sel)["y"] = y / H;
        } else {
            const float w = std::max(fabsf(cx - drag_.ax), kMinSize);
            const float h = std::max(w / drag_.aspect, kMinSize);   // Oran korunur
            (*sel)["x"] = (cx < drag_.ax ? drag_.ax - w : drag_.ax) / W;
            (*sel)["y"] = (drag_.top ? drag_.ay - h : drag_.ay) / H;
            (*sel)["w"] = w / W;
            (*sel)["h"] = h / H;
        }
        changed();
    }
}

// Stüdyo modunda önizleme ikiye bölünür: solda düzenlenen sahne, sağda yayındaki sahne
void App::panel_preview(ImVec2 size) {
    if (doc_.value("studio", false)) {
        const float gap = 12, half = (size.x - gap) / 2, label = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::TextDisabled("Önizleme (düzenlenen sahne)");
        ImGui::SetCursorPos(ImVec2(start.x + half + gap, start.y));
        ImGui::TextColored(theme_error(), "%s", T("Yayında"));
        ImGui::SetCursorPos(ImVec2(start.x, start.y + label));
        preview_area(ImVec2(half, size.y - label));
        ImGui::SetCursorPos(ImVec2(start.x + half + gap, start.y + label));
        panel_program(ImVec2(half, size.y - label));
        ImGui::SetCursorPos(ImVec2(start.x, start.y + size.y));
        ImGui::Dummy(ImVec2(size.x, 0));
        return;
    }
    preview_area(size);
}

void App::panel_program(ImVec2 size) {
    const HANDLE handle = engine_.program_handle();
    if (handle != program_handle_) {
        program_handle_ = handle;
        program_srv_.Reset();
        ComPtr<ID3D11Texture2D> tex;
        if (handle && SUCCEEDED(device_->OpenSharedResource(handle, IID_PPV_ARGS(&tex))))
            device_->CreateShaderResourceView(tex.Get(), nullptr, &program_srv_);
    }
    int W, H;
    engine_.canvas_size(W, H);
    const float f = std::min(size.x / W, size.y / H);
    const ImVec2 start = ImGui::GetCursorScreenPos(), image(W * f, H * f);
    const ImVec2 origin(start.x + (size.x - image.x) / 2, start.y + (size.y - image.y) / 2);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y), IM_COL32(0, 0, 0, 255));
    if (program_srv_) draw->AddImage((ImTextureID)(intptr_t)program_srv_.Get(), origin, ImVec2(origin.x + image.x, origin.y + image.y));
    draw->AddRect(origin, ImVec2(origin.x + image.x, origin.y + image.y), IM_COL32(220, 53, 69, 255), 0.f, ImDrawFlags_None, 2.f);
    ImGui::Dummy(size);
}

// --- Profiller: sahneler ve kaynaklar, kayıt, yayın, ses, tema ve panel düzeni tek pakette ---
// Şu anki durum settings.json'dadır; profile geçerken o, kendi dosyasına (data_dir\Profiles\ad.json) yazılır.

std::wstring App::profile_file(const std::string& name) {
    std::wstring safe = widen(name);
    for (auto& c : safe)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    return data_dir() + L"\\Profiles\\" + safe + L".json";
}

std::vector<std::string> App::profiles() {
    std::vector<std::string> out{profile_};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(data_dir() + L"\\Profiles", ec)) {
        if (e.path().extension() != L".json") continue;
        const json j = read_json(e.path().wstring());
        const std::string name = j.is_object() ? j.value("profile_name", narrow(e.path().stem().wstring())) : narrow(e.path().stem().wstring());
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin() + 1, out.end());
    return out;
}

// Şu anki her şey (yayın anahtarları şifreli)
json App::profile_capture() {
    json record = rec_;
    record["stream"]["key_protected"] = protect_text(rec_["stream"].value("key", std::string()));
    record["stream"].erase("key");
    if (record.contains("stream_targets") && record["stream_targets"].is_array())
        for (auto& t : record["stream_targets"]) {
            t["key_protected"] = protect_text(t.value("key", std::string()));
            t.erase("key");
        }
    return {{"profile_name", profile_}, {"doc", doc_}, {"record", record}, {"theme_style", theme_style()}, {"theme_mode", theme_mode()}, {"layout", layout_}};
}

void App::profile_store() {
    SHCreateDirectoryExW(nullptr, (data_dir() + L"\\Profiles").c_str(), nullptr);
    write_json(profile_file(profile_), profile_capture());
}

void App::profile_load(const json& p) {
    json doc = p.value("doc", json::object());
    if (!doc.is_object() || !doc.contains("scenes") || !doc["scenes"].is_array() || doc["scenes"].empty()) {
        const std::string id = new_id();
        doc = {{"scenes", json::array({{{"id", id}, {"name", T("Sahne")}, {"sources", json::array()}}})}, {"current", id}};
    }
    json social = default_social();
    social.merge_patch(doc.value("social", json::object()));
    doc["social"] = social;
    doc_ = doc;
    if (p.contains("record") && p["record"].is_object()) {
        json rec = Engine::default_record();
        rec.merge_patch(p["record"]);
        rec["stream"]["key"] = unprotect_text(rec["stream"].value("key_protected", std::string()));
        rec["stream"].erase("key_protected");
        if (rec.contains("stream_targets") && rec["stream_targets"].is_array())
            for (auto& t : rec["stream_targets"]) {
                t["key"] = unprotect_text(t.value("key_protected", std::string()));
                t.erase("key_protected");
            }
        json filters = default_mic_filters();
        filters.merge_patch(rec.value("mic_filters", json::object()));
        rec["mic_filters"] = filters;
        rec_ = rec;
        rec_dirty_ = true;
    }
    if (p.contains("theme_style") && p.value("theme_style", 0) != theme_style()) theme_set_style(p.value("theme_style", 0));
    if (p.contains("theme_mode") && p.value("theme_mode", 0) != theme_mode()) theme_set_mode(p.value("theme_mode", 0));
    if (p.contains("layout") && p["layout"].is_object()) {
        json layout = layout_defaults();
        layout.merge_patch(p["layout"]);
        layout_ = layout;
        layout_save("studio", layout_);
    }
    selected_.clear();
    show_properties_ = false;
    changed();
    save_dirty_ = true;
    save_at_ = now();
}

void App::profile_switch(const std::string& name) {
    if (name == profile_) return;
    profile_store();
    json next = read_json(profile_file(name));
    if (!next.is_object()) {        // Yeni profil: şu anki ayarlar, boş bir sahne
        next = profile_capture();
        next.erase("doc");
    }
    profile_ = name;
    profile_load(next);
    message_ = TF("Profil: %s", name.c_str());
    message_error_ = false;
}

// İlk açılışta: profil klasörü yoksa şu anki durum ve eski sahne koleksiyonları profile dönüşür
void App::profiles_migrate() {
    const std::wstring dir = data_dir() + L"\\Profiles";
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(data_dir() + L"\\Scene Collections", ec)) {
        if (e.path().extension() != L".json") continue;
        json old = read_json(e.path().wstring());
        if (!old.is_object()) continue;
        const std::string name = old.value("collection_name", narrow(e.path().stem().wstring()));
        if (name == profile_) continue;
        old.erase("collection_name");
        json p = profile_capture();
        p["profile_name"] = name;
        p["doc"] = old;
        write_json(profile_file(name), p);
    }
    profile_store();
}

void App::profile_delete() {
    const auto all = profiles();
    if (all.size() < 2) return;
    const std::string gone = profile_;
    std::string next;
    for (auto& n : all) if (n != gone) { next = n; break; }
    profile_switch(next);
    DeleteFileW(profile_file(gone).c_str());
}

void App::profile_export() {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC spec[] = {{L"JSON", L"*.json"}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(L"json");
    dlg->SetFileName(widen(profile_).c_str());
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(hwnd)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        json out = profile_capture();
        // Yayın anahtarları bu bilgisayarın Windows hesabına bağlı şifrelidir; başka bilgisayarda açılmaz, dışarı verilmez
        out["record"]["stream"].erase("key_protected");
        if (out["record"].contains("stream_targets")) for (auto& t : out["record"]["stream_targets"]) t.erase("key_protected");
        write_json(path, out);
        message_ = TF("Dışa aktarıldı: %s", narrow(path).c_str());
        message_error_ = false;
    }
    CoTaskMemFree(path);
}

void App::profile_import() {
    const std::wstring path = pick_file(hwnd, L"JSON", L"*.json");
    if (path.empty()) return;
    json in = read_json(path);
    if (in.is_object() && in.contains("scenes") && !in.contains("doc")) {     // Eski sahne koleksiyonu dosyası
        json doc = in;
        doc.erase("collection_name");
        in = {{"profile_name", in.value("collection_name", std::string())}, {"doc", doc}};
    }
    if (!in.is_object() || !in.contains("doc")) {
        message_ = "Bu dosya bir profil değil";
        message_error_ = true;
        return;
    }
    const std::string base = in.value("profile_name", std::string()).empty() ? narrow(std::filesystem::path(path).stem().wstring()) : in.value("profile_name", std::string());
    std::string name = base;
    const auto existing = profiles();
    for (int i = 2; std::find(existing.begin(), existing.end(), name) != existing.end(); i++) name = base + " " + std::to_string(i);
    in["profile_name"] = name;
    SHCreateDirectoryExW(nullptr, (data_dir() + L"\\Profiles").c_str(), nullptr);
    write_json(profile_file(name), in);
    profile_switch(name);
}

void App::profiles_menu() {
    if (!ImGui::BeginMenu("Profil")) return;
    const bool busy = status_.recording || status_.streaming;
    for (auto& name : profiles())
        if (ImGui::MenuItem(name.c_str(), nullptr, name == profile_, !busy || name == profile_) && name != profile_) profile_switch(name);
    ImGui::Separator();
    if (ImGui::MenuItem("Yeni profil...", nullptr, false, !busy)) { profile_action_ = "new"; profile_name_[0] = 0; }
    if (ImGui::MenuItem("Çoğalt...", nullptr, false, !busy)) { profile_action_ = "dup"; strncpy_s(profile_name_, (profile_ + " 2").c_str(), _TRUNCATE); }
    if (ImGui::MenuItem("Profilleri yönet...")) open_settings("profiles");
    ImGui::EndMenu();
}

void App::profile_popup() {
    if (!profile_action_.empty()) ImGui::OpenPopup("Profil adı");
    if (!ImGui::BeginPopupModal("Profil adı", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static std::string pending;
    if (!profile_action_.empty()) { pending = profile_action_; profile_action_.clear(); }
    ImGui::TextUnformatted(pending == "rename" ? "Yeni ad:" : pending == "new" ? "Yeni profilin adı (şu anki ayarlarla, boş bir sahneyle başlar):" : "Kopyanın adı:");
    ImGui::SetNextItemWidth(360);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##profil", profile_name_, sizeof(profile_name_), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string name = profile_name_;
    const auto all = profiles();
    const bool taken = std::find(all.begin(), all.end(), name) != all.end() && name != profile_;
    if (taken) ImGui::TextColored(theme_error(), "Bu ad kullanılıyor.");
    ImGui::BeginDisabled(name.empty() || taken);
    if (ImGui::Button("Tamam", ImVec2(120, 0)) || (enter && !name.empty() && !taken)) {
        SHCreateDirectoryExW(nullptr, (data_dir() + L"\\Profiles").c_str(), nullptr);
        if (pending == "new") {
            profile_switch(name);
        } else if (pending == "dup") {
            profile_store();
            json copy = profile_capture();
            copy["profile_name"] = name;
            write_json(profile_file(name), copy);
            profile_switch(name);
        } else if (pending == "rename" && name != profile_) {
            DeleteFileW(profile_file(profile_).c_str());
            profile_ = name;
            profile_store();
            save_dirty_ = true;
            save_at_ = now();
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Ayarlar > Profiller: hepsi tek yerde
void App::page_profiles() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Profiller");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Bir profil her şeyi birlikte saklar: sahneler ve kaynaklar, kayıt ve yayın ayarları, ses ayarları, tema ve panel boyutları. "
                       "Farklı işler için ayrı profiller açın (örneğin oyun yayını, eğitim videosu, sohbet). Değişiklikler açık profile kendiliğinden kaydedilir.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const bool busy = status_.recording || status_.streaming;
    if (busy) ImGui::TextColored(theme_warn(), "Kayıt ya da yayın sürerken profil değiştirilemez.");
    ImGui::BeginChild("profil_liste", ImVec2(0, 0));
    for (auto& name : profiles()) {
        ImGui::PushID(name.c_str());
        const bool on = name == profile_;
        ImGui::AlignTextToFramePadding();
        if (on) ImGui::TextColored(theme_accent(), "%s", name.c_str());
        else ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine(320);
        if (on) {
            ImGui::TextDisabled("Kullanılıyor");
        } else {
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Geç", ImVec2(110, 0))) profile_switch(name);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Açık profil");
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Yeni profil...", ImVec2(160, 0))) { profile_action_ = "new"; profile_name_[0] = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Çoğalt...", ImVec2(120, 0))) { profile_action_ = "dup"; strncpy_s(profile_name_, (profile_ + " 2").c_str(), _TRUNCATE); }
    ImGui::SameLine();
    if (ImGui::Button("Yeniden adlandır...", ImVec2(170, 0))) { profile_action_ = "rename"; strncpy_s(profile_name_, profile_.c_str(), _TRUNCATE); }
    ImGui::SameLine();
    if (ImGui::Button("Sil", ImVec2(90, 0)) && profiles().size() > 1) profile_delete();
    ImGui::EndDisabled();
    if (ImGui::Button("Dışa aktar...", ImVec2(160, 0))) profile_export();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("İçe aktar...", ImVec2(120, 0))) profile_import();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Dışa aktarılan profile yayın anahtarları girmez; başka bilgisayarda yeniden yazılır. Kısayol tuşları bütün profillerde ortaktır.");
    ImGui::EndChild();
}
// Gruplama: seçilen kaynaklar yeni bir grup sahnesine taşınır, yerlerine tek bir "Sahne" kaynağı gelir
// Grup kaynağı çözülür: kaynakları sahneye, grubun yerine döner
void App::ungroup(const std::string& id) {
    auto& list = scene()["sources"];
    json* sel = source(id);
    if (!sel || sel->value("type", std::string()) != "scene") return;
    const std::string gid = (*sel)["settings"].value("scene", std::string());
    json children;
    for (auto& sc : doc_["scenes"])
        if (sc.value("id", std::string()) == gid && sc.value("group", false)) children = sc["sources"];
    if (!children.is_array()) return;
    const float gx = sel->value("x", 0.f), gy = sel->value("y", 0.f), gw = sel->value("w", 1.f), gh = sel->value("h", 1.f);
    int at = 0;
    for (int i = 0; i < int(list.size()); i++) if (list[i].value("id", std::string()) == id) at = i;
    list.erase(at);
    for (auto& c : children) {      // Grubun kutusu çocukların konumuna işlenir
        c["x"] = gx + c.value("x", 0.f) * gw; c["y"] = gy + c.value("y", 0.f) * gh;
        c["w"] = c.value("w", 1.f) * gw; c["h"] = c.value("h", 1.f) * gh;
        list.insert(list.begin() + at++, c);
    }
    auto& scenes = doc_["scenes"];
    for (size_t i = 0; i < scenes.size(); i++) if (scenes[i].value("id", std::string()) == gid) { scenes.erase(i); break; }
    selected_.clear();
    changed();
}

void App::rename_popup() {
    if (rename_open_) { ImGui::OpenPopup("Yeniden adlandır"); rename_open_ = false; }
    if (!ImGui::BeginPopupModal("Yeniden adlandır", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(320);
    const bool enter = ImGui::InputText("##ad", rename_buf_, sizeof(rename_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string name = rename_buf_;
    const bool ok = name.find_first_not_of(' ') != std::string::npos;
    ImGui::BeginDisabled(!ok);
    if ((ImGui::Button("Tamam", ImVec2(150, 30)) || enter) && ok) {
        if (json* s = source(rename_id_)) { (*s)["name"] = name; changed(); }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(150, 30))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::source_menu(const std::string& id) {
    auto& list = scene()["sources"];
    int index = -1;
    for (int i = 0; i < int(list.size()); i++) if (list[i].value("id", std::string()) == id) index = i;
    if (index < 0) return;
    json& src = list[index];
    json& s = src["settings"];
    const std::string type = src.value("type", std::string());
    const SourceType& kind = type_of(type);
    ImGui::TextDisabled("%s  (%s)", src.value("name", std::string()).c_str(), T(kind.name));
    ImGui::Separator();
    if (ImGui::MenuItem("Özellikler...")) show_properties_ = true;
    auto toggle = [&](const char* label, const char* key, bool fallback) {
        if (ImGui::MenuItem(label, nullptr, s.value(key, fallback))) { s[key] = !s.value(key, fallback); changed(); }
    };
    // Türe göre
    if (type == "screen" || type == "region") {
        const auto monitors = engine_.monitors();
        if (monitors.size() > 1 && ImGui::BeginMenu("Ekranı değiştir")) {
            for (int i = 0; i < int(monitors.size()); i++)
                if (ImGui::MenuItem(monitors[i].c_str(), nullptr, s.value("monitor", 0) == i)) { s["monitor"] = i; changed(); }
            ImGui::EndMenu();
        }
    }
    if (type == "region" && ImGui::MenuItem("Bölgeyi ekranda yeniden seç")) pending_region_ = id;
    if (type == "window" || type == "game") {
        if (ImGui::MenuItem("Pencereyi değiştir...")) show_properties_ = true;
    }
    if (type == "screen" || type == "region" || type == "window" || type == "game") toggle("İmleci göster", "cursor", type != "game");
    if (type == "window") toggle("Yalnızca pencere içi (kenarlıksız)", "client", true);
    if (type == "camera") {
        toggle("Aynala", "mirror", false);
        if (ImGui::MenuItem("Kamerayı değiştir...")) show_properties_ = true;
    }
    if (type == "image" && ImGui::MenuItem("Resmi değiştir...")) {
        const auto files = pick_files(hwnd, TW("Resimler").c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff");
        if (!files.empty()) { s["file"] = narrow(files[0]); changed(); }
    }
    if (type == "video") {
        if (ImGui::MenuItem("Videoyu değiştir...")) {
            const auto files = pick_files(hwnd, TW("Videolar").c_str(), L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.m4v");
            if (!files.empty()) { s["file"] = narrow(files[0]); s["playlist"] = json::array(); changed(); }
        }
        toggle("Döngü", "loop", true);
        toggle("Karışık sıra", "shuffle", false);
    }
    if (type == "slideshow") {
        if (ImGui::MenuItem("Resim ekle...")) {
            if (!s["files"].is_array()) s["files"] = json::array();
            for (auto& f : pick_files(hwnd, TW("Resimler").c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff")) s["files"].push_back(narrow(f));
            changed();
        }
        toggle("Karışık sıra", "shuffle", false);
        toggle("Geçişte sol", "fade", true);
    }
    if (type == "text") {
        if (ImGui::MenuItem("Metni düzenle...")) show_properties_ = true;
        toggle("Arka plan", "background", false);
        toggle("Kenar çizgisi", "outline", true);
    }
    if (type == "browser") {
        if (ImGui::MenuItem("Sayfayı yenile")) { s["reload"] = s.value("reload", 0) + 1; changed(); }
        toggle("Sesi kapat", "muted", false);
    }
    if (type == "color" && ImGui::MenuItem("Rengi değiştir...")) show_properties_ = true;
    if (type == "scene") {
        const std::string target = s.value("scene", std::string());
        bool group = false;
        for (auto& sc : doc_["scenes"]) if (sc.value("id", std::string()) == target) group = sc.value("group", false);
        if (group) {
            if (ImGui::MenuItem("Grubun içini düzenle")) { doc_["current"] = target; selected_.clear(); changed(); return; }
            if (ImGui::MenuItem("Grubu çöz")) { ungroup(id); return; }
        } else if (!target.empty() && ImGui::MenuItem("Bu sahneye geç")) {
            doc_["current"] = target;
            selected_.clear();
            changed();
            return;
        }
    }
    ImGui::Separator();
    // Ortak
    if (ImGui::MenuItem("Yeniden adlandır...")) {
        rename_id_ = id;
        strncpy_s(rename_buf_, src.value("name", std::string()).c_str(), _TRUNCATE);
        rename_open_ = true;
    }
    if (ImGui::MenuItem(src.value("visible", true) ? "Gizle" : "Göster")) { src["visible"] = !src.value("visible", true); changed(); }
    if (ImGui::MenuItem("Çoğalt")) {
        json copy = src;
        copy["id"] = new_id();
        copy["name"] = src.value("name", std::string()) + " (2)";
        copy["x"] = src.value("x", 0.f) + 0.02f;
        copy["y"] = src.value("y", 0.f) + 0.02f;
        list.insert(list.begin() + index + 1, copy);
        selected_ = copy["id"];
        changed();
        return;
    }
    if (ImGui::BeginMenu("Sırala")) {
        const int last = int(list.size()) - 1;
        int to = -1;
        if (ImGui::MenuItem("En öne getir", nullptr, false, index < last)) to = last;
        if (ImGui::MenuItem("Bir öne al", nullptr, false, index < last)) to = index + 1;
        if (ImGui::MenuItem("Bir arkaya al", nullptr, false, index > 0)) to = index - 1;
        if (ImGui::MenuItem("En arkaya gönder", nullptr, false, index > 0)) to = 0;
        ImGui::EndMenu();
        if (to >= 0) {
            json item = list[index];
            list.erase(index);
            list.insert(list.begin() + std::clamp(to, 0, int(list.size())), item);
            changed();
            return;
        }
    }
    if (ImGui::BeginMenu("Konum ve boyut")) {
        int W, H;
        engine_.canvas_size(W, H);
        const float w = src.value("w", 1.f), h = src.value("h", 1.f);
        if (kind.resizable) {
            if (ImGui::MenuItem("Ekrana sığdır (oran korunur)") && w > 0 && h > 0) {
                const float k = std::min(1.f / w, 1.f / h);
                src["w"] = w * k; src["h"] = h * k;
                src["x"] = (1 - w * k) / 2; src["y"] = (1 - h * k) / 2;
                changed();
            }
            if (ImGui::MenuItem("Ekranı kapla")) { src["x"] = 0.f; src["y"] = 0.f; src["w"] = 1.f; src["h"] = 1.f; changed(); }
        }
        // Yazı gibi kendi boyutundaki kaynakların kutusu çizimden gelir (tuvale oranla)
        auto it = boxes_.find(id);
        const float bw = kind.resizable ? w : (it != boxes_.end() && W ? it->second.w / W : 0.f);
        const float bh = kind.resizable ? h : (it != boxes_.end() && H ? it->second.h / H : 0.f);
        if (ImGui::MenuItem("Ortala")) { src["x"] = (1 - bw) / 2; src["y"] = (1 - bh) / 2; changed(); }
        if (ImGui::MenuItem("Yatay ortala")) { src["x"] = (1 - bw) / 2; changed(); }
        if (ImGui::MenuItem("Dikey ortala")) { src["y"] = (1 - bh) / 2; changed(); }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme_error());
    const bool remove = ImGui::MenuItem("Sil");
    ImGui::PopStyleColor();
    if (remove) {
        list.erase(index);
        if (selected_ == id) selected_.clear();
        changed();
    }
}

void App::group_popup() {
    if (!ImGui::BeginPopup("grupla")) return;
    auto& list = scene()["sources"];
    json* sel = source(selected_);
    const json* group = nullptr;
    if (sel && sel->value("type", std::string()) == "scene")
        for (auto& sc : doc_["scenes"])
            if (sc.value("id", std::string()) == (*sel)["settings"].value("scene", std::string()) && sc.value("group", false)) group = &sc;
    if (group) {    // Seçili grup çözülür: kaynakları sahneye geri döner
        ImGui::TextUnformatted(TF("\"%s\" grubu seçili.", sel->value("name", std::string()).c_str()).c_str());
        if (ImGui::Button("Grubu çöz", ImVec2(-1, 0))) {
            ungroup(selected_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Gruplanacak kaynakları seçin:");
    int picked = 0;
    for (int i = int(list.size()) - 1; i >= 0; i--) {
        const std::string id = list[i].value("id", std::string());
        bool on = group_pick_[id];
        if (ImGui::Checkbox((list[i].value("name", std::string()) + "##g" + id).c_str(), &on)) group_pick_[id] = on;
        if (on) picked++;
    }
    ImGui::BeginDisabled(picked < 2);
    if (ImGui::Button("Grup oluştur", ImVec2(-1, 0))) {
        int groups = 1;
        for (auto& sc : doc_["scenes"]) if (sc.value("group", false)) groups++;
        const std::string gid = new_id(), name = TF("Grup %d", groups);
        json children = json::array();
        int top = -1;
        for (int i = 0; i < int(list.size());) {
            if (group_pick_[list[i].value("id", std::string())]) { children.push_back(list[i]); list.erase(i); top = i; }
            else i++;
        }
        doc_["scenes"].push_back({{"id", gid}, {"name", name}, {"group", true}, {"sources", children}});
        json g = new_source("scene", 0, 0, 1, 1);
        g["name"] = name;
        g["settings"]["scene"] = gid;
        list.insert(list.begin() + std::clamp(top, 0, int(list.size())), g);
        selected_ = g["id"];
        group_pick_.clear();
        changed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Grup tek parça taşınır ve boyutlanır. İçeriğini sahne listesinde grubu seçerek düzenlersiniz.");
    ImGui::EndPopup();
}

void App::vcam_toggle() {
    if (status_.vcam_state) { engine_.request_virtual_camera(false); return; }
    if (!vcam_registered()) { vcam_install_ask_ = true; return; }
    engine_.request_virtual_camera(true);
}

void App::studio_transition() {
    if (!doc_.value("studio", false)) return;
    doc_["program"] = doc_.value("current", std::string());
    changed();
}

void App::preview_area(ImVec2 size) {
    update_preview_texture();
    int W, H;
    engine_.canvas_size(W, H);
    const float f = std::min(size.x / W, size.y / H);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 image(W * f, H * f);
    const ImVec2 origin(start.x + (size.x - image.x) / 2, start.y + (size.y - image.y) / 2);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y), IM_COL32(0, 0, 0, 255));
    if (preview_srv_)
        draw->AddImage((ImTextureID)(intptr_t)preview_srv_.Get(), origin, ImVec2(origin.x + image.x, origin.y + image.y));
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("onizleme", image);
    preview_input(origin, f);
    if (json* sel = source(selected_)) {
        auto it = boxes_.find(selected_);
        if (it != boxes_.end() && it->second.w > 0) {
            const Rect& b = it->second;
            const ImVec2 a(origin.x + b.x * f, origin.y + b.y * f), c(a.x + b.w * f, a.y + b.h * f);
            draw->AddRect(a, c, IM_COL32(255, 71, 87, 255), 0.f, ImDrawFlags_None, 2.f);
            if (type_of(sel->value("type", std::string())).resizable)
                for (ImVec2 p : {a, c, ImVec2(a.x, c.y), ImVec2(c.x, a.y)})
                    draw->AddRectFilled(ImVec2(p.x - kHandle, p.y - kHandle), ImVec2(p.x + kHandle, p.y + kHandle), IM_COL32(255, 71, 87, 255));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + size.y));
    ImGui::Dummy(ImVec2(size.x, 0));
}

void App::panel_scenes() {
    auto& scenes = doc_["scenes"];
    const std::string current = doc_.value("current", std::string());
    ImGui::BeginChild("sahneler", ImVec2(0, -38), ImGuiChildFlags_Borders);
    for (auto& s : scenes) {
        const std::string id = s.value("id", std::string());
        ImGui::PushID(id.c_str());
        if (ImGui::Selectable(s.value("name", std::string("Sahne")).c_str(), id == current) && id != current) {
            doc_["current"] = id;
            selected_.clear();
            changed();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (icon_button(ICON_ADD, "Sahne ekle")) {
        const std::string id = new_id();
        scenes.push_back({{"id", id}, {"name", "Sahne " + std::to_string(scenes.size() + 1)}, {"sources", json::array()}});
        doc_["current"] = id;
        selected_.clear();
        changed();
    }
    ImGui::SameLine();
    if (icon_button(ICON_DELETE, "Sahneyi sil") && scenes.size() > 1) {
        for (size_t i = 0; i < scenes.size(); i++)
            if (scenes[i].value("id", std::string()) == current) { scenes.erase(i); break; }
        doc_["current"] = scenes[0]["id"];
        selected_.clear();
        changed();
    }
    ImGui::SameLine();
    if (icon_button(ICON_RENAME, "Sahneyi adlandır")) {
        strncpy_s(scene_name_, scene().value("name", std::string()).c_str(), _TRUNCATE);
        ImGui::OpenPopup("Sahne adı");
    }
    if (ImGui::BeginPopup("Sahne adı")) {
        if (ImGui::InputText("##ad", scene_name_, sizeof(scene_name_), ImGuiInputTextFlags_EnterReturnsTrue)) {
            scene()["name"] = std::string(scene_name_);
            changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::panel_sources() {
    auto& list = scene()["sources"];
    ImGui::BeginChild("kaynaklar", ImVec2(0, -38), ImGuiChildFlags_Borders);
    for (int i = int(list.size()) - 1; i >= 0; i--) {
        auto& s = list[i];
        const std::string id = s.value("id", std::string());
        ImGui::PushID(id.c_str());
        bool visible = s.value("visible", true);
        if (ImGui::Checkbox("##g", &visible)) { s["visible"] = visible; changed(); }
        ImGui::SameLine();
        const std::string label = s.value("name", std::string()) + "  (" + type_of(s.value("type", std::string())).name + ")";
        if (ImGui::Selectable(label.c_str(), id == selected_, ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) show_properties_ = true;
        }
        if (ImGui::BeginPopupContextItem("sag_tik")) {      // Sağ tık: türe göre menü
            selected_ = id;
            source_menu(id);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (int(list.size()) <= i) break;      // Menüden silindiyse döngü bozulmasın
    }
    ImGui::EndChild();
    if (icon_button(ICON_ADD, "Kaynak ekle")) ImGui::OpenPopup("ekle");
    if (ImGui::BeginPopup("ekle")) {
        for (auto& t : kTypes)
            if (ImGui::Selectable(t.name)) add_source(t.key);
        ImGui::EndPopup();
    }
    int index = -1;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].value("id", std::string()) == selected_) index = int(i);
    ImGui::SameLine();
    if (icon_button(ICON_DELETE, "Kaynağı sil") && index >= 0) { list.erase(index); selected_.clear(); changed(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(index < 0);
    if (icon_button(ICON_SETTINGS, "Özellikler")) show_properties_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (icon_button(ICON_UP, "Öne al") && index >= 0 && index + 1 < int(list.size())) { std::swap(list[index], list[index + 1]); changed(); }
    ImGui::SameLine();
    if (icon_button(ICON_DOWN, "Arkaya al") && index > 0) { std::swap(list[index], list[index - 1]); changed(); }
    ImGui::SameLine();
    if (icon_button("\xEE\x9C\x9D", "Grupla / grubu çöz")) { group_pick_.clear(); ImGui::OpenPopup("grupla"); }
    group_popup();
    rename_popup();
}

void App::panel_record() {
    const bool rec = status_.recording, live = status_.streaming;
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
    ImGui::PushStyleColor(ImGuiCol_Button, live ? IM_COL32(90, 90, 100, 255) : IM_COL32(13, 110, 253, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, live ? IM_COL32(110, 110, 120, 255) : IM_COL32(50, 135, 255, 255));
    if (ImGui::Button(live ? "Yayını Durdur" : "Yayını Başlat", ImVec2(half, 42))) toggle_stream();
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, rec ? IM_COL32(90, 90, 100, 255) : IM_COL32(220, 53, 69, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rec ? IM_COL32(110, 110, 120, 255) : IM_COL32(235, 70, 85, 255));
    if (ImGui::Button(rec ? "Kaydı Durdur" : "Kaydı Başlat", ImVec2(-1, 42))) engine_.request_record(!rec);
    ImGui::PopStyleColor(3);
    ImGui::BeginDisabled(!rec);
    if (ImGui::Button(status_.paused ? "Devam et" : "Duraklat", ImVec2(half, 32))) engine_.toggle_pause();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Ayarlar", ImVec2(-1, 32))) open_settings("record");
    {   // Tekrar arabelleği: son saniyeler sürekli tutulur, istenince dosyaya yazılır
        const bool on = status_.replay_on;
        if (on) ImGui::PushStyleColor(ImGuiCol_Text, theme_good());
        if (ImGui::Button(on ? "Tekrar: açık###tekrar" : "Tekrar: kapalı###tekrar", ImVec2(half, 28))) engine_.request_replay(!on);
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", TF("Tekrar arabelleği son %d saniyeyi sürekli tutar. Açmak / kapatmak için tıklayın.", rec_.value("replay_seconds", 30)).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!on);
        if (ImGui::Button("Tekrarı kaydet", ImVec2(-1, 28))) engine_.request_replay_save();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Son saniyeleri kayıt klasörüne \"Replay ...mp4\" olarak yazar. Kısayol: Ctrl + F8");
    }
    {   // Sanal kamera: sahne Discord, Zoom, Meet gibi uygulamalarda kamera olarak seçilebilir
        const int vs = status_.vcam_state;
        if (vs) ImGui::PushStyleColor(ImGuiCol_Text, theme_good());
        if (ImGui::Button(vs == 2 ? "Sanal kamera: kullanılıyor###vcam" : vs == 1 ? "Sanal kamera: açık###vcam" : "Sanal kamera: kapalı###vcam", ImVec2(-1, 28))) vcam_toggle();
        if (vs) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Açıkken sahne, kamera listesinde \"Recording Studio Camera\" olarak görünür (Windows 11).");
    }
    {   // Kayıt ve yayın sürerken sesi aç / kapat: kaynak kapanmaz, yalnızca susturulur
        auto mute_button = [&](bool mic, float width) {
            const bool muted = rec_.value(mic ? "mic_muted" : "desktop_muted", false);
            if (muted) ImGui::PushStyleColor(ImGuiCol_Text, theme_error());
            const char* label = mic ? (muted ? ICON_MUTE "  Mikrofon: kapalı###mik_ac" : ICON_VOLUME "  Mikrofon: açık###mik_ac")
                                    : (muted ? ICON_MUTE "  Bilgisayar sesi: kapalı###pc_ac" : ICON_VOLUME "  Bilgisayar sesi: açık###pc_ac");
            if (ImGui::Button(label, ImVec2(width, 28))) toggle_mute(mic);
            if (muted) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Kayıt ve yayın sürerken de sesi açıp kapatır. Kısayol: Ayarlar > Kısayollar.");
        };
        mute_button(false, half);
        ImGui::SameLine();
        mute_button(true, -1);
    }
    ImGui::Spacing();
    const float start = ImGui::GetCursorPosX();
    ImGui::TextDisabled("Yayın");
    ImGui::SameLine(start + half + ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextDisabled("Kayıt");
    ImGui::PushFont(nullptr, 22.f);
    ImGui::TextUnformatted(format_time(status_.streaming ? status_.stream_elapsed : 0).c_str());
    ImGui::SameLine(start + half + ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextUnformatted(format_time(status_.elapsed).c_str());
    ImGui::PopFont();
    ImGui::BeginDisabled(rec || live);      // Yayın ve kayıt sürerken profil değişmez
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##profil", profile_.c_str())) {
        std::string chosen;
        for (auto& name : profiles())
            if (ImGui::Selectable(name.c_str(), name == profile_)) chosen = name;
        ImGui::Separator();
        if (ImGui::Selectable("Profilleri yönet...")) open_settings("profiles");
        ImGui::EndCombo();
        if (!chosen.empty()) profile_switch(chosen);
    }
    ImGui::SetItemTooltip("Profil: sahneler, kayıt, yayın, ses, tema ve panel düzeni bir arada");
    ImGui::EndDisabled();
    if (status_.stream_state == 1) ImGui::TextColored(theme_warn(), "Sunucuya bağlanıyor...");
    if (status_.stream_state == 2) ImGui::TextColored(theme_good(), "Yayında");
    if (status_.stream_state == 3) ImGui::TextColored(theme_warn(), "Bağlantı koptu, yeniden deneniyor...");
}

void App::panel_audio() {
    ImGui::BeginChild("ses_liste", ImVec2(0, 0), 0);
    auto row = [&](const char* label, const char* on, const char* volume, const char* muted, float level, bool mic) {
        ImGui::PushID(label);
        ImGui::BeginDisabled(status_.recording || status_.streaming);
        if (edit_bool(label, rec_, on)) rec_dirty_ = true;
        ImGui::EndDisabled();
        const float bw = 30, sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - (mic ? bw * 2 + sp : bw));
        const bool is_muted = rec_.value(muted, false);
        if (is_muted) ImGui::PushStyleColor(ImGuiCol_Text, theme_error());
        if (ImGui::Button(is_muted ? ICON_MUTE "###sustur" : ICON_VOLUME "###sustur", ImVec2(bw, 0))) { rec_[muted] = !is_muted; rec_dirty_ = true; }
        if (is_muted) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(is_muted ? "Sesi aç" : "Sustur");
        if (mic) {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FILTER "###filtre", ImVec2(bw, 0))) show_mic_ = true;
            ImGui::SetItemTooltip("Mikrofon filtreleri, dinleme ve bas-konuş");
        }
        ImGui::SetNextItemWidth(150);
        ImGui::ProgressBar(std::min(level, 1.f), ImVec2(150, 10), "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (edit_int("##s", rec_, volume, 0, 100)) rec_dirty_ = true;
        ImGui::PopID();
    };
    row("Bilgisayar sesi", "desktop_audio", "desktop_volume", "desktop_muted", status_.level_desktop, false);
    row("Mikrofon", "mic_audio", "mic_volume", "mic_muted", status_.level_mic, true);
    std::vector<std::string> notes;     // Mikrofonda açık olan filtreler bir bakışta görünsün
    const json& f = rec_["mic_filters"];
    if (f.value("suppress", false)) notes.push_back(T("Gürültü bastırma"));
    if (f.value("gate", false)) notes.push_back(T("Gürültü kapısı"));
    if (f.value("comp", false)) notes.push_back(T("Kompresör"));
    if (rec_.value("mic_monitor", std::string("off")) != "off") notes.push_back(T("Dinleniyor"));
    if (rec_.value("push_to_talk", false)) {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        notes.push_back(TF("Bas-konuş: %s", hotkeys_["push_to_talk"].vk ? key_name(hotkeys_["push_to_talk"]).c_str() : T("tuş atanmadı")));
    }
    if (!notes.empty()) {
        std::string text;
        for (auto& n : notes) text += (text.empty() ? "" : "  ·  ") + n;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }
    audio_inputs_panel();
    ImGui::EndChild();
}

// Ek ses girişleri: ikinci mikrofon / ses kartı girişi, bir hoparlörün sesi, tek bir uygulamanın sesi
void App::audio_inputs_panel() {
    json& list = rec_["audio_inputs"];
    if (!list.is_array()) list = json::array();
    const bool busy = status_.recording || status_.streaming || status_.replay_on;   // Girişler değişirse ses bir an kesilir
    int level_index = 0, remove = -1;
    for (int i = 0; i < int(list.size()); i++) {
        json& in = list[i];
        ImGui::PushID(in.value("id", std::string("x")).c_str());
        ImGui::Separator();
        const std::string kind = in.value("kind", std::string("mic"));
        const char* icon = kind == "app" ? "\xEE\x9C\xB7 " : kind == "speaker" ? "\xEE\x9D\xA7 " : "\xEE\x9C\xA0 ";
        ImGui::BeginDisabled(busy);
        if (edit_bool((icon + in.value("name", std::string("Ses")) + "##acik").c_str(), in, "enabled")) rec_dirty_ = true;
        ImGui::EndDisabled();
        const float bw = 30, sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 2 - sp);
        const bool muted = in.value("muted", false);
        if (muted) ImGui::PushStyleColor(ImGuiCol_Text, theme_error());
        if (ImGui::Button(muted ? ICON_MUTE "###sustur" : ICON_VOLUME "###sustur", ImVec2(bw, 0))) { in["muted"] = !muted; rec_dirty_ = true; }
        if (muted) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(muted ? "Sesi aç" : "Sustur");
        ImGui::SameLine();
        if (ImGui::Button(ICON_SETTINGS "###ayar", ImVec2(bw, 0))) {
            if (mics_.empty()) mics_ = list_audio_devices(true);
            if (speakers_.empty()) speakers_ = list_audio_devices(false);
            audio_apps_ = list_audio_apps();
            ImGui::OpenPopup("giris_ayar");
        }
        ImGui::SetItemTooltip("Ses kaynağının ayarları");
        if (ImGui::BeginPopup("giris_ayar")) {
            ImGui::PushItemWidth(320);
            if (edit_text("Ad", in, "name")) rec_dirty_ = true;
            ImGui::BeginDisabled(busy);
            if (kind == "app") {
                const std::string exe = in.value("exe", std::string());
                if (ImGui::BeginCombo("Uygulama", exe.empty() ? "Seçin" : exe.c_str())) {
                    for (auto& a : audio_apps_)
                        if (ImGui::Selectable(a.c_str(), a == exe)) {
                            in["exe"] = a;
                            if (in.value("name", std::string()).empty() || in.value("name", std::string()) == T("Uygulama sesi")) in["name"] = a.substr(0, a.rfind('.'));
                            rec_dirty_ = true;
                        }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Programın ve alt süreçlerinin sesi alınır (örneğin tarayıcının bütün sekmeleri).");
                ImGui::TextDisabled("Program kapalıysa açılınca kendiliğinden bağlanır. Windows 10 (2004) ve sonrası gerekir.");
            } else {
                std::vector<std::pair<std::string, std::string>> items{{"", "Varsayılan"}};
                for (auto& d : kind == "speaker" ? speakers_ : mics_) items.push_back({narrow(d.id), narrow(d.name)});
                if (edit_choice("Aygıt", in, "device", items)) rec_dirty_ = true;
            }
            ImGui::EndDisabled();
            if (edit_choice("Nereye gitsin", in, "route", {{"both", "Kayıt ve yayın"}, {"record", "Yalnızca kayıt"}, {"stream", "Yalnızca yayın"}})) rec_dirty_ = true;
            ImGui::PopItemWidth();
            ImGui::Spacing();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Kaldır")) { remove = i; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled();
            if (busy) ImGui::TextDisabled("Kayıt, yayın ya da tekrar arabelleği açıkken aygıt değiştirilemez.");
            ImGui::EndPopup();
        }
        const float level = in.value("enabled", true) && level_index < 8 ? status_.level_inputs[level_index] : 0.f;
        if (in.value("enabled", true)) level_index++;
        ImGui::ProgressBar(std::min(level, 1.f), ImVec2(150, 10), "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (!in.contains("volume")) in["volume"] = 100;
        if (edit_int("##s", in, "volume", 0, 100)) rec_dirty_ = true;
        ImGui::PopID();
    }
    if (remove >= 0) { list.erase(remove); rec_dirty_ = true; }
    ImGui::Spacing();
    ImGui::BeginDisabled(busy || list.size() >= 8);
    if (ImGui::Button("\xEE\x9C\x90  Ses kaynağı ekle")) ImGui::OpenPopup("giris_ekle");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("giris_ekle")) {
        auto add = [&](const char* kind, const char* name) {
            list.push_back({{"id", new_id()}, {"kind", kind}, {"name", T(name)}, {"device", ""}, {"exe", ""}, {"enabled", true}, {"muted", false}, {"volume", 100}});
            rec_dirty_ = true;
        };
        if (ImGui::Selectable("Mikrofon / ses girişi")) add("mic", "Mikrofon 2");
        if (ImGui::Selectable("Hoparlör sesi (çıkış aygıtı)")) add("speaker", "Hoparlör");
        if (ImGui::Selectable("Uygulama sesi (yalnızca bir program)")) add("app", "Uygulama sesi");
        ImGui::EndPopup();
    }
}

// İstatistikler: işlemci, ekran kartı, bellek, kare hızı, kayıt ve yayın durumu, disk alanı
void App::stats_window() {
    if (!show_stats_) return;
    static double next = 0;
    static float cpu_sys = -1, cpu_app = -1, gpu = -1;
    static double memory = 0, disk_free = -1, last = 0;
    static ULONGLONG idle0 = 0, kernel0 = 0, user0 = 0, pk0 = 0, pu0 = 0;
    static PDH_HQUERY query = nullptr;
    static PDH_HCOUNTER counter = nullptr;
    auto u64 = [](const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    if (now() >= next) {
        next = now() + 1;
        FILETIME idle, kernel, user, c, e, pk, pu;
        GetSystemTimes(&idle, &kernel, &user);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu);
        const ULONGLONG total = (u64(kernel) - kernel0) + (u64(user) - user0);
        if (last && total) {
            cpu_sys = 100.f * float(total - (u64(idle) - idle0)) / float(total);
            cpu_app = 100.f * float((u64(pk) - pk0) + (u64(pu) - pu0)) / float(total);
        }
        idle0 = u64(idle); kernel0 = u64(kernel); user0 = u64(user); pk0 = u64(pk); pu0 = u64(pu);
        last = now();
        PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) memory = pmc.WorkingSetSize / 1048576.0;
        ULARGE_INTEGER free_bytes{};
        const std::wstring folder = widen(rec_.value("folder", std::string(".")));
        disk_free = GetDiskFreeSpaceExW(std::filesystem::path(folder).root_path().c_str(), &free_bytes, nullptr, nullptr) ? free_bytes.QuadPart / 1073741824.0 : -1;
        if (!query && PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) counter = nullptr;
        }
        if (query && counter && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, nullptr) == PDH_MORE_DATA) {
                std::vector<uint8_t> buffer(size);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    double sum = 0;
                    for (DWORD i = 0; i < count; i++) if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue;
                    gpu = float(std::min(100.0, sum));
                }
            }
        }
    }
    ImGui::SetNextWindowSize(ImVec2(440, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("İstatistikler"), &show_stats_, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    char text[160];
    auto row = [&](const char* label, const std::string& value, ImVec4 color = ImVec4(0, 0, 0, 0)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", T(label));
        ImGui::TableNextColumn();
        if (color.w > 0) ImGui::TextColored(color, "%s", value.c_str());
        else ImGui::TextUnformatted(value.c_str());
    };
    auto pct = [&](float v) { if (v < 0) return std::string("-"); sprintf_s(text, "%.0f %%", v); return std::string(text); };
    if (ImGui::BeginTable("istatistik", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Değer", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Sonuç", ImGuiTableColumnFlags_WidthStretch, 1.f);
        row("İşlemci (bütün sistem)", pct(cpu_sys), cpu_sys > 90 ? theme_warn() : ImVec4(0, 0, 0, 0));
        row("İşlemci (Kayıt Stüdyosu)", pct(cpu_app));
        row("Ekran kartı (3B)", pct(gpu));
        sprintf_s(text, "%.0f MB", memory);
        row("Bellek (Kayıt Stüdyosu)", text);
        sprintf_s(text, "%.1f / %d FPS", status_.render_fps, rec_.value("fps", 60));
        row("Çizim hızı", text, status_.render_fps > 0 && status_.render_fps < rec_.value("fps", 60) * 0.9 ? theme_warn() : ImVec4(0, 0, 0, 0));
        sprintf_s(text, "%.2f ms", status_.frame_ms);
        row("Kare çizim süresi", text);
        if (disk_free >= 0) { sprintf_s(text, "%.1f GB", disk_free); row("Kayıt klasöründe boş alan", text, disk_free < 5 ? theme_error() : ImVec4(0, 0, 0, 0)); }
        if (status_.recording) {
            row("Kayıt süresi", format_time(status_.elapsed));
            sprintf_s(text, "%.1f MB", status_.record_bytes / 1048576.0);
            row("Dosya boyutu", text);
            sprintf_s(text, "%.0f kbps", status_.record_kbps);
            row("Kayıt bit hızı", text);
            const double p = status_.frames ? 100.0 * status_.repeated / status_.frames : 0;
            sprintf_s(text, "%lld (%.1f %%)", status_.repeated, p);
            row("Atlanan kare (kayıt)", text, p > 1 ? theme_warn() : ImVec4(0, 0, 0, 0));
            if (status_.record_part) row("Parça", std::to_string(status_.record_part));
        } else {
            row("Kayıt", T("Kapalı"));
        }
        if (status_.streaming) {
            row("Yayın süresi", format_time(status_.stream_elapsed));
            sprintf_s(text, "%.0f kbps", status_.stream_kbps);
            row("Yayın bit hızı", text);
            row("Atlanan kare (yayın)", std::to_string(status_.stream_dropped), status_.stream_dropped ? theme_warn() : ImVec4(0, 0, 0, 0));
        } else {
            row("Yayın", T("Kapalı"));
        }
        for (auto& t : status_.stream_targets) {     // Ek yayın hedefleri
            const char* state = !t.error.empty() ? t.error.c_str() : t.state == 2 ? T("Yayında") : t.state == 3 ? T("Yeniden bağlanıyor") : T("Bağlanıyor");
            sprintf_s(text, "%s  ·  %.0f kbps", state, t.kbps);
            row(t.name.empty() ? "Ek hedef" : t.name.c_str(), t.error.empty() ? std::string(text) : t.error, t.error.empty() ? ImVec4(0, 0, 0, 0) : theme_error());
        }
        row("Sanal kamera", T(status_.vcam_state == 2 ? "Bir uygulamada kullanılıyor" : status_.vcam_state == 1 ? "Açık" : "Kapalı"));
        if (status_.replay_on) row("Tekrar arabelleği", TF("%d sn tutuluyor", status_.replay_seconds));
        else row("Tekrar arabelleği", T("Kapalı"));
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Kodlayıcı (FFmpeg) ayrı süreçte çalışır; işlemci kullanımı \"bütün sistem\" satırında görünür.");
    ImGui::End();
}

void App::mic_window() {
    if (!show_mic_) return;
    if (speakers_.empty()) speakers_ = list_audio_devices(false);
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(T("Mikrofon ayarları"), &show_mic_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    json& f = rec_["mic_filters"];
    bool edited = false;

    // Giriş düzeyi (kazançtan sonra, filtrelerden önce); gürültü kapısının eşikleri üzerinde gösterilir
    {
        const float level = status_.level_mic_input;
        const float db = level > 1e-5f ? 20 * log10f(level) : -96.f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x, h = 16;
        auto at = [&](float d) { return p.x + w * std::clamp((d + 60) / 60, 0.f, 1.f); };
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 3);
        const ImU32 fill = db > -6 ? IM_COL32(220, 53, 69, 255) : db > -18 ? IM_COL32(255, 193, 7, 255) : IM_COL32(40, 167, 69, 255);
        draw->AddRectFilled(p, ImVec2(at(db), p.y + h), fill, 3);
        if (f.value("gate", false)) {
            const float o = at(float(f.value("gate_open", -32))), c = at(float(f.value("gate_close", -40)));
            draw->AddLine(ImVec2(o, p.y - 3), ImVec2(o, p.y + h + 3), IM_COL32(255, 255, 255, 255), 2);
            draw->AddLine(ImVec2(c, p.y - 3), ImVec2(c, p.y + h + 3), IM_COL32(150, 150, 150, 255), 2);
        }
        ImGui::Dummy(ImVec2(w, h + 4));
        ImGui::TextDisabled("Giriş düzeyi: %.0f dB  (beyaz çizgi: kapı açılır, gri çizgi: kapı kapanır)", db);
    }
    ImGui::PushItemWidth(-190);
    ImGui::SeparatorText("Kazanç");
    edited |= edit_int("Kazanç (dB)", f, "gain", -30, 30);
    ImGui::SeparatorText("Gürültü bastırma");
    edited |= edit_bool("Etkin##bastir", f, "suppress");
    edited |= edit_choice("Bastırma gücü", f, "suppress_level", {{"low", "Hafif"}, {"medium", "Orta"}, {"high", "Güçlü"}});
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Fan, klima, bilgisayar uğultusu ve hışırtı gibi sürekli sesleri azaltır. Açtıktan sonra birkaç saniye sessiz kalın; ortamın gürültüsünü öğrenir.");
    ImGui::PopStyleColor();
    ImGui::SeparatorText("Gürültü kapısı");
    edited |= edit_bool("Etkin##kapi", f, "gate");
    edited |= edit_int("Açılma eşiği (dB)", f, "gate_open", -96, 0);
    edited |= edit_int("Kapanma eşiği (dB)", f, "gate_close", -96, 0);
    edited |= edit_int("Bekleme (ms)", f, "gate_hold", 0, 2000);
    edited |= edit_int("Kapanma süresi (ms)", f, "gate_release", 10, 2000);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Konuşmadığınızda mikrofonu tamamen kısar; klavye ve fare sesi kayda girmez. Açılma eşiğini konuşurkenki düzeyin biraz altına, kapanma eşiğini ortam gürültüsünün biraz üstüne getirin.");
    ImGui::PopStyleColor();
    ImGui::SeparatorText("Kompresör");
    edited |= edit_bool("Etkin##kompresor", f, "comp");
    edited |= edit_int("Eşik (dB)", f, "comp_threshold", -60, 0);
    edited |= edit_int("Oran", f, "comp_ratio", 1, 20);
    edited |= edit_int("Atak (ms)", f, "comp_attack", 1, 200);
    edited |= edit_int("Bırakma (ms)", f, "comp_release", 10, 1000);
    edited |= edit_int("Çıkış kazancı (dB)", f, "comp_gain", -20, 30);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Yüksek sesleri bastırıp sesinizi dengeler: bağırınca patlamaz, alçak sesle konuşunca kaybolmaz.");
    ImGui::PopStyleColor();
    if (ImGui::Button("Filtreleri varsayılana döndür")) { f = default_mic_filters(); edited = true; }

    ImGui::SeparatorText("Dinleme");
    edited |= edit_choice("Dinleme", rec_, "mic_monitor", {{"off", "Kapalı"}, {"on", "Dinle ve kaydet"}, {"only", "Yalnızca dinle (kayda ve yayına girmez)"}});
    {
        std::vector<std::pair<std::string, std::string>> items{{"", "Varsayılan"}};
        for (auto& d : speakers_) items.push_back({narrow(d.id), narrow(d.name)});
        edited |= edit_choice("Çıkış aygıtı", rec_, "monitor_device", items);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Mikrofonun filtrelerden geçmiş, kayda giden halini kulaklıktan duyarsınız. Hoparlörde açarsanız yankı yapar.");
    ImGui::PopStyleColor();

    ImGui::SeparatorText("Bas-konuş");
    edited |= edit_bool("Bas-konuş kullan", rec_, "push_to_talk");
    Hotkey ptt;
    {
        std::lock_guard<std::mutex> g(hotkeys_lock_);
        ptt = hotkeys_["push_to_talk"];
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Tuş:");
    ImGui::SameLine();
    ImGui::TextColored(theme_accent(), "%s", ptt.vk ? key_name(ptt).c_str() : T("atanmadı"));
    ImGui::SameLine();
    if (ImGui::Button("Tuşu ayarla")) { show_mic_ = false; open_settings("hotkeys"); }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Açıkken mikrofon yalnızca tuş basılı tutulduğunda duyulur. Fare yan tuşları da atanabilir.");
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();
    if (edited) rec_dirty_ = true;
    ImGui::End();
}

void App::social_settings() {
    auto& s = doc_["social"];
    ImGui::TextDisabled("Kullanıcı adını yaz; boş bırakılanlar görünmez.");
    for (auto& p : kPlatforms) {
        ImGui::PushID(p.key);
        auto& a = s["accounts"][p.key];
        if (edit_bool("##g", a, "show")) changed();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-90);
        if (edit_text(p.name, a, "handle")) changed();
        ImGui::PopID();
    }
    if (edit_choice("Düzen", s, "layout", {{"vertical", "Alt alta"}, {"horizontal", "Yan yana"}})) changed();
    if (edit_int("Aralık", s, "spacing", 0, 60)) changed();
    if (edit_font(s)) changed();
    if (edit_int("Boyut", s, "font_size", 10, 80)) changed();
    if (edit_bool("Kalın", s, "bold")) changed();
    ImGui::SameLine();
    if (edit_color("Yazı rengi", s, "text_color")) changed();
    if (edit_choice("Simgeler", s, "icon_style", {{"brand", "Marka renkleri"}, {"mono", "Yazı rengi"}})) changed();
    if (edit_bool("Arka plan", s, "background")) changed();
    ImGui::SameLine();
    if (edit_color("##arka", s, "bg_color")) changed();
    if (edit_int("Arka plan opaklığı", s, "bg_opacity", 0, 100)) changed();
    if (edit_int("Köşe yuvarlama", s, "bg_radius", 0, 60)) changed();
    if (edit_bool("Yazı çizgisi", s, "outline")) changed();
    ImGui::SameLine();
    if (edit_color("##cizgi", s, "outline_color")) changed();
    if (edit_int("Çizgi kalınlığı", s, "outline_thickness", 1, 6)) changed();
}

void App::panel_properties() {
    if (!show_properties_) return;
    json* sel = source(selected_);
    if (!sel) {
        show_properties_ = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(440, 560), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(T("Özellikler###ozellikler"), &show_properties_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    json& src = *sel;
    json& s = src["settings"];
    const std::string type = src.value("type", std::string());
    ImGui::PushItemWidth(-130);
    if (edit_text("Ad", src, "name")) changed();
    if (edit_int("Opaklık", src, "opacity", 0, 100)) changed();
    if (type == "screen" || type == "region") {
        auto monitors = engine_.monitors();
        int m = s.value("monitor", 0);
        if (ImGui::BeginCombo("Ekran", m < int(monitors.size()) ? monitors[m].c_str() : "")) {
            for (int i = 0; i < int(monitors.size()); i++)
                if (ImGui::Selectable(monitors[i].c_str(), i == m)) { s["monitor"] = i; changed(); }
            ImGui::EndCombo();
        }
        if (edit_bool("İmleci göster", s, "cursor")) changed();
        if (type == "region") {
            ImGui::SeparatorText("Bölge");
            if (ImGui::Button("Ekranda seç...")) pending_region_ = selected_;
            ImGui::SetItemTooltip("Program küçülür, ekranda kaydedilecek dikdörtgeni fareyle çizersiniz.");
            json& r = s["region"];
            bool edited = false;
            edited |= edit_int("X", r, "x", 0, 7680);
            edited |= edit_int("Y", r, "y", 0, 4320);
            edited |= edit_int("Genişlik", r, "w", 16, 7680);
            edited |= edit_int("Yükseklik", r, "h", 16, 4320);
            if (edited) changed();
            ImGui::TextDisabled("Değerler ekranın kendi pikselidir. Ekran dışına taşan kısım kesilir.");
        }
    } else if (type == "window") {
        static std::vector<GameWindow> windows;
        const std::string exe = s.value("window", std::string()), title = s.value("title", std::string());
        const std::string shown = exe.empty() ? T("Pencere seçin") : exe + "  -  " + title;
        if (ImGui::BeginCombo("Pencere", shown.c_str())) {
            if (ImGui::IsWindowAppearing()) windows = list_game_windows();
            for (auto& w : windows) {
                const std::string label = w.exe + "  -  " + w.title;
                if (ImGui::Selectable(label.c_str(), w.exe == exe && w.title == title)) {
                    s["window"] = w.exe;
                    s["title"] = w.title;
                    if (src.value("name", std::string()) == T("Pencere") || src.value("name", std::string()) == "Pencere") src["name"] = w.title.substr(0, 40);
                    changed();
                }
            }
            ImGui::EndCombo();
        }
        if (edit_bool("İmleci göster", s, "cursor")) changed();
        if (edit_bool("Yalnızca pencere içeriği (başlık çubuğu ve kenarlar olmadan)", s, "client")) changed();
        ImGui::TextWrapped("Yalnızca seçilen pencere alınır; önüne gelen başka pencereler ve bildirimler kayda girmez. "
                           "Pencere simge durumuna küçültülürse görüntü gelmez. Başlık değişirse aynı programın penceresi kullanılır.");
    } else if (type == "game") {
        static std::vector<GameWindow> windows;
        const std::string target = s.value("window", std::string());
        if (ImGui::BeginCombo("Oyun", target.empty() ? "Tam ekran oyunu kendiliğinden bul" : target.c_str())) {
            if (ImGui::IsWindowAppearing()) windows = list_game_windows();
            if (ImGui::Selectable("Tam ekran oyunu kendiliğinden bul", target.empty())) { s["window"] = ""; changed(); }
            for (auto& w : windows) {
                const std::string label = w.exe + "  -  " + w.title;
                if (ImGui::Selectable(label.c_str(), w.exe == target)) { s["window"] = w.exe; changed(); }
            }
            ImGui::EndCombo();
        }
        if (edit_bool("İmleci göster", s, "cursor")) changed();
        if (status_.game_fallback) ImGui::TextColored(theme_warn(), "Oyun pencere olarak alınamıyor, ekran yakalama kullanılıyor. Ekrandaki her şey görünür.");
        ImGui::TextWrapped("Oyunun penceresi doğrudan alınır, oyuna hiçbir şey eklenmez; hile korumalı oyunlarda da güvenlidir.");
        ImGui::TextDisabled("Oyun Araçları'ndaki crosshair ve gösterge burada görünmez. FPS için sahneye FPS göstergesi kaynağını ekleyin.");
    } else if (type == "fps_meter") {
        bool edited = false;
        ImGui::TextDisabled("Bu gösterge sahnenin parçasıdır, kayıtta ve yayında görünür. Önizlemede sürükleyerek taşıyın.");
        ImGui::SeparatorText("Gösterilecekler");
        struct Row { const char* label; const char* key; };
        for (auto r : {Row{"FPS", "show_fps"}, Row{"Kare süresi (ms)", "show_frametime"}, Row{"Ekran yenileme (Hz)", "show_refresh"},
                       Row{"GPU sıcaklığı", "show_gpu_temp"}, Row{"GPU kullanımı", "show_gpu_load"}, Row{"GPU saati", "show_gpu_clock"}, Row{"GPU belleği", "show_gpu_vram"},
                       Row{"CPU sıcaklığı", "show_cpu_temp"}, Row{"CPU kullanımı", "show_cpu_load"}, Row{"CPU saati", "show_cpu_clock"},
                       Row{"RAM", "show_ram"}, Row{"Disk sıcaklığı", "show_disk_temp"}, Row{"Saat", "show_time"}})
            edited |= edit_bool(r.label, s, r.key);
        ImGui::SeparatorText("Görünüm");
        edited |= edit_font(s);
        edited |= edit_int("Boyut", s, "font_size", 8, 72);
        edited |= edit_bool("Kalın", s, "bold");
        edited |= edit_choice("Yerleşim", s, "layout", {{"vertical", "Dikey (satır satır)"}, {"horizontal", "Yatay (tek satır)"}});
        edited |= edit_bool("Satır başlıkları", s, "show_labels");
        edited |= edit_color("Değer rengi", s, "color");
        edited |= edit_color("Başlık rengi", s, "label_color");
        edited |= edit_bool("Değere göre renk değiştir", s, "dynamic_color");
        edited |= edit_bool("Çizgi", s, "outline");
        ImGui::SameLine();
        edited |= edit_color("##fps_cizgi", s, "outline_color");
        edited |= edit_bool("Arka plan", s, "background");
        ImGui::SameLine();
        edited |= edit_color("##fps_arka", s, "bg_color");
        edited |= edit_int("Yenileme (ms)", s, "interval", 100, 2000);
        if (edited) changed();
    } else if (type == "camera") {
        int d = s.value("device", 0);
        if (cameras_.empty()) cameras_ = list_cameras();
        if (cameras_.empty()) ImGui::TextDisabled("Kamera bulunamadı.");
        else if (ImGui::BeginCombo("Kamera", d < int(cameras_.size()) ? cameras_[d].c_str() : "")) {
            for (int i = 0; i < int(cameras_.size()); i++)
                if (ImGui::Selectable(cameras_[i].c_str(), i == d)) { s["device"] = i; changed(); }
            ImGui::EndCombo();
        }
        if (edit_bool("Ayna görüntü", s, "mirror")) changed();
        if (status_.camera_failed)
            ImGui::TextColored(theme_error(), "Kameradan görüntü alınamadı. Başka bir uygulama kullanıyor olabilir ya da sürücüsü sorunlu olabilir.");
    } else if (type == "image" || type == "video") {
        const std::string file = s.value("file", std::string());
        const std::string shown = file.empty() ? "Dosya seçilmedi" : narrow(std::filesystem::path(widen(file)).filename().wstring());
        ImGui::TextWrapped("%s", shown.c_str());
        if (ImGui::Button("Dosya seç...")) {
            const std::wstring path = type == "image"
                ? pick_file(hwnd, TW("Resimler").c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.ico")
                : pick_file(hwnd, TW("Videolar").c_str(), L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.m4v");
            if (!path.empty()) {
                s["file"] = narrow(path);
                if (type == "image") {
                    Bitmap b;
                    int W, H;
                    engine_.canvas_size(W, H);
                    if (load_image(path, b) && b.w)   // Genişlik kalsın, oran resme uysun
                        src["h"] = src.value("w", 0.3f) * W * b.h / b.w / H;
                }
                changed();
            }
        }
        if (type == "video" && edit_bool("Bitince başa sar", s, "loop")) changed();
        if (type == "video") {      // Oynatma listesi: ilk dosyadan sonra bunlar sırayla
            ImGui::SeparatorText("Oynatma listesi");
            json& list = s["playlist"];
            if (!list.is_array()) list = json::array();
            int remove = -1, up = -1;
            for (int i = 0; i < int(list.size()); i++) {
                ImGui::PushID(i);
                if (ImGui::SmallButton("\xEE\x9C\x8E")) up = i;
                ImGui::SameLine();
                if (ImGui::SmallButton("\xEE\x9C\x91")) remove = i;
                ImGui::SameLine();
                ImGui::TextUnformatted(narrow(std::filesystem::path(widen(list[i].get<std::string>())).filename().wstring()).c_str());
                ImGui::PopID();
            }
            if (remove >= 0) { list.erase(remove); changed(); }
            if (up > 0) { std::swap(list[up], list[up - 1]); changed(); }
            if (ImGui::Button("Listeye video ekle...")) {
                for (auto& f : pick_files(hwnd, TW("Videolar").c_str(), L"*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.m4v")) list.push_back(narrow(f));
                if (s.value("file", std::string()).empty() && !list.empty()) s["file"] = list[0];
                changed();
            }
            if (!list.empty()) {
                if (edit_bool("Karışık sıra", s, "shuffle")) changed();
                ImGui::TextDisabled("Liste doluysa yalnızca listedeki videolar sırayla oynar. \"Bitince başa sar\" listeyi döngüye alır (bekleme ekranı için).");
            }
        }
    } else if (type == "browser") {
        if (edit_text("Adres", s, "url")) changed();
        ImGui::SameLine();
        if (ImGui::Button("Dosya...")) {
            const std::wstring path = pick_file(hwnd, TW("Web sayfaları").c_str(), L"*.html;*.htm");
            if (!path.empty()) { s["url"] = narrow(path); changed(); }
        }
        ImGui::TextDisabled("Uyarı ya da sohbet sayfasının adresi (StreamElements, Streamlabs, Twitch sohbeti...) ya da bilgisayarınızdaki bir HTML dosyası.");
        bool edited = false;
        edited |= edit_int("Sayfa genişliği", s, "width", 100, 3840);
        edited |= edit_int("Sayfa yüksekliği", s, "height", 100, 2160);
        if (edit_bool("Sayfanın sesini kapat", s, "muted")) changed();
        ImGui::TextDisabled("Sayfanın sesi hoparlörden çalar; bilgisayar sesi açıksa kayda ve yayına girer.");
        ImGui::Text("Özel CSS");
        if (edit_text("##css", s, "css", true)) changed();
        if (ImGui::Button("Sayfayı yenile")) { s["reload"] = s.value("reload", 0) + 1; changed(); }
        if (edited) {   // Kutunun oranı sayfaya uysun
            int W, H;
            engine_.canvas_size(W, H);
            src["h"] = src.value("w", 1.f) * W * s.value("height", 720) / std::max(1, s.value("width", 1280)) / H;
            changed();
        }
    } else if (type == "slideshow") {
        json& files = s["files"];
        if (!files.is_array()) files = json::array();
        ImGui::Text("%d resim", int(files.size()));
        ImGui::SameLine();
        if (ImGui::Button("Resim ekle...")) {
            for (auto& f : pick_files(hwnd, TW("Resimler").c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff")) files.push_back(narrow(f));
            changed();
        }
        ImGui::SameLine();
        if (ImGui::Button("Klasör ekle...")) {
            const std::wstring folder = pick_folder(hwnd);
            std::error_code ec;
            if (!folder.empty())
                for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
                    std::wstring ext = e.path().extension().wstring();
                    for (auto& c : ext) c = wchar_t(towlower(c));
                    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp" || ext == L".webp" || ext == L".gif") files.push_back(narrow(e.path().wstring()));
                }
            changed();
        }
        ImGui::SameLine();
        if (ImGui::Button("Temizle##slayt") && !files.empty()) { files = json::array(); changed(); }
        if (ImGui::BeginListBox("##slaytlar", ImVec2(-1, 120))) {
            for (auto& f : files) ImGui::TextUnformatted(narrow(std::filesystem::path(widen(f.get<std::string>())).filename().wstring()).c_str());
            ImGui::EndListBox();
        }
        if (edit_int("Her resim (saniye)", s, "interval", 1, 600)) changed();
        if (edit_bool("Yumuşak geçiş", s, "fade")) changed();
        if (edit_bool("Karışık sıra", s, "shuffle")) changed();
    } else if (type == "scene") {
        const std::string target = s.value("scene", std::string());
        std::string shown = T("Sahne seçin");
        for (auto& sc : doc_["scenes"])
            if (sc.value("id", std::string()) == target) shown = sc.value("name", std::string());
        if (ImGui::BeginCombo("Sahne", shown.c_str())) {
            for (auto& sc : doc_["scenes"]) {
                const std::string id = sc.value("id", std::string());
                if (id == scene().value("id", std::string())) continue;     // Kendini içeremez
                if (ImGui::Selectable(sc.value("name", std::string()).c_str(), id == target)) { s["scene"] = id; changed(); }
            }
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("Başka bir sahnenin bütün kaynakları bu sahnede tek parça olarak görünür. Örneğin kamera ve logoyu bir sahnede hazırlayıp her sahneye ekleyin; bir yerde değiştirince her yerde değişir.");
    } else if (type == "text") {
        if (edit_text("##metin", s, "text", true)) changed();
        if (edit_font(s)) changed();
        if (edit_int("Boyut", s, "font_size", 12, 200)) changed();
        if (edit_bool("Kalın", s, "bold")) changed();
        ImGui::SameLine();
        if (edit_color("Renk", s, "color")) changed();
        if (edit_bool("Çizgi", s, "outline")) changed();
        ImGui::SameLine();
        if (edit_color("##cizgi", s, "outline_color")) changed();
        if (edit_bool("Arka plan", s, "background")) changed();
        ImGui::SameLine();
        if (edit_color("##arka", s, "bg_color")) changed();
        if (edit_int("Arka plan opaklığı", s, "bg_opacity", 0, 100)) changed();
    } else if (type == "color") {
        if (edit_color("Renk", s, "color")) changed();
    } else if (type == "social") {
        social_settings();
    }
    if (type_of(type).resizable) {
        ImGui::Spacing();
        if (ImGui::Button("Tuvali kapla")) { src["x"] = 0; src["y"] = 0; src["w"] = 1; src["h"] = 1; changed(); }
    }
    ImGui::Spacing();
    source_filters(src);
    ImGui::PopItemWidth();
    ImGui::End();
}

// Kaynak filtreleri: kayıtta, yayında ve önizlemede aynı anda uygulanır
void App::source_filters(json& src) {
    if (!src.contains("filters") || !src["filters"].is_object()) src["filters"] = default_filters();
    json& f = src["filters"];
    const json defaults = default_filters();     // Geçici nesnenin üzerinde dolaşmak programı kapatıyordu
    for (auto& [k, v] : defaults.items())
        if (!f.contains(k)) f[k] = v;
    bool edited = false;
    auto reset = [&](const char* id, std::initializer_list<const char*> keys) {
        ImGui::PushID(id);
        if (ImGui::SmallButton("Sıfırla")) {
            const json d = default_filters();
            for (const char* k : keys) f[k] = d[k];
            edited = true;
        }
        ImGui::PopID();
    };
    if (ImGui::CollapsingHeader("Kırpma")) {
        edited |= edit_int("Sol (%)", f, "crop_l", 0, 45);
        edited |= edit_int("Üst (%)", f, "crop_t", 0, 45);
        edited |= edit_int("Sağ (%)", f, "crop_r", 0, 45);
        edited |= edit_int("Alt (%)", f, "crop_b", 0, 45);
        reset("kirp", {"crop_l", "crop_t", "crop_r", "crop_b"});
    }
    if (ImGui::CollapsingHeader("Renk düzeltme")) {
        edited |= edit_int("Parlaklık", f, "brightness", -100, 100);
        edited |= edit_int("Kontrast", f, "contrast", -100, 100);
        edited |= edit_int("Doygunluk", f, "saturation", -100, 100);
        edited |= edit_int("Gama", f, "gamma", -100, 100);
        edited |= edit_int("Renk tonu", f, "hue", -180, 180);
        reset("renk", {"brightness", "contrast", "saturation", "gamma", "hue"});
    }
    if (ImGui::CollapsingHeader("Yeşil perde (chroma key)")) {
        edited |= edit_bool("Etkin##perde", f, "chroma");
        ImGui::SameLine();
        edited |= edit_color("Anahtar rengi", f, "chroma_color");
        ImGui::SameLine();
        if (ImGui::SmallButton("Yeşil")) { f["chroma_color"] = "#00ff00"; edited = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Mavi")) { f["chroma_color"] = "#0000ff"; edited = true; }
        edited |= edit_int("Benzerlik", f, "similarity", 1, 100);
        edited |= edit_int("Yumuşaklık", f, "smoothness", 1, 100);
        edited |= edit_int("Renk taşmasını azalt", f, "spill", 0, 100);
        ImGui::TextDisabled("Perde rengi tam kaybolmuyorsa benzerliği artırın; kenarlar sertse yumuşaklığı artırın.");
        reset("perde", {"similarity", "smoothness", "spill"});
    }
    if (ImGui::CollapsingHeader("Keskinleştirme")) {
        edited |= edit_int("Keskinlik", f, "sharpen", 0, 100);
    }
    if (ImGui::CollapsingHeader("Maske (yuvarlak köşe, daire)")) {
        edited |= edit_choice("Şekil", f, "mask", {{"none", "Yok"}, {"rounded", "Yuvarlak köşe"}, {"circle", "Daire"}});
        if (f.value("mask", std::string("none")) == "rounded") edited |= edit_int("Köşe yarıçapı", f, "radius", 2, 400);
        ImGui::TextDisabled("Kamera için daire seçip kutuyu kare yapın. Kırpma ile birlikte kullanılabilir.");
    }
    if (edited) changed();
}

// Kayıt ve yayındaki kodlayıcı bölümü aynıdır. Yayında hız denetimi ve anahtar kare platform kuralına göre sabittir.
bool App::encoder_settings(json& v, bool stream) {
    bool changed = false;
    const std::string key = v.value("encoder", std::string("windows"));
    const EncoderInfo* current = find_encoder(key);
    if (ImGui::BeginCombo("Kodlayıcı", current ? current->name : key.c_str())) {
        for (const char* group : {"Donanım", "Yazılım"}) {
            ImGui::SeparatorText(group);
            for (auto& e : encoder_list()) {
                if (strcmp(e.group, group)) continue;
                const int state = encoder_state(e.key);
                const bool no_stream = stream && !strcmp(e.codec, "vp9");
                std::string label = e.name;
                if (state == 0) label += "  (bu bilgisayarda yok)";
                if (state < 0) label += "  (deneniyor...)";
                if (no_stream) label += "  (yayında kullanılamaz)";
                ImGui::BeginDisabled(state != 1 || no_stream);
                if (ImGui::Selectable(label.c_str(), key == e.key) && key != e.key) {
                    const int bitrate = v.value("bitrate", 6000);
                    v = encoder_defaults(e.key, stream);
                    v["bitrate"] = bitrate;
                    changed = true;
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("En iyisini seç")) {
        const int bitrate = v.value("bitrate", 6000);
        v = encoder_defaults(encoder_best(stream), stream);
        v["bitrate"] = bitrate;
        changed = true;
    }
    const std::string k = v.value("encoder", std::string("windows"));
    const EncoderInfo* e = find_encoder(k);
    if (stream && e && strcmp(e->codec, "h264"))
        ImGui::TextColored(theme_warn(), "HEVC ve AV1 yayını Enhanced RTMP ister. YouTube destekler; diğer platformlarda H.264 en güvenli seçimdir.");
    if (k == "windows") {
        ImGui::TextDisabled("Görüntü doğrudan ekran kartında kodlanır, işlemciyi en az yoran yol budur.");
        return changed;
    }
    if (!stream) {
        changed |= edit_choice("Hız denetimi", v, "rate", {{"cq", "Sabit kalite (kayıt için en iyisi)"}, {"cbr", "CBR (sabit bit hızı)"}, {"vbr", "VBR (değişken bit hızı)"}});
        if (v.value("rate", std::string()) == "cq") {
            changed |= edit_int("Kalite değeri (düşük olan daha iyi)", v, "quality", 0, encoder_quality_max(k));
        } else {
            int kbps = v.value("bitrate", 6000);
            if (slider_int("Bit hızı (kbps)", &kbps, 1000, 100000)) { v["bitrate"] = (kbps + 50) / 100 * 100; changed = true; }
        }
        changed |= edit_int("Anahtar kare aralığı (sn, 0 otomatik)", v, "keyint", 0, 10);
    }
    const Choices presets = encoder_presets(k), profiles = encoder_profiles(k), tunes = encoder_tunes(k);
    if (!presets.empty()) changed |= edit_choice("Hız / kalite ön ayarı", v, "preset", presets);
    if (!profiles.empty()) changed |= edit_choice("Profil", v, "profile", profiles);
    if (!tunes.empty()) changed |= edit_choice("Ayar türü", v, "tune", tunes);
    if (k != "aom" && k != "vp9" && k.rfind("amf", 0) != 0) changed |= edit_int("B kareleri", v, "bframes", 0, 4);
    changed |= edit_text(k == "x264" || k == "x265" ? "Ek seçenekler (ad=değer)" : "Ek FFmpeg seçenekleri", v, "custom");
    if (!stream) {
        if (edit_choice("Dosya biçimi", v, "container", record_containers(k))) {
            changed = true;
            const Choices audio = record_audio_codecs(v.value("container", std::string("mp4")));
            bool valid = false;
            for (auto& a : audio) valid |= a.first == v.value("audio_codec", std::string("aac"));
            if (!valid) v["audio_codec"] = "aac";
        }
        changed |= edit_choice("Ses kodlayıcı", v, "audio_codec", record_audio_codecs(v.value("container", std::string("mp4"))));
    }
    return changed;
}

void App::page_stream() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Yayın");
    ImGui::PopFont();
    if (status_.streaming) ImGui::TextColored(theme_warn(), "Yayın sürerken bu ayarlar değiştirilemez.");
    else ImGui::TextDisabled("Yayın, Ekran Kaydı sahnenizi aynen gönderir. Kaynaklar ve sesler kayıttakiyle aynıdır.");
    ImGui::Spacing();
    ImGui::BeginChild("yayin", ImVec2(0, 0));
    ImGui::BeginDisabled(status_.streaming);
    ImGui::PushItemWidth(420);
    json& st = rec_["stream"];
    ImGui::SeparatorText("Yayın bilgileri");
    const std::string service = st.value("service", std::string("twitch"));
    const StreamService* current = &kServices[0];
    for (auto& s : kServices)
        if (service == s.key) current = &s;
    if (ImGui::BeginCombo("Platform", current->name)) {
        for (auto& s : kServices)
            if (ImGui::Selectable(s.name, current == &s) && current != &s) {
                st["service"] = s.key;
                st["server"] = s.server;
                current = &s;
                rec_dirty_ = true;
            }
        ImGui::EndCombo();
    }
    if (edit_text("Sunucu adresi", st, "server")) rec_dirty_ = true;
    static bool show_key = false;
    char key[256];
    strncpy_s(key, st.value("key", std::string()).c_str(), _TRUNCATE);
    if (ImGui::InputText(T("Yayın anahtarı"), key, sizeof(key), show_key ? 0 : ImGuiInputTextFlags_Password)) {
        st["key"] = std::string(key);
        rec_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Göster", &show_key);
    ImGui::TextWrapped("%s", current->hint);
    ImGui::TextDisabled("Yayın anahtarını kimseyle paylaşmayın. Bilgisayarınızda Windows hesabınıza bağlı şifreli saklanır.");

    ImGui::SeparatorText("Görüntü ve ses");
    const bool custom = st.value("mode", std::string("same")) == "custom";
    if (ImGui::RadioButton("Kayıt ayarlarıyla aynı", !custom)) { st["mode"] = "same"; rec_dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Özel", custom)) { st["mode"] = "custom"; rec_dirty_ = true; }
    ImGui::Spacing();
    if (!custom) {
        int W, H;
        engine_.canvas_size(W, H);
        const int fps = rec_.value("fps", 60);
        const json& video = rec_["video"];
        const bool windows_encoder = video.value("encoder", std::string("windows")) == "windows";
        const int full = windows_encoder ? recording_kbps(W, H, fps, rec_.value("quality", std::string("normal"))) : video.value("bitrate", 6000);
        const EncoderInfo* info = find_encoder(video.value("encoder", std::string("windows")));
        ImGui::Text("Kodlayıcı: %s", info ? info->name : "");
        ImGui::Text("%d x %d  ·  %d FPS  ·  %d kbps görüntü  ·  %d kbps ses", W, H, fps, std::min(kStreamMaxAutoKbps, full), rec_.value("audio_bitrate", 160));
        if (full > kStreamMaxAutoKbps)
            ImGui::TextDisabled("Kayıttaki bit hızı (%d kbps) platform sınırı için %d kbps ile sınırlandı.", full, kStreamMaxAutoKbps);
    } else {
        if (edit_choice("Çözünürlük", st, "resolution", {{"canvas", "Tuvalle aynı"}, {"1920x1080", "1920 x 1080"}, {"1600x900", "1600 x 900"}, {"1280x720", "1280 x 720"}, {"854x480", "854 x 480"}}))
            rec_dirty_ = true;
        const int stream_fps = st.value("fps", 60);
        if (ImGui::RadioButton("30 FPS", stream_fps == 30)) { st["fps"] = 30; rec_dirty_ = true; }
        ImGui::SameLine();
        if (ImGui::RadioButton("60 FPS", stream_fps == 60)) { st["fps"] = 60; rec_dirty_ = true; }
        int kbps = st.value("video_kbps", 4500);
        if (slider_int("Görüntü bit hızı (kbps)", &kbps, 1000, 10000)) { st["video_kbps"] = (kbps + 50) / 100 * 100; rec_dirty_ = true; }
        const int audio_kbps = st.value("audio_kbps", 160);
        if (ImGui::BeginCombo("Ses kalitesi", (std::to_string(audio_kbps) + " kbps").c_str())) {
            for (int v : {96, 128, 160, 192})
                if (ImGui::Selectable((std::to_string(v) + " kbps").c_str(), v == audio_kbps)) { st["audio_kbps"] = v; rec_dirty_ = true; }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Twitch için en çok 6000 kbps önerilir. Yükleme hızınız toplam bit hızının en az iki katı olmalı.");
        ImGui::SeparatorText("Kodlayıcı");
        if (encoder_settings(st["video"], true)) rec_dirty_ = true;
    }

    ImGui::SeparatorText("Yayın gecikmesi");
    if (!st.contains("delay")) st["delay"] = 0;
    if (edit_int("Gecikme (saniye)", st, "delay", 0, 600)) rec_dirty_ = true;
    ImGui::TextDisabled("Yayın bu kadar geriden gönderilir; izleyenler oyundaki yerinizi anında göremez. 0: gecikme yok.");

    ImGui::SeparatorText("Aynı anda başka platformlara");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Ana yayına ek olarak aynı görüntü ve ses bu hedeflere de gönderilir. Her hedef ayrı kodlanır ve ayrı yükleme hızı ister: "
                       "toplam yükleme hızınız, hedef sayısı kadar bit hızını kaldırmalı.");
    ImGui::PopStyleColor();
    json& targets = rec_["stream_targets"];
    if (!targets.is_array()) targets = json::array();
    int remove = -1;
    for (int i = 0; i < int(targets.size()); i++) {
        json& t = targets[i];
        ImGui::PushID(i);
        ImGui::Separator();
        if (edit_bool("##acik", t, "enabled")) rec_dirty_ = true;
        ImGui::SameLine();
        if (edit_text("Ad##hedef", t, "name")) rec_dirty_ = true;
        const std::string ts = t.value("service", std::string("youtube"));
        const StreamService* svc = &kServices[0];
        for (auto& s : kServices)
            if (ts == s.key) svc = &s;
        if (ImGui::BeginCombo("Platform##hedef", svc->name)) {
            for (auto& s : kServices)
                if (ImGui::Selectable(s.name, svc == &s) && svc != &s) { t["service"] = s.key; t["server"] = s.server; rec_dirty_ = true; }
            ImGui::EndCombo();
        }
        if (edit_text("Sunucu adresi##hedef", t, "server")) rec_dirty_ = true;
        char k[256];
        strncpy_s(k, t.value("key", std::string()).c_str(), _TRUNCATE);
        if (ImGui::InputText(T("Yayın anahtarı##hedef"), k, sizeof(k), show_key ? 0 : ImGuiInputTextFlags_Password)) { t["key"] = std::string(k); rec_dirty_ = true; }
        if (ImGui::Button("Hedefi kaldır")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) { targets.erase(remove); rec_dirty_ = true; }
    if (ImGui::Button("\xEE\x9C\x90  Yayın hedefi ekle") && targets.size() < 4) {
        targets.push_back({{"name", "YouTube"}, {"service", "youtube"}, {"server", kServices[1].server}, {"key", ""}, {"enabled", true}});
        rec_dirty_ = true;
    }
    ImGui::PopItemWidth();
    ImGui::EndDisabled();
    ImGui::EndChild();
}

// --- Sohbet ve bildirim pencereleri ---

// "open" listesinde anahtarı açık ya da kapalı işaretler (program yeniden açılınca açık olanlar geri gelir)
static void panel_mark_open(json& p, const std::string& key, bool on) {
    json list = json::array();
    if (p.contains("open") && p["open"].is_array())
        for (auto& k : p["open"])
            if (k.is_string() && k.get<std::string>() != key) list.push_back(k);
    if (on) list.push_back(key);
    p["open"] = list;
}

std::vector<App::PanelEntry> App::panel_entries() {
    std::vector<PanelEntry> out;
    const json st = rec_.value("stream", json::object());
    const std::string main_service = st.value("service", std::string("twitch"));
    out.push_back({"chat:main", TF("Sohbet · %s", T(find_service(main_service).name)), chat_url(main_service, st.value("chat", std::string())), main_service});
    const json targets = rec_.value("stream_targets", json::array());
    for (int i = 0; i < int(targets.size()); i++) {
        const json& t = targets[i];
        const std::string service = t.value("service", std::string("youtube"));
        std::string name = t.value("name", std::string());
        if (name.empty()) name = T("Ek hedef");
        out.push_back({"chat:" + std::to_string(i), TF("Sohbet · %s", name.c_str()), chat_url(service, t.value("chat", std::string())), service});
    }
    const json p = rec_.value("panels", json::object());
    out.push_back({"alerts", T("Bildirimler"), p.value("alerts_url", std::string()), ""});
    return out;
}

void App::panels_remember() {
    json& p = rec_["panels"];
    if (!p.is_object()) p = json::object();
    for (auto& [key, panel] : panels_) {
        const RECT r = panel->rect();
        p["rects"][key] = {r.left, r.top, r.right, r.bottom};
    }
}

void App::panel_toggle(const std::string& key) {
    json& p = rec_["panels"];
    if (!p.is_object()) p = json::object();
    save_dirty_ = true;
    save_at_ = now() + 0.3;
    if (auto it = panels_.find(key); it != panels_.end()) {
        const RECT r = it->second->rect();
        p["rects"][key] = {r.left, r.top, r.right, r.bottom};
        panels_.erase(it);
        panel_mark_open(p, key, false);
        return;
    }
    for (auto& e : panel_entries()) {
        if (e.key != key) continue;
        if (e.url.empty()) {
            message_ = key == "alerts" ? "Önce Ayarlar > Sohbet bölümünde etkinlik akışının adresini yazın"
                                       : "Önce Ayarlar > Sohbet bölümünde kanal adını ya da sohbet adresini yazın";
            message_error_ = true;
            panel_mark_open(p, key, false);
            return;
        }
        // Kayıtlı konum ekranlardan birinde değilse ana ekranın sağına yeni konum verilir
        RECT r{};
        bool placed = false;
        if (p.contains("rects") && p["rects"].contains(key) && p["rects"][key].is_array() && p["rects"][key].size() == 4) {
            const json& a = p["rects"][key];
            r = RECT{a[0].get<int>(), a[1].get<int>(), a[2].get<int>(), a[3].get<int>()};
            placed = r.right - r.left >= 200 && r.bottom - r.top >= 150 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        }
        if (!placed) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
            const int shift = int(panels_.size()) * 40;
            r = RECT{mi.rcWork.right - 420 - shift, mi.rcWork.top + 80 + shift, mi.rcWork.right - 20 - shift, mi.rcWork.top + 760 + shift};
        }
        panels_[key] = std::make_unique<BrowserPanel>(widen(e.title), e.url, r, p.value("topmost", false), p.value("hidden", true));
        panel_mark_open(p, key, true);
        return;
    }
    panel_mark_open(p, key, false);     // Artık olmayan bir hedef
}

void App::panels_tick() {
    if (panels_.empty()) return;
    json& p = rec_["panels"];
    const bool topmost = p.value("topmost", false), hidden = p.value("hidden", true);
    for (auto it = panels_.begin(); it != panels_.end();) {
        if (it->second->closed()) {     // Kullanıcı çarpıya bastı
            const RECT r = it->second->rect();
            p["rects"][it->first] = {r.left, r.top, r.right, r.bottom};
            panel_mark_open(p, it->first, false);
            it = panels_.erase(it);
            save_dirty_ = true;
            save_at_ = now() + 0.3;
            continue;
        }
        it->second->set_flags(topmost, hidden);
        ++it;
    }
}

void App::panels_restore() {
    const json p = rec_.value("panels", json::object());
    if (!p.contains("open") || !p["open"].is_array()) return;
    for (auto& k : p["open"])
        if (k.is_string() && !panels_.count(k.get<std::string>())) panel_toggle(k.get<std::string>());
}

void App::panels_close_all() {     // Çıkışta: konumlar saklanır, açık oldukları bilgisi kalır
    if (panels_.empty()) return;
    panels_remember();
    panels_.clear();
    save();
}

void App::chat_menu() {
    if (!ImGui::BeginMenu("Sohbet ve bildirimler")) return;
    for (auto& e : panel_entries())
        if (ImGui::MenuItem((e.title + "##" + e.key).c_str(), nullptr, panels_.count(e.key) > 0)) panel_toggle(e.key);
    ImGui::Separator();
    if (ImGui::MenuItem("Ayarlar...")) open_settings("chat");
    ImGui::EndMenu();
}

void App::page_chat() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Sohbet ve bildirimler");
    ImGui::PopFont();
    ImGui::TextDisabled("Bu pencereler yalnızca sizin içindir: sahneye eklenmez, izleyiciler görmez. Yayın sürerken de açılıp kapatılabilir.");
    ImGui::Spacing();
    ImGui::BeginChild("sohbet", ImVec2(0, 0));
    ImGui::PushItemWidth(420);
    json& p = rec_["panels"];
    if (!p.is_object()) p = json::object();
    bool dirty = false;

    ImGui::SeparatorText("Sohbet pencereleri");
    ImGui::TextDisabled("Her yayın adresinin kendi sohbet penceresi vardır. Platformu ve ek hedefleri Ayarlar > Yayın bölümünde seçin.");
    for (auto& e : panel_entries()) {
        if (e.key == "alerts") continue;
        json* holder = nullptr;
        if (e.key == "chat:main") holder = &rec_["stream"];
        else {
            const size_t i = size_t(std::stoi(e.key.substr(5)));
            if (rec_["stream_targets"].is_array() && i < rec_["stream_targets"].size()) holder = &rec_["stream_targets"][i];
        }
        if (!holder) continue;
        ImGui::PushID(e.key.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(e.title.c_str());
        const char* hint = e.service == "kick" || e.service == "twitch" ? "Kanal adı"
                         : e.service == "youtube" ? "Canlı yayının bağlantısı ya da video kimliği" : "Sohbet sayfasının tam adresi";
        char buf[512];
        strncpy_s(buf, holder->value("chat", std::string()).c_str(), _TRUNCATE);
        if (ImGui::InputTextWithHint("##adres", hint, buf, sizeof(buf))) {
            (*holder)["chat"] = std::string(buf);
            dirty = true;
            const std::string url = chat_url(e.service, buf);
            if (panels_.count(e.key) && !url.empty()) panels_[e.key]->navigate(url);
        }
        ImGui::SameLine();
        const bool open = panels_.count(e.key) > 0;
        if (ImGui::Button(open ? "Kapat" : "Aç", ImVec2(90, 0))) panel_toggle(e.key);
        if (e.service == "kick")
            ImGui::TextDisabled("Kick sohbeti bu pencerede okunur; mesaj yazmak için kick.com ya da Kick uygulamasını kullanın.");
        else if (e.service == "youtube")
            ImGui::TextDisabled("YouTube'da her canlı yayının kendi sohbeti vardır: yayını başlattıktan sonra bağlantısını yapıştırın.");
        else if (e.service != "twitch")
            ImGui::TextDisabled("Bu platformda kanal adıyla açılan sohbet sayfası yok; sohbetin ya da yorumların göründüğü sayfanın tam adresini yazın.");
        ImGui::PopID();
    }

    ImGui::SeparatorText("Bağış ve etkinlik bildirimleri");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("StreamElements, Streamlabs ya da kullandığınız uyarı servisinin etkinlik akışı (Activity Feed) adresini yazın. "
                       "Düğmelerle servisin paneli bu pencerede açılır; bir kez giriş yaptığınızda bu bilgisayarda hatırlanır.");
    ImGui::PopStyleColor();
    char alerts[512];
    strncpy_s(alerts, p.value("alerts_url", std::string()).c_str(), _TRUNCATE);
    if (ImGui::InputTextWithHint("##bildirim", "Etkinlik akışı adresi", alerts, sizeof(alerts))) {
        p["alerts_url"] = std::string(alerts);
        dirty = true;
        if (panels_.count("alerts") && alerts[0]) panels_["alerts"]->navigate(alerts);
    }
    ImGui::SameLine();
    if (ImGui::Button(panels_.count("alerts") ? "Kapat##bildirim" : "Aç##bildirim", ImVec2(90, 0))) panel_toggle("alerts");
    auto service_button = [&](const char* label, const char* url) {
        if (!ImGui::Button(label)) return;
        p["alerts_url"] = url;
        dirty = true;
        if (panels_.count("alerts")) { panels_["alerts"]->navigate(url); panels_["alerts"]->show(); }
        else panel_toggle("alerts");
    };
    service_button("StreamElements paneli", "https://streamelements.com/dashboard");
    ImGui::SameLine();
    service_button("Streamlabs paneli", "https://streamlabs.com/dashboard");

    ImGui::SeparatorText("Pencereler");
    if (edit_bool("Her zaman üstte", p, "topmost")) dirty = true;
    bool hidden = p.value("hidden", true);
    if (ImGui::Checkbox("Kayıtta ve yayında gizle", &hidden)) { p["hidden"] = hidden; dirty = true; }
    ImGui::TextDisabled("Açıkken bu pencereler ekran kaydına ve yayına girmez; yalnızca siz görürsünüz.");
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (dirty) { save_dirty_ = true; save_at_ = now() + 0.3; }
}

void App::page_theme() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Görünüm");
    ImGui::PopFont();
    ImGui::Spacing();
    if (theme_settings()) {
        save_dirty_ = true;
        save_at_ = now() + 0.3;
    }
    ImGui::Spacing();
    language_settings();
}

void App::page_settings() {
    ImGui::BeginChild("ayar_menu", ImVec2(210, -28), 0, ImGuiWindowFlags_NoScrollbar);
    if (nav_button(ICON_BACK, "Geri", "geri", false)) page_ = "record";
    ImGui::Spacing(); ImGui::Spacing();
    struct Item { const char* key; const char* name; const char* icon; };
    for (auto it : {Item{"profiles", "Profiller", ICON_PEOPLE}, Item{"record", "Kayıt", ICON_RECORD}, Item{"stream", "Yayın", ICON_STREAM}, Item{"chat", "Sohbet", ICON_CHAT}, Item{"theme", "Görünüm", ICON_THEME}, Item{"hotkeys", "Kısayollar", ICON_KEYS}, Item{"help", "Yardım", ICON_HELP}})
        if (nav_button(it.icon, it.name, it.key, settings_tab_ == it.key)) settings_tab_ = it.key;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ayar_icerik", ImVec2(0, -28));
    if (settings_tab_ == "profiles") page_profiles();
    else if (settings_tab_ == "record") tab_record();
    else if (settings_tab_ == "stream") page_stream();
    else if (settings_tab_ == "chat") page_chat();
    else if (settings_tab_ == "theme") page_theme();
    else if (settings_tab_ == "hotkeys") page_hotkeys();
    else help_tabs(kAppName, guide_record, [this] { page_developers(); }, help_request_);
    ImGui::EndChild();
}

void App::tab_record() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Kayıt");
    ImGui::PopFont();
    if (status_.recording || status_.streaming) ImGui::TextColored(theme_warn(), "Kayıt ya da yayın sürerken bu ayarlar değiştirilemez.");
    else ImGui::TextDisabled("Değişiklikler kendiliğinden kaydedilir ve sonraki kayıtta geçerli olur.");
    ImGui::Spacing();
    ImGui::BeginChild("ayar", ImVec2(0, 0));
    // Kayıt sürerken de değiştirilebilir; kayıt ayarı değil, yalnızca gösterge
    ImGui::SeparatorText("Ekranda geçen süre");
    if (edit_bool("Kayıt sırasında geçen süreyi ekranda göster", rec_, "show_timer")) { save_dirty_ = true; save_at_ = now() + 0.3; }
    if (edit_bool("Yayın sırasında geçen süreyi ekranda göster", rec_, "show_stream_timer")) { save_dirty_ = true; save_at_ = now() + 0.3; }
    if (rec_.value("show_timer", false) || rec_.value("show_stream_timer", false)) {
        ImGui::SetNextItemWidth(420);
        if (edit_choice("Konum##sure", rec_, "timer_corner", {{"tr", "Sağ üst"}, {"tl", "Sol üst"}, {"br", "Sağ alt"}, {"bl", "Sol alt"}})) {
            save_dirty_ = true;
            save_at_ = now() + 0.3;
        }
    }
    ImGui::TextDisabled("Süre ana ekranın köşesinde küçük bir kutuda görünür. Kaynak değildir: yalnızca siz görürsünüz, kayda ve yayına girmez.");
    ImGui::BeginDisabled(status_.recording || status_.streaming);
    ImGui::PushItemWidth(420);
    ImGui::SeparatorText("Kayıt klasörü");
    ImGui::TextWrapped("%s", rec_.value("folder", std::string()).c_str());
    if (ImGui::Button("Klasör seç...")) {
        const std::wstring folder = pick_folder(hwnd);
        if (!folder.empty()) { rec_["folder"] = narrow(folder); rec_dirty_ = true; }
    }
    ImGui::SameLine();
    if (ImGui::Button("Klasörü aç")) open_record_folder();
    ImGui::SeparatorText("Video");
    if (edit_choice("Çözünürlük", rec_, "resolution", {{"screen", "Ekranla aynı"}, {"1920x1080", "1920 x 1080"}, {"1600x900", "1600 x 900"}, {"1280x720", "1280 x 720"}}))
        rec_dirty_ = true;
    int fps = rec_.value("fps", 60);
    if (ImGui::RadioButton("30 FPS", fps == 30)) { rec_["fps"] = 30; rec_dirty_ = true; }
    ImGui::SameLine();
    if (ImGui::RadioButton("60 FPS", fps == 60)) { rec_["fps"] = 60; rec_dirty_ = true; }
    ImGui::SeparatorText("Kodlayıcı");
    if (encoder_settings(rec_["video"], false)) rec_dirty_ = true;
    if (rec_["video"].value("encoder", std::string("windows")) == "windows" &&
        edit_choice("Kalite", rec_, "quality", {{"high", "Yüksek"}, {"normal", "Normal"}, {"low", "Düşük (küçük dosya)"}})) rec_dirty_ = true;
    ImGui::SeparatorText("Ses");
    if (edit_bool("Bilgisayar sesini kaydet", rec_, "desktop_audio")) rec_dirty_ = true;
    if (edit_bool("Mikrofonu kaydet", rec_, "mic_audio")) rec_dirty_ = true;
    auto devices = [&](const char* label, const char* key, const std::vector<AudioDevice>& list) {
        std::vector<std::pair<std::string, std::string>> items{{"", "Varsayılan"}};
        for (auto& d : list) items.push_back({narrow(d.id), narrow(d.name)});
        if (edit_choice(label, rec_, key, items)) rec_dirty_ = true;
    };
    devices("Bilgisayar sesi", "desktop_device", speakers_);
    devices("Mikrofon", "mic_device", mics_);
    {
        const std::vector<std::pair<std::string, std::string>> routes{{"both", "Kayıt ve yayın"}, {"record", "Yalnızca kayıt"}, {"stream", "Yalnızca yayın"}};
        if (edit_choice("Bilgisayar sesi nereye gitsin", rec_, "desktop_route", routes)) rec_dirty_ = true;
        if (edit_choice("Mikrofon nereye gitsin", rec_, "mic_route", routes)) rec_dirty_ = true;
        ImGui::TextDisabled("Örneğin telifli müziği yalnızca yayına verip kayda almayabilirsiniz. Ek ses kaynakları için ayar düğmesine bakın.");
    }
    int kbps = rec_.value("audio_bitrate", 160);
    if (ImGui::BeginCombo("Ses kalitesi", (std::to_string(kbps) + " kbps").c_str())) {
        for (int v : {96, 128, 160, 192})
            if (ImGui::Selectable((std::to_string(v) + " kbps").c_str(), v == kbps)) { rec_["audio_bitrate"] = v; rec_dirty_ = true; }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText("Ses parçaları");
    if (edit_bool("Her ses kaynağını ayrı parçaya kaydet (çoklu ses parçası)", rec_, "multitrack")) rec_dirty_ = true;
    {
        const bool windows_encoder = rec_["video"].value("encoder", std::string("windows")) == "windows";
        ImGui::PushStyleColor(ImGuiCol_Text, windows_encoder && rec_.value("multitrack", false) ? theme_warn() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("İlk parça karışımdır; sonra bilgisayar sesi, mikrofon ve eklediğiniz her ses kaynağı ayrı parçaya yazılır. "
                           "Video Düzenleyici bunları ayrı ses kanallarına açar. Yalnızca FFmpeg kodlayıcılarında (x264, Quick Sync, NVENC, AMF...) çalışır; "
                           "Windows kodlayıcısında tek parça kaydedilir.");
        ImGui::PopStyleColor();
    }
    ImGui::SeparatorText("Kaydı bölme");
    if (edit_choice("Bölme", rec_, "split_mode", {{"off", "Kapalı"}, {"time", "Süreye göre"}, {"size", "Dosya boyutuna göre"}})) rec_dirty_ = true;
    if (rec_.value("split_mode", std::string("off")) == "time" && edit_int("Parça süresi (dakika)", rec_, "split_minutes", 1, 600)) rec_dirty_ = true;
    if (rec_.value("split_mode", std::string("off")) == "size" && edit_int("Parça boyutu (MB)", rec_, "split_mb", 50, 102400)) rec_dirty_ = true;
    ImGui::TextDisabled("Uzun kayıtlar belirlenen süre ya da boyutta yeni dosyaya geçer; kayıt durmaz.");
    ImGui::SeparatorText("Tekrar arabelleği");
    if (edit_int("Tutulacak süre (saniye)", rec_, "replay_seconds", 5, 600)) rec_dirty_ = true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Kontroller bölümünden açılır. Açıkken sahnenin son saniyeleri geçici klasörde sürekli tutulur; \"Tekrarı kaydet\" ya da Ctrl + F8 "
                       "ile kayıt klasörüne yazılır. Ayrı bir kodlayıcı çalıştığı için biraz daha işlemci / ekran kartı kullanır.");
    ImGui::PopStyleColor();
    ImGui::SeparatorText("Video Düzenleyici için");
    if (edit_bool("İmleç ve tıklamaları kaydet", rec_, "record_cursor")) rec_dirty_ = true;
    ImGui::TextDisabled("İmleç vurgusu ve tıklama halkası için. Yalnızca konum ve tıklama anı kaydedilir.");
    if (edit_bool("Kısayol tuşlarını kaydet", rec_, "record_keys")) rec_dirty_ = true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Tuş göstergesi için. Yalnızca Ctrl, Alt ya da Win ile basılan tuşlar, F tuşları ve Esc kaydedilir; "
                       "yazdığınız metinler ve şifreler kaydedilmez. Veri kaydın yanına \".input.json\" dosyası olarak yazılır, hiçbir yere gönderilmez.");
    ImGui::PopStyleColor();
    if (edit_bool(T("Kamerayı ayrı dosyaya kaydet"), rec_, "separate_camera")) rec_dirty_ = true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("Kamera kayıt sırasında ana videoya çizilmez, \"- camera.mp4\" olarak ayrı kaydedilir. Video Düzenleyici onu ayrı kanalda, sahnedeki yerine koyar; sonradan taşıyıp büyütebilirsiniz."));
    ImGui::PopStyleColor();
    if (edit_bool(T("Mikrofonu ayrı dosyaya kaydet"), rec_, "separate_mic")) rec_dirty_ = true;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", T("Mikrofon ana videonun sesine karışmaz, \"- microphone.wav\" olarak ayrı kaydedilir. Video Düzenleyici onu ayrı ses kanalına koyar; sesinizi oyun sesinden bağımsız ayarlarsınız."));
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();
    ImGui::EndDisabled();
    ImGui::SeparatorText("Başlangıç");
    bool autostart = autostart_on_;
    if (ImGui::Checkbox("Windows başlayınca otomatik başlat", &autostart)) {
        if (autostart_write(autostart)) {
            autostart_on_ = autostart;
            message_ = autostart ? "Kayıt Stüdyosu Windows açılınca gizli simgelerde başlayacak" : "Otomatik başlatma kapatıldı";
            message_error_ = false;
        } else {
            message_ = "Otomatik başlatma ayarlanamadı";
            message_error_ = true;
        }
    }
    ImGui::TextDisabled("Program saatin yanındaki gizli simgelerde açılır, kayıt ya da yayın kendiliğinden başlamaz. Pencerenin çarpısı da programı kapatmaz, oraya indirir.");
    ImGui::EndChild();
}

void App::page_record() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = ImGui::GetStyle().ItemSpacing.x, row_gap = ImGui::GetStyle().ItemSpacing.y;
    const float bar = 30;
    const ImVec2 origin = ImGui::GetCursorPos();
    // Kontroller panelindeki düğmeler sığsın diye alt sıra en az 300 piksel
    float bottom = std::clamp(float(layout_.value("bottom", 300.0)), 300.f, std::max(300.f, avail.y - bar - 2 * row_gap - 200));
    const float preview_h = avail.y - bottom - bar - 2 * row_gap;
    panel_preview(ImVec2(avail.x, preview_h));
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + preview_h));
    const bool bottom_moved = splitter("##ayirici_alt", false, row_gap, avail.x, &bottom, 300, std::max(300.f, avail.y - bar - 2 * row_gap - 200), -1);    // Yukarı çekince alt sıra büyür
    ImGui::SetCursorPos(ImVec2(origin.x, origin.y + preview_h + row_gap));

    // Seçili kaynak çubuğu
    json* sel = source(selected_);
    ImGui::AlignTextToFramePadding();
    if (sel) ImGui::TextUnformatted(sel->value("name", std::string()).c_str());
    else ImGui::TextDisabled("Kaynak seçilmemiş");
    ImGui::SameLine(220);
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(ICON_SETTINGS "  Özellikler", ImVec2(130, bar))) show_properties_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_CAMERA "  Ekran görüntüsü", ImVec2(170, bar))) take_screenshot();
    ImGui::SetItemTooltip("Tuvalin o anki karesini PNG olarak kayıt klasörüne kaydeder.");

    // Alt sıra: ayırıcılar sürüklenerek panel genişlikleri değişir, Ses Karıştırıcı kalan yeri alır
    float transition = std::clamp(float(layout_.value("transition", 200.0)), 160.f, 420.f);
    float controls = std::clamp(float(layout_.value("controls", 260.0)), 220.f, 480.f);
    bool resized = false;
    begin_panel("Ses Karıştırıcı", ImVec2(avail.x - transition - controls - 2 * gap, bottom));
    panel_audio();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    resized |= splitter("##ayirici_gecis", true, gap, bottom, &transition, 160, std::min(420.f, avail.x - controls - 2 * gap - 260), -1);
    ImGui::SameLine(0, 0);
    begin_panel("Sahne Geçişi", ImVec2(transition, bottom));
    panel_transition();
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    {   // Geçiş ile Kontroller arası: biri büyürken öteki küçülür
        float t = transition;
        if (splitter("##ayirici_kontrol", true, gap, bottom, &t, 160, std::min(420.f, transition + controls - 220), 1)) {
            controls -= t - transition;
            transition = t;
            resized = true;
        }
    }
    ImGui::SameLine(0, 0);
    begin_panel("Kontroller", ImVec2(controls, bottom));
    panel_record();
    ImGui::EndChild();
    if (resized || bottom_moved) {
        layout_["transition"] = transition; layout_["controls"] = controls; layout_["bottom"] = bottom;
        layout_dirty_ = true;
    }
}

void App::page_hotkeys() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Kısayollar");
    ImGui::PopFont();
    ImGui::TextDisabled("\"Değiştir\"e basıp yeni tuşa bas; istersen Ctrl, Shift ya da Alt ile birlikte. Esc vazgeçer. Fare yan tuşları da atanabilir.");
    ImGui::Spacing();
    ImGui::BeginChild("kisayol_liste", ImVec2(0, 0));
    std::lock_guard<std::mutex> g(hotkeys_lock_);
    auto action = [&](const char* key) {
        for (auto& a : kActions)
            if (!strcmp(a.key, key)) hotkey_row(a.key, a.name, &a.fallback);
    };
    ImGui::SeparatorText("Kayıt ve yayın");
    for (const char* k : {"record", "pause", "stream", "screenshot", "replay_toggle", "replay_save", "studio_transition", "vcam_toggle"}) action(k);
    ImGui::SeparatorText("Ses");
    for (const char* k : {"mute_mic", "mute_desktop", "push_to_talk"}) action(k);
    ImGui::TextDisabled("Bas-konuş, Mikrofon ayarları penceresinden açılır.");
    ImGui::SeparatorText("Sahneler");
    for (auto& s : doc_["scenes"]) {
        const std::string label = TF("\"%s\" sahnesine geç", s.value("name", std::string()).c_str());
        hotkey_row("scene:" + s.value("id", std::string()), label.c_str(), nullptr);
    }
    ImGui::SeparatorText("Kaynakları göster / gizle");
    for (auto& s : doc_["scenes"]) {
        if (s["sources"].empty()) continue;
        ImGui::TextDisabled("%s", s.value("name", std::string()).c_str());
        for (auto& src : s["sources"]) {
            const std::string label = "    " + src.value("name", std::string());
            hotkey_row("source:" + src.value("id", std::string()), label.c_str(), nullptr);
        }
    }
    ImGui::EndChild();
    if (capturing_.empty()) return;
    for (int vk = 4; vk < 255; vk++) {      // 4 orta tuş, 5-6 fare yan tuşları
        if (vk == 7) continue;
        if (vk >= VK_SHIFT && vk <= VK_MENU) continue;
        if (vk >= VK_LSHIFT && vk <= VK_RMENU) continue;
        if (vk == VK_LWIN || vk == VK_RWIN || !key_down(vk)) continue;
        const bool ctrl = key_down(VK_CONTROL), shift = key_down(VK_SHIFT), alt = key_down(VK_MENU);
        if (vk != VK_ESCAPE || ctrl || shift || alt) {
            const Hotkey h{vk, ctrl, shift, alt};
            bool used = false;
            for (auto& [name, other] : hotkeys_)
                used |= name != capturing_ && other.vk == h.vk && other.ctrl == h.ctrl && other.shift == h.shift && other.alt == h.alt;
            message_ = used ? TF("%s zaten başka bir işe atanmış", key_name(h).c_str()) : "";
            message_error_ = used;
            if (!used) { hotkeys_[capturing_] = h; save_dirty_ = true; save_at_ = now() + 0.3; }
        }
        capturing_.clear();
        break;
    }
}

// Kısayol satırı; hotkeys_lock_ tutulurken çağrılır. fallback yoksa "Varsayılan" yerine yalnızca "Temizle" olur
void App::hotkey_row(const std::string& key, const char* label, const Hotkey* fallback) {
    ImGui::PushID(key.c_str());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(340);
    auto it = hotkeys_.find(key);
    const Hotkey h = it == hotkeys_.end() ? Hotkey{} : it->second;
    if (capturing_ == key) ImGui::TextColored(theme_accent(), "%s", T("Bir tuşa bas..."));
    else if (h.vk) ImGui::TextColored(theme_accent(), "%s", key_name(h).c_str());
    else ImGui::TextDisabled("Yok");
    ImGui::SameLine(600);
    if (ImGui::Button("Değiştir")) capturing_ = key;
    ImGui::SameLine();
    if (fallback && ImGui::Button("Varsayılan")) { hotkeys_[key] = *fallback; save_dirty_ = true; save_at_ = now(); }
    if (fallback) ImGui::SameLine();
    ImGui::BeginDisabled(!h.vk);
    if (ImGui::Button("Temizle")) { hotkeys_[key] = Hotkey{}; save_dirty_ = true; save_at_ = now(); }
    ImGui::EndDisabled();
    ImGui::PopID();
}

void App::page_developers() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Geliştiriciler");
    ImGui::PopFont();
    ImGui::TextDisabled("Kayıt Stüdyosu'nu hazırlayan ekip");
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
    ImGui::BulletText("Simple Icons - sosyal medya logoları (CC0)");
    ImGui::BulletText("FFmpeg - video ve ses kodlama (GPLv3)");
    ImGui::BulletText("LibreHardwareMonitor - donanım sensörleri (MPL-2.0)");
    ImGui::Spacing();
    ImGui::TextDisabled("%s  ·  Sürüm %s", kAppName, kVersion);
}

static void vcam_popup(App* app, bool& ask, HWND hwnd, Engine& engine, std::string& message, bool& message_error) {
    if (ask) { ask = false; ImGui::OpenPopup("Sanal kamera kurulumu"); }
    if (!ImGui::BeginPopupModal("Sanal kamera kurulumu", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::PushTextWrapPos(520);
    ImGui::TextWrapped("Sanal kamera ilk kullanımda bir kez Windows'a tanıtılmalıdır. Bunun için yönetici izni istenir; kamera sürücüsü "
                       "\"ProgramData\\Recording Studio\" klasörüne kopyalanır. Windows 11 gerekir.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (ImGui::Button("Kur ve başlat", ImVec2(160, 34))) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = hwnd;
        sei.lpVerb = L"runas";
        sei.lpFile = exe;
        sei.lpParameters = L"--vcam-install";
        sei.nShow = SW_HIDE;
        DWORD code = 1;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 30000);
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
        }
        if (code == 0 && vcam_registered()) { engine.request_virtual_camera(true); message = "Sanal kamera kuruldu"; message_error = false; }
        else { message = "Sanal kamera kurulamadı"; message_error = true; }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(120, 34))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    (void)app;
}

void App::popup_exit() {
    vcam_popup(this, vcam_install_ask_, hwnd, engine_, message_, message_error_);
    if (want_close) {
        want_close = false;
        if (status_.recording || status_.streaming) ImGui::OpenPopup("Çıkış");
        else PostQuitMessage(0);
    }
    if (ImGui::BeginPopupModal("Çıkış", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Kayıt ya da yayın devam ediyor. Durdurulup program kapatılsın mı?");
        if (ImGui::Button("Durdur ve çık", ImVec2(160, 34))) PostQuitMessage(0);
        ImGui::SameLine();
        if (ImGui::Button("Vazgeç", ImVec2(120, 34))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
}

int App::run(int test_seconds, int stream_test) {
    std::string error;
    if (!engine_.start(error)) {
        MessageBoxW(hwnd, widen(error).c_str(), TW(kAppName).c_str(), MB_ICONERROR);
        return 1;
    }
    load();

    // Arayüzün kendi cihazı (motorla aynı ekran kartında); önizleme paylaşımlı doku ile gelir
    RECT rc;
    GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(D3D11CreateDeviceAndSwapChain(engine_.adapter(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                             &sd, &swap_, &device_, nullptr, &ctx_))) {
        MessageBoxW(hwnd, TW("Pencere çizimi başlatılamadı").c_str(), TW(kAppName).c_str(), MB_ICONERROR);
        return 1;
    }
    create_rtv();

    setup_imgui(hwnd);
    encoders_probe();
    theme_start(hwnd, theme_style_, theme_mode_);
    profiles_migrate();     // Tema hazır olduktan sonra: profil, temayı da içerir
    if (test_properties >= 0 && test_properties < int(scene()["sources"].size())) {
        selected_ = scene()["sources"][test_properties].value("id", std::string());
        show_properties_ = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());

    keys_thread_ = std::thread(&App::hotkey_thread, this);
    tray_add(hwnd, WM_APP + 2, kAppName);
    // Oyun Araçları yönetici olarak çalışsa da komut gönderebilsin
    ChangeWindowMessageFilterEx(hwnd, RegisterWindowMessageW(L"KayitStudyosuKomut"), MSGFLT_ALLOW, nullptr);
    if (start_minimized) {  // Windows açılınca: pencere açılmaz, simge gizli simgelerde bekler
    } else {
        splash_finish(4.0);     // Hazırlık bitti; açılış ekranı en az süresini doldurunca pencere açılır
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        SetForegroundWindow(hwnd);
    }
    const double started = now();
    bool test_started = false, stream_stopping = false;
    if (!test_seconds && !stream_test) panels_restore();    // Kapatılırken açık olan sohbet / bildirim pencereleri

    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;   // Ardından başka mesaj gelse de kapanma isteği kaybolmasın
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        lang_tick();

        if (doc_dirty_) {
            engine_.set_document(doc_);
            doc_dirty_ = false; save_dirty_ = true; save_at_ = now() + 0.3;
        }
        if (rec_dirty_) {
            engine_.set_record_settings(rec_);
            rec_dirty_ = false; save_dirty_ = true; save_at_ = now() + 0.3;
        }
        if (save_dirty_ && now() >= save_at_) { save(); save_dirty_ = false; }
        if (!pending_region_.empty()) {     // Arayüz karesinin dışında: seçim penceresi kendi döngüsünü çalıştırır
            const std::string id = std::move(pending_region_);
            pending_region_.clear();
            region_now(id);
            continue;
        }
        status_ = engine_.status();
        if (!status_.vcam_error.empty() && status_.vcam_error != shown_vcam_error_) { message_ = status_.vcam_error; message_error_ = true; }
        shown_vcam_error_ = status_.vcam_error;
        if (status_.replay_saves != replay_seen_) {
            replay_seen_ = status_.replay_saves;
            message_ = status_.replay_error.empty() ? TF("Tekrar kaydedildi: %s", narrow(status_.replay_path).c_str()) : status_.replay_error;
            message_error_ = !status_.replay_error.empty();
        }
        if (status_.shots != shots_seen_) {
            shots_seen_ = status_.shots;
            message_ = status_.shot_error.empty() ? TF("Ekran görüntüsü kaydedildi: %s", narrow(status_.shot_path).c_str()) : status_.shot_error;
            message_error_ = !status_.shot_error.empty();
        }
        boxes_ = engine_.boxes();
        if (was_recording_ && !status_.recording) {
            message_ = status_.error.empty() ? TF("Kaydedildi: %s", narrow(status_.last_path).c_str()) : TF("Kayıt sorunla bitti: %s", status_.error.c_str());
            message_error_ = !status_.error.empty();
        } else if (!was_recording_ && status_.recording) {
            message_ = TF("Kayıt başladı: %s", narrow(status_.path).c_str());
            message_error_ = false;
        } else if (!status_.error.empty() && !status_.recording && message_.rfind("Kayıt başlatılamadı", 0) != 0) {
            message_ = TF("Kayıt başlatılamadı: %s", status_.error.c_str());
            message_error_ = true;
        }
        was_recording_ = status_.recording;
        if (status_.camera_failed && !camera_error_shown_) {
            message_ = "Kameradan görüntü alınamadı. Başka bir uygulama kullanıyor olabilir.";
            message_error_ = true;
        }
        camera_error_shown_ = status_.camera_failed;
        if (status_.stream_error != shown_stream_error_) {
            shown_stream_error_ = status_.stream_error;
            if (!status_.stream_error.empty()) { message_ = TF("Yayın: %s", status_.stream_error.c_str()); message_error_ = true; }
        }
        if (was_streaming_ != status_.streaming) {
            if (status_.stream_error.empty()) { message_ = status_.streaming ? "Yayın başlatılıyor" : "Yayın durduruldu"; message_error_ = false; }
            was_streaming_ = status_.streaming;
        }
        // Pencere küçültülmüşken de güncellenir: kayıt çoğunlukla öyle yapılır
        timer_.update(status_.recording && rec_.value("show_timer", false), status_.paused, status_.elapsed,
                      status_.streaming && rec_.value("show_stream_timer", false), status_.stream_elapsed,
                      rec_.value("timer_corner", std::string("tr")));
        panels_tick();

        if (test_seconds) {   // Komut satırından deneme: kaydı başlat, süre dolunca durdur, çık
            if (!test_started && now() - started > 2) { engine_.request_record(true); test_started = true; }
            if (test_started && status_.recording && status_.elapsed >= test_seconds) engine_.request_record(false);
            if (test_started && !status_.recording && !status_.last_path.empty()) PostQuitMessage(0);
        }
        if (stream_test) {   // Komut satırından deneme: yayını aç, süre dolunca kapat, çık
            if (!test_started && now() - started > 2) { toggle_stream(); test_started = true; }
            if (test_started && status_.streaming && status_.stream_elapsed >= stream_test) { engine_.request_stream(false); stream_stopping = true; }
            if (test_started && ((stream_stopping && !status_.streaming) || !status_.stream_error.empty())) PostQuitMessage(0);
        }

        const bool iconic = window_hidden(hwnd);
        engine_.set_active(page_ == "record" && !iconic);
        if (iconic) {
            if (want_close) {   // Simgeden çıkış: kayıt varsa soru görünsün diye pencere açılır
                if (status_.recording || status_.streaming) show_window();
                else PostQuitMessage(0);
            }
            Sleep(30);
            continue;
        }

        // Sol menü yumuşakça açılıp kapanır
        const float menu_target = sidebar_open_ ? sidebar_open_w() : kMenuClosed;
        if (sidebar_dragging_) sidebar_w_ = menu_target;     // Sürüklerken gecikmesiz
        sidebar_w_ += (menu_target - sidebar_w_) * std::min(1.f, io.DeltaTime * 16);
        if (fabsf(menu_target - sidebar_w_) < 0.5f) sidebar_w_ = menu_target;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        theme_tick();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("ana", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_MenuBar);
        main_menu();
        if (page_ == "settings") {
            page_settings();
        } else {
            const float height = ImGui::GetContentRegionAvail().y - 28, gap = ImGui::GetStyle().ItemSpacing.x;
            ImGui::BeginChild("sol", ImVec2(sidebar_w_, -28), 0, ImGuiWindowFlags_NoScrollbar);
            sidebar();
            ImGui::EndChild();
            ImGui::SameLine(0, 0);
            if (sidebar_open_) {    // Sol menü genişliği sürüklenerek ayarlanır
                float w = sidebar_open_w();
                if (splitter("##ayirici_sol", true, gap, height, &w, 200, 480, 1)) { layout_["sidebar"] = w; layout_dirty_ = true; }
                sidebar_dragging_ = ImGui::IsItemActive();
            } else {
                ImGui::Dummy(ImVec2(gap, height));
            }
            ImGui::SameLine(0, 0);
            ImGui::BeginChild("orta", ImVec2(0, -28), 0, ImGuiWindowFlags_NoScrollbar);
            page_record();
            ImGui::EndChild();
            if (layout_dirty_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { layout_save("studio", layout_); layout_dirty_ = false; }
        }
        status_bar();
        layout_popup("studio", layout_);
        profile_popup();
        popup_exit();
        ImGui::End();
        if (page_ == "record") { panel_properties(); mic_window(); }
        stats_window();
        manual_frame(0);
        ImGui::Render();
        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        ctx_->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_->Present(1, 0);
    }
    panels_close_all();
    timer_.close();
    keys_halt_ = true;
    keys_thread_.join();
    tray_remove(hwnd);
    engine_.stop();
    save();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    mouse_position_fix(msg, lp);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == g_show_message) {    // İkinci açılış: var olan pencere öne gelir
        window_show(hwnd);
        return 0;
    }
    if (g_app && msg == taskbar_created_message()) { tray_add(hwnd, WM_APP + 2, kAppName); return 0; }  // Explorer yeniden başladı
    static const UINT remote_message = RegisterWindowMessageW(L"KayitStudyosuKomut");
    if (g_app && msg == remote_message) return g_app->remote(wp);
    switch (msg) {
        case WM_SIZE:
            if (g_app && wp != SIZE_MINIMIZED) g_app->on_resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_APP + 1:
            if (g_app) g_app->on_hotkeys();
            return 0;
        case WM_APP + 2:
            if (g_app) g_app->on_tray(UINT(LOWORD(lp)));
            return 0;
        case WM_KEYDOWN:
            if (g_app && !key_down(VK_CONTROL) && !key_down(VK_SHIFT) && !key_down(VK_MENU)) {
                if (wp == VK_F11 || (wp == VK_ESCAPE && g_app->is_fullscreen())) g_app->toggle_fullscreen();
            }
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // F10 ve Alt pencere menüsünü açıp sonraki tıklamayı yutmasın
            break;
        case WM_CLOSE:      // Çarpı gizli simgelere indirir; çıkış Dosya > Çıkış ya da simgenin menüsünden
            if (g_app) g_app->hide_to_tray();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {1280, 720};
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    int argc = 0, test_seconds = 0, stream_test = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--kayit-testi")) test_seconds = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--yayin-testi")) stream_test = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"--ozellik-testi")) g_test_properties = _wtoi(argv[i + 1]);
    }
    bool startup = false;
    int vcam_cmd = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--baslangic")) startup = true;
        if (!wcscmp(argv[i], L"--vcam-install")) vcam_cmd = 1;
        if (!wcscmp(argv[i], L"--vcam-uninstall")) vcam_cmd = 2;
    }
    if (vcam_cmd) {     // Yönetici olarak açılan kopya: yalnızca sanal kamerayı kurar / kaldırır, pencere açmaz
        LocalFree(argv);
        return vcam_install(vcam_cmd == 2);
    }
    LocalFree(argv);

    // Tek kopya çalışsın: iki kopya aynı ekranı yakalayamaz, ayarlar da birbirini ezer
    g_show_message = RegisterWindowMessageW(L"KayitStudyosuGoster");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\KayitStudyosu");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"KayitStudyosu", nullptr)) {
            if (!startup) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, g_show_message, 0, 0);
            }
        }
        return 0;
    }

    lang_init();
    if (!startup && !test_seconds && !stream_test)
        splash_start({L"\xE714", T(kAppName), T("Ekran kaydı, oyun yakalama ve canlı yayın"), TF("Sürüm %s", kVersion), T("Açık kaynak · Ücretsiz"), 0xe5484d});
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"KayitStudyosu";
    RegisterClassExW(&wc);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int w = int(sw * 0.85), h = int(sh * 0.85);
    App app;
    app.hwnd = CreateWindowW(wc.lpszClassName, TW(kAppName).c_str(), WS_OVERLAPPEDWINDOW, (sw - w) / 2, (sh - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    g_app = &app;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(app.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    app.start_minimized = startup;
    app.test_properties = g_test_properties;
    const int code = app.run(test_seconds, stream_test);
    g_app = nullptr;
    MFShutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
