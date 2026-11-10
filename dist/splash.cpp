// Açılış ekranı: program kendini hazırlarken ekranın ortasında görünür, ilerleme çubuğu dolar.
// Kendi iş parçacığında ve kendi penceresinde çalışır; ana iş parçacığı bu sırada ayarları ve ekran kartını hazırlar.
#include "language.h"

#include <algorithm>
#include <cmath>

namespace {
std::thread g_thread;
std::atomic<bool> g_ready{false}, g_done{false};
std::atomic<double> g_started{0}, g_minimum{4.0};

void present(HWND hwnd, const Bitmap& b, BYTE alpha) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = b.w; bi.bmiHeader.biHeight = -b.h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
        memcpy(bits, b.px.data(), b.px.size());
        HGDIOBJ old = SelectObject(dc, dib);
        RECT r;
        GetWindowRect(hwnd, &r);
        POINT pos{r.left, r.top}, src{0, 0};
        SIZE size{b.w, b.h};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
        UpdateLayeredWindow(hwnd, screen, &pos, &size, dc, &src, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void run(SplashInfo info) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_APPSTARTING);
    wc.lpszClassName = L"AcilisEkrani";
    RegisterClassExW(&wc);
    // Ana ekranın ortası; ölçek ekranın DPI'ına göre
    HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    HDC screen = GetDC(nullptr);
    const float k = std::max(1.f, GetDeviceCaps(screen, LOGPIXELSX) / 96.f);
    ReleaseDC(nullptr, screen);
    Bitmap first = draw_splash(info, k, 0, T("Başlatılıyor..."));
    const int x = (mi.rcWork.left + mi.rcWork.right - first.w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - first.h) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, widen(info.name).c_str(), WS_POPUP, x, y, first.w, first.h,
                                nullptr, nullptr, wc.hInstance, nullptr);
    present(hwnd, first, 0);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    const char* steps[] = {"Başlatılıyor...", "Ayarlar yükleniyor...", "Ekran kartı hazırlanıyor...", "Arayüz hazırlanıyor...", "Hazırlanıyor..."};
    double closing = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        const double t = now() - g_started.load(), minimum = g_minimum.load();
        // İlerleme süreye göre dolar; program hazır olmadan %90'ı geçmez
        float progress = float(1 - exp(-t / (minimum * 0.45)));
        if (!g_ready) progress = std::min(progress, 0.9f);
        if (g_ready && t >= minimum && !closing) closing = now();
        if (closing) progress = 1;
        const int step = std::min(4, int(progress * 4.4f));
        const std::string status = progress >= 1 ? T("Hazır") : T(steps[step]);
        const double fade_in = std::min(1.0, t / 0.25), fade_out = closing ? 1 - std::min(1.0, (now() - closing - 0.15) / 0.3) : 1;
        present(hwnd, draw_splash(info, k, progress, status), BYTE(255 * std::clamp(std::min(fade_in, fade_out), 0.0, 1.0)));
        if (closing && now() - closing > 0.45) break;
        Sleep(16);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    g_done = true;
}
}  // namespace

void splash_start(const SplashInfo& info) {
    if (!splash_enabled() || g_thread.joinable()) return;
    g_started = now();
    g_ready = false;
    g_done = false;
    g_thread = std::thread(run, info);
}

void splash_finish(double minimum_seconds) {
    if (!g_thread.joinable()) return;
    g_minimum = minimum_seconds;
    g_ready = true;
    g_thread.join();
}
