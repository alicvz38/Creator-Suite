#include "fps_meter.h"
#include "language.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>

// --- FPS ölçümü: ekrana her yeni kare çizildiğinde Windows haber verir; görüntü kopyalanmaz, sadece sayılır ---

static bool is_primary_output(IDXGIOutput* output) {
    DXGI_OUTPUT_DESC d;
    if (FAILED(output->GetDesc(&d))) return false;
    const RECT& r = d.DesktopCoordinates;
    return d.AttachedToDesktop && r.left <= 0 && 0 < r.right && r.top <= 0 && 0 < r.bottom;
}

void FpsMeter::start() {
    if (running()) return;
    halt_ = false;
    last_frames_ = frames_;
    last_time_ = now();
    thread_ = std::thread([this] { guarded("FPS ölçer", [this] { run(); }); });
}

void FpsMeter::stop() {
    if (!running()) return;
    halt_ = true;
    thread_.join();
    ok_ = false;
}

double FpsMeter::read() {
    const double t = now();
    const long long frames = frames_;
    const double fps = t > last_time_ ? (frames - last_frames_) / (t - last_time_) : 0;
    last_frames_ = frames;
    last_time_ = t;
    return ok_ ? fps : -1;
}

void FpsMeter::run() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    while (!halt_) {
        // Birincil ekranı süren ekran kartını bul (hibrit dizüstülerde ilk kart olmayabilir)
        ComPtr<ID3D11Device> device;
        ComPtr<IDXGIOutputDuplication> dup;
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT a = 0; !dup && SUCCEEDED(factory->EnumAdapters1(a, &adapter)); a++) {
            ComPtr<IDXGIOutput> output;
            for (UINT i = 0; !dup && SUCCEEDED(adapter->EnumOutputs(i, &output)); i++) {
                ComPtr<IDXGIOutput1> output1;
                if (is_primary_output(output.Get()) && SUCCEEDED(output.As(&output1)) &&
                    SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                                &device, nullptr, nullptr)))
                    output1->DuplicateOutput(device.Get(), &dup);
                output.Reset();
            }
            adapter.Reset();
        }
        if (!dup) {   // UAC ekranı, ekran modu değişimi: biraz sonra yeniden dene
            ok_ = false;
            for (int i = 0; i < 10 && !halt_; i++) Sleep(100);
            continue;
        }
        ok_ = true;
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        LONGLONG last_present = 0;
        while (!halt_) {
            DXGI_OUTDUPL_FRAME_INFO info;
            ComPtr<IDXGIResource> res;
            const HRESULT hr = dup->AcquireNextFrame(100, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) break;
            if (info.LastPresentTime.QuadPart) {      // Sadece fare hareketiyse sayma
                frames_ += info.AccumulatedFrames;
                if (last_present && info.AccumulatedFrames > 0) {
                    const double dt = double(info.LastPresentTime.QuadPart - last_present) / double(frequency.QuadPart);
                    if (dt > 0 && dt < 2) add(now(), dt * 1000 / info.AccumulatedFrames, int(info.AccumulatedFrames));
                }
                last_present = info.LastPresentTime.QuadPart;
            }
            dup->ReleaseFrame();
        }
    }
    ok_ = false;
}

void FpsMeter::add(double t, double ms, int frames) {
    std::lock_guard<std::mutex> g(lock_);
    recent_.push_back({t, ms, frames});
    while (!recent_.empty() && recent_.front().t < t - 120) recent_.pop_front();
    if (session_on_) session_.push_back({t, ms, frames});
}

std::vector<FrameSample> FpsMeter::recent(double seconds) {
    std::lock_guard<std::mutex> g(lock_);
    const double from = now() - seconds;
    std::vector<FrameSample> out;
    for (auto& s : recent_) if (s.t >= from) out.push_back(s);
    return out;
}

void FpsMeter::session_begin() {
    std::lock_guard<std::mutex> g(lock_);
    session_.clear();
    session_on_ = true;
}

