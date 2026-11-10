// Resim yükleme (WIC) ve Direct2D/DirectWrite ile çizim. Çıktılar önceden çarpılmış BGRA.
#include "common.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>

const Platform kPlatforms[8] = {
    {"youtube", "YouTube", 0xff0000}, {"twitch", "Twitch", 0x9146ff}, {"kick", "Kick", 0x53fc18},
    {"instagram", "Instagram", 0xe4405f}, {"tiktok", "TikTok", 0xffffff}, {"x", "X", 0xffffff},
    {"facebook", "Facebook", 0x0866ff}, {"discord", "Discord", 0x5865f2},
};

namespace {

// Paylaşılan fabrikalar: ilk kullanımda oluşturulur, çok iş parçacıklı güvenli
struct Factories {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    Factories() {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, d2d.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    }
};

Factories& factories() {
    static Factories f;
    return f;
}

D2D1_COLOR_F d2d_color(uint32_t rgb, float a = 1) {
    return D2D1::ColorF(rgb, a);
}

// WIC bitmap üzerine Direct2D ile çizim
class Canvas {
public:
    Canvas(int w, int h) : w_(w), h_(h) {
        factories().wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp_);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        factories().d2d->CreateWicBitmapRenderTarget(bmp_.Get(), props, &rt_);
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0));
    }
    ID2D1RenderTarget* rt() { return rt_.Get(); }
    ComPtr<ID2D1SolidColorBrush> brush(uint32_t rgb, float a = 1) {
        ComPtr<ID2D1SolidColorBrush> b;
        rt_->CreateSolidColorBrush(d2d_color(rgb, a), &b);
        return b;
    }
    Bitmap finish() {
        rt_->EndDraw();
        Bitmap out;
        out.w = w_; out.h = h_; out.px.resize(size_t(w_) * h_ * 4);
        WICRect r{0, 0, w_, h_};
        bmp_->CopyPixels(&r, w_ * 4, UINT(out.px.size()), out.px.data());
        return out;
    }

private:
    int w_, h_;
    ComPtr<IWICBitmap> bmp_;
    ComPtr<ID2D1RenderTarget> rt_;
};

ComPtr<IDWriteTextLayout> make_layout(const std::wstring& text, const std::string& font, bool bold, float size) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    factories().dwrite->CreateTextFormat(widen(font).c_str(), nullptr,
                                         bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"tr-TR", &format);
    if (format)
        factories().dwrite->CreateTextLayout(text.c_str(), UINT32(text.size()), format.Get(), 8000, 8000, &layout);
    return layout;
}

// Çizgili yazı: önce kaydırılmış kopyalar (çizgi), sonra asıl renk
void draw_outlined(Canvas& c, IDWriteTextLayout* layout, float x, float y, uint32_t color, int stroke, uint32_t line) {
    if (stroke) {
        auto b = c.brush(line);
        for (int dx = -stroke; dx <= stroke; dx++)
            for (int dy = -stroke; dy <= stroke; dy++)
                if ((dx || dy) && dx * dx + dy * dy <= stroke * stroke + 1)
                    c.rt()->DrawTextLayout(D2D1::Point2F(x + dx, y + dy), layout, b.Get());
    }
    c.rt()->DrawTextLayout(D2D1::Point2F(x, y), layout, c.brush(color).Get());
}

}  // namespace

uint32_t parse_color(const std::string& hex) {
    if (hex.size() < 7) return 0xffffff;
    return uint32_t(strtoul(hex.c_str() + 1, nullptr, 16));
}

