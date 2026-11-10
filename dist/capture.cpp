#include "capture.h"

#include <inspectable.h>
#include <dwmapi.h>
#include <psapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace {

std::string process_name(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) {
        name = path;
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(p);
    return narrow(name);
}

// Kullanıcının gördüğü ana pencere mi?
bool real_window(HWND h) {
    if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return false;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow"})
        if (!wcscmp(cls, shell)) return false;
    return true;
}

bool fullscreen(HWND h) {
    RECT r;
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

BOOL CALLBACK collect(HWND h, LPARAM data) {
    if (real_window(h)) reinterpret_cast<std::vector<HWND>*>(data)->push_back(h);
    return TRUE;
}

std::vector<HWND> windows() {
    std::vector<HWND> list;
    EnumWindows(collect, LPARAM(&list));
    return list;
}

HWND find_target(const std::string& exe, const std::string& title, HWND current) {
    if (!exe.empty()) {
        HWND best = nullptr;
        LONG area = 0;
        for (HWND h : windows()) {
            if (_stricmp(process_name(h).c_str(), exe.c_str())) continue;
            if (!title.empty()) {   // Başlığı aynı olan pencere öncelikli
                wchar_t text[256];
                GetWindowTextW(h, text, 256);
                if (narrow(text) == title) return h;
            }
            RECT r;
            GetWindowRect(h, &r);
            const LONG a = (r.right - r.left) * (r.bottom - r.top);
            if (a > area) { area = a; best = h; }
        }
        return best;
    }
    // Tam ekran uygulama: oyun simge durumuna inse de oyundan çıkılana kadar onda kal
    if (current && IsWindow(current) && IsWindowVisible(current)) return current;
    HWND front = GetForegroundWindow();
    return front && real_window(front) && fullscreen(front) ? front : nullptr;
}

}  // namespace

std::vector<GameWindow> list_game_windows() {
    std::vector<GameWindow> out;
    for (HWND h : windows()) {
        wchar_t title[256];
        GetWindowTextW(h, title, 256);
        const std::string exe = process_name(h);
        if (!exe.empty()) out.push_back({exe, narrow(title)});
    }
    return out;
}

WindowCapture::WindowCapture(IDXGIAdapter1* adapter, const std::string& exe, bool cursor, const std::string& title, bool client, bool game)
    : adapter_(adapter), exe_(exe), title_(title), cursor_(cursor), client_(client), game_(game) {
    thread_ = std::thread(&WindowCapture::loop, this);
}

WindowCapture::~WindowCapture() {
    halt_ = true;
    thread_.join();
}

HANDLE WindowCapture::handle() {
    std::lock_guard<std::mutex> g(lock_);
    return handle_;
}

