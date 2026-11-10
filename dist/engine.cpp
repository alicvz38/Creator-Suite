#include "engine.h"
#include "recorder.h"
#include "stream.h"
#include "capture.h"
#include "fps_meter.h"
#include "browser.h"
#include "vcam_shared.h"

#include <mfvirtualcamera.h>
#include <sddl.h>

#include <d3dcompiler.h>
#include <mferror.h>
#include <propvarutil.h>
#include <timeapi.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {

const double kFade = 0.3;           // Sahne geçişi (sn)
const double kIdleClose = 60;       // Sayfa bu kadar gizli kalırsa kaynaklar ve ses kapanır (sn)

// Kaynak filtreleri de burada: kırpma (uvr), renk düzeltme, yeşil perde, keskinleştirme, maske
const char* kShader = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Quad : register(b0) {
    float4 rect; float opacity; float opaque; float2 texel;
    float4 uvr;         // Dokudan alınan bölge
    float4 clip;        // Görünen kutu, tuval pikseli (x0, y0, x1, y1): kırpma sınırı ve maske
    float4 color;       // parlaklık, kontrast, doygunluk, gama üssü
    float4 color2;      // renk tonu (radyan), keskinlik, maske (0 yok, 1 yuvarlak köşe, 2 daire), köşe yarıçapı
    float4 key;         // anahtar rengi, açık
    float4 keyp;        // benzerlik, yumuşaklık, taşma azaltma, baskın kanal
    float4 flags;       // renk düzeltme açık, sınır açık
};
struct VS { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS vs(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);
    VS o;
    float2 p = rect.xy + c * rect.zw;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = uvr.xy + c * uvr.zw;
    return o;
}
float2 chroma(float3 c) { return float2(dot(c, float3(-0.1687, -0.3313, 0.5)), dot(c, float3(0.5, -0.4187, -0.0813))); }
float4 ps(VS i) : SV_Target {
    float4 c = tex.Sample(smp, i.uv);
    if (color2.y > 0) {
        float4 n = tex.Sample(smp, i.uv + float2(texel.x, 0)) + tex.Sample(smp, i.uv - float2(texel.x, 0)) +
                   tex.Sample(smp, i.uv + float2(0, texel.y)) + tex.Sample(smp, i.uv - float2(0, texel.y));
        c.rgb = clamp(c.rgb + color2.y * (c.rgb * 4 - n.rgb), 0, max(c.a, opaque));
    }
    if (opaque > 0.5) c.a = 1;
    float3 rgb = c.a > 0 ? c.rgb / c.a : 0;
    if (flags.x > 0.5) {
        rgb = (rgb + color.x - 0.5) * color.y + 0.5;
        rgb = lerp(dot(rgb, float3(0.299, 0.587, 0.114)), rgb, color.z);
        if (color2.x != 0) {
            const float3 k = 0.57735;
            float ca = cos(color2.x);
            rgb = rgb * ca + cross(k, rgb) * sin(color2.x) + k * dot(k, rgb) * (1 - ca);
        }
        rgb = pow(saturate(rgb), color.w);
    }
    if (key.w > 0.5) {
        float d = distance(chroma(rgb), chroma(key.rgb));
        c.a *= pow(saturate((d - keyp.x) / max(keyp.y, 0.0001)), 1.5);
        if (keyp.z > 0) {
            if (keyp.w < 0.5) rgb.r = lerp(rgb.r, min(rgb.r, max(rgb.g, rgb.b)), keyp.z);
            else if (keyp.w < 1.5) rgb.g = lerp(rgb.g, min(rgb.g, max(rgb.r, rgb.b)), keyp.z);
            else rgb.b = lerp(rgb.b, min(rgb.b, max(rgb.r, rgb.g)), keyp.z);
        }
    }
    float m = 1;
    if (flags.y > 0.5) {
        float2 p = i.pos.xy;
        if (p.x < clip.x || p.y < clip.y || p.x > clip.z || p.y > clip.w) discard;
        if (color2.z > 0.5) {
            float2 h = (clip.zw - clip.xy) / 2, q = p - (clip.xy + clip.zw) / 2;
            float dist;
            if (color2.z > 1.5) {
                dist = length(q) - min(h.x, h.y);
            } else {
                float r = min(color2.w, min(h.x, h.y));
                float2 e = abs(q) - h + r;
                dist = length(max(e, 0)) + min(max(e.x, e.y), 0) - r;
            }
            m = saturate(0.5 - dist);
        }
    }
    return float4(saturate(rgb) * c.a, c.a) * opacity * m;   // Önceden çarpılmış
}
)";

struct Quad {
    float rect[4]; float opacity; float opaque; float texel[2];
    float uvr[4], clip[4], color[4], color2[4], key[4], keyp[4], flags[4];
};

// Bir kaynağın filtreleri (cfg["filters"]); kaynak çizilirken Gfx'te durur
struct Fx {
    bool active = false;
    float crop[4] = {};             // Sol, üst, sağ, alt (kaynağın oranı)
    bool color = false;
    float brightness = 0, contrast = 1, saturation = 1, gamma = 1, hue = 0;
    float sharpen = 0;
    int mask = 0;                   // 1 yuvarlak köşe, 2 daire
    float radius = 0;
    bool key = false;
    float key_rgb[3] = {}, similarity = 0.4f, smoothness = 0.08f, spill = 0.5f;
    int channel = 1;
    bool clip_on = false;
    float clip[4] = {};             // Ana karenin görünen kutusu; imleç gibi ek çizimler buna kırpılır
};

Fx parse_fx(const json& cfg) {
    Fx fx;
    auto it = cfg.find("filters");
    if (it == cfg.end() || !it->is_object()) return fx;
    const json& f = *it;
    const char* crops[4] = {"crop_l", "crop_t", "crop_r", "crop_b"};
    for (int i = 0; i < 4; i++) fx.crop[i] = std::clamp(f.value(crops[i], 0), 0, 45) / 100.f;
    const int b = f.value("brightness", 0), c = f.value("contrast", 0), s = f.value("saturation", 0), g = f.value("gamma", 0), h = f.value("hue", 0);
    fx.color = b || c || s || g || h;
    fx.brightness = b / 200.f;
    fx.contrast = c < 0 ? 1 + c / 100.f : 1 + c / 50.f;
    fx.saturation = 1 + s / 100.f;
    fx.gamma = powf(2.f, -g / 70.f);
    fx.hue = h * 3.14159265f / 180;
    fx.sharpen = std::clamp(f.value("sharpen", 0), 0, 100) / 100.f * 0.6f;
    const std::string mask = f.value("mask", std::string("none"));
    fx.mask = mask == "rounded" ? 1 : mask == "circle" ? 2 : 0;
    fx.radius = float(f.value("radius", 24));
    fx.key = f.value("chroma", false);
    if (fx.key) {
        const uint32_t k = parse_color(f.value("chroma_color", std::string("#00ff00")));
        fx.key_rgb[0] = ((k >> 16) & 255) / 255.f; fx.key_rgb[1] = ((k >> 8) & 255) / 255.f; fx.key_rgb[2] = (k & 255) / 255.f;
        fx.channel = fx.key_rgb[1] >= fx.key_rgb[0] && fx.key_rgb[1] >= fx.key_rgb[2] ? 1 : fx.key_rgb[2] >= fx.key_rgb[0] ? 2 : 0;
        fx.similarity = std::clamp(f.value("similarity", 40), 1, 100) / 100.f;
        fx.smoothness = std::clamp(f.value("smoothness", 8), 1, 100) / 100.f;
        fx.spill = std::clamp(f.value("spill", 50), 0, 100) / 100.f;
    }
    fx.active = fx.crop[0] || fx.crop[1] || fx.crop[2] || fx.crop[3] || fx.color || fx.sharpen > 0 || fx.mask || fx.key;
    return fx;
}

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

// Ekran kartında çizim yardımcıları
struct Gfx {
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> raster;

    bool init(ID3D11Device* d) {
        device = d;
        d->GetImmediateContext(&ctx);
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)))
            return false;
        d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        D3D11_BUFFER_DESC bd{sizeof(Quad), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
        d->CreateBuffer(&bd, nullptr, &cb);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d->CreateSamplerState(&sd, &sampler);
        D3D11_BLEND_DESC bl{};
        auto& rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        d->CreateBlendState(&bl, &blend);
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;   // Ayna görüntü için ters çevrilen kutu atılmasın
        d->CreateRasterizerState(&rd, &raster);
        return vs && ps && cb && sampler && blend && raster;
    }
    void begin(ID3D11RenderTargetView* rtv, int W, int H, bool transparent = false) {
        D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ID3D11Buffer* b = cb.Get();
        ctx->VSSetConstantBuffers(0, 1, &b);
        ctx->PSSetConstantBuffers(0, 1, &b);
        ID3D11SamplerState* s = sampler.Get();
        ctx->PSSetSamplers(0, 1, &s);
        ctx->OMSetBlendState(blend.Get(), nullptr, 0xffffffff);
        fx = Fx{};
        const float black[4] = {0, 0, 0, 1}, clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv, transparent ? clear : black);
    }
    Fx fx;      // Çizilen kaynağın filtreleri; kaynak dışındaki çizimlerde boş

    // uv: dokunun alınan bölgesi (x, y, w, h). overlay: imleç gibi ek çizim, kırpılmaz ama görünen kutunun dışına taşmaz
    void quad(ID3D11ShaderResourceView* srv, Rect r, int W, int H, float opacity, bool opaque,
              const float* uv = nullptr, bool overlay = false) {
        Quad q{};
        float u[4] = {0, 0, 1, 1};
        if (uv) memcpy(u, uv, sizeof(u));
        if (fx.active && !overlay) {
            const float l = fx.crop[0], t = fx.crop[1], rr = fx.crop[2], b = fx.crop[3];
            r = {r.x + r.w * l, r.y + r.h * t, r.w * std::max(0.f, 1 - l - rr), r.h * std::max(0.f, 1 - t - b)};
            u[0] += u[2] * l; u[1] += u[3] * t; u[2] *= std::max(0.f, 1 - l - rr); u[3] *= std::max(0.f, 1 - t - b);
            fx.clip[0] = std::min(r.x, r.x + r.w); fx.clip[1] = std::min(r.y, r.y + r.h);
            fx.clip[2] = std::max(r.x, r.x + r.w); fx.clip[3] = std::max(r.y, r.y + r.h);
            fx.clip_on = true;
        }
        q.rect[0] = r.x / W; q.rect[1] = r.y / H; q.rect[2] = r.w / W; q.rect[3] = r.h / H;
        q.opacity = opacity; q.opaque = opaque ? 1.f : 0.f;
        memcpy(q.uvr, u, sizeof(u));
        if (fx.active) {
            memcpy(q.clip, fx.clip, sizeof(q.clip));
            q.flags[0] = fx.color ? 1.f : 0.f;
            q.flags[1] = fx.clip_on ? 1.f : 0.f;
            q.color[0] = fx.brightness; q.color[1] = fx.contrast; q.color[2] = fx.saturation; q.color[3] = fx.gamma;
            q.color2[0] = fx.hue; q.color2[1] = overlay ? 0 : fx.sharpen; q.color2[2] = float(fx.mask); q.color2[3] = fx.radius * H / 1080.f;
            if (fx.key && !overlay) {
                memcpy(q.key, fx.key_rgb, sizeof(fx.key_rgb));
                q.key[3] = 1;
                q.keyp[0] = fx.similarity; q.keyp[1] = fx.smoothness; q.keyp[2] = fx.spill; q.keyp[3] = float(fx.channel);
            }
            if (q.color2[1] > 0) {
                ComPtr<ID3D11Resource> res;
                ComPtr<ID3D11Texture2D> tex;
                srv->GetResource(&res);
                D3D11_TEXTURE2D_DESC td{};
                if (SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
                if (td.Width && td.Height) { q.texel[0] = 1.f / td.Width; q.texel[1] = 1.f / td.Height; }
                else q.color2[1] = 0;
            }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, &q, sizeof(q));
        ctx->Unmap(cb.Get(), 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(4, 0);
    }
    Tex make(const Bitmap& b) {
        Tex t;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = b.w; td.Height = b.h; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{b.px.data(), UINT(b.w * 4), 0};
        if (FAILED(device->CreateTexture2D(&td, &data, &t.tex))) return t;
        device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
        t.w = b.w; t.h = b.h;
        return t;
    }
};


// Bir kaynağın canlı hali; kamera, oynatıcı, ekran yakalama burada açılır
class Source {
public:
    virtual ~Source() = default;
    virtual void update(Gfx&, const json& cfg, const json& doc, int W, int H) = 0;   // Son kareyi al
    virtual void draw(Gfx&, const json& cfg, int W, int H, Rect& box) = 0;
    virtual bool pointer_at(const json&, int, int, float&, float&) { return false; }    // İmlecin ucu, tuval pikseli
    virtual ID3D11Texture2D* camera_frame(const json&, int, int, Rect&, int&, int&) { return nullptr; }   // Kameranın ham karesi ve tuvaldeki yeri
};

Rect fixed_box(const json& cfg, int W, int H, int w, int h) {
    return {cfg.value("x", 0.f) * W, cfg.value("y", 0.f) * H, float(w), float(h)};
}

Rect free_box(const json& cfg, int W, int H) {
    return {cfg.value("x", 0.f) * W, cfg.value("y", 0.f) * H, cfg.value("w", 1.f) * W, cfg.value("h", 1.f) * H};
}

std::atomic<long long> g_screen_frames{0};     // FPS göstergesi için: ekrana gelen kare sayısı
std::atomic<int> g_screen_sources{0};

// Başka iş parçacığının yazdığı paylaşımlı dokudan son kareyi alır; kare ekran kartından inmez
struct SharedFrame {
    HANDLE opened = nullptr;
    ComPtr<ID3D11Texture2D> shared;
    ComPtr<IDXGIKeyedMutex> keyed;
    Tex image;

    void reopen(Gfx& g, HANDLE handle) {
        if (handle == opened) return;
        opened = handle;
        shared.Reset(); keyed.Reset();
        if (!handle || FAILED(g.device->OpenSharedResource(handle, IID_PPV_ARGS(&shared)))) return;
        shared.As(&keyed);
        D3D11_TEXTURE2D_DESC td;
        shared->GetDesc(&td);
        td.MiscFlags = 0;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        image = Tex{};
        g.device->CreateTexture2D(&td, nullptr, &image.tex);
        g.device->CreateShaderResourceView(image.tex.Get(), nullptr, &image.srv);
        image.w = int(td.Width); image.h = int(td.Height);
    }
    bool pull(Gfx& g) {        // Meşgulse beklemez, false döner ve sonraki turda yine denenir
        if (!keyed) return true;
        if (keyed->AcquireSync(0, 0) != S_OK) return false;
        g.ctx->CopyResource(image.tex.Get(), shared.Get());
        keyed->ReleaseSync(0);
        return true;
    }
};

// Ekran yakalama (DXGI masaüstü çoğaltma); kare ekran kartından hiç inmeden kullanılır.
// Yeni kareyi beklemek bazen bir ekran tazelemesi kadar sürer. Çizim döngüsü bunu beklemesin diye
// yakalama kendi iş parçacığında, kendi cihazında döner; son kare paylaşımlı dokudan alınır.
// Bir program aynı ekranı tek kez çoğaltabilir: aynı ekranı kullanan kaynaklar (tam ekran, bölge) tek yakalayıcıyı paylaşır.
class ScreenCapturer {
public:
    struct State {
        HANDLE handle = nullptr;
        long long frame = 0, shape_version = 0;
        bool visible = false;
        int x = 0, y = 0, hot_x = 0, hot_y = 0;
    };

    ScreenCapturer(IDXGIAdapter1* adapter, int monitor) : adapter_(adapter), monitor_(monitor) {
        g_screen_sources++;
        thread_ = std::thread(&ScreenCapturer::capture_loop, this);
    }
    ~ScreenCapturer() {
        halt_ = true;
        thread_.join();
        g_screen_sources--;
    }
    State state() {
        std::lock_guard<std::mutex> l(lock_);
        return state_;
    }
    Bitmap shape() {
        std::lock_guard<std::mutex> l(lock_);
        return shape_;
    }

private:
    bool open() {
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(adapter_->EnumOutputs(monitor_, &output)) && FAILED(adapter_->EnumOutputs(0, &output))) return false;
        if (FAILED(output.As(&output1)) || FAILED(output1->DuplicateOutput(device_.Get(), &dup_))) return false;
        DXGI_OUTDUPL_DESC d;
        dup_->GetDesc(&d);
        D3D11_TEXTURE2D_DESC td{};
        if (latest_) latest_->GetDesc(&td);
        if (!latest_ || td.Width != d.ModeDesc.Width || td.Height != d.ModeDesc.Height) {
            td = {};
            td.Width = d.ModeDesc.Width; td.Height = d.ModeDesc.Height; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            latest_.Reset(); latest_keyed_.Reset();
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &latest_))) return false;
            latest_.As(&latest_keyed_);
            ComPtr<IDXGIResource> res;
            HANDLE handle = nullptr;
            latest_.As(&res);
            res->GetSharedHandle(&handle);
            std::lock_guard<std::mutex> l(lock_);
            state_.handle = handle;
        }
        return true;
    }

    void capture_loop() {
        if (FAILED(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &ctx_)))
            return;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        while (!halt_) {
            if (!dup_ && !open()) { Sleep(500); continue; }
            DXGI_OUTDUPL_FRAME_INFO info;
            ComPtr<IDXGIResource> res;
            const HRESULT hr = dup_->AcquireNextFrame(50, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) {   // Çözünürlük değişti, UAC ekranı açıldı...: biraz sonra yeniden aç
                dup_.Reset();
                Sleep(500);
                continue;
            }
            if (info.LastPresentTime.QuadPart) g_screen_frames += info.AccumulatedFrames;   // Sadece fare hareketiyse sayma
            if (info.LastPresentTime.QuadPart && latest_keyed_->AcquireSync(0, 10) == S_OK) {
                ComPtr<ID3D11Texture2D> tex;
                res.As(&tex);
                ctx_->CopyResource(latest_.Get(), tex.Get());
                latest_keyed_->ReleaseSync(0);
                std::lock_guard<std::mutex> l(lock_);
                state_.frame++;
            }
            if (info.LastMouseUpdateTime.QuadPart) {
                std::lock_guard<std::mutex> l(lock_);
                state_.visible = info.PointerPosition.Visible != 0;
                state_.x = info.PointerPosition.Position.x;
                state_.y = info.PointerPosition.Position.y;
            }
            if (info.PointerShapeBufferSize) read_pointer(info.PointerShapeBufferSize);
            dup_->ReleaseFrame();
        }
    }

    void read_pointer(UINT size) {
        std::vector<uint8_t> buffer(size);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
        UINT got = 0;
        if (FAILED(dup_->GetFramePointerShape(size, buffer.data(), &got, &si))) return;
        const bool mono = si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        Bitmap b;
        b.w = int(si.Width); b.h = int(mono ? si.Height / 2 : si.Height);
        b.px.resize(size_t(b.w) * b.h * 4);
        for (int y = 0; y < b.h; y++)
            for (int x = 0; x < b.w; x++) {
                uint8_t* p = &b.px[(size_t(y) * b.w + x) * 4];
                if (mono) {
                    const uint8_t mask = 0x80 >> (x % 8);
                    const bool a = buffer[y * si.Pitch + x / 8] & mask;
                    const bool c = buffer[(y + b.h) * si.Pitch + x / 8] & mask;
                    if (a && !c) continue;                          // Saydam
                    const uint8_t v = c ? 255 : 0;                  // Ters renkli imleç beyaz çizilir
                    p[0] = p[1] = p[2] = v; p[3] = 255;
                    continue;
                }
                const uint8_t* s = &buffer[y * si.Pitch + x * 4];
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                    const int a = s[3];
                    p[0] = uint8_t(s[0] * a / 255); p[1] = uint8_t(s[1] * a / 255);
                    p[2] = uint8_t(s[2] * a / 255); p[3] = uint8_t(a);
                } else if (s[3] == 0 || s[0] || s[1] || s[2]) {     // Maskeli renkli imleç
                    p[0] = s[0]; p[1] = s[1]; p[2] = s[2]; p[3] = 255;
                }
            }
        std::lock_guard<std::mutex> l(lock_);
        shape_ = std::move(b);
        state_.shape_version++;
        state_.hot_x = int(si.HotSpot.x); state_.hot_y = int(si.HotSpot.y);   // İmlecin tıklayan ucu
    }

    ComPtr<IDXGIAdapter1> adapter_;
    int monitor_;
    std::thread thread_;
    std::atomic<bool> halt_{false};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> latest_;
    ComPtr<IDXGIKeyedMutex> latest_keyed_;
    std::mutex lock_;
    State state_;
    Bitmap shape_;
};

