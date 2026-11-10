// Kayıt Stüdyosu ve Oyun Araçları'nın paylaştığı küçük yardımcılar.
#include "common.h"

#include <shlobj.h>

#include <filesystem>
#include <fstream>

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

double now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}

// Korumalı klasöre (Program Files gibi) kurulduysa yanında bu işaret dosyası olur ve ayarlar kullanıcının
// AppData klasörüne yazılır. Yazma denemesiyle karar verilmez: yönetici olarak açılan kopya da aynı ayarları görsün.
// Eski Türkçe adlı ayar dosyaları ve klasörler yeni adlarına taşınır (yeni ad varsa dokunulmaz)
static void migrate_names(const std::wstring& folder) {
    namespace fs = std::filesystem;
    const wchar_t* names[][2] = {
        {L"ayarlar.json", L"settings.json"}, {L"oyun-ayarlar.json", L"game-settings.json"}, {L"fps-ayarlar.json", L"fps-settings.json"},
        {L"crosshair-profiller.json", L"crosshair-profiles.json"}, {L"kayit-profiller.json", L"recording-profiles.json"},
        {L"duzenle-otomatik.json", L"editor-autosave.json"}, {L"dil.json", L"language.json"},
        {L"Sablonlar", L"Templates"}, {L"Donmus kareler", L"Freeze Frames"},
    };
    std::error_code ec;
    for (auto& n : names) {
        const fs::path from = fs::path(folder) / n[0], to = fs::path(folder) / n[1];
        if (fs::exists(from, ec) && !fs::exists(to, ec)) fs::rename(from, to, ec);
    }
}

std::wstring data_dir() {
    static const std::wstring folder = [] {
        const std::wstring base = exe_dir();
        std::wstring out = base;
        if (GetFileAttributesW((base + L"\\settings-in-user-folder").c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW((base + L"\\ayarlar-kullanici-klasorunde").c_str()) != INVALID_FILE_ATTRIBUTES) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
                out = std::wstring(path) + L"\\Recording Studio";
                const std::wstring old = std::wstring(path) + L"\\Kayit Studyosu";     // Eski adlı ayar klasörü
                if (GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES)
                    MoveFileW(old.c_str(), out.c_str());
            }
            CoTaskMemFree(path);
            SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
        }
        migrate_names(out);
        return out;
    }();
    return folder;
}

std::wstring videos_dir() {
    PWSTR path = nullptr;
    std::wstring out = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

json read_json(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) return nullptr;
    return json::parse(f, nullptr, false);
}

void write_json(const std::wstring& path, const json& data) {
    {
        std::ofstream f(path + L".tmp");
        f << data.dump(2);
    }
    MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

const char* build_version() {
    static const std::string text = [] {
        const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* date = __DATE__;    // "Nov  9 2026"
        const int month = int((strstr(months, std::string(date, 3).c_str()) - months) / 3) + 1;
        const char* time = __TIME__;    // "14:32:05"
        char out[32];
        sprintf_s(out, "%s.%02d.%02d.%.2s.%.2s.%.2s", date + 7, month, atoi(date + 4), time, time + 3, time + 6);
        return std::string(out);
    }();
    return text.c_str();
}
