#include "recorder.h"

#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <codecapi.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <complex>

// --- Ses cihazları ---

std::vector<AudioDevice> list_audio_devices(bool capture) {
    std::vector<AudioDevice> out;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    if (FAILED(en->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &list))) return out;
    UINT count = 0;
    list->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> dev;
        ComPtr<IPropertyStore> props;
        LPWSTR id = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(list->Item(i, &dev)) && SUCCEEDED(dev->GetId(&id)) &&
            SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
            out.push_back({id, name.pwszVal});
        PropVariantClear(&name);
        CoTaskMemFree(id);
    }
    return out;
}

static ComPtr<IMMDevice> find_device(bool capture, const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return dev;
    if (!id.empty()) en->GetDevice(id.c_str(), &dev);
    if (!dev) en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &dev);
    return dev;
}

// --- Karıştırıcı ---

void AudioMixer::Ring::push(const float* samples, size_t count) {
    std::lock_guard<std::mutex> g(lock);
    data.insert(data.end(), samples, samples + count);
    const size_t limit = kAudioRate / 2;   // 250 ms'den fazla birikirse en eskiyi at (cihaz saatleri arasındaki fark)
    if (data.size() > limit) data.erase(data.begin(), data.end() - kAudioRate / 8);
}

size_t AudioMixer::Ring::pop(float* out, size_t count) {
    std::lock_guard<std::mutex> g(lock);
    const size_t n = std::min(count, data.size());
    std::copy(data.begin(), data.begin() + n, out);
    data.erase(data.begin(), data.begin() + n);
    return n;
}

void AudioMixer::start(bool desktop, const std::wstring& desktop_id, bool mic, const std::wstring& mic_id, const std::vector<AudioInput>& extra) {
    stop();
    halt_ = false;
    for (auto& u : used_) u = false;
    for (auto& r : rings_) { std::lock_guard<std::mutex> g(r.lock); r.data.clear(); }
    used_[0] = desktop;
    used_[1] = mic;
    if (desktop) threads_.emplace_back(&AudioMixer::capture_loop, this, 0, true, desktop_id);
    if (mic) threads_.emplace_back(&AudioMixer::capture_loop, this, 1, false, mic_id);
    for (int i = 0; i < int(extra.size()) && i + 2 < kMaxInputs; i++) {
        const AudioInput& in = extra[i];
        used_[i + 2] = true;
        if (in.kind == 2) threads_.emplace_back(&AudioMixer::app_loop, this, i + 2, in.exe);
        else threads_.emplace_back(&AudioMixer::capture_loop, this, i + 2, in.kind == 1, in.device);
    }
    if (!threads_.empty()) threads_.emplace_back(&AudioMixer::mix_loop, this);
    if (mic && monitor_mode_) monitor_.start(monitor_device_);
}

void AudioMixer::stop() {
    halt_ = true;
    for (auto& t : threads_) t.join();
    threads_.clear();
    monitor_.stop();
    for (auto& l : level_) l = 0.f;
    mic_input_level_ = 0.f;
}

void AudioMixer::set_stream_sink(Sink sink) {
    std::lock_guard<std::mutex> g(sink_lock_);
    stream_sink_ = std::move(sink);
}

void AudioMixer::set_track_sink(TrackSink sink) {
    std::lock_guard<std::mutex> g(sink_lock_);
    track_sink_ = std::move(sink);
}

void AudioMixer::set_mic_filters(const json& filters) {
    std::lock_guard<std::mutex> g(filters_lock_);
    filters_ = filters;
    filters_dirty_ = true;
}

void AudioMixer::set_monitor(int mode, const std::wstring& device_id) {
    if (mode == monitor_mode_ && device_id == monitor_device_) return;
    const bool restart = (mode != 0) != (monitor_mode_ != 0) || device_id != monitor_device_;
    monitor_mode_ = mode;
    monitor_device_ = device_id;
    if (!restart) return;
    monitor_.stop();
    if (mode && running()) monitor_.start(device_id);
}

