#include "browser.h"
#include "language.h"

#include <DispatcherQueue.h>
#include <inspectable.h>
#include <shlobj.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <wrl.h>

#include "webview2/WebView2.h"
#include "webview2/WebView2EnvironmentOptions.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wuc = winrt::Windows::UI::Composition;
using Microsoft::WRL::Callback;

bool browser_available() {
    LPWSTR version = nullptr;
    const bool ok = SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version;
    if (version) CoTaskMemFree(version);
    return ok;
}

namespace {

// Kullanıcının yazdığı adres: dosya yoluysa file:/// adresine çevrilir
std::wstring to_url(const std::string& text) {
    std::wstring u = widen(text);
    while (!u.empty() && u.back() == L' ') u.pop_back();
    while (!u.empty() && u.front() == L' ') u.erase(u.begin());
    if (u.empty()) return L"about:blank";
    if (u.find(L"://") != std::wstring::npos || u.rfind(L"about:", 0) == 0) return u;
    if (u.size() > 2 && u[1] == L':') {     // C:\... yerel dosya
        std::wstring out = L"file:///";
        for (wchar_t c : u) out += c == L'\\' ? L'/' : c;
        return out;
    }
    return L"https://" + u;
}

// Stil her sayfa yüklenişinde eklenir
std::wstring css_script(const std::string& css) {
    const std::string quoted = json(css).dump();     // JavaScript dizesi olarak güvenli
    return widen("(function(){var c=" + quoted + ";function a(){var s=document.createElement('style');s.textContent=c;"
                 "(document.head||document.documentElement).appendChild(s);}"
                 "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',a);else a();})();");
}

// Zaman uyumsuz WebView2 çağrıları bitene kadar bu iş parçacığının iletileri işlenir
template <typename Done>
bool pump_until(Done done, std::atomic<bool>& halt, double seconds) {
    const double end = now() + seconds;
    MSG msg;
    while (!done() && !halt && now() < end) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return done();
}

}  // namespace

BrowserCapture::BrowserCapture(IDXGIAdapter1* adapter, const BrowserOptions& options) : adapter_(adapter), options_(options) {
    options_.width = std::clamp(options_.width, 16, 3840) & ~1;
    options_.height = std::clamp(options_.height, 16, 2160) & ~1;
    thread_ = std::thread(&BrowserCapture::loop, this);
}

BrowserCapture::~BrowserCapture() {
    halt_ = true;
    thread_.join();
}

HANDLE BrowserCapture::handle() {
    std::lock_guard<std::mutex> g(lock_);
    return handle_;
}

std::string BrowserCapture::error() {
    std::lock_guard<std::mutex> g(lock_);
    return error_;
}

