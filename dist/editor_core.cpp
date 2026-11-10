#include "editor_core.h"
#include "language.h"

#include <mmdeviceapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

// --- Proje ---

static std::string make_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    char text[17];
    sprintf_s(text, "%016llx", (unsigned long long)rng());
    return text;
}

json new_project() {
    json p = {{"canvas", {{"w", 1920}, {"h", 1080}, {"fps", 60}}}, {"clips", json::array()}, {"markers", json::array()}};
    p["tracks"] = json::array();
    for (auto [id, name, kind] : {std::tuple{"v2", "Görüntü 2", "video"}, {"v1", "Görüntü 1", "video"}, {"a1", "Ses 1", "audio"}, {"a2", "Ses 2", "audio"}})
        p["tracks"].push_back({{"id", id}, {"name", name}, {"kind", kind}, {"hidden", false}, {"muted", false}, {"locked", false}, {"magnet", std::string(id) == "v1"}});
    return p;
}

json new_clip(const std::string& type, const std::string& track, double start, double length) {
    json c = {{"id", make_id()}, {"type", type}, {"track", track}, {"start", start}, {"length", length}, {"in", 0.0}, {"speed", 1.0},
              {"file", ""}, {"name", ""}, {"source_length", 0.0}, {"w", 1920}, {"h", 1080}, {"has_audio", false},
              {"volume", 100}, {"opacity", 100}, {"x", 0.0}, {"y", 0.0}, {"scale", 100.0},
              {"fade_in", 0.0}, {"fade_out", 0.0}, {"audio_fade_in", 0.0}, {"audio_fade_out", 0.0},
              {"brightness", 0.0}, {"contrast", 1.0}, {"saturation", 1.0}, {"rotate", 0.0}, {"flip_h", false}, {"flip_v", false},
              {"crop_l", 0.0}, {"crop_r", 0.0}, {"crop_t", 0.0}, {"crop_b", 0.0}, {"blur", 0.0}, {"vignette", 0.0}, {"sepia", 0.0},
              {"invert", false}, {"chroma", false}, {"chroma_color", "#00ff00"}, {"chroma_similarity", 0.25},
              {"t_in", ""}, {"t_in_d", 1.0}, {"t_out", ""}, {"t_out_d", 1.0},
              {"anim", false}, {"anim_scale", 130.0}, {"anim_x", 0.0}, {"anim_y", 0.0},
              {"denoise", 0}, {"level", 0}, {"vol_points", json::array()}, {"keys", json::array()},
              {"corner", 0}, {"border", 0}, {"border_color", "#ffffff"}, {"mask", ""}, {"mask_w", 80}, {"mask_h", 80}, {"mask_x", 0}, {"mask_y", 0}, {"feather", 0},
              {"shadow", false}, {"shadow_color", "#000000"}, {"shadow_opacity", 60}, {"shadow_blur", 24}, {"shadow_x", 10}, {"shadow_y", 14},
              {"m_in", ""}, {"m_in_d", 0.6}, {"m_out", ""}, {"m_out_d", 0.6}, {"reverse", false},
              {"voice", ""}, {"cursor_halo", false}, {"halo_color", "#ffe066"}, {"halo_size", 90}, {"halo_opacity", 45},
              {"click_rings", false}, {"ring_color", "#ff3040"}, {"keys_show", false}, {"lut", ""}, {"group", ""}};
    if (type == "spot") c.update({{"shape_w", 520}, {"shape_h", 320}, {"spot_shape", "ellipse"}, {"dark", 70}, {"feather", 60}, {"corner", 24}});
    if (type == "text") {
        c.update({{"text", "Başlık"}, {"font", "Segoe UI"}, {"font_size", 72}, {"bold", true}, {"color", "#ffffff"}, {"outline", true},
                  {"outline_color", "#000000"}, {"outline_thickness", 3}, {"background", false}, {"bg_color", "#000000"}, {"bg_opacity", 60}});
    }
    if (type == "color") c["color"] = "#1e1e2e";
    if (type == "blur") c.update({{"shape_w", 420}, {"shape_h", 220}, {"mode", "blur"}, {"strength", 20}});
    if (type == "shape") {
        c.update({{"shape", "rect"}, {"shape_w", 500}, {"shape_h", 200}, {"fill", "#0d6efd"}, {"fill_opacity", 100}, {"stroke", "#ffffff"},
                  {"stroke_width", 4}, {"radius", 12}, {"text", ""}, {"font", "Segoe UI"}, {"font_size", 40}, {"bold", true}, {"color", "#ffffff"}});
    }
    return c;
}

const std::vector<TransitionInfo>& transition_list() {
    static const std::vector<TransitionInfo> list = {
        {"fade", "Soldurma", 0, 0}, {"dissolve", "Çözülme", 7, 0}, {"wipeleft", "Sola silme", 1, 0}, {"wiperight", "Sağa silme", 2, 0},
        {"wipeup", "Yukarı silme", 3, 0}, {"wipedown", "Aşağı silme", 4, 0}, {"smoothleft", "Yumuşak silme", 1, 0},
        {"slideleft", "Sola kaydırma", 0, 1}, {"slideright", "Sağa kaydırma", 0, 2}, {"slideup", "Yukarı kaydırma", 0, 3}, {"slidedown", "Aşağı kaydırma", 0, 4},
        {"circleopen", "Daire açılış", 5, 0}, {"rectcrop", "Kutu açılış", 6, 0}, {"radial", "Saat yönünde", 8, 0}, {"zoomin", "Yakınlaştırma", 0, 5}, {"pixelize", "Pikselleştirme", 9, 0},
    };
    return list;
}

const TransitionInfo* find_transition(const std::string& key) {
    for (auto& t : transition_list())
        if (key == t.key) return &t;
    return nullptr;
}

const std::vector<std::string>& style_keys() {
    static const std::vector<std::string> keys = {
        "opacity", "scale", "x", "y", "rotate", "flip_h", "flip_v", "crop_l", "crop_r", "crop_t", "crop_b", "fade_in", "fade_out",
        "brightness", "contrast", "saturation", "blur", "vignette", "sepia", "invert", "chroma", "chroma_color", "chroma_similarity",
        "t_in", "t_in_d", "t_out", "t_out_d", "anim", "anim_scale", "anim_x", "anim_y", "keys", "volume", "audio_fade_in", "audio_fade_out", "denoise", "level",
        "corner", "border", "border_color", "mask", "mask_w", "mask_h", "mask_x", "mask_y", "feather",
        "shadow", "shadow_color", "shadow_opacity", "shadow_blur", "shadow_x", "shadow_y", "m_in", "m_in_d", "m_out", "m_out_d",
        "voice", "cursor_halo", "halo_color", "halo_size", "halo_opacity", "click_rings", "ring_color", "keys_show", "lut"};
    return keys;
}

bool clip_is_visual_still(const json& c) {
    const std::string type = c.value("type", std::string());
    return type == "text" || type == "shape" || type == "blur";
}

Bitmap render_still(const json& c) {
    return c.value("type", std::string()) == "shape" ? draw_shape(c, 1.f) : draw_text(c, 1.f);
}

// --- Giriş / çıkış hareketi ---

const std::vector<MotionInfo>& motion_list() {
    static const std::vector<MotionInfo> list = {
        {"rise", "Aşağıdan yükselerek"}, {"drop", "Yukarıdan inerek"}, {"slide", "Yandan kayarak"},
        {"pop", "Büyüyerek"}, {"wipe", "Soldan açılarak"}, {"type", "Daktilo (yalnız yazı)"},
    };
    return list;
}

int text_length(const json& c) {
    if (c.value("type", std::string()) != "text") return 0;
    return int(widen(c.value("text", std::string())).size());
}

bool clip_reversed(const json& c) {
    const std::string type = c.value("type", std::string());
    return (type == "video" || type == "audio") && c.value("reverse", false);
}

namespace {
// u: 0 başlangıç, 1 hareket bitti. sign: giriş 1, çıkış -1 (çıkış aynı yöne devam eder)
void apply_motion(Motion& m, const std::string& kind, double u, double sign, int length) {
    u = std::clamp(u, 0.0, 1.0);
    if (u >= 1) return;
    const double e = 1 - pow(1 - u, 3);
    if (kind == "rise") { m.dy += sign * 0.06 * (1 - e); m.alpha *= u; }
    else if (kind == "drop") { m.dy -= sign * 0.08 * (1 - e); m.alpha *= u; }
    else if (kind == "slide") { m.dx -= sign * 0.12 * (1 - e); m.alpha *= u; }
    else if (kind == "pop") { m.scale *= std::max(0.0, 1 + 2.70158 * pow(u - 1, 3) + 1.70158 * pow(u - 1, 2)); m.alpha *= std::min(1.0, u * 3); }
    else if (kind == "wipe") m.reveal *= e;
    else if (kind == "type" && length > 0) m.chars = std::min(m.chars < 0 ? length : m.chars, int(u * length));
}
}  // namespace

Motion clip_motion(const json& c, double local) {
    Motion m;
    const double length = c.value("length", 0.0);
    const int letters = text_length(c);
    const std::string in = c.value("m_in", std::string()), out = c.value("m_out", std::string());
    if (!in.empty()) apply_motion(m, in, local / std::max(0.05, c.value("m_in_d", 0.6)), 1, letters);
    if (!out.empty()) apply_motion(m, out, (length - local) / std::max(0.05, c.value("m_out_d", 0.6)), -1, letters);
    return m;
}

double source_time(const json& c, double local) {
    const double length = c.value("length", 0.0);
    return c.value("in", 0.0) + (clip_reversed(c) ? length - local : local) * c.value("speed", 1.0);
}

bool clip_local_of(const json& c, double source, double& local) {
    const double length = c.value("length", 0.0), offset = (source - c.value("in", 0.0)) / std::max(0.01, c.value("speed", 1.0));
    local = clip_reversed(c) ? length - offset : offset;
    return local >= 0 && local <= length;
}

// --- İmleç, tıklama, kısayol verisi ---

std::shared_ptr<const InputData> input_data(const std::string& video) {
    static std::mutex lock;
    struct Entry { std::shared_ptr<const InputData> data; double checked = 0; };
    static std::map<std::string, Entry> cache;
    if (video.empty()) return nullptr;
    std::lock_guard<std::mutex> g(lock);
    Entry& e = cache[video];
    if (e.data || now() - e.checked < 3) return e.data;     // Olmayan dosyaya üç saniyede bir yeniden bakılır
    e.checked = now();
    std::ifstream in(fs::path(widen(video) + L".input.json"), std::ios::binary);
    if (!in) in.open(fs::path(widen(video) + L".girdi.json"), std::ios::binary);      // Eski kayıtlar
    if (!in) return nullptr;
    try {
        const json j = json::parse(in);
        auto d = std::make_shared<InputData>();
        d->w = j.value("w", 0); d->h = j.value("h", 0);
        for (auto& p : j.value("cursor", json::array())) if (p.size() >= 3) d->cursor.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
        for (auto& p : j.value("clicks", json::array())) if (p.size() >= 4) d->clicks.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>(), p[3].get<float>()});
        for (auto& p : j.value("keys", json::array())) if (p.size() >= 2) d->keys.push_back({p[0].get<double>(), p[1].get<std::string>()});
        if (d->w <= 0 || d->h <= 0) return nullptr;
        e.data = d;
    } catch (...) {
        return nullptr;
    }
    return e.data;
}

bool cursor_at(const InputData& d, double t, float& x, float& y) {
    if (d.cursor.empty()) return false;
    auto it = std::lower_bound(d.cursor.begin(), d.cursor.end(), float(t), [](const std::array<float, 3>& a, float v) { return a[0] < v; });
    if (it == d.cursor.begin()) { x = it->at(1); y = it->at(2); return true; }
    if (it == d.cursor.end()) { x = d.cursor.back()[1]; y = d.cursor.back()[2]; return true; }
    const auto& b = *it;
    const auto& a = *(it - 1);
    const float u = std::clamp(float((t - a[0]) / std::max(1e-3f, b[0] - a[0])), 0.f, 1.f);
    x = a[1] + (b[1] - a[1]) * u; y = a[2] + (b[2] - a[2]) * u;
    return true;
}