void WindowCapture::loop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx))) {
        winrt::uninit_apartment();
        return;
    }
    ComPtr<IDXGIDevice> dxgi;
    device.As(&dxgi);
    wgd::Direct3D11::IDirect3DDevice rt_device{nullptr};
    {
        winrt::com_ptr<::IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()))) { winrt::uninit_apartment(); return; }
        rt_device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();
    }

    HWND target = nullptr;
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
    ComPtr<ID3D11Texture2D> latest, probe;
    ComPtr<IDXGIKeyedMutex> keyed;
    double next_look = 0, last_frame = 0, next_probe = 0;
    int dark = 0;
    D3D11_BOX client_box{};

    auto close_session = [&] {
        if (session) session.Close();
        if (pool) pool.Close();
        session = nullptr; pool = nullptr; item = nullptr;
    };

    while (!halt_) {
        const double t = now();
        if (t >= next_look) {
            next_look = t + 1;
            HWND found = find_target(exe_, title_, target);
            target_found_ = found != nullptr;
            if (found != target) {
                close_session();
                target = found;
                dark = 0;
                last_frame = t;
                stalled_ = false;
                if (target) {
                    try {
                        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                        winrt::check_hresult(factory->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
                        size = item.Size();
                        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                        session = pool.CreateCaptureSession(item);
                        session.IsCursorCaptureEnabled(cursor_);
                        try { session.IsBorderRequired(false); } catch (...) {}     // Eski Windows'ta sarı çerçeve kalır
                        session.StartCapture();
                    } catch (...) {
                        close_session();
                    }
                }
            }
            if (target && t - last_frame > 3) stalled_ = true;
            if (game_ && (!target || stalled_)) g_capture_active = false;
            client_box = {};
            if (client_ && target) {    // Pencere içeriğinin karedeki yeri (kare, pencerenin DWM çerçevesidir)
                RECT frame, inner;
                POINT origin{0, 0};
                if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
                    GetClientRect(target, &inner) && ClientToScreen(target, &origin)) {
                    client_box.left = UINT(std::max(0L, origin.x - frame.left));
                    client_box.top = UINT(std::max(0L, origin.y - frame.top));
                    client_box.right = client_box.left + UINT(inner.right);
                    client_box.bottom = client_box.top + UINT(inner.bottom);
                }
            }
        }
        if (!pool) { Sleep(50); continue; }
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try { frame = pool.TryGetNextFrame(); } catch (...) {}
        if (!frame) { Sleep(1); continue; }
        if (game_) g_capture_frames++;

        const auto content = frame.ContentSize();
        if (content.Width != size.Width || content.Height != size.Height) {
            size = content;
            try { pool.Recreate(rt_device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size); } catch (...) {}
            continue;       // Bu karenin boyutu eski havuza göre: yenisini bekle
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(access->GetInterface(IID_PPV_ARGS(&tex)))) continue;

        D3D11_BOX box{0, 0, 0, UINT(size.Width), UINT(size.Height), 1};
        {
            const UINT r = std::min(client_box.right, UINT(size.Width)), b = std::min(client_box.bottom, UINT(size.Height));
            if (r > client_box.left + 16 && b > client_box.top + 16) { box.left = client_box.left; box.top = client_box.top; box.right = r; box.bottom = b; }
        }
        const UINT out_w = box.right - box.left, out_h = box.bottom - box.top;
        D3D11_TEXTURE2D_DESC td{};
        if (latest) latest->GetDesc(&td);
        if (!latest || td.Width != out_w || td.Height != out_h) {
            td = {};
            td.Width = out_w; td.Height = out_h; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            latest.Reset(); keyed.Reset();
            if (FAILED(device->CreateTexture2D(&td, nullptr, &latest))) continue;
            latest.As(&keyed);
            ComPtr<IDXGIResource> res;
            HANDLE shared = nullptr;
            latest.As(&res);
            res->GetSharedHandle(&shared);
            std::lock_guard<std::mutex> g(lock_);
            handle_ = shared;
        }
        if (keyed->AcquireSync(0, 10) == S_OK) {
            ctx->CopySubresourceRegion(latest.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
            keyed->ReleaseSync(0);
            fresh_ = true;
        }

        // Özel tam ekran oyunlarda kareler gelir ama simsiyah olur: arada bir ortadan küçük bir parçaya bak
        if (now() >= next_probe) {
            next_probe = now() + 1;
            if (!probe) {
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width = sd.Height = 32; sd.MipLevels = sd.ArraySize = 1;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
                sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                device->CreateTexture2D(&sd, nullptr, &probe);
            }
            const UINT cx = UINT(size.Width) / 2, cy = UINT(size.Height) / 2;
            D3D11_BOX box{cx > 16 ? cx - 16 : 0, cy > 16 ? cy - 16 : 0, 0, 0, 0, 1};
            box.right = box.left + 32; box.bottom = box.top + 32;
            D3D11_MAPPED_SUBRESOURCE m;
            if (probe && size.Width > 64 && size.Height > 64) {
                ctx->CopySubresourceRegion(probe.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
                if (SUCCEEDED(ctx->Map(probe.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    bool black = true;
                    for (int y = 0; y < 32 && black; y++) {
                        const uint8_t* row = static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                        for (int x = 0; x < 32 * 4 && black; x++)
                            if ((x & 3) != 3 && row[x]) black = false;
                    }
                    ctx->Unmap(probe.Get(), 0);
                    dark = black ? dark + 1 : 0;
                }
            }
        }
        last_frame = now();
        stalled_ = dark >= 4;
        if (game_) g_capture_active = !stalled_;
    }
    close_session();
    g_capture_active = false;
    winrt::uninit_apartment();
}