std::shared_ptr<ScreenCapturer> screen_capturer(IDXGIAdapter1* adapter, int monitor) {
    static std::mutex lock;
    static std::map<int, std::weak_ptr<ScreenCapturer>> open;
    std::lock_guard<std::mutex> l(lock);
    auto c = open[monitor].lock();
    if (!c) {
        c = std::make_shared<ScreenCapturer>(adapter, monitor);
        open[monitor] = c;
    }
    return c;
}

// Tam ekran ya da ekranın bir bölgesi (settings.region: ekran pikseli x, y, w, h)
class ScreenSource : public Source {
public:
    ScreenSource(IDXGIAdapter1* adapter, int monitor, bool region) : capturer_(screen_capturer(adapter, monitor)), region_(region) {}

    void update(Gfx& g, const json&, const json&, int, int) override {
        const ScreenCapturer::State s = capturer_->state();
        pointer_ = s;
        if (s.shape_version != shape_seen_) {
            shape_seen_ = s.shape_version;
            const Bitmap b = capturer_->shape();
            pointer_tex_ = b.empty() ? Tex{} : g.make(b);
        }
        if (s.handle != frame_.opened) seen_ = -1;
        frame_.reopen(g, s.handle);
        if (s.frame != seen_ && frame_.pull(g)) seen_ = s.frame;
    }

    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        const Tex& screen = frame_.image;
        if (!screen.srv || !screen.w || !screen.h) return;
        const Rect area = region(cfg, screen);
        const float uv[4] = {area.x / screen.w, area.y / screen.h, area.w / screen.w, area.h / screen.h};
        const float opacity = cfg.value("opacity", 100) / 100.f;
        g.quad(screen.srv.Get(), box, W, H, opacity, true, uv);
        if (cfg["settings"].value("cursor", true) && pointer_.visible && pointer_tex_.srv) {
            const float sx = box.w / area.w, sy = box.h / area.h;
            Rect c{box.x + (pointer_.x - area.x) * sx, box.y + (pointer_.y - area.y) * sy, pointer_tex_.w * sx, pointer_tex_.h * sy};
            if (!g.fx.clip_on) {    // Bölgenin dışına taşan imleç kesilsin
                g.fx.active = true;
                g.fx.clip_on = true;
                g.fx.clip[0] = box.x; g.fx.clip[1] = box.y; g.fx.clip[2] = box.x + box.w; g.fx.clip[3] = box.y + box.h;
            }
            g.quad(pointer_tex_.srv.Get(), c, W, H, opacity, false, nullptr, true);
        }
    }

    bool pointer_at(const json& cfg, int W, int H, float& x, float& y) override {
        const Tex& screen = frame_.image;
        if (!cfg.value("visible", true) || !pointer_.visible || !screen.srv || screen.w <= 0 || screen.h <= 0) return false;
        const Rect box = free_box(cfg, W, H), area = region(cfg, screen);
        const float px = float(pointer_.x + pointer_.hot_x), py = float(pointer_.y + pointer_.hot_y);
        if (px < area.x || py < area.y || px > area.x + area.w || py > area.y + area.h) return false;
        x = box.x + (px - area.x) * box.w / area.w;
        y = box.y + (py - area.y) * box.h / area.h;
        return true;
    }

private:
    Rect region(const json& cfg, const Tex& screen) const {
        Rect full{0, 0, float(screen.w), float(screen.h)};
        if (!region_) return full;
        const json r = cfg["settings"].value("region", json::object());
        Rect a{float(r.value("x", 0)), float(r.value("y", 0)), float(r.value("w", 0)), float(r.value("h", 0))};
        a.x = std::clamp(a.x, 0.f, full.w - 16); a.y = std::clamp(a.y, 0.f, full.h - 16);
        if (a.w < 16 || a.h < 16) return full;
        a.w = std::min(a.w, full.w - a.x); a.h = std::min(a.h, full.h - a.y);
        return a;
    }

    std::shared_ptr<ScreenCapturer> capturer_;
    bool region_;
    SharedFrame frame_;
    long long seen_ = -1, shape_seen_ = -1;
    ScreenCapturer::State pointer_;
    Tex pointer_tex_;
};

// Oyun yakalama: oyunun penceresi doğrudan alınır, üstteki başka pencereler ve göstergeler girmez.
// Özel tam ekran oyun görüntü vermezse kendiliğinden ekran yakalamaya geçer.
std::atomic<bool> g_game_fallback{false};

class GameSource : public Source {
public:
    GameSource(IDXGIAdapter1* adapter, const std::string& exe, bool cursor) : adapter_(adapter), capture_(adapter, exe, cursor) {}

    ~GameSource() override { g_game_fallback = false; }

    void update(Gfx& g, const json& cfg, const json& doc, int W, int H) override {
        g_game_fallback = capture_.stalled();
        if (capture_.stalled() && !screen_) screen_ = std::make_unique<ScreenSource>(adapter_.Get(), 0, false);
        if (!capture_.stalled() && screen_) screen_.reset();
        if (screen_) { screen_->update(g, cfg, doc, W, H); return; }
        frame_.reopen(g, capture_.handle());
        if (capture_.take_fresh() && !frame_.pull(g)) capture_.mark_fresh();
    }

    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        if (screen_) { screen_->draw(g, cfg, W, H, box); return; }
        const Tex& t = frame_.image;
        if (!capture_.has_target() || !t.srv || !t.w || !t.h) return;
        // Oyunun oranı bozulmasın: kutunun içine sığdır
        const float k = std::min(box.w / t.w, box.h / t.h);
        const Rect fit{box.x + (box.w - t.w * k) / 2, box.y + (box.h - t.h * k) / 2, t.w * k, t.h * k};
        g.quad(t.srv.Get(), fit, W, H, cfg.value("opacity", 100) / 100.f, true);
    }

private:
    ComPtr<IDXGIAdapter1> adapter_;
    WindowCapture capture_;
    SharedFrame frame_;
    std::unique_ptr<ScreenSource> screen_;
};

// Pencere yakalama: seçilen programın penceresi, öndeki başka pencereler girmez. Kare yalnızca pencere değişince gelir.
class WindowSource : public Source {
public:
    WindowSource(IDXGIAdapter1* adapter, const json& s)
        : capture_(adapter, s.value("window", std::string()), s.value("cursor", true), s.value("title", std::string()), s.value("client", true), false) {}

    void update(Gfx& g, const json&, const json&, int, int) override {
        frame_.reopen(g, capture_.handle());
        if (capture_.take_fresh() && !frame_.pull(g)) capture_.mark_fresh();
    }

    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        const Tex& t = frame_.image;
        if (cfg["settings"].value("window", std::string()).empty()) return;     // Henüz pencere seçilmedi
        if (!capture_.has_target() || !t.srv || !t.w || !t.h) return;
        const float k = std::min(box.w / t.w, box.h / t.h);     // Oran korunur
        const Rect fit{box.x + (box.w - t.w * k) / 2, box.y + (box.h - t.h * k) / 2, t.w * k, t.h * k};
        g.quad(t.srv.Get(), fit, W, H, cfg.value("opacity", 100) / 100.f, true);
    }

private:
    WindowCapture capture_;
    SharedFrame frame_;
};

// FPS göstergesi: Oyun Araçları'ndaki göstergenin sahneye girmiş hali, kayıtta ve yayında görünür.
// Bir program aynı ekranı tek kez çoğaltabilir: bu yüzden kareler çalışan yakalamadan sayılır,
// ayrı ölçer sadece sahnede yakalama yoksa açılır. Ölçüm ve çizim kendi iş parçacığındadır, sahne çizimi beklemez.
class FpsSource : public Source {
public:
    FpsSource() { thread_ = std::thread(&FpsSource::loop, this); }
    ~FpsSource() override {
        halt_ = true;
        thread_.join();
    }

