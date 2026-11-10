#include "ui.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>

bool splitter(const char* id, bool vertical, float thickness, float length, float* size, float lo, float hi, float sign) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, vertical ? ImVec2(thickness, std::max(1.f, length)) : ImVec2(std::max(1.f, length), thickness));
    const bool active = ImGui::IsItemActive(), hot = active || ImGui::IsItemHovered();
    if (hot) {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        ImVec4 c = theme_accent();
        c.w = active ? 1.f : 0.6f;
        auto* dl = ImGui::GetWindowDrawList();
        if (vertical) dl->AddLine(ImVec2(p.x + thickness / 2, p.y + 4), ImVec2(p.x + thickness / 2, p.y + length - 4), ImGui::ColorConvertFloat4ToU32(c), 3.f);
        else dl->AddLine(ImVec2(p.x + 4, p.y + thickness / 2), ImVec2(p.x + length - 4, p.y + thickness / 2), ImGui::ColorConvertFloat4ToU32(c), 3.f);
    }
    if (!active) return false;
    const float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    const float v = std::clamp(*size + sign * d, lo, std::max(lo, hi));
    if (v == *size) return false;
    *size = v;
    return true;
}

namespace {
std::wstring layout_path() { return data_dir() + L"\\layout.json"; }
bool layout_equal(const json& a, const json& b) {
    for (auto& [k, v] : b.items())
        if (!a.contains(k) || fabs(a[k].get<double>() - v.get<double>()) > 0.5) return false;
    return true;
}
bool g_layout_save_request = false;
}  // namespace

json layout_load(const char* app, const json& defaults) {
    json out = defaults;
    const json data = read_json(layout_path());
    if (data.is_object() && data.contains(app) && data[app].is_object())
        for (auto& [k, v] : data[app].items())
            if (out.contains(k) && v.is_number()) out[k] = v;
    return out;
}

void layout_save(const char* app, const json& layout) {
    json data = read_json(layout_path());
    if (!data.is_object()) data = json::object();
    data[app] = layout;
    write_json(layout_path(), data);
}