bool input_point(const json& c, const InputData& d, float sx, float sy, double local, float W, float H, float& x, float& y) {
    const json view = clip_at(c, local);
    const Placement p = clip_placement(view, W, H);
    float u = sx / d.w, v = sy / d.h;
    const float cl = float(c.value("crop_l", 0.0)) / 100, cr = float(c.value("crop_r", 0.0)) / 100, ct = float(c.value("crop_t", 0.0)) / 100, cb = float(c.value("crop_b", 0.0)) / 100;
    u = (u - cl) / std::max(0.02f, 1 - cl - cr);
    v = (v - ct) / std::max(0.02f, 1 - ct - cb);
    if (c.value("flip_h", false)) u = 1 - u;
    if (c.value("flip_v", false)) v = 1 - v;
    if (u < -0.02f || u > 1.02f || v < -0.02f || v > 1.02f) return false;     // Kırpılan yerde
    x = p.x + u * p.w; y = p.y + v * p.h;
    return true;
}

bool clip_uses_input(const json& c) {
    return c.value("type", std::string()) == "video" && (c.value("cursor_halo", false) || c.value("click_rings", false) || c.value("keys_show", false));
}

Bitmap halo_bitmap(int size, uint32_t rgb) {
    Bitmap b;
    b.w = b.h = std::max(4, size); b.px.assign(size_t(b.w) * b.h * 4, 0);
    const float r = b.w / 2.f;
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++) {
            const float d = hypotf(x + 0.5f - r, y + 0.5f - r) / r;
            const float a = std::clamp((1 - d) / 0.35f, 0.f, 1.f);
            uint8_t* p = &b.px[(size_t(y) * b.w + x) * 4];
            p[0] = uint8_t((rgb & 255) * a); p[1] = uint8_t(((rgb >> 8) & 255) * a); p[2] = uint8_t(((rgb >> 16) & 255) * a); p[3] = uint8_t(255 * a);
        }
    return b;
}

Bitmap ring_frames(int size, uint32_t rgb, int frames) {
    size = std::max(8, size);
    Bitmap b;
    b.w = size * (frames + 1); b.h = size; b.px.assign(size_t(b.w) * b.h * 4, 0);
    const float c = size / 2.f, width = size * 0.07f;
    for (int f = 0; f < frames; f++) {
        const float u = (f + 0.5f) / frames;
        const float radius = size * (0.12f + 0.34f * (1 - powf(1 - u, 2))), fade = 1 - u;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                const float d = fabsf(hypotf(x + 0.5f - c, y + 0.5f - c) - radius);
                const float a = std::clamp(1 - (d - width / 2), 0.f, 1.f) * fade;
                if (a <= 0) continue;
                uint8_t* p = &b.px[(size_t(y) * b.w + f * size + x) * 4];
                p[0] = uint8_t((rgb & 255) * a); p[1] = uint8_t(((rgb >> 8) & 255) * a); p[2] = uint8_t(((rgb >> 16) & 255) * a); p[3] = uint8_t(255 * a);
            }
    }
    return b;
}

Bitmap key_bitmap(const std::string& text, float unit) {
    json s = {{"text", text}, {"font", "Segoe UI"}, {"font_size", 40}, {"bold", true}, {"color", "#ffffff"}, {"outline", false},
              {"background", true}, {"bg_color", "#101018"}, {"bg_opacity", 82}};
    return draw_text(s, unit);
}

bool load_cube(const std::wstring& path, Lut& out) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) return false;
    out = Lut{};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("LUT_3D_SIZE", 0) == 0) { out.size = atoi(line.c_str() + 11); continue; }
        if (!(isdigit(uint8_t(line[0])) || line[0] == '-' || line[0] == '.')) continue;     // TITLE, DOMAIN_MIN ...
        float r, g, b;
        if (sscanf_s(line.c_str(), "%f %f %f", &r, &g, &b) == 3) { out.rgb.push_back(r); out.rgb.push_back(g); out.rgb.push_back(b); }
    }
    return out.size >= 2 && out.size <= 129 && out.rgb.size() == size_t(out.size) * out.size * out.size * 3;
}

std::string filter_escape(const std::wstring& path) {
    std::string s = narrow(path), out;
    std::replace(s.begin(), s.end(), '\\', '/');
    for (char ch : s) { if (ch == ':' || ch == '\'') out += '\\'; out += ch; }
    return out;
}

const std::vector<VoiceInfo>& voice_list() {
    static const std::vector<VoiceInfo> list = {
        {"deep", "Kalın ses", "asetrate=38400,aresample=48000,atempo=1.25"},
        {"high", "İnce ses", "asetrate=62400,aresample=48000,atempo=0.7692"},
        {"robot", "Robot", "afftfilt=real='hypot(re,im)':imag='0':win_size=512:overlap=0.75,volume=6.5"},     // Ölçümde ~17 dB kısılıyordu
        {"phone", "Telefon", "highpass=f=300,lowpass=f=3400,acompressor=threshold=0.1:ratio=4"},
        {"radio", "Telsiz", "highpass=f=500,lowpass=f=2600,acrusher=bits=10:mix=0.25,acompressor=threshold=0.08:ratio=6,volume=1.4"},
        {"echo", "Yankı", "aecho=0.8:0.7:180:0.35"},
        {"hall", "Salon", "aecho=0.8:0.85:40|80|120:0.35|0.25|0.15"},
    };
    return list;
}

json default_caption_style() {
    return {{"size", 46}, {"color", "#ffffff"}, {"bg_opacity", 60}, {"y", 0.40}};
}

Bitmap caption_bitmap(const std::string& text, const json& style, float unit) {
    const int bg = style.value("bg_opacity", 60);
    json s = {{"text", text}, {"font", "Segoe UI"}, {"font_size", style.value("size", 46)}, {"bold", true}, {"color", style.value("color", std::string("#ffffff"))},
              {"outline", bg < 30}, {"outline_color", "#000000"}, {"outline_thickness", 3}, {"background", bg > 0}, {"bg_color", "#000000"}, {"bg_opacity", bg}};
    return draw_text(s, unit);
}

namespace {
double srt_time(const std::string& s) {
    int h = 0, m = 0, sec = 0, ms = 0;
    if (sscanf_s(s.c_str(), "%d:%d:%d%*[,.]%d", &h, &m, &sec, &ms) < 3) return -1;
    return h * 3600.0 + m * 60.0 + sec + ms / 1000.0;
}

std::string srt_stamp(double t) {
    const long long ms = (long long)std::llround(std::max(0.0, t) * 1000);
    char text[32];
    sprintf_s(text, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return text;
}
}  // namespace

json parse_srt(const std::string& raw) {
    std::string text = raw;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    json out = json::array();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t arrow = line.find("-->");
        if (arrow == std::string::npos) continue;
        const double a = srt_time(line.substr(0, arrow)), b = srt_time(line.substr(arrow + 3));
        if (a < 0 || b <= a) continue;
        std::string body, l;
        while (std::getline(in, l) && !l.empty()) body += (body.empty() ? "" : "\n") + l;
        out.push_back({{"start", a}, {"end", b}, {"text", body}});
    }
    return out;
}

std::string write_srt(const json& captions) {
    std::vector<json> list(captions.begin(), captions.end());
    std::sort(list.begin(), list.end(), [](const json& a, const json& b) { return a.value("start", 0.0) < b.value("start", 0.0); });
    std::string out;
    int n = 1;
    for (auto& c : list)
        out += std::to_string(n++) + "\n" + srt_stamp(c.value("start", 0.0)) + " --> " + srt_stamp(c.value("end", 0.0)) + "\n" + c.value("text", std::string()) + "\n\n";
    return out;
}

// --- Köşe, kenarlık, maske ---

bool clip_can_frame(const json& c) {
    const std::string type = c.value("type", std::string());
    return type == "video" || type == "image" || type == "color";
}

bool clip_has_frame(const json& c) {
    return clip_can_frame(c) && (c.value("corner", 0) > 0 || c.value("border", 0) > 0 || !c.value("mask", std::string()).empty() || c.value("feather", 0) > 0);
}

float shape_distance(float qx, float qy, float hx, float hy, float r, bool ellipse) {
    hx = std::max(hx, 0.5f); hy = std::max(hy, 0.5f);
    if (ellipse) return (hypotf(qx / hx, qy / hy) - 1) * std::min(hx, hy);
    r = std::clamp(r, 0.f, std::min(hx, hy));
    const float ax = fabsf(qx) - hx + r, ay = fabsf(qy) - hy + r;
    return hypotf(std::max(ax, 0.f), std::max(ay, 0.f)) + std::min(std::max(ax, ay), 0.f) - r;
}

FrameShape frame_shape(const json& c, float w, float h, float unit) {
    FrameShape s{};
    const std::string mask = c.value("mask", std::string());
    s.cx = w / 2; s.cy = h / 2; s.hx = w / 2; s.hy = h / 2;
    if (!mask.empty()) {
        s.hx = w * std::clamp(c.value("mask_w", 80), 5, 100) / 200.f;
        s.hy = h * std::clamp(c.value("mask_h", 80), 5, 100) / 200.f;
        s.cx += w * c.value("mask_x", 0) / 100.f;
        s.cy += h * c.value("mask_y", 0) / 100.f;
    }
    s.ellipse = mask == "ellipse";
    s.radius = c.value("corner", 0) * unit;
    s.feather = c.value("feather", 0) * unit;
    s.border = c.value("border", 0) * unit;
    return s;
}

namespace {
float mask_alpha(const FrameShape& s, float d) {
    return s.feather > 0.5f ? std::clamp(-d / s.feather, 0.f, 1.f) : std::clamp(0.5f - d, 0.f, 1.f);
}
}  // namespace

Bitmap frame_mask(const json& c, int w, int h, float unit) {
    const FrameShape s = frame_shape(c, float(w), float(h), unit);
    Bitmap b;
    b.w = w; b.h = h; b.px.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float d = shape_distance(x + 0.5f - s.cx, y + 0.5f - s.cy, s.hx, s.hy, s.radius, s.ellipse);
            const uint8_t a = uint8_t(mask_alpha(s, d) * 255 + 0.5f);
            uint8_t* p = &b.px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = a; p[3] = a;     // Önceden çarpılmış beyaz
        }
    return b;
}

Bitmap frame_border(const json& c, int w, int h, float unit) {
    const FrameShape s = frame_shape(c, float(w), float(h), unit);
    const uint32_t rgb = parse_color(c.value("border_color", std::string("#ffffff")));
    Bitmap b;
    b.w = w; b.h = h; b.px.assign(size_t(w) * h * 4, 0);
    if (s.border <= 0) return b;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float d = shape_distance(x + 0.5f - s.cx, y + 0.5f - s.cy, s.hx, s.hy, s.radius, s.ellipse);
            const float a = std::clamp(0.5f - d, 0.f, 1.f) * std::clamp(d + s.border + 0.5f, 0.f, 1.f);
            if (a <= 0) continue;
            uint8_t* p = &b.px[(size_t(y) * w + x) * 4];
            p[0] = uint8_t((rgb & 255) * a); p[1] = uint8_t(((rgb >> 8) & 255) * a); p[2] = uint8_t(((rgb >> 16) & 255) * a); p[3] = uint8_t(a * 255);
        }
    return b;
}

Bitmap spot_overlay(const json& c, int W, int H) {
    const Placement r = clip_placement(c, float(W), float(H));
    const float unit = H / 1080.f, feather = std::max(1.f, c.value("feather", 60) * unit);
    const float dark = std::clamp(c.value("dark", 70), 0, 100) / 100.f;
    const bool ellipse = c.value("spot_shape", std::string("ellipse")) == "ellipse";
    Bitmap b;
    b.w = W; b.h = H; b.px.assign(size_t(W) * H * 4, 0);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float d = shape_distance(x + 0.5f - (r.x + r.w / 2), y + 0.5f - (r.y + r.h / 2), r.w / 2, r.h / 2, c.value("corner", 24) * unit, ellipse);
            b.px[(size_t(y) * W + x) * 4 + 3] = uint8_t(dark * std::clamp(d / feather + 0.5f, 0.f, 1.f) * 255 + 0.5f);
        }
    return b;
}

namespace {
const double kJoin = 0.25;     // Bu kadar yakın klipler art arda sayılır, aradaki boşluğu geçiş kapatır

const json* neighbour(const json& p, const json& c, bool next) {
    const double start = c.value("start", 0.0), end = start + c.value("length", 0.0);
    const std::string track = c.value("track", std::string()), id = c.value("id", std::string());
    const json* best = nullptr;
    double gap = kJoin;
    for (auto& n : p["clips"]) {
        if (n.value("track", std::string()) != track || n.value("id", std::string()) == id) continue;
        const double d = next ? n.value("start", 0.0) - end : start - (n.value("start", 0.0) + n.value("length", 0.0));
        if (d >= -0.002 && d <= gap) { gap = d; best = &n; }
    }
    return best;
}

Transition own(const json& c, const char* key, const char* duration) {
    Transition t;
    if (!find_transition(c.value(key, std::string()))) return t;
    t.key = c.value(key, std::string());
    t.duration = std::max(0.05, std::min(c.value(duration, 1.0), c.value("length", 0.0)));
    return t;
}
}  // namespace