std::vector<FrameSample> FpsMeter::session_end() {
    std::lock_guard<std::mutex> g(lock_);
    session_on_ = false;
    return std::move(session_);
}

FrameStats frame_stats(const std::vector<FrameSample>& samples) {
    FrameStats st;
    double frames = 0, time = 0;
    std::vector<std::pair<double, int>> sorted;     // Kare süresi, kare sayısı; en yavaştan hızlıya
    for (auto& s : samples) {
        frames += s.frames;
        time += s.ms * s.frames;
        sorted.push_back({s.ms, s.frames});
    }
    if (frames < 1 || time <= 0) return st;
    st.avg = frames * 1000 / time;
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.first > b.first; });
    auto low = [&](double share) {      // En yavaş "share" oranındaki karelerin ortalama FPS'i
        const double want = std::max(1.0, frames * share);
        double n = 0, ms = 0;
        for (auto& [t, c] : sorted) {
            const double take = std::min(double(c), want - n);
            n += take;
            ms += t * take;
            if (n >= want) break;
        }
        return n > 0 && ms > 0 ? n * 1000 / ms : -1;
    };
    st.low1 = low(0.01);
    st.low01 = low(0.001);
    st.min = 1000 / sorted.front().first;
    st.max = 1000 / sorted.back().first;
    return st;
}

// --- Ping ---

void Pinger::start(const std::string& host) {
    if (running() && host == host_) return;
    stop();
    halt_ = false;
    host_ = host;
    {
        std::lock_guard<std::mutex> g(lock_);
        results_.clear();
        error_.clear();
    }
    thread_ = std::thread([this, host] { guarded("ping", [&] { run(host); }); });
}

void Pinger::stop() {
    halt_ = true;
    if (thread_.joinable()) thread_.join();
}

json Pinger::data() {
    std::lock_guard<std::mutex> g(lock_);
    json out = {{"host", host_}};
    if (results_.empty()) return out;
    int lost = 0;
    for (int r : results_) lost += r < 0;
    out["loss"] = 100.0 * lost / results_.size();
    for (auto it = results_.rbegin(); it != results_.rend(); ++it)
        if (*it >= 0) { out["ms"] = *it; break; }
    if (results_.back() < 0 && results_.size() >= 3 && lost == int(results_.size())) out.erase("ms");     // Hiç ulaşılamıyor
    return out;
}

void Pinger::run(std::string host) {
    static const int wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }();
    (void)wsa;
    IN_ADDR address{};
    double next_resolve = 0;
    HANDLE icmp = IcmpCreateFile();
    char payload[32] = "RecordingStudioPing";
    std::vector<uint8_t> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 64);
    while (!halt_) {
        if (now() >= next_resolve) {    // Ad çözümü: dakikada bir yenilenir
            next_resolve = now() + 60;
            addrinfo hints{}, *result = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
                address = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
                freeaddrinfo(result);
            }
        }
        int ms = -1;
        if (icmp != INVALID_HANDLE_VALUE && address.S_un.S_addr &&
            IcmpSendEcho(icmp, address.S_un.S_addr, payload, sizeof(payload), nullptr, reply.data(), DWORD(reply.size()), 1000) > 0) {
            const auto* r = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());
            if (r->Status == IP_SUCCESS) ms = int(r->RoundTripTime);
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            results_.push_back(ms);
            while (results_.size() > 20) results_.pop_front();
        }
        for (int i = 0; i < 10 && !halt_; i++) Sleep(100);
    }
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
}

// --- Sensörler ---

