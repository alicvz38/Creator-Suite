// Video Düzenleyici: Kayıt Stüdyosu'yla aynı tema ve panel dilinde çok kanallı kurgu programı.
#include "ui.h"
#include "editor_core.h"

#include <d3dcompiler.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

json default_social() { return json::object(); }

namespace {

const char* kAppName = "Video Düzenleyici";
const char* kVersion = build_version();      // Derleme anı: yyyy.aa.gg.ss.dd.ss
const float kTrackH = 80, kHeaderW = 160, kRulerH = 24, kEdge = 7, kSnapPx = 9;
const size_t kMaxUndo = 100;
#define ICON_PLAY "\xEE\x9D\xA8"
#define ICON_PAUSE "\xEE\x9D\xA9"
#define ICON_PREV "\xEE\xA2\x92"
#define ICON_NEXT "\xEE\xA2\x93"
#define ICON_ADD "\xEE\x9C\x90"
#define ICON_DELETE "\xEE\x9D\x8D"
#define ICON_SETTINGS "\xEE\x9C\x93"
#define ICON_BACK "\xEE\x9C\xAB"
#define ICON_UNDO "\xEE\x9E\xA7"
#define ICON_REDO "\xEE\x9E\xA6"
#define ICON_CUT "\xEE\xA3\x86"
#define ICON_KEYS "\xEE\x9D\xA5"

const char* kShader = R"(
Texture2D tex : register(t0);
Texture3D lut : register(t1);       // Renk filtresi; pad.x boyutu (0: yok)
SamplerState smp : register(s0);
cbuffer Quad : register(b0) {
    float4 rect; float4 crop;
    float opacity; float brightness; float contrast; float saturation;
    float angle; float aspect; float blur; float vignette;
    float sepia; float invert; float chroma; float similarity;
    float3 key; float mask;
    float progress; float3 pad;
    float4 frame;    // xy: klibin piksel boyutu, z: köşe, w: kenarlık
    float4 frame2;   // x: elips, y: yumuşak kenar, z: 1 gölge geçişi, w: soldan açılma
    float4 maskbox;  // xy: maske merkezi, zw: yarı boyut (piksel)
    float4 bcolor;   // kenarlık rengi, w: çerçeve açık
    float4 scolor;   // gölge rengi
    float4 expand;   // xy: gölge dörtgeninin genişlemesi (uv)
};
float sdf(float2 q, float2 hs, float r, float ellipse) {
    hs = max(hs, 0.5);
    if (ellipse > 0) return (length(q / hs) - 1) * min(hs.x, hs.y);
    r = clamp(r, 0, min(hs.x, hs.y));
    float2 a = abs(q) - hs + r;
    return length(max(a, 0)) + min(max(a.x, a.y), 0) - r;
}
struct VS { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS vs(uint id : SV_VertexID) {
    float2 c = float2(id & 1, id >> 1);
    float2 box = rect.zw * float2(aspect, 1);
    float2 d = (c - 0.5) * box;
    float s = sin(angle), k = cos(angle);
    d = float2(d.x * k - d.y * s, d.x * s + d.y * k) / float2(aspect, 1);
    float2 p = rect.xy + rect.zw * 0.5 + d;
    VS o;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    o.uv = c;
    return o;
}
float4 read(float2 uv) {
    if (blur <= 0) return tex.Sample(smp, uv);
    float4 c = tex.Sample(smp, uv);
    float r = blur / 540.0;
    for (int i = 0; i < 12; i++) {
        float a = i * 0.5236;
        float2 d = float2(cos(a) / aspect, sin(a)) * r;
        c += tex.Sample(smp, uv + d) + tex.Sample(smp, uv + d * 0.5);
    }
    return c / 25.0;
}
float4 ps(VS i) : SV_Target {
    float shadow = frame2.z == 1 ? 1 : 0;
    float2 q = i.uv;
    if (shadow > 0) q = q * (1 + 2 * expand.xy) - expand.xy;
    float2 uv = crop.xy + q * crop.zw;
    if (mask == 9) { float n = 8 + 150 * progress * progress; uv = (floor(uv * n) + 0.5) / n; }
    float4 c = read(uv);
    if (c.a > 0) c.rgb /= c.a;
    if (chroma > 0 && distance(c.rgb, key) < similarity * 1.7) c.a = 0;
    c.rgb = (c.rgb - 0.5) * contrast + 0.5 + brightness;
    float gray = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    c.rgb = lerp(float3(gray, gray, gray), c.rgb, saturation);
    if (sepia > 0) c.rgb = lerp(c.rgb, float3(dot(c.rgb, float3(.393, .769, .189)), dot(c.rgb, float3(.349, .686, .168)), dot(c.rgb, float3(.272, .534, .131))), sepia);
    if (invert > 0) c.rgb = 1 - c.rgb;
    if (pad.x > 0) { float n = pad.x; c.rgb = lut.Sample(smp, saturate(c.rgb) * (n - 1) / n + 0.5 / n).rgb; }
    if (vignette > 0) c.rgb *= 1 - vignette * saturate(dot(i.uv - 0.5, i.uv - 0.5) * 2.2);
    c.rgb = saturate(c.rgb);
    float ring = 0;
    if (bcolor.w > 0) {     // Köşe, maske ve kenarlık
        float d = sdf(q * frame.xy - maskbox.xy, maskbox.zw, frame.z, frame2.x);
        if (shadow > 0) c.a *= saturate(0.5 - d / max(frame2.y, 1));
        else {
            c.a *= frame2.y > 0.5 ? saturate(-d / frame2.y) : saturate(0.5 - d);
            if (frame.w > 0) ring = saturate(0.5 - d) * saturate(d + frame.w + 0.5);
        }
    }
    float m = 1;
    if (mask == 1) m = q.x > 1 - progress;
    if (mask == 2) m = q.x < progress;
    if (mask == 3) m = q.y > 1 - progress;
    if (mask == 4) m = q.y < progress;
    if (mask == 5) m = length((q - 0.5) * float2(aspect, 1)) < progress * 0.5 * sqrt(aspect * aspect + 1);
    if (mask == 6) m = abs(q.x - 0.5) < progress / 2 && abs(q.y - 0.5) < progress / 2;
    if (mask == 7) m = frac(sin(dot(floor(q * 400), float2(12.9898, 78.233))) * 43758.5453) < progress;
    if (mask == 8) m = (atan2(q.x - 0.5, 0.5 - q.y) / 6.28318 + 0.5 < 0.5 ? atan2(q.x - 0.5, 0.5 - q.y) / 6.28318 + 1 : atan2(q.x - 0.5, 0.5 - q.y) / 6.28318) < progress;
    if (frame2.w < 1) m *= q.x <= frame2.w;     // Soldan açılma
    if (shadow > 0) return float4(scolor.rgb * c.a, c.a) * opacity * m;
    c.rgb *= c.a;
    c = lerp(c, float4(bcolor.rgb, 1), ring);
    return c * opacity * m;
}
// İmleç vurgusu (dolu yumuşak daire) ve tıklama halkası
float4 ps_dot(VS i) : SV_Target {
    float r = length(i.uv - 0.5) * 2;
    float a = frame2.x > 0 ? saturate(1 - abs(r - 0.85) / 0.12) : saturate((1 - r) / 0.35);
    return float4(scolor.rgb * a, a) * opacity;
}
// Spot ışığı: alanın dışı kararır
float4 ps_spot(VS i) : SV_Target {
    float d = sdf(i.uv * frame.xy - maskbox.xy, maskbox.zw, frame.z, frame2.x);
    return float4(0, 0, 0, opacity * saturate(d / max(frame2.y, 1) + 0.5));
}
// Bulanık / pikselli alan: altındaki görüntünün kopyasından okur
float4 ps_region(VS i) : SV_Target {
    float2 uv = rect.xy + i.uv * rect.zw;
    float s = max(blur, 1) / 1080.0;
    if (invert > 0) {
        float2 bs = float2(s / aspect, s);
        return float4(tex.Sample(smp, (floor(uv / bs) + 0.5) * bs).rgb, 1);
    }
    float3 c = 0;
    for (int x = -3; x <= 3; x++)
        for (int y = -3; y <= 3; y++) c += tex.Sample(smp, uv + float2(x / aspect, y) * s * 0.35).rgb;
    return float4(c / 49.0, 1);
}
)";

struct Quad {
    float rect[4], crop[4];
    float opacity, brightness, contrast, saturation, angle, aspect, blur, vignette, sepia, invert, chroma, similarity;
    float key[3], mask, progress, pad[3];
    float frame[4], frame2[4], maskbox[4], bcolor[4], scolor[4], expand[4];
};

struct Texture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

struct Media {
    std::wstring path;
    std::string name;
    MediaInfo info;
    Texture thumb;
    bool ready = false, failed = false;
};

struct LiveClip {       // Önizlemede açık bir video klip
    std::unique_ptr<VideoFeed> feed;
    Texture frame;
    std::string key;
    double origin = 0, restarted = 0, span = 1e9;    // span: ters klipte çözülen parçanın zaman çizelgesindeki süresi
    int w = 0, h = 0, index = -1;
};

struct CachedTexture { std::string key; Texture tex; };
struct LutTexture { ComPtr<ID3D11ShaderResourceView> srv; int size = 0; bool tried = false; };

enum class Drag { None, Move, TrimLeft, TrimRight, Seek, Box };

std::wstring pick_files(HWND owner, std::vector<std::wstring>& out, const wchar_t* name, const wchar_t* filter, bool many) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC spec[] = {{name, filter}, {L"Tüm dosyalar", L"*.*"}};
    dlg->SetFileTypes(2, spec);
    DWORD options = 0;
    dlg->GetOptions(&options);
    if (many) dlg->SetOptions(options | FOS_ALLOWMULTISELECT);
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return {};
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; i++) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out.push_back(path);
        CoTaskMemFree(path);
    }
    return out.empty() ? L"" : out[0];
}

std::wstring pick_save(HWND owner, const wchar_t* name, const wchar_t* filter, const wchar_t* extension, const std::wstring& suggested) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC spec[] = {{name, filter}};
    dlg->SetFileTypes(1, spec);
    dlg->SetDefaultExtension(extension);
    if (!suggested.empty()) dlg->SetFileName(suggested.c_str());
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::string format_time(double t, int fps) {
    if (t < 0) t = 0;
    const int total = int(t);
    char text[32];
    sprintf_s(text, "%02d:%02d:%02d.%02d", total / 3600, total / 60 % 60, total % 60, int((t - total) * fps));
    return text;
}

bool slider(const char* label, json& j, const char* key, float lo, float hi, const char* format = "%.2f") {
    float v = float(j.value(key, 0.0));
    if (!slider_float(label, &v, lo, hi, format)) return false;
    j[key] = double(v);
    return true;
}

std::wstring templates_dir();

// Kanalın gösterilen adı: değiştirilmemiş "Görüntü 2" / "Ses 1" seçili dilde yazılır
std::string track_label(const json& track) {
    const std::string name = track.value("name", std::string());
    int n = 0;
    if (sscanf_s(name.c_str(), "Görüntü %d", &n) == 1 && name == "Görüntü " + std::to_string(n)) return TF("Görüntü %d", n);
    if (sscanf_s(name.c_str(), "Ses %d", &n) == 1 && name == "Ses " + std::to_string(n)) return TF("Ses %d", n);
    return name;
}

ImU32 clip_color(const std::string& type, bool selected) {
    ImVec4 c = type == "audio" ? ImVec4(0.18f, 0.55f, 0.42f, 1) : type == "blur" ? ImVec4(0.38f, 0.38f, 0.52f, 1) : type == "spot" ? ImVec4(0.55f, 0.50f, 0.20f, 1) : type == "text" || type == "shape" ? ImVec4(0.62f, 0.42f, 0.14f, 1)
             : type == "image" ? ImVec4(0.45f, 0.30f, 0.62f, 1) : type == "color" ? ImVec4(0.42f, 0.42f, 0.48f, 1) : ImVec4(0.18f, 0.38f, 0.68f, 1);
    if (selected) { c.x = std::min(1.f, c.x + 0.14f); c.y = std::min(1.f, c.y + 0.14f); c.z = std::min(1.f, c.z + 0.14f); }
    return ImGui::ColorConvertFloat4ToU32(c);
}

class App {
public:
    HWND hwnd = nullptr;
    bool want_close = false, start_hidden = false;
    std::vector<std::wstring> open_args;
    void hide_to_tray();
    void on_tray(UINT event);
    void show_window() { window_show(hwnd); }
    void on_resize(int w, int h);
    void on_drop(HDROP drop);
    void sync_theme();
    void open_external(const std::wstring& path);     // Kayıt Stüdyosu'ndan gelen kayıt
    int run();

private:
    void create_rtv();
    bool create_gpu();
    void load_settings();
    void save_theme();
    void analyse_loop();

    // Proje
    void mark_edit() { edited_ = true; }
    void commit();
    void undo();
    void redo();
    void reset_project(const json& p, const std::wstring& path);
    void open_project(const std::wstring& path);
    bool save_project(bool ask);
    void import_media(const std::wstring& path);
    void add_to_timeline(const Media& m, const std::string& track, double time);
    void add_generated(const std::string& type);
    void add_track(const std::string& kind);
    json* selected();
    json* clip_by_id(const std::string& id);
    void remove_clip(const std::string& id, bool ripple);
    void remove_picked(bool ripple);
    void split_clip(const std::string& id);
    void split_selected();
    void duplicate_selected();
    void detach_audio();
    void select_only(const std::string& id);
    void add_group(const std::string& id);
    void group_picked(bool on);
    bool picked(const std::string& id) const { return picks_.count(id) != 0; }
    void copy_style();
    void paste_style();
    void set_transition(json& clip, const std::string& key, bool entering);
    void apply_transition(const std::string& key, bool entering);
    void apply_effect(const std::string& effect);
    void remove_transitions();
    void remove_effect(const std::string& effect);
    void add_shape(const std::string& preset);
    void toggle_mic();
    void clip_menu();    bool overlaps(const json& clip, const std::string& track, double start, double length);
    std::string track_for(const std::string& kind, double start, double length);
    double snap(double t, const std::string& ignore);

    // Oynatma
    void set_playing(bool on);
    void seek(double t);
    void update_preview();
    void render_preview();
    Texture make_texture(const Bitmap& b);
    Texture* still_texture(const json& clip);
    Texture* text_texture(const std::string& id, const std::string& key, const std::function<Bitmap()>& make);   // Tuş ve altyazı resimleri
    void draw_input_effects(const json& clip, double local);
    void draw_captions();
    void tab_captions();
    void draw_quad(ID3D11ShaderResourceView* srv, const Placement& p, const json& clip, double fade, int mask, double progress,
                   const std::function<void(Quad&)>& tweak = nullptr);
    void freeze_frame();

    // Arayüz
    void main_menu();
    void shortcuts();
    void panel_media(ImVec2 size);
    void tab_media();
    void tab_transitions();
    void tab_annotations();
    void tab_effects();
    void tab_zoom();
    std::vector<std::string> zoom_targets();
    void zoom_to(float x, float y, float w, float h);
    void zoom_out();
    double local_time(const json& clip) const { return std::clamp(time_ - clip.value("start", 0.0), 0.0, clip.value("length", 0.0)); }
    double frame_time() { return 1.0 / std::max(1, project_["canvas"].value("fps", 30)); }
    void panel_preview(ImVec2 size);
    void panel_inspector(ImVec2 size);
    void panel_timeline(ImVec2 size);
    void page_settings();
    void page_shortcuts();
    void page_developers();
    void popup_export();
    void popup_canvas();
    void popup_chapters();
    void popup_template();
    void popup_batch();
    void popup_exit();
    void status_bar();

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_, ps_region_, ps_spot_, ps_dot_;
    Texture canvas_copy_;
    int env_drag_ = -1;          // Sürüklenen ses noktası
    int key_drag_ = -1;          // Sürüklenen anahtar kare
    bool zoom_pick_ = false;     // Önizlemede yakınlaşma alanı seçiliyor
    std::map<std::string, LutTexture> luts_;     // Renk filtreleri, dosya yoluna göre
    json layout_ = layout_defaults();            // Bölme boyutları; timeline 0: kanal sayısına göre
    bool layout_dirty_ = false;
    static json layout_defaults() { return {{"library", 340.0}, {"inspector", 340.0}, {"timeline", 0.0}}; }
    static std::vector<LayoutPreset> layout_presets() {
        return {{T("Varsayılan"), layout_defaults()},
                {T("Geniş önizleme"), {{"library", 240.0}, {"inspector", 280.0}, {"timeline", 220.0}}},
                {T("Geniş zaman çizelgesi"), {{"library", 300.0}, {"inspector", 320.0}, {"timeline", 420.0}}},
                {T("Geniş yan paneller"), {{"library", 440.0}, {"inspector", 440.0}, {"timeline", 0.0}}}};
    }
    std::string renaming_;       // Adı değiştirilen kanal
    char rename_buf_[64] = {};
    bool rename_focus_ = false;
    // Toplu dışa aktarma
    std::vector<std::wstring> batch_files_;
    std::vector<std::string> batch_results_;
    std::wstring batch_folder_;
    size_t batch_next_ = 0;
    bool batch_running_ = false;
    float zoom_time_ = 0.8f;
    ImVec2 zoom_from_{};
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11RenderTargetView> canvas_rtv_;
    Texture canvas_;
    int preview_w_ = 0, preview_h_ = 0;

    json project_, committed_;
    std::wstring project_path_;
    std::vector<json> undo_, redo_;
    bool edited_ = false, unsaved_ = false;
    std::string selected_, message_;
    std::set<std::string> picks_;       // Seçili klipler; selected_ Özellikler'de görünen
    json style_clipboard_;
    MicRecorder mic_;
    double mic_start_ = 0;
    std::wstring mic_path_;
    bool mic_play_ = true;
    bool message_error_ = false;

    std::vector<std::shared_ptr<Media>> media_;
    std::thread analyser_;
    std::mutex analyse_lock_;
    std::deque<std::shared_ptr<Media>> analyse_queue_;
    struct Done { std::shared_ptr<Media> media; Bitmap thumb; std::vector<float> wave; std::vector<std::vector<float>> extra_waves; };
    std::deque<Done> analysed_;
    std::atomic<bool> analyser_halt_{false};
    std::map<std::wstring, std::vector<float>> waves_;
    // Okununca zaman çizelgesine eklenecekler: Kayıt Stüdyosu'ndan gelen kayıt (sona) ve yanındaki kamera / mikrofon dosyaları
    struct Pending { double time = -1; json camera; bool main = false; };
    std::map<std::wstring, Pending> auto_add_;
    void place_pending(const Media& m, const Pending& p);
    void queue_add(const std::wstring& path, const Pending& p);
    void open_recorder();

    std::map<std::string, LiveClip> live_;
    std::map<std::string, CachedTexture> stills_;
    AudioPlayer audio_;
    bool playing_ = false;
    double time_ = 0, wall_start_ = 0, wall_from_ = 0;
    std::vector<uint8_t> frame_buffer_;

    double zoom_ = 60, scroll_ = 0;
    bool snapping_ = true;
    double range_in_ = -1, range_out_ = -1, play_until_ = -1;    // Seçili aralık; -1 yok
    bool range_loop_ = false, export_range_ = false;
    int range_drag_ = 0;        // 1 başlangıç, 2 bitiş, 3 yeni aralık
    bool has_range() const { return range_in_ >= 0 && range_out_ > range_in_ + 0.01; }
    void play_range();
    void range_edit(int what);
    Drag drag_ = Drag::None;
    std::string drag_id_;
    double drag_offset_ = 0, drag_start_ = 0, drag_length_ = 0, drag_in_ = 0;
    std::string drag_track_;
    struct Origin { double start, length, in; std::string track; };
    std::map<std::string, Origin> drag_origin_;
    ImVec2 box_from_;
    std::string page_ = "edit", settings_tab_ = "theme";
    int help_request_ = -1;
    bool show_export_ = false, show_canvas_ = false, show_chapters_ = false, show_template_ = false, show_batch_ = false;
    json export_;
    ExportJob export_job_;
    bool exporting_ = false, export_finished_ = false;
    double export_started_ = 0;
    json clipboard_;
    double theme_checked_ = 0;
};

App* g_app = nullptr;
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

// --- Ekran kartı ---

void App::create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    rtv_.Reset();
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
}

void App::on_resize(int w, int h) {
    if (!swap_ || !w || !h) return;
    rtv_.Reset();
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv();
}

bool App::create_gpu() {
    ComPtr<ID3DBlob> vsb, psb, rgb, err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_region", "ps_5_0", 0, 0, &rgb, &err)))
        return false;
    ComPtr<ID3DBlob> spb;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_spot", "ps_5_0", 0, 0, &spb, &err))) return false;
    device_->CreatePixelShader(spb->GetBufferPointer(), spb->GetBufferSize(), nullptr, &ps_spot_);
    ComPtr<ID3DBlob> dtb;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_dot", "ps_5_0", 0, 0, &dtb, &err))) return false;
    device_->CreatePixelShader(dtb->GetBufferPointer(), dtb->GetBufferSize(), nullptr, &ps_dot_);
    device_->CreatePixelShader(rgb->GetBufferPointer(), rgb->GetBufferSize(), nullptr, &ps_region_);
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
    D3D11_BUFFER_DESC bd{sizeof(Quad), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    device_->CreateBuffer(&bd, nullptr, &cb_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&sd, &sampler_);
    D3D11_BLEND_DESC bl{};
    auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&bl, &blend_);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device_->CreateRasterizerState(&rd, &raster_);
    return vs_ && ps_ && cb_ && sampler_ && blend_ && raster_;
}

Texture App::make_texture(const Bitmap& b) {
    Texture t;
    if (b.empty()) return t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = b.w; td.Height = b.h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{b.px.data(), UINT(b.w * 4), 0};
    if (FAILED(device_->CreateTexture2D(&td, &data, &t.tex))) return t;
    device_->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv);
    t.w = b.w; t.h = b.h;
    return t;
}

// --- Ayarlar: tema Kayıt Stüdyosu'nun ayar dosyasından gelir, programlar hep aynı görünür ---

void App::load_settings() {
    const json data = read_json(data_dir() + L"\\settings.json");
    const int style = data.is_object() ? data.value("theme_style", 0) : 0;
    theme_start(hwnd, style, theme_mode_from(data));
    layout_ = layout_load("editor", layout_defaults());
}

void App::sync_theme() {
    const json data = read_json(data_dir() + L"\\settings.json");
    if (!data.is_object()) return;
    if (data.value("theme_style", 0) != theme_style()) theme_set_style(data.value("theme_style", 0));
    if (theme_mode_from(data) != theme_mode()) theme_set_mode(theme_mode_from(data));
}

void App::save_theme() {
    json data = read_json(data_dir() + L"\\settings.json");
    if (!data.is_object()) data = json::object();
    data["theme_style"] = theme_style();
    data["theme_light"] = theme_is_light();
    data["theme_mode"] = theme_mode();
    write_json(data_dir() + L"\\settings.json", data);
}

// --- Proje ---

void App::commit() {
    if (!edited_ || ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    edited_ = false;
    pack_magnetic(project_);
    if (project_ == committed_) return;
    undo_.push_back(committed_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    committed_ = project_;
    unsaved_ = true;
    write_json(data_dir() + L"\\editor-autosave.json", project_);
    if (playing_) set_playing(false);
}

void App::undo() {
    if (undo_.empty()) return;
    redo_.push_back(project_);
    project_ = undo_.back();
    committed_ = project_;
    undo_.pop_back();
    edited_ = false;
    unsaved_ = true;
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(project_);
    project_ = redo_.back();
    committed_ = project_;
    redo_.pop_back();
    edited_ = false;
    unsaved_ = true;
}

void App::reset_project(const json& p, const std::wstring& path) {
    set_playing(false);
    live_.clear();
    stills_.clear();
    project_ = p;
    committed_ = p;
    project_path_ = path;
    undo_.clear();
    redo_.clear();
    selected_.clear();
    picks_.clear();
    time_ = 0;
    edited_ = unsaved_ = false;
    std::vector<std::wstring> files;
    for (auto& c : project_["clips"])
        if (!c.value("file", std::string()).empty()) files.push_back(widen(c.value("file", std::string())));
    for (auto& f : files) import_media(f);
}

void App::open_project(const std::wstring& path) {
    const json data = read_json(path);
    if (!data.is_object() || !data.contains("clips") || !data.contains("tracks")) {
        message_ = "Proje dosyası okunamadı";
        message_error_ = true;
        return;
    }
    reset_project(data, path);
    message_ = "Proje açıldı";
    message_error_ = false;
}

bool App::save_project(bool ask) {
    std::wstring path = project_path_;
    if (ask || path.empty()) path = pick_save(hwnd, L"Video Düzenleyici projesi", L"*.vproje", L"vproje", L"Proje");
    if (path.empty()) return false;
    write_json(path, project_);
    project_path_ = path;
    unsaved_ = false;
    message_ = "Proje kaydedildi";
    message_error_ = false;
    return true;
}

void App::import_media(const std::wstring& path) {
    for (auto& m : media_)
        if (m->path == path) return;
    auto m = std::make_shared<Media>();
    m->path = path;
    m->name = narrow(fs::path(path).filename().wstring());
    media_.push_back(m);
    std::lock_guard<std::mutex> g(analyse_lock_);
    analyse_queue_.push_back(m);
}

void App::analyse_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!analyser_halt_) {
        std::shared_ptr<Media> m;
        {
            std::lock_guard<std::mutex> g(analyse_lock_);
            if (!analyse_queue_.empty()) { m = analyse_queue_.front(); analyse_queue_.pop_front(); }
        }
        if (!m) { Sleep(50); continue; }
        Done done;
        done.media = m;
        std::string error;
        if (!probe_media(m->path, m->info, error)) m->failed = true;
        if (!m->failed && m->info.video) {
            const int h = 54, w = std::max(2, int(54.f * m->info.width / m->info.height)) & ~1;
            read_thumbnail(m->path, std::min(1.0, m->info.duration / 2), w, h, done.thumb);
        }
        if (!m->failed && m->info.audio) done.wave = read_waveform(m->path, m->info.duration);
        for (int k = 1; !m->failed && k < int(m->info.audio_titles.size()); k++)     // Çoklu ses parçası: her parçanın dalgası
            done.extra_waves.push_back(read_waveform(m->path, m->info.duration, k));
        std::lock_guard<std::mutex> g(analyse_lock_);
        analysed_.push_back(std::move(done));
    }
    CoUninitialize();
}