// --- Mikrofon filtreleri ---

json default_mic_filters() {
    return {{"gain", 0}, {"suppress", false}, {"suppress_level", "medium"},
            {"gate", false}, {"gate_open", -32}, {"gate_close", -40}, {"gate_hold", 200}, {"gate_release", 150},
            {"comp", false}, {"comp_threshold", -18}, {"comp_ratio", 3}, {"comp_attack", 6}, {"comp_release", 60}, {"comp_gain", 0}};
}

namespace {
float db_to_gain(float db) { return powf(10.f, db / 20.f); }
float gain_to_db(float g) { return 20.f * log10f(std::max(g, 1e-9f)); }
float time_coef(float ms) { return 1.f - expf(-1.f / std::max(1.f, ms * kAudioRate / 1000.f)); }   // Örnek başına yaklaşma oranı

// Yerinde karmaşık FFT (2'nin kuvveti uzunluk). inverse: ölçeklenmez
void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = 2 * 3.14159265358979f / float(len) * (inverse ? 1 : -1);
        const std::complex<float> step(cosf(angle), sinf(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; k++) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}
}  // namespace

void Denoiser::configure(int level) {
    // Taban tahmini ortalamanın altında kalır: aşırı çıkarma katsayısı bunu telafi eder
    const float floors[3] = {0.25f, 0.1f, 0.03f}, overs[3] = {3.f, 4.5f, 6.f};
    level = std::clamp(level, 0, 2);
    floor_ = floors[level];
    over_ = overs[level];
}

void Denoiser::reset() {
    window_.resize(N);
    for (int i = 0; i < N; i++) window_[i] = sqrtf(0.5f * (1 - cosf(2 * 3.14159265f * i / N)));   // Yarı örtüşmede karesi toplamı 1
    history_.assign(N, 0.f);
    acc_.assign(N, 0.f);
    noise_.assign(N / 2 + 1, 0.f);
    smooth_.assign(N / 2 + 1, 0.f);
    gain_.assign(N / 2 + 1, 1.f);
    in_.clear();
    out_.assign(HOP, 0.f);      // Çıktı hiç eksik kalmasın diye bir adım önde başlar
    first_ = true;
}

void Denoiser::frame() {
    std::copy(history_.begin() + HOP, history_.end(), history_.begin());
    std::copy(in_.begin(), in_.begin() + HOP, history_.end() - HOP);
    in_.erase(in_.begin(), in_.begin() + HOP);
    std::vector<std::complex<float>> x(N);
    for (int i = 0; i < N; i++) x[i] = history_[i] * window_[i];
    fft(x, false);
    std::vector<float> g(N / 2 + 1);
    for (int k = 0; k <= N / 2; k++) {
        const float p = std::norm(x[k]);
        smooth_[k] = first_ ? p : smooth_[k] * 0.6f + p * 0.4f;
        // Gürültü tabanı: en düşük düzeyi izler, yavaşça yükselir (konuşma tabana karışmaz)
        if (first_) noise_[k] = smooth_[k];
        else if (smooth_[k] < noise_[k]) noise_[k] = noise_[k] * 0.8f + smooth_[k] * 0.2f;
        else noise_[k] *= 1.004f;
        g[k] = std::max(floor_, 1.f - over_ * noise_[k] / (smooth_[k] + 1e-12f));
    }
    first_ = false;
    for (int k = 0; k <= N / 2; k++) {      // Komşu frekanslar ve önceki parça ile yumuşatılır ("su sesi" olmasın)
        const float s = (g[std::max(0, k - 1)] + 2 * g[k] + g[std::min(N / 2, k + 1)]) / 4;
        gain_[k] = s * 0.65f + gain_[k] * 0.35f;
        x[k] *= gain_[k];
        if (k > 0 && k < N / 2) x[N - k] = std::conj(x[k]);
    }
    fft(x, true);
    for (int i = 0; i < N; i++) acc_[i] += x[i].real() / N * window_[i];
    out_.insert(out_.end(), acc_.begin(), acc_.begin() + HOP);
    std::copy(acc_.begin() + HOP, acc_.end(), acc_.begin());
    std::fill(acc_.end() - HOP, acc_.end(), 0.f);
}

