#include "encoder.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <sstream>

// --- Kodlayıcı listesi ---

const std::vector<EncoderInfo>& encoder_list() {
    static const std::vector<EncoderInfo> list = {
        {"windows", "Windows donanım kodlayıcı H.264", "Donanım", "h264", ""},
        {"qsv_h264", "Intel Quick Sync H.264", "Donanım", "h264", "h264_qsv"},
        {"qsv_hevc", "Intel Quick Sync HEVC", "Donanım", "hevc", "hevc_qsv"},
        {"qsv_av1", "Intel Quick Sync AV1", "Donanım", "av1", "av1_qsv"},
        {"nvenc_h264", "NVIDIA NVENC H.264", "Donanım", "h264", "h264_nvenc"},
        {"nvenc_hevc", "NVIDIA NVENC HEVC", "Donanım", "hevc", "hevc_nvenc"},
        {"nvenc_av1", "NVIDIA NVENC AV1", "Donanım", "av1", "av1_nvenc"},
        {"amf_h264", "AMD AMF H.264", "Donanım", "h264", "h264_amf"},
        {"amf_hevc", "AMD AMF HEVC", "Donanım", "hevc", "hevc_amf"},
        {"amf_av1", "AMD AMF AV1", "Donanım", "av1", "av1_amf"},
        {"x264", "x264 H.264", "Yazılım", "h264", "libx264"},
        {"x265", "x265 HEVC", "Yazılım", "hevc", "libx265"},
        {"aom", "AOM AV1", "Yazılım", "av1", "libaom-av1"},
        {"vp9", "libvpx VP9", "Yazılım", "vp9", "libvpx-vp9"},
    };
    return list;
}

const EncoderInfo* find_encoder(const std::string& key) {
    for (auto& e : encoder_list())
        if (key == e.key) return &e;
    return nullptr;
}

static std::wstring ffmpeg_path() { return exe_dir() + L"\\lib\\ffmpeg.exe"; }
static bool family(const std::string& key, const char* prefix) { return key.rfind(prefix, 0) == 0; }

// --- Bu bilgisayarda hangileri çalışıyor ---

namespace {
std::mutex g_probe_lock;
std::map<std::string, int> g_probe;
bool g_probe_started = false;

bool try_encoder(const EncoderInfo& e) {
    std::wstring cmd = L"\"" + ffmpeg_path() + L"\" -hide_banner -loglevel quiet -nostdin -f lavfi -i color=black:s=640x360:r=30 -frames:v 3 -pix_fmt yuv420p -c:v " +
                       widen(e.ffmpeg);
    if (std::string(e.key) == "aom") cmd += L" -usage realtime -cpu-used 8";
    cmd += L" -f null -";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    const bool done = WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return done && code == 0;
}
}  // namespace

void encoders_probe() {
    {
        std::lock_guard<std::mutex> g(g_probe_lock);
        if (g_probe_started) return;
        g_probe_started = true;
    }
    std::thread([] {
        const bool present = GetFileAttributesW(ffmpeg_path().c_str()) != INVALID_FILE_ATTRIBUTES;
        for (auto& e : encoder_list()) {
            const bool ok = !e.ffmpeg[0] || (present && try_encoder(e));
            std::lock_guard<std::mutex> g(g_probe_lock);
            g_probe[e.key] = ok ? 1 : 0;
        }
    }).detach();
}

int encoder_state(const std::string& key) {
    std::lock_guard<std::mutex> g(g_probe_lock);
    auto it = g_probe.find(key);
    return it == g_probe.end() ? -1 : it->second;
}

std::string encoder_best(bool stream) {
    // Donanım kodlayıcı oyundan işlemci çalmaz. Yayında platform uyumu için H.264.
    for (const char* key : {"nvenc_h264", "amf_h264"})
        if (encoder_state(key) == 1) return key;
    // Intel'de Windows yolu aynı çipi kareyi ekran kartından indirmeden kullanır: CS:GO açıkken 1080p60'ta
    // Quick Sync karelerin yarıya yakınını atlarken Windows yolu hiç atlamadı
    if (encoder_state("qsv_h264") == 1) return "windows";
    return encoder_state("x264") == 1 ? "x264" : "windows";
}