    void update(Gfx& g, const json& cfg, const json&, int, int H) override {
        Bitmap b;
        bool fresh;
        {
            std::lock_guard<std::mutex> l(lock_);
            if (settings_ != cfg["settings"]) settings_ = cfg["settings"];
            height_ = H;
            fresh = fresh_;
            if (fresh) { b = std::move(bitmap_); fresh_ = false; }
        }
        if (fresh) tex_ = b.empty() ? Tex{} : g.make(b);
    }

    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = fixed_box(cfg, W, H, tex_.w, tex_.h);
        if (tex_.srv) g.quad(tex_.srv.Get(), box, W, H, cfg.value("opacity", 100) / 100.f, false);
    }

private:
    void loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Sensors sensors;
            FpsMeter meter;
            sensors.start();
            enum Mode { kNone, kCapture, kScreen, kMeter } mode = kNone;
            long long last_count = 0;
            double last_time = now(), next_read = 0, value = -1;
            int hz = refresh_rate();
            std::string shown;
            int shown_height = 0;
            while (!halt_) {
                Sleep(100);
                json s;
                int H;
                {
                    std::lock_guard<std::mutex> l(lock_);
                    s = settings_;
                    H = height_;
                }
                if (!s.is_object() || !H) continue;
                const Mode wanted = g_capture_active ? kCapture : g_screen_sources > 0 ? kScreen : kMeter;
                const auto count = [&] { return mode == kCapture ? g_capture_frames.load() : g_screen_frames.load(); };
                if (wanted != mode) {
                    if (mode == kMeter) meter.stop();       // Ekran kaynağı çoğaltmayı alabilsin
                    mode = wanted;
                    if (mode == kMeter) meter.start();
                    last_count = count();
                    last_time = now();
                    next_read = last_time + 0.5;
                }
                sensors.configure(fps_wanted(s), s.value("sensor_interval", 1000));
                const double t = now();
                if (t >= next_read) {
                    next_read = t + std::max(s.value("interval", 500), 100) / 1000.0;
                    if (mode == kMeter) {
                        value = meter.read();
                    } else {
                        const long long c = count();
                        value = t > last_time ? (c - last_count) / (t - last_time) : 0;
                        last_count = c;
                        last_time = t;
                    }
                    hz = refresh_rate();
                }
                const auto rows = fps_rows(s, value, hz, sensors.data());
                const std::string sig = fps_signature(rows) + s.dump();
                if (sig == shown && H == shown_height) continue;
                shown = sig;
                shown_height = H;
                Bitmap b = draw_fps(s, rows, s.value("scale_with_resolution", true) ? H / 1080.f : 1.f);
                std::lock_guard<std::mutex> l(lock_);
                bitmap_ = std::move(b);
                fresh_ = true;
            }
            meter.stop();
        }
        CoUninitialize();
    }

    std::thread thread_;
    std::atomic<bool> halt_{false};
    std::mutex lock_;
    json settings_;
    int height_ = 0;
    Bitmap bitmap_;
    bool fresh_ = false;
    Tex tex_;
};

// İçeriği ayar değişince üretilen kaynaklar: resim, metin, düz renk, sosyal medya, crosshair
class StillSource : public Source {
public:
    void update(Gfx& g, const json& cfg, const json& doc, int, int H) override {
        const std::string type = cfg.value("type", std::string());
        const json& s = cfg["settings"];
        const float k = H / 1080.f;
        std::string key = s.dump() + std::to_string(H);
        if (type == "social") key = doc["social"].dump() + std::to_string(H);
        if (key == key_) return;
        key_ = key;
        Bitmap b;
        if (type == "image") {
            load_image(widen(s.value("file", std::string())), b);
        } else if (type == "text") {
            b = draw_text(s, k);
        } else if (type == "social") {
            b = draw_social(doc["social"], k);
        } else if (type == "color") {
            const uint32_t c = parse_color(s.value("color", std::string("#1e1e2e")));
            b.w = b.h = 1;
            b.px = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), 255};
        }
        tex_ = b.empty() ? Tex{} : g.make(b);
    }
    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        const std::string type = cfg.value("type", std::string());
        const bool free = type == "image" || type == "color";
        box = free ? free_box(cfg, W, H) : fixed_box(cfg, W, H, tex_.w, tex_.h);
        if (tex_.srv) g.quad(tex_.srv.Get(), box, W, H, cfg.value("opacity", 100) / 100.f, false);
    }

private:
    std::string key_;
    Tex tex_;
};

std::atomic<bool> g_camera_failed{false};   // Kameranın hiçbir biçimi görüntü vermedi

// Video dosyası ya da kamera: ayrı iş parçacığında çözülür, son kare ekran kartına yüklenir
class MediaSource : public Source {
public:
    // files: oynatma listesi (birden çoksa sırayla, sonuncudan sonra döngü açıksa başa döner)
    MediaSource(std::vector<std::wstring> files, int camera, bool loop, bool shuffle = false)
        : files_(std::move(files)), camera_(camera), loop_(loop), shuffle_(shuffle) {
        thread_ = std::thread(&MediaSource::run, this);
    }
    ~MediaSource() override {
        halt_ = true;
        thread_.join();
    }
    void update(Gfx& g, const json& cfg, const json&, int, int) override {
        std::lock_guard<std::mutex> l(lock_);
        loop_ = cfg["settings"].value("loop", true);
        if (!fresh_ || frame_.empty()) return;
        fresh_ = false;
        if (tex_.w != frame_.w || tex_.h != frame_.h) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = frame_.w; td.Height = frame_.h; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            tex_ = Tex{};
            g.device->CreateTexture2D(&td, nullptr, &tex_.tex);
            g.device->CreateShaderResourceView(tex_.tex.Get(), nullptr, &tex_.srv);
            tex_.w = frame_.w; tex_.h = frame_.h;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g.ctx->Map(tex_.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < frame_.h; y++)
                memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, &frame_.px[size_t(y) * frame_.w * 4],
                       size_t(frame_.w) * 4);
            g.ctx->Unmap(tex_.tex.Get(), 0);
        }
    }
    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        if (!tex_.srv) return;
        Rect area = box;
        if (camera_ >= 0 && tex_.w && tex_.h) {     // Kamera görüntüsü kutuda basılmasın, oranı korunsun
            const float k = std::min(box.w / tex_.w, box.h / tex_.h);
            area.w = tex_.w * k; area.h = tex_.h * k;
            area.x = box.x + (box.w - area.w) / 2;
            area.y = box.y + (box.h - area.h) / 2;
        }
        if (cfg["settings"].value("mirror", false)) {   // Ayna: kutuyu ters çevir
            area.x += area.w; area.w = -area.w;
        }
        g.quad(tex_.srv.Get(), area, W, H, cfg.value("opacity", 100) / 100.f, true);
    }

    ID3D11Texture2D* camera_frame(const json& cfg, int W, int H, Rect& area, int& w, int& h) override {
        if (camera_ < 0 || !tex_.tex || !tex_.w || !tex_.h) return nullptr;
        const Rect box = free_box(cfg, W, H);
        const float k = std::min(box.w / tex_.w, box.h / tex_.h);
        area = {box.x + (box.w - tex_.w * k) / 2, box.y + (box.h - tex_.h * k) / 2, tex_.w * k, tex_.h * k};
        w = tex_.w; h = tex_.h;
        return tex_.tex.Get();
    }

private:
    struct CameraMode {
        GUID subtype;
        UINT32 w, h;
        DWORD index;        // Kameranın kendi biçim listesindeki sırası
    };

    // Seçili kamerayı açar. Her deneme için yeniden açılır: başarısız akış okuyucuyu kullanılmaz bırakır
    bool open_camera(ComPtr<IMFMediaSource>& src, ComPtr<IMFSourceReader>& reader) {
        ComPtr<IMFAttributes> query, attrs;
        MFCreateAttributes(&query, 1);
        query->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        MFCreateAttributes(&attrs, 1);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (FAILED(MFEnumDeviceSources(query.Get(), &devices, &count))) return false;
        if (UINT32(camera_) < count && SUCCEEDED(devices[camera_]->ActivateObject(IID_PPV_ARGS(&src))))
            MFCreateSourceReaderFromMediaSource(src.Get(), attrs.Get(), &reader);
        for (UINT32 i = 0; i < count; i++) devices[i]->Release();
        CoTaskMemFree(devices);
        return reader != nullptr;
    }

    // Sürücülerin listelediği her biçim gerçekten çalışmaz: en iyi görüntüden başlayarak kare veren bulunur
    std::vector<CameraMode> camera_modes() {
        std::vector<CameraMode> modes;
        ComPtr<IMFMediaSource> src;
        ComPtr<IMFSourceReader> reader;
        if (!open_camera(src, reader)) return modes;
        for (DWORD i = 0; i < 300; i++) {
            ComPtr<IMFMediaType> type;
            if (FAILED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &type))) break;
            CameraMode m{GUID_NULL, 0, 0, i};
            UINT32 fps_n = 0, fps_d = 1;
            type->GetGUID(MF_MT_SUBTYPE, &m.subtype);
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &m.w, &m.h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fps_n, &fps_d);
            if (m.w == 0 || m.w > 1920 || (fps_d && fps_n / fps_d < 15)) continue;
            bool known = false;
            for (auto& o : modes) known |= o.subtype == m.subtype && o.w == m.w && o.h == m.h;
            if (!known) modes.push_back(m);
        }
        auto rank = [](const GUID& g) { return g == MFVideoFormat_YUY2 ? 0 : g == MFVideoFormat_NV12 ? 1 : 2; };
        std::stable_sort(modes.begin(), modes.end(), [&](const CameraMode& a, const CameraMode& b) {
            if (a.w * a.h != b.w * b.h) return a.w * a.h > b.w * b.h;
            return rank(a.subtype) < rank(b.subtype);
        });
        src->Shutdown();
        return modes;
    }

    void run_camera() {
        for (const CameraMode& mode : camera_modes()) {
            if (halt_) break;
            ComPtr<IMFMediaSource> src;
            ComPtr<IMFSourceReader> reader;
            ComPtr<IMFMediaType> type;
            if (!open_camera(src, reader)) break;
            bool shown = false;
            if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, mode.index, &type))) {
                // YUY2 ve NV12 burada çevrilir; sıkıştırılmış biçimleri Windows RGB'ye açar
                pixel_format_ = mode.subtype == MFVideoFormat_YUY2 ? 1 : mode.subtype == MFVideoFormat_NV12 ? 2 : 0;
                if (!pixel_format_) type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                if (SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())))
                    shown = read_loop(reader.Get());
            }
            reader.Reset();
            src->Shutdown();
            if (shown) return;
            Sleep(200);
        }
        if (!halt_) g_camera_failed = true;
    }

    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (camera_ >= 0) {
            run_camera();
        } else {
            multi_ = files_.size() > 1;
            std::vector<size_t> order(files_.size());
            for (size_t i = 0; i < order.size(); i++) order[i] = i;
            unsigned seed = GetTickCount();
            size_t at = 0;
            while (!halt_ && !files_.empty()) {
                if (at == 0 && shuffle_ && order.size() > 1)     // Karışık sıra: her turda yeniden
                    for (size_t i = order.size() - 1; i > 0; i--) { seed = seed * 1103515245 + 12345; std::swap(order[i], order[(seed >> 8) % (i + 1)]); }
                ComPtr<IMFSourceReader> reader;
                ComPtr<IMFAttributes> attrs;
                MFCreateAttributes(&attrs, 2);
                attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
                MFCreateSourceReaderFromURL(files_[order[at]].c_str(), attrs.Get(), &reader);
                ComPtr<IMFMediaType> type;
                MFCreateMediaType(&type);
                type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                if (reader && SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type.Get())))
                    read_loop(reader.Get());
                else Sleep(200);    // Açılamayan dosya atlanır
                if (!multi_) break;
                bool loop;
                { std::lock_guard<std::mutex> l(lock_); loop = loop_; }
                if (at + 1 < files_.size()) at++;
                else if (loop) at = 0;
                else { while (!halt_) Sleep(50); }      // Liste bitti: son kare kalır
            }
        }
        CoUninitialize();
    }

    // YUV -> BGRA. Kameraların çoğu tam aralık (0-255) verir; 16 altı siyaha düşerse loş görüntü kaybolur
    void put_pixel(uint8_t* out, int y, int u, int v) const {
        const int d = u - 128, e = v - 128;
        int r, g, b;
        if (full_range_) {
            r = y + ((359 * e + 128) >> 8);
            g = y - ((88 * d + 183 * e + 128) >> 8);
            b = y + ((454 * d + 128) >> 8);
        } else {
            const int c = (y - 16) * 298;
            r = (c + 409 * e + 128) >> 8;
            g = (c - 100 * d - 208 * e + 128) >> 8;
            b = (c + 516 * d + 128) >> 8;
        }
        out[0] = uint8_t(std::clamp(b, 0, 255));
        out[1] = uint8_t(std::clamp(g, 0, 255));
        out[2] = uint8_t(std::clamp(r, 0, 255));
        out[3] = 255;
    }

    void convert_yuy2(const BYTE* data, DWORD length, UINT32 w, UINT32 h, UINT32 stride) {
        for (UINT32 y = 0; y < h && size_t(y + 1) * stride <= length; y++) {
            const BYTE* row = data + size_t(y) * stride;
            uint8_t* out = &frame_.px[size_t(y) * w * 4];
            for (UINT32 x = 0; x + 1 < w; x += 2) {
                put_pixel(out + size_t(x) * 4, row[x * 2], row[x * 2 + 1], row[x * 2 + 3]);
                put_pixel(out + size_t(x) * 4 + 4, row[x * 2 + 2], row[x * 2 + 1], row[x * 2 + 3]);
            }
        }
    }

    void convert_nv12(const BYTE* data, DWORD length, UINT32 w, UINT32 h, UINT32 stride) {
        const size_t chroma = size_t(stride) * h;
        if (chroma + size_t(stride) * (h / 2) > length) return;
        for (UINT32 y = 0; y < h; y++) {
            const BYTE* luma = data + size_t(y) * stride;
            const BYTE* uv = data + chroma + size_t(y / 2) * stride;
            uint8_t* out = &frame_.px[size_t(y) * w * 4];
            for (UINT32 x = 0; x < w; x++) put_pixel(out + size_t(x) * 4, luma[x], uv[x & ~1u], uv[(x & ~1u) + 1]);
        }
    }

    // En az bir kare gösterildiyse true döner
    bool read_loop(IMFSourceReader* reader) {
        ComPtr<IMFMediaType> current;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        LONG stride = 0;
        current->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride));
        // Sürücü sınırlı aralık dese bile veride 16 altı görülürse tam aralıktır
        full_range_ = MFGetAttributeUINT32(current.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255) != MFNominalRange_16_235;
        double base = now();
        bool shown = false;
        while (!halt_) {
            DWORD flags = 0;
            LONGLONG stamp = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &stamp, &sample))) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (multi_) return shown;       // Listede sonraki dosyaya geçilir
                bool loop;
                { std::lock_guard<std::mutex> l(lock_); loop = loop_; }
                if (!loop) { Sleep(50); continue; }
                PROPVARIANT start;
                InitPropVariantFromInt64(0, &start);
                reader->SetCurrentPosition(GUID_NULL, start);
                base = now();
                continue;
            }
            if (!sample) continue;
            if (camera_ < 0) {   // Dosya: zamanı gelene kadar bekle
                const double due = base + stamp / 1e7;
                while (!halt_ && now() < due) Sleep(1);
            }
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
            {
                std::lock_guard<std::mutex> l(lock_);
                frame_.w = int(w); frame_.h = int(h);
                frame_.px.resize(size_t(w) * h * 4);
                if (pixel_format_ && !full_range_) {
                    const DWORD step = pixel_format_ == 1 ? 2 : 1, limit = std::min<DWORD>(length, w * h);
                    for (DWORD i = 0; i < limit && !full_range_; i += step * 31) full_range_ = data[i] < 10;
                }
                if (pixel_format_ == 1) {
                    convert_yuy2(data, length, w, h, stride > 0 ? UINT32(stride) : w * 2);
                } else if (pixel_format_ == 2) {
                    convert_nv12(data, length, w, h, stride > 0 ? UINT32(stride) : w);
                } else {
                    const int abs_stride = stride < 0 ? -stride : (stride ? stride : int(w) * 4);
                    for (UINT32 y = 0; y < h && (y + 1) * abs_stride <= length; y++) {
                        const UINT32 row = stride < 0 ? h - 1 - y : y;   // Ters satırlı biçim
                        memcpy(&frame_.px[size_t(y) * w * 4], data + size_t(row) * abs_stride, size_t(w) * 4);
                    }
                }
                fresh_ = true;
            }
            buffer->Unlock();
            shown = true;
        }
        return shown;
    }

    std::vector<std::wstring> files_;
    int camera_;
    int pixel_format_ = 0;      // 0 RGB32, 1 YUY2, 2 NV12
    bool full_range_ = true;
    bool loop_, shuffle_ = false, multi_ = false;
    std::thread thread_;
    std::atomic<bool> halt_{false};
    std::mutex lock_;
    Bitmap frame_;
    bool fresh_ = false;
    Tex tex_;
};

