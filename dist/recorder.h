// Ses karıştırıcı (WASAPI) ve MP4 kaydedici (Media Foundation).
#pragma once
#include "common.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct AudioDevice { std::wstring id, name; };
std::vector<AudioDevice> list_audio_devices(bool capture);   // capture: mikrofonlar, değilse hoparlörler

const int kAudioRate = 48000;
const int kAudioBlock = 480;   // 10 ms

json default_mic_filters();

// Gürültü bastırma: kısa parçaların frekans dağılımından sürekli gürültü (fan, uğultu, hışırtı) tahmin edilip bastırılır.
// Mono çalışır; yaklaşık 10 ms gecikme ekler.
class Denoiser {
public:
    static const int N = 512, HOP = 256;
    void configure(int level);              // 0 hafif, 1 orta, 2 güçlü
    void reset();
    void process(float* mono, int frames);

private:
    void frame();
    float floor_ = 0.12f, over_ = 2.f;
    std::vector<float> window_, history_, acc_, noise_, smooth_, gain_;
    std::vector<float> in_, out_;           // Bekleyen girdi ve hazır çıktı
    bool first_ = true;
};

// Mikrofon filtre zinciri: kazanç > gürültü bastırma > gürültü kapısı > kompresör (48 kHz stereo)
class MicChain {
public:
    void configure(const json& f);
    void process(float* stereo, int frames, float& input_peak);   // input_peak: kazançtan sonraki tepe (kapı eşiği için)

private:
    float gain_ = 1;
    bool suppress_ = false, gate_ = false, comp_ = false;
    int suppress_level_ = -1;
    Denoiser denoise_;
    std::vector<float> mono_;
    float gate_open_ = -32, gate_close_ = -40, gate_attack_ = 0, gate_release_ = 0, gate_gain_ = 1;
    int gate_hold_ = 0, gate_hold_left_ = 0;
    bool gate_is_open_ = false;
    float comp_threshold_ = -18, comp_ratio_ = 3, comp_attack_ = 0, comp_release_ = 0, comp_makeup_ = 1, comp_env_ = 0;
};

// Ses izleme: mikrofonun kayda giden halini kulaklıktan dinletir
class AudioMonitor {
public:
    ~AudioMonitor() { stop(); }
    void start(const std::wstring& device_id);
    void stop();
    void push(const float* stereo, int frames);

private:
    void loop(std::wstring id);
    std::thread thread_;
    std::atomic<bool> halt_{false}, active_{false};
    std::mutex lock_;
    std::vector<float> data_;
};

// Ek ses girişi: ikinci mikrofon / ses kartı girişi, bir hoparlörün sesi ya da tek bir uygulamanın sesi
struct AudioInput {
    int kind = 0;               // 0 giriş aygıtı, 1 çıkış aygıtı (o hoparlöre giden ses), 2 uygulama
    std::wstring device;        // Aygıt kimliği; boşsa varsayılan
    std::string exe;            // Uygulama: programın exe adı (alt süreçleriyle birlikte, örneğin tarayıcı sekmeleri)
};

// Uygulama listesi (ses yakalamak için): pencere açık programlar
std::vector<std::string> list_audio_apps();

// Bilgisayar sesi + mikrofon + ek girişler. Kendi saatiyle 10 ms'lik karışık bloklar üretir.
// Giriş sırası: 0 bilgisayar sesi, 1 mikrofon, 2.. ek girişler.
class AudioMixer {
public:
    static const int kMaxInputs = 10;
    using Sink = std::function<void(const int16_t*, int frames)>;
    // Çoklu ses parçası için: her girişin bu bloktaki hali (ses düzeyi ve filtreler uygulanmış), kMaxInputs uzunluğunda
    using TrackSink = std::function<void(const int16_t* const* inputs, int frames)>;
    ~AudioMixer() { stop(); }
    void start(bool desktop, const std::wstring& desktop_id, bool mic, const std::wstring& mic_id, const std::vector<AudioInput>& extra = {});
    void stop();
    void set_sink(Sink sink);
    // Ayrı mikrofon kaydı: mikrofon yalnızca bu uca gider, karışıma girmez
    void set_mic_sink(Sink sink);
    void set_track_sink(TrackSink sink);
    // Yayının ayrı karışımı: verilirse set_sink kayda, bu yayına gider
    void set_stream_sink(Sink sink);
    // Girişin gideceği yer: 0 kayıt ve yayın, 1 yalnızca kayıt, 2 yalnızca yayın
    void set_route(int input, int route) { if (input >= 0 && input < kMaxInputs) route_[input] = route; }
    void set_volumes(float desktop, float mic) { volume_[0] = desktop; volume_[1] = mic; }
    void set_volume(int input, float v) { if (input >= 0 && input < kMaxInputs) volume_[input] = v; }
    void set_mic_filters(const json& filters);
    // 0 kapalı, 1 dinle ve kaydet, 2 yalnızca dinle (kayda ve yayına girmez)
    void set_monitor(int mode, const std::wstring& device_id);
    float level(int i) const { return i >= 0 && i < kMaxInputs ? level_[i].load() : 0.f; }   // 0..1 tepe değer
    float mic_input_level() const { return mic_input_level_; }     // Mikrofonun filtre öncesi hali (kazançtan sonra)
    bool running() const { return !threads_.empty(); }

private:
    struct Ring {
        std::mutex lock;
        std::vector<float> data;    // Stereo örnekler
        void push(const float* samples, size_t count);
        size_t pop(float* out, size_t count);
    };
    void capture_loop(int index, bool loopback, std::wstring id);
    void app_loop(int index, std::string exe);
    void mix_loop();