// --- Seçenekler ---

Choices encoder_presets(const std::string& k) {
    if (k == "x264" || k == "x265") {
        Choices c;
        for (const char* p : {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"}) c.push_back({p, p});
        return c;
    }
    if (family(k, "qsv")) return {{"veryfast", "veryfast"}, {"faster", "faster"}, {"fast", "fast"}, {"medium", "medium"}, {"slow", "slow"}, {"slower", "slower"}, {"veryslow", "veryslow"}};
    if (family(k, "nvenc")) return {{"p1", "P1 En hızlı"}, {"p2", "P2 Daha hızlı"}, {"p3", "P3 Hızlı"}, {"p4", "P4 Orta"}, {"p5", "P5 Yavaş"}, {"p6", "P6 Daha yavaş"}, {"p7", "P7 En yüksek kalite"}};
    if (family(k, "amf")) return {{"speed", "Hız"}, {"balanced", "Dengeli"}, {"quality", "Kalite"}};
    if (k == "aom" || k == "vp9") {
        Choices c;
        for (int i = k == "aom" ? 9 : 8; i >= 0; i--) c.push_back({std::to_string(i), std::to_string(i) + (i ? "" : " (en yavaş, en kaliteli)")});
        return c;
    }
    return {};
}

Choices encoder_profiles(const std::string& k) {
    const EncoderInfo* e = find_encoder(k);
    if (!e || !e->ffmpeg[0] || std::string(e->codec) != "h264") return {};
    Choices c = {{"", "Otomatik"}};
    if (k == "x264") c.push_back({"baseline", "baseline"});
    c.push_back({"main", "main"});
    c.push_back({"high", "high"});
    return c;
}

Choices encoder_tunes(const std::string& k) {
    if (k == "x264") return {{"", "Yok"}, {"film", "film"}, {"animation", "animation"}, {"grain", "grain"}, {"stillimage", "stillimage"}, {"psnr", "psnr"}, {"ssim", "ssim"}, {"fastdecode", "fastdecode"}, {"zerolatency", "zerolatency"}};
    if (k == "x265") return {{"", "Yok"}, {"grain", "grain"}, {"psnr", "psnr"}, {"ssim", "ssim"}, {"fastdecode", "fastdecode"}, {"zerolatency", "zerolatency"}};
    if (family(k, "nvenc")) return {{"", "Varsayılan"}, {"hq", "Yüksek kalite"}, {"ll", "Düşük gecikme"}, {"ull", "En düşük gecikme"}};
    return {};
}

Choices record_containers(const std::string& k) {
    if (k == "windows") return {{"mp4", "MP4"}};
    if (k == "vp9") return {{"mkv", "MKV"}, {"mp4", "MP4 (parçalı)"}};
    return {{"mp4", "MP4 (parçalı)"}, {"mkv", "MKV"}, {"mov", "MOV (parçalı)"}, {"ts", "MPEG-TS"}, {"flv", "FLV"}};
}

Choices record_audio_codecs(const std::string& container) {
    if (container == "mkv") return {{"aac", "AAC"}, {"flac", "FLAC (kayıpsız)"}, {"pcm", "PCM (kayıpsız, sıkıştırmasız)"}};
    if (container == "mov") return {{"aac", "AAC"}, {"alac", "ALAC (kayıpsız)"}, {"pcm", "PCM (kayıpsız, sıkıştırmasız)"}};
    if (container == "mp4") return {{"aac", "AAC"}, {"flac", "FLAC (kayıpsız)"}};
    return {{"aac", "AAC"}};
}

int encoder_quality_max(const std::string& k) { return k == "aom" || k == "vp9" ? 63 : 51; }

std::wstring container_extension(const std::string& c) { return L"." + widen(c.empty() ? std::string("mp4") : c); }

json default_video() {
    return {{"encoder", "windows"}, {"rate", "cbr"}, {"bitrate", 6000}, {"quality", 20}, {"keyint", 2}, {"preset", ""}, {"profile", ""},
            {"tune", ""}, {"bframes", 2}, {"custom", ""}, {"container", "mp4"}, {"audio_codec", "aac"}};
}

json encoder_defaults(const std::string& k, bool stream) {
    json v = default_video();
    v["encoder"] = k;
    v["rate"] = stream ? "cbr" : "cq";
    v["quality"] = k == "aom" || k == "vp9" ? 32 : k == "x265" || family(k, "qsv") ? 24 : 20;
    if (k == "x264") v["preset"] = "veryfast";
    if (k == "x265") v["preset"] = "superfast";
    if (family(k, "qsv")) { v["preset"] = "veryfast"; v["bframes"] = 0; }
    if (family(k, "nvenc")) v["preset"] = "p5";
    if (family(k, "amf")) v["preset"] = "balanced";
    if (k == "aom") v["preset"] = "8";
    if (k == "vp9") v["preset"] = "6";
    if (k == "vp9") v["container"] = "mkv";
    if (k == "windows") v["rate"] = "cbr";
    return v;
}

EncodeConfig make_encode_config(const json& v, int w, int h, int fps, int kbps, int audio_kbps, bool audio, bool stream) {
    EncodeConfig c;
    c.encoder = v.value("encoder", std::string("windows"));
    c.rate = stream ? "cbr" : v.value("rate", std::string("cbr"));
    c.preset = v.value("preset", std::string());
    c.profile = v.value("profile", std::string());
    c.tune = v.value("tune", std::string());
    c.custom = v.value("custom", std::string());
    c.container = v.value("container", std::string("mp4"));
    c.audio_codec = stream ? "aac" : v.value("audio_codec", std::string("aac"));
    c.width = w; c.height = h; c.fps = fps;
    c.video_kbps = kbps; c.audio_kbps = audio_kbps;
    c.quality = v.value("quality", 20);
    c.keyint = stream ? 2 : v.value("keyint", 2);     // Yayın platformları 2 saniye ister
    c.bframes = v.value("bframes", 2);
    c.audio = audio; c.stream = stream;
    return c;
}

// FFmpeg komut satırı
std::string encode_options(const EncodeConfig& c) {
    const EncoderInfo* e = find_encoder(c.encoder);
    std::ostringstream a;
    const std::string k = c.encoder;
    const int kbps = c.video_kbps;
    a << " -c:v " << e->ffmpeg << " -fps_mode cfr -r " << c.fps;
    a << " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    if (c.keyint > 0) a << " -g " << c.fps * c.keyint;
    if (c.stream) a << " -sc_threshold 0";

    const bool x_family = k == "x264" || k == "x265";
    if (x_family || family(k, "qsv")) { if (!c.preset.empty()) a << " -preset " << c.preset; }
    if (family(k, "nvenc") && !c.preset.empty()) a << " -preset " << c.preset;
    if (family(k, "amf") && !c.preset.empty()) a << " -quality " << c.preset;
    if (k == "aom") a << " -usage realtime -row-mt 1 -cpu-used " << (c.preset.empty() ? "8" : c.preset);
    if (k == "vp9") a << " -deadline realtime -row-mt 1 -cpu-used " << (c.preset.empty() ? "6" : c.preset);
    if (!c.tune.empty()) a << " -tune " << c.tune;
    if (!c.profile.empty()) a << " -profile:v " << c.profile;
    if (k != "aom" && k != "vp9" && !family(k, "amf")) a << " -bf " << c.bframes;

    std::string params = c.custom;      // x264 ve x265'te OBS'teki gibi "anahtar=değer" listesi
    if (c.rate == "cq") {
        if (x_family) a << " -crf " << c.quality;
        else if (family(k, "qsv")) a << " -q:v " << c.quality;
        else if (family(k, "nvenc")) a << " -rc constqp -qp " << c.quality;
        else if (family(k, "amf")) a << " -rc cqp -qp_i " << c.quality << " -qp_p " << c.quality;
        else a << " -crf " << c.quality << " -b:v 0";
    } else {
        const bool cbr = c.rate == "cbr";
        const int peak = cbr ? kbps : kbps * 3 / 2;
        a << " -b:v " << kbps << "k -maxrate " << peak << "k -bufsize " << (cbr ? kbps : kbps * 2) << "k";
        if (cbr && (x_family || k == "aom" || k == "vp9")) a << " -minrate " << kbps << "k";
        if (family(k, "nvenc")) a << " -rc " << (cbr ? "cbr" : "vbr");
        if (family(k, "amf")) a << " -rc " << (cbr ? "cbr" : "vbr_peak");
        if (cbr && k == "x264") params = "nal-hrd=cbr" + (params.empty() ? "" : " " + params);
    }
    if (x_family || !params.empty()) {
        if (x_family) {
            std::replace(params.begin(), params.end(), ' ', ':');
            if (!params.empty()) a << (k == "x264" ? " -x264-params " : " -x265-params ") << params;
        } else {
            a << " " << params;     // Diğerlerinde doğrudan FFmpeg seçeneği
        }
    }
    if (c.audio) {
        if (c.audio_codec == "flac") a << " -c:a flac";
        else if (c.audio_codec == "alac") a << " -c:a alac";
        else if (c.audio_codec == "pcm") a << " -c:a pcm_s16le";
        else a << " -c:a aac -b:a " << c.audio_kbps << "k";
    }
    return a.str();
}

std::wstring ffmpeg_exe() { return ffmpeg_path(); }

static std::wstring build_command(const EncodeConfig& c) {
    std::string input = " -hide_banner -loglevel error -probesize 32 -analyzeduration 0 -fflags +nobuffer -f matroska -i pipe:0";
    input += " -map 0:v:0";
    if (c.audio) input += " -map 0:a:0";
    for (size_t i = 0; c.audio && i < c.audio_tracks.size(); i++) input += " -map 0:a:" + std::to_string(i + 1);
    std::string titles;
    if (c.audio && !c.audio_tracks.empty()) {   // Video Düzenleyici parçaları bu adlarla ayrı kanallara açar
        titles += " -metadata:s:a:0 title=Mix";
        for (size_t i = 0; i < c.audio_tracks.size(); i++) {
            std::string t = c.audio_tracks[i];
            for (auto& ch : t) if (ch == '"') ch = '\'';
            titles += " -metadata:s:a:" + std::to_string(i + 1) + " title=\"" + t + "\"";
        }
    }
    std::wstring cmd = L"\"" + ffmpeg_path() + L"\"" + widen(input + encode_options(c) + titles);
    if (c.stream) return cmd + L" -f flv -flvflags no_duration_filesize pipe:1";
    if (!c.segment_folder.empty()) {    // Tekrar arabelleği: dönen kısa MPEG-TS parçaları ve listesi
        wchar_t seg[160];
        swprintf_s(seg, L" -f segment -segment_time %d -segment_format mpegts -segment_wrap %d -segment_list_size %d -segment_list_type csv -segment_list \"",
                   c.segment_seconds, c.segment_count, c.segment_count);
        return cmd + seg + c.segment_folder + L"\\list.csv\" -reset_timestamps 0 -y \"" + c.segment_folder + L"\\part%03d.ts\"";
    }
    if (c.container == "mp4" || c.container == "mov")   // Parçalı: elektrik kesilse de o ana kadarki kayıt açılır
        cmd += L" -movflags +frag_keyframe+empty_moov+default_base_moof -frag_duration 2000000";
    if (c.container == "mkv") cmd += L" -f matroska";
    if (c.container == "ts") cmd += L" -f mpegts";
    if (c.container == "flv") cmd += L" -f flv";
    return cmd + L" -y \"" + c.path + L"\"";
}

// --- Matroska: FFmpeg'e giden kareler kendi zaman damgasıyla taşınır ---

namespace {

using Bytes = std::vector<uint8_t>;

void put_id(Bytes& o, uint32_t id) {
    for (int s = 24; s >= 0; s -= 8)
        if (id >> s) o.push_back(uint8_t(id >> s));
}
void put_size(Bytes& o, uint64_t size) {
    o.push_back(1);
    for (int s = 48; s >= 0; s -= 8) o.push_back(uint8_t(size >> s));
}
void put_master(Bytes& o, uint32_t id, const Bytes& body) {
    put_id(o, id);
    put_size(o, body.size());
    o.insert(o.end(), body.begin(), body.end());
}
void put_uint(Bytes& o, uint32_t id, uint64_t v) {
    put_id(o, id);
    o.push_back(0x88);
    for (int s = 56; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}
void put_text(Bytes& o, uint32_t id, const std::string& s) {
    put_id(o, id);
    put_size(o, s.size());
    o.insert(o.end(), s.begin(), s.end());
}

Bytes matroska_header(const EncodeConfig& c) {
    Bytes out, b;
    put_uint(b, 0x4286, 1); put_uint(b, 0x42F7, 1); put_uint(b, 0x42F2, 4); put_uint(b, 0x42F3, 8);
    put_text(b, 0x4282, "matroska"); put_uint(b, 0x4287, 4); put_uint(b, 0x4285, 2);
    put_master(out, 0x1A45DFA3, b);
    put_id(out, 0x18538067);
    out.push_back(1);
    out.insert(out.end(), 7, 0xFF);     // Boyutu bilinmeyen kesit
    Bytes info;
    put_uint(info, 0x2AD7B1, 1000000);  // Milisaniye
    put_text(info, 0x4D80, "Recording Studio");
    put_text(info, 0x5741, "Recording Studio");
    put_master(out, 0x1549A966, info);

    Bytes video, track, tracks;
    put_uint(video, 0xB0, c.width);
    put_uint(video, 0xBA, c.height);
    put_text(video, 0x2EB524, "NV12");
    put_uint(track, 0xD7, 1); put_uint(track, 0x73C5, 1); put_uint(track, 0x83, 1); put_uint(track, 0x9C, 0);
    put_text(track, 0x86, "V_UNCOMPRESSED");
    put_master(track, 0xE0, video);
    put_master(tracks, 0xAE, track);
    if (c.audio) {
        Bytes audio, t;
        const float rate = float(kAudioRate);
        uint32_t bits;
        memcpy(&bits, &rate, 4);
        put_id(audio, 0xB5);
        audio.push_back(0x84);
        for (int s = 24; s >= 0; s -= 8) audio.push_back(uint8_t(bits >> s));
        put_uint(audio, 0x9F, 2);
        put_uint(audio, 0x6264, 16);
        put_uint(t, 0xD7, 2); put_uint(t, 0x73C5, 2); put_uint(t, 0x83, 2); put_uint(t, 0x9C, 0);
        put_text(t, 0x86, "A_PCM/INT/LIT");
        put_master(t, 0xE1, audio);
        put_master(tracks, 0xAE, t);
        for (size_t i = 0; i < c.audio_tracks.size(); i++) {    // Ek ses parçaları: 3, 4, ...
            Bytes extra;
            const uint64_t number = 3 + i;
            put_uint(extra, 0xD7, number); put_uint(extra, 0x73C5, number); put_uint(extra, 0x83, 2); put_uint(extra, 0x9C, 0);
            put_text(extra, 0x86, "A_PCM/INT/LIT");
            put_master(extra, 0xE1, audio);
            put_master(tracks, 0xAE, extra);
        }
    }
    put_master(out, 0x1654AE6B, tracks);
    return out;
}

const char* kConvert = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct VS { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VS vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    VS o;
    o.pos = float4(c.x * 2 - 1, 1 - c.y * 2, 0, 1);
    o.uv = c;
    return o;
}
float ps_y(VS i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    return dot(c, float3(0.2126, 0.7152, 0.0722)) * 219.0 / 255.0 + 16.0 / 255.0;
}
float2 ps_uv(VS i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb;
    return float2(dot(c, float3(-0.1146, -0.3854, 0.5)), dot(c, float3(0.5, -0.4542, -0.0458))) * 224.0 / 255.0 + 128.0 / 255.0;
}
)";

const size_t kVideoQueue = 12;      // Bundan fazla kare bekliyorsa kodlayıcı yetişemiyor: kare atlanır
const size_t kStartQueue = 90;

}  // namespace

// --- Ekran kartı tarafı ---

bool FfmpegOutput::make_gpu(ID3D11Device* d, std::string& error) {
    error = "Kodlayıcı dönüşümü hazırlanamadı";
    ComPtr<ID3DBlob> vs, y, uv, err;
    if (FAILED(D3DCompile(kConvert, strlen(kConvert), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kConvert, strlen(kConvert), nullptr, nullptr, nullptr, "ps_y", "ps_5_0", 0, 0, &y, &err)) ||
        FAILED(D3DCompile(kConvert, strlen(kConvert), nullptr, nullptr, nullptr, "ps_uv", "ps_5_0", 0, 0, &uv, &err)))
        return false;
    d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_);
    d->CreatePixelShader(y->GetBufferPointer(), y->GetBufferSize(), nullptr, &ps_y_);
    d->CreatePixelShader(uv->GetBufferPointer(), uv->GetBufferSize(), nullptr, &ps_uv_);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d->CreateSamplerState(&sd, &sampler_);

    D3D11_TEXTURE2D_DESC td{};
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    auto plane = [&](int w, int h, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv) {
        td.Width = w; td.Height = h; td.Format = format;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET; td.CPUAccessFlags = 0;
        return SUCCEEDED(d->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(d->CreateRenderTargetView(tex.Get(), nullptr, &rtv));
    };
    const int w = config_.width, h = config_.height;
    if (!plane(w, h, DXGI_FORMAT_R8_UNORM, y_, y_rtv_) || !plane(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, uv_, uv_rtv_)) return false;
    slots_.resize(4);
    for (auto& s : slots_) {
        D3D11_TEXTURE2D_DESC sdesc{};
        sdesc.MipLevels = sdesc.ArraySize = 1;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sdesc.Width = w; sdesc.Height = h; sdesc.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(d->CreateTexture2D(&sdesc, nullptr, &s.y))) return false;
        sdesc.Width = w / 2; sdesc.Height = h / 2; sdesc.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(d->CreateTexture2D(&sdesc, nullptr, &s.uv))) return false;
    }
    error.clear();
    return vs_ && ps_y_ && ps_uv_ && sampler_;
}