bool layout_menu(const char* app, json& layout, const std::vector<LayoutPreset>& presets) {
    bool changed = false;
    ImGui::TextDisabled("%s", T("Düzen"));
    for (auto& p : presets)
        if (ImGui::MenuItem(p.name, nullptr, layout_equal(layout, p.layout))) { layout = p.layout; changed = true; }
    json data = read_json(layout_path());
    const json saved = data.is_object() && data.contains("profiles") && data["profiles"].contains(app) ? data["profiles"][app] : json::object();
    if (!saved.empty()) {
        ImGui::Separator();
        for (auto& [name, l] : saved.items())
            if (ImGui::MenuItem(name.c_str(), nullptr, layout_equal(layout, l))) {
                for (auto& [k, v] : l.items()) if (layout.contains(k)) layout[k] = v;
                changed = true;
            }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Şu anki düzeni kaydet...")) g_layout_save_request = true;
    if (ImGui::BeginMenu("Kayıtlı düzeni sil", !saved.empty())) {
        for (auto& [name, l] : saved.items())
            if (ImGui::MenuItem(name.c_str())) {
                data["profiles"][app].erase(name);
                write_json(layout_path(), data);
                break;
            }
        ImGui::EndMenu();
    }
    if (changed) layout_save(app, layout);
    return changed;
}

void layout_popup(const char* app, const json& layout) {
    static char name[64] = "";
    if (g_layout_save_request) { g_layout_save_request = false; name[0] = 0; ImGui::OpenPopup("###duzen_kaydet"); }
    if (!ImGui::BeginPopupModal((std::string(T("Düzeni kaydet")) + "###duzen_kaydet").c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("%s", T("Bölmelerin şu anki boyutları bu adla saklanır. Görünüm menüsünden tek tıkla geri dönersiniz."));
    ImGui::SetNextItemWidth(320);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputTextWithHint("##duzen_ad", T("Düzen adı"), name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(!name[0]);
    if (ImGui::Button("Kaydet", ImVec2(120, 30)) || (enter && name[0])) {
        json data = read_json(layout_path());
        if (!data.is_object()) data = json::object();
        data["profiles"][app][name] = layout;
        write_json(layout_path(), data);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Vazgeç", ImVec2(120, 30))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --- Başka alfabeler: Segoe UI'da olmayan harfler için Windows yazı tipi, yalnızca gerekince ana yazı tipine eklenir ---
namespace {
std::vector<std::string> g_scripts_loaded;
bool g_scripts_all = false;         // Dil listesi açıldı: her dilin kendi adı görünsün

void load_script_font(const std::string& script) {
    if (script.empty() || std::find(g_scripts_loaded.begin(), g_scripts_loaded.end(), script) != g_scripts_loaded.end()) return;
    g_scripts_loaded.push_back(script);
    const std::vector<const wchar_t*> files = script == "zh" ? std::vector<const wchar_t*>{L"msyh.ttc", L"simsun.ttc"} :
                                              script == "ja" ? std::vector<const wchar_t*>{L"YuGothM.ttc", L"msgothic.ttc"} :
                                              script == "ko" ? std::vector<const wchar_t*>{L"malgun.ttf", L"gulim.ttc"} :
                                              script == "th" ? std::vector<const wchar_t*>{L"LeelawUI.ttf", L"tahoma.ttf"} :
                                              script == "hi" ? std::vector<const wchar_t*>{L"Nirmala.ttc", L"Nirmala.ttf", L"mangal.ttf"} :
                                                               std::vector<const wchar_t*>{};
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) return;
    for (const wchar_t* f : files) {
        const std::wstring path = std::wstring(windows) + L"\\Fonts\\" + f;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.DstFont = io.Fonts->Fonts[0];
        io.Fonts->AddFontFromFileTTF(narrow(path).c_str(), 17.f, &cfg);
        return;
    }
}

void font_hook() {      // lang_tick içinden, kare dışında
    if (!ImGui::GetCurrentContext()) return;
    if (const LanguageInfo* l = language_info(lang_code())) load_script_font(l->script);
    if (g_scripts_all) {
        g_scripts_all = false;
        for (auto& l : language_list()) load_script_font(l.script);
    }
}
}  // namespace

void language_settings() {
    ImGui::SeparatorText(T("Dil / Language"));
    auto name = [](const std::string& code) -> std::string {     // Seçili dildeki adı ve yanında kendi yazılışı
        for (auto& l : language_list())
            if (code == l.code) {
                const std::string local = T(l.turkish), native = language_native(l);
                if (local == native) return native;
                if (local == l.turkish && lang_code() != "tr") return native;      // Bu dilde adı çevrilmemiş
                return local + " — " + native;
            }
        return code;
    };
    const std::string setting = lang_setting();
    const std::string system = std::string(T("Sistem dili")) + " (" + name(system_language()) + ")";
    const std::string shown = setting == "system" ? system : name(setting);
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##dil", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        g_scripts_all = true;
        if (ImGui::Selectable(system.c_str(), setting == "system")) lang_set("system");
        for (auto& l : language_list())
            if (ImGui::Selectable(name(l.code).c_str(), setting == l.code)) lang_set(l.code);
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%s", T("Seçim Kayıt Stüdyosu, Oyun Araçları ve Video Düzenleyici'de ortaktır."));
    bool splash = splash_enabled();
    if (ImGui::Checkbox(T("Açılışta tanıtım ekranını göster"), &splash)) splash_set_enabled(splash);
}

static const char* kFonts[] = {"Segoe UI", "Bahnschrift", "Arial", "Verdana", "Tahoma", "Trebuchet MS", "Consolas", "Impact"};

void setup_imgui(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const float dpi = GetDpiForWindow(hwnd) / 96.f;
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    io.Fonts->AddFontFromFileTTF((narrow(fonts) + "\\Fonts\\segoeui.ttf").c_str(), 17.f);
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.GlyphOffset = ImVec2(0, 2);
    io.Fonts->AddFontFromFileTTF((narrow(fonts) + "\\Fonts\\segmdl2.ttf").c_str(), 17.f, &icons);
    lang_set_font_hook(font_hook);
    if (const LanguageInfo* l = language_info(lang_code())) load_script_font(l->script);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0; style.FrameRounding = 4; style.ChildRounding = 4; style.GrabRounding = 4; style.PopupRounding = 4;
    style.FontScaleDpi = dpi;
    style.ScaleAllSizes(dpi);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.15f, 1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Button] = ImVec4(0.19f, 0.22f, 0.29f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.31f, 0.47f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.25f, 0.69f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.22f, 0.29f, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.28f, 0.37f, 1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.32f, 0.43f, 1);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.2f, 0.8f, 0.4f, 1);
}

// Tıklama iletisi gerçek konumu taşır. Fare hareketi iletisi gelmeden yapılan tıklamada (dokunmatik, kalem,
// ani imleç atlaması) arayüz eski konumu kullanıp yanlış düğmeye basmasın.
void mouse_position_fix(UINT msg, LPARAM lp) {
    if (!ImGui::GetCurrentContext()) return;
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP)
        ImGui::GetIO().AddMousePosEvent(float(short(LOWORD(lp))), float(short(HIWORD(lp))));
}

// --- Tema ---
// İki tema tipi (Klasik, Bootstrap), her birinin koyu ve açık hali. Geçişte renkler ve köşeler kayarak değişir.

struct Palette {
    ImVec4 colors[ImGuiCol_COUNT];
    ImVec4 good, warn, error, accent;
    float frame_rounding, child_rounding, frame_border;
};

static Palette g_palettes[2][2];    // [tema tipi][0 koyu, 1 açık]
static Palette g_current, g_from;
static const Palette* g_target = &g_palettes[0][0];
static HWND g_theme_window = nullptr;
static int g_theme_style = 0;
static bool g_theme_light = false;
static int g_theme_mode = 0;           // 0 koyu, 1 açık, 2 sistem
static double g_theme_poll = 0;

// Windows uygulamalar için açık mı koyu mu kullanıyor
static bool system_light() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

static bool mode_light(int mode) { return mode == 2 ? system_light() : mode == 1; }
static float g_theme_pos = 1, g_theme_scale = 1;
static const float kThemeSeconds = 0.35f;
static const char* kThemeNames[] = {"Klasik", "Bootstrap"};

static ImVec4 hex(uint32_t rgb, float a = 1) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

static void theme_apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    memcpy(style.Colors, g_current.colors, sizeof(style.Colors));
    const float k = g_theme_scale;
    style.FrameRounding = style.GrabRounding = style.PopupRounding = style.TabRounding = g_current.frame_rounding * k;
    style.ChildRounding = g_current.child_rounding * k;
    style.FrameBorderSize = g_current.frame_border;
}

static void theme_title_bar() {
    const BOOL dark = !g_theme_light;
    DwmSetWindowAttribute(g_theme_window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

static void theme_go() {
    g_from = g_current;     // Geçiş sürerken yeni seçim yapılırsa o anki renkten devam eder
    g_target = &g_palettes[g_theme_style][g_theme_light ? 1 : 0];
    g_theme_pos = 0;
    theme_title_bar();
}

static void classic_palettes(const ImVec4* dark_colors) {
    Palette& d = g_palettes[0][0];
    Palette& l = g_palettes[0][1];
    memcpy(d.colors, dark_colors, sizeof(d.colors));
    d.good = ImVec4(0.3f, 0.85f, 0.45f, 1); d.warn = ImVec4(1, 0.7f, 0.3f, 1);
    d.error = ImVec4(1, 0.4f, 0.4f, 1); d.accent = ImVec4(0.4f, 0.7f, 1, 1);
    d.frame_rounding = 4; d.child_rounding = 4; d.frame_border = 0;

    ImGuiStyle bright;
    ImGui::StyleColorsLight(&bright);
    memcpy(l.colors, bright.Colors, sizeof(l.colors));
    ImVec4* c = l.colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.95f, 0.97f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.99f, 0.99f, 1, 1);
    c[ImGuiCol_Button] = ImVec4(0.85f, 0.87f, 0.91f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.75f, 0.81f, 0.93f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.72f, 0.98f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.85f, 0.87f, 0.91f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.79f, 0.83f, 0.90f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.73f, 0.79f, 0.90f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.15f, 0.68f, 0.35f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.45f, 0.50f, 1);
    l.good = ImVec4(0.05f, 0.5f, 0.2f, 1); l.warn = ImVec4(0.75f, 0.4f, 0, 1);
    l.error = ImVec4(0.8f, 0.1f, 0.1f, 1); l.accent = ImVec4(0.1f, 0.35f, 0.8f, 1);
    l.frame_rounding = 4; l.child_rounding = 4; l.frame_border = 0;
}

// Bootstrap 5.3'ün resmi renkleri: yüzey, kenarlık, birincil mavi ve durum renkleri
static void bootstrap_palette(Palette& p, bool light) {
    ImGuiStyle base;
    if (light) ImGui::StyleColorsLight(&base);
    else ImGui::StyleColorsDark(&base);
    memcpy(p.colors, base.Colors, sizeof(p.colors));
    ImVec4* c = p.colors;
    const ImVec4 primary = hex(0x0d6efd), body = hex(light ? 0xffffff : 0x212529), text = hex(light ? 0x212529 : 0xdee2e6);
    const ImVec4 surface = hex(light ? 0xf8f9fa : 0x2b3035), raised = hex(light ? 0xe9ecef : 0x343a40), border = hex(light ? 0xdee2e6 : 0x495057);
    const ImVec4 selected = hex(light ? 0xcfe2ff : 0x052c65), selected_hover = hex(light ? 0xb6d4fe : 0x084298), selected_active = hex(light ? 0x9ec5fe : 0x0a58ca);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = hex(light ? 0x6c757d : 0xadb5bd);
    c[ImGuiCol_WindowBg] = body;
    c[ImGuiCol_ChildBg] = ImVec4(body.x, body.y, body.z, 0);
    c[ImGuiCol_PopupBg] = surface;
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = light ? body : surface;
    c[ImGuiCol_FrameBgHovered] = light ? surface : raised;
    c[ImGuiCol_FrameBgActive] = raised;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = surface;
    c[ImGuiCol_TitleBgActive] = raised;
    c[ImGuiCol_MenuBarBg] = surface;
    c[ImGuiCol_ScrollbarBg] = surface;
    c[ImGuiCol_ScrollbarGrab] = hex(light ? 0xced4da : 0x495057);
    c[ImGuiCol_ScrollbarGrabHovered] = hex(light ? 0xadb5bd : 0x6c757d);
    c[ImGuiCol_ScrollbarGrabActive] = hex(0x6c757d);
    c[ImGuiCol_CheckMark] = primary;
    c[ImGuiCol_SliderGrab] = primary;
    c[ImGuiCol_SliderGrabActive] = hex(0x0a58ca);
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = light ? border : hex(0x495057);
    c[ImGuiCol_ButtonActive] = light ? selected : hex(0x0a58ca);
    c[ImGuiCol_Header] = selected;
    c[ImGuiCol_HeaderHovered] = selected_hover;
    c[ImGuiCol_HeaderActive] = selected_active;
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = primary;
    c[ImGuiCol_ResizeGrip] = border;
    c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = primary;
    c[ImGuiCol_Tab] = raised;
    c[ImGuiCol_TabHovered] = selected_hover;
    c[ImGuiCol_TabSelected] = selected;
    c[ImGuiCol_TabSelectedOverline] = primary;
    c[ImGuiCol_PlotHistogram] = hex(0x198754);
    c[ImGuiCol_TextSelectedBg] = selected_active;
    p.good = hex(light ? 0x198754 : 0x75b798);
    p.warn = hex(light ? 0x997404 : 0xffda6a);
    p.error = hex(light ? 0xdc3545 : 0xea868f);
    p.accent = hex(light ? 0x0d6efd : 0x6ea8fe);
    p.frame_rounding = 6; p.child_rounding = 8; p.frame_border = 1;
}

int theme_mode_from(const json& settings) {
    if (!settings.is_object()) return 0;
    const int mode = settings.value("theme_mode", -1);     // -1: henüz seçilmedi, eski ayara bakılır
    if (mode >= 0) return std::min(mode, 2);
    return settings.value("theme_light", false) ? 1 : 0;
}

void theme_set_mode(int mode) {
    g_theme_mode = std::clamp(mode, 0, 2);
    const bool light = mode_light(g_theme_mode);
    if (light != g_theme_light) theme_set(light);
}

int theme_mode() { return g_theme_mode; }

void theme_start(HWND hwnd, int style_index, int mode) {
    g_theme_mode = std::clamp(mode, 0, 2);
    const bool light = mode_light(g_theme_mode);
    ImGuiStyle& style = ImGui::GetStyle();
    g_theme_scale = style.FontScaleDpi;
    classic_palettes(style.Colors);     // setup_imgui'nin hazırladığı koyu palet
    bootstrap_palette(g_palettes[1][0], false);
    bootstrap_palette(g_palettes[1][1], true);
    g_theme_window = hwnd;
    g_theme_style = style_index == 1 ? 1 : 0;
    g_theme_light = light;
    g_target = &g_palettes[g_theme_style][light ? 1 : 0];
    g_current = *g_target;
    g_theme_pos = 1;
    theme_apply();
    theme_title_bar();
}

void theme_set(bool light) {
    g_theme_light = light;
    theme_go();
}

void theme_set_style(int style_index) {
    g_theme_style = style_index;
    theme_go();
}

bool theme_is_light() { return g_theme_light; }
int theme_style() { return g_theme_style; }

void theme_tick() {
    if (g_theme_mode == 2 && now() >= g_theme_poll) {     // Sistem teması değişince program da değişir
        g_theme_poll = now() + 1;
        if (system_light() != g_theme_light) theme_set(!g_theme_light);
    }
    if (g_theme_pos >= 1) return;
    g_theme_pos = std::min(g_theme_pos + ImGui::GetIO().DeltaTime / kThemeSeconds, 1.f);
    const float t = g_theme_pos * g_theme_pos * (3 - 2 * g_theme_pos);
    for (int i = 0; i < ImGuiCol_COUNT; i++) g_current.colors[i] = mix(g_from.colors[i], g_target->colors[i], t);
    g_current.good = mix(g_from.good, g_target->good, t);
    g_current.warn = mix(g_from.warn, g_target->warn, t);
    g_current.error = mix(g_from.error, g_target->error, t);
    g_current.accent = mix(g_from.accent, g_target->accent, t);
    g_current.frame_rounding = g_from.frame_rounding + (g_target->frame_rounding - g_from.frame_rounding) * t;
    g_current.child_rounding = g_from.child_rounding + (g_target->child_rounding - g_from.child_rounding) * t;
    g_current.frame_border = g_from.frame_border + (g_target->frame_border - g_from.frame_border) * t;
    theme_apply();
}

ImVec4 theme_good() { return g_current.good; }
ImVec4 theme_warn() { return g_current.warn; }
ImVec4 theme_error() { return g_current.error; }
ImVec4 theme_accent() { return g_current.accent; }

bool theme_settings() {
    bool changed = false;
    ImGui::SeparatorText("Tema");
    ImGui::TextDisabled("Programın genel görünümü. Kartın üstüne tıklayınca hemen uygulanır.");
    ImGui::Spacing();
    const float k = g_theme_scale;
    const ImVec2 size(250 * k, 132 * k);
    for (int i = 0; i < 2; i++) {
        ImGui::PushID(i);
        if (i) ImGui::SameLine();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("kart", size) && g_theme_style != i) {
            theme_set_style(i);
            changed = true;
        }
        const Palette& t = g_palettes[i][g_theme_light ? 1 : 0];
        const float round = t.child_rounding * k;
        auto* draw = ImGui::GetWindowDrawList();
        const auto color = [](const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); };
        const ImVec2 end(p.x + size.x, p.y + size.y);
        draw->AddRectFilled(p, end, color(t.colors[ImGuiCol_WindowBg]), round);
        draw->AddRectFilled(ImVec2(p.x + 1, p.y + 1), ImVec2(end.x - 1, p.y + 30 * k), color(t.colors[ImGuiCol_MenuBarBg]), round, ImDrawFlags_RoundCornersTop);
        draw->AddText(ImVec2(p.x + 14 * k, p.y + 6 * k), color(t.colors[ImGuiCol_Text]), kThemeNames[i]);
        const float r = t.frame_rounding * k;
        float x = p.x + 14 * k;
        const float y = p.y + 44 * k;
        for (ImGuiCol c : {ImGuiCol_Button, ImGuiCol_Header, ImGuiCol_SliderGrab}) {
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + 66 * k, y + 28 * k), color(t.colors[c]), r);
            x += 74 * k;
        }
        draw->AddRectFilled(ImVec2(p.x + 14 * k, y + 40 * k), ImVec2(end.x - 14 * k, y + 50 * k), color(t.colors[ImGuiCol_FrameBg]), r);
        draw->AddRectFilled(ImVec2(p.x + 14 * k, y + 40 * k), ImVec2(p.x + 130 * k, y + 50 * k), color(t.colors[ImGuiCol_PlotHistogram]), r);
        const bool on = g_theme_style == i;
        const bool hover = ImGui::IsItemHovered();
        draw->AddRect(p, end, color(on ? g_current.accent : (hover ? g_current.colors[ImGuiCol_ButtonHovered] : g_current.colors[ImGuiCol_Border])), round,
                      ImDrawFlags_None, on ? 3.f * k : 1.f);
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Mod");
    int mode = g_theme_mode;
    if (ImGui::RadioButton("Sistem varsayılanı", mode == 2)) mode = 2;
    ImGui::SameLine();
    if (ImGui::RadioButton("Koyu", mode == 0)) mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Açık", mode == 1)) mode = 1;
    if (mode != g_theme_mode) {
        theme_set_mode(mode);
        changed = true;
    }
    ImGui::TextDisabled("Sistem varsayılanı, Windows'un açık / koyu ayarını izler ve o değişince kendiliğinden değişir.");
    return changed;
}