static bool track_locked(const json& p, const json& c) {
    const json* t = find_track(p, c.value("track", std::string()));
    return t && t->value("locked", false);
}

// Bölmede animasyon noktaları iki parçaya dağılır: kesim anındaki görünüm iki tarafa da nokta olur, sağdakiler başa kayar
void split_keys(json& left, json& right, double at) {
    if (clip_keys(left).empty()) return;
    const json view = clip_at(left, at);
    json lk = json::array(), rk = json::array();
    for (auto& k : left["keys"]) {
        const double t = k.value("t", 0.0);
        if (t < at - 1e-4) lk.push_back(k);
        if (t > at + 1e-4) { json moved = k; moved["t"] = t - at; rk.push_back(moved); }
    }
    left["keys"] = lk;
    right["keys"] = rk;
    set_key(left, at, view, 1e-4);
    set_key(right, 0, view, 1e-4);
}

void project_cut(json& p, double time) {
    const size_t count = p["clips"].size();
    for (size_t i = 0; i < count; i++) {
        json& c = p["clips"][i];
        const double start = c.value("start", 0.0), length = c.value("length", 0.0);
        if (track_locked(p, c) || time <= start + 0.001 || time >= start + length - 0.001) continue;
        json right = c;
        const double left = time - start;
        split_keys(c, right, left);
        c["length"] = left;
        c["fade_out"] = 0.0; c["audio_fade_out"] = 0.0; c["t_out"] = "";
        right["id"] = make_id();
        right["start"] = time;
        right["length"] = length - left;
        if (clip_reversed(c)) c["in"] = c.value("in", 0.0) + (length - left) * c.value("speed", 1.0);   // Ters klipte ilk parça kaynağın sonudur
        else right["in"] = right.value("in", 0.0) + left * right.value("speed", 1.0);
        right["fade_in"] = 0.0; right["audio_fade_in"] = 0.0; right["t_in"] = "";
        c["m_out"] = ""; right["m_in"] = "";
        p["clips"].push_back(right);
    }
}

void project_delete_range(json& p, double from, double to) {
    if (to - from < 0.01) return;
    project_cut(p, from);
    project_cut(p, to);
    const double gap = to - from;
    json kept = json::array();
    for (auto& c : p["clips"]) {
        const double start = c.value("start", 0.0), end = start + c.value("length", 0.0);
        if (!track_locked(p, c) && start >= from - 1e-4 && end <= to + 1e-4) continue;
        if (!track_locked(p, c) && start >= to - 1e-4) c["start"] = start - gap;
        kept.push_back(c);
    }
    p["clips"] = kept;
    json markers = json::array();
    for (auto m : p["markers"]) {
        const double t = marker_time(m);
        if (t > from && t < to) continue;            // Sınırdaki işaret kalır
        const double moved = t <= from ? t : t - gap;
        if (m.is_object()) m["t"] = moved; else m = moved;
        markers.push_back(m);
    }
    p["markers"] = markers;
    if (p.contains("captions") && p["captions"].is_array()) {   // Altyazılar da aralıkla birlikte kayar ya da kısalır
        json caps = json::array();
        for (auto c : p["captions"]) {
            double a = c.value("start", 0.0), b = c.value("end", 0.0);
            auto move = [&](double t) { return t <= from ? t : t >= to ? t - gap : from; };
            a = move(a); b = move(b);
            if (b - a < 0.05) continue;
            c["start"] = a; c["end"] = b;
            caps.push_back(c);
        }
        p["captions"] = caps;
    }
}

void project_keep_range(json& p, double from, double to) {
    project_delete_range(p, to, std::max(to, project_length(p)) + 1);
    project_delete_range(p, 0, from);
}

std::vector<Key> clip_keys(const json& c) {
    std::vector<Key> out;
    if (!c.contains("keys") || !c["keys"].is_array()) return out;
    for (auto& k : c["keys"]) {
        if (!k.is_object()) continue;
        Key key;
        key.t = k.value("t", 0.0); key.scale = k.value("scale", 100.0); key.x = k.value("x", 0.0); key.y = k.value("y", 0.0);
        key.rotate = k.value("rotate", 0.0); key.opacity = k.value("opacity", 100.0); key.smooth = k.value("smooth", true);
        out.push_back(key);
    }
    std::sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    return out;
}

json clip_at(const json& c, double local) {
    const auto keys = clip_keys(c);
    if (keys.empty()) return c;
    json out = c;
    auto put = [&](const Key& k) { out["scale"] = k.scale; out["x"] = k.x; out["y"] = k.y; out["rotate"] = k.rotate; out["opacity"] = int(k.opacity + 0.5); };
    if (local <= keys.front().t) { put(keys.front()); return out; }
    if (local >= keys.back().t) { put(keys.back()); return out; }
    for (size_t i = 0; i + 1 < keys.size(); i++) {
        const Key& a = keys[i];
        const Key& b = keys[i + 1];
        if (local > b.t) continue;
        double u = (local - a.t) / std::max(1e-6, b.t - a.t);
        if (a.smooth) u = u * u * (3 - 2 * u);
        Key k;
        k.scale = a.scale + (b.scale - a.scale) * u; k.x = a.x + (b.x - a.x) * u; k.y = a.y + (b.y - a.y) * u;
        k.rotate = a.rotate + (b.rotate - a.rotate) * u; k.opacity = a.opacity + (b.opacity - a.opacity) * u;
        put(k);
        out["opacity"] = int(k.opacity + 0.5);
        return out;
    }
    return out;
}

void set_key(json& c, double local, const json& view, double tolerance) {
    if (!c.contains("keys") || !c["keys"].is_array()) c["keys"] = json::array();
    json key = {{"t", local}, {"scale", view.value("scale", 100.0)}, {"x", view.value("x", 0.0)}, {"y", view.value("y", 0.0)},
                {"rotate", view.value("rotate", 0.0)}, {"opacity", double(view.value("opacity", 100))}, {"smooth", true}};
    for (auto& k : c["keys"])
        if (fabs(k.value("t", 0.0) - local) <= tolerance) {
            key["t"] = k.value("t", 0.0);
            key["smooth"] = k.value("smooth", true);
            k = key;
            return;
        }
    c["keys"].push_back(key);
}

double marker_time(const json& m) { return m.is_number() ? m.get<double>() : m.is_object() ? m.value("t", 0.0) : 0.0; }
std::string marker_name(const json& m) { return m.is_object() ? m.value("name", std::string()) : std::string(); }

std::vector<std::pair<double, std::string>> project_chapters(const json& p) {
    std::vector<std::pair<double, std::string>> out;
    if (p.contains("markers") && p["markers"].is_array())
        for (auto& m : p["markers"]) out.push_back({marker_time(m), marker_name(m)});
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return out;
}

