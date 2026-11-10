#include "language.h"

#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {
struct Table {
    std::string code;
    std::unordered_map<std::string, std::string> text;
};

std::mutex g_lock;
std::list<Table> g_tables;          // Eski tablolar silinmez: daha önce dönen işaretçiler geçerli kalır
Table* g_table = nullptr;           // nullptr: Türkçe
std::string g_setting = "system";
bool g_splash = true;
double g_checked = 0;
fs::file_time_type g_stamp{};

std::wstring settings_path() { return data_dir() + L"\\language.json"; }

// --- Sağdan sola diller: arayüz harfleri soldan sağa dizer; çeviriler yüklenirken görsel sıraya çevrilir ---

// Arapça harflerin bağlı biçimleri: tek, son, baş, orta (0: yok). Baş biçimi olmayan harf sonrakine bağlanmaz.
struct ArabicForms { wchar_t letter, iso, fin, ini, med; };
const ArabicForms kArabic[] = {
    {0x0621, 0xFE80, 0, 0, 0}, {0x0622, 0xFE81, 0xFE82, 0, 0}, {0x0623, 0xFE83, 0xFE84, 0, 0}, {0x0624, 0xFE85, 0xFE86, 0, 0},
    {0x0625, 0xFE87, 0xFE88, 0, 0}, {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, {0x0627, 0xFE8D, 0xFE8E, 0, 0},
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, {0x0629, 0xFE93, 0xFE94, 0, 0}, {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, {0x062F, 0xFEA9, 0xFEAA, 0, 0}, {0x0630, 0xFEAB, 0xFEAC, 0, 0},
    {0x0631, 0xFEAD, 0xFEAE, 0, 0}, {0x0632, 0xFEAF, 0xFEB0, 0, 0}, {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, {0x0640, 0x0640, 0x0640, 0x0640, 0x0640}, {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0, 0}, {0x0649, 0xFEEF, 0xFEF0, 0, 0}, {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    // Farsça ve Urduca harfler
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, {0x0698, 0xFB8A, 0xFB8B, 0, 0},
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91}, {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},
    {0x0679, 0xFB66, 0xFB67, 0xFB68, 0xFB69}, {0x0688, 0xFB88, 0xFB89, 0, 0}, {0x0691, 0xFB8C, 0xFB8D, 0, 0},
    {0x06BA, 0xFB9E, 0xFB9F, 0xFEE7, 0xFEE8}, {0x06C1, 0xFBA6, 0xFBA7, 0xFBA8, 0xFBA9}, {0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD},
    {0x06D2, 0xFBAE, 0xFBAF, 0, 0},
};

const ArabicForms* arabic_forms(wchar_t c) {
    for (auto& f : kArabic) if (f.letter == c) return &f;
    return nullptr;
}
bool arabic_mark(wchar_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }    // Hareke: bağlanmayı bozmaz

// Harfleri bağlı biçimlerine çevirir (mantıksal sırada)
std::wstring arabic_shape(const std::wstring& s) {
    std::wstring out;
    const size_t n = s.size();
    auto prev_joins = [&](size_t i) {      // Önceki (sağdaki) harf bu harfe bağlanabilir mi
        for (size_t j = i; j-- > 0;) {
            if (arabic_mark(s[j])) continue;
            const ArabicForms* f = arabic_forms(s[j]);
            return f && f->ini != 0;
        }
        return false;
    };
    auto next_letter = [&](size_t i) -> const ArabicForms* {
        for (size_t j = i + 1; j < n; j++) {
            if (arabic_mark(s[j])) continue;
            return arabic_forms(s[j]);
        }
        return nullptr;
    };
    for (size_t i = 0; i < n; i++) {
        const ArabicForms* f = arabic_forms(s[i]);
        if (!f) { out += s[i]; continue; }
        const bool before = prev_joins(i);
        // Lam + elif bitişik yazılır
        if (s[i] == 0x0644 && i + 1 < n) {
            const wchar_t a = s[i + 1];
            const wchar_t lig = a == 0x0622 ? 0xFEF5 : a == 0x0623 ? 0xFEF7 : a == 0x0625 ? 0xFEF9 : a == 0x0627 ? 0xFEFB : 0;
            if (lig) { out += wchar_t(lig + (before ? 1 : 0)); i++; continue; }
        }
        const ArabicForms* nx = next_letter(i);
        const bool after = f->ini != 0 && nx != nullptr;
        wchar_t c = f->iso;
        if (before && after && f->med) c = f->med;
        else if (before && f->fin) c = f->fin;
        else if (after && f->ini) c = f->ini;
        out += c;
    }
    return out;
}

bool rtl_char(wchar_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
wchar_t mirror(wchar_t c) {
    switch (c) {
    case L'(': return L')'; case L')': return L'(';
    case L'[': return L']'; case L']': return L'[';
    case L'<': return L'>'; case L'>': return L'<';
    case L'{': return L'}'; case L'}': return L'{';
    case 0x00AB: return 0x00BB; case 0x00BB: return 0x00AB;
    default: return c;
    }
}

// Tek satırı görsel sıraya çevirir: sağdan sola parçalar ters döner, Latin harf, rakam ve %d gibi yer tutucular olduğu gibi kalır
std::wstring rtl_line(const std::wstring& s) {
    struct Token { std::wstring text; int dir; };   // 1 sağdan sola, 2 soldan sağa, 0 nötr
    std::vector<Token> tokens;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'%') {     // printf yer tutucusu tek parça soldan sağa
            size_t j = i + 1;
            if (j < s.size() && s[j] == L'%') j++;
            else {
                while (j < s.size() && wcschr(L"-+ #0123456789.", s[j])) j++;
                while (j < s.size() && wcschr(L"hlzjt", s[j])) j++;
                if (j < s.size() && iswalpha(s[j])) j++;
            }
            tokens.push_back({s.substr(i, j - i), 2});
            i = j;
            continue;
        }
        const wchar_t c = s[i];
        const int dir = rtl_char(c) ? 1 : (iswalnum(c) || (c >= 0x00C0 && c < 0x0590)) ? 2 : 0;
        tokens.push_back({std::wstring(1, c), dir});
        i++;
    }
    // Nötrler: iki yanı soldan sağaysa soldan sağa, yoksa satırın yönü (sağdan sola)
    for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].dir) continue;
        size_t j = i;
        while (j < tokens.size() && !tokens[j].dir) j++;
        const int left = i > 0 ? tokens[i - 1].dir : 1, right = j < tokens.size() ? tokens[j].dir : 1;
        const int d = left == 2 && right == 2 ? 2 : 1;
        for (size_t k = i; k < j; k++) tokens[k].dir = d;
        i = j - 1;
    }
    // Aynı yöndeki ardışık parçalar birleşir; sıra ters çevrilir, sağdan sola parçaların içi de ters döner
    std::vector<Token> runs;
    for (auto& t : tokens) {
        if (!runs.empty() && runs.back().dir == t.dir) runs.back().text += t.text;
        else runs.push_back(t);
    }
    std::wstring out;
    for (size_t r = runs.size(); r-- > 0;) {
        if (runs[r].dir == 2) { out += runs[r].text; continue; }
        for (size_t k = runs[r].text.size(); k-- > 0;) out += mirror(runs[r].text[k]);
    }
    return out;
}