bool load_image(const std::wstring& path, Bitmap& out) {
    auto wic = factories().wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> convert;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&convert)))
        return false;
    if (FAILED(convert->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        return false;
    UINT w, h;
    convert->GetSize(&w, &h);
    out.w = int(w); out.h = int(h); out.px.resize(size_t(w) * h * 4);
    return SUCCEEDED(convert->CopyPixels(nullptr, w * 4, UINT(out.px.size()), out.px.data()));
}

bool save_png(const Bitmap& b, const std::wstring& path) {
    // Dosyada saydamlık çarpılmamış durur
    std::vector<uint8_t> straight(b.px);
    for (size_t i = 0; i + 3 < straight.size(); i += 4) {
        const int a = straight[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; c++) straight[i + c] = uint8_t(std::min(255, straight[i + c] * 255 / a));
    }
    auto wic = factories().wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(b.w, b.h)))
        return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    return SUCCEEDED(frame->WritePixels(b.h, b.w * 4, UINT(straight.size()), straight.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

Bitmap draw_text(const json& s, float k) {
    const std::wstring text = widen(s.value("text", std::string("Metin")));
    const float size = std::max(6.f, s.value("font_size", 48) * k);
    auto layout = make_layout(text.empty() ? L" " : text, s.value("font", std::string("Segoe UI")), s.value("bold", true), size);
    if (!layout) return {};
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    const int stroke = s.value("outline", true) ? std::max(1, int(s.value("outline_thickness", 3) * k)) : 0;
    const bool background = s.value("background", false);
    const float pad = background ? size * 0.35f : 0;
    const int w = int(ceil(m.widthIncludingTrailingWhitespace + 2 * (pad + stroke))) + 1;
    const int h = int(ceil(m.height + 2 * (pad + stroke))) + 1;
    Canvas c(w, h);
    if (background) {
        auto b = c.brush(parse_color(s.value("bg_color", std::string("#000000"))), s.value("bg_opacity", 60) / 100.f);
        c.rt()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(w), float(h)), pad, pad), b.Get());
    }
    // Daktilo: yalnızca ilk harfler görünür, yazının yeri ve boyutu değişmez
    const int visible = s.value("visible_chars", -1);
    auto hidden = c.brush(0, 0);
    if (visible >= 0 && visible < int(text.size()))
        layout->SetDrawingEffect(hidden.Get(), DWRITE_TEXT_RANGE{UINT32(visible), UINT32(text.size() - visible)});
    draw_outlined(c, layout.Get(), pad + stroke, pad + stroke, parse_color(s.value("color", std::string("#ffffff"))), stroke,
                  parse_color(s.value("outline_color", std::string("#000000"))));
    return c.finish();
}

// Açılış ekranı: gölgeli koyu kart, simge, ad, durum ve ilerleme çubuğu
Bitmap draw_splash(const SplashInfo& info, float k, float progress, const std::string& status) {
    const float margin = 24 * k, cw = 560 * k, ch = 320 * k, r = 18 * k;
    const int W = int(cw + 2 * margin), H = int(ch + 2 * margin);
    Canvas c(W, H);
    auto* rt = c.rt();
    for (int i = 10; i >= 1; i--) {     // Yumuşak gölge
        const float g = i * 2.2f * k;
        auto b = c.brush(0x000000, 0.035f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(margin - g, margin - g + 6 * k, margin + cw + g, margin + ch + g + 6 * k), r + g, r + g), b.Get());
    }
    const D2D1_RECT_F card = D2D1::RectF(margin, margin, margin + cw, margin + ch);
    D2D1_GRADIENT_STOP stops[2] = {{0, D2D1::ColorF(0x1d2030)}, {1, D2D1::ColorF(0x10111a)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    rt->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1LinearGradientBrush> fill;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(card.left, card.top), D2D1::Point2F(card.right, card.bottom)), collection.Get(), &fill);
    rt->FillRoundedRectangle(D2D1::RoundedRect(card, r, r), fill.Get());
    // Köşede vurgu renginde ışıltı
    D2D1_GRADIENT_STOP glow_stops[2] = {{0, D2D1::ColorF(info.accent, 0.38f)}, {1, D2D1::ColorF(info.accent, 0.f)}};
    ComPtr<ID2D1GradientStopCollection> glow_collection;
    rt->CreateGradientStopCollection(glow_stops, 2, &glow_collection);
    ComPtr<ID2D1RadialGradientBrush> glow;
    rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(card.right - 90 * k, card.top + 40 * k), D2D1::Point2F(0, 0), 300 * k, 220 * k),
                                  glow_collection.Get(), &glow);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    factories().d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card, r, r), &clip);
    rt->FillGeometry(clip.Get(), glow.Get());
    rt->DrawRoundedRectangle(D2D1::RoundedRect(card, r, r), c.brush(0xffffff, 0.08f).Get(), 1.f * k);

    // Simge: vurgu renginde yuvarlak kare içinde
    const float ix = margin + 40 * k, iy = margin + 44 * k, is = 76 * k;
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix, iy, ix + is, iy + is), 18 * k, 18 * k), c.brush(info.accent).Get());
    ComPtr<IDWriteTextFormat> icon_format;
    factories().dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40 * k, L"", &icon_format);
    if (icon_format) {
        icon_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        icon_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt->DrawTextW(info.glyph.c_str(), UINT32(info.glyph.size()), icon_format.Get(), D2D1::RectF(ix, iy, ix + is, iy + is), c.brush(0xffffff).Get());
    }
    auto text = [&](const std::string& s, float size, bool bold, uint32_t rgb, float alpha, float x, float y) {
        auto layout = make_layout(widen(s), "Segoe UI", bold, size * k);
        if (layout) rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), c.brush(rgb, alpha).Get());
    };
    text(info.name, 32, true, 0xffffff, 1.f, margin + 40 * k, margin + 140 * k);
    text(info.tagline, 15, false, 0xc8cbe0, 0.9f, margin + 42 * k, margin + 186 * k);
    text(info.version, 12, false, 0xc8cbe0, 0.6f, margin + 42 * k, margin + 210 * k);

    // Durum ve ilerleme
    text(status, 12.5f, false, 0xc8cbe0, 0.8f, margin + 42 * k, margin + 254 * k);
    const float bx = margin + 42 * k, by = margin + 280 * k, bw = cw - 84 * k, bh = 4 * k;
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw, by + bh), bh / 2, bh / 2), c.brush(0xffffff, 0.12f).Get());
    const float p = std::clamp(progress, 0.f, 1.f);
    if (p > 0) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + bw * p, by + bh), bh / 2, bh / 2), c.brush(info.accent).Get());
    // Sağ altta not
    auto note = make_layout(widen(info.note), "Segoe UI", false, 11.5f * k);
    if (note) {
        DWRITE_TEXT_METRICS m{};
        note->GetMetrics(&m);
        rt->DrawTextLayout(D2D1::Point2F(margin + cw - 42 * k - m.width, margin + 254 * k), note.Get(), c.brush(0xc8cbe0, 0.55f).Get());
    }
    return c.finish();
}