json* App::clip_by_id(const std::string& id) {
    for (auto& c : project_["clips"])
        if (c.value("id", std::string()) == id) return &c;
    return nullptr;
}

json* App::selected() { return selected_.empty() ? nullptr : clip_by_id(selected_); }

bool App::overlaps(const json& clip, const std::string& track, double start, double length) {
    const std::string id = clip.is_object() ? clip.value("id", std::string()) : std::string();
    for (auto& c : project_["clips"]) {
        if (c.value("track", std::string()) != track || c.value("id", std::string()) == id) continue;
        const double s = c.value("start", 0.0), e = s + c.value("length", 0.0);
        if (start < e - 1e-6 && start + length > s + 1e-6) return true;
    }
    return false;
}

std::string App::track_for(const std::string& kind, double start, double length) {
    const auto& tracks = project_["tracks"];
    for (int i = int(tracks.size()) - 1; i >= 0; i--) {     // Önce alttaki kanal
        if (tracks[i].value("kind", std::string()) != kind) continue;
        if (!overlaps(json(), tracks[i].value("id", std::string()), start, length)) return tracks[i].value("id", std::string());
    }
    add_track(kind);
    return kind == "audio" ? project_["tracks"].back().value("id", std::string()) : project_["tracks"][0].value("id", std::string());
}

void App::add_track(const std::string& kind) {
    int count = 0, serial = 1;
    for (auto& t : project_["tracks"]) {
        if (t.value("kind", std::string()) == kind) count++;
        serial = std::max(serial, atoi(t.value("id", std::string("v0")).c_str() + 1) + 1);
    }
    json t = {{"id", (kind == "audio" ? "a" : "v") + std::to_string(serial)}, {"name", (kind == "audio" ? "Ses " : "Görüntü ") + std::to_string(count + 1)},
              {"kind", kind}, {"hidden", false}, {"muted", false}, {"locked", false}};
    if (kind == "audio") project_["tracks"].push_back(t);
    else project_["tracks"].insert(project_["tracks"].begin(), t);
    mark_edit();
}

void App::add_to_timeline(const Media& m, const std::string& track, double time) {
    if (m.failed || !m.ready) return;
    // İlk video projenin boyutunu ve kare hızını belirler: dışa aktarırken yeniden ölçekleme olmaz, görüntü netliği korunur
    if (project_["clips"].empty() && m.info.video && !m.info.image && m.info.width >= 320) {
        auto& canvas = project_["canvas"];
        canvas["w"] = m.info.width & ~1;
        canvas["h"] = m.info.height & ~1;
        canvas["fps"] = std::clamp(int(m.info.fps + 0.5), 10, 240);
        message_ = TF("Proje videoya uyarlandı: %d x %d, %d FPS", m.info.width, m.info.height, canvas.value("fps", 30));
        message_error_ = false;
    }
    const std::string type = m.info.image ? "image" : m.info.video ? "video" : "audio";
    const double length = m.info.image ? 5.0 : std::max(0.1, m.info.duration);
    const std::string kind = type == "audio" ? "audio" : "video";
    const json* wanted = track.empty() ? nullptr : find_track(project_, track);
    std::string target = wanted && wanted->value("kind", std::string()) == kind && (wanted->value("magnet", false) || !overlaps(json(), track, time, length)) ? track : track_for(kind, time, length);
    json c = new_clip(type, target, time, length);
    c["file"] = narrow(m.path);
    c["name"] = m.name;
    c["source_length"] = m.info.image ? 0.0 : m.info.duration;
    c["w"] = m.info.width;
    c["h"] = m.info.height;
    c["has_audio"] = m.info.audio;
    select_only(c.value("id", std::string()));
    project_["clips"].push_back(c);
    mark_edit();
}

void App::add_generated(const std::string& type) {
    const double length = 5.0;
    json c = new_clip(type, track_for("video", time_, length), time_, length);
    c["name"] = type == "text" ? "Yazı" : "Düz renk";
    if (type == "text") c["y"] = 0.3;
    select_only(c.value("id", std::string()));
    project_["clips"].push_back(c);
    mark_edit();
}

void App::add_shape(const std::string& preset) {
    const double length = 4.0;
    if (preset == "blur" || preset == "pixel") {
        json r = new_clip("blur", track_for("video", time_, length), time_, length);
        r["mode"] = preset;
        r["name"] = preset == "pixel" ? "Pikselli alan" : "Bulanık alan";
        select_only(r.value("id", std::string()));
        project_["clips"].push_back(r);
        mark_edit();
        return;
    }
    if (preset == "spot") {     // Alanın dışı kararır; en üst kanalda olmalı
        json r = new_clip("spot", track_for("video", time_, length), time_, length);
        r["name"] = "Spot ışığı";
        r["fade_in"] = 0.3; r["fade_out"] = 0.3;
        select_only(r.value("id", std::string()));
        project_["clips"].push_back(r);
        mark_edit();
        return;
    }
    json c = new_clip("shape", track_for("video", time_, length), time_, length);
    c["shape"] = preset;
    c["name"] = "Açıklama";
    if (preset == "highlight") { c["shape"] = "rect"; c["fill"] = "#ffe066"; c["fill_opacity"] = 40; c["stroke_width"] = 0; c["radius"] = 6; c["name"] = "Vurgu"; }
    if (preset == "frame") { c["shape"] = "rect"; c["fill_opacity"] = 0; c["stroke"] = "#ff3040"; c["stroke_width"] = 8; c["name"] = "Çerçeve"; }
    if (preset == "arrow" || preset == "line") { c["fill"] = "#ff3040"; c["shape_h"] = 90; c["shape_w"] = 320; c["stroke_width"] = 10; c["name"] = preset == "arrow" ? "Ok" : "Çizgi"; }
    if (preset == "ellipse") { c["shape_h"] = 300; c["shape_w"] = 300; c["name"] = "Elips"; }
    if (preset == "bubble") { c["text"] = "Yazınız"; c["fill"] = "#ffffff"; c["color"] = "#101018"; c["stroke_width"] = 0; c["radius"] = 28; c["name"] = "Konuşma balonu"; }
    if (preset == "lower") { c["shape"] = "rect"; c["text"] = "Adınız Soyadınız"; c["shape_w"] = 800; c["shape_h"] = 110; c["fill"] = "#0d6efd"; c["stroke_width"] = 0; c["radius"] = 8; c["x"] = -0.26; c["y"] = 0.36; c["name"] = "Alt başlık"; }
    if (preset == "badge") { c["text"] = "1"; c["shape_w"] = 110; c["shape_h"] = 110; c["fill"] = "#dc3545"; c["stroke_width"] = 0; c["font_size"] = 56; c["name"] = "Numara"; }
    select_only(c.value("id", std::string()));
    project_["clips"].push_back(c);
    mark_edit();
}

void App::copy_style() {
    json* c = selected();
    if (!c) return;
    style_clipboard_ = json::object();
    for (auto& key : style_keys())
        if (c->contains(key)) style_clipboard_[key] = (*c)[key];
    message_ = "Klibin ayarları kopyalandı. Başka klip seçip yapıştırın.";
    message_error_ = false;
}

void App::paste_style() {
    if (!style_clipboard_.is_object()) return;
    int count = 0;
    for (auto& id : picks_)
        if (json* c = clip_by_id(id)) {
            for (auto& [key, value] : style_clipboard_.items()) (*c)[key] = value;
            count++;
        }
    if (count) mark_edit();
    message_ = TF("%d klibe uygulandı", count);
    message_error_ = false;
}

void App::set_transition(json& c, const std::string& key, bool entering) {
    if (c.value("type", std::string()) == "audio") return;
    c[entering ? "t_in" : "t_out"] = key;
    mark_edit();
}

void App::apply_transition(const std::string& key, bool entering) {
    for (auto& id : picks_)
        if (json* c = clip_by_id(id)) set_transition(*c, key, entering);
}

void App::apply_effect(const std::string& e) {
    if (picks_.empty()) { message_ = "Önce zaman çizelgesinden klip seçin"; message_error_ = false; return; }
    int skipped = 0;
    for (auto& id : picks_) {
        json* c = clip_by_id(id);
        if (!c) continue;
        const std::string type = c->value("type", std::string());
        if (e == "reverse") {
            if (type == "video" || type == "audio") (*c)["reverse"] = !c->value("reverse", false);
            else skipped++;
            continue;
        }
        if (e.rfind("v_", 0) == 0) {     // Ses efekti: sesi olan kliplere
            if (clip_has_audio(*c)) (*c)["voice"] = e.substr(2); else skipped++;
            continue;
        }
        if (e == "halo" || e == "rings" || e == "keys") {
            if (type != "video" || !input_data(c->value("file", std::string()))) { skipped++; continue; }
            (*c)[e == "halo" ? "cursor_halo" : e == "rings" ? "click_rings" : "keys_show"] = true;
            continue;
        }
        if (type == "audio" || type == "blur" || type == "spot") continue;
        if (e.rfind("m_", 0) == 0) {    // Giriş ve çıkış hareketi birlikte
            std::string kind = e.substr(2);
            if (kind == "type" && type != "text") { skipped++; continue; }
            (*c)["m_in"] = kind; (*c)["m_out"] = kind;
            const double d = kind == "type" ? std::min(2.0, std::max(0.4, text_length(*c) * 0.06)) : 0.6;
            (*c)["m_in_d"] = std::min(d, c->value("length", 1.0) / 2); (*c)["m_out_d"] = std::min(d, c->value("length", 1.0) / 2);
            continue;
        }
        if (e == "shadow") (*c)["shadow"] = true;
        if (e == "round" || e == "border" || e == "circle") {
            if (!clip_can_frame(*c)) { skipped++; continue; }
            if (e == "round") (*c)["corner"] = 40;
            if (e == "border") { (*c)["border"] = 6; if (c->value("corner", 0) == 0) (*c)["corner"] = 16; }
            if (e == "circle") {    // Ortada daire: genişlik yüksekliğe göre oranlanır
                const double w = std::max(1, c->value("w", 1920)), h = std::max(1, c->value("h", 1080));
                (*c)["mask"] = "ellipse"; (*c)["mask_h"] = 80; (*c)["mask_w"] = int(std::clamp(80 * h / w, 5.0, 100.0) + 0.5);
                (*c)["mask_x"] = 0; (*c)["mask_y"] = 0;
            }
        }
        if (e == "blur") (*c)["blur"] = 6.0;
        if (e == "vignette") (*c)["vignette"] = 60.0;
        if (e == "sepia") (*c)["sepia"] = 100.0;
        if (e == "invert") (*c)["invert"] = true;
        if (e == "gray") (*c)["saturation"] = 0.0;
        if (e == "vivid") { (*c)["saturation"] = 1.4; (*c)["contrast"] = 1.1; (*c)["brightness"] = 0.02; }
        if (e == "cinema") { (*c)["saturation"] = 0.8; (*c)["contrast"] = 1.2; (*c)["brightness"] = -0.03; }
        if (e == "chroma") (*c)["chroma"] = true;
        if (e == "flip") (*c)["flip_h"] = !c->value("flip_h", false);
        if (e == "zoom") {
            json end = *c;
            end["scale"] = c->value("scale", 100.0) * 1.3;
            (*c)["keys"] = json::array();
            set_key(*c, 0, *c, 0);
            set_key(*c, c->value("length", 0.0), end, 0);
        }
        if (e == "clear") {
            const json fresh = new_clip("video", "", 0, 0);     // Geçici nesnenin üzerinde dolaşılmaz (döngüde silinmiş olur)
            const auto& keys = style_keys();
            for (auto& [key, value] : fresh.items())
                if (std::find(keys.begin(), keys.end(), key) != keys.end()) (*c)[key] = value;
        }
    }
    mark_edit();
    if (skipped) {
        message_ = e == "reverse" ? "Ters oynatma yalnızca video ve ses kliplerine uygulanır"
                 : e.rfind("v_", 0) == 0 ? "Ses efekti yalnızca sesi olan kliplere uygulanır"
                 : e == "halo" || e == "rings" || e == "keys" ? "İmleç verisi olan video yok. Kayıt Stüdyosu'nda imleç kaydı açıkken alınan kayıtlarda çalışır."
                 : e == "m_type" ? "Daktilo yalnızca yazı kliplerine uygulanır" : "Köşe, kenarlık ve maske video, resim ve düz renk kliplerine uygulanır";
        message_error_ = false;
    }
}

// Seçili kliplerin geçişleri silinir; önündeki klibin bu klibe açılan çıkış geçişi de
void App::remove_transitions() {
    for (auto& id : picks_) {
        json* c = clip_by_id(id);
        if (!c) continue;
        (*c)["t_in"] = "";
        (*c)["t_out"] = "";
        if (json* prev = previous_clip(project_, *c)) (*prev)["t_out"] = "";
    }
    mark_edit();
    message_ = "Geçişler kaldırıldı";
    message_error_ = false;
}

struct EffectInfo { const char* key; const char* name; };
const EffectInfo kEffects[] = {
    {"anim", "Animasyon"}, {"blur", "Bulanıklık"}, {"vignette", "Kenar karartma"}, {"color", "Renk ayarı"}, {"sepia", "Sepya"},
    {"invert", "Negatif"}, {"chroma", "Yeşil perde"}, {"flip", "Aynalama"}, {"rotate", "Döndürme"}, {"crop", "Kırpma"}, {"fade", "Yumuşak giriş / çıkış"},
    {"shadow", "Gölge"}, {"frame", "Köşe / kenarlık / maske"}, {"motion", "Giriş / çıkış hareketi"}, {"reverse", "Ters oynatma"},
    {"cursor", "İmleç / tuş efektleri"}, {"voice", "Ses efekti"}, {"lut", "Renk filtresi (LUT)"},
};

bool effect_on(const json& c, const std::string& e) {
    if (e == "anim") return c.value("anim", false) || (c.contains("keys") && c["keys"].is_array() && !c["keys"].empty());
    if (e == "blur") return c.value("blur", 0.0) > 0;
    if (e == "vignette") return c.value("vignette", 0.0) > 0;
    if (e == "color") return c.value("brightness", 0.0) != 0 || c.value("contrast", 1.0) != 1 || c.value("saturation", 1.0) != 1;
    if (e == "sepia") return c.value("sepia", 0.0) > 0;
    if (e == "invert") return c.value("invert", false);
    if (e == "chroma") return c.value("chroma", false);
    if (e == "flip") return c.value("flip_h", false) || c.value("flip_v", false);
    if (e == "rotate") return c.value("rotate", 0.0) != 0;
    if (e == "crop") return c.value("crop_l", 0.0) + c.value("crop_r", 0.0) + c.value("crop_t", 0.0) + c.value("crop_b", 0.0) > 0;
    if (e == "fade") return c.value("fade_in", 0.0) > 0 || c.value("fade_out", 0.0) > 0;
    if (e == "shadow") return c.value("shadow", false);
    if (e == "frame") return clip_has_frame(c);
    if (e == "motion") return !c.value("m_in", std::string()).empty() || !c.value("m_out", std::string()).empty();
    if (e == "reverse") return clip_reversed(c);
    if (e == "cursor") return clip_uses_input(c);
    if (e == "voice") return !c.value("voice", std::string()).empty();
    if (e == "lut") return !c.value("lut", std::string()).empty();
    return false;
}

void effect_off(json& c, const std::string& e) {
    if (e == "anim") { c["anim"] = false; c["keys"] = json::array(); }
    if (e == "blur") c["blur"] = 0.0;
    if (e == "vignette") c["vignette"] = 0.0;
    if (e == "color") { c["brightness"] = 0.0; c["contrast"] = 1.0; c["saturation"] = 1.0; }
    if (e == "sepia") c["sepia"] = 0.0;
    if (e == "invert") c["invert"] = false;
    if (e == "chroma") c["chroma"] = false;
    if (e == "flip") { c["flip_h"] = false; c["flip_v"] = false; }
    if (e == "rotate") c["rotate"] = 0.0;
    if (e == "crop") { c["crop_l"] = 0.0; c["crop_r"] = 0.0; c["crop_t"] = 0.0; c["crop_b"] = 0.0; }
    if (e == "fade") { c["fade_in"] = 0.0; c["fade_out"] = 0.0; }
    if (e == "shadow") c["shadow"] = false;
    if (e == "frame") { c["corner"] = 0; c["border"] = 0; c["mask"] = ""; c["feather"] = 0; }
    if (e == "motion") { c["m_in"] = ""; c["m_out"] = ""; }
    if (e == "reverse") c["reverse"] = false;
    if (e == "cursor") { c["cursor_halo"] = false; c["click_rings"] = false; c["keys_show"] = false; }
    if (e == "voice") c["voice"] = "";
    if (e == "lut") c["lut"] = "";
}

// e boşsa hepsi
void App::remove_effect(const std::string& e) {
    for (auto& id : picks_)
        if (json* c = clip_by_id(id))
            for (auto& info : kEffects)
                if (e.empty() || e == info.key) effect_off(*c, info.key);
    mark_edit();
    message_ = e.empty() ? "Efektler kaldırıldı" : "Efekt kaldırıldı";
    message_error_ = false;
}

void App::toggle_mic() {
    if (mic_.running()) {
        const double seconds = mic_.stop();
        if (mic_play_) set_playing(false);
        if (seconds > 0.1) {
            json c = new_clip("audio", track_for("audio", mic_start_, seconds), mic_start_, seconds);
            c["file"] = narrow(mic_path_);
            c["name"] = narrow(fs::path(mic_path_).filename().wstring());
            c["source_length"] = seconds;
            c["has_audio"] = true;
            select_only(c.value("id", std::string()));
            project_["clips"].push_back(c);
            mark_edit();
            import_media(mic_path_);
            message_ = "Ses kaydı eklendi";
            message_error_ = false;
        }
        return;
    }
    const std::wstring folder = videos_dir() + L"\\Recording Studio";
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\Voice %04d-%02d-%02d %02d-%02d-%02d.wav", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    mic_path_ = folder + name;
    mic_start_ = time_;
    mic_.start(mic_path_);
    if (mic_play_) set_playing(true);
    message_ = "Ses kaydediliyor";
    message_error_ = false;
}

void App::select_only(const std::string& id) {
    picks_.clear();
    selected_ = id;
    if (!id.empty()) picks_.insert(id);
    add_group(id);
}

// Gruplu bir klip seçilince grubun hepsi seçilir: birlikte taşınır, silinir, ayarlanır
void App::add_group(const std::string& id) {
    const json* c = clip_by_id(id);
    const std::string group = c ? c->value("group", std::string()) : std::string();
    if (group.empty()) return;
    for (auto& other : project_["clips"])
        if (other.value("group", std::string()) == group) picks_.insert(other.value("id", std::string()));
}

void App::group_picked(bool on) {
    if (on && picks_.size() < 2) { message_ = T("Gruplamak için en az iki klip seçin"); message_error_ = false; return; }
    const std::string group = on ? new_clip("audio", "", 0, 0).value("id", std::string()) : std::string();
    for (auto& id : picks_)
        if (json* c = clip_by_id(id)) (*c)["group"] = group;
    mark_edit();
    message_ = on ? T("Klipler gruplandı") : T("Grup çözüldü");
    message_error_ = false;
}

void App::remove_clip(const std::string& id, bool ripple) {
    auto& clips = project_["clips"];
    for (size_t i = 0; i < clips.size(); i++) {
        if (clips[i].value("id", std::string()) != id) continue;
        const std::string track = clips[i].value("track", std::string());
        const double start = clips[i].value("start", 0.0), length = clips[i].value("length", 0.0);
        clips.erase(i);
        if (ripple)     // Boşluk kalmasın: arkadakiler öne gelir
            for (auto& c : clips)
                if (c.value("track", std::string()) == track && c.value("start", 0.0) > start) c["start"] = c.value("start", 0.0) - length;
        break;
    }
    picks_.erase(id);
    if (selected_ == id) selected_ = picks_.empty() ? std::string() : *picks_.begin();
    mark_edit();
}

void App::remove_picked(bool ripple) {
    const std::set<std::string> list = picks_;
    for (auto& id : list) remove_clip(id, ripple);
}

void App::split_clip(const std::string& id) {
    json* c = clip_by_id(id);
    if (!c) return;
    const double start = c->value("start", 0.0), length = c->value("length", 0.0);
    if (time_ <= start + 0.02 || time_ >= start + length - 0.02) return;
    json right = *c;
    const double left = time_ - start;
    split_keys(*c, right, left);
    (*c)["length"] = left;
    (*c)["fade_out"] = 0.0;
    (*c)["audio_fade_out"] = 0.0;
    (*c)["t_out"] = "";
    right["id"] = new_clip("audio", "", 0, 0).value("id", std::string());
    right["start"] = time_;
    right["length"] = length - left;
    if (clip_reversed(*c)) (*c)["in"] = c->value("in", 0.0) + (length - left) * c->value("speed", 1.0);    // Ters klipte ilk parça kaynağın sonu
    else right["in"] = right.value("in", 0.0) + left * right.value("speed", 1.0);
    right["fade_in"] = 0.0;
    right["audio_fade_in"] = 0.0;
    right["t_in"] = "";
    right["m_in"] = "";
    (*c)["m_out"] = "";
    project_["clips"].push_back(right);
    mark_edit();
}

// Seçim varsa seçilenler, yoksa işaretçinin altındaki bütün klipler bölünür
void App::split_selected() {
    std::vector<std::string> ids(picks_.begin(), picks_.end());
    if (ids.empty())
        for (auto& c : project_["clips"]) {
            const json* track = find_track(project_, c.value("track", std::string()));
            if (!track || !track->value("locked", false)) ids.push_back(c.value("id", std::string()));
        }
    for (auto& id : ids) split_clip(id);
}

void App::duplicate_selected() {
    json* c = selected();
    if (!c) return;
    json copy = *c;
    const double length = copy.value("length", 0.0), start = copy.value("start", 0.0) + length;
    copy["id"] = new_clip("audio", "", 0, 0).value("id", std::string());
    copy["group"] = "";
    copy["start"] = start;
    if (overlaps(copy, copy.value("track", std::string()), start, length))
        copy["track"] = track_for(copy.value("type", std::string()) == "audio" ? "audio" : "video", start, length);
    select_only(copy.value("id", std::string()));
    project_["clips"].push_back(copy);
    mark_edit();
}

// İşaretçideki kare resme dönüşür ve araya girer; arkadaki klipler o kadar ileri kayar
void App::freeze_frame() {
    json* c = selected();
    if (!c || c->value("type", std::string()) != "video") { message_ = "Önce bir video klip seçin"; message_error_ = false; return; }
    const double start = c->value("start", 0.0), length = c->value("length", 0.0), speed = c->value("speed", 1.0);
    if (time_ < start || time_ > start + length) { message_ = "İşaretçiyi klibin üzerine getirin"; message_error_ = false; return; }
    const double local = std::clamp(time_ - start, 0.0, std::max(0.0, length - 0.02));
    const double source = c->value("in", 0.0) + (clip_reversed(*c) ? length - local : local) * speed;
    const int w = std::max(2, c->value("w", 1920)), h = std::max(2, c->value("h", 1080));
    Bitmap b;
    if (!read_thumbnail(widen(c->value("file", std::string())), source, w, h, b)) { message_ = "Kare okunamadı"; message_error_ = true; return; }
    const std::wstring folder = data_dir() + L"\\Freeze Frames";
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const std::wstring path = folder + L"\\" + widen(c->value("id", std::string())) + L"-" + std::to_wstring((long long)(source * 1000)) + L".png";
    if (!save_png(b, path)) { message_ = "Kare kaydedilemedi"; message_error_ = true; return; }
    const json source_clip = *c;
    const std::string track = c->value("track", std::string()), id = c->value("id", std::string());
    const double at = time_, hold = 3.0;
    split_clip(id);
    for (auto& other : project_["clips"])     // Aynı kanalda işaretçiden sonrakiler ileri kayar
        if (other.value("track", std::string()) == track && other.value("start", 0.0) >= at - 1e-4) other["start"] = other.value("start", 0.0) + hold;
    json still = new_clip("image", track, at, hold);
    for (auto& key : style_keys())      // Görünüm ayarları korunur; geçiş, hareket ve animasyon taşınmaz
        if (source_clip.contains(key) && key != "keys" && key.rfind("t_", 0) != 0 && key.rfind("m_", 0) != 0 && key.rfind("fade", 0) != 0 && key.find("audio") == std::string::npos)
            still[key] = source_clip[key];
    const json view = clip_at(source_clip, local);     // O andaki yakınlaşma ve konum korunur
    for (const char* k : {"scale", "x", "y", "rotate", "opacity"}) still[k] = view[k];
    still["file"] = narrow(path);
    still["name"] = "Donmuş kare";
    still["w"] = w; still["h"] = h;
    select_only(still.value("id", std::string()));
    project_["clips"].push_back(still);
    mark_edit();
    message_ = "Donmuş kare eklendi (3 sn). Süresini Özellikler'den değiştirebilirsiniz.";
    message_error_ = false;
}

void App::detach_audio() {
    json* c = selected();
    if (!c || c->value("type", std::string()) != "video" || !c->value("has_audio", false)) return;
    json a = *c;
    (*c)["has_audio"] = false;
    a["id"] = new_clip("audio", "", 0, 0).value("id", std::string());
    a["type"] = "audio";
    a["track"] = track_for("audio", a.value("start", 0.0), a.value("length", 0.0));
    select_only(a.value("id", std::string()));
    project_["clips"].push_back(a);
    mark_edit();
}