    Ring rings_[kMaxInputs];
    bool used_[kMaxInputs] = {};
    std::vector<std::thread> threads_;
    std::atomic<bool> halt_{false};
    std::atomic<float> volume_[kMaxInputs];
    std::atomic<float> level_[kMaxInputs];
    std::atomic<float> mic_input_level_{0.f};
    std::mutex sink_lock_;
    Sink sink_, mic_sink_, stream_sink_;
    TrackSink track_sink_;
    std::atomic<int> route_[kMaxInputs];
    std::mutex filters_lock_;
    json filters_;
    std::atomic<bool> filters_dirty_{false};
    MicChain chain_;
    AudioMonitor monitor_;
    std::atomic<int> monitor_mode_{0};
    std::wstring monitor_device_;

public:
    AudioMixer() { for (int i = 0; i < kMaxInputs; i++) { volume_[i] = 1.f; level_[i] = 0.f; route_[i] = 0; } }
};
// Basit WAV yazıcı: 48 kHz, stereo, 16 bit
class WavWriter {
public:
    ~WavWriter() { close(); }
    bool open(const std::wstring& path);
    void write(const int16_t* samples, int frames);
    void close();
    bool is_open() const { return file_ != nullptr; }

private:
    FILE* file_ = nullptr;
    long long bytes_ = 0;
    std::mutex lock_;
};

int recording_kbps(int w, int h, int fps, const std::string& quality);

// Motorun beslediği çıkış: Windows kodlayıcısı (Recorder) ya da FFmpeg kodlayıcısı (FfmpegOutput)
class FrameOutput {
public:
    virtual ~FrameOutput() = default;
    virtual void write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas) = 0;
    virtual void write_audio(const int16_t* samples, int frames) = 0;
    virtual void write_audio_track(int, const int16_t*, int) {}     // Çoklu ses parçası: 1 = ikinci parça (ilki karışım)
    virtual void finish(std::string& error) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual bool paused() const = 0;
    virtual double elapsed() const = 0;
    virtual double next_frame_time() const = 0;     // Bir sonraki kare ne zaman çizilmeli (now() cinsinden)
    virtual long long frames() const = 0;
    virtual long long unique() const = 0;
    virtual long long dropped() const { return 0; } // Kodlayıcı yetişemediği için atlanan kare
    virtual const std::wstring& path() const = 0;
    virtual bool failed() const { return false; }   // Kodlayıcı kendiliğinden durdu
    virtual std::string failure() { return {}; }
};

// Parçalı MP4'e (fMP4) yazar: elektrik kesilse de o ana kadarki kayıt açılabilir.
class Recorder : public FrameOutput {
public:
    // Dosya yerine yayına yazmak için make_sink verilir: kodlayıcı türlerini alıp paketleri toplayan ucu oluşturur
    using SinkMaker = std::function<IMFMediaSink*(IMFMediaType* video, IMFMediaType* audio)>;
    bool open(ID3D11Device* device, const std::wstring& path, int w, int h, int fps, const std::string& quality,
              int audio_kbps, bool with_audio, std::string& error, SinkMaker make_sink = nullptr, int video_kbps = 0);
    void write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas) override;
    void write_audio(const int16_t* samples, int frames) override;
    void finish(std::string& error) override;

    void pause() override;
    void resume() override;
    bool paused() const override { return paused_at_ != 0; }
    double elapsed() const override;
    double next_frame_time() const override;

    long long frames() const override { return frames_; }
    long long unique() const override { return unique_; }
    const std::wstring& path() const override { return path_; }

private:
    ComPtr<IMFSinkWriter> writer_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    std::vector<ComPtr<ID3D11Texture2D>> ring_;   // Kodlayıcı işlerken üzerine yazılmasın
    std::mutex write_lock_;
    DWORD video_stream_ = 0, audio_stream_ = 0;
    bool with_audio_ = false;
    int fps_ = 30, ring_index_ = 0;
    double started_ = 0, paused_at_ = 0, paused_total_ = 0;
    long long frames_ = 0, unique_ = 0, audio_frames_ = 0;
    std::wstring path_;
};