std::string rtl_visual(const std::string& text) {
    const std::wstring shaped = arabic_shape(widen(text));
    std::wstring out;
    size_t from = 0;
    for (;;) {
        const size_t nl = shaped.find(L'\n', from);
        out += rtl_line(shaped.substr(from, nl == std::wstring::npos ? std::wstring::npos : nl - from));
        if (nl == std::wstring::npos) break;
        out += L'\n';
        from = nl + 1;
    }
    return narrow(out);
}

void (*g_font_hook)() = nullptr;

void load_table(const std::string& code) {     // g_lock tutulurken çağrılır
    if (code == "tr") { g_table = nullptr; return; }
    for (auto& t : g_tables)
        if (t.code == code) { g_table = &t; return; }
    auto read = [](const std::string& c, Table& t) {
        std::ifstream in(fs::path(exe_dir()) / L"languages" / (widen(c) + L".json"), std::ios::binary);
        std::string data;
        if (in) {
            data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else {    // Kurulum dosyası gibi tek exe: çeviriler kaynak olarak gömülüdür (LANG_EN, LANG_DE, ...)
            std::wstring name = L"LANG_" + widen(c);
            for (auto& ch : name) ch = towupper(ch);
            if (HRSRC res = FindResourceW(nullptr, name.c_str(), MAKEINTRESOURCEW(10))) {
                HGLOBAL mem = LoadResource(nullptr, res);
                if (mem) data.assign(static_cast<const char*>(LockResource(mem)), SizeofResource(nullptr, res));
            }
        }
        if (data.empty()) return;
        try {
            const json j = json::parse(data);
            for (auto& [k, v] : j.items())
                if (v.is_string() && !v.get<std::string>().empty() && !t.text.count(k)) t.text[k] = v.get<std::string>();
        } catch (...) {
        }
    };
    Table t;
    t.code = code;
    read(code, t);
    const LanguageInfo* info = language_info(code);
    if (info && info->rtl)
        for (auto& [k, v] : t.text) v = rtl_visual(v);
    if (code != "en") read("en", t);       // Çevrilmemiş metin Türkçe değil İngilizce görünür
    g_tables.push_back(std::move(t));
    g_table = &g_tables.back();
}
void apply() {
    const std::string code = g_setting == "system" ? system_language() : g_setting;
    load_table(code);
}

void read_settings() {
    const json j = read_json(settings_path());
    if (j.is_object()) {
        g_setting = j.value("language", std::string("system"));
        g_splash = j.value("splash", true);
    }
    std::error_code ec;
    g_stamp = fs::last_write_time(settings_path(), ec);
}

void save_settings() {
    write_json(settings_path(), {{"language", g_setting}, {"splash", g_splash}});
    std::error_code ec;
    g_stamp = fs::last_write_time(settings_path(), ec);
}
}  // namespace