// YouTube açıklamasına yapıştırılacak liste: ilk bölüm 0:00'da olmalı
std::string youtube_chapters(const json& p) {
    auto list = project_chapters(p);
    if (list.empty() || list.front().first > 0.5) list.insert(list.begin(), {0.0, std::string()});
    std::string out;
    int n = 1;
    for (auto& [t, name] : list) {
        const int s = int(t);
        char stamp[32];
        if (s >= 3600) sprintf_s(stamp, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        else sprintf_s(stamp, "%d:%02d", s / 60, s % 60);
        out += std::string(stamp) + " " + (name.empty() ? std::string(n == 1 ? T("Giriş") : TF("Bölüm %d", n)) : name) + "\n";
        n++;
    }
    return out;
}

json* previous_clip(json& p, const json& c) { return const_cast<json*>(neighbour(p, c, false)); }

Transition transition_in(const json& p, const json& c) {
    Transition t = own(c, "t_in", "t_in_d");
    if (!t.key.empty()) return t;
    if (const json* prev = neighbour(p, c, false)) {     // Önceki klibin çıkış geçişi aslında bu iki klibin arasındadır
        t = own(*prev, "t_out", "t_out_d");
        t.duration = std::min(t.duration, c.value("length", 0.0));
    }
    return t;
}

Transition transition_out(const json& p, const json& c) {
    if (neighbour(p, c, true)) return {};      // Arkasında klip varsa kararmaz, sonraki klip üstüne gelir
    return own(c, "t_out", "t_out_d");
}

double clip_tail(const json& p, const json& c) {
    const json* next = neighbour(p, c, true);
    if (!next) return 0;
    const Transition t = transition_in(p, *next);
    if (t.key.empty()) return 0;
    const double gap = std::max(0.0, next->value("start", 0.0) - c.value("start", 0.0) - c.value("length", 0.0));
    return gap + t.duration;
}

void pack_magnetic(json& p) {
    for (auto& track : p["tracks"]) {
        if (!track.value("magnet", false)) continue;
        std::vector<json*> clips;
        for (auto& c : p["clips"])
            if (c.value("track", std::string()) == track.value("id", std::string())) clips.push_back(&c);
        // Ortasına göre sıralanır: sürüklenen klip bıraktığı yere girer
        std::sort(clips.begin(), clips.end(), [](const json* a, const json* b) {
            return a->value("start", 0.0) + a->value("length", 0.0) / 2 < b->value("start", 0.0) + b->value("length", 0.0) / 2;
        });
        double at = 0;
        for (json* c : clips) {
            if (fabs(c->value("start", 0.0) - at) > 1e-6) (*c)["start"] = at;
            at += c->value("length", 0.0);
        }
    }
}
double project_length(const json& p) {
    double end = 0;
    for (auto& c : p["clips"]) end = std::max(end, c.value("start", 0.0) + c.value("length", 0.0));
    return end;
}

const json* find_track(const json& p, const std::string& id) {
    for (auto& t : p["tracks"])
        if (t.value("id", std::string()) == id) return &t;
    return nullptr;
}

bool clip_has_video(const json& c) { return c.value("type", std::string()) != "audio"; }

bool clip_has_audio(const json& c) {
    const std::string type = c.value("type", std::string());
    return type == "audio" || (type == "video" && c.value("has_audio", false));
}

Placement clip_placement(const json& c, float W, float H) {
    float mw = float(c.value("w", 0)), mh = float(c.value("h", 0));
    const std::string type = c.value("type", std::string());
    if (type == "color") { mw = W; mh = H; }
    if (type == "blur" || type == "spot") { mw = float(c.value("shape_w", 420)); mh = float(c.value("shape_h", 220)); }
    mw *= std::max(0.02f, 1 - float(c.value("crop_l", 0.0) + c.value("crop_r", 0.0)) / 100);
    mh *= std::max(0.02f, 1 - float(c.value("crop_t", 0.0) + c.value("crop_b", 0.0)) / 100);
    Placement out;
    if (mw <= 0 || mh <= 0) return out;
    // Yazı piksel ölçüsüyle durur (1080p'ye göre), görüntü tuvale sığdırılır
    const float fit = clip_is_visual_still(c) || type == "spot" ? H / 1080.f : std::min(W / mw, H / mh);
    const float k = fit * float(c.value("scale", 100.0)) / 100.f;
    out.w = mw * k;
    out.h = mh * k;
    out.x = (W - out.w) / 2 + float(c.value("x", 0.0)) * W;
    out.y = (H - out.h) / 2 + float(c.value("y", 0.0)) * H;
    return out;
}

// --- Süreç ---

bool Process::start(const std::wstring& command, bool capture_stderr) {
    kill();
    // Devredilebilir uçlar açıkken başka süreç başlarsa o da bu uçları alır ve okuma hiç bitmez: başlatmalar sırayla yapılır
    static std::mutex spawn_lock;
    std::lock_guard<std::mutex> spawn(spawn_lock);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_write = nullptr, err_write = nullptr;
    if (!CreatePipe(&out_, &out_write, &sa, 1 << 20)) return false;
    SetHandleInformation(out_, HANDLE_FLAG_INHERIT, 0);
    if (capture_stderr) {
        if (!CreatePipe(&err_, &err_write, &sa, 1 << 16)) { CloseHandle(out_write); return false; }
        SetHandleInformation(err_, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out_write;
    si.hStdError = capture_stderr ? err_write : nul;
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(out_write);
    if (err_write) CloseHandle(err_write);
    CloseHandle(nul);
    if (!ok) { kill(); return false; }
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) {
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        AssignProcessToJobObject(job_, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    if (capture_stderr) {
        err_thread_ = std::thread([this] {
            char buffer[1024];
            DWORD got = 0;
            while (ReadFile(err_, buffer, sizeof(buffer), &got, nullptr) && got) {
                std::lock_guard<std::mutex> g(lock_);
                err_text_.append(buffer, got);
                if (err_text_.size() > 20000) err_text_.erase(0, err_text_.size() - 20000);
            }
        });
    }
    return true;
}

size_t Process::read_some(void* data, size_t size) {
    DWORD got = 0;
    if (!out_ || !ReadFile(out_, data, DWORD(size), &got, nullptr)) return 0;
    return got;
}

size_t Process::read(void* data, size_t size) {
    size_t have = 0;
    while (have < size) {
        const size_t got = read_some(static_cast<char*>(data) + have, size - have);
        if (!got) break;
        have += got;
    }
    return have;
}

std::string Process::error_text() {
    std::lock_guard<std::mutex> g(lock_);
    return err_text_;
}

int Process::wait() {
    if (!process_) return -1;
    WaitForSingleObject(process_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process_, &code);
    return int(code);
}

void Process::kill() {
    if (process_) TerminateProcess(process_, 1);
    if (err_thread_.joinable()) err_thread_.join();
    if (process_) CloseHandle(process_);
    if (out_) CloseHandle(out_);
    if (err_) CloseHandle(err_);
    if (job_) CloseHandle(job_);
    process_ = job_ = out_ = err_ = nullptr;
}

// --- Medya bilgisi: FFmpeg dosyayı açınca yazdığı satırlar okunur ---

bool probe_media(const std::wstring& path, MediaInfo& out, std::string& error) {
    const std::wstring ext = fs::path(path).extension().wstring();
    Process p;
    if (!p.start(L"\"" + ffmpeg_exe() + L"\" -hide_banner -nostdin -i \"" + path + L"\"", true)) { error = "FFmpeg açılamadı"; return false; }
    char sink[256];
    while (p.read_some(sink, sizeof(sink))) {}
    p.wait();
    Sleep(10);
    p.kill();       // Hata metni iş parçacığı bitsin
    const std::string text = p.error_text();
    int h = 0, m = 0;
    double s = 0;
    const size_t at = text.find("Duration: ");
    if (at != std::string::npos && sscanf_s(text.c_str() + at, "Duration: %d:%d:%lf", &h, &m, &s) == 3) out.duration = h * 3600.0 + m * 60.0 + s;
    std::istringstream lines(text);
    std::string line;
    bool in_audio = false;
    while (std::getline(lines, line)) {
        if (line.find("Stream #") == std::string::npos) {
            const size_t title = line.find("title");     // Parçanın adı, akış satırının altındaki Metadata bölümünde
            const size_t colon = line.find(':', title == std::string::npos ? 0 : title);
            if (in_audio && title != std::string::npos && colon != std::string::npos && line.find_first_not_of(' ') == title) {
                std::string t = line.substr(colon + 1);
                while (!t.empty() && (t.front() == ' ')) t.erase(t.begin());
                while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
                out.audio_titles.back() = t;
            }
            continue;
        }
        in_audio = line.find("Audio:") != std::string::npos;
        if (in_audio) { out.audio = true; out.audio_titles.push_back(""); }
        if (line.find("Video:") == std::string::npos || out.video) continue;
        for (size_t i = line.find("Video:"); i < line.size(); i++) {     // Boyut: "1920x1080"
            int w = 0, ht = 0;
            if (isdigit(uint8_t(line[i])) && sscanf_s(line.c_str() + i, "%dx%d", &w, &ht) == 2 && w > 15 && ht > 15) {
                out.width = w; out.height = ht;
                break;
            }
        }
        const size_t fps = line.find(" fps");
        if (fps != std::string::npos) {
            size_t b = line.rfind(' ', fps - 1);
            if (b != std::string::npos) out.fps = atof(line.c_str() + b + 1);
        }
        out.video = out.width > 0;
    }
    std::wstring low = ext;
    std::transform(low.begin(), low.end(), low.begin(), towlower);
    out.image = out.video && !out.audio && (low == L".png" || low == L".jpg" || low == L".jpeg" || low == L".bmp" || low == L".webp" || low == L".gif" || low == L".tif" || low == L".tiff");
    if (!out.video && !out.audio) {
        error = "Bu dosya açılamadı";
        return false;
    }
    return true;
}

// --- Önizleme kareleri ---

void VideoFeed::start(const std::wstring& file, double source_time, double speed, int fps, int w, int h, double reverse_span) {
    frame_size_ = size_t(w) * h * 4;
    wchar_t head[256], tail[256];
    if (reverse_span > 0) {     // Ters: kısa bir parça çözülür ve tersine verilir
        const double from = std::max(0.0, source_time - reverse_span);
        swprintf_s(head, L" -hide_banner -loglevel quiet -nostdin -ss %.3f -t %.3f -i \"", from, std::max(0.05, source_time - from));
        swprintf_s(tail, L"\" -an -vf \"scale=%d:%d,reverse,setpts=(PTS-STARTPTS)/%.4f,fps=%d\" -f rawvideo -pix_fmt bgra pipe:1", w, h, speed, fps);
    } else {
        swprintf_s(head, L" -hide_banner -loglevel quiet -nostdin -ss %.3f -i \"", std::max(0.0, source_time));
        swprintf_s(tail, L"\" -an -vf \"setpts=PTS/%.4f,fps=%d,scale=%d:%d\" -f rawvideo -pix_fmt bgra pipe:1", speed, fps, w, h);
    }
    if (proc_.start(L"\"" + ffmpeg_exe() + L"\"" + head + file + tail)) thread_ = std::thread(&VideoFeed::loop, this);
}

VideoFeed::~VideoFeed() {
    halt_ = true;
    wake_.notify_all();
    proc_.kill();       // Okuma hemen biter
    if (thread_.joinable()) thread_.join();
}

void VideoFeed::loop() {
    int index = 0;
    while (!halt_) {
        std::vector<uint8_t> frame(frame_size_);
        if (proc_.read(frame.data(), frame.size()) != frame.size()) return;
        std::unique_lock<std::mutex> g(lock_);
        wake_.wait(g, [&] { return frames_.size() < 4 || halt_; });
        frames_.push_back({index++, std::move(frame)});
    }
}

bool VideoFeed::take(int index, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> g(lock_);
    bool found = false;
    while (!frames_.empty() && frames_.front().first <= index) {
        out = std::move(frames_.front().second);
        frames_.pop_front();
        found = true;
    }
    wake_.notify_all();
    return found;
}

// --- Filtre grafiği ---

namespace {

struct Graph {
    std::vector<std::string> inputs;
    std::string filters;
    bool has_video = false;
};

std::string num(double v) {
    char text[32];
    sprintf_s(text, "%.4f", v);
    return text;
}

std::string quoted(const std::string& s) { return " \"" + s + "\""; }

// Ses noktaları: kaynak zamanına bağlı kazanç çizgisi, noktalar arası doğrusal
std::string envelope_expr(const json& c, double skip) {
    std::vector<std::pair<double, double>> pts;
    if (c.contains("vol_points") && c["vol_points"].is_array())
        for (auto& p : c["vol_points"])
            if (p.is_array() && p.size() == 2) pts.push_back({p[0].get<double>(), p[1].get<double>() / 100.0});
    if (pts.empty()) return {};
    std::sort(pts.begin(), pts.end());
    const std::string s = "(" + num(c.value("in", 0.0)) + "+(t+" + num(skip) + ")*" + num(c.value("speed", 1.0)) + ")";
    std::string e = num(pts.back().second);
    for (size_t i = pts.size() - 1; i > 0; i--) {
        const auto& a = pts[i - 1];
        const auto& b = pts[i];
        const double span = std::max(1e-3, b.first - a.first);
        e = "if(lt(" + s + "," + num(b.first) + ")," + num(a.second) + "+(" + num(b.second - a.second) + ")*(" + s + "-" + num(a.first) + ")/" + num(span) + "," + e + ")";
    }
    return "if(lt(" + s + "," + num(pts[0].first) + ")," + num(pts[0].second) + "," + e + ")";
}

// Anahtar karelerden zamanın ifadesi: T değişkeni klip içindeki zaman
std::string key_expr(const std::vector<Key>& keys, double (*field)(const Key&), const std::string& T) {
    std::string e = num(field(keys.back()));
    for (size_t i = keys.size() - 1; i > 0; i--) {
        const Key& a = keys[i - 1];
        const Key& b = keys[i];
        const std::string u = "((" + T + "-" + num(a.t) + ")/" + num(std::max(1e-3, b.t - a.t)) + ")";
        const std::string eased = a.smooth ? "(" + u + "*" + u + "*(3-2*" + u + "))" : u;
        e = "if(lt(" + T + "," + num(b.t) + ")," + num(field(a)) + "+(" + num(field(b) - field(a)) + ")*" + eased + "," + e + ")";
    }
    return "if(lt(" + T + "," + num(keys.front().t) + ")," + num(field(keys.front())) + "," + e + ")";
}

bool key_varies(const std::vector<Key>& keys, double (*field)(const Key&)) {
    for (auto& k : keys)
        if (fabs(field(k) - field(keys.front())) > 1e-6) return true;
    return false;
}

std::string atempo_chain(double speed) {
    std::string out;
    while (speed > 2.0) { out += "atempo=2.0,"; speed /= 2; }
    while (speed < 0.5) { out += "atempo=0.5,"; speed /= 0.5; }
    if (fabs(speed - 1) > 0.001) out += "atempo=" + num(speed) + ",";
    return out;
}

// Ses grafiği: from noktasından sonrası. Her klip kendi girişi olur.
void add_audio(const json& project, double from, double until, Graph& g, int& input_index, std::map<std::string, int>* video_inputs, bool& any) {
    std::vector<std::string> labels;
    for (auto& c : project["clips"]) {
        if (!clip_has_audio(c) || c.value("volume", 100) <= 0) continue;
        const json* track = find_track(project, c.value("track", std::string()));
        if (track && track->value("muted", false)) continue;
        const double start = c.value("start", 0.0), length = c.value("length", 0.0), speed = c.value("speed", 1.0);
        if (start + length <= from || start >= until) continue;
        const double skip = std::max(0.0, from - start);
        const bool reverse = clip_reversed(c);
        // Ters klipte kalan kısım kaynağın başındadır
        const double source_in = reverse ? c.value("in", 0.0) : c.value("in", 0.0) + skip * speed, left = length - skip;
        int index = -1;
        const std::string id = c.value("id", std::string());
        if (video_inputs && video_inputs->count(id) && !reverse) {
            index = (*video_inputs)[id];
        } else {
            index = input_index++;
            g.inputs.push_back(" -ss " + num(source_in) + " -t " + num(left * speed) + " -i" + quoted(c.value("file", std::string())));
        }
        const std::string label = "a" + std::to_string(index);
        std::string f = "[" + std::to_string(index) + ":a:" + std::to_string(c.value("audio_stream", 0)) + "]" + std::string(reverse ? "areverse," : "") + "asetpts=PTS-STARTPTS," + atempo_chain(speed);
        f += "aresample=48000";
        const int denoise = c.value("denoise", 0), level = c.value("level", 0);
        if (denoise > 0) f += ",afftdn=nr=" + num(3 + denoise * 0.3) + ":nf=-30";   // Gürültü azaltma; ölçümde -30 dB tabanı farklı gürültülerde çalıştı
        if (level > 0) f += ",speechnorm=e=" + num(1.5 + level * 0.12) + ":r=0.0002:l=1";   // Gecikmesiz ses dengeleme
        for (auto& v : voice_list())        // Ses efekti
            if (c.value("voice", std::string()) == v.key) f += std::string(",") + v.filter + ",aresample=48000";
        f += ",volume=" + num(c.value("volume", 100) / 100.0);
        const std::string env = envelope_expr(c, skip);
        if (!env.empty()) f += ",volume='" + env + "':eval=frame";
        const double in_fade = c.value("audio_fade_in", 0.0) - skip, out_fade = c.value("audio_fade_out", 0.0);
        if (in_fade > 0) f += ",afade=t=in:st=0:d=" + num(in_fade);
        if (out_fade > 0) f += ",afade=t=out:st=" + num(std::max(0.0, left - out_fade)) + ":d=" + num(std::min(out_fade, left));
        const int delay = int(std::max(0.0, start - from) * 1000);
        if (delay > 0) f += ",adelay=" + std::to_string(delay) + ":all=1";
        g.filters += f + "[" + label + "];";
        labels.push_back(label);
    }
    any = !labels.empty();
    if (labels.empty()) return;
    for (auto& l : labels) g.filters += "[" + l + "]";
    if (labels.size() == 1) g.filters += "anull[aout];";
    else g.filters += "amix=inputs=" + std::to_string(labels.size()) + ":normalize=0:duration=longest[aout];";
}

}  // namespace

std::wstring audio_preview_command(const json& project, double from) {
    Graph g;
    int index = 0;
    bool any = false;
    add_audio(project, from, from + 900, g, index, nullptr, any);
    std::string cmd = " -hide_banner -loglevel quiet -nostdin";
    for (auto& i : g.inputs) cmd += i;
    if (!any) return {};
    g.filters.pop_back();   // Son ';'
    cmd += " -filter_complex \"" + g.filters + "\" -map [aout] -f s16le -ac 2 -ar 48000 pipe:1";
    return L"\"" + ffmpeg_exe() + L"\"" + widen(cmd);
}

// Kalite ölçekleme: keskin kenar, doğru renk
static const char* kScaleFlags = "lanczos+accurate_rnd+full_chroma_int";

std::wstring export_command(const json& project, const EncodeConfig& config, const std::wstring& work, std::string& error) {
    const double total = project_length(project);
    if (total <= 0) { error = "Zaman çizelgesi boş"; return {}; }
    const int W = config.width, H = config.height, F = config.fps;
    const std::string size = std::to_string(W) + "x" + std::to_string(H), fps = std::to_string(F);
    Graph g;
    int index = 0;
    std::map<std::string, int> video_inputs;
    // Birleştirme RGB'de yapılır: renk çözünürlüğü kaybolmaz, renk matrisi karışmaz
    g.filters = "color=c=black:s=" + size + ":r=" + fps + ":d=" + num(total) + ",format=gbrp[b0];";
    int step = 0;
    const float unit = H / 1080.f;
    auto even = [](double v) { return std::max(2, int(v + 0.5) & ~1); };
    auto filter_path = [](const fs::path& p) {     // Filtre içinde dosya yolu: / ve kaçışlı :
        std::string s = narrow(p.wstring());
        std::replace(s.begin(), s.end(), '\\', '/');
        std::string out;
        for (char ch : s) { if (ch == ':') out += '\\'; out += ch; }
        return out;
    };
    // Zamanlı resimler tuval genişliğinde tek bir saydam şeride dizilir; tek girişle bindirilir
    auto strip = [&](std::vector<std::tuple<double, double, Bitmap>> items, int sh, const std::wstring& name, int& input) -> bool {
        std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });
        auto save = [&](const Bitmap& src, const std::wstring& file) {
            Bitmap b;
            b.w = W; b.h = sh; b.px.assign(size_t(W) * sh * 4, 0);
                const int ox = (W - src.w) / 2, oy = (sh - src.h) / 2;
            for (int y = 0; y < src.h; y++) {
                if (y + oy < 0 || y + oy >= sh) continue;
                for (int x = 0; x < src.w; x++)
                    if (x + ox >= 0 && x + ox < W) memcpy(&b.px[(size_t(y + oy) * W + x + ox) * 4], &src.px[(size_t(y) * src.w + x) * 4], 4);
            }
            return save_png(b, (fs::path(work) / file).wstring());
        };
        if (!save(Bitmap{}, name + L"_bos.png")) return false;     // Bütün kareler aynı boyda olmalı
        std::string list = "ffconcat version 1.0\n", last;
        auto add = [&](const std::wstring& file, double d) {
            std::string p = narrow((fs::path(work) / file).wstring());
            std::replace(p.begin(), p.end(), '\\', '/');
            list += "file '" + p + "'\nduration " + num(std::max(0.001, d)) + "\n";
            last = p;
        };
        double at = 0;
        for (size_t i = 0; i < items.size(); i++) {
            double t0 = std::max(at, std::get<0>(items[i])), t1 = std::get<1>(items[i]);
            if (i + 1 < items.size()) t1 = std::min(t1, std::get<0>(items[i + 1]));
            if (t1 - t0 < 0.02) continue;
            if (t0 > at + 0.001) add(name + L"_bos.png", t0 - at);
            const std::wstring file = name + L"_" + std::to_wstring(i) + L".png";
            if (!save(std::get<2>(items[i]), file)) return false;
            add(file, t1 - t0);
            at = t1;
        }
        add(name + L"_bos.png", std::max(0.1, total + 1 - at));
        list += "file '" + last + "'\n";
        const fs::path script = fs::path(work) / (name + L".txt");
        { std::ofstream o(script, std::ios::binary); o << list; }
        input = index++;
        g.inputs.push_back(" -f concat -safe 0 -i" + quoted(narrow(script.wstring())));
        return true;
    };
    // Kayıt Stüdyosu verisiyle imleç vurgusu, tıklama halkası ve tuş göstergesi: klibin üstüne ayrı katmanlar
    auto input_effects = [&](const json& c, const std::string& n) -> bool {
        if (!clip_uses_input(c)) return true;
        const auto data = input_data(c.value("file", std::string()));
        if (!data) return true;
        const double start = c.value("start", 0.0), length = c.value("length", 0.0), grid = 1.0 / 30;
        const std::string window = "between(t," + num(start) + "," + num(start + length) + ")";
        auto point = [&](double local, float sx, float sy, float& x, float& y) { return input_point(c, *data, sx, sy, local, float(W), float(H), x, y); };
        if (c.value("cursor_halo", false) && !data->cursor.empty()) {
            const int size = even(std::clamp(c.value("halo_size", 90), 10, 400) * unit);
            const fs::path png = fs::path(work) / (L"halo" + widen(n) + L".png"), cmds = fs::path(work) / (L"halo" + widen(n) + L".txt");
            if (!save_png(halo_bitmap(size, parse_color(c.value("halo_color", std::string("#ffe066")))), png.wstring())) return false;
            std::string text;
            float lx = -1e9f, ly = -1e9f;
            for (double local = 0; local <= length; local += grid) {
                float sx, sy, x, y;
                if (!cursor_at(*data, source_time(c, local), sx, sy)) continue;
                if (!point(local, sx, sy, x, y)) { x = -10000; y = -10000; }
                if (fabsf(x - lx) < 0.5f && fabsf(y - ly) < 0.5f) continue;
                lx = x; ly = y;
                text += num(start + local) + " overlay@h" + n + " x " + std::to_string(int(x) - size / 2) + ", overlay@h" + n + " y " + std::to_string(int(y) - size / 2) + ";\n";
            }
            { std::ofstream o(cmds, std::ios::binary); o << text; }
            const std::string i = std::to_string(index++);
            g.inputs.push_back(" -loop 1 -framerate " + fps + " -t " + num(total + 1) + " -i" + quoted(narrow(png.wstring())));
            g.filters += "[" + i + ":v]format=gbrap,colorchannelmixer=aa=" + num(std::clamp(c.value("halo_opacity", 45), 0, 100) / 100.0) + "[hl" + n + "];[b" +
                         std::to_string(step) + "]sendcmd=f='" + filter_path(cmds) + "'[hq" + n + "];[hq" + n + "][hl" + n + "]overlay@h" + n +
                         "=x=-10000:y=-10000:format=gbrp:eof_action=pass:enable='" + window + "'[b" + std::to_string(step + 1) + "];";
            step++;
        }
        if (c.value("click_rings", false) && !data->clicks.empty()) {
            const int size = even(150 * unit), frames = 12;
            const double ring = 0.45;
            const fs::path png = fs::path(work) / (L"halka" + widen(n) + L".png"), cmds = fs::path(work) / (L"halka" + widen(n) + L".txt");
            if (!save_png(ring_frames(size, parse_color(c.value("ring_color", std::string("#ff3040"))), frames), png.wstring())) return false;
            std::vector<std::pair<double, std::array<float, 4>>> clicks;     // Klip içi an, tıklama
            for (auto& k : data->clicks) {
                double local;
                if (clip_local_of(c, k[0], local)) clicks.push_back({local, k});
            }
            std::sort(clicks.begin(), clicks.end(), [](auto& a, auto& b) { return a.first < b.first; });
            std::string text;
            int shown = -2, at_x = -1, at_y = -1;
            for (double local = 0; local <= length + grid; local += grid) {
                int frame = frames, x = at_x, y = at_y;      // frames: boş kare
                for (auto& [when, k] : clicks) {
                    if (when > local + 1e-6) break;
                    if (local - when < ring) {
                        float px, py;
                        if (point(when, k[1], k[2], px, py)) { frame = std::min(frames - 1, int((local - when) / ring * frames)); x = int(px) - size / 2; y = int(py) - size / 2; }
                    }
                }
                if (frame == shown && x == at_x && y == at_y) continue;
                text += num(start + local) + " crop@r" + n + " x " + std::to_string(frame * size);
                if (x != at_x || y != at_y) text += ", overlay@r" + n + " x " + std::to_string(x) + ", overlay@r" + n + " y " + std::to_string(y);
                text += ";\n";
                shown = frame; at_x = x; at_y = y;
            }
            { std::ofstream o(cmds, std::ios::binary); o << text; }
            const std::string i = std::to_string(index++);
            g.inputs.push_back(" -loop 1 -framerate " + fps + " -t " + num(total + 1) + " -i" + quoted(narrow(png.wstring())));
            g.filters += "[" + i + ":v]format=gbrap,crop@r" + n + "=w=" + std::to_string(size) + ":h=" + std::to_string(size) + ":x=" + std::to_string(frames * size) +
                         ":y=0[rg" + n + "];[b" + std::to_string(step) + "]sendcmd=f='" + filter_path(cmds) + "'[rq" + n + "];[rq" + n + "][rg" + n + "]overlay@r" + n +
                         "=x=-10000:y=-10000:format=gbrp:eof_action=pass:enable='" + window + "'[b" + std::to_string(step + 1) + "];";
            step++;
        }
        if (c.value("keys_show", false) && !data->keys.empty()) {
            std::vector<std::tuple<double, double, Bitmap>> items;
            int sh = 2;
            for (auto& [t, label] : data->keys) {
                double local;
                if (!clip_local_of(c, t, local)) continue;
                Bitmap b = key_bitmap(label, unit);
                sh = std::max(sh, b.h);
                items.push_back({start + local, std::min(start + length, start + local + 1.4), std::move(b)});
            }
            if (!items.empty()) {
                int input;
                sh = even(sh + 4);
                if (!strip(std::move(items), sh, L"tus" + widen(n), input)) return false;
                g.filters += "[" + std::to_string(input) + ":v]format=gbrap,setpts=PTS-STARTPTS[ks" + n + "];[b" + std::to_string(step) + "][ks" + n + "]overlay=x=0:y=" +
                             std::to_string(int(H * 0.05)) + ":format=gbrp:eof_action=pass[b" + std::to_string(step + 1) + "];";     // Üstte: altyazıyla çakışmaz
                step++;
            }
        }
        return true;
    };
    std::vector<const json*> tracks;
    for (auto& t : project["tracks"]) tracks.push_back(&t);
    for (auto it = tracks.rbegin(); it != tracks.rend(); ++it) {    // Alttaki kanal önce çizilir
        const json& track = **it;
        if (track.value("kind", std::string()) != "video" || track.value("hidden", false)) continue;
        std::vector<const json*> clips;
        for (auto& c : project["clips"])
            if (c.value("track", std::string()) == track.value("id", std::string()) && clip_has_video(c)) clips.push_back(&c);
        std::sort(clips.begin(), clips.end(), [](const json* a, const json* b) { return a->value("start", 0.0) < b->value("start", 0.0); });
        for (const json* cp : clips) {
            const json& c = *cp;
            const std::string type = c.value("type", std::string());
            const double start = c.value("start", 0.0), length = c.value("length", 0.0), speed = c.value("speed", 1.0);
            const double tail = clip_tail(project, c), span = length + tail;
            if (type == "spot") {   // Spot ışığı: alanın dışını karartan tuval boyutunda resim
                const Bitmap b = spot_overlay(c, W, H);
                const std::wstring png = (fs::path(work) / (L"spot" + std::to_wstring(index) + L".png")).wstring();
                if (!save_png(b, png)) { error = "Spot ışığı hazırlanamadı"; return {}; }
                const std::string n = std::to_string(index++);
                g.inputs.push_back(" -loop 1 -framerate " + fps + " -t " + num(length) + " -i" + quoted(narrow(png)));
                std::string f = "[" + n + ":v]format=gbrap,setpts=PTS-STARTPTS";
                const double fi = c.value("fade_in", 0.0), fo = c.value("fade_out", 0.0);
                if (fi > 0) f += ",fade=t=in:st=0:d=" + num(fi) + ":alpha=1";
                if (fo > 0) f += ",fade=t=out:st=" + num(std::max(0.0, length - fo)) + ":d=" + num(fo) + ":alpha=1";
                f += ",setpts=PTS+" + num(start) + "/TB[v" + n + "];";
                f += "[b" + std::to_string(step) + "][v" + n + "]overlay=x=0:y=0:eof_action=pass:format=gbrp:enable='between(t," + num(start) + "," + num(start + length) + ")'[b" + std::to_string(step + 1) + "];";
                g.filters += f;
                step++;
                continue;
            }
            if (type == "blur") {   // Altındaki görüntünün bir bölgesi bulanıklaşır ya da pikselleşir
                const Placement r = clip_placement(c, float(W), float(H));
                const int x = std::clamp(int(r.x), 0, W - 2) & ~1, y = std::clamp(int(r.y), 0, H - 2) & ~1;
                const int w = std::max(2, std::min(int(r.w), W - x) & ~1), h = std::max(2, std::min(int(r.h), H - y) & ~1);
                const double strength = std::max(1, c.value("strength", 20)) * H / 1080.0;
                const std::string n = std::to_string(step), from = "[b" + n + "]", to = "[b" + std::to_string(step + 1) + "]";
                std::string fx = c.value("mode", std::string("blur")) == "pixel"
                    ? "scale=" + std::to_string(std::max(1, int(w / strength))) + ":" + std::to_string(std::max(1, int(h / strength))) + ":flags=area,scale=" + std::to_string(w) + ":" + std::to_string(h) + ":flags=neighbor"
                    : "gblur=sigma=" + num(strength / 2) + ":steps=3";
                g.filters += from + "split[ra" + n + "][rb" + n + "];[rb" + n + "]crop=" + std::to_string(w) + ":" + std::to_string(h) + ":" + std::to_string(x) + ":" + std::to_string(y) + "," + fx +
                             "[rc" + n + "];[ra" + n + "][rc" + n + "]overlay=x=" + std::to_string(x) + ":y=" + std::to_string(y) + ":format=gbrp:enable='between(t," + num(start) + "," + num(start + length) + ")'" + to + ";";
                step++;
                continue;
            }
            std::string input;
            json placed = c;
            const bool reversed = clip_reversed(c);
            const std::string m_in = c.value("m_in", std::string()), m_out = c.value("m_out", std::string());
            const double md_in = std::max(0.05, c.value("m_in_d", 0.6)), md_out = std::max(0.05, c.value("m_out_d", 0.6));
            if (type == "video") {
                if (!reversed) input = " -ss " + num(c.value("in", 0.0)) + " -t " + num(span * speed) + " -i" + quoted(c.value("file", std::string()));
            } else if (type == "text" && (m_in == "type" || m_out == "type") && text_length(c) > 0) {
                // Daktilo: harf harf resimler sırayla gösterilir
                const int letters = text_length(c), steps_in = std::min(letters, 90);
                std::map<int, std::string> files;
                auto frame_of = [&](int chars) -> std::string {
                    if (files.count(chars)) return files[chars];
                    json partial = c;
                    partial["visible_chars"] = chars;
                    const Bitmap b = render_still(partial);
                    const std::wstring png = (fs::path(work) / (L"yazi" + std::to_wstring(index) + L"_" + std::to_wstring(chars) + L".png")).wstring();
                    if (b.empty() || !save_png(b, png)) return {};
                    placed["w"] = b.w; placed["h"] = b.h;
                    std::string path = narrow(png);
                    std::replace(path.begin(), path.end(), '\\', '/');
                    return files[chars] = path;
                };
                std::vector<std::pair<std::string, double>> list;
                double used = 0;
                if (m_in == "type")
                    for (int k = 0; k < steps_in; k++) { list.push_back({frame_of(k * letters / steps_in), md_in / steps_in}); used += md_in / steps_in; }
                const double out_d = m_out == "type" ? std::min(md_out, std::max(0.0, length - used)) : 0;
                list.push_back({frame_of(letters), std::max(0.04, length - used - out_d)});
                if (m_out == "type")
                    for (int k = steps_in - 1; k >= 0; k--) list.push_back({frame_of(k * letters / steps_in), out_d / steps_in});
                if (tail > 0) list.push_back({frame_of(m_out == "type" ? 0 : letters), tail});
                std::string text = "ffconcat version 1.0\n";
                for (auto& [file, d] : list) {
                    if (file.empty()) { error = "Yazı hazırlanamadı"; return {}; }
                    text += "file '" + file + "'\nduration " + num(d) + "\n";
                }
                text += "file '" + list.back().first + "'\n";
                const fs::path script = fs::path(work) / (L"daktilo" + std::to_wstring(index) + L".txt");
                { std::ofstream o(script, std::ios::binary); o << text; }
                input = " -f concat -safe 0 -t " + num(span) + " -i" + quoted(narrow(script.wstring()));
            } else if (type == "image") {
                input = " -loop 1 -framerate " + fps + " -t " + num(span) + " -i" + quoted(c.value("file", std::string()));
            } else if (type == "color") {
                std::string color = c.value("color", std::string("#000000"));
                color.replace(0, 1, "0x");
                input = " -f lavfi -t " + num(span) + " -i color=c=" + color + ":s=" + size + ":r=" + fps;
            } else if (clip_is_visual_still(c)) {
                const Bitmap b = render_still(c);
                const std::wstring png = (fs::path(work) / (L"yazi" + std::to_wstring(index) + L".png")).wstring();
                if (b.empty() || !save_png(b, png)) { error = "Yazı hazırlanamadı"; return {}; }
                placed["w"] = b.w; placed["h"] = b.h;
                input = " -loop 1 -framerate " + fps + " -t " + num(span) + " -i" + quoted(narrow(png));
            } else {
                continue;
            }
            const int i = index;
            const std::string n = std::to_string(i);
            std::string f, source = "[" + n + ":v]";
            if (reversed) {     // Ters: kaynak kısa parçalara bölünür, her parça tersine çevrilip sondan başa dizilir (bellek sınırlı kalır)
                const double total_source = length * speed, chunk = 1.0;
                const int pieces = std::max(1, int(ceil(total_source / chunk - 1e-6)));
                std::string joined;
                for (int k = pieces - 1; k >= 0; k--) {
                    const double from = c.value("in", 0.0) + k * chunk, d = std::min(chunk, total_source - k * chunk);
                    const std::string m = std::to_string(index++);
                    g.inputs.push_back(" -ss " + num(from) + " -t " + num(std::max(0.04, d)) + " -i" + quoted(c.value("file", std::string())));
                    f += "[" + m + ":v]reverse,setpts=PTS-STARTPTS[rv" + n + "_" + std::to_string(k) + "];";
                    joined += "[rv" + n + "_" + std::to_string(k) + "]";
                }
                f += joined + "concat=n=" + std::to_string(pieces) + ":v=1:a=0[rs" + n + "];";
                source = "[rs" + n + "]";
            } else {
                index++;
                g.inputs.push_back(input);
                if (type == "video") video_inputs[c.value("id", std::string())] = i;
            }
            const Placement p = clip_placement(placed, float(W), float(H));
            const int w = std::max(2, int(p.w + 0.5f) & ~1), h = std::max(2, int(p.h + 0.5f) & ~1);
            f += source + "setpts=(PTS-STARTPTS)/" + num(speed) + ",fps=" + fps + ",settb=AVTB,";
            if (tail > 0 && type == "video") f += "tpad=stop_mode=clone:stop_duration=" + num(tail) + ",";
            const double cl = c.value("crop_l", 0.0) / 100, cr = c.value("crop_r", 0.0) / 100, ct = c.value("crop_t", 0.0) / 100, cb = c.value("crop_b", 0.0) / 100;
            if (cl + cr + ct + cb > 0)
                f += "crop=iw*" + num(1 - cl - cr) + ":ih*" + num(1 - ct - cb) + ":iw*" + num(cl) + ":ih*" + num(ct) + ",";
            if (c.value("flip_h", false)) f += "hflip,";
            if (c.value("flip_v", false)) f += "vflip,";
            const double br = c.value("brightness", 0.0), co = c.value("contrast", 1.0), sa = c.value("saturation", 1.0);
            if (br != 0 || co != 1 || sa != 1) f += "eq=brightness=" + num(br) + ":contrast=" + num(co) + ":saturation=" + num(sa) + ",";
            const double se = c.value("sepia", 0.0) / 100, vi = c.value("vignette", 0.0) / 100, bl = c.value("blur", 0.0);
            if (se > 0) {
                auto mix = [&](double on, double off) { return num(off + (on - off) * se); };
                f += "colorchannelmixer=" + mix(.393, 1) + ":" + mix(.769, 0) + ":" + mix(.189, 0) + ":0:" + mix(.349, 0) + ":" + mix(.686, 1) + ":" + mix(.168, 0) + ":0:" + mix(.272, 0) + ":" + mix(.534, 0) + ":" + mix(.131, 1) + ",";
            }
            if (c.value("invert", false)) f += "negate,";
            if (!c.value("lut", std::string()).empty() && fs::exists(fs::path(widen(c.value("lut", std::string())))))     // Renk filtresi
                f += "lut3d=file='" + filter_escape(widen(c.value("lut", std::string()))) + "':interp=tetrahedral,";
            if (vi > 0) f += "vignette=a=" + num(0.2 + vi * 1.1) + ",";
            if (bl > 0) f += "gblur=sigma=" + num(bl) + ",";
            if (c.value("chroma", false)) {
                std::string key = c.value("chroma_color", std::string("#00ff00"));
                key.replace(0, 1, "0x");
                f += "chromakey=color=" + key + ":similarity=" + num(c.value("chroma_similarity", 0.25)) + ":blend=0.05,";
            }
            f += "scale=" + std::to_string(w) + ":" + std::to_string(h) + ":flags=" + kScaleFlags + ",format=gbrap";
            std::string label = "c" + n;
            f += "[" + label + "];";
            const float unit = H / 1080.f;
            if (clip_has_frame(c)) {    // Köşe ve maske saydamlığı çarpar, kenarlık üstüne çizilir
                const std::wstring base = (fs::path(work) / (L"cerceve" + std::to_wstring(i))).wstring();
                if (!save_png(frame_mask(c, w, h, unit), base + L"_maske.png")) { error = "Maske hazırlanamadı"; return {}; }
                const std::string mi = std::to_string(index++);
                g.inputs.push_back(" -loop 1 -framerate " + fps + " -t " + num(span) + " -i" + quoted(narrow(base + L"_maske.png")));
                f += "[" + mi + ":v]format=gbrap,settb=AVTB[mk" + n + "];[" + label + "][mk" + n + "]blend=c0_mode=normal:c1_mode=normal:c2_mode=normal:c3_mode=multiply:shortest=1[fm" + n + "];";
                label = "fm" + n;
                if (c.value("border", 0) > 0) {
                    if (!save_png(frame_border(c, w, h, unit), base + L"_kenar.png")) { error = "Kenarlık hazırlanamadı"; return {}; }
                    const std::string bi = std::to_string(index++);
                    g.inputs.push_back(" -loop 1 -framerate " + fps + " -t " + num(span) + " -i" + quoted(narrow(base + L"_kenar.png")));
                    f += "[" + bi + ":v]format=gbrap,settb=AVTB[bd" + n + "];[" + label + "][bd" + n + "]overlay=x=0:y=0:format=gbrp:shortest=1[fb" + n + "];";
                    label = "fb" + n;
                }
            }
            if (m_in == "wipe" || m_out == "wipe") {    // Soldan açılma / kapanma: yalnızca hareket süresince
                std::string reveal = "1", when;
                if (m_in == "wipe") { reveal += "*(1-pow(1-clip(T/" + num(md_in) + ",0,1),3))"; when = "lt(t," + num(md_in) + ")"; }
                if (m_out == "wipe") {
                    reveal += "*(1-pow(1-clip((" + num(length) + "-T)/" + num(md_out) + ",0,1),3))";
                    when += std::string(when.empty() ? "" : "+") + "gt(t," + num(length - md_out) + ")";
                }
                f += "[" + label + "]geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='alpha(X,Y)*lte(X,W*" + reveal + ")':enable='" + when + "'[wp" + n + "];";
                label = "wp" + n;
            }
            int fw = w, fh = h;
            if (c.value("shadow", false)) {     // Gölge: renklendirilmiş, kaydırılmış ve yumuşatılmış kopya altta
                const int sx = int(lround(c.value("shadow_x", 10) * unit)), sy = int(lround(c.value("shadow_y", 14) * unit));
                const double soft = std::max(0, c.value("shadow_blur", 24)) * unit;
                const int margin = (int(std::max(abs(sx), abs(sy)) + soft * 1.5 + 4) + 1) & ~1;
                const uint32_t rgb = parse_color(c.value("shadow_color", std::string("#000000")));
                const std::string mg = std::to_string(margin), mg2 = std::to_string(2 * margin);
                f += "[" + label + "]split[sa" + n + "][sb" + n + "];[sb" + n + "]lutrgb=r=" + std::to_string((rgb >> 16) & 255) + ":g=" + std::to_string((rgb >> 8) & 255) +
                     ":b=" + std::to_string(rgb & 255) + ":a='val*" + num(std::clamp(c.value("shadow_opacity", 60), 0, 100) / 100.0) + "',pad=iw+" + mg2 + ":ih+" + mg2 + ":" +
                     std::to_string(margin + sx) + ":" + std::to_string(margin + sy) + ":color=black@0";
                if (soft >= 1) f += ",gblur=sigma=" + num(soft / 2);
                f += "[sc" + n + "];[sa" + n + "]pad=iw+" + mg2 + ":ih+" + mg2 + ":" + mg + ":" + mg + ":color=black@0[sd" + n + "];[sc" + n + "][sd" + n + "]overlay=x=0:y=0:format=gbrp[sh" + n + "];";
                label = "sh" + n;
                fw += 2 * margin; fh += 2 * margin;
            }
            const std::string box = "s=" + std::to_string(fw) + "x" + std::to_string(fh) + ":r=" + fps;
            const Transition tin = transition_in(project, c), tout = transition_out(project, c);
            const std::string t_in = tin.key, t_out = tout.key;
            const double di = tin.duration, dv = tout.duration;
            // Hareketin ölçek ve kayma ifadeleri; T klip içindeki zaman
            auto motion = [&](const std::string& T, std::string& scale, std::string& dx, std::string& dy) {
                scale = "1"; dx = "0"; dy = "0";
                for (int side = 0; side < 2; side++) {
                    const std::string kind = side == 0 ? m_in : m_out;
                    if (kind.empty()) continue;
                    const double sign = side == 0 ? 1 : -1;
                    const std::string u = side == 0 ? "clip(" + T + "/" + num(md_in) + ",0,1)" : "clip((" + num(length) + "-" + T + ")/" + num(md_out) + ",0,1)";
                    const std::string rest = "pow(1-" + u + ",3)";    // 1 - kolay çıkış
                    if (kind == "rise") dy += "+(" + num(sign * 0.06) + ")*" + rest;
                    if (kind == "drop") dy += "-(" + num(sign * 0.08) + ")*" + rest;
                    if (kind == "slide") dx += "-(" + num(sign * 0.12) + ")*" + rest;
                    if (kind == "pop") scale += "*max(0.002,1+2.70158*pow(" + u + "-1,3)+1.70158*pow(" + u + "-1,2))";
                }
            };
            if (find_transition(t_in)) {    // Saydam katmandan klibe geçiş: xfade saydamlığı da işler
                f += "color=black@0.0:" + box + ":d=" + num(di) + ",format=gbrap,settb=AVTB[ti" + n + "];";
                f += "[ti" + n + "][" + label + "]xfade=transition=" + t_in + ":duration=" + num(di) + ":offset=0[d" + n + "];";
                label = "d" + n;
            }
            if (find_transition(t_out)) {
                f += "color=black@0.0:" + box + ":d=" + num(dv) + ",format=gbrap,settb=AVTB[to" + n + "];";
                f += "[" + label + "][to" + n + "]xfade=transition=" + t_out + ":duration=" + num(dv) + ":offset=" + num(std::max(0.0, span - dv)) + "[e" + n + "];";
                label = "e" + n;
            }
            f += "[" + label + "]format=gbrap";
            const double fi = c.value("fade_in", 0.0), fo = c.value("fade_out", 0.0);
            if (fi > 0) f += ",fade=t=in:st=0:d=" + num(fi) + ":alpha=1";
            if (fo > 0) f += ",fade=t=out:st=" + num(std::max(0.0, length - fo)) + ":d=" + num(fo) + ":alpha=1";
            for (int side = 0; side < 2; side++) {      // Hareketin saydamlığı doğrusal: fade ile aynı
                const std::string kind = side == 0 ? m_in : m_out;
                if (kind.empty() || kind == "wipe" || kind == "type") continue;
                const double d = (side == 0 ? md_in : md_out) / (kind == "pop" ? 3 : 1);
                if (side == 0) f += ",fade=t=in:st=0:d=" + num(d) + ":alpha=1";
                else f += ",fade=t=out:st=" + num(std::max(0.0, length - d)) + ":d=" + num(d) + ":alpha=1";
            }
            std::string m_scale, m_dx, m_dy, g_scale, g_dx, g_dy;
            motion("t", m_scale, m_dx, m_dy);
            motion("(t-" + num(start) + ")", g_scale, g_dx, g_dy);     // Bindirmede zaman tuvalin zamanıdır
            const auto keys = clip_keys(c);
            if (!keys.empty()) {    // Animasyon noktaları: ölçek, konum, dönüş ve opaklık kareden kareye
                auto fs = [](const Key& k) { return k.scale; };
                auto fx = [](const Key& k) { return k.x; };
                auto fy = [](const Key& k) { return k.y; };
                auto fr = [](const Key& k) { return k.rotate; };
                auto fo = [](const Key& k) { return k.opacity; };
                if (key_varies(keys, fo)) f += ",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='alpha(X,Y)*(" + key_expr(keys, fo, "T") + ")/100'";
                else if (keys.front().opacity < 100) f += ",colorchannelmixer=aa=" + num(keys.front().opacity / 100.0);
                double bw = fw, bh = fh;
                if (key_varies(keys, fr) || keys.front().rotate != 0) {
                    const std::string a = "(" + key_expr(keys, fr, "t") + ")*PI/180";
                    const double side = ceil(hypot(double(fw), double(fh)) / 2) * 2;
                    f += ",rotate=a='" + a + "':ow=" + num(side) + ":oh=" + num(side) + ":c=black@0";    // none boş alanı temizlemiyor, açı değişince iz kalıyor
                    bw = bh = side;
                }
                // Ölçek klibin kendi ölçeğine göre oranlanır; konum tuval ortasından
                const double base = std::max(1.0, c.value("scale", 100.0));
                const std::string k = "(" + key_expr(keys, fs, "t") + ")/" + num(base) + "*(" + m_scale + ")";
                f += ",scale=w='max(2,trunc(" + num(bw) + "*" + k + "/2)*2)':h='max(2,trunc(" + num(bh) + "*" + k + "/2)*2)':eval=frame:flags=" + kScaleFlags;
                const std::string T = "(t-" + num(start) + ")";
                const std::string x = "(" + num(W / 2.0) + "+(" + key_expr(keys, fx, T) + "+" + g_dx + ")*" + num(W) + ")-overlay_w/2";
                const std::string y = "(" + num(H / 2.0) + "+(" + key_expr(keys, fy, T) + "+" + g_dy + ")*" + num(H) + ")-overlay_h/2";
                f += ",setpts=PTS+" + num(start) + "/TB[v" + n + "];";
                f += "[b" + std::to_string(step) + "][v" + n + "]overlay=x='" + x + "':y='" + y + "':eof_action=pass:format=gbrp:eval=frame:enable='between(t," + num(start) + "," + num(start + span) + ")'[b" + std::to_string(step + 1) + "];";
                g.filters += f;
                step++;
                if (!input_effects(c, n)) { error = "İmleç efektleri hazırlanamadı"; return {}; }
                continue;
            }
            const int opacity = c.value("opacity", 100);
            if (opacity < 100) f += ",colorchannelmixer=aa=" + num(opacity / 100.0);
            const double angle = c.value("rotate", 0.0);
            if (angle != 0) {
                const std::string a = "(" + num(angle) + "*PI/180)";
                f += ",rotate=" + a + ":ow=rotw(" + a + "):oh=roth(" + a + "):c=black@0";
            }
            if (m_scale != "1")
                f += ",scale=w='max(2,trunc(iw*" + m_scale + "/2)*2)':h='max(2,trunc(ih*" + m_scale + "/2)*2)':eval=frame:flags=" + kScaleFlags;
            std::string x = num(p.x + p.w / 2) + "+(" + g_dx + ")*" + num(W) + "-overlay_w/2", y = num(p.y + p.h / 2) + "+(" + g_dy + ")*" + num(H) + "-overlay_h/2";
            if (c.value("anim", false) && angle == 0) {     // Yavaşça yakınlaşma ve kayma
                json last = placed;
                last["scale"] = c.value("anim_scale", 130.0);
                last["x"] = c.value("anim_x", 0.0);
                last["y"] = c.value("anim_y", 0.0);
                const Placement e = clip_placement(last, float(W), float(H));
                const std::string k = "min(1,t/" + num(length) + ")";
                f += ",scale=w='trunc((" + num(p.w) + "+(" + num(e.w - p.w) + ")*" + k + ")/2)*2':h='trunc((" + num(p.h) + "+(" + num(e.h - p.h) + ")*" + k + ")/2)*2':eval=frame:flags=" + kScaleFlags;
                const std::string at = "min(1,(t-" + num(start) + ")/" + num(length) + ")";
                x = "(" + num(p.x + p.w / 2) + "+(" + num(e.x + e.w / 2 - p.x - p.w / 2) + ")*" + at + ")-overlay_w/2";
                y = "(" + num(p.y + p.h / 2) + "+(" + num(e.y + e.h / 2 - p.y - p.h / 2) + ")*" + at + ")-overlay_h/2";
            }
            f += ",setpts=PTS+" + num(start) + "/TB[v" + n + "];";
            f += "[b" + std::to_string(step) + "][v" + n + "]overlay=x='" + x + "':y='" + y + "':eof_action=pass:format=gbrp:eval=frame:enable='between(t," + num(start) + "," + num(start + span) + ")'[b" + std::to_string(step + 1) + "];";
            g.filters += f;
            step++;
            if (!input_effects(c, n)) { error = "İmleç efektleri hazırlanamadı"; return {}; }
        }
    }
    if (project.value("captions_burn", true) && project.contains("captions") && project["captions"].is_array() && !project["captions"].empty()) {
        // Altyazı videoya yazılır: hepsi tek şeritte
        const json style = project.value("caption_style", default_caption_style());
        std::vector<std::tuple<double, double, Bitmap>> items;
        int sh = 2;
        for (auto& cap : project["captions"]) {
            const std::string text = cap.value("text", std::string());
            if (text.empty()) continue;
            Bitmap b = caption_bitmap(text, style, unit);
            sh = std::max(sh, b.h);
            items.push_back({cap.value("start", 0.0), cap.value("end", 0.0), std::move(b)});
        }
        int input;
        sh = even(sh + 4);
        if (!items.empty()) {
            if (!strip(std::move(items), sh, L"altyazi", input)) { error = "Altyazı hazırlanamadı"; return {}; }
            const int y = std::clamp(int(H / 2.0 + style.value("y", 0.40) * H - sh / 2.0), 0, std::max(0, H - sh));
            g.filters += "[" + std::to_string(input) + ":v]format=gbrap,setpts=PTS-STARTPTS[cap];[b" + std::to_string(step) + "][cap]overlay=x=0:y=" + std::to_string(y) +
                         ":format=gbrp:eof_action=pass[b" + std::to_string(step + 1) + "];";
            step++;
        }
    }
    const std::string mode = project.value("export_mode", std::string("video"));
    if (mode == "gif") {    // Renk paleti videodan çıkarılır: GIF'te bantlanma az olur
        g.filters += "[b" + std::to_string(step) + "]fps=" + std::to_string(project.value("gif_fps", 15)) + ",scale=" + std::to_string(project.value("gif_width", 640)) +
                     ":-2:flags=lanczos,format=rgb24,split[g1][g2];[g1]palettegen=stats_mode=diff[gp];[g2][gp]paletteuse=dither=bayer:bayer_scale=4[vout];";
    } else if (mode == "frame") {
        g.filters += "[b" + std::to_string(step) + "]format=rgb24[vout];";
    } else {
        g.filters += "[b" + std::to_string(step) + "]scale=flags=" + kScaleFlags + ":out_color_matrix=bt709:out_range=tv,format=yuv420p[vout];";
    }
    const bool with_audio = mode == "video" && config.audio;
    bool any_audio = false, normalize = false;
    if (with_audio) {
        add_audio(project, 0, total + 1, g, index, &video_inputs, any_audio);
        if (!any_audio) g.filters += "anullsrc=r=48000:cl=stereo:d=" + num(total) + "[aout];";
        normalize = any_audio && project.value("loudnorm", false);
        if (normalize) g.filters += "[aout]loudnorm=I=-14:TP=-1.5:LRA=11,aresample=48000[anorm];";      // YouTube ses seviyesi
    }
    g.filters.pop_back();

    // Bölümler: oynatıcılarda ve YouTube'da bölüm listesi olarak görünür
    int chapters_input = -1;
    const auto chapters = project_chapters(project);
    if (mode == "video" && project.value("chapters_embed", true) && !chapters.empty() && (config.container == "mp4" || config.container == "mkv" || config.container == "mov")) {
        auto list = chapters;
        if (list.front().first > 0.5) list.insert(list.begin(), {0.0, std::string()});
        auto escape = [](const std::string& s) {
            std::string out;
            for (char ch : s) { if (ch == '=' || ch == ';' || ch == '#' || ch == '\\' || ch == '\n') out += '\\'; out += ch; }
            return out;
        };
        std::string meta = ";FFMETADATA1\n";
        for (size_t i = 0; i < list.size(); i++) {
            const double a = list[i].first, b = i + 1 < list.size() ? list[i + 1].first : total;
            if (b - a < 0.05) continue;
            const std::string title = list[i].second.empty() ? std::string(i == 0 ? T("Giriş") : TF("Bölüm %d", int(i + 1))) : list[i].second;
            meta += "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" + std::to_string((long long)(a * 1000)) + "\nEND=" + std::to_string((long long)(b * 1000)) + "\ntitle=" + escape(title) + "\n";
        }
        const fs::path file = fs::path(work) / L"bolumler.txt";
        { std::ofstream o(file, std::ios::binary); o << meta; }
        chapters_input = index++;
        g.inputs.push_back(" -f ffmetadata -i" + quoted(narrow(file.wstring())));
    }

    const fs::path script = fs::path(work) / L"grafik.txt";
    { std::ofstream o(script, std::ios::binary); o << g.filters; }
    std::string cmd = " -hide_banner -loglevel error -nostdin -nostats -progress pipe:1";
    for (auto& i : g.inputs) cmd += i;
    cmd += " -/filter_complex" + quoted(narrow(script.wstring())) + " -map [vout]";
    if (mode == "gif") return L"\"" + ffmpeg_exe() + L"\"" + widen(cmd + " -t " + num(total) + " -loop 0") + L" -y \"" + config.path + L"\"";
    if (mode == "frame") return L"\"" + ffmpeg_exe() + L"\"" + widen(cmd + " -frames:v 1 -update 1") + L" -y \"" + config.path + L"\"";
    if (with_audio) cmd += normalize ? " -map [anorm]" : " -map [aout]";
    if (chapters_input >= 0) cmd += " -map_chapters " + std::to_string(chapters_input);
    cmd += encode_options(config) + " -t " + num(total);
    std::wstring line = L"\"" + ffmpeg_exe() + L"\"" + widen(cmd);
    if (config.container == "mp4" || config.container == "mov") line += L" -movflags +faststart";
    return line + L" -y \"" + config.path + L"\"";
}

// Yalnız ses: görüntü hiç işlenmez
std::wstring export_audio_command(const json& project, const EncodeConfig& config, const std::wstring& work, std::string& error) {
    const double total = project_length(project);
    Graph g;
    int index = 0;
    bool any = false;
    add_audio(project, 0, total + 1, g, index, nullptr, any);
    if (!any) { error = T("Projede ses yok"); return {}; }
    const bool normalize = project.value("loudnorm", false);
    if (normalize) g.filters += "[aout]loudnorm=I=-14:TP=-1.5:LRA=11,aresample=48000[anorm];";
    g.filters.pop_back();
    const fs::path script = fs::path(work) / L"grafik.txt";
    { std::ofstream o(script, std::ios::binary); o << g.filters; }
    std::string cmd = " -hide_banner -loglevel error -nostdin -nostats -progress pipe:1";
    for (auto& i : g.inputs) cmd += i;
    cmd += " -/filter_complex" + quoted(narrow(script.wstring())) + (normalize ? " -map [anorm]" : " -map [aout]");
    const std::string kbps = std::to_string(std::max(64, config.audio_kbps)) + "k";
    if (config.container == "mp3") cmd += " -c:a libmp3lame -b:a " + kbps;
    else if (config.container == "wav") cmd += " -c:a pcm_s16le";
    else cmd += " -c:a aac -b:a " + kbps;
    cmd += " -ar 48000 -t " + num(total);
    return L"\"" + ffmpeg_exe() + L"\"" + widen(cmd) + L" -y \"" + config.path + L"\"";
}

void export_start(ExportJob& job, const json& project, const EncodeConfig& config) {
    job.path = config.path;
    job.progress = 0; job.done = false; job.cancel = false; job.error.clear();
    job.thread = std::thread([&job, project, config] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const fs::path work = fs::path(temp) / (L"video-editor-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::create_directories(work, ec);
        const double total = project_length(project);
        std::string error;
        const std::wstring command = project.value("export_mode", std::string()) == "audio" ? export_audio_command(project, config, work.wstring(), error)
                                                                                            : export_command(project, config, work.wstring(), error);
        if (!command.empty()) {
            Process p;
            if (p.start(command, true)) {
                std::string text, line;
                char buffer[2048];
                size_t got;
                while (!job.cancel && (got = p.read_some(buffer, sizeof(buffer)))) {
                    text.append(buffer, got);
                    size_t end;
                    while ((end = text.find('\n')) != std::string::npos) {
                        line = text.substr(0, end);
                        text.erase(0, end + 1);
                        if (line.rfind("out_time_us=", 0) == 0) job.progress = std::clamp(float(atof(line.c_str() + 12) / 1e6 / total), 0.f, 1.f);
                    }
                }
                if (job.cancel) {
                    p.kill();
                    fs::remove(config.path, ec);
                } else {
                    const int code = p.wait();
                    Sleep(20);
                    p.kill();
                    if (code != 0) {
                        error = p.error_text();
                        while (!error.empty() && isspace(uint8_t(error.back()))) error.pop_back();
                        const size_t nl = error.find_last_of('\n');
                        if (nl != std::string::npos) error = error.substr(nl + 1);
                        if (error.empty()) error = "Kodlayıcı durdu";
                    }
                }
            } else {
                error = "FFmpeg başlatılamadı";
            }
        }
        job.error = error;
        fs::remove_all(work, ec);
        CoUninitialize();
        job.done = true;
    });
}

// --- Ses çalma ---

void AudioPlayer::start(const std::wstring& command, double from) {
    stop();
    from_ = from;
    played_ = 0;
    started_ = false;
    halt_ = false;
    thread_ = std::thread(&AudioPlayer::loop, this, command);
}

void AudioPlayer::stop() {
    halt_ = true;
    if (thread_.joinable()) thread_.join();
    started_ = false;
}

double AudioPlayer::position() const { return from_ + double(played_.load()) / 48000.0; }

void AudioPlayer::loop(std::wstring command) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
        UINT32 capacity = 0;
        const bool audio_ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                              SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
                              SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                              SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 1000000, 0, &wf, nullptr)) &&
                              SUCCEEDED(client->GetBufferSize(&capacity)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&render)));
        Process proc;
        const bool have_stream = audio_ok && !command.empty() && proc.start(command);
        std::vector<uint8_t> chunk;
        size_t chunk_at = 0;
        bool ended = !have_stream;
        long long written = 0;
        if (audio_ok) client->Start();
        const double wall = now();
        while (!halt_) {
            if (!audio_ok) {    // Ses aygıtı yoksa da saat işlesin
                played_ = (long long)((now() - wall) * 48000);
                Sleep(5);
                continue;
            }
            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            const UINT32 room = capacity - padding;
            if (room > 480) {
                BYTE* out = nullptr;
                if (SUCCEEDED(render->GetBuffer(room, &out))) {
                    size_t need = size_t(room) * 4, at = 0;
                    while (at < need) {
                        if (chunk_at >= chunk.size()) {
                            if (ended) break;
                            chunk.resize(19200);    // 100 ms
                            const size_t got = proc.read(chunk.data(), chunk.size());
                            chunk.resize(got);
                            chunk_at = 0;
                            if (got < 19200) ended = true;
                            if (!got) break;
                        }
                        const size_t part = std::min(need - at, chunk.size() - chunk_at);
                        memcpy(out + at, chunk.data() + chunk_at, part);
                        at += part;
                        chunk_at += part;
                    }
                    if (at < need) memset(out + at, 0, need - at);     // Ses bitince sessizlik
                    render->ReleaseBuffer(room, 0);
                    written += room;
                }
            }
            started_ = true;
            client->GetCurrentPadding(&padding);
            played_ = std::max(0LL, written - (long long)padding);
            Sleep(4);
        }
        if (audio_ok) client->Stop();
    }
    CoUninitialize();
}