// Sanal kamera: Windows 11 kamerayı kaydeder (vcam.dll kamera hizmetinde çalışır); tuval NV12'ye çevrilip paylaşılan belleğe yazılır
const char* kNv12 = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VS { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VS o;
    o.pos = float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1);
    o.uv = c;
    return o;
}
float ps_y(VS i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    return dot(c, float3(0.2126, 0.7152, 0.0722)) * 219.0 / 255.0 + 16.0 / 255.0;
}
float2 ps_uv(VS i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    return float2(dot(c, float3(-0.1146, -0.3854, 0.5)), dot(c, float3(0.5, -0.4542, -0.0458))) * 224.0 / 255.0 + 128.0 / 255.0;
}
)";

bool vcam_installed() {
    HKEY k;
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVcamClsidText + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

class VirtualCamera {
public:
    ~VirtualCamera() { stop(); }
    bool start(std::string& error) {
        if (camera_) return true;
        if (!vcam_installed()) { error = "Sanal kamera kurulu değil"; return false; }
        const HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                                                 kVcamName, kVcamClsidText, nullptr, 0, &camera_);
        if (FAILED(hr) || FAILED(camera_->Start(nullptr))) {
            camera_.Reset();
            error = "Sanal kamera başlatılamadı (Windows 11 gerekir)";
            return false;
        }
        return true;
    }
    void stop() {
        if (camera_) { camera_->Stop(); camera_->Remove(); camera_->Shutdown(); camera_.Reset(); }
        for (auto* v : {&global_, &own_}) {
            if (v->view) UnmapViewOfFile(v->view);
            if (v->mapping) CloseHandle(v->mapping);
            *v = View{};
        }
    }
    bool running() const { return camera_ != nullptr; }
    // Kamera hizmeti belleği okuyorsa: Global bellek açıldıysa ya da kendi belleğimizden kare alınıyorsa (okuma sayacı yok, açılmış olması yeter)
    bool in_use() const {
        for (const auto* v : {&own_, &global_})
            if (v->view && GetTickCount64() - uint64_t(reinterpret_cast<const VcamHeader*>(v->view)->read_tick) < 1500) return true;
        return false;
    }

    // Saniyede 30 kez: tuval küçültülüp NV12 yapılır, bir önceki kare (GPU beklemesin diye) belleğe yazılır
    void write(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* canvas) {
        if (!camera_ || now() < next_at_) return;
        next_at_ = std::max(next_at_ + 1.0 / kVcamFps, now() - 0.1);
        if (!own_.view) {      // Kendi oturumumuzda, kamera hizmetinin okuyabileceği izinlerle
            SECURITY_ATTRIBUTES sa{sizeof(sa)};
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)(A;;GA;;;S-1-15-2-2)(A;;GA;;;LS)(A;;GA;;;SY)S:(ML;;NW;;;LW)",
                                                                     SDDL_REVISION_1, &sd, nullptr))
                sa.lpSecurityDescriptor = sd;
            const size_t bytes = vcam_mapping_bytes();
            own_.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, DWORD(uint64_t(bytes) >> 32), DWORD(bytes), kVcamMapping + 7);
            if (sd) LocalFree(sd);
            if (own_.mapping) own_.view = static_cast<uint8_t*>(MapViewOfFile(own_.mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
            if (own_.view) {
                auto* h = reinterpret_cast<VcamHeader*>(own_.view);
                h->width = kVcamWidth; h->height = kVcamHeight; h->slot = 0; h->frame = 0; h->tick = 0; h->read_tick = 0; h->magic = kVcamMagic;
            }
        }
        if (!global_.view && now() >= next_open_) {    // Kamera hizmetinin oluşturduğu Global bellek (varsa)
            next_open_ = now() + 1;
            global_.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kVcamMapping);
            if (global_.mapping) global_.view = static_cast<uint8_t*>(MapViewOfFile(global_.mapping, FILE_MAP_ALL_ACCESS, 0, 0, vcam_mapping_bytes()));
            if (global_.mapping && !global_.view) { CloseHandle(global_.mapping); global_.mapping = nullptr; }
        }
        if ((!own_.view && !global_.view) || !prepare(device)) return;
        // Önceki kare hazırsa belleğe
        Slot& done = slots_[(index_ + 1) % 2];
        if (done.used) {
            D3D11_MAPPED_SUBRESOURCE y{}, uv{};
            if (SUCCEEDED(ctx->Map(done.y.Get(), 0, D3D11_MAP_READ, 0, &y))) {
                if (SUCCEEDED(ctx->Map(done.uv.Get(), 0, D3D11_MAP_READ, 0, &uv))) {
                    for (auto* v : {&own_, &global_}) {
                        if (!v->view) continue;
                        auto* header = reinterpret_cast<VcamHeader*>(v->view);
                        if (header->magic != kVcamMagic) continue;
                        const LONG target = 1 - (header->slot & 1);
                        uint8_t* out = v->view + kVcamHeaderBytes + size_t(target) * kVcamFrameBytes;
                        for (int r = 0; r < kVcamHeight; r++) memcpy(out + size_t(r) * kVcamWidth, static_cast<uint8_t*>(y.pData) + size_t(r) * y.RowPitch, kVcamWidth);
                        out += size_t(kVcamWidth) * kVcamHeight;
                        for (int r = 0; r < kVcamHeight / 2; r++) memcpy(out + size_t(r) * kVcamWidth, static_cast<uint8_t*>(uv.pData) + size_t(r) * uv.RowPitch, kVcamWidth);
                        MemoryBarrier();
                        InterlockedExchange(&header->slot, target);
                        InterlockedIncrement64(&header->frame);
                        header->tick = LONG64(GetTickCount64());
                    }
                    last_written_ = now();
                    ctx->Unmap(done.uv.Get(), 0);
                }
                ctx->Unmap(done.y.Get(), 0);
            }
            done.used = false;
        }
        // Bu kare: ekran kartında NV12
        ID3D11SamplerState* sampler = sampler_.Get();
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetSamplers(0, 1, &sampler);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        auto pass = [&](ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, int w, int h) {
            D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->RSSetViewports(1, &vp);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &canvas);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(0, 1, &none);
        };
        pass(y_rtv_.Get(), ps_y_.Get(), kVcamWidth, kVcamHeight);
        pass(uv_rtv_.Get(), ps_uv_.Get(), kVcamWidth / 2, kVcamHeight / 2);
        ID3D11RenderTargetView* no_rtv = nullptr;
        ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
        Slot& now_slot = slots_[index_];
        ctx->CopyResource(now_slot.y.Get(), y_.Get());
        ctx->CopyResource(now_slot.uv.Get(), uv_.Get());
        now_slot.used = true;
        index_ = (index_ + 1) % 2;
    }

private:
    struct Slot { ComPtr<ID3D11Texture2D> y, uv; bool used = false; };
    bool prepare(ID3D11Device* d) {
        if (ready_) return true;
        ComPtr<ID3DBlob> vs, y, uv, err;
        if (FAILED(D3DCompile(kNv12, strlen(kNv12), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
            FAILED(D3DCompile(kNv12, strlen(kNv12), nullptr, nullptr, nullptr, "ps_y", "ps_5_0", 0, 0, &y, &err)) ||
            FAILED(D3DCompile(kNv12, strlen(kNv12), nullptr, nullptr, nullptr, "ps_uv", "ps_5_0", 0, 0, &uv, &err)))
            return false;
        d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
        d->CreatePixelShader(y->GetBufferPointer(), y->GetBufferSize(), nullptr, &ps_y_);
        d->CreatePixelShader(uv->GetBufferPointer(), uv->GetBufferSize(), nullptr, &ps_uv_);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d->CreateSamplerState(&sd, &sampler_);
        D3D11_TEXTURE2D_DESC td{};
        td.MipLevels = td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        auto plane = [&](int w, int h, DXGI_FORMAT f, ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11RenderTargetView>& rtv) {
            td.Width = w; td.Height = h; td.Format = f; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
            return SUCCEEDED(d->CreateTexture2D(&td, nullptr, &t)) && SUCCEEDED(d->CreateRenderTargetView(t.Get(), nullptr, &rtv));
        };
        if (!plane(kVcamWidth, kVcamHeight, DXGI_FORMAT_R8_UNORM, y_, y_rtv_) || !plane(kVcamWidth / 2, kVcamHeight / 2, DXGI_FORMAT_R8G8_UNORM, uv_, uv_rtv_)) return false;
        for (auto& s : slots_) {
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.Width = kVcamWidth; td.Height = kVcamHeight; td.Format = DXGI_FORMAT_R8_UNORM;
            if (FAILED(d->CreateTexture2D(&td, nullptr, &s.y))) return false;
            td.Width = kVcamWidth / 2; td.Height = kVcamHeight / 2; td.Format = DXGI_FORMAT_R8G8_UNORM;
            if (FAILED(d->CreateTexture2D(&td, nullptr, &s.uv))) return false;
        }
        ready_ = true;
        return true;
    }

    ComPtr<IMFVirtualCamera> camera_;
    struct View { HANDLE mapping = nullptr; uint8_t* view = nullptr; };
    View own_, global_;
    double next_at_ = 0, next_open_ = 0, last_written_ = 0;
    bool ready_ = false;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_y_, ps_uv_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> y_, uv_;
    ComPtr<ID3D11RenderTargetView> y_rtv_, uv_rtv_;
    Slot slots_[2];
    int index_ = 0;
};

// Tarayıcı kaynağı: sayfa kendi boyutunda çizilir, kutuya ölçeklenir; saydam yerler sahnede saydam kalır
class BrowserSource : public Source {
public:
    BrowserSource(IDXGIAdapter1* adapter, const BrowserOptions& o) : capture_(adapter, o) {}
    void update(Gfx& g, const json&, const json&, int, int) override {
        frame_.reopen(g, capture_.handle());
        if (capture_.take_fresh() && !frame_.pull(g)) capture_.mark_fresh();
    }
    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        if (frame_.image.srv) g.quad(frame_.image.srv.Get(), box, W, H, cfg.value("opacity", 100) / 100.f, false);
    }
    std::string error() { return capture_.error(); }

private:
    BrowserCapture capture_;
    SharedFrame frame_;
};

std::atomic<bool> g_browser_failed{false};

// Slayt gösterisi: resimler sırayla (isteğe bağlı karışık), yumuşak geçişle. Resimler arka planda okunur, çizim beklemez.
class SlideshowSource : public Source {
public:
    ~SlideshowSource() override { if (loader_.joinable()) loader_.join(); }

    void update(Gfx& g, const json& cfg, const json&, int, int) override {
        const json& s = cfg["settings"];
        std::vector<std::wstring> files;
        for (auto& f : s.value("files", json::array()))
            if (f.is_string()) files.push_back(widen(f.get<std::string>()));
        if (files != files_) {
            if (loader_.joinable()) loader_.join();
            files_ = files;
            position_ = -1;
            next_at_ = 0;
            current_ = previous_ = Tex{};
            loading_ = false;
        }
        if (files_.empty()) return;
        if (loading_ && ready_) {
            loader_.join();
            loading_ = ready_ = false;
            previous_ = current_;
            current_ = loaded_.empty() ? Tex{} : g.make(loaded_);
            fade_start_ = now();
        }
        if (!loading_ && now() >= next_at_) {
            next_at_ = now() + std::clamp(s.value("interval", 5), 1, 3600);
            if (files_.size() == 1 && current_.srv) return;
            int next = (position_ + 1) % int(files_.size());
            if (s.value("shuffle", false) && files_.size() > 2) {
                seed_ = seed_ * 1103515245 + 12345;
                next = int((seed_ >> 8) % files_.size());
                if (next == position_) next = (next + 1) % int(files_.size());
            }
            position_ = next;
            loading_ = true;
            const std::wstring file = files_[next];
            loader_ = std::thread([this, file] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                Bitmap b;
                load_image(file, b);
                loaded_ = std::move(b);
                CoUninitialize();
                ready_ = true;
            });
        }
    }

    void draw(Gfx& g, const json& cfg, int W, int H, Rect& box) override {
        box = free_box(cfg, W, H);
        const float opacity = cfg.value("opacity", 100) / 100.f;
        const bool fade = cfg["settings"].value("fade", true);
        const float t = fade ? float(std::clamp((now() - fade_start_) / 0.8, 0.0, 1.0)) : 1.f;
        auto fit = [&](const Tex& tex) {    // Oran korunur, kutunun ortasına sığdırılır
            const float k = std::min(box.w / tex.w, box.h / tex.h);
            return Rect{box.x + (box.w - tex.w * k) / 2, box.y + (box.h - tex.h * k) / 2, tex.w * k, tex.h * k};
        };
        if (t < 1 && previous_.srv && previous_.w) g.quad(previous_.srv.Get(), fit(previous_), W, H, opacity * (1 - t), false);
        if (current_.srv && current_.w) g.quad(current_.srv.Get(), fit(current_), W, H, opacity * t, false);
    }