const std::vector<LanguageInfo>& language_list() {
    static const std::vector<LanguageInfo> list = {
        {"de", "Deutsch", "Almanca"}, {"ar", "العربية", "Arapça", "", true}, {"az", "Azərbaycan dili", "Azerbaycanca"},
        {"bg", "Български", "Bulgarca"}, {"cs", "Čeština", "Çekçe"}, {"zh", "中文", "Çince", "zh"}, {"da", "Dansk", "Danca"},
        {"id", "Bahasa Indonesia", "Endonezce"}, {"fa", "فارسی", "Farsça", "", true}, {"nl", "Nederlands", "Felemenkçe"},
        {"fi", "Suomi", "Fince"}, {"fr", "Français", "Fransızca"}, {"hi", "हिन्दी", "Hintçe", "hi"}, {"he", "עברית", "İbranice", "", true},
        {"en", "English", "İngilizce"}, {"es", "Español", "İspanyolca"}, {"sv", "Svenska", "İsveççe"}, {"it", "Italiano", "İtalyanca"},
        {"ja", "日本語", "Japonca", "ja"}, {"kk", "Қазақ тілі", "Kazakça"}, {"ko", "한국어", "Korece", "ko"}, {"pl", "Polski", "Lehçe"},
        {"hu", "Magyar", "Macarca"}, {"no", "Norsk", "Norveççe"}, {"uz", "Oʻzbekcha", "Özbekçe"}, {"pt", "Português", "Portekizce"},
        {"ro", "Română", "Rumence"}, {"ru", "Русский", "Rusça"}, {"sr", "Српски", "Sırpça"}, {"th", "ไทย", "Tayca", "th"},
        {"tr", "Türkçe", "Türkçe"}, {"uk", "Українська", "Ukraynaca"}, {"ur", "اردو", "Urduca", "", true},
        {"vi", "Tiếng Việt", "Vietnamca"}, {"el", "Ελληνικά", "Yunanca"},
    };
    return list;
}

std::string language_native(const LanguageInfo& l) { return l.rtl ? rtl_visual(l.native) : std::string(l.native); }

const LanguageInfo* language_info(const std::string& code) {
    for (auto& l : language_list()) if (code == l.code) return &l;
    return nullptr;
}

std::string system_language() {
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_TURKISH: return "tr";
    case LANG_GERMAN: return "de";
    case LANG_FRENCH: return "fr";
    case LANG_SPANISH: return "es";
    case LANG_PORTUGUESE: return "pt";
    case LANG_ITALIAN: return "it";
    case LANG_DUTCH: return "nl";
    case LANG_POLISH: return "pl";
    case LANG_RUSSIAN: return "ru";
    case LANG_UKRAINIAN: return "uk";
    case LANG_CZECH: return "cs";
    case LANG_HUNGARIAN: return "hu";
    case LANG_ROMANIAN: return "ro";
    case LANG_BULGARIAN: return "bg";
    case LANG_GREEK: return "el";
    case LANG_SWEDISH: return "sv";
    case LANG_DANISH: return "da";
    case LANG_NORWEGIAN: return "no";
    case LANG_FINNISH: return "fi";
    case LANG_SERBIAN: return "sr";     // Hırvatça da aynı ana kodu kullanır; çeviri Kiril Sırpçadır
    case LANG_AZERI: return "az";
    case LANG_KAZAK: return "kk";
    case LANG_UZBEK: return "uz";
    case LANG_CHINESE: return "zh";
    case LANG_JAPANESE: return "ja";
    case LANG_KOREAN: return "ko";
    case LANG_HINDI: return "hi";
    case LANG_INDONESIAN: return "id";
    case LANG_VIETNAMESE: return "vi";
    case LANG_THAI: return "th";
    case LANG_ARABIC: return "ar";
    case LANG_PERSIAN: return "fa";
    case LANG_HEBREW: return "he";
    case LANG_URDU: return "ur";
    default: return "en";
    }
}

