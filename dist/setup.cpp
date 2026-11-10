// Kurulum sihirbazı: dosyaları kullanıcının program klasörüne açar, kısayolları ve kaldırma kaydını oluşturur.
// Aynı exe "Uninstall.exe" adıyla kurulum klasörüne kopyalanır ve --kaldir ile programı kaldırır.
// İstenen klasöre kurar. Klasör korumalıysa (Program Files gibi) kendini yönetici olarak yeniden açar;
#include "ui.h"

#include <compressapi.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

namespace {

const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\KayitStudyosu";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const std::wstring kVersionText = widen(build_version());     // Derleme anı: yyyy.aa.gg.ss.dd.ss
const wchar_t* kVersion = kVersionText.c_str();
const wchar_t* kMenuFolder = L"Kayıt Stüdyosu";
const wchar_t* kMarker = L"settings-in-user-folder";

std::wstring self_path() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring default_folder() { return known_folder(FOLDERID_ProgramFiles) + L"\\Recording Studio"; }

bool mutex_exists(const wchar_t* name) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (h) CloseHandle(h);
    return h != nullptr || GetLastError() == ERROR_ACCESS_DENIED;   // Yönetici olarak açık kopya
}

bool apps_running() { return mutex_exists(L"Local\\KayitStudyosu") || mutex_exists(L"Local\\OyunAraclari") || mutex_exists(L"Local\\VideoDuzenleyici"); }

bool windows_ok() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto get = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)};
    return get && get(&v) == 0 && v.dwMajorVersion >= 10 && v.dwBuildNumber >= 19041;   // Windows 10 2004 ve sonrası
}

bool media_ok() {   // Windows "N" sürümlerinde kayıt bileşenleri bulunmaz
    HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m) FreeLibrary(m);
    return m != nullptr;
}

bool dotnet_ok() {  // Sensör yardımcısı için: Windows 10 1803 ve sonrasında zaten vardır
    DWORD release = 0, size = sizeof(release);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full", L"Release", RRF_RT_REG_DWORD,
                        nullptr, &release, &size) == ERROR_SUCCESS && release >= 461808;
}

bool pawnio_ok() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO", 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

int run_hidden(std::wstring command, DWORD wait_ms) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, wait_ms);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return int(code);
}

bool make_shortcut(const std::wstring& link, const std::wstring& target, const std::wstring& args, const std::wstring& folder, const std::wstring& about) {
    ComPtr<IShellLinkW> shell;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
    shell->SetPath(target.c_str());
    shell->SetArguments(args.c_str());
    shell->SetWorkingDirectory(folder.c_str());
    shell->SetDescription(about.c_str());
    shell->SetIconLocation(target.c_str(), 0);
    if (FAILED(shell.As(&file))) return false;
    return SUCCEEDED(file->Save(link.c_str(), TRUE));
}

void set_value(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

void set_dword(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

struct Job {
    std::atomic<float> progress{0};
    std::atomic<bool> done{false}, failed{false};
    std::mutex lock;
    std::string step, error;
    void set_step(const char* text) { std::lock_guard<std::mutex> g(lock); step = text; }
    void fail(const std::string& text) {
        std::lock_guard<std::mutex> g(lock);
        error = text;
        failed = true;
        done = true;
    }
};

// Paketi klasöre açar; dosya adları kaldırma için listeye yazılır
bool unpack(const std::wstring& folder, Job& job, std::string& list) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(100), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
    if (!mem) { job.fail(T("Kurulum paketi okunamadı")); return false; }
    const void* packed = LockResource(mem);
    const SIZE_T packed_size = SizeofResource(nullptr, res);
    DECOMPRESSOR_HANDLE d;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d)) { job.fail(T("Paket açılamadı")); return false; }
    SIZE_T raw_size = 0;
    Decompress(d, packed, packed_size, nullptr, 0, &raw_size);
    std::vector<char> raw(raw_size);
    const BOOL ok = Decompress(d, packed, packed_size, raw.data(), raw.size(), &raw_size);
    CloseDecompressor(d);
    if (!ok) { job.fail(T("Paket bozuk. Kurulum dosyasını yeniden indirin.")); return false; }
    size_t at = 0;
    while (at < raw_size) {
        uint32_t name_size;
        uint64_t data_size;
        memcpy(&name_size, raw.data() + at, 4);
        at += 4;
        const std::string name(raw.data() + at, name_size);
        at += name_size;
        memcpy(&data_size, raw.data() + at, 8);
        at += 8;
        const fs::path target = fs::path(folder) / widen(name);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        // Antivirüs taraması dosyayı kısa süre kilitleyebilir: hemen vazgeçme
        bool written = false;
        for (int attempt = 0; attempt < 40 && !written; attempt++) {
            if (attempt) Sleep(250);
            std::ofstream f(target, std::ios::binary | std::ios::trunc);
            f.write(raw.data() + at, std::streamsize(data_size));
            f.close();
            written = bool(f);
        }
        if (!written) { job.fail("Dosya yazılamadı: " + name); return false; }
        at += size_t(data_size);
        list += name + "\n";
        job.progress = 0.05f + 0.75f * float(at) / float(raw_size);
    }
    return true;
}