void Denoiser::process(float* mono, int frames) {
    if (window_.empty()) reset();
    in_.insert(in_.end(), mono, mono + frames);
    while (int(in_.size()) >= HOP) frame();
    const int n = std::min<int>(frames, int(out_.size()));
    std::copy(out_.begin(), out_.begin() + n, mono);
    std::fill(mono + n, mono + frames, 0.f);
    out_.erase(out_.begin(), out_.begin() + n);
}

void MicChain::configure(const json& f) {
    gain_ = db_to_gain(float(std::clamp(f.value("gain", 0), -30, 30)));
    suppress_ = f.value("suppress", false);
    const std::string level = f.value("suppress_level", std::string("medium"));
    const int l = level == "low" ? 0 : level == "high" ? 2 : 1;
    if (l != suppress_level_) { denoise_.configure(l); suppress_level_ = l; }
    if (!suppress_) denoise_.reset();
    gate_ = f.value("gate", false);
    gate_open_ = float(f.value("gate_open", -32));
    gate_close_ = std::min(gate_open_, float(f.value("gate_close", -40)));
    gate_hold_ = std::max(0, f.value("gate_hold", 200)) * kAudioRate / 1000;
    gate_attack_ = time_coef(5);
    gate_release_ = time_coef(float(std::max(10, f.value("gate_release", 150))) / 4);
    comp_ = f.value("comp", false);
    comp_threshold_ = float(f.value("comp_threshold", -18));
    comp_ratio_ = std::max(1.f, float(f.value("comp_ratio", 3)));
    comp_attack_ = time_coef(float(std::max(1, f.value("comp_attack", 6))));
    comp_release_ = time_coef(float(std::max(1, f.value("comp_release", 60))));
    comp_makeup_ = db_to_gain(float(f.value("comp_gain", 0)));
}

void MicChain::process(float* s, int frames, float& input_peak) {
    float peak = 0, sum = 0;
    for (int i = 0; i < frames * 2; i++) {
        s[i] *= gain_;
        peak = std::max(peak, fabsf(s[i]));
        sum += s[i] * s[i];
    }
    input_peak = peak;
    if (suppress_) {
        mono_.resize(frames);
        for (int i = 0; i < frames; i++) mono_[i] = (s[i * 2] + s[i * 2 + 1]) / 2;
        denoise_.process(mono_.data(), frames);
        for (int i = 0; i < frames; i++) s[i * 2] = s[i * 2 + 1] = mono_[i];
    }
    if (gate_) {
        if (suppress_) {
            sum = 0;
            for (int i = 0; i < frames * 2; i++) sum += s[i] * s[i];
        }
        const float db = gain_to_db(sqrtf(sum / std::max(1, frames * 2)) * 1.41421f);    // Tepe ölçeğinde
        if (db >= gate_open_) { gate_is_open_ = true; gate_hold_left_ = gate_hold_; }
        else if (db < gate_close_ && gate_is_open_) {
            gate_hold_left_ -= frames;
            if (gate_hold_left_ <= 0) gate_is_open_ = false;
        }
        const float target = gate_is_open_ ? 1.f : 0.f;
        for (int i = 0; i < frames; i++) {
            gate_gain_ += (target - gate_gain_) * (target > gate_gain_ ? gate_attack_ : gate_release_);
            s[i * 2] *= gate_gain_;
            s[i * 2 + 1] *= gate_gain_;
        }
    }
    if (comp_) {
        for (int i = 0; i < frames; i++) {
            const float x = std::max(fabsf(s[i * 2]), fabsf(s[i * 2 + 1]));
            comp_env_ += (x - comp_env_) * (x > comp_env_ ? comp_attack_ : comp_release_);
            const float over = gain_to_db(comp_env_) - comp_threshold_;
            const float g = (over > 0 ? db_to_gain(-over * (1 - 1 / comp_ratio_)) : 1.f) * comp_makeup_;
            s[i * 2] *= g;
            s[i * 2 + 1] *= g;
        }
    }
}