Bitmap draw_shape(const json& s, float k) {
    const std::string shape = s.value("shape", std::string("rect"));
    const float stroke = std::max(0.f, s.value("stroke_width", 4) * k), pad = stroke + 4 * k + 2;
    const float W = std::max(8.f, s.value("shape_w", 400) * k), H = std::max(8.f, s.value("shape_h", 200) * k);
    const bool bubble = shape == "bubble";
    const float tail = bubble ? H * 0.28f : 0;
    Canvas c(int(ceil(W + 2 * pad)), int(ceil(H + tail + 2 * pad)));
    auto* rt = c.rt();
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    auto fill = c.brush(parse_color(s.value("fill", std::string("#0d6efd"))), s.value("fill_opacity", 100) / 100.f);
    auto line = c.brush(parse_color(s.value("stroke", std::string("#ffffff"))));
    const D2D1_RECT_F box = D2D1::RectF(pad, pad, pad + W, pad + H);
    const float radius = std::min({float(s.value("radius", 12)) * k, W / 2, H / 2});
    auto triangle = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F d, ID2D1Brush* brush) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factories().d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(b);
        sink->AddLine(d);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        rt->FillGeometry(path.Get(), brush);
    };
    if (shape == "ellipse" || shape == "badge") {
        const auto e = D2D1::Ellipse(D2D1::Point2F(pad + W / 2, pad + H / 2), W / 2, H / 2);
        rt->FillEllipse(e, fill.Get());
        if (stroke > 0) rt->DrawEllipse(e, line.Get(), stroke);
    } else if (shape == "arrow" || shape == "line") {
        const float y = pad + H / 2, thick = std::max(2.f, stroke), head = shape == "arrow" ? std::min(H, W / 2) : 0;
        rt->FillRectangle(D2D1::RectF(pad, y - thick / 2, pad + W - head * 0.9f, y + thick / 2), fill.Get());
        if (head > 0) triangle(D2D1::Point2F(pad + W - head, pad), D2D1::Point2F(pad + W, y), D2D1::Point2F(pad + W - head, pad + H), fill.Get());
    } else {
        const auto r = D2D1::RoundedRect(box, radius, radius);
        if (bubble) triangle(D2D1::Point2F(pad + W * 0.2f, pad + H - 1), D2D1::Point2F(pad + W * 0.2f, pad + H + tail), D2D1::Point2F(pad + W * 0.42f, pad + H - 1), fill.Get());
        rt->FillRoundedRectangle(r, fill.Get());
        if (stroke > 0) rt->DrawRoundedRectangle(r, line.Get(), stroke);
    }
    const std::string text = s.value("text", std::string());
    if (!text.empty()) {
        if (auto layout = make_layout(widen(text), s.value("font", std::string("Segoe UI")), s.value("bold", true), std::max(6.f, s.value("font_size", 40) * k))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            draw_outlined(c, layout.Get(), pad + (W - m.width) / 2, pad + (H - m.height) / 2, parse_color(s.value("color", std::string("#ffffff"))), 0, 0);
        }
    }
    return c.finish();
}