private:
    std::vector<std::wstring> files_;
    int position_ = -1;
    double next_at_ = 0, fade_start_ = 0;
    unsigned seed_ = GetTickCount();
    Tex current_, previous_;
    std::thread loader_;
    std::atomic<bool> ready_{false};
    bool loading_ = false;
    Bitmap loaded_;
};

std::wstring make_path(const std::wstring& folder, const std::wstring& extension = L".mp4", const wchar_t* prefix = L"Recording") {
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\%s %04d-%02d-%02d %02d-%02d-%02d", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring path = folder + name + extension;
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && i < 100; i++)   // Aynı saniyede ikinci dosya
        path = folder + name + L" (" + std::to_wstring(i) + L")" + extension;
    return path;
}

}  // namespace

std::vector<std::string> list_cameras() {
    std::vector<std::string> names;
    ComPtr<IMFAttributes> query;
    MFCreateAttributes(&query, 1);
    query->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(query.Get(), &devices, &count))) return names;
    for (UINT32 i = 0; i < count; i++) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
            names.push_back(narrow(name));
            CoTaskMemFree(name);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return names;
}

json Engine::default_record() {
    return {{"folder", narrow(videos_dir() + L"\\Recording Studio")}, {"resolution", "screen"}, {"fps", 60},
            {"quality", "normal"}, {"audio_bitrate", 160}, {"transition", "fade"}, {"video", default_video()},
            {"desktop_audio", true}, {"desktop_device", ""}, {"desktop_volume", 100},
            {"mic_audio", true}, {"mic_device", ""}, {"mic_volume", 100}, {"record_cursor", true}, {"record_keys", false}, {"separate_camera", false}, {"separate_mic", false},
            {"desktop_muted", false}, {"mic_muted", false}, {"push_to_talk", false}, {"mic_monitor", "off"}, {"monitor_device", ""},
            {"mic_filters", default_mic_filters()}, {"audio_inputs", json::array()}, {"multitrack", false},
            {"split_mode", "off"}, {"split_minutes", 30}, {"split_mb", 2048}, {"replay_seconds", 30},
            {"stream_targets", json::array()}, {"desktop_route", "both"}, {"mic_route", "both"},
            {"stream", {{"service", "twitch"}, {"server", "rtmp://live.twitch.tv/app"}, {"key", ""}, {"mode", "same"},
                        {"resolution", "canvas"}, {"fps", 60}, {"video_kbps", 4500}, {"audio_kbps", 160}, {"video", default_video()}}}};
}

struct Engine::Impl {
    Gfx gfx;
    std::map<std::string, std::unique_ptr<Source>> sources;
    std::map<std::string, std::string> source_keys;     // Hangi ayarla açıldı (değişince yeniden açılır)
    ComPtr<ID3D11Texture2D> canvas, preview, previous;
    ComPtr<ID3D11RenderTargetView> canvas_rtv;
    ComPtr<ID3D11ShaderResourceView> previous_srv;
    int W = 0, H = 0;
    std::string scene_id;
    double fade_start = 0;
    AudioMixer mixer;
    std::string audio_key, audio_fx_key;
    std::unique_ptr<FrameOutput> recorder;
    std::unique_ptr<Streamer> stream;
    ComPtr<ID3D11ShaderResourceView> canvas_srv;
    ComPtr<ID3D11Texture2D> stream_frame;       // Yayın çözünürlüğü tuvalden farklıysa
    ComPtr<ID3D11RenderTargetView> stream_rtv;
    int stream_w = 0, stream_h = 0;
    // Video Düzenleyici'nin imleç vurgusu, tıklama halkası ve tuş göstergesi için kayıt yanına yazılan veri
    struct InputLog {
        json cursor = json::array(), clicks = json::array(), keys = json::array();
        double last_t = -1;
        float last_x = -1, last_y = -1;
        bool mouse[2] = {}, key[256] = {};
        void clear() { *this = InputLog(); }
    } input;
    // Ayrı kamera ve mikrofon dosyaları (Video Düzenleyici ayrı kanallara koyar)
    std::unique_ptr<Recorder> camera_rec;
    WavWriter mic_wav;
    std::wstring camera_path, mic_path;
    double camera_offset = 0;
    Rect camera_area;
    bool camera_mirror = false;
    // Kaydı bölme: önceki parçaların toplam süresi ve bu parçanın başlangıcı
    double record_base = 0;
    int record_part = 0;
    double size_checked = 0;
    long long record_bytes = 0, last_bytes = 0;
    double last_bytes_at = 0, record_kbps = 0;
    // Çoklu ses parçası: kayıttaki her ek parçanın karıştırıcı girişi
    std::vector<int> track_inputs;
    // Tekrar arabelleği
    std::unique_ptr<FfmpegOutput> replay;
    std::wstring replay_folder;
    double replay_started = 0;
    int replay_seconds = 30, replay_segment = 2;
    std::thread replay_saver;
    // Stüdyo modu: düzenlenen sahne ayrı tuvale çizilir, yayındaki sahne ayrı önizlemeye kopyalanır
    ComPtr<ID3D11Texture2D> edit_canvas, program_preview;
    ComPtr<ID3D11RenderTargetView> edit_rtv;
    std::string program_id;
    // Sahne içinde sahne (ve gruplar): iç sahne kendi tuvaline çizilir
    struct Nested { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv; long long frame = -1; };
    std::map<std::string, Nested> nested;
    long long frame_no = 0;
    // Aynı anda birden fazla platforma yayın: ana yayına ek hedefler
    std::vector<std::unique_ptr<Streamer>> extra_streams;
    std::vector<std::string> extra_names;
    std::vector<Status::Target> extra_failed;
    VirtualCamera vcam;
    // İstatistik
    long long rendered = 0;
    double fps_since = 0, render_fps = 0;
    long long fps_count = 0;
};

namespace {
std::wstring companion_path(const std::wstring& video, const wchar_t* suffix) {     // "Kayit X.mp4" -> "Kayit X - camera.mp4"
    const size_t dot = video.find_last_of(L'.'), slash = video.find_last_of(L"\\/");
    const std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? video.substr(0, dot) : video;
    return stem + suffix;
}

std::wstring file_name(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
}  // namespace

namespace {
std::string key_name(int vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    LONG scan = LONG(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
    switch (vk) {   // Genişletilmiş tuşlar: adları doğru gelsin
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: scan |= 1 << 24; break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(scan, name, 64) > 0) return narrow(name);
    return "Tuş " + std::to_string(vk);
}

// Her karede: imleç yeri (hareket ettikçe, saniyede en çok 30), tıklamalar, isteğe bağlı kısayollar.
// Gizlilik: düz yazı kaydedilmez; yalnızca Ctrl / Alt / Win ile basılan tuşlar ve F tuşları, Esc.
void log_input(Engine::Impl& m, const json& scene, const json& rec, double t) {
    auto& in = m.input;
    float x = 0, y = 0;
    bool have = false;
    for (auto& cfg : scene["sources"]) {
        const std::string type = cfg.value("type", std::string());
        if (type != "screen" && type != "region") continue;
        auto it = m.sources.find(cfg.value("id", std::string()));
        if (it != m.sources.end() && it->second->pointer_at(cfg, m.W, m.H, x, y)) { have = true; break; }
    }
    if (rec.value("record_cursor", true) && have) {
        const bool moved = fabsf(x - in.last_x) >= 1 || fabsf(y - in.last_y) >= 1;
        if ((moved && t - in.last_t >= 1.0 / 30) || in.last_t < 0) {
            in.cursor.push_back({std::round(t * 1000) / 1000, int(x), int(y)});
            in.last_t = t; in.last_x = x; in.last_y = y;
        }
        const int buttons[2] = {VK_LBUTTON, VK_RBUTTON};
        for (int b = 0; b < 2; b++) {
            const bool down = (GetAsyncKeyState(buttons[b]) & 0x8000) != 0;
            if (down && !in.mouse[b]) in.clicks.push_back({std::round(t * 1000) / 1000, int(x), int(y), b});
            in.mouse[b] = down;
        }
    }
    if (rec.value("record_keys", false)) {
        auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const bool ctrl = held(VK_CONTROL), alt = held(VK_MENU), shift = held(VK_SHIFT), win = held(VK_LWIN) || held(VK_RWIN);
        for (int vk = 8; vk < 255; vk++) {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || (vk >= VK_LSHIFT && vk <= VK_RMENU)) continue;
            const bool down = held(vk);
            if (down && !in.key[vk]) {
                const bool function = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_ESCAPE;
                if (ctrl || alt || win || function) {
                    std::string combo;
                    if (ctrl) combo += "Ctrl + ";
                    if (alt) combo += "Alt + ";
                    if (shift) combo += "Shift + ";
                    if (win) combo += "Win + ";
                    in.keys.push_back({std::round(t * 1000) / 1000, combo + key_name(vk)});
                }
            }
            in.key[vk] = down;
        }
    }
}

void save_input(const Engine::Impl& m, const std::wstring& video) {
    const auto& in = m.input;
    const bool camera = !m.camera_path.empty() && m.camera_path != L"-", mic = !m.mic_path.empty();
    if (video.empty() || (in.cursor.empty() && in.keys.empty() && !camera && !mic)) return;
    json data = {{"version", 1}, {"w", m.W}, {"h", m.H}, {"cursor", in.cursor}, {"clicks", in.clicks}, {"keys", in.keys}};
    if (camera)
        data["camera"] = {{"file", narrow(file_name(m.camera_path))}, {"offset", m.camera_offset}, {"mirror", m.camera_mirror},
                          {"x", m.camera_area.x}, {"y", m.camera_area.y}, {"w", m.camera_area.w}, {"h", m.camera_area.h}};
    if (mic) data["mic"] = {{"file", narrow(file_name(m.mic_path))}};
    std::ofstream o(video + L".input.json", std::ios::binary);
    o << data.dump();
}
}  // namespace

bool Engine::create_device(std::string& error) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { error = "DXGI başlatılamadı"; return false; }
    for (UINT i = 0; !adapter_; i++) {   // Ana ekranın bağlı olduğu ekran kartı
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT j = 0;; j++) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            if (d.AttachedToDesktop && d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0) adapter_ = a;
        }
    }
    if (!adapter_) { error = "Ana ekran bulunamadı"; return false; }
    for (UINT j = 0;; j++) {
        ComPtr<IDXGIOutput> o;
        if (adapter_->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC d;
        o->GetDesc(&d);
        const auto& r = d.DesktopCoordinates;
        monitor_rects_.push_back(r);
        monitors_.push_back("Ekran " + std::to_string(j + 1) + " (" + std::to_string(r.right - r.left) + " x " +
                            std::to_string(r.bottom - r.top) + ")");
    }
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device_, nullptr, &ctx))) {
        error = "Ekran kartı cihazı oluşturulamadı";
        return false;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);   // Kodlayıcı da aynı cihazı kullanıyor
    return true;
}

bool Engine::start(std::string& error) {
    if (!create_device(error)) return false;
    impl_ = std::make_shared<Impl>();
    if (!impl_->gfx.init(device_.Get())) { error = "Gölgelendirici derlenemedi"; return false; }
    thread_ = std::thread(&Engine::run, this);
    return true;
}

void Engine::stop() {
    halt_ = true;
    if (thread_.joinable()) thread_.join();
}

void Engine::set_document(const json& doc) {
    std::lock_guard<std::mutex> g(lock_);
    doc_ = doc;
    doc_dirty_ = true;
}

void Engine::set_record_settings(const json& s) {
    std::lock_guard<std::mutex> g(lock_);
    record_ = s;
    record_dirty_ = true;
}

Status Engine::status() {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

std::map<std::string, Rect> Engine::boxes() {
    std::lock_guard<std::mutex> g(lock_);
    return boxes_;
}

void Engine::canvas_size(int& w, int& h) {
    std::lock_guard<std::mutex> g(lock_);
    w = width_; h = height_;
}

HANDLE Engine::preview_handle() {
    std::lock_guard<std::mutex> g(lock_);
    return preview_;
}

std::vector<std::string> Engine::monitors() { return monitors_; }

HANDLE Engine::program_handle() {
    std::lock_guard<std::mutex> g(lock_);
    return program_preview_;
}

namespace {
// Gizli pencerede komut çalıştırır, bitmesini bekler; çıkış kodu döner
int run_hidden_process(const std::wstring& command, DWORD timeout) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(pi.hProcess, timeout) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return int(code);
}

struct Segment { std::wstring file; double start = 0, end = 0; };

std::vector<Segment> read_segments(const std::wstring& folder) {
    std::vector<Segment> out;
    std::ifstream in(folder + L"\\list.csv");
    std::string line;
    while (std::getline(in, line)) {
        const size_t a = line.find(','), b = line.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        Segment s;
        s.file = folder + L"\\" + widen(line.substr(0, a));
        s.start = atof(line.c_str() + a + 1);
        s.end = atof(line.c_str() + b + 1);
        out.push_back(s);
    }
    return out;
}

// Tekrar arabelleği için kodlayıcı: kayıttaki FFmpeg kodlayıcısı, Windows kodlayıcısı seçiliyse çalışan ilk H.264
std::string replay_encoder(const std::string& record) {
    if (record != "windows" && find_encoder(record)) return record;
    for (const char* k : {"nvenc_h264", "amf_h264", "qsv_h264", "x264"})
        if (encoder_state(k) == 1) return k;
    return "x264";
}
}  // namespace