void BrowserCapture::loop() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    auto fail = [&](const char* text) { std::lock_guard<std::mutex> g(lock_); error_ = text; };
    const int W = options_.width, H = options_.height;
    {
        // Görsel ağaç (Windows.UI.Composition) bu iş parçacığında, kendi ileti kuyruğuyla çalışır
        DispatcherQueueOptions dq{sizeof(dq), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        ABI::Windows::System::IDispatcherQueueController* raw_queue = nullptr;
        if (FAILED(CreateDispatcherQueueController(dq, &raw_queue))) { fail("Tarayıcı kaynağı başlatılamadı"); winrt::uninit_apartment(); return; }
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        winrt::attach_abi(queue, raw_queue);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
        ComPtr<IDXGIDevice> dxgi;
        if (device) device.As(&dxgi);
        wgd::Direct3D11::IDirect3DDevice rt_device{nullptr};
        if (dxgi) {
            winrt::com_ptr<::IInspectable> inspectable;
            if (SUCCEEDED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) rt_device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
        }
        if (!rt_device) { fail("Tarayıcı kaynağı için ekran kartı açılamadı"); }

        wuc::Compositor compositor;
        wuc::ContainerVisual root = compositor.CreateContainerVisual();
        root.Size({float(W), float(H)});

        // WebView2 bir üst pencere ister: görünmez, görev çubuğunda yok, odak almaz
        HWND host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP, 0, 0, W, H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

        // Arka planda da tam hızda çizsin; ses kendiliğinden başlasın (uyarı sesleri)
        auto env_options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        env_options->put_AdditionalBrowserArguments(L"--disable-background-timer-throttling --disable-renderer-backgrounding "
                                                    L"--disable-backgrounding-occluded-windows --autoplay-policy=no-user-gesture-required");
        PWSTR local = nullptr;
        std::wstring user_data;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) user_data = std::wstring(local) + L"\\Recording Studio\\Browser";
        CoTaskMemFree(local);

        ComPtr<ICoreWebView2Environment> env;
        ComPtr<ICoreWebView2CompositionController> composition;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        bool env_done = false, controller_done = false;
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.empty() ? nullptr : user_data.c_str(), env_options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT result, ICoreWebView2Environment* e) -> HRESULT {
                if (SUCCEEDED(result)) env = e;
                env_done = true;
                return S_OK;
            }).Get());
        if (FAILED(hr) || !pump_until([&] { return env_done; }, halt_, 30) || !env) {
            if (!halt_) fail("Tarayıcı açılamadı. Microsoft Edge WebView2 Runtime kurulu olmalı.");
        } else {
            ComPtr<ICoreWebView2Environment3> env3;
            if (SUCCEEDED(env.As(&env3)))
                env3->CreateCoreWebView2CompositionController(host,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [&](HRESULT result, ICoreWebView2CompositionController* c) -> HRESULT {
                            if (SUCCEEDED(result)) composition = c;
                            controller_done = true;
                            return S_OK;
                        }).Get());
            pump_until([&] { return controller_done; }, halt_, 30);
        }
        if (composition) {
            composition->put_RootVisualTarget(reinterpret_cast<::IUnknown*>(winrt::get_abi(root)));
            composition.As(&controller);
        }
        if (controller) {
            controller->put_Bounds(RECT{0, 0, W, H});
            controller->put_IsVisible(TRUE);
            ComPtr<ICoreWebView2Controller2> c2;
            if (SUCCEEDED(controller.As(&c2))) c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});    // Saydam
            controller->get_CoreWebView2(&webview);
        }
        if (webview) {
            ComPtr<ICoreWebView2Settings> settings;
            if (SUCCEEDED(webview->get_Settings(&settings))) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
            }
            ComPtr<ICoreWebView2_8> w8;
            if (options_.muted && SUCCEEDED(webview.As(&w8))) w8->put_IsMuted(TRUE);
            if (!options_.css.empty()) webview->AddScriptToExecuteOnDocumentCreated(css_script(options_.css).c_str(), nullptr);
            webview->Navigate(to_url(options_.url).c_str());
        } else if (!halt_ && error().empty()) {
            fail("Tarayıcı açılamadı. Microsoft Edge WebView2 Runtime kurulu olmalı.");
        }

        // Görsel ağaç yakalanır: pencere görünmese de çizim gelir, saydamlık korunur
        wgc::GraphicsCaptureItem item{nullptr};
        wgc::Direct3D11CaptureFramePool pool{nullptr};
        wgc::GraphicsCaptureSession session{nullptr};
        if (webview && rt_device) {
            try {
                item = wgc::GraphicsCaptureItem::CreateFromVisual(root);
                pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, {W, H});
                session = pool.CreateCaptureSession(item);
                try { session.IsCursorCaptureEnabled(false); } catch (...) {}
                try { session.IsBorderRequired(false); } catch (...) {}
                session.StartCapture();
            } catch (...) {
                fail("Tarayıcı görüntüsü alınamadı");
                session = nullptr; pool = nullptr;
            }
        }
        ComPtr<ID3D11Texture2D> latest;
        ComPtr<IDXGIKeyedMutex> keyed;
        if (pool) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &latest))) {
                latest.As(&keyed);
                ComPtr<IDXGIResource> res;
                HANDLE shared = nullptr;
                latest.As(&res);
                res->GetSharedHandle(&shared);
                std::lock_guard<std::mutex> g(lock_);
                handle_ = shared;
            }
        }
        MSG msg;
        while (!halt_) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            if (reload_.exchange(false) && webview) webview->Reload();
            if (!pool || !keyed) continue;
            wgc::Direct3D11CaptureFrame frame{nullptr};
            try { frame = pool.TryGetNextFrame(); } catch (...) {}
            if (!frame) continue;
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) continue;
            D3D11_TEXTURE2D_DESC fd{};
            tex->GetDesc(&fd);
            if (keyed->AcquireSync(0, 10) == S_OK) {
                D3D11_BOX box{0, 0, 0, std::min<UINT>(fd.Width, UINT(W)), std::min<UINT>(fd.Height, UINT(H)), 1};
                ctx->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                keyed->ReleaseSync(0);
                fresh_ = true;
            }
        }
        if (session) session.Close();
        if (pool) pool.Close();
        if (controller) controller->Close();
        webview.Reset(); controller.Reset(); composition.Reset(); env.Reset();
        if (host) DestroyWindow(host);
        {
            std::lock_guard<std::mutex> g(lock_);
            handle_ = nullptr;
        }
    }
    winrt::uninit_apartment();
}

// --- Sohbet ve bildirim pencereleri ---

BrowserPanel::BrowserPanel(const std::wstring& title, const std::string& url, RECT rect, bool topmost, bool hidden)
    : title_(title), url_(url), rect_(rect) {
    topmost_ = topmost;
    hidden_ = hidden;
    thread_ = std::thread(&BrowserPanel::loop, this);
}