Bitmap draw_crosshair(const json& s) {
    const int kSize = 201, c0 = kSize / 2;
    Canvas c(kSize, kSize);
    c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    const int length = s.value("length", 10), gap = s.value("gap", 4), thick = s.value("thickness", 2);
    const bool separate = s.value("arms_separate", false);
    const bool t_shape = s.value("t_shape", false) && !separate;
    const int inner_group = s.value("rotate", 0), outer_group = s.value("outer_rotate", 0);
    const float pivot = c0 + 0.5f;      // Ekranın tam ortasındaki pikselin merkezi
    // group: çizgi takımı birlikte döner, orbit: çizgi merkez etrafında döner, spin: çizgi kendi ortası etrafında döner
    struct Rect { float x0, y0, x1, y1; int group = 0, orbit = 0, spin = 0; };
    std::vector<Rect> rects;
    enum Side { kUp, kDown, kLeft, kRight };
    auto arm = [&](Side side, int len, int th, int g, int group, int orbit, int spin) {
        if (len <= 0) return;
        // Dönen çizgi merkezden tam eşit uzakta olsun; dönmeyen çizgi eskisi gibi piksele otursun
        const float t0 = group || orbit || spin ? 0.5f - th / 2.f : float(-(th / 2)), t1 = t0 + th;
        const float near0 = float(g + 1), far0 = float(g + 1 + len), near1 = float(-g), far1 = float(-g - len);
        if (side == kUp) rects.push_back({t0, far1, t1, near1, group, orbit, spin});
        if (side == kDown) rects.push_back({t0, near0, t1, far0, group, orbit, spin});
        if (side == kLeft) rects.push_back({far1, t0, near1, t1, group, orbit, spin});
        if (side == kRight) rects.push_back({near0, t0, far0, t1, group, orbit, spin});
    };
    const std::pair<Side, std::string> sides[] = {{kUp, "arm_up"}, {kDown, "arm_down"}, {kLeft, "arm_left"}, {kRight, "arm_right"}};
    const bool outer = s.value("outer", false);
    for (auto& [side, key] : sides) {
        const int orbit = s.value(key + "_angle", 0), spin = s.value(key + "_spin", 0);
        if (separate)
            arm(side, s.value(key + "_length", 10), s.value(key + "_thickness", 2), s.value(key + "_gap", 4), inner_group, orbit, spin);
        else if (!t_shape || side != kUp)
            arm(side, length, thick, gap, inner_group, orbit, spin);
        if (outer && (!t_shape || side != kUp))
            arm(side, s.value("outer_length", 4), s.value("outer_thickness", 2), s.value("outer_gap", 18), outer_group,
                s.value("outer_" + key + "_angle", 0), s.value("outer_" + key + "_spin", 0));
    }

    auto pass = [&](uint32_t color, int grow) {
        auto b = c.brush(color);
        auto fill = [&](const Rect& r) {
            const D2D1_POINT_2F middle = D2D1::Point2F(c0 + (r.x0 + r.x1) / 2, c0 + (r.y0 + r.y1) / 2);
            c.rt()->SetTransform(D2D1::Matrix3x2F::Rotation(float(r.spin), middle) *
                                 D2D1::Matrix3x2F::Rotation(float(r.group + r.orbit), D2D1::Point2F(pivot, pivot)));
            // Dik açılar keskin kalır, eğik çizgilerin kenarı yumuşatılır
            const bool slanted = (r.group + r.orbit + r.spin) % 90 != 0;
            c.rt()->SetAntialiasMode(slanted ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
            c.rt()->FillRectangle(D2D1::RectF(c0 + r.x0 - grow, c0 + r.y0 - grow, c0 + r.x1 + grow, c0 + r.y1 + grow), b.Get());
            c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            c.rt()->SetTransform(D2D1::Matrix3x2F::Identity());
        };
        for (auto& r : rects) fill(r);
        if (s.value("dot", true)) {
            const int d = s.value("dot_size", 2), d0 = -(d / 2);
            if (s.value("dot_shape", std::string("square")) == "round") {
                c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                const float r = d / 2.f + grow, cc = c0 + d0 + d / 2.f;
                c.rt()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cc, cc), r, r), b.Get());
                c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            } else {
                const float e0 = inner_group ? 0.5f - d / 2.f : float(d0);
                fill({e0, e0, e0 + d, e0 + d, inner_group});
            }
        }
        if (s.value("ring", false)) {
            c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float r = float(s.value("ring_radius", 12));
            const float cc = c0 + (-(thick / 2) + thick / 2.f);
            c.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cc, cc), r, r), b.Get(), float(s.value("ring_thickness", 1) + 2 * grow));
            c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        }
    };
    if (s.value("outline", false))
        pass(parse_color(s.value("outline_color", std::string("#000000"))), s.value("outline_thickness", 1));
    pass(parse_color(s.value("color", std::string("#00ff00"))), 0);
    return c.finish();
}