void install(std::wstring folder, bool desktop, bool pawnio, bool protected_folder, Job& job) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    job.set_step(T("Dosyalar kopyalanıyor"));
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    std::string list;
    if (!unpack(folder, job, list)) { CoUninitialize(); return; }

    const std::wstring remover = folder + L"\\Uninstall.exe";
    if (_wcsicmp(self_path().c_str(), remover.c_str()) != 0 && !CopyFileW(self_path().c_str(), remover.c_str(), FALSE)) {
        job.fail(T("Kaldırma dosyası oluşturulamadı"));
        CoUninitialize();
        return;
    }
    list += "Uninstall.exe\n";
    if (protected_folder) {     // Programlar ayarlarını AppData'ya yazsın
        std::ofstream marker(fs::path(folder) / kMarker, std::ios::binary);
        list += narrow(kMarker) + "\n";
    }
    { std::ofstream f(fs::path(folder) / L"kurulu-dosyalar.txt", std::ios::binary); f << list; }

    job.set_step(T("Kısayollar oluşturuluyor"));
    job.progress = 0.85f;
    const std::wstring record = folder + L"\\Recording Studio.exe", tools = folder + L"\\Game Tools.exe", editor = folder + L"\\Video Editor.exe";
    const std::wstring menu = known_folder(FOLDERID_Programs) + L"\\" + kMenuFolder;
    SHCreateDirectoryExW(nullptr, menu.c_str(), nullptr);
    make_shortcut(menu + L"\\Kayıt Stüdyosu.lnk", record, L"", folder, L"Ekran kaydı ve canlı yayın");
    make_shortcut(menu + L"\\Oyun Araçları.lnk", tools, L"", folder, L"Crosshair ve FPS göstergesi");
    make_shortcut(menu + L"\\Video Düzenleyici.lnk", editor, L"", folder, L"Video kurgu programı");
    make_shortcut(menu + L"\\Kayıt Stüdyosu'nu Kaldır.lnk", remover, L"--kaldir", folder, L"Programı kaldırır");
    if (desktop) {
        const std::wstring screen = known_folder(FOLDERID_Desktop);
        make_shortcut(screen + L"\\Kayıt Stüdyosu.lnk", record, L"", folder, L"Ekran kaydı ve canlı yayın");
        make_shortcut(screen + L"\\Oyun Araçları.lnk", tools, L"", folder, L"Crosshair ve FPS göstergesi");
        make_shortcut(screen + L"\\Video Düzenleyici.lnk", editor, L"", folder, L"Video kurgu programı");
    }

    job.set_step(T("Windows'a kaydediliyor"));
    job.progress = 0.9f;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        set_value(key, L"DisplayName", L"Kayıt Stüdyosu");
        set_value(key, L"DisplayVersion", kVersion);
        set_value(key, L"Publisher", L"Ali Ceviz");
        set_value(key, L"InstallLocation", folder);
        set_value(key, L"DisplayIcon", record);
        set_value(key, L"UninstallString", L"\"" + remover + L"\" --kaldir");
        set_value(key, L"QuietUninstallString", L"\"" + remover + L"\" --kaldir --sessiz");
        set_dword(key, L"NoModify", 1);
        set_dword(key, L"NoRepair", 1);
        std::error_code ec;
        uintmax_t total = 0;
        for (auto& e : fs::recursive_directory_iterator(folder, ec))
            if (e.is_regular_file(ec)) total += e.file_size(ec);
        set_dword(key, L"EstimatedSize", DWORD(total / 1024));
        RegCloseKey(key);
    }
    if (pawnio && !pawnio_ok()) {
        job.set_step(T("Sıcaklık sürücüsü indiriliyor (Windows onay isteyebilir)"));
        job.progress = 0.93f;
        run_hidden(L"winget install -e --id namazso.PawnIO --silent --accept-package-agreements --accept-source-agreements", 300000);
    }
    job.progress = 1;
    job.done = true;
    CoUninitialize();
}