void lang_init() {
    std::lock_guard<std::mutex> g(g_lock);
    read_settings();
    apply();
    g_checked = now();
}

void lang_set_font_hook(void (*hook)()) { g_font_hook = hook; }

void lang_tick() {
    if (g_font_hook) g_font_hook();     // Kare dışında: seçilen dilin yazı tipi gerekiyorsa yüklenir
    if (now() - g_checked < 2) return;
    g_checked = now();
    std::error_code ec;
    const auto stamp = fs::last_write_time(settings_path(), ec);
    if (ec || stamp == g_stamp) return;
    std::lock_guard<std::mutex> g(g_lock);
    read_settings();
    apply();
}

std::string lang_setting() { std::lock_guard<std::mutex> g(g_lock); return g_setting; }
std::string lang_code() { std::lock_guard<std::mutex> g(g_lock); return g_table ? g_table->code : "tr"; }
bool splash_enabled() { std::lock_guard<std::mutex> g(g_lock); return g_splash; }

void lang_set(const std::string& setting) {
    std::lock_guard<std::mutex> g(g_lock);
    g_setting = setting;
    save_settings();
    apply();
}

void splash_set_enabled(bool on) {
    std::lock_guard<std::mutex> g(g_lock);
    g_splash = on;
    save_settings();
}

const char* T(const char* text) {
    if (!text) return text;
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_table || !*text) return text;
    auto it = g_table->text.find(text);     // İşaretçiyle önbellek yok: geçici metinlerin adresi başka metne denk gelebilir
    if (it != g_table->text.end()) return it->second.c_str();
    // Başta simge (Segoe MDL2, UTF-8'de 0xEE ile başlayan 3 bayt) varsa geri kalanı çevrilir
    if (uint8_t(text[0]) == 0xEE && text[1] && text[2] && text[3]) {
        std::string rest = text + 3;
        const size_t hash_at = rest.find("##");
        const std::string label = hash_at == std::string::npos ? rest : rest.substr(0, hash_at);
        auto found = g_table->text.find(label);
        if (found == g_table->text.end()) {     // Simgeden sonraki boşluklar anahtarda olmayabilir
            const size_t lead = label.find_first_not_of(' ');
            if (lead != std::string::npos && lead > 0) {
                auto trimmed = g_table->text.find(label.substr(lead));
                if (trimmed != g_table->text.end()) {
                    auto& made = g_table->text[text];
                    made = std::string(text, 3) + label.substr(0, lead) + trimmed->second + (hash_at == std::string::npos ? "" : rest.substr(hash_at));
                    return made.c_str();
                }
            }
        } else {
            auto& made = g_table->text[text];
            made = std::string(text, 3) + found->second + (hash_at == std::string::npos ? "" : rest.substr(hash_at));
            return made.c_str();
        }
    }
    // "Etiket##kimlik": görünen kısım çevrilir, kimlik aynı kalır (dil değişince açık listeler kapanmaz)
    const char* hash = strstr(text, "##");
    if (hash && hash != text) {
        auto label = g_table->text.find(std::string(text, hash));
        if (label != g_table->text.end()) {
            auto& made = g_table->text[text];       // Düğüm tabanlı harita: değer adresi sabit kalır
            made = label->second + hash;
            return made.c_str();
        }
    }
    return text;
}

std::string T(const std::string& text) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_table) return text;
    auto it = g_table->text.find(text);
    return it == g_table->text.end() ? text : it->second;
}

std::wstring TW(const char* text) { return widen(T(text)); }

std::string TF(const char* format, ...) {
    const char* f = T(format);
    va_list args;
    va_start(args, format);
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), f, args);
    va_end(args);
    return buffer;
}