Bitmap draw_keys(const std::vector<KeyCap>& caps, float unit, uint32_t text, uint32_t active, float bg_alpha) {
    float right = 0, bottom = 0;
    for (auto& c : caps) { right = std::max(right, c.x + c.w); bottom = std::max(bottom, c.y + c.h); }
    if (caps.empty() || unit < 4) return {};
    Canvas c(int(ceilf(right * unit)) + 2, int(ceilf(bottom * unit)) + 2);
    c.rt()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float gap = std::max(2.f, unit * 0.08f), round = unit * 0.16f;
    auto idle = c.brush(0x101010, bg_alpha), lit = c.brush(active, 0.92f), edge = c.brush(0xffffff, 0.35f), ink = c.brush(text);
    for (auto& k : caps) {
        const D2D1_RECT_F r = D2D1::RectF(1 + k.x * unit, 1 + k.y * unit, 1 + (k.x + k.w) * unit - gap, 1 + (k.y + k.h) * unit - gap);
        c.rt()->FillRoundedRectangle(D2D1::RoundedRect(r, round, round), k.down ? lit.Get() : idle.Get());
        c.rt()->DrawRoundedRectangle(D2D1::RoundedRect(r, round, round), edge.Get(), 1.f);
        const std::wstring label = widen(k.label);
        if (auto layout = make_layout(label, "Segoe UI", true, unit * (label.size() > 2 ? 0.27f : 0.38f))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            c.rt()->DrawTextLayout(D2D1::Point2F((r.left + r.right - m.width) / 2, (r.top + r.bottom - m.height) / 2), layout.Get(),
                                   k.down ? c.brush(0x000000).Get() : ink.Get());
        }
    }
    return c.finish();
}