// --- Ses izleme ---

void AudioMonitor::start(const std::wstring& device_id) {
    stop();
    halt_ = false;
    {
        std::lock_guard<std::mutex> g(lock_);
        data_.clear();
    }
    thread_ = std::thread(&AudioMonitor::loop, this, device_id);
    active_ = true;
}

void AudioMonitor::stop() {
    active_ = false;
    halt_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioMonitor::push(const float* stereo, int frames) {
    if (!active_) return;
    std::lock_guard<std::mutex> g(lock_);
    data_.insert(data_.end(), stereo, stereo + size_t(frames) * 2);
    if (data_.size() > size_t(kAudioRate) / 5) data_.erase(data_.begin(), data_.end() - kAudioRate / 25);  // Gecikme birikmesin
}

void AudioMixer::set_sink(Sink sink) {
    std::lock_guard<std::mutex> g(sink_lock_);
    sink_ = std::move(sink);
}

void AudioMixer::set_mic_sink(Sink sink) {
    std::lock_guard<std::mutex> g(sink_lock_);
    mic_sink_ = std::move(sink);
}

bool WavWriter::open(const std::wstring& path) {
    close();
    std::lock_guard<std::mutex> g(lock_);
    if (_wfopen_s(&file_, path.c_str(), L"wb") != 0) { file_ = nullptr; return false; }
    uint8_t header[44] = {};
    fwrite(header, 1, 44, file_);       // Kapanınca doldurulur
    bytes_ = 0;
    return true;
}

void WavWriter::write(const int16_t* samples, int frames) {
    std::lock_guard<std::mutex> g(lock_);
    if (!file_) return;
    fwrite(samples, sizeof(int16_t), size_t(frames) * 2, file_);
    bytes_ += frames * 4;
}

void WavWriter::close() {
    std::lock_guard<std::mutex> g(lock_);
    if (!file_) return;
    auto u32 = [](uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); };
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    u32(h + 4, uint32_t(36 + bytes_));
    u32(h + 24, kAudioRate);
    u32(h + 28, kAudioRate * 4);
    h[32] = 4; h[34] = 16;
    memcpy(h + 36, "data", 4);
    u32(h + 40, uint32_t(bytes_));
    fseek(file_, 0, SEEK_SET);
    fwrite(h, 1, 44, file_);
    fclose(file_);
    file_ = nullptr;
}

void AudioMixer::capture_loop(int index, bool loopback, std::wstring id) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        auto dev = find_device(!loopback, id);
        ComPtr<IAudioClient> client, silence;
        ComPtr<IAudioCaptureClient> capture;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX wf{};   // İstediğimiz biçim; Windows gerekirse kendisi dönüştürür
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2; wf.nSamplesPerSec = kAudioRate; wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8; wf.nAvgBytesPerSec = kAudioRate * 8;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        bool ok = dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                  SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &wf, nullptr)) &&
                  SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
        UINT32 silence_size = 0;
        if (ok && loopback) {
            // Hiç ses çalmazken Windows bilgisayar sesini göndermeyi bırakıyor; sessizlik çalarak akışı canlı tut
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &silence)) &&
                SUCCEEDED(silence->GetMixFormat(&mix)) &&
                SUCCEEDED(silence->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr)) &&
                SUCCEEDED(silence->GetBufferSize(&silence_size)) && SUCCEEDED(silence->GetService(IID_PPV_ARGS(&render))))
                silence->Start();
            CoTaskMemFree(mix);
        }
        std::vector<float> zeros;
        while (ok && !halt_) {
            if (render) {
                UINT32 padding = 0;
                silence->GetCurrentPadding(&padding);
                BYTE* out = nullptr;
                if (silence_size > padding && SUCCEEDED(render->GetBuffer(silence_size - padding, &out)))
                    render->ReleaseBuffer(silence_size - padding, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD f = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &f, nullptr, nullptr))) break;
                if (f & AUDCLNT_BUFFERFLAGS_SILENT) {
                    zeros.assign(size_t(frames) * 2, 0.f);
                    rings_[index].push(zeros.data(), zeros.size());
                } else {
                    rings_[index].push(reinterpret_cast<float*>(data), size_t(frames) * 2);
                }
                capture->ReleaseBuffer(frames);
            }
            Sleep(4);
        }
        if (client) client->Stop();
        if (silence) silence->Stop();
    }
    CoUninitialize();
}