// Kenarlar, işaretçi ve işaretler mıknatıs gibi tutar
double App::snap(double t, const std::string& ignore) {
    if (!snapping_ || ImGui::GetIO().KeyAlt) return t;      // Alt basılıyken yapışmaz
    const double range = kSnapPx / zoom_;
    double best = t, gap = range;
    auto test = [&](double v) {
        if (fabs(v - t) < gap) { gap = fabs(v - t); best = v; }
    };
    test(0);
    test(time_);
    for (auto& m : project_["markers"]) test(marker_time(m));
    for (auto& c : project_["clips"]) {
        if (c.value("id", std::string()) == ignore) continue;
        test(c.value("start", 0.0));
        test(c.value("start", 0.0) + c.value("length", 0.0));
    }
    return best;
}

// --- Oynatma ---

void App::seek(double t) {
    const bool was = playing_;
    if (was) set_playing(false);
    time_ = std::clamp(t, 0.0, std::max(project_length(project_), 0.0));
    if (was) set_playing(true);
}

void App::set_playing(bool on) {
    if (on == playing_) return;
    playing_ = on;
    play_until_ = -1;
    if (on) {
        if (time_ >= project_length(project_) - 0.01) time_ = 0;
        wall_start_ = now();
        wall_from_ = time_;
        audio_.start(audio_preview_command(project_, time_), time_);
    } else {
        audio_.stop();
    }
}

void App::play_range() {
    if (!has_range()) { message_ = "Önce cetvelde Shift ile sürükleyerek ya da I ve O tuşlarıyla aralık belirleyin"; message_error_ = false; return; }
    set_playing(false);
    time_ = range_in_;
    set_playing(true);
    play_until_ = range_out_;
}

// 1 aralığı sil, 2 aralık dışını sil
void App::range_edit(int what) {
    if (!has_range()) { message_ = "Önce bir aralık belirleyin"; message_error_ = false; return; }
    set_playing(false);
    const double length = range_out_ - range_in_;
    if (what == 1) {
        project_delete_range(project_, range_in_, range_out_);
        time_ = range_in_;
        message_ = TF("Aralık silindi (%s)", format_time(length, 100).c_str());
    } else {
        project_keep_range(project_, range_in_, range_out_);
        time_ = 0;
        message_ = TF("Yalnızca aralık bırakıldı (%s)", format_time(length, 100).c_str());
    }
    message_error_ = false;
    range_in_ = range_out_ = -1;
    picks_.clear();
    selected_.clear();
    mark_edit();
}

void App::update_preview() {
    const auto& canvas = project_["canvas"];
    const int fps = std::min(canvas.value("fps", 60), 30);
    if (playing_) {
        time_ = audio_.started() ? audio_.position() : wall_from_ + now() - wall_start_;
        if (play_until_ > 0 && time_ >= play_until_) {     // Aralık oynatılıyor
            const double from = range_in_, until = play_until_;
            set_playing(false);
            time_ = until;
            if (range_loop_ && from >= 0) { time_ = from; set_playing(true); play_until_ = until; }
        }
        if (time_ >= project_length(project_)) { time_ = project_length(project_); set_playing(false); }
    }
    std::map<std::string, bool> wanted;
    for (auto& c : project_["clips"]) {
        if (c.value("type", std::string()) != "video") continue;
        const json* track = find_track(project_, c.value("track", std::string()));
        if (track && track->value("hidden", false)) continue;
        const double start = c.value("start", 0.0), length = c.value("length", 0.0);
        const bool active = time_ >= start && time_ < start + length + clip_tail(project_, c), soon = playing_ && time_ < start && start - time_ < 1.0;
        if (!active && !soon) continue;
        const std::string id = c.value("id", std::string());
        wanted[id] = true;
        LiveClip& live = live_[id];
        const bool reverse = clip_reversed(c);
        const std::string key = c.value("file", std::string()) + "|" + std::to_string(c.value("speed", 1.0)) + "|" + std::to_string(c.value("in", 0.0)) + "|" + std::to_string(start) +
                                (reverse ? "|ters" + std::to_string(length) : "");
        const double frame = 1.0 / fps;
        bool restart = !live.feed || live.key != key;
        if (live.feed && !restart) {
            if (!playing_) restart = fabs(time_ - live.origin - std::max(live.index, 0) * frame) > frame && live.index >= 0;
            else restart = active && (time_ < live.origin - 0.05 || time_ - live.origin - std::max(live.index, 0) * frame > 1.0 || time_ - live.origin > live.span - frame);
        }
        if (restart && now() - live.restarted > (playing_ ? 0.0 : 0.05)) {
            const double origin = active ? time_ : start, speed = c.value("speed", 1.0);
            const float mw = float(c.value("w", 1920)), mh = float(c.value("h", 1080));
            const float k = std::min(1.f, std::min(960.f / std::max(mw, 1.f), float(kPreviewHeight) / std::max(mh, 1.f)));
            live.w = std::max(2, int(mw * k)) & ~1;
            live.h = std::max(2, int(mh * k)) & ~1;
            live.feed = std::make_unique<VideoFeed>();
            if (reverse) {      // Ters klip: kaynağın sonundan geriye doğru kısa parçalar
                const double source = c.value("in", 0.0) + std::max(0.0, length - (origin - start)) * speed;
                const double chunk = std::max(0.05, std::min(2.0, source - c.value("in", 0.0)));
                live.feed->start(widen(c.value("file", std::string())), source, speed, fps, live.w, live.h, chunk);
                live.span = chunk / speed;
            } else {
                live.feed->start(widen(c.value("file", std::string())), c.value("in", 0.0) + (origin - start) * speed, speed, fps, live.w, live.h);
                live.span = 1e9;
            }
            live.key = key;
            live.origin = origin;
            live.index = -1;
            live.restarted = now();
        }
        if (!live.feed) continue;
        const int index = std::max(0, int((time_ - live.origin) * fps));
        if (live.feed->take(index, frame_buffer_)) {
            live.index = index;
            if (live.frame.w != live.w || live.frame.h != live.h) {
                Bitmap blank;
                blank.w = live.w; blank.h = live.h;
                blank.px.assign(size_t(live.w) * live.h * 4, 0);
                live.frame = Texture{};
                D3D11_TEXTURE2D_DESC td{};
                td.Width = live.w; td.Height = live.h; td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                device_->CreateTexture2D(&td, nullptr, &live.frame.tex);
                device_->CreateShaderResourceView(live.frame.tex.Get(), nullptr, &live.frame.srv);
                live.frame.w = live.w; live.frame.h = live.h;
            }
            ctx_->UpdateSubresource(live.frame.tex.Get(), 0, nullptr, frame_buffer_.data(), live.w * 4, 0);
        }
    }
    for (auto it = live_.begin(); it != live_.end();)
        if (!wanted.count(it->first)) it = live_.erase(it); else ++it;
}

Texture* App::still_texture(const json& c) {
    const std::string id = c.value("id", std::string()), type = c.value("type", std::string());
    std::string key;
    if (type == "image") key = c.value("file", std::string());
    if (type == "color") key = c.value("color", std::string());
    if (clip_is_visual_still(c))
        for (const char* k : {"text", "font", "font_size", "bold", "color", "outline", "outline_color", "outline_thickness", "background", "bg_color", "bg_opacity",
                              "shape", "shape_w", "shape_h", "fill", "fill_opacity", "stroke", "stroke_width", "radius", "visible_chars"})
            key += c.contains(k) ? c[k].dump() : "";
    CachedTexture& cached = stills_[id];
    if (cached.key != key || !cached.tex.srv) {
        cached.key = key;
        Bitmap b;
        if (type == "image") load_image(widen(key), b);
        if (clip_is_visual_still(c)) b = render_still(c);
        if (type == "color") {
            const uint32_t rgb = parse_color(key);
            b.w = b.h = 1;
            b.px = {uint8_t(rgb), uint8_t(rgb >> 8), uint8_t(rgb >> 16), 255};
        }
        cached.tex = make_texture(b);
    }
    return &cached.tex;
}

Texture* App::text_texture(const std::string& id, const std::string& key, const std::function<Bitmap()>& make) {
    CachedTexture& cached = stills_[id];
    if (cached.key != key || !cached.tex.srv) {
        cached.key = key;
        cached.tex = make_texture(make());
    }
    return &cached.tex;
}

// Kayıt Stüdyosu verisiyle imleç vurgusu, tıklama halkası ve tuş göstergesi (dışa aktarmadakiyle aynı ölçüler)
void App::draw_input_effects(const json& c, double local) {
    if (!clip_uses_input(c)) return;
    const auto data = input_data(c.value("file", std::string()));
    if (!data) return;
    const float W = float(project_["canvas"].value("w", 1920)), H = float(project_["canvas"].value("h", 1080)), unit = H / 1080.f;
    const json plain = new_clip("color", "", 0, 0);     // Kırpma ve döndürme olmadan düz dörtgen
    auto rgb = [](const std::string& hex, float* out) {
        const uint32_t v = parse_color(hex);
        out[0] = ((v >> 16) & 255) / 255.f; out[1] = ((v >> 8) & 255) / 255.f; out[2] = (v & 255) / 255.f;
    };
    ctx_->PSSetShader(ps_dot_.Get(), nullptr, 0);
    if (c.value("cursor_halo", false)) {
        float sx, sy, x, y;
        if (cursor_at(*data, source_time(c, local), sx, sy) && input_point(c, *data, sx, sy, local, W, H, x, y)) {
            const float size = std::clamp(c.value("halo_size", 90), 10, 400) * unit;
            draw_quad(nullptr, Placement{x - size / 2, y - size / 2, size, size}, plain, 1.0, 0, 1.0, [&](Quad& q) {
                q.opacity = std::clamp(c.value("halo_opacity", 45), 0, 100) / 100.f;
                rgb(c.value("halo_color", std::string("#ffe066")), q.scolor);
            });
        }
    }
    if (c.value("click_rings", false))
        for (auto& k : data->clicks) {
            double when;
            if (!clip_local_of(c, k[0], when) || local < when || local - when >= 0.45) continue;
            float x, y;
            if (!input_point(c, *data, k[1], k[2], when, W, H, x, y)) continue;
            const float u = float((local - when) / 0.45), full = 150 * unit;
            const float size = full * (0.24f + 0.68f * (1 - (1 - u) * (1 - u)));
            draw_quad(nullptr, Placement{x - size / 2, y - size / 2, size, size}, plain, 1.0, 0, 1.0, [&](Quad& q) {
                q.opacity = 1 - u;
                q.frame2[0] = 1;
                rgb(c.value("ring_color", std::string("#ff3040")), q.scolor);
            });
        }
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    if (c.value("keys_show", false)) {
        const std::string* label = nullptr;
        for (auto& [t, text] : data->keys) {        // O an gösterilen: son basılan, 1,4 saniye
            double when;
            if (clip_local_of(c, t, when) && when <= local && local - when < 1.4) label = &text;
        }
        if (label) {
            Texture* t = text_texture("#tus", *label + std::to_string(int(H)), [&] { return key_bitmap(*label, unit); });
            if (t->srv) draw_quad(t->srv.Get(), Placement{(W - t->w) / 2, H * 0.05f + 2, float(t->w), float(t->h)}, plain, 1.0, 0, 1.0);     // Üstte: altyazıyla çakışmaz
        }
    }
}

void App::draw_captions() {
    if (!project_.value("captions_burn", true) || !project_.contains("captions") || !project_["captions"].is_array()) return;
    for (auto& cap : project_["captions"]) {
        if (time_ < cap.value("start", 0.0) || time_ >= cap.value("end", 0.0) || cap.value("text", std::string()).empty()) continue;
        const float W = float(project_["canvas"].value("w", 1920)), H = float(project_["canvas"].value("h", 1080));
        const json style = project_.value("caption_style", default_caption_style());
        const std::string text = cap.value("text", std::string());
        Texture* t = text_texture("#altyazi", text + style.dump() + std::to_string(int(H)), [&] { return caption_bitmap(text, style, H / 1080.f); });
        if (!t->srv) return;
        const float cy = H / 2 + float(style.value("y", 0.40)) * H;
        draw_quad(t->srv.Get(), Placement{(W - t->w) / 2, std::clamp(cy - t->h / 2.f, 0.f, H - t->h), float(t->w), float(t->h)}, new_clip("color", "", 0, 0), 1.0, 0, 1.0);
        return;
    }
}

void App::draw_quad(ID3D11ShaderResourceView* srv, const Placement& p, const json& c, double fade, int mask, double progress,
                    const std::function<void(Quad&)>& tweak) {
    const float W = float(project_["canvas"].value("w", 1920)), H = float(project_["canvas"].value("h", 1080));
    const float l = float(c.value("crop_l", 0.0)) / 100, r = float(c.value("crop_r", 0.0)) / 100, t = float(c.value("crop_t", 0.0)) / 100, b = float(c.value("crop_b", 0.0)) / 100;
    Quad q{};
    q.rect[0] = p.x / W; q.rect[1] = p.y / H; q.rect[2] = p.w / W; q.rect[3] = p.h / H;
    q.crop[0] = c.value("flip_h", false) ? 1 - r : l;
    q.crop[1] = c.value("flip_v", false) ? 1 - b : t;
    q.crop[2] = (c.value("flip_h", false) ? -1.f : 1.f) * (1 - l - r);
    q.crop[3] = (c.value("flip_v", false) ? -1.f : 1.f) * (1 - t - b);
    q.opacity = float(c.value("opacity", 100) / 100.0 * fade);
    q.brightness = float(c.value("brightness", 0.0));
    q.contrast = float(c.value("contrast", 1.0));
    q.saturation = float(c.value("saturation", 1.0));
    q.angle = float(c.value("rotate", 0.0) * 3.14159265 / 180);
    q.aspect = W / H;
    q.blur = float(c.value("blur", 0.0));
    q.vignette = float(c.value("vignette", 0.0)) / 100;
    q.sepia = float(c.value("sepia", 0.0)) / 100;
    q.invert = c.value("invert", false) ? 1.f : 0.f;
    q.chroma = c.value("chroma", false) ? 1.f : 0.f;
    q.similarity = float(c.value("chroma_similarity", 0.25));
    const uint32_t key = parse_color(c.value("chroma_color", std::string("#00ff00")));
    q.key[0] = ((key >> 16) & 255) / 255.f; q.key[1] = ((key >> 8) & 255) / 255.f; q.key[2] = (key & 255) / 255.f;
    q.mask = float(mask);
    q.progress = float(progress);
    q.frame2[3] = 1;    // Soldan açılma yok
    ID3D11ShaderResourceView* lut = nullptr;
    if (const std::string file = c.value("lut", std::string()); !file.empty()) {
        LutTexture& t = luts_[file];
        if (!t.tried) {     // İlk kullanımda yüklenir
            t.tried = true;
            Lut cube;
            if (load_cube(widen(file), cube)) {
                std::vector<uint8_t> px(size_t(cube.size) * cube.size * cube.size * 4);
                for (size_t i = 0; i * 3 < cube.rgb.size(); i++) {
                    for (int k = 0; k < 3; k++) px[i * 4 + k] = uint8_t(std::clamp(cube.rgb[i * 3 + k], 0.f, 1.f) * 255 + 0.5f);
                    px[i * 4 + 3] = 255;
                }
                D3D11_TEXTURE3D_DESC td{};
                td.Width = td.Height = td.Depth = UINT(cube.size);
                td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA data{px.data(), UINT(cube.size * 4), UINT(cube.size * cube.size * 4)};
                ComPtr<ID3D11Texture3D> tex;
                if (SUCCEEDED(device_->CreateTexture3D(&td, &data, &tex))) device_->CreateShaderResourceView(tex.Get(), nullptr, &t.srv);
                t.size = cube.size;
            }
        }
        if (t.srv) { lut = t.srv.Get(); q.pad[0] = float(t.size); }
    }
    ctx_->PSSetShaderResources(1, 1, &lut);
    if (tweak) tweak(q);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &q, sizeof(q));
    ctx_->Unmap(cb_.Get(), 0);
    ctx_->PSSetShaderResources(0, 1, &srv);
    ctx_->Draw(4, 0);
}

void App::render_preview() {
    const int cw = project_["canvas"].value("w", 1920), ch = project_["canvas"].value("h", 1080);
    const int h = kPreviewHeight, w = std::max(2, h * cw / std::max(ch, 1));
    if (w != preview_w_ || h != preview_h_) {
        preview_w_ = w; preview_h_ = h;
        canvas_ = Texture{};
        canvas_rtv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        device_->CreateTexture2D(&td, nullptr, &canvas_.tex);
        device_->CreateShaderResourceView(canvas_.tex.Get(), nullptr, &canvas_.srv);
        device_->CreateRenderTargetView(canvas_.tex.Get(), nullptr, &canvas_rtv_);
        canvas_.w = w; canvas_.h = h;
        canvas_copy_ = Texture{};
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        device_->CreateTexture2D(&td, nullptr, &canvas_copy_.tex);
        device_->CreateShaderResourceView(canvas_copy_.tex.Get(), nullptr, &canvas_copy_.srv);
    }
    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* rtv = canvas_rtv_.Get();
    ctx_->ClearRenderTargetView(rtv, black);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* b = cb_.Get();
    ctx_->VSSetConstantBuffers(0, 1, &b);
    ctx_->PSSetConstantBuffers(0, 1, &b);
    ID3D11SamplerState* s = sampler_.Get();
    ctx_->PSSetSamplers(0, 1, &s);
    ctx_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    const auto& tracks = project_["tracks"];
    for (int i = int(tracks.size()) - 1; i >= 0; i--) {     // Alttaki kanal önce
        if (tracks[i].value("kind", std::string()) != "video" || tracks[i].value("hidden", false)) continue;
        const std::string id = tracks[i].value("id", std::string());
        std::vector<const json*> clips;
        for (auto& c : project_["clips"])
            if (c.value("track", std::string()) == id) clips.push_back(&c);
        std::sort(clips.begin(), clips.end(), [](const json* a, const json* b) { return a->value("start", 0.0) < b->value("start", 0.0); });
        for (const json* cp : clips) {
            const json& c = *cp;
            const double start = c.value("start", 0.0), length = c.value("length", 0.0);
            if (time_ < start || time_ >= start + length + clip_tail(project_, c)) continue;
            const double local = time_ - start;
            double fade = 1;
            const double fi = c.value("fade_in", 0.0), fo = c.value("fade_out", 0.0);
            if (fi > 0) fade = std::min(fade, local / fi);
            if (fo > 0) fade = std::max(0.0, std::min(fade, (length - local) / fo));
            json placed = clip_at(c, local);
            ID3D11ShaderResourceView* srv = nullptr;
            const std::string type = c.value("type", std::string());
            const float unit = float(ch) / 1080.f;
            if (type == "spot") {       // Tuvalin tamamına çizilir, alanın dışı kararır
                const Placement r = clip_placement(c, float(cw), float(ch));
                ctx_->PSSetShader(ps_spot_.Get(), nullptr, 0);
                draw_quad(nullptr, Placement{0, 0, float(cw), float(ch)}, c, fade, 0, 1.0, [&](Quad& q) {
                    q.angle = 0;
                    q.opacity = float(std::clamp(c.value("dark", 70), 0, 100) / 100.0 * fade);
                    q.frame[0] = float(cw); q.frame[1] = float(ch); q.frame[2] = c.value("corner", 24) * unit;
                    q.frame2[0] = c.value("spot_shape", std::string("ellipse")) == "ellipse" ? 1.f : 0.f;
                    q.frame2[1] = c.value("feather", 60) * unit;
                    q.maskbox[0] = r.x + r.w / 2; q.maskbox[1] = r.y + r.h / 2; q.maskbox[2] = r.w / 2; q.maskbox[3] = r.h / 2;
                });
                ctx_->PSSetShader(ps_.Get(), nullptr, 0);
                continue;
            }
            if (type == "blur") {       // O ana kadar çizilenin kopyasından bölge efekti
                ID3D11RenderTargetView* none = nullptr;
                ctx_->OMSetRenderTargets(1, &none, nullptr);
                ctx_->CopyResource(canvas_copy_.tex.Get(), canvas_.tex.Get());
                ctx_->OMSetRenderTargets(1, &rtv, nullptr);
                json region = c;
                region["rotate"] = 0.0; region["opacity"] = 100;
                region["blur"] = double(c.value("strength", 20));
                region["invert"] = c.value("mode", std::string("blur")) == "pixel";
                for (const char* k : {"brightness", "sepia", "vignette"}) region[k] = 0.0;
                region["contrast"] = 1.0; region["saturation"] = 1.0; region["chroma"] = false;
                for (const char* k : {"crop_l", "crop_r", "crop_t", "crop_b"}) region[k] = 0.0;
                region["flip_h"] = false; region["flip_v"] = false;
                ctx_->PSSetShader(ps_region_.Get(), nullptr, 0);
                draw_quad(canvas_copy_.srv.Get(), clip_placement(c, float(cw), float(ch)), region, 1.0, 0, 1.0);
                ctx_->PSSetShader(ps_.Get(), nullptr, 0);
                ID3D11ShaderResourceView* no_srv = nullptr;
                ctx_->PSSetShaderResources(0, 1, &no_srv);
                continue;
            }
            if (type == "video") {
                auto it = live_.find(c.value("id", std::string()));
                if (it == live_.end() || !it->second.frame.srv) continue;
                srv = it->second.frame.srv.Get();
            } else {
                Texture* t = still_texture(c);
                srv = t->srv.Get();
                if (clip_is_visual_still(c)) { placed["w"] = t->w; placed["h"] = t->h; }
            }
            const Motion motion = clip_motion(c, local);
            if (motion.chars >= 0 && type == "text") {      // Daktilo: harfler sırayla görünür, yazının yeri değişmez
                json typed = c;
                typed["visible_chars"] = motion.chars;
                typed["id"] = c.value("id", std::string()) + "#daktilo";
                srv = still_texture(typed)->srv.Get();
            }
            if (!srv) continue;
            json unkeyed = c;       // Köşe ve kenarlık animasyonsuz boyuta göre ölçülür (dışa aktarmadaki gibi)
            unkeyed["w"] = placed.value("w", 0); unkeyed["h"] = placed.value("h", 0);
            const Placement base = clip_placement(unkeyed, float(cw), float(ch));
            Placement p = clip_placement(placed, float(cw), float(ch));
            if (motion.scale != 1) {
                const float s = float(motion.scale);
                p.x += p.w * (1 - s) / 2; p.y += p.h * (1 - s) / 2; p.w *= s; p.h *= s;
            }
            p.x += float(motion.dx) * cw;
            p.y += float(motion.dy) * ch;
            fade *= motion.alpha;
            if (c.value("anim", false) && length > 0 && clip_keys(c).empty()) {     // Eski projelerdeki yavaşça yakınlaşma
                json last = placed;
                last["scale"] = c.value("anim_scale", 130.0);
                last["x"] = c.value("anim_x", 0.0);
                last["y"] = c.value("anim_y", 0.0);
                const Placement e = clip_placement(last, float(cw), float(ch));
                const float k = float(std::min(1.0, local / length));
                p.x += (e.x - p.x) * k; p.y += (e.y - p.y) * k; p.w += (e.w - p.w) * k; p.h += (e.h - p.h) * k;
            }
            // Geçiş: giriş ya da çıkıştan hangisi sürüyorsa
            int mask = 0;
            double progress = 1;
            const Transition tin = transition_in(project_, c), tout = transition_out(project_, c);
            const TransitionInfo* in = find_transition(tin.key), *out = find_transition(tout.key);
            const TransitionInfo* active = nullptr;
            const double di = tin.duration, dv = tout.duration;
            if (in && local < di) { active = in; progress = local / std::max(di, 0.01); }
            else if (out && local > length - dv) { active = out; progress = std::max(0.0, (length - local) / std::max(dv, 0.01)); }
            if (active) {
                mask = active->mask;
                const float gone = float(1 - progress);
                if (active->slide == 1) p.x += gone * p.w;
                if (active->slide == 2) p.x -= gone * p.w;
                if (active->slide == 3) p.y += gone * p.h;
                if (active->slide == 4) p.y -= gone * p.h;
                if (active->slide == 5) { const float k = float(0.2 + 0.8 * progress); p.x += p.w * (1 - k) / 2; p.y += p.h * (1 - k) / 2; p.w *= k; p.h *= k; }
                if (active->mask == 0 && active->slide == 0) fade *= progress;
                if (active->mask == 9) fade *= progress;
                if (active->slide == 5) fade *= progress;
            }
            const bool framed = clip_has_frame(c);
            const FrameShape shape = frame_shape(c, base.w, base.h, unit);
            auto set_frame = [&](Quad& q) {
                q.frame[0] = base.w; q.frame[1] = base.h; q.frame[2] = shape.radius; q.frame[3] = shape.border;
                q.frame2[0] = shape.ellipse ? 1.f : 0.f; q.frame2[1] = shape.feather;
                q.maskbox[0] = shape.cx; q.maskbox[1] = shape.cy; q.maskbox[2] = shape.hx; q.maskbox[3] = shape.hy;
                const uint32_t b = parse_color(c.value("border_color", std::string("#ffffff")));
                q.bcolor[0] = ((b >> 16) & 255) / 255.f; q.bcolor[1] = ((b >> 8) & 255) / 255.f; q.bcolor[2] = (b & 255) / 255.f;
                q.frame2[3] = float(motion.reveal);
            };
            if (c.value("shadow", false) && base.w > 1 && base.h > 1) {     // Gölge: kaydırılmış, yumuşak, tek renk kopya altta
                const float k = p.w / base.w, soft = std::max(0, c.value("shadow_blur", 24)) * unit;
                const float margin = (std::max(fabsf(float(c.value("shadow_x", 10))), fabsf(float(c.value("shadow_y", 14)))) * unit + soft * 1.5f + 4);
                const float a = float(placed.value("rotate", 0.0) * 3.14159265 / 180);
                const float ox = c.value("shadow_x", 10) * unit * k, oy = c.value("shadow_y", 14) * unit * k;
                Placement sp = p;
                sp.x += ox * cosf(a) - oy * sinf(a) - margin * k; sp.y += ox * sinf(a) + oy * cosf(a) - margin * k;
                sp.w += 2 * margin * k; sp.h += 2 * margin * k;
                draw_quad(srv, sp, placed, fade, mask, progress, [&](Quad& q) {
                    if (framed) set_frame(q);
                    else {
                        q.frame[0] = base.w; q.frame[1] = base.h;
                        q.maskbox[0] = base.w / 2; q.maskbox[1] = base.h / 2; q.maskbox[2] = base.w / 2; q.maskbox[3] = base.h / 2;
                        q.frame2[3] = float(motion.reveal);
                    }
                    q.bcolor[3] = 1;
                    q.frame2[1] = std::max(shape.feather, soft);
                    q.frame2[2] = 1;
                    q.expand[0] = margin / base.w; q.expand[1] = margin / base.h;
                    q.blur = soft / std::max(1.f, base.h) * 270.f;
                    const uint32_t s = parse_color(c.value("shadow_color", std::string("#000000")));
                    q.scolor[0] = ((s >> 16) & 255) / 255.f; q.scolor[1] = ((s >> 8) & 255) / 255.f; q.scolor[2] = (s & 255) / 255.f;
                    q.opacity *= std::clamp(c.value("shadow_opacity", 60), 0, 100) / 100.f;
                });
            }
            draw_quad(srv, p, placed, fade, mask, progress, [&](Quad& q) {
                q.frame2[3] = float(motion.reveal);
                if (framed) { set_frame(q); q.bcolor[3] = 1; }
            });
            if (local <= length) draw_input_effects(c, local);
        }
    }
    draw_captions();
    ID3D11RenderTargetView* none = nullptr;
    ctx_->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* no_srv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &no_srv);
}