Bitmap draw_fps(const json& s, const std::vector<FpsRow>& rows, float k, const std::vector<float>& graph) {
    if (rows.empty()) return {};
    const std::string font = s.value("font", std::string("Consolas"));
    const bool bold = s.value("bold", true);
    const float size = std::max(6.f, roundf(s.value("font_size", 18) * k));
    const float unit_size = std::max(5.f, roundf(size * 0.68f));     // Birim yazısı değerden küçük
    auto layout = [&](const std::string& text, char kind) {
        return make_layout(widen(text), font, kind == 'l' ? true : bold, kind == 'u' ? unit_size : size);
    };
    std::map<std::string, float> widths;
    auto measure = [&](const std::string& text, char kind) {
        const std::string key = kind + text;
        auto it = widths.find(key);
        if (it != widths.end()) return it->second;
        DWRITE_TEXT_METRICS m{};
        if (auto l = layout(text, kind)) l->GetMetrics(&m);
        return widths[key] = m.widthIncludingTrailingWhitespace;
    };
    std::map<char, float> baseline;
    float line_h = size, ascent = size;
    for (char kind : {'l', 'v', 'u'}) {
        DWRITE_LINE_METRICS lm{};
        UINT32 count = 0;
        if (auto l = layout("0", kind)) l->GetLineMetrics(&lm, 1, &count);
        baseline[kind] = lm.baseline;
        if (kind == 'v') { line_h = lm.height; ascent = lm.baseline; }
    }
    const uint32_t label_color = parse_color(s.value("label_color", std::string("#ff9f1c")));

    auto part_width = [&](const FpsPart& p, const std::string& tmpl) {
        const float w = measure(p.text, p.kind);
        return p.kind == 'v' ? std::max(w, measure(tmpl, 'v')) : w;
    };
    auto cell_width = [&](const FpsCell& c) {
        float w = 0;
        for (auto& p : c.parts) w += part_width(p, c.tmpl);
        return w;
    };
    struct Item { float x, y; std::string text; char kind; uint32_t color; };
    std::vector<Item> items;
    auto put = [&](const FpsCell& c, float x, float y) {
        for (auto& p : c.parts) {
            const float w = part_width(p, c.tmpl);
            items.push_back({x + w - measure(p.text, p.kind), y, p.text, p.kind, p.color});   // Şablon içinde sağa yaslı
            x += w;
        }
        return x;
    };

    const float gap = measure("  ", 'v');
    float width = 0, height = line_h;
    if (s.value("layout", std::string("vertical")) == "horizontal") {
        float x = 0;
        for (auto& r : rows) {
            if (x) x += gap;
            if (!r.label.empty()) {
                items.push_back({x, ascent, r.label, 'l', label_color});
                x += measure(r.label, 'l') + gap / 2;
            }
            for (auto& [column, c] : r.cells) x = put(c, x, ascent) + gap / 2;
        }
        width = x - gap / 2;
    } else {
        // Sütun genişlikleri hizalı satırlardaki en geniş hücreye göre
        float label_w = 0;
        for (auto& r : rows) label_w = std::max(label_w, r.label.empty() ? 0.f : measure(r.label, 'l'));
        const float start = label_w ? label_w + gap : 0;
        std::map<int, float> column_w, left;
        for (auto& r : rows)
            if (r.align)
                for (auto& [column, c] : r.cells) column_w[column] = std::max(column_w[column], cell_width(c));
        float x = start;
        for (auto& [column, w] : column_w) {
            left[column] = x;
            x += w + gap;
        }
        for (size_t i = 0; i < rows.size(); i++) {
            const FpsRow& r = rows[i];
            const float y = i * line_h + ascent;
            if (!r.label.empty()) items.push_back({0, y, r.label, 'l', label_color});
            x = start;
            for (auto& [column, c] : r.cells) {
                const float w = cell_width(c);
                if (r.align) {
                    x = left[column] + column_w[column];
                    put(c, x - w, y);       // Sütunda sağa yaslı
                } else {
                    x = put(c, x, y);
                }
                width = std::max(width, x);
                x += gap;
            }
        }
        height = rows.size() * line_h;
    }

    // Kare süresi grafiği: yazıların altında
    const bool show_graph = s.value("show_graph", false) && graph.size() > 1;
    const float text_h = height, graph_h = show_graph ? roundf(line_h * 2.5f) : 0, graph_gap = show_graph ? roundf(line_h * 0.3f) : 0;
    if (show_graph) {
        width = std::max(width, roundf(size * 10));
        height += graph_gap + graph_h;
    }
    const int stroke = s.value("outline", true) ? std::max(1, int(lround(s.value("outline_thickness", 1) * k))) : 0;
    const bool background = s.value("background", false);
    const float edge = (background ? roundf(s.value("padding", 6) * k) : 0) + stroke;
    const int w = int(ceil(width + 2 * edge)), h = int(ceil(height + 2 * edge));
    Canvas c(std::max(w, 1), std::max(h, 1));
    if (background)
        c.rt()->FillRectangle(D2D1::RectF(0, 0, float(w), float(h)), c.brush(parse_color(s.value("bg_color", std::string("#000000")))).Get());
    // Önce tüm kenarlıklar, sonra asıl yazılar üstüne: komşu yazının kenarlığı rakamı örtmesin
    const uint32_t line = parse_color(s.value("outline_color", std::string("#000000")));
    std::vector<ComPtr<IDWriteTextLayout>> layouts;
    for (auto& i : items) {
        layouts.push_back(layout(i.text, i.kind));
        if (stroke && layouts.back()) {
            auto b = c.brush(line);
            const float tx = edge + i.x, ty = edge + i.y - baseline[i.kind];
            for (int dx = -stroke; dx <= stroke; dx++)
                for (int dy = -stroke; dy <= stroke; dy++)
                    if (dx || dy) c.rt()->DrawTextLayout(D2D1::Point2F(tx + dx, ty + dy), layouts.back().Get(), b.Get());
        }
    }
    for (size_t n = 0; n < items.size(); n++)
        if (layouts[n])
            c.rt()->DrawTextLayout(D2D1::Point2F(edge + items[n].x, edge + items[n].y - baseline[items[n].kind]), layouts[n].Get(),
                                   c.brush(items[n].color).Get());
    if (show_graph) {   // Her çubuk bir kare süresi; ortancanın iki katını aşan takılmalar kritik renkte
        std::vector<float> sorted(graph);
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const float median = sorted[sorted.size() / 2];
        const float top = std::max(*std::max_element(graph.begin(), graph.end()), median * 2.f) * 1.1f;
        const float x0 = edge, y0 = edge + text_h + graph_gap, gw = width, bar = gw / graph.size();
        const uint32_t normal = parse_color(s.value("color", std::string("#ffffff"))), spike = parse_color(s.value("crit_color", std::string("#ff4040")));
        auto normal_brush = c.brush(normal, 0.85f), spike_brush = c.brush(spike), line_brush = c.brush(normal, 0.35f);
        for (size_t i = 0; i < graph.size(); i++) {
            const float h = std::max(1.f, graph[i] / top * graph_h);
            c.rt()->FillRectangle(D2D1::RectF(x0 + i * bar, y0 + graph_h - h, x0 + (i + 1) * bar - (bar > 3 ? 1 : 0), y0 + graph_h),
                                  graph[i] > median * 2 ? spike_brush.Get() : normal_brush.Get());
        }
        c.rt()->DrawLine(D2D1::Point2F(x0, y0 + graph_h), D2D1::Point2F(x0 + gw, y0 + graph_h), line_brush.Get(), 1.f);
    }
    return c.finish();
}