// --- Mikrofon kaydı ---

bool MicRecorder::start(const std::wstring& path) {
    stop();
    halt_ = false;
    frames_ = 0;
    thread_ = std::thread(&MicRecorder::loop, this, path);
    return true;
}

double MicRecorder::stop() {
    halt_ = true;
    if (thread_.joinable()) thread_.join();
    return seconds();
}

void MicRecorder::loop(std::wstring path) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = 48000 * 4;
        std::ofstream file(fs::path(path), std::ios::binary);
        char header[44] = {};
        file.write(header, 44);
        const bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                        SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) &&
                        SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 2000000, 0, &wf, nullptr)) &&
                        SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
        std::vector<char> zeros;
        while (ok && !halt_) {
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
                BYTE* data = nullptr;
                UINT32 count = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr))) break;
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    zeros.assign(size_t(count) * 4, 0);
                    file.write(zeros.data(), zeros.size());
                } else {
                    file.write(reinterpret_cast<char*>(data), std::streamsize(count) * 4);
                    int top = 0;
                    const int16_t* s = reinterpret_cast<int16_t*>(data);
                    for (UINT32 i = 0; i < count * 2; i++) top = std::max(top, abs(int(s[i])));
                    level_ = std::max(top / 32768.f, level_.load() * 0.9f);
                }
                frames_ += count;
                capture->ReleaseBuffer(count);
            }
            Sleep(5);
        }
        if (client) client->Stop();
        const uint32_t bytes = uint32_t(frames_.load() * 4), riff = bytes + 36, fmt = 16, rate = 48000, byte_rate = 192000;
        const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
        file.seekp(0);
        file.write("RIFF", 4); file.write(reinterpret_cast<const char*>(&riff), 4); file.write("WAVEfmt ", 8);
        file.write(reinterpret_cast<const char*>(&fmt), 4); file.write(reinterpret_cast<const char*>(&pcm), 2);
        file.write(reinterpret_cast<const char*>(&channels), 2); file.write(reinterpret_cast<const char*>(&rate), 4);
        file.write(reinterpret_cast<const char*>(&byte_rate), 4); file.write(reinterpret_cast<const char*>(&align), 2);
        file.write(reinterpret_cast<const char*>(&bits), 2); file.write("data", 4); file.write(reinterpret_cast<const char*>(&bytes), 4);
    }
    CoUninitialize();
}