// --- Süreç ---

bool FfmpegOutput::start_process(const std::wstring& command, std::string& error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, out_write = nullptr, err_write = nullptr;
    if (!CreatePipe(&in_read, &stdin_, &sa, 1 << 22) || !CreatePipe(&stdout_, &out_write, &sa, 1 << 20) || !CreatePipe(&stderr_, &err_write, &sa, 1 << 16)) {
        error = "Kodlayıcı bağlantısı açılamadı";
        return false;
    }
    SetHandleInformation(stdin_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read; si.hStdOutput = out_write; si.hStdError = err_write;
    PROCESS_INFORMATION pi{};
    std::wstring line = command;
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(in_read); CloseHandle(out_write); CloseHandle(err_write);
    if (!ok) { error = "FFmpeg başlatılamadı (lib\\ffmpeg.exe)"; return false; }
    // Program beklenmedik kapanırsa FFmpeg arkada kalmasın
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
    return true;
}

bool FfmpegOutput::open(ID3D11Device* device, const EncodeConfig& config, std::string& error, DataSink sink) {
    config_ = config;
    sink_ = std::move(sink);
    if (!find_encoder(config_.encoder) || !find_encoder(config_.encoder)->ffmpeg[0]) { error = "Kodlayıcı bulunamadı"; return false; }
    if (!make_gpu(device, error)) return false;
    if (!start_process(build_command(config_), error)) return false;
    const Bytes header = matroska_header(config_);
    Item first;
    first.track = 0;
    first.data = header;
    queue_.push_back(std::move(first));
    writer_ = std::thread(&FfmpegOutput::writer_loop, this);
    reader_ = std::thread(&FfmpegOutput::reader_loop, this);
    errors_ = std::thread(&FfmpegOutput::error_loop, this);
    started_ = now();
    return true;
}

FfmpegOutput::~FfmpegOutput() {
    std::string ignored;
    finish(ignored);
}

bool FfmpegOutput::write_pipe(const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size) {
        DWORD wrote = 0;
        if (!WriteFile(stdin_, p, DWORD(std::min<size_t>(size, 1 << 20)), &wrote, nullptr) || !wrote) return false;
        p += wrote;
        size -= wrote;
    }
    return true;
}