// "Etiket##kimlik" yazısının görünen kısmı
static std::string visible_label(const char* label) {
    std::string text = label;
    const size_t cut = text.find("##");
    return cut == std::string::npos ? text : text.substr(0, cut);
}

// Yazarken alt sınıra zorlanmaz (10 yazmak için önce 1 yazılır); kutudan çıkınca sınıra çekilir
template <typename V, typename Slider, typename Box>
static bool number_row(const char* label, V* value, V lo, V hi, Slider slider, Box box) {
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x, box_w = 72;
    const float total = ImGui::CalcItemWidth();
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(std::max(40.f, total - box_w - spacing));
    changed |= slider();
    ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(box_w);
    V typed = *value;
    if (box(&typed)) {
        *value = std::min(typed, hi);
        changed = true;
    }
    if (ImGui::IsItemDeactivated() && *value < lo) {
        *value = lo;
        changed = true;
    }
    ImGui::SetItemTooltip("Değeri yazmak için tıklayın");
    ImGui::PopID();
    const std::string shown = visible_label(label);
    if (!shown.empty()) {
        ImGui::SameLine(0, spacing);
        ImGui::TextUnformatted(shown.c_str());
    }
    return changed;
}

bool slider_int(const char* label, int* value, int lo, int hi) {
    return number_row<int>(label, value, lo, hi,
                           [&] { return ImGui::SliderInt("##kaydirici", value, lo, hi, "", ImGuiSliderFlags_AlwaysClamp); },
                           [](int* v) { return ImGui::InputInt("##sayi", v, 0, 0); });
}