void remove_empty(const std::wstring& folder) {
    std::error_code ec;
    if (fs::is_directory(folder, ec) && fs::is_empty(folder, ec)) fs::remove(folder, ec);
}

// Kurulum korumalı klasördeyse ayarlar AppData'dadır
fs::path settings_of(const fs::path& target) {
    if (!fs::exists(target / kMarker)) return target;
    return fs::path(known_folder(FOLDERID_RoamingAppData)) / L"Recording Studio";
}

void uninstall(const fs::path& folder, bool remove_settings, Job& job) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path settings = settings_of(folder);     // İşaret dosyası silinmeden önce öğrenilir
    job.set_step(T("Otomatik başlatma kapatılıyor"));
    if (run_hidden(L"schtasks /Query /TN \"Game Tools\"", 15000) == 0) {
        // Bu görevi yalnız yönetici silebilir: Oyun Araçları kendi görevini siler
        const std::wstring tools = (folder / L"Game Tools.exe").wstring();
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = tools.c_str();
        info.lpParameters = L"--gorev-sil";
        info.nShow = SW_HIDE;
        if (ShellExecuteExW(&info) && info.hProcess) {
            WaitForSingleObject(info.hProcess, 20000);
            CloseHandle(info.hProcess);
        }
    }
    HKEY run;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, L"Recording Studio");
        RegDeleteValueW(run, L"Video Editor");
        RegCloseKey(run);
    }
    {   // Sanal kamera kurulduysa: kaldırma yönetici izni ister, Kayıt Stüdyosu bunu kendisi yapar
        HKEY cam;
        const std::wstring record = (folder / L"Recording Studio.exe").wstring();
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{6E3B0F7A-2C4D-4F1B-9A61-5D2E8C7B4A10}", 0, KEY_READ, &cam) == ERROR_SUCCESS) {
            RegCloseKey(cam);
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOCLOSEPROCESS;
            info.lpVerb = L"runas";
            info.lpFile = record.c_str();
            info.lpParameters = L"--vcam-uninstall";
            info.nShow = SW_HIDE;
            if (fs::exists(record) && ShellExecuteExW(&info) && info.hProcess) {
                WaitForSingleObject(info.hProcess, 20000);
                CloseHandle(info.hProcess);
            }
        }
    }
    job.progress = 0.2f;
    job.set_step(T("Kısayollar siliniyor"));
    std::error_code ec;
    fs::remove_all(fs::path(known_folder(FOLDERID_Programs)) / kMenuFolder, ec);
    const fs::path screen = known_folder(FOLDERID_Desktop);
    fs::remove(screen / L"Kayıt Stüdyosu.lnk", ec);
    fs::remove(screen / L"Oyun Araçları.lnk", ec);
    fs::remove(screen / L"Video Düzenleyici.lnk", ec);
    job.progress = 0.4f;
    job.set_step(T("Dosyalar siliniyor"));
    // Sadece kurulumun yazdığı dosyalar silinir; klasörde başka dosya varsa ona dokunulmaz
    std::ifstream f(folder / L"kurulu-dosyalar.txt", std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        for (int attempt = 0; attempt < 40; attempt++) {    // Uninstall.exe yeni kapanmış olabilir
            if (fs::remove(folder / widen(line), ec) || !fs::exists(folder / widen(line), ec)) break;
            Sleep(250);
        }
    }
    f.close();
    fs::remove(folder / L"kurulu-dosyalar.txt", ec);
    // Eski sürümlerin kurduğu, artık pakette olmayan klasörler (listede yer almadıkları için ayrıca silinir)
    fs::remove_all(folder / L"manuals", ec);
    for (const wchar_t* old : {L"Kayit Studyosu.exe", L"Oyun Araclari.exe", L"Video Duzenleyici.exe", L"hazir-profiller.json"}) fs::remove(folder / old, ec);
    // Boş kalan alt klasörler, içten dışa doğru
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) dirs.push_back(it->path());
    std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
    for (auto& d : dirs) remove_empty(d.wstring());
    if (remove_settings)
        for (auto name : {L"settings.json", L"game-settings.json", L"fps-settings.json", L"crosshair-profiles.json", L"recording-profiles.json", L"editor-autosave.json",
                          L"layout.json", L"language.json", L"benchmarks.json"}) fs::remove(settings / name, ec);
    if (remove_settings)    // Profiller ayardır; şablonlar ve donmuş kareler kullanıcının işi olduğu için kalır
        for (auto dir : {L"Profiles", L"Scene Collections"}) fs::remove_all(settings / dir, ec);
    if (remove_settings && settings != folder) remove_empty(settings.wstring());
    remove_empty(folder.wstring());
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    job.progress = 1;
    job.done = true;
    CoUninitialize();
}