void FfmpegOutput::writer_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait(g, [&] { return !queue_.empty() || stop_; });
            if (queue_.empty()) break;
            item = std::move(queue_.front());
            queue_.pop_front();
            if (item.track == 1) queued_video_--;
        }
        bool ok;
        if (item.track == 0) {
            ok = write_pipe(item.data.data(), item.data.size());
        } else {
            // Her blok kendi kümesinde: kümenin zamanı blok zamanıdır
            Bytes h;
            const uint64_t block = 4 + item.data.size();
            put_id(h, 0x1F43B675);
            put_size(h, 10 + 9 + block);
            put_uint(h, 0xE7, uint64_t(item.ms));
            put_id(h, 0xA3);
            put_size(h, block);
            h.push_back(uint8_t(0x80 | item.track));
            h.push_back(0); h.push_back(0);
            h.push_back(0x80);
            ok = write_pipe(h.data(), h.size()) && write_pipe(item.data.data(), item.data.size());
        }
        if (!ok) { failed_ = true; break; }
        if (item.track == 1) {
            std::lock_guard<std::mutex> g(lock_);
            if (spare_.size() < 16) spare_.push_back(std::move(item.data));
        }
    }
    CloseHandle(stdin_);    // FFmpeg dosyayı bitirsin
    stdin_ = nullptr;
    std::lock_guard<std::mutex> g(lock_);
    queue_.clear();
}