void AudioMonitor::loop(std::wstring id) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        auto dev = find_device(false, id);
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2; wf.nSamplesPerSec = kAudioRate; wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8; wf.nAvgBytesPerSec = kAudioRate * 8;
        UINT32 size = 0;
        const bool ok = dev && SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                                     400000, 0, &wf, nullptr)) &&
                        SUCCEEDED(client->GetBufferSize(&size)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))) &&
                        SUCCEEDED(client->Start());
        std::vector<float> chunk;
        while (ok && !halt_) {
            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            const UINT32 room = size > padding ? size - padding : 0;
            {
                std::lock_guard<std::mutex> g(lock_);
                const UINT32 have = UINT32(data_.size() / 2), n = std::min(room, have);
                chunk.assign(data_.begin(), data_.begin() + size_t(n) * 2);
                data_.erase(data_.begin(), data_.begin() + size_t(n) * 2);
            }
            UINT32 frames = UINT32(chunk.size() / 2);
            if (!frames && padding < kAudioBlock / 2 && room >= kAudioBlock) {   // Veri yoksa sessizlik: cızırtı olmasın
                chunk.assign(kAudioBlock * 2, 0.f);
                frames = kAudioBlock;
            }
            BYTE* out = nullptr;
            if (frames && SUCCEEDED(render->GetBuffer(frames, &out))) {
                memcpy(out, chunk.data(), size_t(frames) * 8);
                render->ReleaseBuffer(frames, 0);
            }
            Sleep(3);
        }
        if (client) client->Stop();
    }
    CoUninitialize();
}