// --- Arayüz ---

void App::main_menu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Dosya")) {
        if (ImGui::MenuItem("Yeni proje", "Ctrl+N")) reset_project(new_project(), L"");
        if (ImGui::MenuItem("Proje aç...", "Ctrl+O")) {
            std::vector<std::wstring> files;
            if (!pick_files(hwnd, files, L"Video Düzenleyici projesi", L"*.vproje", false).empty()) open_project(files[0]);
        }
        if (ImGui::MenuItem("Kaydet", "Ctrl+S")) save_project(false);
        if (ImGui::MenuItem("Farklı kaydet...")) save_project(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Medya ekle...", "Ctrl+I")) {
            std::vector<std::wstring> files;
            pick_files(hwnd, files, L"Video, ses ve resim", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.ts;*.flv;*.wmv;*.m4v;*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif", true);
            for (auto& f : files) import_media(f);
        }
        if (ImGui::MenuItem("Dışa aktar...", "Ctrl+E")) show_export_ = true;
        if (ImGui::MenuItem(T("Toplu dışa aktar..."))) show_batch_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem(T("Şablon olarak kaydet..."))) show_template_ = true;
        if (ImGui::BeginMenu(T("Şablondan yeni proje"))) {
            std::error_code ec;
            bool any = false;
            for (auto& entry : fs::directory_iterator(templates_dir(), ec)) {
                if (entry.path().extension() != L".vproje") continue;
                any = true;
                if (ImGui::MenuItem(narrow(entry.path().stem().wstring()).c_str())) {
                    json t = read_json(entry.path().wstring());
                    if (t.is_object() && t.contains("clips")) {
                        t.erase("template_name");
                        reset_project(t, L"");      // Kaydedilmemiş yeni proje: şablonun üstüne yazılmaz
                        unsaved_ = true;
                    }
                }
            }
            if (!any) ImGui::TextDisabled("%s", T("Henüz şablon yok"));
            ImGui::Separator();
            if (ImGui::MenuItem(T("Şablon klasörünü aç"))) {
                SHCreateDirectoryExW(nullptr, templates_dir().c_str(), nullptr);
                ShellExecuteW(nullptr, L"open", templates_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Çıkış")) want_close = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Düzenle")) {
        if (ImGui::MenuItem("Geri al", "Ctrl+Z", false, !undo_.empty())) undo();
        if (ImGui::MenuItem("Yinele", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        const bool has = selected() != nullptr;
        if (ImGui::MenuItem("Klibi böl", "S", false, has)) split_selected();
        if (ImGui::MenuItem("Çoğalt", "Ctrl+D", false, has)) duplicate_selected();
        if (ImGui::MenuItem("Sil", "Delete", false, has)) remove_picked(false);
        if (ImGui::MenuItem("Sil ve boşluğu kapat", "Shift+Delete", false, has)) remove_picked(true);
        if (ImGui::MenuItem("Sesi ayır", nullptr, false, has)) detach_audio();
        ImGui::Separator();
        if (ImGui::MenuItem("Aralığı oynat", "P", false, has_range())) play_range();
        if (ImGui::MenuItem("Aralığı sil", nullptr, false, has_range())) range_edit(1);
        if (ImGui::MenuItem("Aralık dışını sil", nullptr, false, has_range())) range_edit(2);
        if (ImGui::MenuItem("Aralığı kaldır", "Esc", false, has_range())) range_in_ = range_out_ = -1;
        ImGui::Separator();
        if (ImGui::MenuItem("Ayarlarını kopyala", "Ctrl+Shift+C", false, has)) copy_style();
        if (ImGui::MenuItem("Ayarlarını yapıştır", "Ctrl+Shift+V", false, has && style_clipboard_.is_object())) paste_style();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Ekle")) {
        if (ImGui::MenuItem("Yazı")) add_generated("text");
        if (ImGui::MenuItem("Alt başlık")) add_shape("lower");
        if (ImGui::MenuItem("Konuşma balonu")) add_shape("bubble");
        if (ImGui::MenuItem("Ok")) add_shape("arrow");
        if (ImGui::MenuItem("Düz renk")) add_generated("color");
        ImGui::Separator();
        if (ImGui::MenuItem("Görüntü kanalı")) add_track("video");
        if (ImGui::MenuItem("Ses kanalı")) add_track("audio");
        if (ImGui::MenuItem("İşaret", "M")) { project_["markers"].push_back({{"t", time_}, {"name", ""}}); mark_edit(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Görünüm")) {
        layout_menu("editor", layout_, layout_presets());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Proje")) {
        if (ImGui::MenuItem("Proje ayarları...")) show_canvas_ = true;
        if (ImGui::MenuItem(T("Bölümler..."))) show_chapters_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Araçlar")) {
        if (ImGui::MenuItem("Kayıt Stüdyosu'nu aç")) open_recorder();
        ImGui::Separator();
        if (ImGui::MenuItem("Ayarlar")) { page_ = "settings"; settings_tab_ = "theme"; }
        if (ImGui::MenuItem("Kısayollar")) { page_ = "settings"; settings_tab_ = "keys"; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Yardım")) {
        const char* items[] = {"Kullanım Kılavuzu", "İletişim", "Geliştiriciler", "Lisans Sözleşmesi", "Gizlilik", "Telif Hakları"};
        if (ImGui::MenuItem(items[0], "F1")) manual_open();
        for (int i = 1; i < 6; i++)
            if (ImGui::MenuItem(items[i])) { help_request_ = i; page_ = "settings"; settings_tab_ = "help"; }
        ImGui::EndMenu();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 170);
    if (ImGui::Button("\xEE\x9C\x94  Kayıt Stüdyosu", ImVec2(162, 0))) open_recorder();
    ImGui::SetItemTooltip("Kayıt Stüdyosu'na geç. Orada Araçlar > Son kaydı Video Düzenleyici'de aç ile kaydınız buraya gelir.");
    ImGui::EndMenuBar();
}

void App::shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || page_ != "edit" || show_export_ || show_canvas_ || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift;
    auto press = [](ImGuiKey k, bool repeat = false) { return ImGui::IsKeyPressed(k, repeat); };
    const double frame = 1.0 / std::max(project_["canvas"].value("fps", 60), 1);
    if (press(ImGuiKey_Space)) set_playing(!playing_);
    if (press(ImGuiKey_LeftArrow, true)) seek(time_ - (shift ? 1.0 : frame));
    if (press(ImGuiKey_RightArrow, true)) seek(time_ + (shift ? 1.0 : frame));
    if (press(ImGuiKey_Home)) seek(0);
    if (press(ImGuiKey_End)) seek(project_length(project_));
    if (!ctrl && press(ImGuiKey_S)) split_selected();
    if (!ctrl && press(ImGuiKey_M)) { project_["markers"].push_back({{"t", time_}, {"name", ""}}); mark_edit(); }
    if (!ctrl && press(ImGuiKey_I)) { range_in_ = time_; if (range_out_ <= range_in_) range_out_ = -1; }
    if (!ctrl && press(ImGuiKey_O)) { range_out_ = time_; if (range_in_ < 0 || range_in_ >= range_out_) range_in_ = 0; }
    if (!ctrl && press(ImGuiKey_P)) play_range();
    if (press(ImGuiKey_Escape)) range_in_ = range_out_ = -1;
    if (press(ImGuiKey_Delete) && selected()) remove_picked(shift);
    if (ctrl && press(ImGuiKey_Z)) undo();
    if (ctrl && press(ImGuiKey_Y)) redo();
    if (ctrl && press(ImGuiKey_S)) save_project(false);
    if (ctrl && press(ImGuiKey_D)) duplicate_selected();
    if (ctrl && press(ImGuiKey_E)) show_export_ = true;
    if (ctrl && press(ImGuiKey_R)) toggle_mic();
    if (ctrl && press(ImGuiKey_N)) reset_project(new_project(), L"");
    if (ctrl && shift && press(ImGuiKey_C)) copy_style();
    if (ctrl && shift && press(ImGuiKey_V)) paste_style();
    if (ctrl && press(ImGuiKey_G)) group_picked(!shift);
    if (ctrl && press(ImGuiKey_A)) {
        picks_.clear();
        for (auto& c : project_["clips"]) { selected_ = c.value("id", std::string()); picks_.insert(selected_); }
    }
    if (ctrl && !shift && press(ImGuiKey_C) && selected()) clipboard_ = *selected();
    if (ctrl && !shift && press(ImGuiKey_V) && clipboard_.is_object()) {
        json copy = clipboard_;
        copy["id"] = new_clip("audio", "", 0, 0).value("id", std::string());
        copy["group"] = "";
        copy["start"] = time_;
        if (overlaps(copy, copy.value("track", std::string()), time_, copy.value("length", 0.0)))
            copy["track"] = track_for(copy.value("type", std::string()) == "audio" ? "audio" : "video", time_, copy.value("length", 0.0));
        select_only(copy.value("id", std::string()));
        project_["clips"].push_back(copy);
        mark_edit();
    }
    if (press(ImGuiKey_Equal, true) || press(ImGuiKey_KeypadAdd, true)) zoom_ = std::min(zoom_ * 1.25, 800.0);
    if (press(ImGuiKey_Minus, true) || press(ImGuiKey_KeypadSubtract, true)) zoom_ = std::max(zoom_ / 1.25, 2.0);
}

void App::panel_media(ImVec2 size) {
    begin_panel("Kitaplık", size);
    if (ImGui::BeginTabBar("kitaplik", ImGuiTabBarFlags_FittingPolicyShrink)) {     // Kısa adlar sığar, tam adı ipucunda
        struct Tab { const char* label; const char* tip; void (App::*draw)(); };
        static const Tab tabs[] = {{"Medya", "Medya", &App::tab_media}, {"Geçiş", "Geçişler", &App::tab_transitions},
                                   {"Şekil", "Açıklamalar: yazı, şekil, bulanık alan, spot ışığı", &App::tab_annotations},
                                   {"Efekt", "Efektler", &App::tab_effects}, {"Yakın", "Yakınlaştır ve kaydır", &App::tab_zoom},
                                   {"Altyazı", "Altyazı", &App::tab_captions}};
        for (auto& t : tabs) {
            const bool open = ImGui::BeginTabItem((std::string(T(t.label)) + "###" + t.label).c_str());     // Dil değişince seçili sekme korunur
            ImGui::SetItemTooltip("%s", T(t.tip));
            if (open) { (this->*t.draw)(); ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void App::tab_transitions() {
    ImGui::TextWrapped("Geçişi zaman çizelgesindeki klibin başına ya da sonuna sürükleyin. Art arda iki klibin arasında ikisi birbirine geçer.");
    ImGui::BeginChild("gecis_liste", ImVec2(0, -70), ImGuiChildFlags_Borders);
    const auto& list = transition_list();
    for (int i = 0; i < int(list.size()); i++) {
        ImGui::PushID(i);
        ImGui::Button(list[i].name, ImVec2(-1, 28));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("GECIS", &i, sizeof(i));
            ImGui::TextUnformatted(list[i].name);
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(picks_.empty());
    static int chosen = 0;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##gecis", list[chosen].name)) {
        for (int i = 0; i < int(list.size()); i++)
            if (ImGui::Selectable(list[i].name, i == chosen)) chosen = i;
        ImGui::EndCombo();
    }
    if (ImGui::Button("Seçilenlerin başına", ImVec2(ImGui::GetContentRegionAvail().x / 2 - 4, 26))) apply_transition(list[chosen].key, true);
    ImGui::SameLine();
    if (ImGui::Button("Sonuna", ImVec2(-1, 26))) apply_transition(list[chosen].key, false);
    if (ImGui::Button("Seçilenlerin geçişlerini kaldır", ImVec2(-1, 26))) remove_transitions();
    ImGui::EndDisabled();
}

void App::tab_annotations() {
    ImGui::TextWrapped("Tıklayın: işaretçinin olduğu yere eklenir. Özellikler'den yazısını, rengini ve boyutunu değiştirin.");
    struct Item { const char* key; const char* name; };
    if (ImGui::Button("Yazı", ImVec2(-1, 30))) add_generated("text");
    for (auto it : {Item{"lower", "Alt başlık"}, Item{"bubble", "Konuşma balonu"}, Item{"rect", "Dikdörtgen"}, Item{"ellipse", "Elips"}, Item{"frame", "Vurgu çerçevesi"},
                    Item{"highlight", "Fosforlu vurgu"}, Item{"arrow", "Ok"}, Item{"line", "Çizgi"}, Item{"badge", "Numara rozeti"},
                    Item{"blur", "Bulanık alan (gizli bilgi)"}, Item{"pixel", "Pikselli alan (gizli bilgi)"}, Item{"spot", "Spot ışığı"}})
        if (ImGui::Button(it.name, ImVec2(-1, 30))) add_shape(it.key);
    if (ImGui::Button("Düz renk", ImVec2(-1, 30))) add_generated("color");
}

void App::tab_effects() {
    ImGui::TextWrapped("Zaman çizelgesinde klipleri seçip efekte tıklayın. Ayarları Özellikler'den değiştirilir.");
    struct Item { const char* key; const char* name; };
    ImGui::BeginDisabled(picks_.empty());
    for (auto it : {Item{"zoom", "Yavaşça yakınlaş"}, Item{"blur", "Bulanıklaştır"}, Item{"vignette", "Kenar karartma"}, Item{"vivid", "Canlı renk"}, Item{"cinema", "Sinematik"},
                    Item{"gray", "Siyah beyaz"}, Item{"sepia", "Sepya"}, Item{"invert", "Negatif"}, Item{"chroma", "Yeşil perde"}, Item{"flip", "Aynala"}})
        if (ImGui::Button(it.name, ImVec2(-1, 30))) apply_effect(it.key);
    if (ImGui::Button(T("Renk filtresi (LUT) uygula..."), ImVec2(-1, 30))) {
        std::vector<std::wstring> files;
        if (!pick_files(hwnd, files, TW("Renk filtresi (LUT)").c_str(), L"*.cube", false).empty()) {
            for (auto& id : picks_)
                if (json* c = clip_by_id(id); c && clip_can_frame(*c)) (*c)["lut"] = narrow(files[0]);
            mark_edit();
        }
    }
    ImGui::SeparatorText("Görünüm");
    for (auto it : {Item{"shadow", "Gölge"}, Item{"round", "Yuvarlak köşe"}, Item{"border", "Kenarlık"}, Item{"circle", "Daire maske"}})
        if (ImGui::Button(it.name, ImVec2(-1, 30))) apply_effect(it.key);
    ImGui::SeparatorText("Giriş / çıkış hareketi");
    for (auto& m : motion_list())
        if (ImGui::Button(m.name, ImVec2(-1, 30))) apply_effect(std::string("m_") + m.key);
    ImGui::SeparatorText("İmleç ve klavye (Kayıt Stüdyosu kayıtları)");
    for (auto it : {Item{"halo", "İmleç vurgusu"}, Item{"rings", "Tıklama halkası"}, Item{"keys", "Tuş göstergesi"}})
        if (ImGui::Button(it.name, ImVec2(-1, 30))) apply_effect(it.key);
    ImGui::SeparatorText("Ses efekti");
    for (auto& v : voice_list())
        if (ImGui::Button(v.name, ImVec2(-1, 30))) apply_effect(std::string("v_") + v.key);
    ImGui::SeparatorText("Zaman");
    if (ImGui::Button("Ters oynat", ImVec2(-1, 30))) apply_effect("reverse");
    ImGui::SetItemTooltip("Seçili video ya da ses klibini sondan başa oynatır. Tekrar basınca kapanır.");
    if (ImGui::Button("Donmuş kare ekle (işaretçide)", ImVec2(-1, 30))) freeze_frame();
    ImGui::SetItemTooltip("İşaretçideki kareyi 3 saniyelik resim olarak araya ekler, arkadaki klipler kayar.");
    ImGui::Spacing();
    if (ImGui::Button("Seçilenlerin efektlerini kaldır", ImVec2(-1, 30))) remove_effect("");
    ImGui::EndDisabled();
}

// Seçili görüntü klipleri; seçim yoksa işaretçinin altındakiler
std::vector<std::string> App::zoom_targets() {
    std::vector<std::string> out;
    auto usable = [&](const json& c) {
        const std::string type = c.value("type", std::string());
        return type != "audio" && type != "blur" && type != "spot" && time_ >= c.value("start", 0.0) && time_ <= c.value("start", 0.0) + c.value("length", 0.0);
    };
    for (auto& id : picks_)
        if (json* c = clip_by_id(id); c && usable(*c)) out.push_back(id);
    if (out.empty())
        for (auto& c : project_["clips"])
            if (usable(c)) out.push_back(c.value("id", std::string()));
    return out;
}

// Seçilen alan (tuval pikseli) ekranı dolduracak şekilde işaretçiden itibaren yakınlaşır
void App::zoom_to(float rx, float ry, float rw, float rh) {
    const float W = float(project_["canvas"].value("w", 1920)), H = float(project_["canvas"].value("h", 1080));
    const auto targets = zoom_targets();
    if (targets.empty()) { message_ = "İşaretçinin altında yakınlaşacak görüntü yok"; message_error_ = false; return; }
    const double k = W / std::max(1.f, rw);
    for (auto& id : targets) {
        json& c = *clip_by_id(id);
        const double local = local_time(c), length = c.value("length", 0.0), end = std::min(length, local + zoom_time_);
        json view = clip_at(c, local);
        if (clip_is_visual_still(c)) {
            Texture* t = still_texture(c);
            view["w"] = t->w; view["h"] = t->h;
        }
        const Placement p = clip_placement(view, W, H);
        const double cx = p.x + p.w / 2, cy = p.y + p.h / 2, zx = rx + rw / 2, zy = ry + rh / 2;
        json target = view;
        target["scale"] = view.value("scale", 100.0) * k;
        target["x"] = (W / 2 + (cx - zx) * k - W / 2) / W;
        target["y"] = (H / 2 + (cy - zy) * k - H / 2) / H;
        set_key(c, local, view, frame_time() / 2);
        set_key(c, end, target, frame_time() / 2);
    }
    message_ = "Yakınlaşma eklendi. Uzaklaşmak için işaretçiyi ilerletip Uzaklaş'a basın.";
    message_error_ = false;
    mark_edit();
}

void App::zoom_out() {
    const auto targets = zoom_targets();
    for (auto& id : targets) {
        json& c = *clip_by_id(id);
        const double local = local_time(c), end = std::min(c.value("length", 0.0), local + zoom_time_);
        json view = clip_at(c, local);
        json base = c;      // Animasyonsuz hali
        set_key(c, local, view, frame_time() / 2);
        set_key(c, end, base, frame_time() / 2);
    }
    if (!targets.empty()) mark_edit();
}

void App::tab_captions() {
    if (!project_.contains("captions") || !project_["captions"].is_array()) project_["captions"] = json::array();
    json& caps = project_["captions"];
    auto sort = [&] {
        std::vector<json> list(caps.begin(), caps.end());
        std::stable_sort(list.begin(), list.end(), [](const json& a, const json& b) { return a.value("start", 0.0) < b.value("start", 0.0); });
        caps = json(list);
    };
    bool e = false;
    if (ImGui::Button("İşaretçiye altyazı ekle", ImVec2(-1, 30))) {
        caps.push_back({{"start", time_}, {"end", time_ + 3.0}, {"text", "Altyazı"}});
        sort();
        e = true;
    }
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (ImGui::Button("SRT içe aktar...", ImVec2(half, 28))) {
        std::vector<std::wstring> files;
        if (!pick_files(hwnd, files, L"Altyazı", L"*.srt", false).empty()) {
            std::ifstream in(fs::path(files[0]), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const json loaded = parse_srt(text);
            for (auto& c : loaded) caps.push_back(c);
            sort();
            e = true;
            message_ = TF("%d altyazı eklendi", int(loaded.size()));
            message_error_ = loaded.empty();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(caps.empty());
    if (ImGui::Button("SRT dışa aktar...", ImVec2(half, 28))) {
        const std::wstring path = pick_save(hwnd, L"Altyazı", L"*.srt", L"srt", L"Altyazi");
        if (!path.empty()) {
            std::ofstream o(fs::path(path), std::ios::binary);
            o << write_srt(caps);
            message_ = "Altyazı kaydedildi";
            message_error_ = false;
        }
    }
    ImGui::EndDisabled();
    bool burn = project_.value("captions_burn", true);
    if (ImGui::Checkbox("Altyazıyı videoya yaz", &burn)) { project_["captions_burn"] = burn; e = true; }
    ImGui::SetItemTooltip("Kapalıyken altyazı videoda görünmez; dışa aktarırken ayrı .srt dosyası olarak kaydedebilirsiniz.");
    if (ImGui::CollapsingHeader("Görünüm")) {
        json style = project_.value("caption_style", default_caption_style());
        bool s = false;
        ImGui::PushItemWidth(-110);
        s |= edit_int("Yazı boyutu", style, "size", 20, 120);
        s |= edit_color("Renk", style, "color");
        s |= edit_int("Arka plan", style, "bg_opacity", 0, 100);
        s |= slider("Dikey konum", style, "y", -0.45f, 0.45f);
        ImGui::PopItemWidth();
        if (s) { project_["caption_style"] = style; e = true; }
    }
    ImGui::Separator();
    const int fps = project_["canvas"].value("fps", 30);
    int remove = -1;
    for (size_t i = 0; i < caps.size(); i++) {
        json& c = caps[i];
        ImGui::PushID(int(i));
        const bool now = time_ >= c.value("start", 0.0) && time_ < c.value("end", 0.0);
        if (ImGui::Selectable(format_time(c.value("start", 0.0), fps).c_str(), now, 0, ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) seek(c.value("start", 0.0) + 0.001);
        ImGui::SameLine();
        if (ImGui::SmallButton("\xC3\x97")) remove = int(i);
        ImGui::SetItemTooltip("Sil");
        e |= edit_text("##yazi", c, "text", true);     // Enter yeni satır açar
        float a = float(c.value("start", 0.0)), b = float(c.value("end", 0.0));
        const float w = (ImGui::GetContentRegionAvail().x - 8) / 2;
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputFloat("##bas", &a, 0, 0, "%.2f sn")) { c["start"] = double(std::max(0.f, a)); e = true; }
        ImGui::SetItemTooltip("Başlangıç (saniye)");
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputFloat("##bit", &b, 0, 0, "%.2f sn")) { c["end"] = double(std::max(a + 0.1f, b)); e = true; }
        ImGui::SetItemTooltip("Bitiş (saniye)");
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (remove >= 0) { caps.erase(size_t(remove)); e = true; }
    if (caps.empty()) ImGui::TextDisabled("Henüz altyazı yok.");
    if (e) mark_edit();
}

void App::tab_zoom() {
    ImGui::TextWrapped("İşaretçiyi yakınlaşmanın başlayacağı ana getirin, \"Alan seç\"e basıp önizlemede yakınlaşılacak bölgeyi çizin. Seçili klip yoksa işaretçinin altındaki görüntüler yakınlaşır.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-90);
    slider_float("Süre (sn)", &zoom_time_, 0.1f, 5.f, "%.1f");
    if (!zoom_pick_) {
        if (ImGui::Button("Alan seç ve yakınlaş", ImVec2(-1, 32))) zoom_pick_ = true;
    } else if (ImGui::Button("Vazgeç (önizlemede alan çiziliyor)", ImVec2(-1, 32))) zoom_pick_ = false;
    if (ImGui::Button("Uzaklaş (tam görünüme dön)", ImVec2(-1, 32))) zoom_out();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Yakınlaşmalar animasyon noktası olarak eklenir. Zaman çizelgesindeki elmasları sürükleyerek zamanlamayı, Özellikler'den ayrıntıları değiştirin.");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(picks_.empty());
    if (ImGui::Button("Seçilenlerin animasyonunu kaldır", ImVec2(-1, 28))) remove_effect("anim");
    ImGui::EndDisabled();
}

void App::tab_media() {
    if (ImGui::Button(ICON_ADD "  Medya ekle", ImVec2(-1, 30))) {
        std::vector<std::wstring> files;
        pick_files(hwnd, files, L"Video, ses ve resim", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.ts;*.flv;*.wmv;*.m4v;*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.opus;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif", true);
        for (auto& f : files) import_media(f);
    }
    const bool recording = mic_.running();
    if (recording) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(220, 53, 69, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
    }
    char label[64] = "Mikrofondan ses kaydet";
    if (recording) sprintf_s(label, "Kaydı bitir  %s", format_time(mic_.seconds(), 100).c_str());
    if (ImGui::Button(label, ImVec2(-1, 30))) toggle_mic();
    if (recording) {
        ImGui::PopStyleColor(2);
        ImGui::ProgressBar(mic_.level(), ImVec2(-1, 6), "");
    } else {
        ImGui::Checkbox("Kaydederken videoyu oynat", &mic_play_);
        ImGui::SetItemTooltip("Hoparlör sesi mikrofona girmesin diye kulaklık kullanın");
    }
    ImGui::BeginChild("medya_liste", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (media_.empty()) ImGui::TextWrapped("Dosyaları buraya sürükleyin ya da Medya ekle düğmesini kullanın.");
    for (size_t i = 0; i < media_.size(); i++) {
        Media& m = *media_[i];
        ImGui::PushID(int(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##medya", ImVec2(-1, 60));
        auto* dl = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + ImGui::GetItemRectSize().x, p.y + 60), ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4);
        if (m.thumb.srv) dl->AddImage((ImTextureID)(intptr_t)m.thumb.srv.Get(), ImVec2(p.x + 3, p.y + 3), ImVec2(p.x + 3 + m.thumb.w, p.y + 57));
        const float tx = p.x + (m.thumb.srv ? m.thumb.w + 10.f : 8.f);
        dl->PushClipRect(p, ImVec2(p.x + ImGui::GetItemRectSize().x, p.y + 60), true);
        dl->AddText(ImVec2(tx, p.y + 8), ImGui::GetColorU32(ImGuiCol_Text), m.name.c_str());
        char info[64] = "Okunuyor...";
        if (m.failed) strcpy_s(info, "Açılamadı");
        else if (m.ready && m.info.image) sprintf_s(info, "Resim  %d x %d", m.info.width, m.info.height);
        else if (m.ready) sprintf_s(info, "%s", format_time(m.info.duration, 100).c_str());
        dl->AddText(ImVec2(tx, p.y + 32), ImGui::GetColorU32(ImGuiCol_TextDisabled), info);
        dl->PopClipRect();
        if (m.ready && !m.failed) {
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) add_to_timeline(m, "", time_);
            if (ImGui::BeginDragDropSource()) {
                const int index = int(i);
                ImGui::SetDragDropPayload("MEDYA", &index, sizeof(index));
                ImGui::TextUnformatted(m.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void App::panel_preview(ImVec2 size) {
    begin_panel("Önizleme", size);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bar = 40, ah = avail.y - bar;
    const float aspect = float(preview_w_) / std::max(preview_h_, 1);
    float h = ah, w = h * aspect;
    if (w > avail.x) { w = avail.x; h = w / aspect; }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 p(start.x + (avail.x - w) / 2, start.y + (ah - h) / 2);
    auto* dl = ImGui::GetWindowDrawList();
    if (canvas_.srv) dl->AddImage((ImTextureID)(intptr_t)canvas_.srv.Get(), p, ImVec2(p.x + w, p.y + h));
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("onizleme", ImVec2(w, h));
    if (zoom_pick_) {   // Yakınlaşma alanı: tuval oranında kutu
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActivated()) zoom_from_ = io.MousePos;
        if (ImGui::IsItemActive() && ImGui::IsMousePosValid()) {
            const float aspect = w / h;
            float bw = fabs(io.MousePos.x - zoom_from_.x), bh = fabs(io.MousePos.y - zoom_from_.y);
            bw = std::max(bw, bh * aspect); bh = bw / aspect;
            const float x0 = io.MousePos.x < zoom_from_.x ? zoom_from_.x - bw : zoom_from_.x, y0 = io.MousePos.y < zoom_from_.y ? zoom_from_.y - bh : zoom_from_.y;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), IM_COL32(13, 110, 253, 50));
            dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), IM_COL32(13, 110, 253, 255), 0.f, ImDrawFlags_None, 2.f);
            if (bw > 8) {
                const float cw = float(project_["canvas"].value("w", 1920));
                const float k = cw / w;
                ImGui::GetStateStorage()->SetFloat(ImGui::GetID("zx"), (x0 - p.x) * k);
                ImGui::GetStateStorage()->SetFloat(ImGui::GetID("zy"), (y0 - p.y) * k);
                ImGui::GetStateStorage()->SetFloat(ImGui::GetID("zw"), bw * k);
                ImGui::GetStateStorage()->SetFloat(ImGui::GetID("zh"), bh * k);
            }
        }
        if (ImGui::IsItemDeactivated()) {
            auto* st = ImGui::GetStateStorage();
            const float zw = st->GetFloat(ImGui::GetID("zw"), 0);
            if (zw > 8) {   // Tek tıklama seçimi bitirmez, alan çizilene ya da Vazgeç'e basılana kadar beklenir
                zoom_to(st->GetFloat(ImGui::GetID("zx"), 0), st->GetFloat(ImGui::GetID("zy"), 0), zw, st->GetFloat(ImGui::GetID("zh"), 0));
                zoom_pick_ = false;
            }
            st->SetFloat(ImGui::GetID("zw"), 0);
        }
        dl->AddText(ImVec2(p.x + 8, p.y + 8), IM_COL32(255, 255, 255, 230), T("Yakınlaşılacak alanı çizin"));
    }
    // Seçili klip önizlemede sürüklenerek taşınır
    if (json* c = zoom_pick_ ? nullptr : selected()) {
        const std::string type = c->value("type", std::string());
        const float cw = float(project_["canvas"].value("w", 1920)), ch = float(project_["canvas"].value("h", 1080));
        const double t0 = c->value("start", 0.0), t1 = t0 + c->value("length", 0.0);
        if (type != "audio" && time_ >= t0 && time_ < t1) {
            json placed = clip_at(*c, local_time(*c));
            if (type == "text" || type == "shape") {
                Texture* t = still_texture(*c);
                placed["w"] = t->w; placed["h"] = t->h;
            }
            const Placement b = clip_placement(placed, cw, ch);
            const float k = w / cw;
            dl->AddRect(ImVec2(p.x + b.x * k, p.y + b.y * k), ImVec2(p.x + (b.x + b.w) * k, p.y + (b.y + b.h) * k), IM_COL32(255, 200, 40, 230), 0.f, ImDrawFlags_None, 2.f);
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1)) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (!clip_keys(*c).empty()) {   // Animasyonlu klipte işaretçideki anahtar kare değişir
                    json view = clip_at(*c, local_time(*c));
                    view["x"] = view.value("x", 0.0) + d.x / w;
                    view["y"] = view.value("y", 0.0) + d.y / h;
                    set_key(*c, local_time(*c), view, frame_time() / 2);
                } else {
                    (*c)["x"] = c->value("x", 0.0) + d.x / w;
                    (*c)["y"] = c->value("y", 0.0) + d.y / h;
                }
                mark_edit();
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + ah + 6));
    if (icon_button(ICON_PREV, "Başa dön (Home)")) seek(0);
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? ICON_PAUSE "###oynat" : ICON_PLAY "###oynat", ImVec2(52, 30))) set_playing(!playing_);
    ImGui::SetItemTooltip("Oynat / durdur (Boşluk)");
    ImGui::SameLine();
    if (icon_button(ICON_NEXT, "Sona git (End)")) seek(project_length(project_));
    ImGui::SameLine();
    if (icon_button(ICON_CUT, "İşaretçide böl (S)")) split_selected();
    ImGui::SameLine();
    const int fps = project_["canvas"].value("fps", 60);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s  /  %s", format_time(time_, fps).c_str(), format_time(project_length(project_), fps).c_str());
    ImGui::EndChild();
}

void App::panel_inspector(ImVec2 size) {
    begin_panel("Özellikler", size);
    json* cp = selected();
    if (!cp) {
        ImGui::TextWrapped("Düzenlemek için zaman çizelgesinden bir klip seçin. Ctrl ile tıklayarak ya da boş yerden sürükleyerek birden çok klip seçebilirsiniz.");
        ImGui::EndChild();
        return;
    }
    json& c = *cp;
    {   // Eski projelerde olmayan ayarlar varsayılanla tamamlanır
        static std::map<std::string, json> defaults;
        const std::string t = c.value("type", std::string());
        if (!defaults.count(t)) defaults[t] = new_clip(t, "", 0, 0);
        for (auto& [key, value] : defaults[t].items())
            if (!c.contains(key)) c[key] = value;
    }
    const json before = c;
    const std::string type = c.value("type", std::string());
    const bool region = type == "blur" || type == "spot", visual = type != "audio" && !region, still = clip_is_visual_still(c);
    ImGui::BeginChild("ozellik", ImVec2(0, 0));
    ImGui::PushItemWidth(-130);
    bool e = false;
    ImGui::TextDisabled("%s", c.value("name", std::string()).c_str());
    if (picks_.size() > 1) ImGui::TextColored(theme_accent(), "%d klip seçili: yaptığınız değişiklik hepsine uygulanır", int(picks_.size()));
    if (visual) {     // Uygulananlar: × ile tek tek kaldırılır
        std::vector<std::pair<std::string, std::string>> applied;
        const Transition tin = transition_in(project_, c), tout = transition_out(project_, c);
        if (const TransitionInfo* tr = find_transition(tin.key)) applied.push_back({"gecis", std::string(T("Giriş: ")) + T(tr->name)});
        if (const TransitionInfo* tr = find_transition(tout.key)) applied.push_back({"gecis", std::string(T("Çıkış: ")) + T(tr->name)});
        else if (const TransitionInfo* tr = find_transition(c.value("t_out", std::string()))) applied.push_back({"gecis", std::string(T("Sonraki klibe: ")) + T(tr->name)});
        for (auto& info : kEffects)
            if (effect_on(c, info.key)) applied.push_back({info.key, T(info.name)});
        if (!applied.empty()) {
            ImGui::SeparatorText("Uygulananlar");
            for (size_t i = 0; i < applied.size(); i++) {
                ImGui::PushID(int(i));
                if (ImGui::SmallButton(("\xC3\x97  " + applied[i].second).c_str())) {
                    if (applied[i].first == "gecis") remove_transitions();
                    else remove_effect(applied[i].first);
                }
                ImGui::SetItemTooltip("Kaldır");
                ImGui::PopID();
                if (i + 1 < applied.size() && ImGui::GetContentRegionAvail().x > 160) ImGui::SameLine();
            }
            if (ImGui::Button("Hepsini kaldır")) { remove_transitions(); remove_effect(""); }
        }
    }
    if (ImGui::SmallButton("Ayarları kopyala")) copy_style();
    ImGui::SameLine();
    ImGui::BeginDisabled(!style_clipboard_.is_object());
    if (ImGui::SmallButton("Seçilenlere yapıştır")) paste_style();
    ImGui::EndDisabled();
    if (type == "spot") {
        ImGui::SeparatorText("Spot ışığı");
        ImGui::TextWrapped("Alanın dışını karartır, izleyicinin gözünü bu bölgeye çeker. Altındaki kanallara uygulanır.");
        e |= edit_choice("Biçim", c, "spot_shape", {{"ellipse", "Elips"}, {"rect", "Dikdörtgen"}});
        e |= edit_int("Karartma (%)", c, "dark", 0, 100);
        e |= edit_int("Yumuşak kenar", c, "feather", 0, 300);
        if (c.value("spot_shape", std::string()) == "rect") e |= edit_int("Köşe yuvarlaklığı", c, "corner", 0, 300);
        e |= edit_int("Genişlik", c, "shape_w", 20, 1920);
        e |= edit_int("Yükseklik", c, "shape_h", 20, 1080);
        e |= slider("Yatay konum", c, "x", -1, 1);
        e |= slider("Dikey konum", c, "y", -1, 1);
        const float limit = std::max(0.1f, std::min(5.f, float(c.value("length", 1.0))));
        e |= slider("Yumuşak giriş (sn)", c, "fade_in", 0, limit);
        e |= slider("Yumuşak çıkış (sn)", c, "fade_out", 0, limit);
        ImGui::TextDisabled("Önizlemede sürükleyerek de taşıyabilirsiniz.");
    }
    if (type == "blur") {
        ImGui::SeparatorText("Bulanık alan");
        ImGui::TextWrapped("Altındaki kanallarda görünen her şeyi bu bölgede gizler. Kullanıcı adı, sohbet, şifre gibi bilgiler için.");
        e |= edit_choice("Tür", c, "mode", {{"blur", "Bulanık"}, {"pixel", "Pikselli"}});
        e |= edit_int("Güç", c, "strength", 2, 80);
        e |= edit_int("Genişlik", c, "shape_w", 20, 1920);
        e |= edit_int("Yükseklik", c, "shape_h", 20, 1080);
        e |= slider("Yatay konum", c, "x", -1, 1);
        e |= slider("Dikey konum", c, "y", -1, 1);
        ImGui::TextDisabled("Önizlemede sürükleyerek de taşıyabilirsiniz.");
    }
    if (type == "video" || type == "audio") {
        ImGui::SeparatorText("Hız");
        float speed = float(c.value("speed", 1.0));
        if (slider_float("Hız", &speed, 0.25f, 4.f, "%.2f")) {
            const double span = c.value("length", 0.0) * c.value("speed", 1.0);
            c["speed"] = double(speed);
            c["length"] = span / speed;
            e = true;
        }
        e |= edit_bool("Ters oynat", c, "reverse");
        ImGui::SetItemTooltip("Klip sondan başa oynar. Önizlemede iki saniyede bir kısa takılma olabilir, dışa aktarmada olmaz.");
    }
    const bool animated = visual && !clip_keys(c).empty();
    const double local = local_time(c);
    const bool inside = time_ >= c.value("start", 0.0) && time_ <= c.value("start", 0.0) + c.value("length", 0.0);
    if (visual) {
        ImGui::SeparatorText(animated ? "Konum ve boyut (işaretçideki anahtar kare)" : "Konum ve boyut");
        json view = animated ? clip_at(c, local) : c;
        bool ve = false;
        ImGui::BeginDisabled(animated && !inside);
        ve |= edit_int("Opaklık", view, "opacity", 0, 100);
        ve |= slider("Ölçek (%)", view, "scale", 5, 800, "%.0f");
        ve |= slider("Yatay konum", view, "x", -2, 2);
        ve |= slider("Dikey konum", view, "y", -2, 2);
        ve |= slider("Döndür (derece)", view, "rotate", -360, 360, "%.0f");
        ImGui::EndDisabled();
        if (ve) {
            if (animated) set_key(c, local, view, frame_time() / 2);
            else for (const char* k : {"opacity", "scale", "x", "y", "rotate"}) c[k] = view[k];
            e = true;
        }
        e |= edit_bool("Yatay aynala", c, "flip_h");
        ImGui::SameLine();
        e |= edit_bool("Dikey aynala", c, "flip_v");
        if (ImGui::Button("Ortala")) { c["x"] = 0.0; c["y"] = 0.0; e = true; }
        ImGui::SameLine();
        if (ImGui::Button("Sıfırla")) { c["x"] = 0.0; c["y"] = 0.0; c["scale"] = 100.0; c["opacity"] = 100; c["rotate"] = 0.0; e = true; }
    }
    if (type == "video" || type == "image") {
        ImGui::SeparatorText("Kırp (%)");
        e |= slider("Sol", c, "crop_l", 0, 45, "%.0f");
        e |= slider("Sağ", c, "crop_r", 0, 45, "%.0f");
        e |= slider("Üst", c, "crop_t", 0, 45, "%.0f");
        e |= slider("Alt", c, "crop_b", 0, 45, "%.0f");
    }
    if (visual) {
        ImGui::SeparatorText("Geçiş");
        auto pick = [&](const char* label, const char* key) {
            const TransitionInfo* now = find_transition(c.value(key, std::string()));
            if (ImGui::BeginCombo(label, now ? now->name : "Yok")) {
                if (ImGui::Selectable("Yok", !now)) { c[key] = ""; e = true; }
                for (auto& tr : transition_list())
                    if (ImGui::Selectable(tr.name, now == &tr)) { c[key] = tr.key; e = true; }
                ImGui::EndCombo();
            }
        };
        const float limit = std::max(0.1f, std::min(5.f, float(c.value("length", 1.0))));
        pick("Giriş geçişi", "t_in");
        if (find_transition(c.value("t_in", std::string()))) e |= slider("Giriş süresi (sn)", c, "t_in_d", 0.1f, limit);
        pick("Çıkış geçişi", "t_out");
        if (find_transition(c.value("t_out", std::string()))) e |= slider("Çıkış süresi (sn)", c, "t_out_d", 0.1f, limit);
        e |= slider("Yumuşak giriş (sn)", c, "fade_in", 0, limit);
        e |= slider("Yumuşak çıkış (sn)", c, "fade_out", 0, limit);
    }
    if (type == "video" || type == "image" || type == "color") {
        ImGui::SeparatorText("Renk ve efekt");
        e |= slider("Parlaklık", c, "brightness", -0.5f, 0.5f);
        e |= slider("Karşıtlık", c, "contrast", 0.2f, 2.f);
        e |= slider("Doygunluk", c, "saturation", 0, 3);
        e |= slider("Bulanıklık", c, "blur", 0, 30, "%.1f");
        e |= slider("Kenar karartma", c, "vignette", 0, 100, "%.0f");
        e |= slider("Sepya", c, "sepia", 0, 100, "%.0f");
        e |= edit_bool("Negatif", c, "invert");
        {   // Renk filtresi
            const std::string lut = c.value("lut", std::string());
            if (ImGui::Button(lut.empty() ? T("Renk filtresi (LUT) seç...") : T("Başka LUT seç..."))) {
                std::vector<std::wstring> files;
                if (!pick_files(hwnd, files, TW("Renk filtresi (LUT)").c_str(), L"*.cube", false).empty()) { c["lut"] = narrow(files[0]); e = true; }
            }
            ImGui::SetItemTooltip("%s", T(".cube biçimindeki renk filtresi dosyası. Kamera ve oyun görüntüsüne sinematik renk verir."));
            if (!lut.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", narrow(fs::path(widen(lut)).filename().wstring()).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("\xC3\x97##lut")) { c["lut"] = ""; e = true; }
                if (const auto it = luts_.find(lut); it != luts_.end() && it->second.tried && !it->second.srv) ImGui::TextColored(theme_warn(), "%s", T("Bu LUT dosyası okunamadı"));
            }
        }
        e |= edit_bool("Yeşil perde (renk sil)", c, "chroma");
        if (c.value("chroma", false)) {
            e |= edit_color("Silinecek renk", c, "chroma_color");
            e |= slider("Benzerlik", c, "chroma_similarity", 0.01f, 0.6f);
        }
    }
    if (type == "video") {
        ImGui::SeparatorText("İmleç ve klavye");
        const auto data = input_data(c.value("file", std::string()));
        if (!data) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Bu video için imleç verisi yok. Kayıt Stüdyosu'nda \"İmleç ve tıklamaları kaydet\" açıkken alınan kayıtlarda kullanılabilir.");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%d imleç noktası, %d tıklama, %d kısayol", int(data->cursor.size()), int(data->clicks.size()), int(data->keys.size()));
            e |= edit_bool("İmleç vurgusu", c, "cursor_halo");
            if (c.value("cursor_halo", false)) {
                ImGui::SameLine();
                e |= edit_color("##vurgu", c, "halo_color");
                e |= edit_int("Vurgu boyutu", c, "halo_size", 20, 400);
                e |= edit_int("Vurgu opaklığı", c, "halo_opacity", 5, 100);
            }
            e |= edit_bool("Tıklama halkası", c, "click_rings");
            if (c.value("click_rings", false)) { ImGui::SameLine(); e |= edit_color("##halka", c, "ring_color"); }
            ImGui::BeginDisabled(data->keys.empty());
            e |= edit_bool("Tuş göstergesi", c, "keys_show");
            ImGui::EndDisabled();
            if (data->keys.empty()) ImGui::TextDisabled("Kısayol kaydı yok (Kayıt Stüdyosu'nda \"Kısayol tuşlarını kaydet\").");
        }
    }
    if (clip_can_frame(c)) {
        ImGui::SeparatorText("Köşe, kenarlık ve maske");
        e |= edit_int("Köşe yuvarlaklığı", c, "corner", 0, 400);
        e |= edit_int("Kenarlık", c, "border", 0, 60);
        ImGui::SameLine();
        e |= edit_color("##kenarlik", c, "border_color");
        e |= edit_choice("Maske", c, "mask", {{"", "Yok"}, {"rect", "Dikdörtgen"}, {"ellipse", "Elips / daire"}});
        if (!c.value("mask", std::string()).empty()) {
            e |= edit_int("Maske genişliği (%)", c, "mask_w", 5, 100);
            e |= edit_int("Maske yüksekliği (%)", c, "mask_h", 5, 100);
            e |= edit_int("Maske yatay (%)", c, "mask_x", -50, 50);
            e |= edit_int("Maske dikey (%)", c, "mask_y", -50, 50);
        }
        e |= edit_int("Yumuşak kenar", c, "feather", 0, 200);
    }
    if (visual) {
        ImGui::SeparatorText("Gölge");
        e |= edit_bool("Gölge", c, "shadow");
        if (c.value("shadow", false)) {
            ImGui::SameLine();
            e |= edit_color("##golge", c, "shadow_color");
            e |= edit_int("Gölge opaklığı", c, "shadow_opacity", 0, 100);
            e |= edit_int("Gölge yumuşaklığı", c, "shadow_blur", 0, 120);
            e |= edit_int("Gölge yatay", c, "shadow_x", -100, 100);
            e |= edit_int("Gölge dikey", c, "shadow_y", -100, 100);
        }
        ImGui::SeparatorText("Giriş / çıkış hareketi");
        auto motion_pick = [&](const char* label, const char* key) {
            const std::string now = c.value(key, std::string());
            const char* shown = "Yok";
            for (auto& m : motion_list())
                if (now == m.key) shown = m.name;
            if (ImGui::BeginCombo(label, shown)) {
                if (ImGui::Selectable("Yok", now.empty())) { c[key] = ""; e = true; }
                for (auto& m : motion_list()) {
                    if (std::string(m.key) == "type" && type != "text") continue;
                    if (ImGui::Selectable(m.name, now == m.key)) { c[key] = m.key; e = true; }
                }
                ImGui::EndCombo();
            }
        };
        const float limit = std::max(0.1f, std::min(5.f, float(c.value("length", 1.0))));
        motion_pick("Girişte", "m_in");
        if (!c.value("m_in", std::string()).empty()) e |= slider("Giriş hareketi (sn)", c, "m_in_d", 0.1f, limit);
        motion_pick("Çıkışta", "m_out");
        if (!c.value("m_out", std::string()).empty()) e |= slider("Çıkış hareketi (sn)", c, "m_out_d", 0.1f, limit);
    }
    if (visual) {
        ImGui::SeparatorText("Animasyon");
        if (!inside) ImGui::TextColored(theme_warn(), "Anahtar kare için işaretçiyi bu klibin üzerine getirin.");
        ImGui::BeginDisabled(!inside);
        if (ImGui::Button("İşaretçiye anahtar kare ekle")) {
            json view = clip_at(c, local);
            if (clip_keys(c).empty()) set_key(c, 0, view, 0);     // Başlangıç da sabitlenir: önceki görünüm korunur
            set_key(c, local, view, frame_time() / 2);
            e = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Sonra işaretçiyi başka ana götürüp konum, ölçek, dönüş ya da opaklığı değiştirin; aradaki hareket kendiliğinden oluşur.");
        if (c.value("anim", false) && clip_keys(c).empty()) {
            ImGui::TextDisabled("Eski yakınlaşma ayarı açık.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Kaldır##eski")) { c["anim"] = false; e = true; }
        }
        if (c.contains("keys") && c["keys"].is_array() && !c["keys"].empty()) {
            std::vector<std::pair<double, size_t>> order;
            for (size_t i = 0; i < c["keys"].size(); i++) order.push_back({c["keys"][i].value("t", 0.0), i});
            std::sort(order.begin(), order.end());
            int remove = -1;
            for (size_t n = 0; n < order.size(); n++) {
                json& k = c["keys"][order[n].second];
                ImGui::PushID(int(n));
                const bool here = fabs(k.value("t", 0.0) - local) <= frame_time() / 2 && inside;
                if (ImGui::Selectable(format_time(c.value("start", 0.0) + k.value("t", 0.0), project_["canvas"].value("fps", 30)).c_str(), here, 0, ImVec2(110, 0)))
                    seek(c.value("start", 0.0) + k.value("t", 0.0));
                ImGui::SameLine();
                ImGui::TextDisabled("%%%.0f", k.value("scale", 100.0));
                ImGui::SameLine(190);
                bool smooth = k.value("smooth", true);
                if (ImGui::Checkbox("Yumuşak", &smooth)) { k["smooth"] = smooth; e = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("\xC3\x97")) remove = int(order[n].second);
                ImGui::PopID();
            }
            if (remove >= 0) { c["keys"].erase(size_t(remove)); e = true; }
            if (ImGui::SmallButton("Tüm animasyonu kaldır")) { c["keys"] = json::array(); e = true; }
        }
    }
    if (clip_has_audio(c)) {
        ImGui::SeparatorText("Ses");
        e |= edit_int("Ses seviyesi (%)", c, "volume", 0, 200);
        const float limit = std::max(0.1f, float(c.value("length", 1.0)));
        e |= slider("Ses girişi (sn)", c, "audio_fade_in", 0, std::min(limit, 10.f));
        e |= slider("Ses çıkışı (sn)", c, "audio_fade_out", 0, std::min(limit, 10.f));
        e |= edit_int("Gürültü azaltma", c, "denoise", 0, 100);
        ImGui::SetItemTooltip("Fan, uğultu ve hışırtıyı azaltır. Yüksek değer sesi biraz boğuklaştırabilir.");
        e |= edit_int("Ses dengeleme", c, "level", 0, 100);
        ImGui::SetItemTooltip("Kısık yerleri yükseltir, bağıran yerleri yumuşatır.");
        {
            std::vector<std::pair<std::string, std::string>> voices{{"", "Yok"}};
            for (auto& v : voice_list()) voices.push_back({v.key, v.name});
            e |= edit_choice("Ses efekti", c, "voice", voices);
        }
        const size_t points = c.contains("vol_points") && c["vol_points"].is_array() ? c["vol_points"].size() : 0;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Ses noktaları: %d. Zaman çizelgesinde klibin üstündeki sarı çizgiye çift tıklayarak ekleyin.", int(points));
        ImGui::PopStyleColor();
        if (points && ImGui::SmallButton("Ses noktalarını temizle")) { c["vol_points"] = json::array(); e = true; }
        if (type == "video" && ImGui::Button("Sesi ayrı kanala ayır")) detach_audio();
    }
    if (type == "shape") {
        ImGui::SeparatorText("Şekil");
        e |= edit_choice("Biçim", c, "shape", {{"rect", "Dikdörtgen"}, {"ellipse", "Elips"}, {"bubble", "Konuşma balonu"}, {"arrow", "Ok"}, {"line", "Çizgi"}, {"badge", "Rozet"}});
        e |= edit_int("Genişlik", c, "shape_w", 10, 1920);
        e |= edit_int("Yükseklik", c, "shape_h", 10, 1080);
        e |= edit_color("Dolgu", c, "fill");
        ImGui::SameLine();
        e |= edit_color("Kenar", c, "stroke");
        e |= edit_int("Dolgu opaklığı", c, "fill_opacity", 0, 100);
        e |= edit_int("Kenar kalınlığı", c, "stroke_width", 0, 40);
        e |= edit_int("Köşe yuvarlaklığı", c, "radius", 0, 200);
    }
    if (type == "text" || type == "shape") {
        ImGui::SeparatorText("Yazı");
        e |= edit_text("##metin", c, "text", true);
        e |= edit_font(c);
        e |= edit_int("Yazı boyutu", c, "font_size", 8, 200);
        e |= edit_bool("Kalın", c, "bold");
        ImGui::SameLine();
        e |= edit_color("Renk", c, "color");
        if (type == "text") {
            e |= edit_bool("Çizgi", c, "outline");
            ImGui::SameLine();
            e |= edit_color("##cizgi", c, "outline_color");
            e |= edit_bool("Arka plan", c, "background");
            ImGui::SameLine();
            e |= edit_color("##arka", c, "bg_color");
            e |= edit_int("Arka plan opaklığı", c, "bg_opacity", 0, 100);
        }
    }
    if (type == "color") e |= edit_color("Renk", c, "color");
    if (type == "image" || still || type == "color" || region) {
        ImGui::SeparatorText("Süre");
        e |= slider("Süre (sn)", c, "length", 0.2f, 120.f);
    }
    ImGui::PopItemWidth();
    if (e) {
        // Çoklu seçimde değişen ayar diğer kliplere de yazılır
        static const std::set<std::string> own = {"id", "track", "start", "length", "in", "file", "name", "source_length", "w", "h", "has_audio", "type", "audio_stream"};
        for (auto& [key, value] : c.items()) {
            if (own.count(key) || (before.contains(key) && before[key] == value)) continue;
            for (auto& id : picks_) {
                json* other = clip_by_id(id);
                if (!other || other == cp || !other->contains(key)) continue;
                if ((other->value("type", std::string()) == "audio") != (type == "audio") && key != "volume") continue;
                (*other)[key] = value;
            }
        }
        mark_edit();
    }
    ImGui::Spacing();
    if (ImGui::Button("Seçilenleri sil")) remove_picked(false);
    ImGui::EndChild();
    ImGui::EndChild();
}

void App::clip_menu() {
    if (!ImGui::BeginPopup("klip_menu")) return;
    const bool has = selected() != nullptr;
    if (ImGui::MenuItem("İşaretçide böl", "S", false, has)) split_selected();
    if (ImGui::MenuItem("Çoğalt", "Ctrl+D", false, has)) duplicate_selected();
    if (ImGui::MenuItem("Sesi ayır", nullptr, false, has)) detach_audio();
    if (ImGui::MenuItem(T("Grupla"), "Ctrl+G", false, picks_.size() > 1)) group_picked(true);
    if (ImGui::MenuItem(T("Grubu çöz"), "Ctrl+Shift+G", false, has && !selected()->value("group", std::string()).empty())) group_picked(false);
    if (ImGui::MenuItem("İşaretçide donmuş kare ekle", nullptr, false, has)) freeze_frame();
    if (ImGui::MenuItem("Ters oynat (aç / kapat)", nullptr, false, has)) apply_effect("reverse");
    ImGui::Separator();
    if (ImGui::MenuItem("Ayarlarını kopyala", "Ctrl+Shift+C", false, has)) copy_style();
    if (ImGui::MenuItem("Ayarlarını seçilenlere yapıştır", "Ctrl+Shift+V", false, style_clipboard_.is_object())) paste_style();
    if (ImGui::BeginMenu("Giriş geçişi")) {
        if (ImGui::MenuItem("Yok")) apply_transition("", true);
        for (auto& tr : transition_list())
            if (ImGui::MenuItem(tr.name)) apply_transition(tr.key, true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Çıkış geçişi")) {
        if (ImGui::MenuItem("Yok")) apply_transition("", false);
        for (auto& tr : transition_list())
            if (ImGui::MenuItem(tr.name)) apply_transition(tr.key, false);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Geçişleri kaldır", nullptr, false, has)) remove_transitions();
    if (ImGui::BeginMenu("Efekti kaldır", has)) {
        if (ImGui::MenuItem("Hepsini kaldır")) remove_effect("");
        ImGui::Separator();
        for (auto& info : kEffects)
            if (ImGui::MenuItem(info.name, nullptr, false, selected() && effect_on(*selected(), info.key))) remove_effect(info.key);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Sil", "Delete")) remove_picked(false);
    if (ImGui::MenuItem("Sil ve boşluğu kapat", "Shift+Delete")) remove_picked(true);
    ImGui::EndPopup();
}

void App::panel_timeline(ImVec2 size) {
    begin_panel("Zaman Çizelgesi", size);
    ImGui::SameLine(160);
    if (ImGui::SmallButton("+ Görüntü kanalı")) add_track("video");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Ses kanalı")) add_track("audio");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    float zoom = float(zoom_);
    if (ImGui::SliderFloat("##yakinlik", &zoom, 2, 800, T("Yakınlık %.0f"), ImGuiSliderFlags_Logarithmic)) zoom_ = zoom;
    ImGui::SameLine();
    ImGui::Checkbox("Yapıştır", &snapping_);
    ImGui::SetItemTooltip("Klipler kenarlara, işaretçiye ve işaretlere yapışır. Alt basılı tutunca geçici olarak kapanır.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("Başlangıç [I]")) { range_in_ = time_; if (range_out_ <= range_in_) range_out_ = -1; }
    ImGui::SetItemTooltip("Aralığın başını işaretçiye koyar. Cetvelde Shift ile sürükleyerek de aralık seçebilirsiniz.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Bitiş [O]")) { range_out_ = time_; if (range_in_ < 0 || range_in_ >= range_out_) range_in_ = 0; }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_range());
    if (ImGui::SmallButton("Aralığı oynat [P]")) play_range();
    ImGui::SameLine();
    ImGui::Checkbox("Döngü", &range_loop_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Aralığı sil")) range_edit(1);
    ImGui::SetItemTooltip("Aralık bütün kanallardan kesilir, arkası öne gelir. Kilitli kanallar değişmez.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Aralık dışını sil")) range_edit(2);
    ImGui::SetItemTooltip("Yalnızca aralık kalır ve başa alınır.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Kaldır")) range_in_ = range_out_ = -1;
    ImGui::EndDisabled();
    if (has_range()) {
        ImGui::SameLine();
        ImGui::TextColored(theme_accent(), "%s - %s  (%s)", format_time(range_in_, 100).c_str(), format_time(range_out_, 100).c_str(), format_time(range_out_ - range_in_, 100).c_str());
    }
    ImGui::BeginChild("cizelge", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGuiIO& io = ImGui::GetIO();
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    auto& tracks = project_["tracks"];
    const float left = origin.x + kHeaderW, top = origin.y + kRulerH;
    const float rows = std::max(float(tracks.size()) * kTrackH, area.y - kRulerH);
    auto to_x = [&](double t) { return left + float((t - scroll_) * zoom_); };
    auto to_t = [&](float x) { return std::max(0.0, double(x - left) / zoom_ + scroll_); };
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled), line = ImGui::GetColorU32(ImGuiCol_Border);

    ImGui::SetCursorScreenPos(ImVec2(left, origin.y));
    ImGui::InvisibleButton("alan", ImVec2(std::max(1.f, area.x - kHeaderW), std::max(1.f, area.y)), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    // Fare altındaki klip
    const int mouse_row = int(floor((io.MousePos.y - top) / kTrackH));
    const double mouse_t = to_t(io.MousePos.x);
    std::string hit_id;
    Drag hit_kind = Drag::None;
    if (io.MousePos.y >= top && mouse_row >= 0 && mouse_row < int(tracks.size()) && !tracks[mouse_row].value("locked", false)) {
        const std::string track_id = tracks[mouse_row].value("id", std::string());
        for (auto& c : project_["clips"]) {
            if (c.value("track", std::string()) != track_id) continue;
            const float x0 = to_x(c.value("start", 0.0)), x1 = to_x(c.value("start", 0.0) + c.value("length", 0.0));
            if (io.MousePos.x < x0 || io.MousePos.x >= x1) continue;
            hit_id = c.value("id", std::string());
            hit_kind = io.MousePos.x < x0 + kEdge ? Drag::TrimLeft : io.MousePos.x > x1 - kEdge ? Drag::TrimRight : Drag::Move;
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("MEDYA")) {
            const std::string track = mouse_row >= 0 && mouse_row < int(tracks.size()) ? tracks[mouse_row].value("id", std::string()) : std::string();
            add_to_timeline(*media_[*static_cast<const int*>(pl->Data)], track, snap(mouse_t, ""));
        }
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GECIS")) {
            if (json* c = hit_id.empty() ? nullptr : clip_by_id(hit_id)) {
                const bool entering = mouse_t < c->value("start", 0.0) + c->value("length", 0.0) / 2;
                const std::string key = transition_list()[*static_cast<const int*>(pl->Data)].key;
                if (picked(hit_id)) apply_transition(key, entering);
                else set_transition(*c, key, entering);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0 ? 1.2 : 1 / 1.2), 2.0, 800.0);
            scroll_ = std::max(0.0, mouse_t - double(io.MousePos.x - left) / zoom_);
        } else {
            scroll_ = std::max(0.0, scroll_ - io.MouseWheel * 60 / zoom_);
        }
    }

    // Cetvel
    dl->PushClipRect(ImVec2(left, origin.y), ImVec2(origin.x + area.x, origin.y + area.y), true);
    dl->AddRectFilled(ImVec2(left, origin.y), ImVec2(origin.x + area.x, top), ImGui::GetColorU32(ImGuiCol_FrameBg));
    double step = 1;
    for (double s : {0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0})
        if (s * zoom_ >= 70) { step = s; break; }
    const double end_t = to_t(origin.x + area.x);
    for (double t = floor(scroll_ / step) * step; t <= end_t; t += step) {
        const float x = to_x(t);
        dl->AddLine(ImVec2(x, origin.y + 14), ImVec2(x, top), dim);
        char label[24];
        const int sec = int(t);
        sprintf_s(label, "%02d:%02d", sec / 60, sec % 60);
        dl->AddText(ImVec2(x + 3, origin.y), dim, label);
    }
    dl->PopClipRect();

    // Kanallar ve klipler
    for (int row = 0; row < int(tracks.size()); row++) {
        json& track = tracks[row];
        const std::string track_id = track.value("id", std::string());
        const float y = top + row * kTrackH;
        dl->AddLine(ImVec2(origin.x, y + kTrackH), ImVec2(origin.x + area.x, y + kTrackH), line);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 6, y + 4));
        ImGui::PushID(row);
        if (renaming_ == track_id) {    // Çift tıklayınca ad yerinde değiştirilir
            ImGui::SetNextItemWidth(kHeaderW - 60);
            if (rename_focus_) { ImGui::SetKeyboardFocusHere(); rename_focus_ = false; }
            const bool done = ImGui::InputText("##ad", rename_buf_, sizeof(rename_buf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (done || ImGui::IsItemDeactivated()) {
                if (rename_buf_[0] && track.value("name", std::string()) != rename_buf_) { track["name"] = std::string(rename_buf_); mark_edit(); }
                renaming_.clear();
            }
        } else {
            const std::string label = track_label(track);
            ImGui::TextUnformatted(label.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                renaming_ = track_id;
                rename_focus_ = true;
                strncpy_s(rename_buf_, label.c_str(), _TRUNCATE);
            }
            ImGui::SetItemTooltip("%s", T("Adını değiştirmek için çift tıklayın"));
        }
        const bool audio_track = track.value("kind", std::string()) == "audio";
        float slot = y + 3;
        // Simgeli aç / kapa düğmeleri: açıkken vurgulu
        auto toggle = [&](const char* id, const char* on_icon, const char* off_icon, const char* key, bool show_on, const char* tip) {
            bool value = track.value(key, false);
            const bool lit = value == show_on;
            if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            ImGui::SetCursorScreenPos(ImVec2(origin.x + kHeaderW - 46, slot));
            if (ImGui::Button((std::string(value ? on_icon : off_icon) + "##" + id).c_str(), ImVec2(36, 22))) { track[key] = !value; mark_edit(); }
            if (lit) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", T(tip));
            slot += 25;     // Simgeler alt alta
        };
        if (audio_track) toggle("ses", "\xEE\x9D\x8F", "\xEE\x9D\xA7", "muted", true, "Sessiz: kanalı susturur");
        else toggle("goz", "\xEE\xB4\x9A", "\xEE\xA2\x90", "hidden", true, "Gizli: kanalı görüntüden çıkarır");
        toggle("kilit", "\xEE\x9C\xAE", "\xEE\x9E\x85", "locked", true, "Kilit: kanaldaki klipler taşınmaz ve kesilmez");
        toggle("miknatis", "\xEE\x9C\x9B", "\xEE\x9C\x9B", "magnet", true, "Mıknatıs: klipler arasında boşluk kalmaz, taşınan klip araya girer");
        const bool magnet = track.value("magnet", false);        ImGui::PopID();

        dl->PushClipRect(ImVec2(left, top), ImVec2(origin.x + area.x, top + rows), true);
        if (magnet) dl->AddRectFilled(ImVec2(left, y + 1), ImVec2(origin.x + area.x, y + kTrackH - 1), IM_COL32(13, 110, 253, 18));
        for (auto& c : project_["clips"]) {
            if (c.value("track", std::string()) != track_id) continue;
            const double start = c.value("start", 0.0), length = c.value("length", 0.0);
            const float x0 = to_x(start), x1 = to_x(start + length);
            const std::string id = c.value("id", std::string()), type = c.value("type", std::string());
            const bool sel = picked(id);
            const ImVec2 a(x0, y + 3), b(x1, y + kTrackH - 3);
            dl->AddRectFilled(a, b, clip_color(type, sel), 4);
            if (!c.value("group", std::string()).empty()) {     // Gruplu klip: üstte renkli şerit, rengi gruba göre
                const uint32_t h = uint32_t(std::hash<std::string>()(c.value("group", std::string())));
                dl->AddRectFilled(a, ImVec2(b.x, a.y + 4), IM_COL32(120 + (h & 127), 120 + ((h >> 8) & 127), 120 + ((h >> 16) & 127), 255), 4, ImDrawFlags_RoundCornersTop);
            }
            const int stream = c.value("audio_stream", 0);
            auto wave = waves_.find(widen(c.value("file", std::string())) + (stream ? L"#" + std::to_wstring(stream) : L""));
            if (clip_has_audio(c) && wave != waves_.end() && !wave->second.empty()) {
                const auto& peaks = wave->second;
                const float mid = (a.y + b.y) / 2 + (type == "audio" ? 0 : 12), amp = (b.y - a.y) * (type == "audio" ? 0.45f : 0.22f);
                const float first = std::max(x0, left), last = std::min(x1, origin.x + area.x);
                for (float px = first; px < last; px += 2) {
                    const double src = c.value("in", 0.0) + (double(px - x0) / zoom_) * c.value("speed", 1.0);
                    const size_t i = size_t(src * 100);
                    if (i >= peaks.size()) break;
                    const float h = peaks[i] * amp * std::min(2.f, c.value("volume", 100) / 100.f);
                    dl->AddLine(ImVec2(px, mid - h), ImVec2(px, mid + h), IM_COL32(255, 255, 255, 110));
                }
            }
            // Geçişler kenarlarda açık renkli şerit olarak görünür
            if (find_transition(c.value("t_in", std::string())))
                dl->AddRectFilledMultiColor(a, ImVec2(std::min(x1, x0 + float(c.value("t_in_d", 1.0) * zoom_)), b.y), IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90));
            if (find_transition(c.value("t_out", std::string())))
                dl->AddRectFilledMultiColor(ImVec2(std::max(x0, x1 - float(c.value("t_out_d", 1.0) * zoom_)), a.y), b, IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0));
            dl->PushClipRect(a, b, true);
            const std::string label = clip_is_visual_still(c) && !c.value("text", std::string()).empty() ? c.value("text", std::string()) : c.value("name", std::string());
            dl->AddText(ImVec2(std::max(x0, left) + 6, a.y + 3), IM_COL32_WHITE, label.c_str());
            dl->PopClipRect();
            if (sel) dl->AddRect(a, b, id == selected_ ? IM_COL32(255, 200, 40, 255) : IM_COL32(255, 230, 150, 200), 4.f, ImDrawFlags_None, 2.f);
        }
        dl->PopClipRect();
    }

    // Seçili klibin anahtar kareleri: elmasa tıkla zamana git, sürükle, sağ tıkla sil
    bool envelope_used = false;
    if (json* kc = selected(); kc && kc->contains("keys") && (*kc)["keys"].is_array() && !(*kc)["keys"].empty()) {
        int row = -1;
        for (int r = 0; r < int(tracks.size()); r++)
            if (tracks[r].value("id", std::string()) == kc->value("track", std::string())) row = r;
        if (row >= 0) {
            json& keys = (*kc)["keys"];
            const double start = kc->value("start", 0.0), length = kc->value("length", 0.0);
            const float ky = top + (row + 1) * kTrackH - 11;
            int hit_key = -1;
            dl->PushClipRect(ImVec2(left, top), ImVec2(origin.x + area.x, top + rows), true);
            for (int i = 0; i < int(keys.size()); i++) {
                const float kx = to_x(start + keys[i].value("t", 0.0));
                const ImVec2 pts[4] = {ImVec2(kx, ky - 6), ImVec2(kx + 6, ky), ImVec2(kx, ky + 6), ImVec2(kx - 6, ky)};
                dl->AddConvexPolyFilled(pts, 4, i == key_drag_ ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 200, 255, 255));
                dl->AddPolyline(pts, 4, IM_COL32(0, 0, 0, 200), ImDrawFlags_Closed, 1.f);
                if (fabs(io.MousePos.x - kx) < 7 && fabs(io.MousePos.y - ky) < 7) hit_key = i;
            }
            dl->PopClipRect();
            if (hovered && hit_key >= 0) {
                ImGui::SetTooltip("Anahtar kare  ·  tıklayın: zamana git, sürükleyin: taşıyın, sağ tık: silin");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { key_drag_ = hit_key; seek(start + keys[hit_key].value("t", 0.0)); envelope_used = true; }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { keys.erase(size_t(hit_key)); mark_edit(); envelope_used = true; }
            }
            if (key_drag_ >= 0 && key_drag_ < int(keys.size())) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (ImGui::IsMousePosValid() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2)) {
                        const double t = std::clamp(snap(mouse_t, "") - start, 0.0, length);
                        keys[key_drag_]["t"] = t;
                        seek(start + t);
                        mark_edit();
                    }
                    drag_ = Drag::None;
                    envelope_used = true;
                } else {
                    key_drag_ = -1;
                }
            }
        }
    }
    // Seçili klibin ses çizgisi: çift tıkla nokta ekle, sürükle, sağ tıkla sil
    if (json* sc = selected(); sc && clip_has_audio(*sc)) {
        int row = -1;
        for (int r = 0; r < int(tracks.size()); r++)
            if (tracks[r].value("id", std::string()) == sc->value("track", std::string())) row = r;
        if (row >= 0) {
            const double start = sc->value("start", 0.0), length = sc->value("length", 0.0), in = sc->value("in", 0.0), speed = std::max(0.01, sc->value("speed", 1.0));
            const float y0 = top + row * kTrackH + 6, y1 = top + (row + 1) * kTrackH - 6, x0 = to_x(start), x1 = to_x(start + length);
            auto x_of = [&](double s) { return to_x(start + (s - in) / speed); };
            auto s_of = [&](float x) { return in + (to_t(x) - start) * speed; };
            auto y_of = [&](double v) { return y1 - float(v / 200.0) * (y1 - y0); };
            auto v_of = [&](float y) { return std::clamp(double(y1 - y) / (y1 - y0) * 200.0, 0.0, 200.0); };
            if (!sc->contains("vol_points") || !(*sc)["vol_points"].is_array()) (*sc)["vol_points"] = json::array();
            json& pts = (*sc)["vol_points"];
            std::vector<std::pair<double, double>> list;
            for (auto& p : pts) list.push_back({p[0].get<double>(), p[1].get<double>()});
            std::sort(list.begin(), list.end());
            dl->PushClipRect(ImVec2(std::max(x0, left), y0 - 4), ImVec2(std::min(x1, origin.x + area.x), y1 + 4), true);
            const ImU32 env = IM_COL32(255, 210, 60, 255);
            if (list.empty()) {
                dl->AddLine(ImVec2(x0, y_of(100)), ImVec2(x1, y_of(100)), IM_COL32(255, 210, 60, 140), 1.5f);
            } else {
                dl->AddLine(ImVec2(x0, y_of(list.front().second)), ImVec2(x_of(list.front().first), y_of(list.front().second)), env, 2);
                for (size_t i = 0; i + 1 < list.size(); i++)
                    dl->AddLine(ImVec2(x_of(list[i].first), y_of(list[i].second)), ImVec2(x_of(list[i + 1].first), y_of(list[i + 1].second)), env, 2);
                dl->AddLine(ImVec2(x_of(list.back().first), y_of(list.back().second)), ImVec2(x1, y_of(list.back().second)), env, 2);
            }
            int hit_point = -1;
            for (int i = 0; i < int(pts.size()); i++) {
                const ImVec2 c(x_of(pts[i][0].get<double>()), y_of(pts[i][1].get<double>()));
                dl->AddCircleFilled(c, 5, env);
                dl->AddCircle(c, 5, IM_COL32(0, 0, 0, 200));
                if (fabs(io.MousePos.x - c.x) < 7 && fabs(io.MousePos.y - c.y) < 7) hit_point = i;
            }
            dl->PopClipRect();
            const bool inside = io.MousePos.x >= x0 && io.MousePos.x <= x1 && io.MousePos.y >= y0 - 4 && io.MousePos.y <= y1 + 4;
            if (hovered && hit_point >= 0) {
                ImGui::SetTooltip("Ses %%%.0f  ·  sürükleyin, sağ tıkla silin", pts[hit_point][1].get<double>());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { env_drag_ = hit_point; envelope_used = true; }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { pts.erase(hit_point); mark_edit(); envelope_used = true; }
            } else if (hovered && inside && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (pts.empty()) {      // İlk nokta: sesin şimdiki seviyesini koruyan başlangıç da eklenir
                    pts.push_back(json::array({in, 100.0}));
                    pts.push_back(json::array({in + length * speed, 100.0}));
                }
                pts.push_back(json::array({s_of(io.MousePos.x), v_of(io.MousePos.y)}));
                env_drag_ = int(pts.size()) - 1;
                drag_ = Drag::None;
                envelope_used = true;
                mark_edit();
            }
            if (env_drag_ >= 0 && env_drag_ < int(pts.size())) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (!ImGui::IsMousePosValid()) { envelope_used = true; }      // Fare pencereden çıktıysa nokta yerinde kalır
                    else {
                    pts[env_drag_][0] = std::clamp(s_of(io.MousePos.x), in, in + length * speed);
                    pts[env_drag_][1] = v_of(io.MousePos.y);
                    drag_ = Drag::None;
                    envelope_used = true;
                    mark_edit();
                    }
                } else {
                    env_drag_ = -1;
                }
            }
        }
    }
    if (envelope_used) hit_id.clear();

    // Fare: cetvel zamanı taşır, klip seçilir ve taşınır, boş alanda seçim kutusu
    if (drag_ == Drag::None && (hit_kind == Drag::TrimLeft || hit_kind == Drag::TrimRight)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (drag_ == Drag::TrimLeft || drag_ == Drag::TrimRight) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (!envelope_used && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && io.MousePos.y >= top && !hit_id.empty()) {
        if (!picked(hit_id)) select_only(hit_id);
        else selected_ = hit_id;
        ImGui::OpenPopup("klip_menu");
    }
    const float rin = to_x(range_in_), rout = to_x(range_out_);
    const bool near_in = has_range() && fabs(io.MousePos.x - rin) < 6, near_out = has_range() && fabs(io.MousePos.x - rout) < 6;
    if (hovered && io.MousePos.y < top && (near_in || near_out)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (!envelope_used && env_drag_ < 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (io.MousePos.y < top && (near_in || near_out)) {
            range_drag_ = near_in ? 1 : 2;
        } else if (io.MousePos.y < top && io.KeyShift) {
            range_drag_ = 3;
            range_in_ = range_out_ = snap(mouse_t, "");
            drag_offset_ = range_in_;
        } else if (io.MousePos.y < top) {
            drag_ = Drag::Seek;
        } else if (!hit_id.empty()) {
            if (io.KeyCtrl || io.KeyShift) {
                if (picked(hit_id)) { picks_.erase(hit_id); if (selected_ == hit_id) selected_ = picks_.empty() ? std::string() : *picks_.begin(); }
                else { picks_.insert(hit_id); selected_ = hit_id; add_group(hit_id); }
            } else {
                if (!picked(hit_id)) select_only(hit_id);
                selected_ = hit_id;
                drag_ = hit_kind;
                drag_id_ = hit_id;
                drag_origin_.clear();
                for (auto& id : picks_)
                    if (json* c = clip_by_id(id))
                        drag_origin_[id] = {c->value("start", 0.0), c->value("length", 0.0), c->value("in", 0.0), c->value("track", std::string())};
                const Origin& o = drag_origin_[drag_id_];
                drag_start_ = o.start; drag_length_ = o.length; drag_in_ = o.in; drag_track_ = o.track;
                drag_offset_ = mouse_t - drag_start_;
            }
        } else {
            if (!io.KeyCtrl && !io.KeyShift) { picks_.clear(); selected_.clear(); }
            drag_ = Drag::Box;
            box_from_ = io.MousePos;
        }
    }
    if (range_drag_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double t = snap(mouse_t, "");
        if (range_drag_ == 1) range_in_ = std::min(t, range_out_ - 0.02);
        if (range_drag_ == 2) range_out_ = std::max(t, range_in_ + 0.02);
        if (range_drag_ == 3) { range_in_ = std::min(drag_offset_, t); range_out_ = std::max(drag_offset_, t); }
    } else {
        range_drag_ = 0;
    }
    if (drag_ == Drag::Seek) {
        seek(mouse_t);
    } else if (drag_ == Drag::Box) {
        const ImVec2 a(std::min(box_from_.x, io.MousePos.x), std::min(box_from_.y, io.MousePos.y));
        const ImVec2 b(std::max(box_from_.x, io.MousePos.x), std::max(box_from_.y, io.MousePos.y));
        dl->AddRectFilled(a, b, IM_COL32(13, 110, 253, 40));
        dl->AddRect(a, b, IM_COL32(13, 110, 253, 200));
        for (int row = 0; row < int(tracks.size()); row++) {
            const float y0 = top + row * kTrackH, y1 = y0 + kTrackH;
            if (y1 < a.y || y0 > b.y) continue;
            for (auto& c : project_["clips"]) {
                if (c.value("track", std::string()) != tracks[row].value("id", std::string())) continue;
                const float x0 = to_x(c.value("start", 0.0)), x1 = to_x(c.value("start", 0.0) + c.value("length", 0.0));
                if (x1 < a.x || x0 > b.x) continue;
                const std::string id = c.value("id", std::string());
                if (!picked(id)) { picks_.insert(id); add_group(id); if (selected_.empty()) selected_ = id; }
            }
        }
    } else if (drag_ != Drag::None) {
        if (json* c = clip_by_id(drag_id_)) {
            const double speed = c->value("speed", 1.0), source = c->value("source_length", 0.0);
            const bool bounded = source > 0;
            if (drag_ == Drag::Move) {
                double start = std::max(0.0, mouse_t - drag_offset_);
                const double end = snap(start + drag_length_, drag_id_) - drag_length_, begin = snap(start, drag_id_);
                start = begin != start ? begin : end;
                double delta = start - drag_start_;
                for (auto& [id, o] : drag_origin_) delta = std::max(delta, -o.start);     // Hiçbiri sıfırın soluna geçmesin
                for (auto& [id, o] : drag_origin_)
                    if (json* other = clip_by_id(id)) (*other)["start"] = o.start + delta;
                if (drag_origin_.size() == 1 && mouse_row >= 0 && mouse_row < int(tracks.size()) &&
                    tracks[mouse_row].value("kind", std::string()) == (c->value("type", std::string()) == "audio" ? "audio" : "video") && !tracks[mouse_row].value("locked", false))
                    (*c)["track"] = tracks[mouse_row].value("id", std::string());
            } else if (drag_ == Drag::TrimLeft) {
                // Ters klipte sol kenar kaynağın sonudur: in değişmez
                const bool reverse = clip_reversed(*c);
                double start = snap(mouse_t, drag_id_);
                const double room = reverse ? source - drag_in_ - drag_length_ * speed : drag_in_;
                const double low = bounded ? drag_start_ - room / speed : 0.0;
                start = std::clamp(start, std::max(0.0, low), drag_start_ + drag_length_ - 0.05);
                (*c)["start"] = start;
                (*c)["length"] = drag_start_ + drag_length_ - start;
                if (bounded && !reverse) (*c)["in"] = drag_in_ + (start - drag_start_) * speed;
            } else {
                // Ters klipte sağ kenar kaynağın başıdır: kısalınca in ilerler
                const bool reverse = clip_reversed(*c);
                double end = snap(mouse_t, drag_id_);
                const double high = !bounded ? 1e9 : reverse ? drag_start_ + drag_length_ + drag_in_ / speed : drag_start_ + (source - drag_in_) / speed;
                end = std::clamp(end, drag_start_ + 0.05, high);
                (*c)["length"] = end - drag_start_;
                if (reverse) (*c)["in"] = std::max(0.0, drag_in_ + (drag_start_ + drag_length_ - end) * speed);
            }
            mark_edit();
        }
    }
    if (drag_ != Drag::None && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (drag_ == Drag::Move || drag_ == Drag::TrimLeft || drag_ == Drag::TrimRight) {
            bool blocked = false;
            for (auto& [id, o] : drag_origin_)
                if (json* c = clip_by_id(id)) {
                    const json* track = find_track(project_, c->value("track", std::string()));
                    if (track && track->value("magnet", false)) continue;   // Mıknatıslı kanalda araya girer
                    blocked |= overlaps(*c, c->value("track", std::string()), c->value("start", 0.0), c->value("length", 0.0));
                }
            if (blocked) {      // Bırakılan yer doluysa klipler eski yerine döner
                for (auto& [id, o] : drag_origin_)
                    if (json* c = clip_by_id(id)) { (*c)["start"] = o.start; (*c)["length"] = o.length; (*c)["in"] = o.in; (*c)["track"] = o.track; }
                message_ = "Bu aralık dolu, klip eski yerine döndü";
                message_error_ = false;
            }
        }
        drag_ = Drag::None;
    }
    clip_menu();

    // İşaretler, aralık ve işaretçi
    dl->PushClipRect(ImVec2(left, origin.y), ImVec2(origin.x + area.x, origin.y + area.y), true);
    if (range_in_ >= 0 && range_out_ > range_in_) {
        const float a = to_x(range_in_), b = to_x(range_out_);
        dl->AddRectFilled(ImVec2(a, origin.y), ImVec2(b, top), IM_COL32(40, 167, 69, 150));
        dl->AddRectFilled(ImVec2(a, top), ImVec2(b, origin.y + area.y), IM_COL32(40, 167, 69, 35));
        for (float x : {a, b}) {
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + area.y), IM_COL32(40, 200, 90, 230), 2);
            dl->AddRectFilled(ImVec2(x - 4, origin.y), ImVec2(x + 4, top), IM_COL32(40, 200, 90, 255), 2);
        }
    }
    for (auto& m : project_["markers"]) {      // İşaretler bölüm başlarıdır; adı varsa yanında yazar
        const float x = to_x(marker_time(m));
        dl->AddTriangleFilled(ImVec2(x - 5, origin.y), ImVec2(x + 5, origin.y), ImVec2(x, origin.y + 9), IM_COL32(120, 220, 120, 255));
        const std::string name = marker_name(m);
        if (!name.empty()) dl->AddText(ImVec2(x + 6, origin.y + 6), IM_COL32(120, 220, 120, 255), name.c_str());
        if (fabsf(io.MousePos.x - x) < 6 && io.MousePos.y >= origin.y && io.MousePos.y < top && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) show_chapters_ = true;
    }
    const float px = to_x(time_);
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + area.y), IM_COL32(235, 70, 85, 255), 2);
    dl->AddTriangleFilled(ImVec2(px - 6, origin.y), ImVec2(px + 6, origin.y), ImVec2(px, origin.y + 11), IM_COL32(235, 70, 85, 255));
    dl->PopClipRect();
    ImGui::EndChild();
    ImGui::EndChild();
}