void FfmpegOutput::reader_loop() {
    std::vector<uint8_t> buffer(1 << 16);
    DWORD got = 0;
    while (ReadFile(stdout_, buffer.data(), DWORD(buffer.size()), &got, nullptr) && got)
        if (sink_) sink_(buffer.data(), got);
}

void FfmpegOutput::error_loop() {
    char buffer[1024];
    DWORD got = 0;
    while (ReadFile(stderr_, buffer, sizeof(buffer), &got, nullptr) && got) {
        std::lock_guard<std::mutex> g(lock_);
        last_error_.append(buffer, got);
        if (last_error_.size() > 4000) last_error_.erase(0, last_error_.size() - 4000);
    }
    // Süreç bitti. Biz durdurmadıysak kodlayıcı hata verdi.
    std::lock_guard<std::mutex> g(lock_);
    if (!stop_) failed_ = true;
    wake_.notify_all();
}

std::string FfmpegOutput::failure() {
    std::lock_guard<std::mutex> g(lock_);
    std::string text = last_error_;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    const size_t line = text.find_last_of('\n');
    if (line != std::string::npos) text = text.substr(line + 1);
    return "Kodlayıcı durdu" + (text.empty() ? std::string() : ": " + text);
}

// --- Zaman ---

double FfmpegOutput::elapsed() const {
    const double t = paused_at_ ? paused_at_ : now();
    return t - started_ - paused_total_;
}