// Çalışan exe kendini silemez: gizli bir komut, Uninstall.exe kapanınca dosyayı ve boş klasörü siler
void schedule_self_delete() {
    const fs::path self = self_path();
    const std::wstring file = self.wstring(), dir = self.parent_path().wstring();
    std::wstring command = L"cmd.exe /c for /l %i in (1,1,120) do @(del /f /q \"" + file + L"\" 2>nul & if not exist \"" + file +
                           L"\" (rmdir \"" + dir + L"\" 2>nul & exit) & ping -n 2 127.0.0.1 >nul)";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, L"C:\\", &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// Kendini yönetici olarak çalıştırır. Çıkış kodu döner; onay verilmezse -1
int run_elevated(const std::wstring& args) {
    const std::wstring self = self_path();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return -1;
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return int(code);
}

bool removal_needs_admin(const fs::path& target) { return !IsUserAnAdmin() && fs::exists(target / kMarker); }

// Kurulum klasöründen çalışan exe o klasörü silemez: kendini geçici klasöre kopyalayıp oradan devam eder
int restart_from_temp(const fs::path& target, bool silent, bool wipe, bool& restarted) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path folder = fs::path(temp) / L"KayitStudyosuKaldir";
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path copy = folder / L"Uninstall.exe";
    if (!CopyFileW(self_path().c_str(), copy.c_str(), FALSE)) return 1;
    std::wstring command = L"\"" + copy.wstring() + L"\" --kaldir --hedef \"" + target.wstring() + L"\"";
    if (silent) command += L" --sessiz";
    if (wipe) command += L" --ayarlari-sil";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &si, &pi)) return 1;
    restarted = true;
    CloseHandle(pi.hProcess);   // Beklemeden kapanır: bu exe açık kaldıkça kendi klasörü silinemez
    CloseHandle(pi.hThread);
    return 0;
}