BrowserPanel::~BrowserPanel() {
    halt_ = true;
    if (thread_.joinable()) thread_.join();
}

RECT BrowserPanel::rect() {
    std::lock_guard<std::mutex> g(lock_);
    return rect_;
}

void BrowserPanel::set_flags(bool topmost, bool hidden) {
    if (topmost == topmost_ && hidden == hidden_) return;
    topmost_ = topmost;
    hidden_ = hidden;
    flags_changed_ = true;
}

void BrowserPanel::navigate(const std::string& url) {
    std::lock_guard<std::mutex> g(lock_);
    pending_url_ = url;
}

void BrowserPanel::show() { show_ = true; }

// Pencere ayarları yalnızca pencerenin kendi iş parçacığında değiştirilir
void BrowserPanel::apply_flags() {
    if (!hwnd_) return;
    SetWindowDisplayAffinity(hwnd_, hidden_ ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    SetWindowPos(hwnd_, topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK BrowserPanel::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<BrowserPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        if (self->controller_) {
            RECT r;
            GetClientRect(h, &r);
            static_cast<ICoreWebView2Controller*>(self->controller_)->put_Bounds(r);
        }
        [[fallthrough]];
    case WM_MOVE:
        if (!IsIconic(h) && !IsZoomed(h)) {
            std::lock_guard<std::mutex> g(self->lock_);
            GetWindowRect(h, &self->rect_);
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {240, 200};
        return 0;
    case WM_CLOSE:
        self->closed_ = true;       // Pencere iş parçacığının döngüsü biter, pencere orada kapanır
        return 0;
    case WM_PAINT: {                // Sayfa açılamadıysa nedeni yazılır
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, HBRUSH(GetStockObject(BLACK_BRUSH)));
        std::wstring text;
        {
            std::lock_guard<std::mutex> g(self->lock_);
            text = widen(self->error_);
        }
        if (!text.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 230));
            InflateRect(&r, -16, -16);
            DrawTextW(dc, text.c_str(), -1, &r, DT_WORDBREAK);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void BrowserPanel::loop() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    static std::once_flag registered;
    std::call_once(registered, [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = BrowserPanel::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"KayitStudyosuPanel";
        RegisterClassExW(&wc);
    });
    const RECT r = rect_;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, L"KayitStudyosuPanel", title_.c_str(), WS_OVERLAPPEDWINDOW, r.left, r.top,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) { winrt::uninit_apartment(); closed_ = true; return; }
    apply_flags();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    auto fail = [&](const char* text) {
        { std::lock_guard<std::mutex> g(lock_); error_ = T(text); }
        InvalidateRect(hwnd_, nullptr, TRUE);
    };
    // Tarayıcı kaynağından ayrı veri klasörü: ortam seçenekleri farklı, iki ortam aynı klasörü paylaşamaz
    PWSTR local = nullptr;
    std::wstring user_data;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) user_data = std::wstring(local) + L"\\Recording Studio\\Panels";
    CoTaskMemFree(local);
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool env_done = false, controller_done = false;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.empty() ? nullptr : user_data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT result, ICoreWebView2Environment* e) -> HRESULT {
            if (SUCCEEDED(result)) env = e;
            env_done = true;
            return S_OK;
        }).Get());
    std::atomic<bool> stop{false};
    auto done = [&] { return halt_ || closed_; };
    if (SUCCEEDED(hr)) pump_until([&] { return env_done || done(); }, stop, 30);
    if (env) {
        env->CreateCoreWebView2Controller(hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [&](HRESULT result, ICoreWebView2Controller* c) -> HRESULT {
                if (SUCCEEDED(result)) controller = c;
                controller_done = true;
                return S_OK;
            }).Get());
        pump_until([&] { return controller_done || done(); }, stop, 30);
    }
    if (controller) {
        controller_ = controller.Get();
        RECT client;
        GetClientRect(hwnd_, &client);
        controller->put_Bounds(client);
        controller->put_IsVisible(TRUE);
        controller->get_CoreWebView2(&webview);
    }
    if (webview) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
        }
        webview->Navigate(to_url(url_).c_str());
    } else if (!done()) {
        fail("Tarayıcı açılamadı. Microsoft Edge WebView2 Runtime kurulu olmalı.");
    }

    MSG msg;
    while (!done()) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (flags_changed_.exchange(false)) apply_flags();
        if (show_.exchange(false)) {
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }
        std::string next;
        {
            std::lock_guard<std::mutex> g(lock_);
            next.swap(pending_url_);
        }
        if (!next.empty() && webview) webview->Navigate(to_url(next).c_str());
    }
    controller_ = nullptr;
    if (controller) controller->Close();
    webview.Reset(); controller.Reset(); env.Reset();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    winrt::uninit_apartment();
}