void FfmpegOutput::pause() { if (!paused_at_) paused_at_ = now(); }
void FfmpegOutput::resume() { if (paused_at_) { paused_total_ += now() - paused_at_; paused_at_ = 0; } }

// --- Kareler ---

void FfmpegOutput::push(Item&& item) {
    {
        std::lock_guard<std::mutex> g(lock_);
        if (stop_) return;
        if (item.track == 1) queued_video_++;
        queue_.push_back(std::move(item));
    }
    wake_.notify_one();
}

void FfmpegOutput::write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas) {
    collect(ctx, false);
    if (paused() || failed_) return;
    const long long index = (long long)(elapsed() * config_.fps);
    if (index < frames_) return;
    Slot& slot = slots_[write_slot_];
    if (slot.busy) { collect(ctx, true); }
    if (slot.busy) { dropped_++; return; }

    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(canvas, nullptr, &srv))) return;
    ID3D11ShaderResourceView* source = srv.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetSamplers(0, 1, &sampler);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    auto pass = [&](ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, int w, int h) {
        D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &source);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    };
    pass(y_rtv_.Get(), ps_y_.Get(), config_.width, config_.height);
    pass(uv_rtv_.Get(), ps_uv_.Get(), config_.width / 2, config_.height / 2);
    ID3D11RenderTargetView* no_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &no_rtv, nullptr);
    ctx->CopyResource(slot.y.Get(), y_.Get());
    ctx->CopyResource(slot.uv.Get(), uv_.Get());
    slot.ms = index * 1000 / config_.fps;
    slot.busy = true;
    write_slot_ = (write_slot_ + 1) % int(slots_.size());
    frames_ = index + 1;
    unique_++;
}