void AudioMixer::mix_loop() {
    std::vector<float> in[kMaxInputs];
    std::vector<int16_t> pcm[kMaxInputs];
    for (int i = 0; i < kMaxInputs; i++) { in[i].assign(kAudioBlock * 2, 0.f); pcm[i].assign(kAudioBlock * 2, 0); }
    std::vector<float> sum(kAudioBlock * 2), live(kAudioBlock * 2);
    std::vector<int16_t> out(kAudioBlock * 2), live_out(kAudioBlock * 2);
    const int16_t* blocks[kMaxInputs];
    for (int i = 0; i < kMaxInputs; i++) blocks[i] = pcm[i].data();
    const double start = now();
    long long count = 0;
    float last[kMaxInputs];
    for (int i = 0; i < kMaxInputs; i++) last[i] = volume_[i];
    while (!halt_) {
        // Tek ana saat: ses cihazları ne yaparsa yapsın saniyede tam 100 blok üretilir
        const long long due = (long long)((now() - start) * kAudioRate / kAudioBlock);
        if (count >= due) { Sleep(2); continue; }
        count++;
        for (int i = 0; i < kMaxInputs; i++) {
            std::fill(in[i].begin(), in[i].end(), 0.f);
            if (used_[i]) rings_[i].pop(in[i].data(), in[i].size());
        }
        if (filters_dirty_.exchange(false)) {
            std::lock_guard<std::mutex> g(filters_lock_);
            chain_.configure(filters_);
        }
        float raw = 0;
        if (used_[1]) chain_.process(in[1].data(), kAudioBlock, raw);
        mic_input_level_ = std::max(raw, mic_input_level_.load() * 0.9f);
        const int monitor = monitor_mode_;
        // Ses düzeyi blok boyunca kayarak değişir: susturma ve bas-konuş tık sesi yapmaz
        std::fill(sum.begin(), sum.end(), 0.f);
        std::fill(live.begin(), live.end(), 0.f);
        std::lock_guard<std::mutex> g(sink_lock_);
        const bool separate = bool(mic_sink_);      // Mikrofon ayrı dosyaya: karışıma girmez
        for (int k = 0; k < kMaxInputs; k++) {
            if (!used_[k]) { level_[k] = 0.f; std::fill(pcm[k].begin(), pcm[k].end(), int16_t(0)); continue; }
            const float v = volume_[k];
            const int route = route_[k];
            float peak = 0;
            for (size_t i = 0; i < sum.size(); i++) {
                const float t = float(i / 2 + 1) / kAudioBlock;
                in[k][i] *= last[k] + (v - last[k]) * t;
                peak = std::max(peak, std::fabs(in[k][i]));
                const float s = k == 1 && monitor == 2 ? 0.f : in[k][i];      // Yalnızca dinle: kayda ve yayına girmez
                pcm[k][i] = int16_t(std::clamp(s, -1.f, 1.f) * 32767);
                if (!(k == 1 && separate) && route != 2) sum[i] += s;     // Kayıt karışımı
                if (route != 1) live[i] += s;                               // Yayın karışımı
            }
            last[k] = v;
            level_[k] = std::max(peak, level_[k].load() * 0.9f);
        }
        for (size_t i = 0; i < out.size(); i++) out[i] = int16_t(std::clamp(sum[i], -1.f, 1.f) * 32767);
        for (size_t i = 0; i < live_out.size(); i++) live_out[i] = int16_t(std::clamp(live[i], -1.f, 1.f) * 32767);
        if (monitor && used_[1]) monitor_.push(in[1].data(), kAudioBlock);
        if (sink_) sink_(out.data(), kAudioBlock);
        if (mic_sink_) mic_sink_(pcm[1].data(), kAudioBlock);
        if (track_sink_) track_sink_(blocks, kAudioBlock);
        if (stream_sink_) stream_sink_(live_out.data(), kAudioBlock);
    }
}

// --- Uygulama sesi (Windows 10 2004 ve sonrası): yalnızca seçilen programın ve alt süreçlerinin sesi ---

namespace {

struct ActivateHandler : IActivateAudioInterfaceCompletionHandler, IAgileObject {
    LONG refs = 1;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ComPtr<IAudioClient> client;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs);
        if (!r) { CloseHandle(done); delete this; }
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT result = E_FAIL;
        ComPtr<IUnknown> unknown;
        if (SUCCEEDED(op->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) && unknown) unknown.As(&client);
        SetEvent(done);
        return S_OK;
    }
};

std::wstring exe_of(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, path, &size)) {
        name = path;
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(p);
    return name;
}

// Programın kök süreci: tarayıcı gibi çok süreçli programlarda ses alt süreçten çıkar, ağacın tepesi seçilir
DWORD find_root_process(const std::string& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    std::map<DWORD, std::pair<DWORD, std::wstring>> all;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) all[e.th32ProcessID] = {e.th32ParentProcessID, e.szExeFile};
    CloseHandle(snap);
    const std::wstring want = widen(exe);
    for (auto& [pid, info] : all) {
        if (_wcsicmp(info.second.c_str(), want.c_str())) continue;
        auto parent = all.find(info.first);
        if (parent == all.end() || _wcsicmp(parent->second.second.c_str(), want.c_str())) return pid;
    }
    return 0;
}

}  // namespace