void App::status_bar() {
    ImGui::AlignTextToFramePadding();
    if (message_error_) ImGui::TextColored(theme_error(), "%s", T(message_.c_str()));
    else ImGui::TextDisabled("%s", T(message_.c_str()));
    char text[128];
    const auto& c = project_["canvas"];
    sprintf_s(text, "%s%s%d x %d  ·  %d FPS", unsaved_ ? T("Kaydedilmedi") : "", unsaved_ ? "  ·  " : "", c.value("w", 1920), c.value("h", 1080), c.value("fps", 60));
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x - 16);
    ImGui::TextDisabled("%s", text);
}

void App::popup_canvas() {
    if (show_canvas_) { ImGui::OpenPopup("Proje ayarları"); show_canvas_ = false; }
    if (!ImGui::BeginPopupModal("Proje ayarları", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    json& c = project_["canvas"];
    const std::string current = std::to_string(c.value("w", 1920)) + "x" + std::to_string(c.value("h", 1080));
    json holder = {{"size", current}, {"fps", std::to_string(c.value("fps", 60))}};
    ImGui::PushItemWidth(260);
    if (edit_choice("Tuval", holder, "size", {{"1920x1080", "1920 x 1080 (yatay)"}, {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160 (4K)"}, {"1280x720", "1280 x 720"}, {"1080x1920", "1080 x 1920 (dikey, kısa video)"}, {"1080x1080", "1080 x 1080 (kare)"}})) {
        int w = 1920, h = 1080;
        sscanf_s(holder.value("size", std::string()).c_str(), "%dx%d", &w, &h);
        c["w"] = w; c["h"] = h;
        mark_edit();
    }
    if (edit_choice("FPS", holder, "fps", {{"24", "24"}, {"25", "25"}, {"30", "30"}, {"50", "50"}, {"60", "60"}})) {
        c["fps"] = atoi(holder.value("fps", std::string("60")).c_str());
        mark_edit();
    }
    ImGui::PopItemWidth();
    if (ImGui::Button("Tamam", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

EncodeConfig make_export_config(const json& canvas, const json& x, const std::wstring& path);

static std::wstring pick_folder(HWND owner) {
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

void App::popup_chapters() {
    if (show_chapters_) { ImGui::OpenPopup("###bolumler"); show_chapters_ = false; }
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal((std::string(T("Bölümler")) + "###bolumler").c_str(), nullptr, 0)) return;
    if (!project_["markers"].is_array()) project_["markers"] = json::array();
    json& markers = project_["markers"];
    for (auto& m : markers)
        if (!m.is_object()) m = json{{"t", marker_time(m)}, {"name", ""}};     // Eski sayı biçimi
    ImGui::TextWrapped("%s", T("Bölümler zaman çizelgesindeki işaretlerdir (M tuşu). Ad verin: dışa aktarılan videoya bölüm bilgisi olarak eklenir, YouTube açıklaması için de kopyalanabilir."));
    bool e = false;
    if (ImGui::Button(T("İşaretçiye bölüm ekle"), ImVec2(220, 30))) { markers.push_back({{"t", time_}, {"name", ""}}); e = true; }
    ImGui::Separator();
    std::vector<size_t> order(markers.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return marker_time(markers[a]) < marker_time(markers[b]); });
    const int fps = project_["canvas"].value("fps", 30);
    int remove = -1;
    ImGui::BeginChild("bolum_liste", ImVec2(0, std::min(320.f, 34.f * order.size() + 8)));
    for (size_t n = 0; n < order.size(); n++) {
        json& m = markers[order[n]];
        ImGui::PushID(int(order[n]));
        if (ImGui::Selectable(format_time(marker_time(m), fps).c_str(), false, 0, ImVec2(110, 0))) seek(marker_time(m));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-40);
        char buf[128];
        strncpy_s(buf, m.value("name", std::string()).c_str(), _TRUNCATE);
        if (ImGui::InputTextWithHint("##ad", n == 0 ? T("Giriş") : TF("Bölüm %d", int(n + 1)).c_str(), buf, sizeof(buf))) { m["name"] = std::string(buf); e = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("\xC3\x97")) remove = int(order[n]);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (remove >= 0) { markers.erase(size_t(remove)); e = true; }
    if (markers.empty()) ImGui::TextDisabled("%s", T("Henüz bölüm yok."));
    bool embed = project_.value("chapters_embed", true);
    if (ImGui::Checkbox(T("Dışa aktarılan videoya bölüm bilgisi ekle (MP4, MKV, MOV)"), &embed)) { project_["chapters_embed"] = embed; e = true; }
    ImGui::BeginDisabled(markers.empty());
    if (ImGui::Button(T("YouTube açıklaması için kopyala"), ImVec2(260, 30))) {
        ImGui::SetClipboardText(youtube_chapters(project_).c_str());
        message_ = T("Bölüm listesi panoya kopyalandı");
        message_error_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", T("YouTube ilk bölümün 0:00'da başlamasını ister; gerekirse liste başına kendiliğinden eklenir."));
    ImGui::SameLine();
    if (ImGui::Button(T("Kapat"), ImVec2(120, 30))) ImGui::CloseCurrentPopup();
    if (e) mark_edit();
    ImGui::EndPopup();
}

// Şablon: projenin olduğu gibi saklanan kopyası; yeni proje ondan başlar
std::wstring templates_dir() { return data_dir() + L"\\Templates"; }

void App::popup_template() {
    static char name[96] = "";
    if (show_template_) { ImGui::OpenPopup("###sablon"); show_template_ = false; name[0] = 0; }
    if (!ImGui::BeginPopupModal((std::string(T("Şablon olarak kaydet")) + "###sablon").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("%s", T("Kanallar, klipler, yazılar ve ayarlar şablona girer. Dosya > Şablondan yeni proje ile aynı düzenle başlarsınız."));
    ImGui::SetNextItemWidth(360);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##sablon_ad", T("Şablon adı"), name, sizeof(name));
    std::wstring file = widen(name);
    for (auto& ch : file) if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
    ImGui::BeginDisabled(file.empty());
    if (ImGui::Button(T("Kaydet"), ImVec2(120, 30))) {
        SHCreateDirectoryExW(nullptr, templates_dir().c_str(), nullptr);
        json t = project_;
        t["template_name"] = std::string(name);
        write_json(templates_dir() + L"\\" + file + L".vproje", t);
        message_ = TF("Şablon kaydedildi: %s", name);
        message_error_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Vazgeç"), ImVec2(120, 30))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Toplu dışa aktarma: seçilen projeler sırayla, dışa aktarma penceresindeki video ayarlarıyla aynı klasöre
void App::popup_batch() {
    if (show_batch_) { ImGui::OpenPopup("###toplu"); show_batch_ = false; }
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal((std::string(T("Toplu dışa aktar")) + "###toplu").c_str(), nullptr, 0)) return;
    // Sıradaki iş
    if (batch_running_) {
        if (exporting_ && export_job_.done) {
            export_job_.thread.join();
            exporting_ = false;
            batch_results_.push_back(export_job_.error.empty() ? std::string("\xE2\x9C\x93 ") + narrow(fs::path(export_job_.path).filename().wstring())
                                                               : std::string("\xC3\x97 ") + narrow(fs::path(export_job_.path).filename().wstring()) + ": " + export_job_.error);
            if (export_job_.cancel) batch_running_ = false;
        }
        if (!exporting_ && batch_running_) {
            if (batch_next_ >= batch_files_.size()) {
                batch_running_ = false;
                message_ = TF("Toplu dışa aktarma bitti: %d proje", int(batch_files_.size()));
                message_error_ = false;
            } else {
                const std::wstring file = batch_files_[batch_next_++];
                const json p = read_json(file);
                if (!p.is_object() || !p.contains("clips")) {
                    batch_results_.push_back(std::string("\xC3\x97 ") + narrow(fs::path(file).filename().wstring()) + ": " + T("Proje okunamadı"));
                } else {
                    const std::wstring out = batch_folder_ + L"\\" + fs::path(file).stem().wstring() + L"." + widen(export_.value("container", std::string("mp4")));
                    EncodeConfig c = make_export_config(p["canvas"], export_, out);
                    exporting_ = true;
                    export_started_ = now();
                    export_start(export_job_, p, c);
                }
            }
        }
    }
    ImGui::TextWrapped("%s", T("Projeler sırayla, Dışa aktar penceresindeki video ayarlarıyla (kalite, boyut, kodlayıcı) seçtiğiniz klasöre aktarılır."));
    ImGui::BeginDisabled(batch_running_);
    if (ImGui::Button(T("Proje ekle..."), ImVec2(160, 30))) {
        std::vector<std::wstring> files;
        pick_files(hwnd, files, TW("Video Düzenleyici projesi").c_str(), L"*.vproje", true);
        for (auto& f : files) batch_files_.push_back(f);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Klasör seç..."), ImVec2(160, 30))) {
        const std::wstring folder = pick_folder(hwnd);
        if (!folder.empty()) batch_folder_ = folder;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Listeyi temizle"), ImVec2(160, 30))) { batch_files_.clear(); batch_results_.clear(); batch_next_ = 0; }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s %s", T("Klasör:"), batch_folder_.empty() ? T("(seçilmedi)") : narrow(batch_folder_).c_str());
    ImGui::BeginChild("toplu_liste", ImVec2(0, 200), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < batch_files_.size(); i++) {
        const std::string name = narrow(fs::path(batch_files_[i]).filename().wstring());
        if (i < batch_results_.size()) ImGui::TextUnformatted(batch_results_[i].c_str());
        else if (batch_running_ && i + 1 == batch_next_) ImGui::TextColored(theme_accent(), "%s  %.0f%%", name.c_str(), export_job_.progress * 100);
        else ImGui::TextDisabled("%s", name.c_str());
    }
    ImGui::EndChild();
    if (!batch_running_) {
        ImGui::BeginDisabled(batch_files_.empty() || batch_folder_.empty() || exporting_);
        if (ImGui::Button(T("Başlat"), ImVec2(140, 32))) { batch_running_ = true; batch_next_ = 0; batch_results_.clear(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("Kapat"), ImVec2(140, 32))) ImGui::CloseCurrentPopup();
    } else if (ImGui::Button(T("Durdur"), ImVec2(140, 32))) {
        export_job_.cancel = true;
    }
    ImGui::EndPopup();
}

// Kalite yüzdesi kodlayıcının sabit kalite değerine çevrilir (düşük değer daha iyi)
int quality_value(const std::string& encoder, int percent) {
    percent = std::clamp(percent, 1, 100);
    if (encoder == "aom" || encoder == "vp9") return std::clamp(int(52 - percent * 0.36 + 0.5), 4, 63);
    const int base = int(30 - percent * 0.18 + 0.5);
    if (encoder == "x264") return std::clamp(base, 0, 51);
    return std::clamp(base + 2, 1, 51);
}

// Dışa aktarma penceresindeki seçimlerden kodlayıcı ayarı (toplu dışa aktarma da kullanır)
EncodeConfig make_export_config(const json& canvas, const json& x, const std::wstring& path) {
    EncodeConfig c;
    int w = canvas.value("w", 1920), h = canvas.value("h", 1080);
    const std::string size = x.value("size", std::string("canvas"));
    if (size != "canvas") {
        int bw = w, bh = h;
        sscanf_s(size.c_str(), "%dx%d", &bw, &bh);
        if (w < h) std::swap(bw, bh);   // Dikey projede seçilen boyut da dikey okunur; oran hep korunur
        const double k = std::min(double(bw) / w, double(bh) / h);
        w = int(w * k); h = int(h * k);
    }
    c.encoder = x.value("encoder", std::string("x264"));
    c.rate = "cq";
    c.quality = quality_value(c.encoder, x.value("quality_pct", 90));
    c.preset = x.value("speed", std::string());
    c.bframes = c.encoder.rfind("qsv", 0) == 0 ? 0 : 3;
    c.keyint = 0;
    c.custom = c.encoder == "x264" ? "aq-mode=3" : "";
    c.width = std::max(2, w & ~1);
    c.height = std::max(2, h & ~1);
    const std::string fps = x.value("fps", std::string("canvas"));
    c.fps = fps == "canvas" ? canvas.value("fps", 60) : atoi(fps.c_str());
    c.audio = true;
    c.audio_kbps = x.value("audio", 320);
    c.container = x.value("container", std::string("mp4"));
    c.path = path;
    return c;
}

void App::popup_export() {
    if (show_export_) {
        ImGui::OpenPopup("Dışa aktar");
        show_export_ = false;
        export_finished_ = false;
        set_playing(false);
    }
    ImGui::SetNextWindowSize(ImVec2(620, 0));
    if (!ImGui::BeginPopupModal("Dışa aktar", nullptr, ImGuiWindowFlags_NoResize)) return;
    if (exporting_ && export_job_.done) {
        export_job_.thread.join();
        exporting_ = false;
        export_finished_ = !export_job_.cancel;
        if (export_job_.cancel) { message_ = "Dışa aktarma iptal edildi"; message_error_ = false; ImGui::CloseCurrentPopup(); }
        else if (export_job_.error.empty()) { message_ = TF("Kaydedildi: %s", narrow(export_job_.path).c_str()); message_error_ = false; }
        else { message_ = TF("Dışa aktarılamadı: %s", export_job_.error.c_str()); message_error_ = true; }
    }
    if (exporting_) {
        ImGui::PushFont(nullptr, 20.f);
        ImGui::TextUnformatted("Videonuz hazırlanıyor");
        ImGui::PopFont();
        ImGui::TextDisabled("%s", narrow(fs::path(export_job_.path).filename().wstring()).c_str());
        const float progress = export_job_.progress;
        char text[64];
        sprintf_s(text, "%%%.0f", progress * 100);
        ImGui::ProgressBar(progress, ImVec2(-1, 26), text);
        const double spent = now() - export_started_;
        if (progress > 0.02f) ImGui::Text("Geçen süre %s  ·  Kalan yaklaşık %s", format_time(spent, 1).substr(0, 8).c_str(), format_time(spent / progress - spent, 1).substr(0, 8).c_str());
        else ImGui::TextDisabled("Başlıyor...");
        if (ImGui::Button("Vazgeç", ImVec2(130, 32))) export_job_.cancel = true;
        ImGui::EndPopup();
        return;
    }
    if (export_finished_) {
        ImGui::PushFont(nullptr, 20.f);
        ImGui::TextUnformatted(export_job_.error.empty() ? "Video hazır" : "Video oluşturulamadı");
        ImGui::PopFont();
        if (export_job_.error.empty()) {
            ImGui::TextWrapped("%s", narrow(export_job_.path).c_str());
            if (ImGui::Button("Videoyu aç", ImVec2(150, 32))) ShellExecuteW(nullptr, L"open", export_job_.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button("Klasörde göster", ImVec2(150, 32))) {
                const std::wstring args = L"/select,\"" + export_job_.path + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
        } else {
            ImGui::TextColored(theme_error(), "%s", export_job_.error.c_str());
        }
        if (ImGui::Button("Kapat", ImVec2(130, 32))) { export_finished_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        return;
    }
    json& x = export_;
    const auto& canvas = project_["canvas"];
    ImGui::PushFont(nullptr, 20.f);
    ImGui::TextUnformatted(T("Dışa aktar"));
    ImGui::PopFont();
    ImGui::TextDisabled("%s", TF("Proje: %d x %d, %d FPS, %s", canvas.value("w", 1920), canvas.value("h", 1080), canvas.value("fps", 60), format_time(project_length(project_), 1).substr(0, 8).c_str()).c_str());
    ImGui::Spacing();
    ImGui::PushItemWidth(-200);
    edit_choice(T("Tür"), x, "kind", {{"video", T("Video")}, {"gif", T("Hareketli GIF")}, {"audio", T("Yalnız ses (MP3, WAV, M4A)")}, {"frame", T("Tek kare (işaretçideki görüntü, PNG)")}});
    const std::string kind = x.value("kind", std::string("video"));
    if (kind != "video") {
        if (kind == "gif") {
            edit_choice(T("Genişlik"), x, "gif_width", {{"480", "480 px"}, {"640", "640 px"}, {"800", "800 px"}, {"1080", "1080 px"}});
            edit_choice(T("Kare hızı"), x, "gif_fps", {{"10", "10"}, {"15", "15"}, {"20", "20"}, {"25", "25"}});
            ImGui::TextDisabled("%s", T("GIF'te ses olmaz; kısa ve küçük tutmak için aralık seçip yalnızca onu aktarabilirsiniz."));
        }
        if (kind == "audio") {
            edit_choice(T("Dosya biçimi"), x, "audio_format", {{"mp3", "MP3"}, {"m4a", "M4A (AAC)"}, {"wav", T("WAV (sıkıştırmasız)")}});
            if (x.value("audio_format", std::string("mp3")) != "wav")
                edit_choice(T("Ses kalitesi"), x, "audio_text", {{"128", "128 kbps"}, {"192", "192 kbps"}, {"256", "256 kbps"}, {"320", "320 kbps"}});
        }
        if (kind == "frame") ImGui::TextWrapped("%s", TF("İşaretçideki kare (%s) proje boyutunda PNG olarak kaydedilir.", format_time(time_, canvas.value("fps", 60)).c_str()).c_str());
        ImGui::PopItemWidth();
        static bool loud = false;
        if (kind == "audio") ImGui::Checkbox(T("Ses yüksekliğini YouTube standardına getir (-14 LUFS)"), &loud);
        if (kind != "frame" && has_range()) ImGui::Checkbox(T("Yalnızca seçili aralığı dışa aktar"), &export_range_);
        ImGui::Spacing();
        if (ImGui::Button(T("Dışa aktar"), ImVec2(160, 36))) {
            const std::string ext = kind == "gif" ? "gif" : kind == "frame" ? "png" : x.value("audio_format", std::string("mp3"));
            const std::wstring wext = widen(ext), filter = L"*." + wext;
            const std::wstring suggested = project_path_.empty() ? L"Video" : fs::path(project_path_).stem().wstring();
            const std::wstring path = pick_save(hwnd, kind == "audio" ? TW("Ses").c_str() : TW("Görüntü").c_str(), filter.c_str(), wext.c_str(), suggested);
            if (!path.empty()) {
                EncodeConfig c = make_export_config(canvas, x, path);
                c.container = ext;
                c.audio_kbps = atoi(x.value("audio_text", std::string("320")).c_str());
                json what = project_;
                if (kind == "frame") project_keep_range(what, time_, time_ + 1.0 / std::max(1, canvas.value("fps", 60)));
                else if (export_range_ && has_range()) project_keep_range(what, range_in_, range_out_);
                what["export_mode"] = kind;
                what["gif_width"] = atoi(x.value("gif_width", std::string("640")).c_str());
                what["gif_fps"] = atoi(x.value("gif_fps", std::string("15")).c_str());
                what["loudnorm"] = kind == "audio" && loud;
                exporting_ = true;
                export_started_ = now();
                export_start(export_job_, what, c);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Vazgeç"), ImVec2(130, 36))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (edit_choice("Hazır ayar", x, "preset_name", {{"best", "En yüksek kalite"}, {"high", "Yüksek kalite"}, {"standard", "Standart"}, {"small", "Küçük dosya (720p)"}, {"custom", "Özel"}})) {
        const std::string n = x.value("preset_name", std::string());
        x["encoder"] = "x264";
        x["container"] = "mp4";
        if (n == "best") { x["size"] = "canvas"; x["quality_pct"] = 100; x["speed"] = "slow"; x["audio"] = 320; }
        if (n == "high") { x["size"] = "canvas"; x["quality_pct"] = 90; x["speed"] = "medium"; x["audio"] = 320; }
        if (n == "standard") { x["size"] = "canvas"; x["quality_pct"] = 75; x["speed"] = "veryfast"; x["audio"] = 192; }
        if (n == "small") { x["size"] = "1280x720"; x["quality_pct"] = 55; x["speed"] = "veryfast"; x["audio"] = 128; }
    }
    ImGui::SeparatorText("Video");
    const std::string project_size = "Proje boyutu (" + std::to_string(canvas.value("w", 1920)) + " x " + std::to_string(canvas.value("h", 1080)) + ")";
    if (edit_choice("Boyut", x, "size", {{"canvas", project_size}, {"3840x2160", "3840 x 2160"}, {"2560x1440", "2560 x 1440"}, {"1920x1080", "1920 x 1080"}, {"1280x720", "1280 x 720"}, {"854x480", "854 x 480"}}))
        x["preset_name"] = "custom";
    if (edit_choice("Kare hızı", x, "fps", {{"canvas", "Projeyle aynı"}, {"24", "24"}, {"25", "25"}, {"30", "30"}, {"50", "50"}, {"60", "60"}})) x["preset_name"] = "custom";
    if (edit_int("Kalite (%)", x, "quality_pct", 1, 100)) x["preset_name"] = "custom";
    const std::string key = x.value("encoder", std::string("x264"));
    const EncoderInfo* current = find_encoder(key);
    if (ImGui::BeginCombo("Kodlayıcı", current ? current->name : "")) {
        for (auto& e : encoder_list()) {
            if (!e.ffmpeg[0]) continue;
            const int state = encoder_state(e.key);
            std::string label = e.name;
            if (state == 0) label += "  (bu bilgisayarda yok)";
            if (state < 0) label += "  (deneniyor...)";
            ImGui::BeginDisabled(state != 1);
            if (ImGui::Selectable(label.c_str(), key == e.key)) {
                x["encoder"] = e.key;
                x["speed"] = encoder_defaults(e.key, false).value("preset", std::string());
                if (e.key == std::string("vp9")) x["container"] = "mkv";
                x["preset_name"] = "custom";
            }
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    const Choices presets = encoder_presets(x.value("encoder", std::string("x264")));
    if (!presets.empty() && edit_choice("Kodlama hızı", x, "speed", presets)) x["preset_name"] = "custom";
    if (edit_choice("Dosya biçimi", x, "container", record_containers(x.value("encoder", std::string("x264"))))) x["preset_name"] = "custom";
    ImGui::SeparatorText("Ses");
    if (edit_choice("Ses kalitesi", x, "audio_text", {{"128", "128 kbps"}, {"192", "192 kbps"}, {"256", "256 kbps"}, {"320", "320 kbps"}})) {
        x["audio"] = atoi(x.value("audio_text", std::string("320")).c_str());
        x["preset_name"] = "custom";
    }
    ImGui::PopItemWidth();
    static bool loudness = false;
    ImGui::Checkbox("Ses yüksekliğini YouTube standardına getir (-14 LUFS)", &loudness);
    ImGui::SetItemTooltip("Videonun sesi çok kısık ya da çok yüksekse platformlardaki seviyeye ayarlar.");
    export_["loudnorm"] = loudness;
    if (has_range()) ImGui::Checkbox("Yalnızca seçili aralığı dışa aktar", &export_range_);
    else export_range_ = false;
    static bool srt = false;
    const bool has_captions = project_.contains("captions") && project_["captions"].is_array() && !project_["captions"].empty();
    if (has_captions) ImGui::Checkbox("Altyazıyı ayrıca .srt dosyası olarak kaydet", &srt);
    const int value = quality_value(x.value("encoder", std::string("x264")), x.value("quality_pct", 90));
    ImGui::TextDisabled("Kalite %%%d: sabit kalite değeri %d. Yavaş kodlama aynı kalitede daha küçük dosya verir.", x.value("quality_pct", 90), value);
    ImGui::Spacing();
    if (ImGui::Button("Dışa aktar", ImVec2(160, 36))) {
        const std::string container = x.value("container", std::string("mp4"));
        const std::wstring ext = widen(container), filter = L"*." + ext;
        const std::wstring suggested = project_path_.empty() ? L"Video" : fs::path(project_path_).stem().wstring();
        const std::wstring path = pick_save(hwnd, L"Video", filter.c_str(), ext.c_str(), suggested);
        if (!path.empty()) {
            EncodeConfig c = make_export_config(canvas, x, path);
            exporting_ = true;
            export_started_ = now();
            json what = project_;
            if (export_range_ && has_range()) project_keep_range(what, range_in_, range_out_);
            what["loudnorm"] = export_.value("loudnorm", false);
            if (srt && has_captions && what.contains("captions")) {     // Videoyla aynı adla, yanına
                std::ofstream o(fs::path(path).replace_extension(L".srt"), std::ios::binary);
                o << write_srt(what["captions"]);
            }
            export_start(export_job_, what, c);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(130, 36))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Çarpı programı kapatmaz: proje açık kalır, simge saatin yanındaki gizli simgelerde durur
void App::hide_to_tray() {
    if (playing_) set_playing(false);
    ShowWindow(hwnd, SW_HIDE);
    json data = read_json(data_dir() + L"\\settings.json");
    if (!data.is_object()) data = json::object();
    if (data.value("editor_tray_note_shown", false)) return;
    data["editor_tray_note_shown"] = true;
    write_json(data_dir() + L"\\settings.json", data);
    tray_note(hwnd, "Video Düzenleyici çalışmaya devam ediyor", "Projeniz açık kaldı. Pencereyi açmak ya da programı kapatmak için saatin yanındaki simgeyi kullanın.");
}

void App::on_tray(UINT event) {
    if (event == WM_LBUTTONUP) { show_window(); return; }
    if (event != WM_RBUTTONUP) return;
    enum { kOpen = 1, kRecorder, kExit };
    const int choice = tray_menu(hwnd, {{kOpen, "Aç"}, {kRecorder, "Kayıt Stüdyosu'nu aç"}, {0, ""}, {kExit, "Çıkış"}});
    if (choice == kOpen) show_window();
    if (choice == kRecorder) open_recorder();
    if (choice == kExit) want_close = true;
}

void App::popup_exit() {
    if (want_close) {
        want_close = false;
        if (unsaved_ && project_length(project_) > 0) ImGui::OpenPopup("Çıkış");
        else PostQuitMessage(0);
    }
    if (!ImGui::BeginPopupModal("Çıkış", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Projede kaydedilmemiş değişiklikler var.");
    if (ImGui::Button("Kaydet ve çık", ImVec2(150, 0))) {
        if (save_project(false)) PostQuitMessage(0);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Kaydetmeden çık", ImVec2(150, 0))) PostQuitMessage(0);
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::page_shortcuts() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Kısayollar");
    ImGui::PopFont();
    ImGui::Spacing();
    struct Row { const char* key; const char* what; };
    for (auto r : {Row{"Boşluk", "Oynat / durdur"}, Row{"Sol / Sağ ok", "Bir kare geri / ileri"}, Row{"Shift + ok", "Bir saniye geri / ileri"}, Row{"Home / End", "Başa / sona git"},
                   Row{"S", "Seçili klibi işaretçide böl"}, Row{"Delete", "Klibi sil"}, Row{"Shift + Delete", "Sil ve boşluğu kapat"}, Row{"Ctrl + Z / Y", "Geri al / yinele"},
                   Row{"Ctrl + C / V", "Kopyala / işaretçiye yapıştır"}, Row{"Ctrl + D", "Çoğalt"}, Row{"M", "İşaret ekle"}, Row{"I / O", "Aralığın başı / sonu"}, Row{"Shift + cetvelde sürükle", "Aralık seç"}, Row{"P", "Aralığı oynat"}, Row{"Esc", "Aralığı kaldır"}, Row{"+ / -", "Yakınlaştır / uzaklaştır"},
                   Row{"Ctrl + tekerlek", "Zaman çizelgesini yakınlaştır"}, Row{"Ctrl + S", "Projeyi kaydet"}, Row{"Ctrl + E", "Dışa aktar"}}) {
        ImGui::TextColored(theme_accent(), "%s", T(r.key));
        ImGui::SameLine(220);
        ImGui::TextUnformatted(r.what);
    }
}

void App::page_developers() {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Geliştiriciler");
    ImGui::PopFont();
    ImGui::TextDisabled("Video Düzenleyici'yi hazırlayan ekip");
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
    ImGui::BulletText("nlohmann/json - proje dosyaları (MIT)");
    ImGui::BulletText("FFmpeg - video ve ses işleme (GPLv3)");
    ImGui::Spacing();
    ImGui::TextDisabled("%s  ·  Sürüm %s", kAppName, kVersion);
}

void App::page_settings() {
    ImGui::BeginChild("ayar_menu", ImVec2(210, -28), 0, ImGuiWindowFlags_NoScrollbar);
    if (nav_button(ICON_BACK, "Geri", "geri", false)) page_ = "edit";
    ImGui::Spacing(); ImGui::Spacing();
    struct Item { const char* key; const char* name; const char* icon; };
    for (auto it : {Item{"theme", "Görünüm", ICON_THEME}, Item{"keys", "Kısayollar", ICON_KEYS}, Item{"help", "Yardım", ICON_HELP}})
        if (nav_button(it.icon, it.name, it.key, settings_tab_ == it.key)) settings_tab_ = it.key;
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ayar_icerik", ImVec2(0, -28));
    if (settings_tab_ == "theme") {
        ImGui::PushFont(nullptr, 24.f);
        ImGui::TextUnformatted("Görünüm");
        ImGui::PopFont();
        ImGui::TextDisabled("Tema Kayıt Stüdyosu ile ortaktır.");
        ImGui::Spacing();
        if (theme_settings()) save_theme();
        ImGui::Spacing();
        language_settings();
        ImGui::Spacing();
        ImGui::SeparatorText("Başlangıç");
        static int autostart = -1;
        if (autostart < 0) autostart = autostart_enabled(L"Video Editor") ? 1 : 0;
        bool on = autostart == 1;
        if (ImGui::Checkbox("Windows başlayınca otomatik başlat", &on)) {
            if (autostart_set(L"Video Editor", on)) {
                autostart = on ? 1 : 0;
                message_ = on ? "Video Düzenleyici Windows açılınca gizli simgelerde başlayacak" : "Otomatik başlatma kapatıldı";
                message_error_ = false;
            } else {
                message_ = "Otomatik başlatma ayarlanamadı";
                message_error_ = true;
            }
        }
        ImGui::TextDisabled("Program saatin yanındaki gizli simgelerde açılır. Pencerenin çarpısı da programı kapatmaz, oraya indirir.");
    } else if (settings_tab_ == "keys") {
        page_shortcuts();
    } else {
        help_tabs(kAppName, guide_editor, [this] { page_developers(); }, help_request_);
    }
    ImGui::EndChild();
}

void App::open_external(const std::wstring& path) {
    if (fs::path(path).extension() == L".vproje") { open_project(path); return; }
    Pending p;
    p.main = true;
    queue_add(path, p);
    message_ = TF("Kayıt eklendi: %s", narrow(fs::path(path).filename().wstring()).c_str());
    message_error_ = false;
}

void App::place_pending(const Media& m, const Pending& p) {
    const double at = p.time < 0 ? project_length(project_) : p.time;
    add_to_timeline(m, "", at);
    json* c = selected();
    // Kayıt Stüdyosu'nun çoklu ses parçalı kaydı: ilk parça karışımdır, diğerleri ayrı ses kanallarına açılır
    if (c && m.info.audio_titles.size() > 1 && m.info.audio_titles[0] == "Mix") {
        const std::string video_id = c->value("id", std::string());
        (*c)["has_audio"] = false;
        for (int k = 1; k < int(m.info.audio_titles.size()); k++) {
            const std::string t = m.info.audio_titles[k];
            const std::string name = t == "Desktop audio" ? T("Bilgisayar sesi") : t == "Microphone" ? T("Mikrofon") : t.empty() ? TF("Ses parçası %d", k + 1) : t;
            const double length = std::max(0.1, m.info.duration);
            json a = new_clip("audio", track_for("audio", at, length), at, length);
            a["file"] = narrow(m.path);
            a["name"] = name;
            a["source_length"] = m.info.duration;
            a["has_audio"] = true;
            a["audio_stream"] = k;
            project_["clips"].push_back(a);
        }
        select_only(video_id);
        c = selected();
        message_ = TF("%d ses parçası ayrı kanallara açıldı", int(m.info.audio_titles.size()) - 1);
        message_error_ = false;
        mark_edit();
    }
    if (c && p.camera.is_object()) {    // Kamera sahnedeki yerine: kayıt tuvali proje tuvaliyle aynı ölçüde varsayılır
        const json& canvas = project_["canvas"];
        const double W = canvas.value("w", 1920), H = canvas.value("h", 1080), mw = std::max(1, c->value("w", 1)), mh = std::max(1, c->value("h", 1));
        const double fit = std::min(W / mw, H / mh), bw = p.camera.value("w", 0.0);
        if (bw > 1) {
            (*c)["scale"] = bw / (mw * fit) * 100;
            (*c)["x"] = (p.camera.value("x", 0.0) + bw / 2 - W / 2) / W;
            (*c)["y"] = (p.camera.value("y", 0.0) + p.camera.value("h", 0.0) / 2 - H / 2) / H;
        }
        (*c)["flip_h"] = p.camera.value("mirror", false);
        (*c)["name"] = std::string(T("Kamera"));
        mark_edit();
    }
    if (!p.main) return;
    // Kaydın yanındaki ayrı kamera ve mikrofon dosyaları
    json side = read_json(m.path + L".input.json");
    if (!side.is_object()) side = read_json(m.path + L".girdi.json");     // Eski kayıtlar
    if (!side.is_object()) return;
    const fs::path folder = fs::path(m.path).parent_path();
    if (side.contains("camera") && side["camera"].is_object()) {
        const std::wstring file = (folder / widen(side["camera"].value("file", std::string()))).wstring();
        if (fs::exists(file)) {
            Pending cam;
            cam.time = at + side["camera"].value("offset", 0.0);
            cam.camera = side["camera"];
            queue_add(file, cam);
        }
    }
    if (side.contains("mic") && side["mic"].is_object()) {
        const std::wstring file = (folder / widen(side["mic"].value("file", std::string()))).wstring();
        if (fs::exists(file)) {
            Pending mic;
            mic.time = at;
            queue_add(file, mic);
        }
    }
}

// Kitaplıkta okunmuşsa hemen, değilse okununca eklenir
void App::queue_add(const std::wstring& path, const Pending& p) {
    for (auto& m : media_)
        if (m->path == path && m->ready) {
            if (!m->failed) place_pending(*m, p);
            return;
        }
    auto_add_[path] = p;
    import_media(path);
}

void App::open_recorder() {
    if (HWND other = FindWindowW(L"KayitStudyosu", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        PostMessageW(other, RegisterWindowMessageW(L"KayitStudyosuGoster"), 0, 0);
        return;
    }
    const std::wstring exe = exe_dir() + L"\\Recording Studio.exe";
    if (INT_PTR(ShellExecuteW(hwnd, L"open", exe.c_str(), nullptr, exe_dir().c_str(), SW_SHOWNORMAL)) <= 32) {
        message_ = "Kayıt Stüdyosu açılamadı";
        message_error_ = true;
    }
}

void App::on_drop(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; i++) {
        wchar_t path[MAX_PATH];
        if (!DragQueryFileW(drop, i, path, MAX_PATH)) continue;
        if (fs::path(path).extension() == L".vproje") open_project(path);
        else import_media(path);
    }
    DragFinish(drop);
}

int App::run() {
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
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap_, &device_, nullptr, &ctx_))) {
        MessageBoxW(hwnd, TW("Pencere çizimi başlatılamadı").c_str(), TW(kAppName).c_str(), MB_ICONERROR);
        return 1;
    }
    create_rtv();
    if (!create_gpu()) {
        MessageBoxW(hwnd, TW("Gölgelendirici derlenemedi").c_str(), TW(kAppName).c_str(), MB_ICONERROR);
        return 1;
    }
    setup_imgui(hwnd);
    load_settings();
    encoders_probe();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());
    analyser_ = std::thread(&App::analyse_loop, this);
    project_ = new_project();
    committed_ = project_;
    export_ = {{"preset_name", "high"}, {"size", "canvas"}, {"fps", "canvas"}, {"encoder", "x264"}, {"speed", "medium"}, {"quality_pct", 90}, {"container", "mp4"}, {"audio", 320}, {"audio_text", "320"},
               {"kind", "video"}, {"gif_width", "640"}, {"gif_fps", "15"}, {"audio_format", "mp3"}};
    bool project_given = false;
    for (auto& a : open_args) project_given |= fs::path(a).extension() == L".vproje";
    if (!project_given) {    // Son çalışma kaldığı yerden sürer
        const json last = read_json(data_dir() + L"\\editor-autosave.json");
        if (last.is_object() && last.contains("tracks") && last.contains("clips") && !last["clips"].empty()) {
            reset_project(last, L"");
            unsaved_ = true;
            message_ = "Son çalışma geri yüklendi";
        }
    }
    for (auto& a : open_args) open_external(a);
    tray_add(hwnd, WM_APP + 3, kAppName);
    if (!start_hidden) {    // Windows açılınca pencere açılmaz, simge gizli simgelerde bekler
        splash_finish(4.0);
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        SetForegroundWindow(hwnd);
    }

    ImGuiIO& io = ImGui::GetIO();
    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (window_hidden(hwnd)) {
            if (want_close) {   // Simgeden çıkış: kaydedilmemiş proje varsa soru görünsün diye pencere açılır
                if (unsaved_ && project_length(project_) > 0) show_window();
                else PostQuitMessage(0);
            }
            Sleep(30);
            continue;
        }
        {   // Arka planda okunan medya; kilit dışında işlenir (eklerken yeni dosya kuyruğa girebilir)
            std::deque<Done> done;
            {
                std::lock_guard<std::mutex> g(analyse_lock_);
                done.swap(analysed_);
            }
            while (!done.empty()) {
                Done d = std::move(done.front());
                done.pop_front();
                d.media->thumb = make_texture(d.thumb);
                d.media->ready = true;
                if (auto it = auto_add_.find(d.media->path); it != auto_add_.end()) {
                    const Pending p = it->second;
                    auto_add_.erase(it);
                    if (!d.media->failed) place_pending(*d.media, p);
                }
                if (!d.wave.empty()) waves_[d.media->path] = std::move(d.wave);
                for (size_t k = 0; k < d.extra_waves.size(); k++) waves_[d.media->path + L"#" + std::to_wstring(k + 1)] = std::move(d.extra_waves[k]);
                if (d.media->failed) { message_ = TF("Açılamadı: %s", d.media->name.c_str()); message_error_ = true; }
            }
        }
        if (now() > theme_checked_) { theme_checked_ = now() + 2; sync_theme(); }
        lang_tick();
        commit();
        update_preview();
        render_preview();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        theme_tick();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("ana", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
        main_menu();
        shortcuts();
        if (page_ == "settings") {
            page_settings();
        } else {
            // Bölmeler arasındaki ayırıcılar sürüklenerek boyutlandırılır; boyutlar düzen olarak saklanır
            const ImVec2 avail = ImGui::GetContentRegionAvail(), origin = ImGui::GetCursorPos();
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float auto_timeline = std::min(avail.y * 0.45f, kRulerH + 96 + kTrackH * std::max<size_t>(project_["tracks"].size(), 3));
            const float max_side = std::max(240.f, (avail.x - 2 * gap - 320) / 2);   // Önizlemeye en az 320 px kalsın
            float lib = std::clamp(float(layout_.value("library", 340.0)), 220.f, max_side);
            float insp = std::clamp(float(layout_.value("inspector", 340.0)), 260.f, max_side);
            float timeline = layout_.value("timeline", 0.0) > 0 ? float(layout_.value("timeline", 0.0)) : auto_timeline;
            timeline = std::clamp(timeline, 140.f, std::max(140.f, avail.y - 28 - gap - 220));
            const float top = avail.y - 28 - timeline - gap;
            bool resized = false;
            panel_media(ImVec2(lib, top));
            ImGui::SameLine(0, 0);
            resized |= splitter("##ayirici_kitaplik", true, gap, top, &lib, 220, max_side, 1);
            ImGui::SameLine(0, 0);
            panel_preview(ImVec2(avail.x - lib - insp - 2 * gap, top));
            ImGui::SameLine(0, 0);
            resized |= splitter("##ayirici_ozellik", true, gap, top, &insp, 260, max_side, -1);
            ImGui::SameLine(0, 0);
            panel_inspector(ImVec2(insp, top));
            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + top));
            resized |= splitter("##ayirici_zaman", false, gap, avail.x, &timeline, 140, avail.y - 28 - gap - 220, -1);
            ImGui::SetCursorPos(ImVec2(origin.x, origin.y + top + gap));
            panel_timeline(ImVec2(avail.x, timeline));
            if (resized) {
                layout_["library"] = lib; layout_["inspector"] = insp; layout_["timeline"] = timeline;
                layout_dirty_ = true;
            }
            if (layout_dirty_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { layout_save("editor", layout_); layout_dirty_ = false; }
        }
        status_bar();
        popup_export();
        popup_canvas();
        popup_chapters();
        popup_template();
        popup_batch();
        layout_popup("editor", layout_);
        popup_exit();
        ImGui::End();
        manual_frame(1);
        ImGui::Render();
        ID3D11RenderTargetView* rtv = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        ctx_->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_->Present(1, 0);
    }
    set_playing(false);
    if (mic_.running()) mic_.stop();
    live_.clear();
    if (exporting_) { export_job_.cancel = true; export_job_.thread.join(); }
    analyser_halt_ = true;
    analyser_.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    mouse_position_fix(msg, lp);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == g_show_message) {
        window_show(hwnd);
        return 0;
    }
    if (g_app && msg == taskbar_created_message()) { tray_add(hwnd, WM_APP + 3, kAppName); return 0; }
    switch (msg) {
        case WM_SIZE:
            if (g_app && wp != SIZE_MINIMIZED) g_app->on_resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (g_app && data && data->dwData == 1 && data->cbData >= sizeof(wchar_t))
                g_app->open_external(std::wstring(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t) - 1));
            return TRUE;
        }
        case WM_DROPFILES:
            if (g_app) g_app->on_drop(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:      // Çarpı gizli simgelere indirir; çıkış Dosya > Çıkış ya da simgenin menüsünden
            if (g_app) g_app->hide_to_tray();
            return 0;
        case WM_APP + 3:
            if (g_app) g_app->on_tray(UINT(LOWORD(lp)));
            return 0;
        case WM_GETMINMAXINFO: {
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {1280, 720};
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_show_message = RegisterWindowMessageW(L"VideoDuzenleyiciGoster");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\VideoDuzenleyici");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"VideoDuzenleyici", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, g_show_message, 0, 0);
        }
        return 0;
    }
    lang_init();
    bool startup = false;
    {
        int n = 0;
        LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; i < n; i++) startup |= !wcscmp(args[i], L"--baslangic");
        LocalFree(args);
    }
    if (!startup) splash_start({L"\xE8B2", T(kAppName), T("Video kurgu, efekt ve dışa aktarma"), TF("Sürüm %s", kVersion), T("Açık kaynak · Ücretsiz"), 0x0d6efd});
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"VideoDuzenleyici";
    RegisterClassExW(&wc);
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const int w = int(sw * 0.85), h = int(sh * 0.85);
    App app;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--baslangic")) app.start_hidden = true;
        else app.open_args.push_back(argv[i]);
    }
    LocalFree(argv);
    app.hwnd = CreateWindowW(wc.lpszClassName, TW(kAppName).c_str(), WS_OVERLAPPEDWINDOW, (sw - w) / 2, (sh - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    g_app = &app;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(app.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DragAcceptFiles(app.hwnd, TRUE);
    const int code = app.run();     // Pencere, hazırlık ve açılış ekranı bitince gösterilir
    tray_remove(app.hwnd);
    g_app = nullptr;
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return code;
}