// Ekran kartının bitirdiği kareleri beklemeden alır; wait ise en eskisini bekler
void FfmpegOutput::collect(ID3D11DeviceContext* ctx, bool wait) {
    for (;;) {
        Slot& s = slots_[read_slot_];
        if (!s.busy) return;
        D3D11_MAPPED_SUBRESOURCE y{}, uv{};
        const HRESULT hr = ctx->Map(s.y.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &y);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (FAILED(hr)) { s.busy = false; read_slot_ = (read_slot_ + 1) % int(slots_.size()); return; }
        if (FAILED(ctx->Map(s.uv.Get(), 0, D3D11_MAP_READ, 0, &uv))) { ctx->Unmap(s.y.Get(), 0); return; }
        const int w = config_.width, h = config_.height;
        bool room;
        std::vector<uint8_t> data;
        {
            std::lock_guard<std::mutex> g(lock_);
            // Donanım kodlayıcılar açılırken bir saniye kadar kare almaz: ilk anda kuyruk daha uzun olabilir
            room = size_t(queued_video_) < (elapsed() < 3 ? kStartQueue : kVideoQueue);
            if (room && !spare_.empty()) { data = std::move(spare_.back()); spare_.pop_back(); }
        }
        if (room) {
            data.resize(size_t(w) * h * 3 / 2);
            uint8_t* out = data.data();
            for (int row = 0; row < h; row++, out += w) memcpy(out, static_cast<uint8_t*>(y.pData) + size_t(row) * y.RowPitch, w);
            for (int row = 0; row < h / 2; row++, out += w) memcpy(out, static_cast<uint8_t*>(uv.pData) + size_t(row) * uv.RowPitch, w);
            Item item;
            item.track = 1; item.ms = s.ms; item.data = std::move(data);
            push(std::move(item));
        } else {
            dropped_++;
        }
        ctx->Unmap(s.y.Get(), 0);
        ctx->Unmap(s.uv.Get(), 0);
        s.busy = false;
        read_slot_ = (read_slot_ + 1) % int(slots_.size());
        wait = false;
    }
}