bool slider_float(const char* label, float* value, float lo, float hi, const char* format) {
    return number_row<float>(label, value, lo, hi,
                             [&] { return ImGui::SliderFloat("##kaydirici", value, lo, hi, "", ImGuiSliderFlags_AlwaysClamp); },
                             [&](float* v) { return ImGui::InputFloat("##sayi", v, 0, 0, format); });
}

bool edit_int(const char* label, json& j, const char* key, int lo, int hi) {
    int v = j.value(key, lo);
    if (!slider_int(label, &v, lo, hi)) return false;
    j[key] = v;
    return true;
}

bool edit_bool(const char* label, json& j, const char* key) {
    bool v = j.value(key, false);
    if (!ImGui::Checkbox(label, &v)) return false;
    j[key] = v;
    return true;
}

bool edit_text(const char* label, json& j, const char* key, bool multiline) {
    char buf[512];
    strncpy_s(buf, j.value(key, std::string()).c_str(), _TRUNCATE);
    const bool done = multiline ? ImGui::InputTextMultiline(T(label), buf, sizeof(buf), ImVec2(-1, 60))
                                : ImGui::InputText(T(label), buf, sizeof(buf));
    if (done) j[key] = std::string(buf);
    return done;
}

bool edit_color(const char* label, json& j, const char* key) {
    const uint32_t c = parse_color(j.value(key, std::string("#ffffff")));
    float rgb[3] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f};
    if (!ImGui::ColorEdit3(T(label), rgb, ImGuiColorEditFlags_NoInputs)) return false;
    char hex[8];
    sprintf_s(hex, "#%02x%02x%02x", int(rgb[0] * 255 + .5f), int(rgb[1] * 255 + .5f), int(rgb[2] * 255 + .5f));
    j[key] = std::string(hex);
    return true;
}