void Sensors::start() {
    if (running()) return;
    halt_ = false;
    {
        std::lock_guard<std::mutex> g(lock_);
        status_ = "loading";
        error_.clear();
        sent_.clear();
        data_ = json::object();
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr;
    CreatePipe(&child_in, &stdin_, &sa, 0);
    CreatePipe(&stdout_, &child_out, &sa, 0);
    SetHandleInformation(stdin_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_in;
    si.hStdOutput = child_out;
    si.hStdError = child_out;
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe_dir() + L"\\lib\\sensor.exe\"";
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (ok) {
        process_ = pi.hProcess;
        CloseHandle(pi.hThread);
    } else {
        std::lock_guard<std::mutex> g(lock_);
        error_ = "lib\\sensor.exe açılamadı";
    }
    thread_ = std::thread([this] { guarded("sensörler", [this] { read_helper(); }); });
}

void Sensors::stop() {
    if (!running()) return;
    halt_ = true;
    if (stdin_) { CloseHandle(stdin_); stdin_ = nullptr; }      // Yardımcı stdin kapanınca çıkar
    if (process_) {
        if (WaitForSingleObject(process_, 1500) != WAIT_OBJECT_0) TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
    thread_.join();
    if (stdout_) { CloseHandle(stdout_); stdout_ = nullptr; }
}

void Sensors::send(const std::string& line) {
    DWORD written;
    if (stdin_) WriteFile(stdin_, line.data(), DWORD(line.size()), &written, nullptr);
}

void Sensors::configure(const std::string& wanted, int interval_ms) {
    const std::string text = "wanted " + wanted + "\ninterval " + std::to_string(interval_ms) + "\n";
    interval_ = interval_ms;
    {
        std::lock_guard<std::mutex> g(lock_);
        if (text == sent_) return;
        sent_ = text;
    }
    send(text);
}

json Sensors::data() {
    std::lock_guard<std::mutex> g(lock_);
    return data_;
}

std::string Sensors::status() {
    std::lock_guard<std::mutex> g(lock_);
    return status_;
}

std::string Sensors::error() {
    std::lock_guard<std::mutex> g(lock_);
    return error_;
}

void Sensors::read_helper() {
    std::string buffer;
    char chunk[4096];
    DWORD got;
    while (process_ && !halt_ && ReadFile(stdout_, chunk, sizeof(chunk), &got, nullptr) && got) {
        buffer.append(chunk, got);
        size_t end;
        while ((end = buffer.find('\n')) != std::string::npos) {
            json line = json::parse(buffer.substr(0, end), nullptr, false);
            buffer.erase(0, end + 1);
            if (!line.is_object()) continue;
            std::lock_guard<std::mutex> g(lock_);
            if (line.contains("error")) {
                error_ = line["error"].is_string() ? line["error"].get<std::string>() : line["error"].dump();
            } else {
                status_ = "ok";
                data_ = std::move(line);
            }
        }
    }
    if (!halt_) fallback();
}

// Kütüphane yüklenemezse en azından kullanım değerleri gösterilsin
void Sensors::fallback() {
    {
        std::lock_guard<std::mutex> g(lock_);
        status_ = "fallback";
    }
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    unsigned long long last_idle = ticks(idle), last_total = ticks(kernel) + ticks(user);
    while (!halt_) {
        for (int waited = 0; waited < interval_ && !halt_; waited += 50) Sleep(50);
        GetSystemTimes(&idle, &kernel, &user);
        const unsigned long long total = ticks(kernel) + ticks(user);
        const double span = double(total - last_total);
        const double load = span > 0 ? 100.0 * (1.0 - double(ticks(idle) - last_idle) / span) : 0;
        last_idle = ticks(idle);
        last_total = total;
        MEMORYSTATUSEX mem{sizeof(mem)};
        GlobalMemoryStatusEx(&mem);
        SYSTEM_POWER_STATUS power{};
        GetSystemPowerStatus(&power);
        json d = {{"cpu", {{"temp", nullptr}, {"load", load}, {"clock", nullptr}, {"power", nullptr}, {"cores", json::array()}}},
                  {"gpus", json::array()},
                  {"ram", {{"used", double(mem.ullTotalPhys - mem.ullAvailPhys) / (1ull << 30)}, {"load", int(mem.dwMemoryLoad)}}},
                  {"disks", json::array()}, {"battery", nullptr}};
        if (power.BatteryLifePercent <= 100) d["battery"] = {{"level", int(power.BatteryLifePercent)}, {"rate", nullptr}};
        std::lock_guard<std::mutex> g(lock_);
        data_ = std::move(d);
    }
}

// --- Ayarlar ---

json default_fps() {
    return {{"position", "top-left"}, {"offset_x", 12}, {"offset_y", 12},
            {"font", "Consolas"}, {"font_size", 18}, {"bold", true}, {"layout", "vertical"}, {"show_labels", true},
            {"scale_with_resolution", true},
            {"show_fps", true}, {"show_frametime", false}, {"show_refresh", false}, {"gpu", ""},
            {"show_gpu_temp", true}, {"show_gpu_hotspot", false}, {"show_gpu_load", true}, {"show_gpu_clock", false},
            {"show_gpu_vram", true}, {"show_gpu_power", false}, {"show_gpu_fan", false},
            {"show_cpu_temp", true}, {"show_cpu_load", true}, {"show_cpu_clock", false}, {"show_cpu_power", false},
            {"show_cpu_cores", false}, {"show_ram", true}, {"show_ram_load", false}, {"show_disk_temp", true},
            {"show_battery", false}, {"show_time", false},
            {"show_fps_avg", false}, {"show_low1", false}, {"show_low01", false}, {"lows_window", 10}, {"show_graph", false}, {"graph_seconds", 5},
            {"show_ping", false}, {"ping_host", "1.1.1.1"}, {"ping_warn", 80}, {"ping_crit", 150},
            {"alert_on", false}, {"alert_gpu_temp", 85}, {"alert_cpu_temp", 90}, {"alert_battery", 15}, {"alert_sound", true},
            {"bench_seconds", 60},
            {"label_color", "#ff9f1c"}, {"color", "#ffffff"}, {"unit_color", "#a0a0a0"}, {"dynamic_color", true},
            {"mid_fps", 60}, {"low_fps", 30}, {"temp_warn", 75}, {"temp_crit", 90}, {"warn_color", "#ffd000"},
            {"crit_color", "#ff4040"}, {"opacity", 100}, {"outline", true}, {"outline_thickness", 1},
            {"outline_color", "#000000"}, {"background", false}, {"bg_color", "#000000"}, {"padding", 6},
            {"interval", 500}, {"sensor_interval", 1000}};
}

json merge_known(const json& defaults, const json& data) {
    json out = defaults;
    if (!data.is_object()) return out;
    for (auto& [key, value] : defaults.items()) {
        auto it = data.find(key);
        if (it == data.end()) continue;
        const bool same = (value.is_number_integer() && it->is_number_integer()) || (value.is_number_float() && it->is_number()) ||
                          (value.is_boolean() && it->is_boolean()) ||
                          (value.is_string() && it->is_string()) || (value.is_array() && it->is_array()) || (value.is_object() && it->is_object());
        if (same) out[key] = *it;
    }
    return out;
}

std::string fps_wanted(const json& s) {
    struct Group { const char* name; std::vector<const char*> keys; };
    static const Group groups[] = {
        {"gpu", {"show_gpu_temp", "show_gpu_hotspot", "show_gpu_load", "show_gpu_clock", "show_gpu_vram", "show_gpu_power", "show_gpu_fan"}},
        {"cpu", {"show_cpu_temp", "show_cpu_load", "show_cpu_clock", "show_cpu_power", "show_cpu_cores"}},
        {"ram", {"show_ram", "show_ram_load"}}, {"disk", {"show_disk_temp"}}, {"battery", {"show_battery"}},
    };
    std::string out;
    for (auto& g : groups)
        for (auto key : g.keys)
            if (s.value(key, false)) { out += std::string(g.name) + " "; break; }
    return out;
}

const json* fps_pick_gpu(const json& data, const std::string& name) {
    // Seçili ekran kartı; yoksa harici kart, o da yoksa ilk kart
    auto it = data.find("gpus");
    if (it == data.end() || !it->is_array() || it->empty()) return nullptr;
    // Sensör listesi bir an eksik ya da bozuk gelebilir (oyun harita yüklerken): nesne olmayan kayıt atlanır
    auto text = [](const json& g, const char* key) { auto f = g.find(key); return f != g.end() && f->is_string() ? f->get<std::string>() : std::string(); };
    for (auto& g : *it)
        if (g.is_object() && text(g, "name") == name) return &g;
    for (auto& g : *it)
        if (g.is_object() && g.contains("discrete") && g["discrete"].is_boolean() && g["discrete"].get<bool>()) return &g;
    for (auto& g : *it)
        if (g.is_object()) return &g;
    return nullptr;
}

// Canlı değer yoksa örnek değeri kullan: önizlemede kapalı bir bilgi de görünsün
static json fill(const json* live, const json& sample) {
    json out = live && live->is_object() ? *live : json::object();
    for (auto& [key, value] : sample.items())
        if (!out.contains(key) || out[key].is_null() || (out[key].is_array() && out[key].empty())) out[key] = value;
    return out;
}

json fps_preview_data(const json& live, const std::string& gpu) {
    static const json sample_gpu = {{"name", ""}, {"discrete", true}, {"temp", 58}, {"hotspot", 67}, {"load", 92},
                                    {"clock", 1530}, {"vram", 1840}, {"power", 24.8}, {"fan", 1850}};
    static const json sample_disk = {{"name", ""}, {"type", "SSD"}, {"temp", 38}};
    static const json sample_cpu = {{"temp", 64}, {"load", 37}, {"clock", 3400}, {"power", 15.2},
                                    {"cores", {{"C1", 62}, {"C2", 65}, {"C3", 61}, {"C4", 64}}}};
    static const json sample_ram = {{"used", 5.4}, {"load", 67}};
    static const json sample_battery = {{"level", 85}, {"rate", 12.4}};
    auto part = [&](const char* key) { auto it = live.find(key); return it == live.end() ? nullptr : &*it; };
    json out = {{"cpu", fill(part("cpu"), sample_cpu)}, {"ram", fill(part("ram"), sample_ram)},
                {"battery", fill(part("battery"), sample_battery)}};
    out["gpus"] = json::array({fill(fps_pick_gpu(live, gpu), sample_gpu)});
    out["disks"] = json::array();
    if (auto* disks = part("disks"); disks && disks->is_array())
        for (auto& d : *disks) out["disks"].push_back(fill(&d, sample_disk));
    if (out["disks"].empty()) out["disks"].push_back(sample_disk);
    return out;
}

bool fps_missing(const json& live, const std::string& gpu, const std::string& key) {
    if (live.empty()) return false;
    auto has = [](const json* j, const char* field) {
        if (!j || !j->is_object() || !j->contains(field)) return false;
        const json& v = (*j)[field];
        return !v.is_null() && !(v.is_array() && v.empty());
    };
    auto part = [&](const char* name) { auto it = live.find(name); return it == live.end() ? nullptr : &*it; };
    if (key.rfind("show_gpu_", 0) == 0) return !has(fps_pick_gpu(live, gpu), key.c_str() + 9);
    if (key == "show_cpu_cores") return !has(part("cpu"), "cores");
    if (key.rfind("show_cpu_", 0) == 0) return !has(part("cpu"), key.c_str() + 9);
    if (key == "show_ram") return !has(part("ram"), "used");
    if (key == "show_ram_load") return !has(part("ram"), "load");
    if (key == "show_battery") return !has(part("battery"), "level");
    if (key == "show_disk_temp") {
        if (auto* disks = part("disks"); disks && disks->is_array())
            for (auto& d : *disks)
                if (has(&d, "temp")) return false;
        return true;
    }
    return false;
}

// --- Satırlar ---
// Şablon, değerin en az kaplayacağı genişliktir: basamak sayısı değişince yazılar kaymaz.
// Aynı tür değerler alt alta gelsin diye her hücrenin sabit bir sütunu var.

namespace {

enum { MAIN, HOTSPOT, LOAD, CLOCK, MEMORY, POWER, FAN };
const int kCoresPerRow = 4;

bool num(const json& j, const char* key, double& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

struct Builder {
    const json& s;
    std::vector<FpsRow> rows;
    uint32_t color, unit;
    bool dynamic;

    explicit Builder(const json& settings) : s(settings) {
        color = parse_color(s.value("color", std::string("#ffffff")));
        unit = parse_color(s.value("unit_color", std::string("#a0a0a0")));
        dynamic = s.value("dynamic_color", true);
    }
    uint32_t fps_color(double fps) const {
        if (dynamic && fps >= 0) {
            if (fps < s.value("low_fps", 30)) return parse_color(s.value("crit_color", std::string("#ff4040")));
            if (fps < s.value("mid_fps", 60)) return parse_color(s.value("warn_color", std::string("#ffd000")));
        }
        return color;
    }
    uint32_t temp_color(double temp) const {
        if (dynamic) {
            if (temp >= s.value("temp_crit", 90)) return parse_color(s.value("crit_color", std::string("#ff4040")));
            if (temp >= s.value("temp_warn", 75)) return parse_color(s.value("warn_color", std::string("#ffd000")));
        }
        return color;
    }
    // Okunamayan değer "--" olarak yazılır; hücre yerini korur, yerleşim kaymaz
    FpsCell cell(bool known, double value, int decimals, const std::string& suffix, const std::string& tmpl,
                 uint32_t value_color, const std::string& prefix = "") const {
        FpsCell c;
        c.tmpl = tmpl;
        if (!prefix.empty()) c.parts.push_back({prefix, 'u', unit});
        std::string text = "--";
        if (known && std::isfinite(value)) {    // Sensör bir an saçma büyük değer verse de tampon taşmasın
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.*f", decimals, std::clamp(value, -1e9, 1e9));
            text = buffer;
        }
        if (text.size() > tmpl.size()) {   // Sığmayan değer şablonun en büyüğüyle sınırlansın
            text = tmpl;
            std::replace(text.begin(), text.end(), '0', '9');
        }
        c.parts.push_back({text, 'v', value_color});
        if (!suffix.empty()) c.parts.push_back({suffix, 'u', unit});
        return c;
    }
    FpsCell value(const json& j, const char* key, int decimals, const std::string& suffix, const std::string& tmpl) const {
        double v = 0;
        const bool known = num(j, key, v);
        return cell(known, v, decimals, suffix, tmpl, color);
    }
    FpsCell temp(const json& j, const char* key, const std::string& prefix = "") const {
        double v = 0;
        const bool known = num(j, key, v);
        return cell(known, v, 0, "°C", "100", known ? temp_color(v) : color, prefix);
    }
    FpsCell power(const json& j, const char* key) const {
        double v = 0;
        const bool known = num(j, key, v);
        return cell(known, v, known && v < 10 ? 1 : 0, " W", "000", color);
    }
    void add(const std::string& label, std::vector<std::pair<int, FpsCell>> cells, bool align = true) {
        if (cells.empty()) return;
        rows.push_back({s.value("show_labels", true) ? label : "", std::move(cells), align});
    }
    bool on(const char* key) const { return s.value(key, false); }
};

}  // namespace

std::vector<FpsRow> fps_rows(const json& s, double fps, int hz, const json& data) {
    Builder b(s);
    {
        std::vector<std::pair<int, FpsCell>> cells;
        const bool known = fps >= 0;
        if (b.on("show_fps"))
            cells.push_back({MAIN, b.cell(known, fps, 0, s.value("show_labels", true) ? "" : " FPS", "000", b.fps_color(fps))});
        if (b.on("show_frametime"))
            cells.push_back({LOAD, b.cell(fps > 0, fps > 0 ? 1000 / fps : 0, 1, " ms", "000.0", b.color)});
        if (b.on("show_refresh")) cells.push_back({CLOCK, b.cell(true, hz, 0, " Hz", "000", b.color)});
        if (auto f = data.find("frame"); f != data.end() && f->is_object()) {     // Ortalama ve en düşük FPS
            auto stat = [&](const char* key, const char* prefix) {
                double v = 0;
                const bool known = num(*f, key, v) && v >= 0;
                return b.cell(known, v, 0, "", "000", known ? b.fps_color(v) : b.color, prefix);
            };
            if (b.on("show_fps_avg")) cells.push_back({HOTSPOT, stat("avg", T("ORT "))});
            if (b.on("show_low1")) cells.push_back({MEMORY, stat("low1", "1% ")});
            if (b.on("show_low01")) cells.push_back({POWER, stat("low01", "0.1% ")});
        }
        b.add("FPS", std::move(cells));
    }
    if (b.on("show_ping")) {
        if (auto p = data.find("ping"); p != data.end() && p->is_object()) {
            double ms = 0, loss = 0;
            const bool known = num(*p, "ms", ms);
            const uint32_t ping_color = !known ? b.color : ms >= s.value("ping_crit", 150) ? parse_color(s.value("crit_color", std::string("#ff4040")))
                                      : ms >= s.value("ping_warn", 80) ? parse_color(s.value("warn_color", std::string("#ffd000"))) : b.color;
            std::vector<std::pair<int, FpsCell>> cells{{MAIN, b.cell(known, ms, 0, " ms", "000", ping_color)}};
            if (num(*p, "loss", loss)) cells.push_back({LOAD, b.cell(true, loss, 0, "%", "100", loss > 0 ? parse_color(s.value("warn_color", std::string("#ffd000"))) : b.color, T("kayıp "))});
            b.add("PING", std::move(cells));
        }
    }
    if (const json* gpu = fps_pick_gpu(data, s.value("gpu", std::string()))) {
        std::vector<std::pair<int, FpsCell>> cells;
        if (b.on("show_gpu_temp")) cells.push_back({MAIN, b.temp(*gpu, "temp")});
        if (b.on("show_gpu_hotspot")) cells.push_back({HOTSPOT, b.temp(*gpu, "hotspot", "HS ")});
        if (b.on("show_gpu_load")) cells.push_back({LOAD, b.value(*gpu, "load", 0, "%", "100")});
        if (b.on("show_gpu_clock")) cells.push_back({CLOCK, b.value(*gpu, "clock", 0, " MHz", "0000")});
        if (b.on("show_gpu_vram")) cells.push_back({MEMORY, b.value(*gpu, "vram", 0, " MB", "00000")});
        if (b.on("show_gpu_power")) cells.push_back({POWER, b.power(*gpu, "power")});
        if (b.on("show_gpu_fan")) cells.push_back({FAN, b.value(*gpu, "fan", 0, " RPM", "0000")});
        b.add("GPU", std::move(cells));
    }
    if (auto it = data.find("cpu"); it != data.end() && it->is_object()) {
        const json& cpu = *it;
        std::vector<std::pair<int, FpsCell>> cells;
        if (b.on("show_cpu_temp")) cells.push_back({MAIN, b.temp(cpu, "temp")});
        if (b.on("show_cpu_load")) cells.push_back({LOAD, b.value(cpu, "load", 0, "%", "100")});
        if (b.on("show_cpu_clock")) cells.push_back({CLOCK, b.value(cpu, "clock", 0, " MHz", "0000")});
        if (b.on("show_cpu_power")) cells.push_back({POWER, b.power(cpu, "power")});
        b.add("CPU", std::move(cells));
        if (b.on("show_cpu_cores") && cpu.contains("cores") && cpu["cores"].is_array()) {
            const json& cores = cpu["cores"];
            for (size_t i = 0; i < cores.size(); i += kCoresPerRow) {
                std::vector<std::pair<int, FpsCell>> row;
                for (size_t n = 0; n < kCoresPerRow && i + n < cores.size(); n++) {
                    const json& core = cores[i + n];     // Beklenen: ["C1", 62]; eksik ya da bozuk kayıt "--" olur
                    const bool pair = core.is_array() && !core.empty();
                    const bool known = pair && core.size() > 1 && core[1].is_number();
                    const double v = known ? core[1].get<double>() : 0;
                    const std::string name = pair && core[0].is_string() ? core[0].get<std::string>() : "C" + std::to_string(i + n + 1);
                    row.push_back({int(n), b.cell(known, v, 0, "°C", "100", known ? b.temp_color(v) : b.color, name + " ")});
                }
                b.add("", std::move(row), false);
            }
        }
    }
    if (auto it = data.find("ram"); it != data.end() && it->is_object()) {
        std::vector<std::pair<int, FpsCell>> cells;
        if (b.on("show_ram")) cells.push_back({MAIN, b.value(*it, "used", 1, " GB", "00.0")});
        if (b.on("show_ram_load")) cells.push_back({LOAD, b.value(*it, "load", 0, "%", "100")});
        b.add("RAM", std::move(cells));
    }
    if (b.on("show_disk_temp")) {
        if (auto it = data.find("disks"); it != data.end() && it->is_array()) {
            std::map<std::string, int> total, seen;
            auto disk_type = [](const json& d) {
                auto f = d.is_object() ? d.find("type") : d.end();
                return f != d.end() && f->is_string() ? f->get<std::string>() : std::string("DISK");
            };
            for (auto& d : *it) if (d.is_object()) total[disk_type(d)]++;
            for (auto& d : *it) {
                if (!d.is_object()) continue;
                const std::string type = disk_type(d);
                seen[type]++;
                b.add(type + (total[type] > 1 ? std::to_string(seen[type]) : ""), {{MAIN, b.temp(d, "temp")}});
            }
        }
    }
    if (b.on("show_battery")) {
        if (auto it = data.find("battery"); it != data.end() && it->is_object())
            b.add(T("PİL"), {{MAIN, b.value(*it, "level", 0, "%", "100")}, {POWER, b.power(*it, "rate")}});
    }
    if (b.on("show_time")) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char text[8];
        sprintf_s(text, "%02d:%02d", t.wHour, t.wMinute);
        FpsCell c;
        c.tmpl = "00:00";
        c.parts.push_back({text, 'v', b.color});
        b.add(T("SAAT"), {{MAIN, c}});
    }
    return std::move(b.rows);
}

std::string fps_signature(const std::vector<FpsRow>& rows) {
    std::string out;
    for (auto& r : rows) {
        out += r.label + "|";
        for (auto& [column, c] : r.cells)
            for (auto& p : c.parts) out += p.text + std::to_string(p.color) + ",";
    }
    return out;
}

void fps_place(const json& s, int w, int h, int area_w, int area_h, float k, int& x, int& y) {
    // Seçili köşeye göre kutunun sol üst noktası
    const std::string p = s.value("position", std::string("top-left"));
    const float hx = p.find("right") != std::string::npos ? 1.f : p.find("left") != std::string::npos ? 0.f : 0.5f;
    const float vy = p.rfind("bottom", 0) == 0 ? 1.f : 0.f;
    const float ox = s.value("offset_x", 12) * k, oy = s.value("offset_y", 12) * k;
    x = int(lround(ox + hx * (area_w - w - 2 * ox)));
    y = int(lround(oy + vy * (area_h - h - 2 * oy)));
}

int refresh_rate() {
    HDC dc = GetDC(nullptr);
    const int hz = GetDeviceCaps(dc, VREFRESH);
    ReleaseDC(nullptr, dc);
    return hz;
}

bool is_admin() { return IsUserAnAdmin() != FALSE; }

bool pawnio_installed() {
    // CPU sıcaklıkları için LibreHardwareMonitor'ın kullandığı sürücü
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}