// Beyaz simge dosyasından boyutlandırılmış saydamlık maskesi
static std::vector<uint8_t> icon_mask(const std::string& slug, int size) {
    std::vector<uint8_t> mask(size_t(size) * size, 0);
    auto wic = factories().wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> convert;
    const std::wstring path = exe_dir() + L"\\icons\\" + widen(slug) + L".png";
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return mask;
    decoder->GetFrame(0, &frame);
    wic->CreateBitmapScaler(&scaler);
    scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant);
    wic->CreateFormatConverter(&convert);
    convert->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    std::vector<uint8_t> px(size_t(size) * size * 4);
    convert->CopyPixels(nullptr, size * 4, UINT(px.size()), px.data());
    for (size_t i = 0; i < mask.size(); i++) mask[i] = px[i * 4 + 3];
    return mask;
}

// Simgenin içindeki boşluklar (kenardan ulaşılamayan saydam pikseller)
static std::vector<uint8_t> icon_holes(const std::vector<uint8_t>& mask, int size) {
    std::vector<uint8_t> outside(mask.size(), 0);
    std::vector<int> stack;
    auto push = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= size || y >= size) return;
        const int i = y * size + x;
        if (!outside[i] && mask[i] < 128) { outside[i] = 1; stack.push_back(i); }
    };
    for (int i = 0; i < size; i++) { push(i, 0); push(i, size - 1); push(0, i); push(size - 1, i); }
    while (!stack.empty()) {
        const int i = stack.back(); stack.pop_back();
        push(i % size + 1, i / size); push(i % size - 1, i / size);
        push(i % size, i / size + 1); push(i % size, i / size - 1);
    }
    std::vector<uint8_t> holes(mask.size(), 0);
    for (size_t i = 0; i < mask.size(); i++) holes[i] = (!outside[i] && mask[i] < 128) ? 255 : 0;
    return holes;
}

Bitmap make_icon(const std::string& slug, int size, bool brand, uint32_t rgb) {
    Bitmap out;
    out.w = out.h = size;
    out.px.assign(size_t(size) * size * 4, 0);
    const auto mask = icon_mask(slug, size);

    auto over = [&](int i, uint32_t color, int alpha) {  // Önceden çarpılmış "üstüne çiz"
        uint8_t* p = &out.px[size_t(i) * 4];
        const int inv = 255 - alpha;
        const int comp[3] = {int(color & 255), int((color >> 8) & 255), int((color >> 16) & 255)};
        for (int c = 0; c < 3; c++) p[c] = uint8_t(comp[c] * alpha / 255 + p[c] * inv / 255);
        p[3] = uint8_t(alpha + p[3] * inv / 255);
    };

    if (brand && slug == "tiktok") {  // Kayık camgöbeği / pembe gölgeler
        const int shift = std::max(1, size / 28);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                const int sx = x + shift, sy = y + shift, px = x - shift, py = y - shift;
                if (sx < size && sy < size) over(y * size + x, 0x25f4ee, mask[sy * size + sx]);
                if (px >= 0 && py >= 0) over(y * size + x, 0xfe2c55, mask[py * size + px]);
                over(y * size + x, 0xffffff, mask[y * size + x]);
            }
        return out;
    }
    if (brand && (slug == "youtube" || slug == "facebook" || slug == "twitch")) {  // Gerçek logoda içi beyaz
        const auto holes = icon_holes(mask, size);
        for (size_t i = 0; i < holes.size(); i++)
            if (holes[i]) over(int(i), 0xffffff, 255);
    }
    static const uint32_t kInsta[5] = {0xfeda75, 0xfa7e1e, 0xd62976, 0x962fbf, 0x4f5bd5};
    uint32_t color = rgb;
    if (brand)
        for (auto& p : kPlatforms)
            if (slug == p.key) color = p.color;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t c = color;
            if (brand && slug == "instagram") {  // Sol alttan sağ üste renk geçişi
                const float t = float(x + size - 1 - y) / std::max(2 * (size - 1), 1) * 4;
                const int i = std::min(int(t), 3);
                const float f = t - i;
                c = 0;
                for (int s = 0; s < 24; s += 8) {
                    const int a = (kInsta[i] >> s) & 255, b = (kInsta[i + 1] >> s) & 255;
                    c |= uint32_t(a + (b - a) * f) << s;
                }
            }
            over(y * size + x, c, mask[y * size + x]);
        }
    return out;
}

