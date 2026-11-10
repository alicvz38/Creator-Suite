// İki programın ortak arayüz parçaları: görünüm, ayar denetimleri, kısayol tuşları.
#pragma once
#include "common.h"
#include "language.h"

#include "imgui/imgui.h"

void setup_imgui(HWND hwnd);    // Yazı tipleri ve renkler
void mouse_position_fix(UINT msg, LPARAM lp);    // Pencere iletisi işlenmeden önce çağrılır

// Tema: iki tip (0 Klasik, 1 Bootstrap), her birinin koyu ve açık hali; renkler kayarak geçer
// mode: 0 koyu, 1 açık, 2 sistem varsayılanı (Windows ayarını izler)
void theme_start(HWND hwnd, int style_index, int mode);    // Açılışta, geçişsiz
int theme_mode_from(const json& settings);  // Ayar dosyasından; eski "theme_light" anahtarını da okur
void theme_set_mode(int mode);
int theme_mode();
void theme_set(bool light);
void theme_set_style(int style_index);
bool theme_is_light();
int theme_style();
bool theme_settings();                      // Ayarlar sayfasındaki tema seçimi; değişince true
void theme_tick();                          // Her karede NewFrame'den sonra
ImVec4 theme_good();
ImVec4 theme_warn();
ImVec4 theme_error();
ImVec4 theme_accent();
#define ICON_THEME "\xEE\x9E\x90"
#define ICON_SUN "\xEE\x9C\x86"
#define ICON_MOON "\xEE\x9C\x88"

// Kaydırıcı ve yanında elle yazılabilen sayı kutusu
bool slider_int(const char* label, int* value, int lo, int hi);
bool slider_float(const char* label, float* value, float lo, float hi, const char* format = "%.2f");
bool edit_int(const char* label, json& j, const char* key, int lo, int hi);
bool edit_bool(const char* label, json& j, const char* key);
bool edit_text(const char* label, json& j, const char* key, bool multiline = false);
void language_settings();               // Ayarlar sayfasında: dil seçimi ve açılış ekranı (üç programda ortak)

// Bölmeler arasındaki sürüklenebilir ayırıcı. vertical: dikey çizgi (genişlik değişir). sign: +1 sağa / aşağı sürükleyince büyür, -1 küçülür
bool splitter(const char* id, bool vertical, float thickness, float length, float* size, float lo, float hi, float sign);
// Düzen: bölme boyutları (data_dir()\layout.json). app: "editor" ya da "studio"
struct LayoutPreset { const char* name; json layout; };
json layout_load(const char* app, const json& defaults);
void layout_save(const char* app, const json& layout);
bool layout_menu(const char* app, json& layout, const std::vector<LayoutPreset>& presets);  // Görünüm menüsünün içinde; değişince true
void layout_popup(const char* app, const json& layout);                                    // Her karede: "Düzeni kaydet" penceresi
bool edit_color(const char* label, json& j, const char* key);
bool edit_choice(const char* label, json& j, const char* key, const std::vector<std::pair<std::string, std::string>>& items);
bool edit_font(json& j);
bool nav_button(const char* icon, const char* name, const char* id, bool on, bool wide = true);

// Yardım sayfaları (yardim.cpp)
#define ICON_HELP "\xEE\xA2\x97"
#define ICON_CHAT "\xEE\xA2\xBD"
#define ICON_MAIL "\xEE\x9C\x95"
void guide_record();                    // Kayıt Stüdyosu kılavuzu
void guide_game();                      // Oyun Araçları kılavuzu
void guide_editor();                    // Video Düzenleyici kılavuzu
// Kılavuz penceresi (F1): app 0 Kayıt Stüdyosu, 1 Video Düzenleyici, 2 Oyun Araçları; her karede ana pencereden sonra çağrılır
void manual_window(bool* open, int app);
void manual_open();             // Menüden: kılavuz penceresini aç
void manual_frame(int app);     // Her karede Render'dan önce: F1 ve pencere
void manual_view(int app);
void contact_page(const char* app);     // Düğmelerle iletişim; numara ve adres ekranda görünmez
void thanks_section();                  // Geliştiriciler sayfasında: teşekkürler (üç programda ortak)
// Kılavuz, İletişim, Geliştiriciler, Lisans, Gizlilik, Telif sekmeleri. request >= 0 ise o sekmeye geçilir (0-5)
void help_tabs(const char* app, void (*guide_fn)(), const std::function<void()>& developers, int& request);
void legal_tabs(float height);          // Kurulum sihirbazı için lisans, gizlilik ve telif sekmeleri