bool edit_choice(const char* label, json& j, const char* key, const std::vector<std::pair<std::string, std::string>>& items) {
    const std::string current = j.value(key, std::string());
    std::string shown = items.empty() ? "" : items[0].second;
    for (auto& i : items)
        if (i.first == current) shown = i.second;
    bool changed = false;
    if (ImGui::BeginCombo(label, shown.c_str())) {
        for (auto& i : items)
            if (ImGui::Selectable(i.second.c_str(), i.first == current)) { j[key] = i.first; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool edit_font(json& j) {
    const std::string current = j.value("font", std::string("Segoe UI"));
    bool changed = false;
    if (ImGui::BeginCombo("Yazı tipi", current.c_str())) {
        for (auto f : kFonts)
            if (ImGui::Selectable(f, current == f)) { j["font"] = f; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool nav_button(const char* icon, const char* name, const char* id, bool on, bool wide) {
    name = T(name);
    const std::string text = (wide ? std::string(icon) + "   " + name : std::string(icon)) + "###" + id;
    if (on) {   // Seçili düğme fare üstündeyken de seçili görünsün
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(wide ? 0.f : 0.5f, 0.5f));
    if (wide) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 4));
    const bool pressed = ImGui::Button(text.c_str(), ImVec2(-1, 36));
    ImGui::PopStyleVar(wide ? 2 : 1);
    if (on) ImGui::PopStyleColor(2);
    if (!wide) ImGui::SetItemTooltip("%s", name);     // name zaten çevrildi
    return pressed;
}

std::string key_name(const Hotkey& h) {
    std::string s;
    if (h.ctrl) s += "Ctrl + ";
    if (h.shift) s += "Shift + ";
    if (h.alt) s += "Alt + ";
    if (h.vk >= VK_F1 && h.vk <= VK_F24) return s + "F" + std::to_string(h.vk - VK_F1 + 1);
    if (h.vk == VK_MBUTTON) return s + T("Farenin orta tuşu");
    if (h.vk == VK_XBUTTON1) return s + T("Fare yan tuşu 1");
    if (h.vk == VK_XBUTTON2) return s + T("Fare yan tuşu 2");
    wchar_t name[64];
    const UINT scan = MapVirtualKeyW(h.vk, MAPVK_VK_TO_VSC);
    if (GetKeyNameTextW(scan << 16, name, 64)) return s + narrow(name);
    return s + "Tuş " + std::to_string(h.vk);
}

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// --- Bildirim alanı ve otomatik başlatma ---

void tray_add(HWND hwnd, UINT message, const char* tip) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = message;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wcscpy_s(nid.szTip, TW(tip).c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void tray_remove(HWND hwnd) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void tray_note(HWND hwnd, const char* title, const char* text) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, TW(title).c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, TW(text).c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

int tray_menu(HWND hwnd, const std::vector<std::pair<int, std::string>>& items) {
    HMENU menu = CreatePopupMenu();
    for (auto& [id, text] : items) {
        if (id == 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else AppendMenuW(menu, MF_STRING, UINT_PTR(id), TW(text.c_str()).c_str());
    }
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);      // Dışarı tıklanınca menü kapansın diye gerekli
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    return choice;
}

void window_show(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

bool window_hidden(HWND hwnd) { return IsIconic(hwnd) || !IsWindowVisible(hwnd); }

UINT taskbar_created_message() {
    static const UINT id = RegisterWindowMessageW(L"TaskbarCreated");
    return id;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static std::wstring startup_command() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return L"\"" + std::wstring(path) + L"\" --baslangic";
}

bool autostart_enabled(const wchar_t* name) {
    wchar_t value[1024] = L"";
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    if (startup_command() != value) autostart_set(name, true);      // Program taşındıysa yeni yeri göstersin
    return true;
}

bool autostart_set(const wchar_t* name, bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS status;
    if (enable) {
        const std::wstring command = startup_command();
        status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}