static ID2D1Bitmap* to_d2d(Canvas& c, const Bitmap& b, ComPtr<ID2D1Bitmap>& out) {
    auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    c.rt()->CreateBitmap(D2D1::SizeU(b.w, b.h), b.px.data(), b.w * 4, props, &out);
    return out.Get();
}

Bitmap draw_social(const json& s, float k) {
    struct Item { std::string slug; std::wstring text; ComPtr<IDWriteTextLayout> layout; float tw, th; int w, h; };
    std::vector<Item> items;
    const float size = std::max(6.f, s.value("font_size", 26) * k);
    const std::string font = s.value("font", std::string("Segoe UI"));
    const bool bold = s.value("bold", true), background = s.value("background", true);
    const int stroke = s.value("outline", true) ? std::max(1, int(s.value("outline_thickness", 2) * k)) : 0;
    const int icon_size = int(size * 1.3f), gap = int(size * 0.45f);
    const int pad_x = background ? int(size * 0.6f) : stroke, pad_y = background ? int(size * 0.35f) : stroke;
    const auto& accounts = s["accounts"];
    for (auto& p : kPlatforms) {
        if (!accounts.contains(p.key)) continue;
        const auto& a = accounts[p.key];
        const std::string handle = a.value("handle", std::string());
        if (!a.value("show", true) || handle.empty()) continue;
        Item it{p.key, widen(handle)};
        it.layout = make_layout(it.text, font, bold, size);
        DWRITE_TEXT_METRICS m{};
        it.layout->GetMetrics(&m);
        it.tw = m.widthIncludingTrailingWhitespace; it.th = m.height;
        it.h = int(std::max(float(icon_size), it.th + 2 * stroke)) + 2 * pad_y;
        it.w = pad_x + icon_size + gap + int(ceil(it.tw)) + 2 * stroke + pad_x;
        items.push_back(std::move(it));
    }
    if (items.empty()) return {};
    const bool horizontal = s.value("layout", std::string("vertical")) == "horizontal";
    const int spacing = int(s.value("spacing", 10) * k);
    int W = 0, H = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const int sep = i ? spacing : 0;
        if (horizontal) { W += items[i].w + sep; H = std::max(H, items[i].h); }
        else { H += items[i].h + sep; W = std::max(W, items[i].w); }
    }
    Canvas c(W, H);
    const bool brand = s.value("icon_style", std::string("brand")) == "brand";
    const uint32_t text_color = parse_color(s.value("text_color", std::string("#ffffff")));
    const uint32_t line = parse_color(s.value("outline_color", std::string("#000000")));
    int x = 0, y = 0;
    for (auto& it : items) {
        if (background) {
            const float r = std::min(it.h / 2.f, s.value("bg_radius", 60) * k);
            auto b = c.brush(parse_color(s.value("bg_color", std::string("#000000"))), s.value("bg_opacity", 55) / 100.f);
            c.rt()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(float(x), float(y), float(x + it.w), float(y + it.h)), r, r), b.Get());
        }
        ComPtr<ID2D1Bitmap> icon;
        Bitmap ib = make_icon(it.slug, icon_size, brand, text_color);
        to_d2d(c, ib, icon);
        const float ix = float(x + pad_x), iy = float(y + (it.h - icon_size) / 2);
        if (icon) c.rt()->DrawBitmap(icon.Get(), D2D1::RectF(ix, iy, ix + icon_size, iy + icon_size));
        draw_outlined(c, it.layout.Get(), ix + icon_size + gap, y + (it.h - it.th) / 2, text_color, stroke, line);
        if (horizontal) x += it.w + spacing; else y += it.h + spacing;
    }
    return c.finish();
}
