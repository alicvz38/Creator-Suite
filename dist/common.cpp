// Kayıt Stüdyosu ve Oyun Araçları'nın paylaştığı küçük yardımcılar.
#include "common.h"

#include <shlobj.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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
        f << data.dump(2, ' ', false, json::error_handler_t::replace);     // Bozuk UTF-8 metin dosyayı yazdırmamazlık etmesin
    }
    MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// --- Hata kaydı ve çökme koruması ---

namespace {
std::mutex g_log_lock;
std::string g_last_log;
double g_last_log_at = -100;
std::atomic<bool> g_dumped{false};

std::wstring exe_stem() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).stem().wstring();
}

// dbghelp çalışma anında yüklenir: her programın bağlantısına kütüphane eklemek gerekmez
void write_dump(EXCEPTION_POINTERS* info) {
    if (g_dumped.exchange(true)) return;
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (!dbghelp) return;
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
    auto write = reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (!write) return;
    const std::wstring folder = data_dir() + L"\\Crash";
    CreateDirectoryW(folder.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE file = CreateFileW((folder + L"\\" + exe_stem() + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    struct { DWORD thread; EXCEPTION_POINTERS* pointers; BOOL client; } ex{GetCurrentThreadId(), info, FALSE};
    const int kind = 0x00000001 | 0x00001000;      // MiniDumpWithDataSegs | MiniDumpWithThreadInfo
    write(GetCurrentProcess(), GetCurrentProcessId(), file, kind, info ? &ex : nullptr, nullptr, nullptr);
    CloseHandle(file);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* info) {
    char text[96];
    sprintf_s(text, "Çökme: kod 0x%08lX, adres %p", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    log_error(text);
    write_dump(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {      // Yakalanmayan C++ istisnası: yığın henüz açılmadı, döküm atıldığı yeri gösterir
    std::string what = "bilinmeyen";
    if (auto e = std::current_exception()) {
        try { std::rethrow_exception(e); } catch (const std::exception& x) { what = x.what(); } catch (...) {}
    }
    log_error("Yakalanmayan hata: " + what);
    write_dump(nullptr);
    _exit(3);
}

void on_abort(int) {       // abort() ve başarısız assert
    log_error("Program abort ile kapandı");
    write_dump(nullptr);
}
}  // namespace

void log_error(const std::string& text) {
    std::lock_guard<std::mutex> g(g_log_lock);
    if (text == g_last_log && now() - g_last_log_at < 10) return;     // Her turda tekrarlayan hata dosyayı şişirmesin
    g_last_log = text;
    g_last_log_at = now();
    const std::wstring path = data_dir() + L"\\error-log.txt";
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow > 512 * 1024) DeleteFileW(path.c_str());
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64];
    sprintf_s(stamp, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string line = stamp + narrow(exe_stem()) + " " + build_version() + ": " + text + "\r\n";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line.data(), DWORD(line.size()), &written, nullptr);
    CloseHandle(file);
}

void json_check_failed(const char* expr, const char* file, int line) {
    const char* base = strrchr(file, '\\');
    const std::string text = std::string("json denetimi: ") + expr + " (" + (base ? base + 1 : file) + ":" + std::to_string(line) + ")";
    log_error(text);
    throw std::logic_error(text);
}

void install_crash_guard() {
    SetUnhandledExceptionFilter(on_unhandled);
    std::set_terminate(on_terminate);
    signal(SIGABRT, on_abort);
    _set_error_mode(_OUT_TO_STDERR);        // assert kutusu oyunun arkasında açılıp tuşla "Durdur"a basılmasın
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