struct Hotkey { int vk = 0; bool ctrl = false, shift = false, alt = false; };
std::string key_name(const Hotkey& h);
// Bildirim alanı simgesi (saatin yanındaki gizli simgeler) ve Windows açılınca başlatma; üç program ortak kullanır
void tray_add(HWND hwnd, UINT message, const char* tip);       // message: simgeye tıklanınca pencereye gelen ileti
void tray_remove(HWND hwnd);
void tray_note(HWND hwnd, const char* title, const char* text); // Türkçe verilir, çevrilerek gösterilir
int tray_menu(HWND hwnd, const std::vector<std::pair<int, std::string>>& items);    // id 0 ayırıcı; seçilen id, yoksa 0
void window_show(HWND hwnd);            // Gizli ya da simge durumundaki pencereyi öne getirir
bool window_hidden(HWND hwnd);          // Gizli ya da simge durumunda mı
UINT taskbar_created_message();         // Explorer yeniden başlayınca simge yeniden eklenmeli
bool autostart_enabled(const wchar_t* name);
bool autostart_set(const wchar_t* name, bool enable);   // HKCU Run: exe "--baslangic" ile, görev çubuğunda değil gizli simgelerde başlar
bool key_down(int vk);

// Dil: ImGui çağrılarındaki görünen metin T()'den geçer. Kaynakta Türkçe yazılır, çevirisi dil\*.json'dan gelir.
// (Yalnızca bu başlığı içeren arayüz dosyalarında geçerlidir; ImGui'nin kendi dosyaları etkilenmez.)
#define Button(label, ...) Button(T(label), __VA_ARGS__)
#define SmallButton(label) SmallButton(T(label))
#define Checkbox(label, ...) Checkbox(T(label), __VA_ARGS__)
#define RadioButton(label, ...) RadioButton(T(label), __VA_ARGS__)
#define MenuItem(label, ...) MenuItem(T(label), __VA_ARGS__)
#define BeginMenu(label, ...) BeginMenu(T(label), __VA_ARGS__)
#define Selectable(label, ...) Selectable(T(label), __VA_ARGS__)
#define SeparatorText(label) SeparatorText(T(label))
#define CollapsingHeader(label, ...) CollapsingHeader(T(label), __VA_ARGS__)
#define BeginTabItem(label, ...) BeginTabItem(T(label), __VA_ARGS__)
#define BeginCombo(label, preview, ...) BeginCombo(T(label), T(preview), __VA_ARGS__)
#define TextUnformatted(text) TextUnformatted(T(text))
#define Text(fmt, ...) Text(T(fmt), __VA_ARGS__)
#define TextDisabled(fmt, ...) TextDisabled(T(fmt), __VA_ARGS__)
#define TextWrapped(fmt, ...) TextWrapped(T(fmt), __VA_ARGS__)
#define BulletText(fmt, ...) BulletText(T(fmt), __VA_ARGS__)
#define TextColored(color, fmt, ...) TextColored(color, T(fmt), __VA_ARGS__)
#define SetItemTooltip(fmt, ...) SetItemTooltip(T(fmt), __VA_ARGS__)
#define SetTooltip(fmt, ...) SetTooltip(T(fmt), __VA_ARGS__)
#define InputTextWithHint(label, hint, ...) InputTextWithHint(label, T(hint), __VA_ARGS__)
#define OpenPopup(id, ...) OpenPopup(T(id), __VA_ARGS__)
#define BeginPopup(id, ...) BeginPopup(T(id), __VA_ARGS__)
#define BeginPopupModal(name, ...) BeginPopupModal(T(name), __VA_ARGS__)
#define IsPopupOpen(id, ...) IsPopupOpen(T(id), __VA_ARGS__)
#define TableSetupColumn(label, ...) TableSetupColumn(T(label), __VA_ARGS__)