// --- Küçük resim ve dalga biçimi ---

std::vector<float> read_waveform(const std::wstring& file, double duration, int stream) {
    Process p;
    std::vector<float> peaks;
    if (!p.start(L"\"" + ffmpeg_exe() + L"\" -hide_banner -loglevel quiet -nostdin -i \"" + file + L"\" -map 0:a:" + std::to_wstring(stream) +
                 L" -ac 1 -ar 4800 -f s16le pipe:1")) return peaks;
    peaks.reserve(size_t(duration * 100) + 16);
    std::vector<int16_t> block(48);
    for (;;) {
        const size_t got = p.read(block.data(), block.size() * 2);
        if (got < block.size() * 2) break;
        int top = 0;
        for (int16_t v : block) top = std::max(top, abs(int(v)));
        peaks.push_back(top / 32768.f);
    }
    return peaks;
}

bool read_thumbnail(const std::wstring& file, double time, int w, int h, Bitmap& out) {
    wchar_t tail[128];
    swprintf_s(tail, L"\" -frames:v 1 -vf scale=%d:%d -f rawvideo -pix_fmt bgra pipe:1", w, h);
    wchar_t head[96];
    swprintf_s(head, L" -hide_banner -loglevel quiet -nostdin -ss %.3f -i \"", time);
    Process p;
    if (!p.start(L"\"" + ffmpeg_exe() + L"\"" + head + file + tail)) return false;
    out.w = w; out.h = h;
    out.px.resize(size_t(w) * h * 4);
    return p.read(out.px.data(), out.px.size()) == out.px.size();
}