std::vector<std::string> list_audio_apps() {
    std::vector<std::string> out;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetWindowTextLengthW(h) == 0) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;
        const std::string name = narrow(exe_of(pid));
        if (name.empty() || !_stricmp(name.c_str(), "explorer.exe") || !_stricmp(name.c_str(), "ApplicationFrameHost.exe")) continue;
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

void AudioMixer::app_loop(int index, std::string exe) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<float> samples;
    while (!halt_) {
        const DWORD pid = find_root_process(exe);
        if (!pid) { for (int i = 0; i < 20 && !halt_; i++) Sleep(100); continue; }     // Program açılınca yakalanır
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = pid;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
        auto* handler = new ActivateHandler;
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, handler, &op)) &&
            WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0)
            client = handler->client;
        handler->Release();
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 2; wf.nSamplesPerSec = kAudioRate; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 4; wf.nAvgBytesPerSec = kAudioRate * 4;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<IAudioCaptureClient> capture;
        const bool ok = client &&
                        SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                                     200000, 0, &wf, nullptr)) &&
                        SUCCEEDED(client->SetEventHandle(ready)) && SUCCEEDED(client->GetService(IID_PPV_ARGS(&capture))) && SUCCEEDED(client->Start());
        while (ok && !halt_) {
            if (process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;    // Program kapandı: yeniden açılınca tekrar bağlanır
            WaitForSingleObject(ready, 20);
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD f = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &f, nullptr, nullptr))) break;
                samples.resize(size_t(frames) * 2);
                const int16_t* s = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < samples.size(); i++) samples[i] = (f & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.f : s[i] / 32768.f;
                rings_[index].push(samples.data(), samples.size());
                capture->ReleaseBuffer(frames);
            }
        }
        if (client) client->Stop();
        CloseHandle(ready);
        if (process) CloseHandle(process);
        for (int i = 0; i < 10 && !halt_; i++) Sleep(100);
    }
    CoUninitialize();
}
// --- Kaydedici ---

static int bitrate_for(int w, int h, int fps, const std::string& quality) {
    const double base = w * h * fps * (quality == "high" ? 0.13 : quality == "low" ? 0.05 : 0.085);
    return int(std::max(1500000.0, base * (fps >= 60 ? 0.75 : 1.0)));
}

int recording_kbps(int w, int h, int fps, const std::string& quality) { return bitrate_for(w, h, fps, quality) / 1000; }