void FfmpegOutput::write_audio(const int16_t* samples, int frames) {
    if (!config_.audio || paused() || failed_) return;
    Item item;
    item.track = 2;
    item.ms = audio_frames_ * 1000 / kAudioRate;
    item.data.resize(size_t(frames) * 4);
    memcpy(item.data.data(), samples, item.data.size());
    audio_frames_ += frames;
    push(std::move(item));
}

void FfmpegOutput::write_audio_track(int track, const int16_t* samples, int frames) {
    if (!config_.audio || track < 1 || track > int(config_.audio_tracks.size()) || paused() || failed_) return;
    if (track_frames_.size() < config_.audio_tracks.size()) track_frames_.resize(config_.audio_tracks.size(), 0);
    long long& done = track_frames_[track - 1];
    Item item;
    item.track = 2 + track;
    item.ms = done * 1000 / kAudioRate;
    item.data.resize(size_t(frames) * 4);
    memcpy(item.data.data(), samples, item.data.size());
    done += frames;
    push(std::move(item));
}

void FfmpegOutput::finish(std::string& error) {
    if (!process_) return;
    {
        std::lock_guard<std::mutex> g(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (writer_.joinable()) writer_.join();
    // Yavaş kodlayıcılar elinde kalan kareleri bitirsin
    if (WaitForSingleObject(process_, 120000) != WAIT_OBJECT_0) {
        TerminateProcess(process_, 1);
        error = "Kodlayıcı zamanında kapanmadı";
    }
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    if (reader_.joinable()) reader_.join();
    if (errors_.joinable()) errors_.join();
    if (code != 0 && error.empty() && !config_.stream) { failed_ = true; error = failure(); }
    CloseHandle(process_); process_ = nullptr;
    if (stdout_) { CloseHandle(stdout_); stdout_ = nullptr; }
    if (stderr_) { CloseHandle(stderr_); stderr_ = nullptr; }
    if (job_) { CloseHandle(job_); job_ = nullptr; }
}