void Engine::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    timeBeginPeriod(1);   // Windows zamanlayıcısı 15,6 ms yerine 1 ms hassasiyetle uyansın
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    auto& m = *impl_;
    json doc, rec;
    double next_tick = now(), inactive_since = 0, ms = 0, shot_at = 0;

    // Tuvali ekran kartından indirir; PNG'ye yazma ayrı iş parçacığında (kayıt kare kaçırmasın)
    auto take_screenshot = [&] {
        D3D11_TEXTURE2D_DESC td{};
        m.canvas->GetDesc(&td);
        td.BindFlags = 0; td.MiscFlags = 0;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Bitmap b;
        if (SUCCEEDED(device_->CreateTexture2D(&td, nullptr, &staging))) {
            m.gfx.ctx->CopyResource(staging.Get(), m.canvas.Get());
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(m.gfx.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                b.w = int(td.Width); b.h = int(td.Height);
                b.px.resize(size_t(b.w) * b.h * 4);
                for (int y = 0; y < b.h; y++)
                    memcpy(&b.px[size_t(y) * b.w * 4], static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(b.w) * 4);
                m.gfx.ctx->Unmap(staging.Get(), 0);
                for (size_t i = 3; i < b.px.size(); i += 4) b.px[i] = 255;
            }
        }
        if (b.empty()) {
            std::lock_guard<std::mutex> g(lock_);
            status_.shot_error = "Ekran görüntüsü alınamadı";
            status_.shots++;
            return;
        }
        const std::wstring path = make_path(widen(rec.value("folder", std::string("."))), L".png", L"Screenshot");
        if (shot_thread_.joinable()) shot_thread_.join();
        shot_thread_ = std::thread([this, b = std::move(b), path] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool ok = save_png(b, path);
            CoUninitialize();
            std::lock_guard<std::mutex> g(lock_);
            status_.shot_path = path;
            status_.shot_error = ok ? "" : "Ekran görüntüsü kaydedilemedi";
            status_.shots++;
        });
    };

    auto make_shared = [&](D3D11_TEXTURE2D_DESC td, ComPtr<ID3D11Texture2D>& tex) {
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;   // Arayüz ayrı cihazda: önizleme kopyalanmadan paylaşılır
        tex.Reset();
        device_->CreateTexture2D(&td, nullptr, &tex);
        ComPtr<IDXGIResource> res;
        HANDLE handle = nullptr;
        if (tex && SUCCEEDED(tex.As(&res))) res->GetSharedHandle(&handle);
        return handle;
    };

    auto make_canvas = [&](int W, int H) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W; td.Height = H; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        m.canvas.Reset(); m.canvas_rtv.Reset(); m.canvas_srv.Reset(); m.preview.Reset(); m.previous.Reset(); m.previous_srv.Reset();
        m.edit_canvas.Reset(); m.edit_rtv.Reset();
        device_->CreateTexture2D(&td, nullptr, &m.canvas);
        device_->CreateRenderTargetView(m.canvas.Get(), nullptr, &m.canvas_rtv);
        device_->CreateShaderResourceView(m.canvas.Get(), nullptr, &m.canvas_srv);
        device_->CreateTexture2D(&td, nullptr, &m.previous);
        device_->CreateShaderResourceView(m.previous.Get(), nullptr, &m.previous_srv);
        device_->CreateTexture2D(&td, nullptr, &m.edit_canvas);
        device_->CreateRenderTargetView(m.edit_canvas.Get(), nullptr, &m.edit_rtv);
        const HANDLE handle = make_shared(td, m.preview), program = make_shared(td, m.program_preview);
        m.W = W; m.H = H;
        std::lock_guard<std::mutex> g(lock_);
        width_ = W; height_ = H; preview_ = handle; program_preview_ = program;
    };

    // Ek ses girişleri (rec["audio_inputs"]); açık olanlar karıştırıcıya 2. sıradan başlayarak girer
    auto extra_inputs = [&](std::vector<std::string>* names) {
        std::vector<AudioInput> list;
        for (auto& j : rec.value("audio_inputs", json::array())) {
            if (!j.value("enabled", true) || int(list.size()) + 2 >= AudioMixer::kMaxInputs) continue;
            AudioInput in;
            const std::string kind = j.value("kind", std::string("mic"));
            in.kind = kind == "app" ? 2 : kind == "speaker" ? 1 : 0;
            in.device = widen(j.value("device", std::string()));
            in.exe = j.value("exe", std::string());
            list.push_back(in);
            if (names) names->push_back(j.value("name", std::string("Ses")));
        }
        return list;
    };

    auto start_audio = [&](bool force) {
        const std::string key = (rec.value("desktop_audio", true) ? rec.value("desktop_device", std::string("")) + "|1" : "|0") +
                                (rec.value("mic_audio", true) ? rec.value("mic_device", std::string("")) + "|1" : "|0");
        std::string full = key;
        for (auto& j : rec.value("audio_inputs", json::array()))
            full += "|" + j.value("kind", std::string()) + j.value("device", std::string()) + j.value("exe", std::string()) + (j.value("enabled", true) ? "1" : "0");
        if (!force && full == m.audio_key && m.mixer.running()) return;
        m.audio_key = full;
        m.mixer.start(rec.value("desktop_audio", true), widen(rec.value("desktop_device", std::string(""))),
                      rec.value("mic_audio", true), widen(rec.value("mic_device", std::string(""))), extra_inputs(nullptr));
    };

    auto any_audio = [&] {
        return rec.value("desktop_audio", true) || rec.value("mic_audio", true) || !extra_inputs(nullptr).empty();
    };

    // Karıştırıcının sesi o an açık olan kayda, yayına ve tekrar arabelleğine gider
    auto update_audio_sink = [&] {
        FrameOutput* r = m.recorder.get();
        Streamer* s = m.stream.get();
        FfmpegOutput* replay = m.replay.get();
        if (r && m.mic_wav.is_open()) {     // Mikrofon kendi dosyasına; kayıt duraklatılınca o da durur
            WavWriter* wav = &m.mic_wav;
            m.mixer.set_mic_sink([r, wav](const int16_t* samples, int n) { if (!r->paused()) wav->write(samples, n); });
        } else {
            m.mixer.set_mic_sink(nullptr);
        }
        if (r && !m.track_inputs.empty()) {     // Çoklu ses parçası: her giriş kendi parçasına
            std::vector<int> map = m.track_inputs;
            m.mixer.set_track_sink([r, map](const int16_t* const* inputs, int n) {
                for (size_t k = 0; k < map.size(); k++) r->write_audio_track(int(k) + 1, inputs[map[k]], n);
            });
        } else {
            m.mixer.set_track_sink(nullptr);
        }
        // Yayının kendi karışımı (yalnızca kayda / yalnızca yayına giden sesler ayrılır); ek hedefler de aynı sesi alır
        if (s) {
            std::vector<Streamer*> all{s};
            for (auto& e : m.extra_streams) all.push_back(e.get());
            m.mixer.set_stream_sink([all](const int16_t* samples, int n) { for (Streamer* x : all) x->write_audio(samples, n); });
        } else {
            m.mixer.set_stream_sink(nullptr);
        }
        if (!r && !replay) { m.mixer.set_sink(nullptr); return; }
        m.mixer.set_sink([r, replay](const int16_t* samples, int n) {
            if (r) r->write_audio(samples, n);
            if (replay) replay->write_audio(samples, n);
        });
    };

    // Kayıt başlatma (bölünen kayıtta sonraki parça da buradan açılır)
    auto open_recording = [&](std::string& error) -> bool {
        const bool audio = any_audio();
        start_audio(false);
        const json video = rec.value("video", json::object());
        const bool windows_encoder = video.value("encoder", std::string("windows")) == "windows";
        const std::wstring folder = widen(rec.value("folder", std::string(".")));
        const std::wstring path = windows_encoder ? make_path(folder) : make_path(folder, container_extension(video.value("container", std::string("mp4"))));
        std::unique_ptr<FrameOutput> r;
        bool opened = false;
        m.track_inputs.clear();
        if (windows_encoder) {
            auto out = std::make_unique<Recorder>();
            opened = out->open(device_.Get(), path, m.W, m.H, rec.value("fps", 60), rec.value("quality", std::string("normal")),
                               rec.value("audio_bitrate", 160), audio, error);
            r = std::move(out);
        } else {
            EncodeConfig c = make_encode_config(video, m.W, m.H, rec.value("fps", 60), video.value("bitrate", 6000), rec.value("audio_bitrate", 160), audio, false);
            c.path = path;
            if (audio && rec.value("multitrack", false) && c.container != "flv") {   // İlk parça karışım, sonra her giriş ayrı
                std::vector<std::string> names;
                extra_inputs(&names);
                if (rec.value("desktop_audio", true)) { c.audio_tracks.push_back("Desktop audio"); m.track_inputs.push_back(0); }
                if (rec.value("mic_audio", true)) { c.audio_tracks.push_back("Microphone"); m.track_inputs.push_back(1); }
                for (size_t i = 0; i < names.size(); i++) { c.audio_tracks.push_back(names[i]); m.track_inputs.push_back(int(i) + 2); }
            }
            auto out = std::make_unique<FfmpegOutput>();
            opened = out->open(device_.Get(), c, error);
            r = std::move(out);
        }
        if (!opened) { m.track_inputs.clear(); return false; }
        m.recorder = std::move(r);
        m.input.clear();
        m.camera_rec.reset();
        m.camera_path.clear();
        m.mic_path.clear();
        m.record_bytes = m.last_bytes = 0;
        m.last_bytes_at = now();
        m.record_kbps = 0;
        if (rec.value("separate_mic", false) && rec.value("mic_audio", true)) {
            m.mic_path = companion_path(path, L" - microphone.wav");
            if (!m.mic_wav.open(m.mic_path)) m.mic_path.clear();
        }
        update_audio_sink();
        std::lock_guard<std::mutex> g(lock_);
        status_.error.clear();
        status_.path = path;
        return true;
    };

    auto close_recording = [&](bool split) {
        auto r = std::move(m.recorder);
        update_audio_sink();
        const std::string failure = r->failed() ? r->failure() : std::string();
        std::string error;
        if (split) m.record_base += r->elapsed();
        r->finish(error);
        if (m.camera_rec) { std::string ignored; m.camera_rec->finish(ignored); m.camera_rec.reset(); }
        m.mic_wav.close();
        save_input(m, r->path());
        m.camera_path.clear();
        m.mic_path.clear();
        m.input.clear();
        m.track_inputs.clear();
        std::lock_guard<std::mutex> g(lock_);
        status_.last_path = r->path();
        status_.error = failure.empty() ? error : failure;
        if (!split) status_.recording = status_.paused = false;    // Pencere gizliyken döngü boşta kalır, durum burada düzelir
    };

    // Tekrar arabelleği: aynı sahne ayrı bir kodlayıcıyla geçici klasöre kısa parçalar halinde sürekli yazılır
    auto start_replay = [&]() -> std::string {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        m.replay_folder = std::wstring(temp) + L"Recording Studio Replay";
        SHCreateDirectoryExW(nullptr, m.replay_folder.c_str(), nullptr);
        for (const auto& e : std::filesystem::directory_iterator(m.replay_folder)) { std::error_code ec; std::filesystem::remove(e.path(), ec); }
        m.replay_seconds = std::clamp(rec.value("replay_seconds", 30), 5, 600);
        m.replay_segment = 2;
        json video = rec.value("video", json::object());
        const std::string encoder = replay_encoder(video.value("encoder", std::string("windows")));
        if (video.value("encoder", std::string("windows")) != encoder) video = encoder_defaults(encoder, false);
        video["encoder"] = encoder;
        video["rate"] = "cbr";
        const int fps = rec.value("fps", 60);
        EncodeConfig c = make_encode_config(video, m.W, m.H, fps, recording_kbps(m.W, m.H, fps, rec.value("quality", std::string("normal"))),
                                            rec.value("audio_bitrate", 160), any_audio(), false);
        c.audio_codec = "aac";
        c.keyint = 1;       // Parçalar anahtar karede bölünür
        c.segment_folder = m.replay_folder;
        c.segment_seconds = m.replay_segment;
        c.segment_count = m.replay_seconds / m.replay_segment + 4;
        start_audio(false);
        auto out = std::make_unique<FfmpegOutput>();
        std::string error;
        if (!out->open(device_.Get(), c, error)) return error.empty() ? "Tekrar arabelleği başlatılamadı" : error;
        m.replay = std::move(out);
        m.replay_started = now();
        update_audio_sink();
        return {};
    };

    auto stop_replay = [&] {
        if (!m.replay) return;
        auto r = std::move(m.replay);
        update_audio_sink();
        std::string ignored;
        r->finish(ignored);
        if (m.replay_saver.joinable()) m.replay_saver.join();
        for (const auto& e : std::filesystem::directory_iterator(m.replay_folder)) { std::error_code ec; std::filesystem::remove(e.path(), ec); }
    };

    // Basılan anı içeren parça bitince son N saniyedeki parçalar birleştirilip kayıt klasörüne yazılır
    auto save_replay = [&] {
        if (!m.replay) return;
        if (m.replay_saver.joinable()) m.replay_saver.join();
        const double pressed = now() - m.replay_started;
        const std::wstring folder = m.replay_folder, out = make_path(widen(rec.value("folder", std::string("."))), L".mp4", L"Replay");
        const int seconds = m.replay_seconds, segment = m.replay_segment;
        m.replay_saver = std::thread([this, pressed, folder, out, seconds, segment] {
            std::vector<Segment> list;
            for (int i = 0; i < (segment + 3) * 10; i++) {
                list = read_segments(folder);
                if (!list.empty() && list.back().end >= pressed - 0.05) break;
                Sleep(100);
            }
            std::sort(list.begin(), list.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
            std::wstring concat;
            for (auto& s : list)
                if (s.end > pressed - seconds && s.start <= pressed + 0.05) concat += L"file '" + s.file + L"'\n";
            std::string error;
            if (concat.empty()) {
                error = "Tekrar arabelleğinde henüz görüntü yok";
            } else {
                const std::wstring list_file = folder + L"\\concat.txt";
                { std::ofstream f(list_file, std::ios::binary); f << narrow(concat); }
                const std::wstring cmd = L"\"" + ffmpeg_exe() + L"\" -hide_banner -loglevel error -nostdin -f concat -safe 0 -i \"" + list_file +
                                         L"\" -c copy -avoid_negative_ts make_zero -movflags +faststart -y \"" + out + L"\"";
                if (run_hidden_process(cmd, 60000) != 0) error = "Tekrar kaydedilemedi";
            }
            std::lock_guard<std::mutex> g(lock_);
            status_.replay_path = error.empty() ? out : L"";
            status_.replay_error = error;
            status_.replay_saves++;
        });
    };

    // Sahne çizimi; boxes verilirse kaynakların tuvaldeki kutuları yazılır
    auto draw_scene = [&](const json& scene, ID3D11RenderTargetView* rtv, std::map<std::string, Rect>* boxes, bool separate_camera, bool fade,
                          bool transparent = false) {
        m.gfx.begin(rtv, m.W, m.H, transparent);
        for (auto& cfg : scene["sources"]) {
            if (cfg.value("type", std::string()) == "scene") {      // İç sahne: önceden kendi tuvaline çizildi
                const Rect box = free_box(cfg, m.W, m.H);
                auto n = m.nested.find(cfg["settings"].value("scene", std::string()));
                if (cfg.value("visible", true) && n != m.nested.end() && n->second.frame == m.frame_no && n->second.srv) {
                    m.gfx.fx = parse_fx(cfg);
                    m.gfx.quad(n->second.srv.Get(), box, m.W, m.H, cfg.value("opacity", 100) / 100.f, false);
                    m.gfx.fx = Fx{};
                }
                if (boxes) (*boxes)[cfg.value("id", std::string())] = box;
                continue;
            }
            auto it = m.sources.find(cfg.value("id", std::string()));
            if (it == m.sources.end()) continue;
            Rect box;
            const bool skip = separate_camera && cfg.value("type", std::string()) == "camera";
            if (cfg.value("visible", true) && !skip) {
                m.gfx.fx = parse_fx(cfg);
                it->second->draw(m.gfx, cfg, m.W, m.H, box);
                m.gfx.fx = Fx{};
            } else {
                box = free_box(cfg, m.W, m.H);
            }
            if (boxes) (*boxes)[cfg.value("id", std::string())] = box;
        }
        if (fade && m.fade_start) {
            const double t = (now() - m.fade_start) / kFade;
            if (t >= 1) m.fade_start = 0;
            else m.gfx.quad(m.previous_srv.Get(), {0, 0, float(m.W), float(m.H)}, m.W, m.H, float(1 - t), true);
        }
        ID3D11RenderTargetView* none = nullptr;
        m.gfx.ctx->OMSetRenderTargets(1, &none, nullptr);
        ID3D11ShaderResourceView* no_srv = nullptr;
        m.gfx.ctx->PSSetShaderResources(0, 1, &no_srv);
    };

    while (!halt_) {
        bool recording = m.recorder != nullptr;
        const bool streaming = m.stream != nullptr;
        {   // Arayüzden gelen değişiklikler
            std::lock_guard<std::mutex> g(lock_);
            if (doc_dirty_) { doc = doc_; doc_dirty_ = false; }
            if (record_dirty_) { rec = record_; record_dirty_ = false; }
        }
        if (rec.is_null() || doc.is_null()) { Sleep(5); continue; }

        int W = 0, H = 0;
        const std::string res = rec.value("resolution", std::string("screen"));
        if (res == "screen") {
            W = GetSystemMetrics(SM_CXSCREEN); H = GetSystemMetrics(SM_CYSCREEN);
        } else {
            sscanf_s(res.c_str(), "%dx%d", &W, &H);
        }
        W &= ~1; H &= ~1;   // Kodlayıcılar çift sayı ister
        if (!recording && !streaming && !m.replay && (W != m.W || H != m.H)) make_canvas(W, H);

        const int request = record_request_.exchange(0);
        if (request == 1 && !recording) {
            std::string error;
            m.record_base = 0;
            m.record_part = 0;
            if (!open_recording(error)) {
                std::lock_guard<std::mutex> g(lock_);
                status_.error = error;
            }
        } else if ((request == 2 && recording) || (recording && m.recorder->failed())) {
            close_recording(false);
            m.record_base = 0;
            m.record_part = 0;
        } else if (recording && !m.recorder->paused()) {     // Kaydı bölme: süre ya da boyut dolunca yeni dosyaya geçilir
            const std::string split = rec.value("split_mode", std::string("off"));
            bool due = split == "time" && m.recorder->elapsed() >= std::max(1, rec.value("split_minutes", 30)) * 60.0;
            if (split == "size" && m.record_bytes >= (long long)std::max(10, rec.value("split_mb", 2048)) * 1024 * 1024) due = true;
            if (due) {
                close_recording(true);
                std::string error;
                m.record_part = std::max(1, m.record_part) + 1;
                if (!open_recording(error)) {
                    std::lock_guard<std::mutex> g(lock_);
                    status_.error = error;
                }
            }
        }
        if (m.recorder && now() - m.size_checked > 1) {     // Dosya boyutu ve bit hızı (bölme ve istatistik için)
            m.size_checked = now();
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(m.recorder->path().c_str(), GetFileExInfoStandard, &fa)) {
                m.record_bytes = (long long(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
                // Dosya parça parça (2 sn'de bir) büyür: hız son büyümeden bu yana geçen süreyle ölçülür
                const double dt = now() - m.last_bytes_at;
                if (m.record_bytes > m.last_bytes && dt > 0.5) {
                    m.record_kbps = (m.record_bytes - m.last_bytes) * 8 / 1000.0 / dt;
                    m.last_bytes = m.record_bytes;
                    m.last_bytes_at = now();
                } else if (dt > 6) {
                    m.record_kbps = 0;
                }
            }
        }

        int stream_action = stream_request_.exchange(0);
        if (m.stream && m.stream->failed()) {   // Bağlantı kalıcı olarak koptu ya da sunucu reddetti
            const std::string error = m.stream->stats().error;
            stream_action = 2;
            std::lock_guard<std::mutex> g(lock_);
            status_.stream_error = error;
        }
        if (stream_action == 1 && !m.stream) {
            const json st = rec.value("stream", json::object());
            StreamConfig c;
            c.url = st.value("server", std::string());
            c.key = st.value("key", std::string());
            c.audio = any_audio();
            c.width = m.W; c.height = m.H;
            const bool custom_stream = st.value("mode", std::string("same")) == "custom";
            if (custom_stream) {
                const std::string size = st.value("resolution", std::string("canvas"));
                if (size != "canvas") sscanf_s(size.c_str(), "%dx%d", &c.width, &c.height);
                c.fps = st.value("fps", 60);
                c.video_kbps = st.value("video_kbps", 4500);
                c.audio_kbps = st.value("audio_kbps", 160);
            } else {
                c.fps = rec.value("fps", 60);
                c.video_kbps = std::min(kStreamMaxAutoKbps, recording_kbps(m.W, m.H, c.fps, rec.value("quality", std::string("normal"))));
                c.audio_kbps = rec.value("audio_bitrate", 160);
            }
            c.width &= ~1; c.height &= ~1;
            const json video = custom_stream ? st.value("video", json::object()) : rec.value("video", json::object());
            const std::string encoder = video.value("encoder", std::string("windows"));
            const EncoderInfo* info = find_encoder(encoder);
            c.use_ffmpeg = encoder != "windows";
            c.codec = info ? info->codec : "h264";
            if (c.use_ffmpeg) {
                if (!custom_stream) c.video_kbps = std::min(kStreamMaxAutoKbps, video.value("bitrate", 6000));
                c.encode = make_encode_config(video, c.width, c.height, c.fps, c.video_kbps, c.audio_kbps, c.audio, true);
            }
            start_audio(false);
            c.delay_ms = std::clamp(st.value("delay", 0), 0, 600) * 1000;
            auto s = std::make_unique<Streamer>();
            std::string error;
            if (c.codec == "vp9") error = "VP9 ile yayın yapılamaz. Yayın için H.264, HEVC ya da AV1 kodlayıcı seçin.";
            m.extra_streams.clear();
            m.extra_names.clear();
            m.extra_failed.clear();
            if (error.empty() && s->start(device_.Get(), c, error)) {
                for (auto& t : rec.value("stream_targets", json::array())) {   // Ek hedefler: aynı görüntü, kendi bağlantısı
                    if (!t.value("enabled", true) || t.value("server", std::string()).empty()) continue;
                    StreamConfig ct = c;
                    ct.url = t.value("server", std::string());
                    ct.key = t.value("key", std::string());
                    auto extra = std::make_unique<Streamer>();
                    std::string e;
                    if (extra->start(device_.Get(), ct, e)) {
                        m.extra_streams.push_back(std::move(extra));
                        m.extra_names.push_back(t.value("name", std::string()));
                    } else {
                        m.extra_failed.push_back({t.value("name", std::string()), 0, 0, e});
                    }
                }
                m.stream_frame.Reset(); m.stream_rtv.Reset();
                m.stream_w = c.width; m.stream_h = c.height;
                if (c.width != m.W || c.height != m.H) {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = c.width; td.Height = c.height; td.MipLevels = td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    device_->CreateTexture2D(&td, nullptr, &m.stream_frame);
                    device_->CreateRenderTargetView(m.stream_frame.Get(), nullptr, &m.stream_rtv);
                }
                m.stream = std::move(s);
                update_audio_sink();
                std::lock_guard<std::mutex> g(lock_);
                status_.stream_error.clear();
            } else {
                std::lock_guard<std::mutex> g(lock_);
                status_.stream_error = error;
            }
        } else if (stream_action == 2 && m.stream) {
            auto s = std::move(m.stream);
            auto extras = std::move(m.extra_streams);
            m.extra_streams.clear();
            update_audio_sink();
            s->stop();
            for (auto& e : extras) e->stop();
            m.extra_names.clear();
            std::lock_guard<std::mutex> g(lock_);
            status_.streaming = false;
            status_.stream_state = 0;
        }

        for (size_t i = 0; i < m.extra_streams.size();) {     // Kopan ek hedef tek başına kapanır, diğerleri sürer
            if (!m.extra_streams[i]->failed()) { i++; continue; }
            auto dead = std::move(m.extra_streams[i]);
            m.extra_failed.push_back({m.extra_names[i], 0, 0, dead->stats().error});
            m.extra_streams.erase(m.extra_streams.begin() + i);
            m.extra_names.erase(m.extra_names.begin() + i);
            update_audio_sink();
            dead->stop();
        }

        // Tekrar arabelleği
        const int replay_action = replay_request_.exchange(0);
        if (replay_action == 1 && !m.replay) {
            const std::string error = start_replay();
            std::lock_guard<std::mutex> g(lock_);
            status_.replay_error = error;
        } else if ((replay_action == 2 && m.replay) || (m.replay && m.replay->failed())) {
            const std::string failure = m.replay->failed() ? m.replay->failure() : std::string();
            stop_replay();
            std::lock_guard<std::mutex> g(lock_);
            status_.replay_error = failure;
            status_.replay_on = false;      // Başka çıkış yoksa döngü boşta kalır, aşağıdaki durum güncellemesine inmez
        }
        if (replay_save_request_.exchange(false)) {
            if (m.replay) save_replay();
            else { std::lock_guard<std::mutex> g(lock_); status_.replay_error = "Tekrar arabelleği kapalı"; status_.replay_saves++; }
        }

        const int vcam_action = vcam_request_.exchange(0);
        if (vcam_action == 1) {
            std::string error;
            m.vcam.start(error);
            std::lock_guard<std::mutex> g(lock_);
            status_.vcam_error = error;
        } else if (vcam_action == 2) {
            m.vcam.stop();
        }

        FrameOutput* recorder = m.recorder.get();
        Streamer* stream = m.stream.get();
        FfmpegOutput* replay = m.replay.get();
        recording = recorder != nullptr;
        if (pause_request_.exchange(false) && recorder) {
            if (recorder->paused()) recorder->resume(); else recorder->pause();
            if (m.camera_rec) { if (recorder->paused()) m.camera_rec->pause(); else m.camera_rec->resume(); }
        }

        if (shot_request_.exchange(false) && !shot_at)     // Kaynaklar kapalıysa açılıp ilk kareyi vermeleri beklenir
            shot_at = now() + (inactive_since || m.sources.empty() ? 0.8 : 0.0);
        if (!recorder && !stream && !replay && !m.vcam.running() && !active_ && !shot_at) {   // Çizilecek bir şey yok: kaynaklar bir süre açık kalır
            if (!inactive_since) inactive_since = now();
            if (now() - inactive_since > kIdleClose) {
                m.sources.clear(); m.source_keys.clear();
                m.mixer.stop();
            }
            Sleep(30);
            next_tick = now();
            continue;
        }
        inactive_since = 0;
        start_audio(false);
        {   // Susturma ve bas-konuş: ses düzeyi sıfıra iner (karıştırıcı yumuşak geçer)
            const bool ptt_closed = rec.value("push_to_talk", false) && !ptt_held_;
            m.mixer.set_volumes(rec.value("desktop_muted", false) ? 0.f : rec.value("desktop_volume", 100) / 100.f,
                                rec.value("mic_muted", false) || ptt_closed ? 0.f : rec.value("mic_volume", 100) / 100.f);
            auto route = [](const std::string& r) { return r == "record" ? 1 : r == "stream" ? 2 : 0; };
            m.mixer.set_route(0, route(rec.value("desktop_route", std::string("both"))));
            m.mixer.set_route(1, route(rec.value("mic_route", std::string("both"))));
            int index = 2;
            for (auto& j : rec.value("audio_inputs", json::array())) {
                if (!j.value("enabled", true)) continue;
                m.mixer.set_route(index, route(j.value("route", std::string("both"))));
                m.mixer.set_volume(index++, j.value("muted", false) ? 0.f : j.value("volume", 100) / 100.f);
            }
            const json filters = rec.value("mic_filters", json::object());
            const std::string monitor = rec.value("mic_monitor", std::string("off")), device = rec.value("monitor_device", std::string());
            const std::string key = filters.dump() + "|" + monitor + "|" + device;
            if (key != m.audio_fx_key) {
                m.audio_fx_key = key;
                m.mixer.set_mic_filters(filters);
                m.mixer.set_monitor(monitor == "only" ? 2 : monitor == "on" ? 1 : 0, widen(device));
            }
        }

        // Kayıtta kare sınırına hizalı, değilse sabit aralıkla çiz
        const bool record_paces = recorder && !recorder->paused();
        const bool stream_paces = stream && (!record_paces || stream->fps() > rec.value("fps", 60));
        const int fps = stream_paces ? stream->fps() : recorder || replay ? rec.value("fps", 60) : 60;
        const double target = stream_paces ? stream->next_frame_time() + 0.001 : record_paces ? recorder->next_frame_time() + 0.001
                            : replay ? replay->next_frame_time() + 0.001 : next_tick;
        const double left = target - now();
        if (left > 0 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -LONGLONG(left * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        }
        next_tick = std::max(next_tick + 1.0 / fps, now() - 0.05);
        const double t0 = now();

        // Sahneler: yayındaki (program) ve düzenlenen. Stüdyo modu kapalıyken ikisi aynıdır.
        auto find_scene = [&](const std::string& id) -> const json* {
            for (auto& s : doc["scenes"])
                if (s.value("id", std::string()) == id) return &s;
            return nullptr;
        };
        const bool studio = doc.value("studio", false);
        const std::string current = doc.value("current", std::string());
        const json* edit = find_scene(current);
        if (!edit && !doc["scenes"].empty()) edit = &doc["scenes"][0];
        if (!edit) continue;
        const json* scene = studio ? find_scene(doc.value("program", current)) : edit;
        if (!scene) scene = edit;
        const std::string program_id = scene->value("id", std::string());
        if (m.scene_id != program_id) {
            if (!m.scene_id.empty() && rec.value("transition", std::string("fade")) == "fade") {
                m.gfx.ctx->CopyResource(m.previous.Get(), m.canvas.Get());
                m.fade_start = now();
            }
            m.scene_id = program_id;
        }
        const bool two_scenes = edit != scene;
        m.frame_no++;

        // Çizilecek bütün sahneler: yayındaki, düzenlenen ve bunların içindeki sahneler (en çok 4 kat)
        std::vector<const json*> needed;
        std::function<void(const json*, int)> collect = [&](const json* sc, int depth) {
            if (!sc || depth > 4 || std::find(needed.begin(), needed.end(), sc) != needed.end()) return;
            needed.push_back(sc);
            for (auto& cfg : (*sc)["sources"])
                if (cfg.value("type", std::string()) == "scene") collect(find_scene(cfg["settings"].value("scene", std::string())), depth + 1);
        };
        collect(scene, 0);
        if (two_scenes) collect(edit, 0);

        // Kaynakları eşitle: gereken sahnelerin kaynakları açık olur
        std::map<std::string, bool> wanted;
        auto sync = [&](const json& sc) {
            for (auto& cfg : sc["sources"]) {
                const std::string id = cfg.value("id", std::string()), type = cfg.value("type", std::string());
                if (type == "scene") continue;
                const json& s = cfg["settings"];
                std::string key;   // Bu ayarlar değişince kaynak yeniden açılır
                if (type == "screen" || type == "region") key = std::to_string(s.value("monitor", 0));
                if (type == "window") key = s.value("window", std::string()) + "|" + s.value("title", std::string()) + (s.value("cursor", true) ? "|1" : "|0") + (s.value("client", true) ? "|1" : "|0");
                if (type == "game") key = s.value("window", std::string()) + (s.value("cursor", false) ? "|1" : "|0");
                if (type == "video") key = s.value("file", std::string()) + s.value("playlist", json::array()).dump() + (s.value("shuffle", false) ? "|1" : "|0");
                if (type == "camera") key = std::to_string(s.value("device", 0));
                if (type == "browser") key = s.value("url", std::string()) + "|" + s.value("css", std::string()) + "|" + std::to_string(s.value("width", 1280)) + "x" +
                                             std::to_string(s.value("height", 720)) + (s.value("muted", false) ? "|1|" : "|0|") + std::to_string(s.value("reload", 0));
                wanted[id] = true;
                if (m.sources.count(id) && m.source_keys[id] == key) continue;
                m.source_keys[id] = key;
                if (type == "screen" || type == "region") m.sources[id] = std::make_unique<ScreenSource>(adapter_.Get(), s.value("monitor", 0), type == "region");
                else if (type == "window") m.sources[id] = std::make_unique<WindowSource>(adapter_.Get(), s);
                else if (type == "fps_meter") m.sources[id] = std::make_unique<FpsSource>();
                else if (type == "game") m.sources[id] = std::make_unique<GameSource>(adapter_.Get(), s.value("window", std::string()), s.value("cursor", false));
                else if (type == "video") {
                    std::vector<std::wstring> files;
                    for (auto& f : s.value("playlist", json::array())) if (f.is_string() && !f.get<std::string>().empty()) files.push_back(widen(f.get<std::string>()));
                    if (files.empty()) files.push_back(widen(s.value("file", std::string())));
                    m.sources[id] = std::make_unique<MediaSource>(files, -1, s.value("loop", true), s.value("shuffle", false));
                }
                else if (type == "camera") { g_camera_failed = false; m.sources[id] = std::make_unique<MediaSource>(std::vector<std::wstring>{}, s.value("device", 0), true); }
                else if (type == "slideshow") m.sources[id] = std::make_unique<SlideshowSource>();
                else if (type == "browser") {
                    BrowserOptions o;
                    o.url = s.value("url", std::string());
                    o.css = s.value("css", std::string());
                    o.width = s.value("width", 1280);
                    o.height = s.value("height", 720);
                    o.muted = s.value("muted", false);
                    m.sources[id] = std::make_unique<BrowserSource>(adapter_.Get(), o);
                }
                else m.sources[id] = std::make_unique<StillSource>();
            }
        };
        for (const json* sc : needed) sync(*sc);
        for (auto it = m.sources.begin(); it != m.sources.end();)
            if (!wanted.count(it->first)) { m.source_keys.erase(it->first); it = m.sources.erase(it); } else ++it;
        {   // Her kaynak karede bir kez güncellenir
            std::map<std::string, bool> done;
            for (const json* sc : needed)
                for (auto& cfg : (*sc)["sources"]) {
                    const std::string id = cfg.value("id", std::string());
                    auto it = m.sources.find(id);
                    if (it != m.sources.end() && !done[id]) { done[id] = true; it->second->update(m.gfx, cfg, doc, m.W, m.H); }
                }
        }
        // İç sahneler önce, içten dışa doğru kendi tuvallerine çizilir
        std::function<void(const json&, int)> render_nested = [&](const json& sc, int depth) {
            if (depth > 4) return;
            for (auto& cfg : sc["sources"]) {
                if (cfg.value("type", std::string()) != "scene" || !cfg.value("visible", true)) continue;
                const json* child = find_scene(cfg["settings"].value("scene", std::string()));
                if (!child) continue;
                auto& n = m.nested[child->value("id", std::string())];
                if (n.frame == m.frame_no) continue;
                n.frame = m.frame_no;       // Kendini içeren sahne döngüye girmesin
                render_nested(*child, depth + 1);
                D3D11_TEXTURE2D_DESC td{};
                if (n.tex) n.tex->GetDesc(&td);
                if (!n.tex || int(td.Width) != m.W || int(td.Height) != m.H) {
                    td = {};
                    td.Width = m.W; td.Height = m.H; td.MipLevels = td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    n.tex.Reset(); n.rtv.Reset(); n.srv.Reset();
                    device_->CreateTexture2D(&td, nullptr, &n.tex);
                    device_->CreateRenderTargetView(n.tex.Get(), nullptr, &n.rtv);
                    device_->CreateShaderResourceView(n.tex.Get(), nullptr, &n.srv);
                }
                draw_scene(*child, n.rtv.Get(), nullptr, false, false, true);
            }
        };
        render_nested(*scene, 0);
        if (two_scenes) render_nested(*edit, 0);

        // Çiz. Ayrı kamera kaydında kamera ana videoya çizilmez (önizlemede de görünmez), kendi dosyasına yazılır
        std::map<std::string, Rect> boxes;
        const bool separate_camera = recorder && rec.value("separate_camera", false);
        draw_scene(*scene, m.canvas_rtv.Get(), two_scenes ? nullptr : &boxes, separate_camera, true);
        if (active_) {
            if (two_scenes) {
                draw_scene(*edit, m.edit_rtv.Get(), &boxes, false, false);
                m.gfx.ctx->CopyResource(m.preview.Get(), m.edit_canvas.Get());
            } else {
                m.gfx.ctx->CopyResource(m.preview.Get(), m.canvas.Get());
            }
            if (studio && m.program_preview) m.gfx.ctx->CopyResource(m.program_preview.Get(), m.canvas.Get());
        }
        if (shot_at && now() >= shot_at) {
            shot_at = 0;
            take_screenshot();
        }
        if (recorder) {
            if (!recorder->paused() && (rec.value("record_cursor", true) || rec.value("record_keys", false)))
                log_input(m, *scene, rec, recorder->elapsed());
            if (separate_camera && !recorder->paused()) {
                for (auto& cfg : (*scene)["sources"]) {
                    if (cfg.value("type", std::string()) != "camera" || !cfg.value("visible", true)) continue;
                    auto it = m.sources.find(cfg.value("id", std::string()));
                    Rect area;
                    int cw = 0, ch = 0;
                    ID3D11Texture2D* frame = it == m.sources.end() ? nullptr : it->second->camera_frame(cfg, m.W, m.H, area, cw, ch);
                    if (!frame) break;
                    if (!m.camera_rec && m.camera_path.empty() && !(cw & 1) && !(ch & 1)) {     // İlk kare gelince açılır; başlangıç farkı not edilir
                        auto cam = std::make_unique<Recorder>();
                        std::string error;
                        const std::wstring path = companion_path(recorder->path(), L" - camera.mp4");
                        if (cam->open(device_.Get(), path, cw, ch, rec.value("fps", 60), "high", 128, false, error)) {
                            m.camera_rec = std::move(cam);
                            m.camera_path = path;
                            m.camera_offset = recorder->elapsed();
                            m.camera_area = area;
                            m.camera_mirror = cfg["settings"].value("mirror", false);
                        } else {
                            m.camera_path = L"-";       // Bir daha denenmez
                        }
                    }
                    if (m.camera_rec) m.camera_rec->write_video(m.gfx.ctx.Get(), frame);
                    break;
                }
            }
            recorder->write_video(m.gfx.ctx.Get(), m.canvas.Get());
        }
        if (replay) replay->write_video(m.gfx.ctx.Get(), m.canvas.Get());
        m.vcam.write(device_.Get(), m.gfx.ctx.Get(), m.canvas_srv.Get());
        if (stream) {
            if (m.stream_frame) {       // Yayın çözünürlüğüne ekran kartında küçült
                m.gfx.begin(m.stream_rtv.Get(), m.stream_w, m.stream_h);
                m.gfx.quad(m.canvas_srv.Get(), {0, 0, float(m.stream_w), float(m.stream_h)}, m.stream_w, m.stream_h, 1.f, true);
                ID3D11RenderTargetView* none = nullptr;
                m.gfx.ctx->OMSetRenderTargets(1, &none, nullptr);
                ID3D11ShaderResourceView* no_srv = nullptr;
                m.gfx.ctx->PSSetShaderResources(0, 1, &no_srv);
            }
            stream->write_video(m.gfx.ctx.Get(), m.stream_frame ? m.stream_frame.Get() : m.canvas.Get());
            for (auto& e : m.extra_streams) e->write_video(m.gfx.ctx.Get(), m.stream_frame ? m.stream_frame.Get() : m.canvas.Get());
        }
        m.gfx.ctx->Flush();

        ms = ms * 0.95 + (now() - t0) * 1000 * 0.05;
        m.fps_count++;
        if (!m.fps_since) m.fps_since = now();
        if (now() - m.fps_since >= 1) {
            m.render_fps = m.fps_count / (now() - m.fps_since);
            m.fps_count = 0;
            m.fps_since = now();
        }
        std::lock_guard<std::mutex> g(lock_);
        boxes_ = std::move(boxes);
        status_.recording = recorder != nullptr;
        status_.paused = recorder && recorder->paused();
        status_.elapsed = recorder ? m.record_base + recorder->elapsed() : 0;
        status_.frames = recorder ? recorder->frames() : 0;
        status_.repeated = recorder ? recorder->frames() - recorder->unique() + recorder->dropped() : 0;
        status_.frame_ms = ms;
        status_.render_fps = m.render_fps;
        status_.record_part = recorder ? m.record_part : 0;
        status_.record_bytes = recorder ? m.record_bytes : 0;
        status_.record_kbps = recorder ? m.record_kbps : 0;
        status_.camera_failed = g_camera_failed;
        status_.game_fallback = g_game_fallback;
        status_.streaming = stream != nullptr;
        if (stream) {
            const StreamStats ss = stream->stats();
            status_.stream_state = int(ss.state);
            status_.stream_elapsed = stream->elapsed();
            status_.stream_kbps = ss.kbps;
            status_.stream_dropped = ss.dropped + stream->encoder_dropped();
        } else {
            status_.stream_state = 0;
        }
        status_.stream_targets.clear();
        for (size_t i = 0; i < m.extra_streams.size(); i++) {
            const StreamStats ss = m.extra_streams[i]->stats();
            status_.stream_targets.push_back({m.extra_names[i], int(ss.state), ss.kbps, ss.error});
        }
        for (auto& f : m.extra_failed) status_.stream_targets.push_back(f);
        status_.replay_on = replay != nullptr;
        status_.vcam_state = m.vcam.running() ? (m.vcam.in_use() ? 2 : 1) : 0;
        status_.replay_seconds = replay ? int(std::min(double(m.replay_seconds), now() - m.replay_started)) : 0;
        status_.level_desktop = m.mixer.level(0);
        status_.level_mic = m.mixer.level(1);
        status_.level_mic_input = m.mixer.mic_input_level();
        for (int i = 0; i < 8; i++) status_.level_inputs[i] = m.mixer.level(i + 2);
    }
    m.mixer.set_sink(nullptr);
    m.mixer.set_track_sink(nullptr);
    m.mixer.set_stream_sink(nullptr);
    for (auto& e : m.extra_streams) e->stop();
    m.extra_streams.clear();
    if (m.stream) { m.stream->stop(); m.stream.reset(); }
    if (m.recorder) close_recording(false);
    stop_replay();
    m.vcam.stop();
    if (m.replay_saver.joinable()) m.replay_saver.join();
    m.mixer.stop();
    m.sources.clear();
    if (shot_thread_.joinable()) shot_thread_.join();
    if (timer) CloseHandle(timer);
    timeEndPeriod(1);
    MFShutdown();
}