std::wstring pick_folder(HWND owner) {
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

// Program Files gibi korumalı klasörde ayarlar yazılamaz: kurulmadan önce denenir
bool folder_writable(const std::wstring& folder) {
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const std::wstring test = folder + L"\\.yazma-denemesi";
    HANDLE h = CreateFileW(test.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    mouse_position_fix(msg, lp);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_KEYMENU) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    lang_init();     // Kurulumda: Windows dili (çeviriler exe içine gömülü)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Uninstall.exe doğrudan açılınca da kaldırma ekranı gelsin; aynı exe kurulum adıyla çalışınca kurulum yapar
    bool remove_mode = _wcsicmp(fs::path(self_path()).filename().c_str(), L"Uninstall.exe") == 0;
    bool silent = false, wipe = false, desktop_arg = false, pawnio_arg = false, protected_arg = false, child = false;
    std::wstring folder = default_folder(), target_arg;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--kaldir")) remove_mode = true;
        else if (!wcscmp(argv[i], L"--sessiz")) silent = true;
        else if (!wcscmp(argv[i], L"--ayarlari-sil")) wipe = true;
        else if (!wcscmp(argv[i], L"--alt")) child = true;
        else if (!wcscmp(argv[i], L"--hedef") && i + 1 < argc) target_arg = argv[++i];
        else if (!wcscmp(argv[i], L"--masaustu")) desktop_arg = true;
        else if (!wcscmp(argv[i], L"--pawnio")) pawnio_arg = true;
        else if (!wcscmp(argv[i], L"--korumali")) protected_arg = true;
        else if (!wcscmp(argv[i], L"--klasor") && i + 1 < argc) folder = argv[++i];
    }
    LocalFree(argv);

    const fs::path target = target_arg.empty() ? fs::path(self_path()).parent_path() : fs::path(target_arg);
    if (remove_mode && target_arg.empty()) {
        bool restarted = false;
        const int code = restart_from_temp(target, silent, wipe, restarted);
        if (restarted) return code;
        if (silent) return 1;
    }
    auto removal_args = [&](bool settings) {
        return L"--kaldir --sessiz --alt --hedef \"" + target.wstring() + L"\"" + std::wstring(settings ? L" --ayarlari-sil" : L"");
    };

    if (silent) {   // Komut satırından: arayüz yok, sonuç çıkış kodunda
        if (apps_running()) return 3;
        Job job;
        if (remove_mode) {
            if (removal_needs_admin(target)) {
                const int code = run_elevated(removal_args(wipe));
                schedule_self_delete();
                return code < 0 ? 5 : code;
            }
            uninstall(target, wipe, job);
            if (!child) schedule_self_delete();
        } else if (!folder_writable(folder)) {
            return 4;
        } else {
            install(folder, desktop_arg, pawnio_arg, protected_arg, job);
        }
        return job.failed ? 1 : 0;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"KayitStudyosuKurulum";
    RegisterClassExW(&wc);
    const float scale = GetDpiForSystem() / 96.f;
    const int w = int(680 * scale), h = int(500 * scale);
    HWND hwnd = CreateWindowW(wc.lpszClassName, remove_mode ? TW("Creator Suite'i Kaldır").c_str() : TW("Creator Suite Kurulumu").c_str(),
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                              (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &device, nullptr, &ctx))) {
        MessageBoxW(hwnd, TW("Pencere çizimi başlatılamadı").c_str(), TW("Creator Suite Kurulumu").c_str(), MB_ICONERROR);
        return 1;
    }
    ComPtr<ID3D11Texture2D> back;
    swap->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
    setup_imgui(hwnd);
    theme_start(hwnd, 0, 2);     // Kurulum Windows'un temasını izler
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device.Get(), ctx.Get());
    ShowWindow(hwnd, SW_SHOW);

    enum { Legal, Start, Working, Finished } page = remove_mode ? Start : Legal;
    bool accepted = false;
    Job job;
    std::thread worker;
    bool desktop = true, want_pawnio = !pawnio_ok(), launch_record = true, launch_tools = false, launch_editor = false, remove_settings = false;
    const bool windows_good = windows_ok(), media_good = media_ok(), dotnet_good = dotnet_ok(), pawnio_present = pawnio_ok();
    std::string notice;
    char path_text[512];
    strncpy_s(path_text, narrow(folder).c_str(), _TRUNCATE);

    MSG msg{};
    bool quit = false;
    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (page == Working && job.done) {
            worker.join();
            page = Finished;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        theme_tick();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("kurulum", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
        ImGui::PushFont(nullptr, 26.f);
        ImGui::TextUnformatted(remove_mode ? "Creator Suite'i Kaldır" : "Creator Suite Kurulumu");
        ImGui::PopFont();
        ImGui::Spacing();
        const float bottom = ImGui::GetWindowHeight() - 54 * scale;
        auto right_button = [&](const char* label) {
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 174 * scale, bottom));
            return ImGui::Button(label, ImVec2(160 * scale, 38 * scale));
        };
        if (page == Legal) {
            ImGui::TextDisabled("Devam etmeden önce lisans sözleşmesini ve gizlilik bilgilerini okuyun.");
            ImGui::Spacing();
            legal_tabs(bottom - ImGui::GetCursorPosY() - 40 * scale);
            ImGui::Spacing();
            ImGui::Checkbox("Lisans sözleşmesini okudum ve kabul ediyorum", &accepted);
            ImGui::BeginDisabled(!accepted);
            if (right_button("İleri")) page = Start;
            ImGui::EndDisabled();
        } else if (page == Start) {
            if (remove_mode) {
                ImGui::TextWrapped("Kayıt Stüdyosu, Oyun Araçları ve Video Düzenleyici bu bilgisayardan kaldırılacak. Yaptığınız kayıtlar silinmez.");
                ImGui::Spacing();
                ImGui::Checkbox("Ayarlarımı, sahnelerimi ve crosshair profillerimi de sil", &remove_settings);
            } else {
                ImGui::TextWrapped("Kayıt Stüdyosu (ekran kaydı ve canlı yayın), Oyun Araçları (crosshair ve FPS göstergesi) ve Video Düzenleyici kurulacak.");
                ImGui::Spacing();
                ImGui::TextUnformatted("Kurulum klasörü");
                ImGui::TextDisabled("İstediğiniz klasörü seçebilirsiniz. Korumalı bir klasörse Windows yönetici onayı ister.");
                ImGui::SetNextItemWidth(-110 * scale);
                ImGui::InputText("##klasor", path_text, sizeof(path_text));
                ImGui::SameLine();
                if (ImGui::Button("Değiştir...", ImVec2(-1, 0))) {
                    const std::wstring chosen = pick_folder(hwnd);
                    if (!chosen.empty()) {
                        const bool named = fs::path(chosen).filename() == L"Recording Studio";
                        strncpy_s(path_text, narrow(named ? chosen : (fs::path(chosen) / L"Recording Studio").wstring()).c_str(), _TRUNCATE);
                    }
                }
                ImGui::Spacing();
                ImGui::Checkbox("Masaüstüne kısayol ekle", &desktop);
                if (pawnio_present) {
                    ImGui::TextDisabled("İşlemci sıcaklığı sürücüsü (PawnIO) zaten kurulu.");
                } else {
                    ImGui::Checkbox("İşlemci sıcaklığı sürücüsünü de kur (PawnIO, internetten iner)", &want_pawnio);
                    ImGui::TextDisabled("Sadece FPS göstergesinde işlemci sıcaklığı için gerekir. Windows onay isteyebilir.");
                }
                ImGui::Spacing();
                if (!windows_good) ImGui::TextColored(theme_error(), "Windows 10 sürüm 2004 ya da daha yenisi gerekir. Bazı özellikler çalışmayabilir.");
                if (!media_good) ImGui::TextColored(theme_error(), "Bu Windows'ta Medya Özellik Paketi yok: kayıt ve yayın çalışmaz. Ayarlar > İsteğe bağlı özellikler'den ekleyin.");
                if (!dotnet_good) ImGui::TextColored(theme_warn(), ".NET Framework 4.7.2 bulunamadı: FPS göstergesinde sıcaklıklar görünmez.");
            }
            if (!notice.empty()) ImGui::TextColored(theme_error(), "%s", notice.c_str());
            if (!remove_mode) {
                ImGui::SetCursorPos(ImVec2(14 * scale, bottom));
                if (ImGui::Button("Geri", ImVec2(120 * scale, 38 * scale))) page = Legal;
            }
            if (right_button(remove_mode ? "Kaldır" : "Kur")) {
                folder = widen(path_text);
                while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L' ')) folder.pop_back();
                std::error_code ec;
                const fs::path chosen(folder);
                if (remove_mode) {
                    if (apps_running()) {
                        notice = "Önce Kayıt Stüdyosu ve Oyun Araçları'nı kapatın (simge menüsünden Çıkış).";
                    } else {
                        notice.clear();
                        page = Working;
                        worker = std::thread([&] {
                            if (removal_needs_admin(target)) {
                                job.set_step(T("Windows yönetici onayı bekleniyor"));
                                const int code = run_elevated(removal_args(remove_settings));
                                if (code < 0) job.fail(T("Yönetici onayı verilmedi. Program kaldırılmadı."));
                                else if (code != 0) job.fail(T("Program kaldırılamadı."));
                                else { job.progress = 1; job.done = true; }
                            } else {
                                uninstall(target, remove_settings, job);
                            }
                        });
                    }
                } else if (apps_running()) {
                    notice = "Önce Kayıt Stüdyosu ve Oyun Araçları'nı kapatın (simge menüsünden Çıkış).";
                } else if (!chosen.is_absolute() || !chosen.has_root_directory()) {
                    notice = "Geçerli bir klasör yazın. Örnek: D:\\Programlar\\Recording Studio";
                } else if (GetDriveTypeW(chosen.root_path().c_str()) == DRIVE_NO_ROOT_DIR) {
                    notice = "Bu sürücü bulunamadı. Başka bir klasör seçin.";
                } else {
                    notice.clear();
                    page = Working;
                    const bool plain = folder_writable(folder);
                    worker = std::thread([&, plain] {
                        if (plain) {
                            install(folder, desktop, want_pawnio, false, job);
                            return;
                        }
                        job.set_step(T("Windows yönetici onayı bekleniyor"));
                        job.progress = 0.1f;
                        std::wstring args = L"--sessiz --korumali --klasor \"" + folder + L"\"";
                        if (desktop) args += L" --masaustu";
                        if (want_pawnio) args += L" --pawnio";
                        const int code = run_elevated(args);
                        if (code < 0) job.fail(T("Yönetici onayı verilmedi. Başka bir klasör seçebilirsiniz."));
                        else if (code != 0) job.fail(T("Bu klasöre kurulamadı."));
                        else { job.progress = 1; job.done = true; }
                    });
                }
            }
        } else if (page == Working) {
            std::string step;
            {
                std::lock_guard<std::mutex> g(job.lock);
                step = job.step;
            }
            ImGui::Spacing();
            ImGui::TextUnformatted(step.c_str());
            ImGui::ProgressBar(job.progress, ImVec2(-1, 24 * scale), "");
        } else {
            std::string error;
            {
                std::lock_guard<std::mutex> g(job.lock);
                error = job.error;
            }
            if (job.failed) {
                ImGui::TextColored(theme_error(), remove_mode ? "Kaldırma tamamlanamadı:" : "Kurulum tamamlanamadı:");
                ImGui::TextWrapped("%s", error.c_str());
            } else if (remove_mode) {
                ImGui::TextUnformatted("Program kaldırıldı.");
            } else {
                ImGui::TextUnformatted("Kurulum tamamlandı.");
                ImGui::TextDisabled("Programları Başlat menüsünden ya da masaüstünden açabilirsiniz.");
                ImGui::Spacing();
                if (want_pawnio && !pawnio_present && !pawnio_ok())
                    ImGui::TextWrapped("Sıcaklık sürücüsü kurulamadı. Diğer her şey çalışır; işlemci sıcaklığı için FPS Ölçer sayfasındaki İndir düğmesini kullanabilirsiniz.");
                ImGui::Checkbox("Kayıt Stüdyosu'nu başlat", &launch_record);
                ImGui::Checkbox("Oyun Araçları'nı başlat", &launch_tools);
                ImGui::Checkbox("Video Düzenleyici'yi başlat", &launch_editor);
            }
            if (right_button("Bitir")) {
                bool opened = true;
                if (!job.failed && !remove_mode) {
                    auto open_app = [&](const wchar_t* name) {
                        opened &= INT_PTR(ShellExecuteW(nullptr, L"open", (folder + L"\\" + name).c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL)) > 32;
                    };
                    if (launch_record) open_app(L"Recording Studio.exe");
                    if (launch_tools) open_app(L"Game Tools.exe");
                    if (launch_editor) open_app(L"Video Editor.exe");
                    if (!opened) MessageBoxW(hwnd, TW("Kurulum tamamlandı ama program başlatılamadı. Başlat menüsünden açabilirsiniz.").c_str(), TW("Kayıt Stüdyosu").c_str(), MB_ICONINFORMATION);
                }
                PostQuitMessage(0);
            }
        }
        ImGui::End();
        ImGui::Render();
        ID3D11RenderTargetView* target = rtv.Get();
        ctx->OMSetRenderTargets(1, &target, nullptr);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        const float clear[4] = {bg.x, bg.y, bg.z, 1};
        ctx->ClearRenderTargetView(target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap->Present(1, 0);
    }
    if (worker.joinable()) worker.join();
    if (remove_mode && !target_arg.empty()) schedule_self_delete();    // Geçici kopya, vazgeçilse de kalmasın
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CoUninitialize();
    return 0;
}