bool Recorder::open(ID3D11Device* device, const std::wstring& path, int w, int h, int fps, const std::string& quality,
                    int audio_kbps, bool with_audio, std::string& error, SinkMaker make_sink, int video_kbps) {
    path_ = path; fps_ = fps; with_audio_ = with_audio;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ring_.resize(8);
    for (auto& t : ring_)
        if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) { error = "Kodlayıcı dokusu oluşturulamadı"; return false; }

    UINT token = 0;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &manager_)) || FAILED(manager_->ResetDevice(device, token)) ||
        FAILED(MFCreateAttributes(&attrs, 4))) { error = "Media Foundation başlatılamadı"; return false; }
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager_.Get());

    const int bitrate = video_kbps ? video_kbps * 1000 : bitrate_for(w, h, fps, quality);
    ComPtr<IMFMediaType> out, in, aout;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, 100 /* High */);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (make_sink) out->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, fps * 2);
    if (with_audio) {
        MFCreateMediaType(&aout);
        aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        // Windows AAC kodlayıcısı sadece 96/128/160/192 kbps kabul eder
        const int kbps = audio_kbps >= 192 ? 192 : audio_kbps >= 160 ? 160 : audio_kbps >= 128 ? 128 : 96;
        aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps * 1000 / 8);
        aout->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);     // Ham AAC
    }

    HRESULT hr;
    ComPtr<IMFAttributes> params;
    if (make_sink) {
        ComPtr<IMFMediaSink> sink;
        sink.Attach(make_sink(out.Get(), aout.Get()));
        video_stream_ = 0;
        audio_stream_ = 1;
        hr = MFCreateSinkWriterFromMediaSink(sink.Get(), attrs.Get(), &writer_);
        // Yayın platformları sabit bit hızı, 2 saniyede bir anahtar kare ve B karesiz akış ister
        MFCreateAttributes(&params, 4);
        params->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        params->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
        params->SetUINT32(CODECAPI_AVEncMPVGOPSize, fps * 2);
        params->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    } else {
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_FMPEG4);
        hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &writer_);
        if (SUCCEEDED(hr)) hr = writer_->AddStream(out.Get(), &video_stream_);
    }
    if (FAILED(hr)) { error = make_sink ? "Yayın kodlayıcısı açılamadı" : "Kayıt dosyası oluşturulamadı"; return false; }

    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer_->SetInputMediaType(video_stream_, in.Get(), params.Get());
    if (FAILED(hr)) { error = "Video kodlayıcı açılamadı"; return false; }

    if (with_audio) {
        ComPtr<IMFMediaType> ain;
        if (!make_sink) hr = writer_->AddStream(aout.Get(), &audio_stream_);
        MFCreateMediaType(&ain);
        ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
        ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * 4);
        if (SUCCEEDED(hr)) hr = writer_->SetInputMediaType(audio_stream_, ain.Get(), nullptr);
        if (FAILED(hr)) { error = "Ses kodlayıcı açılamadı"; return false; }
    }
    if (FAILED(writer_->BeginWriting())) { error = "Kodlayıcı başlatılamadı"; return false; }
    started_ = now();
    return true;
}

double Recorder::elapsed() const {
    const double t = paused_at_ ? paused_at_ : now();
    return t - started_ - paused_total_;
}

double Recorder::next_frame_time() const {
    return started_ + paused_total_ + double(frames_) / fps_;
}

void Recorder::pause() {
    if (!paused_at_) paused_at_ = now();
}

void Recorder::resume() {
    if (paused_at_) { paused_total_ += now() - paused_at_; paused_at_ = 0; }
}

void Recorder::write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas) {
    if (paused()) return;
    const long long index = (long long)(elapsed() * fps_);
    if (index < frames_) return;   // Bu kare aralığı için zaten yazıldı
    ID3D11Texture2D* slot = ring_[ring_index_ % ring_.size()].Get();
    ring_index_++;
    ctx->CopyResource(slot, canvas);
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMF2DBuffer> buffer2d;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), slot, 0, FALSE, &buffer))) return;
    DWORD length = 0;
    buffer.As(&buffer2d);
    buffer2d->GetContiguousLength(&length);
    buffer->SetCurrentLength(length);
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    const LONGLONG frame = 10000000LL / fps_;
    sample->SetSampleTime(index * frame);
    sample->SetSampleDuration(frame);
    std::lock_guard<std::mutex> g(write_lock_);
    writer_->WriteSample(video_stream_, sample.Get());
    frames_ = index + 1;   // Atlanan aralıklarda önceki kare ekranda kalır
    unique_++;
}

void Recorder::write_audio(const int16_t* samples, int frames) {
    if (!with_audio_ || paused()) return;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    const DWORD bytes = DWORD(frames) * 4;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) return;
    BYTE* data = nullptr;
    buffer->Lock(&data, nullptr, nullptr);
    memcpy(data, samples, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(audio_frames_ * 10000000LL / kAudioRate);
    sample->SetSampleDuration(frames * 10000000LL / kAudioRate);
    std::lock_guard<std::mutex> g(write_lock_);
    writer_->WriteSample(audio_stream_, sample.Get());
    audio_frames_ += frames;
}

void Recorder::finish(std::string& error) {
    std::lock_guard<std::mutex> g(write_lock_);
    if (writer_ && FAILED(writer_->Finalize())) error = "Kayıt dosyası kapatılamadı";
    writer_.Reset();
}
