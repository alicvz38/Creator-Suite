// Kodlayıcılar: OBS'teki donanım ve yazılım kodlayıcıları FFmpeg üzerinden çalışır.
// FFmpeg ayrı süreçtir: x264 ya da benzeri işlemciyi yorsa da çizim döngüsü beklemez, yetişemezse kare atlanır.
// Kareler zaman damgasıyla gönderildiği için atlanan kare ses ile görüntüyü kaydırmaz.
#pragma once
#include "recorder.h"

#include <condition_variable>
#include <deque>

struct EncoderInfo {
    const char* key;        // Ayarlarda saklanan ad
    const char* name;       // Ekranda görünen ad
    const char* group;      // "Donanım" ya da "Yazılım"
    const char* codec;      // h264, hevc, av1, vp9
    const char* ffmpeg;     // FFmpeg'deki adı; Windows kodlayıcısında boş
};
const std::vector<EncoderInfo>& encoder_list();
const EncoderInfo* find_encoder(const std::string& key);

void encoders_probe();                              // Hangileri çalışıyor, arka planda bir kez dener
int encoder_state(const std::string& key);          // -1 deneniyor, 0 bu bilgisayarda yok, 1 çalışıyor
std::string encoder_best(bool stream);              // Bu bilgisayardaki en uygun kodlayıcı

using Choices = std::vector<std::pair<std::string, std::string>>;
Choices encoder_presets(const std::string& key);
Choices encoder_profiles(const std::string& key);
Choices encoder_tunes(const std::string& key);
Choices record_containers(const std::string& key);
Choices record_audio_codecs(const std::string& container);
int encoder_quality_max(const std::string& key);    // Sabit kalite değerinin üst sınırı
json encoder_defaults(const std::string& key, bool stream);     // Seçilen kodlayıcıya uygun başlangıç ayarları
json default_video();
std::wstring container_extension(const std::string& container);

struct EncodeConfig {
    std::string encoder, rate = "cbr", preset, profile, tune, custom;
    std::string container = "mp4", audio_codec = "aac";
    int width = 1920, height = 1080, fps = 60, video_kbps = 6000, audio_kbps = 160;
    int quality = 20, keyint = 2, bframes = 2;
    bool audio = true, stream = false;
    std::wstring path;
    // Çoklu ses parçası: ilk parça karışımdır ("Mix"), bunlar sonraki parçaların adları (her ses girişi ayrı)
    std::vector<std::string> audio_tracks;
    // Tekrar arabelleği: dosya yerine bu klasöre kısa parçalar yazılır (segment_seconds uzunlukta, segment_count tane döner)
    std::wstring segment_folder;
    int segment_seconds = 0, segment_count = 0;
};
EncodeConfig make_encode_config(const json& video, int w, int h, int fps, int kbps, int audio_kbps, bool audio, bool stream);
std::string encode_options(const EncodeConfig& c);      // FFmpeg'in kodlayıcı seçenekleri (Video Düzenleyici de kullanır)
std::wstring ffmpeg_exe();

class FfmpegOutput : public FrameOutput {
public:
    using DataSink = std::function<void(const uint8_t*, size_t)>;   // Yayında FLV verisi buraya gelir
    ~FfmpegOutput() override;
    bool open(ID3D11Device* device, const EncodeConfig& config, std::string& error, DataSink sink = nullptr);

    void write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas) override;
    void write_audio(const int16_t* samples, int frames) override;
    void write_audio_track(int track, const int16_t* samples, int frames) override;
    void finish(std::string& error) override;
    void pause() override;
    void resume() override;
    bool paused() const override { return paused_at_ != 0; }
    double elapsed() const override;
    double next_frame_time() const override { return started_ + paused_total_ + double(frames_) / config_.fps; }
    long long frames() const override { return frames_; }
    long long unique() const override { return unique_; }
    long long dropped() const override { return dropped_; }
    const std::wstring& path() const override { return config_.path; }
    bool failed() const override { return failed_; }
    std::string failure() override;

private:
    struct Item { int track = 1; long long ms = 0; std::vector<uint8_t> data; };
    struct Slot { ComPtr<ID3D11Texture2D> y, uv; long long ms = 0; bool busy = false; };

    bool make_gpu(ID3D11Device* device, std::string& error);
    bool start_process(const std::wstring& command, std::string& error);
    void collect(ID3D11DeviceContext* ctx, bool wait);
    void push(Item&& item);
    void writer_loop();
    void reader_loop();
    void error_loop();
    bool write_pipe(const void* data, size_t size);

    EncodeConfig config_;
    DataSink sink_;
    // Ekran kartında BGRA'dan NV12'ye
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_y_, ps_uv_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Texture2D> y_, uv_;
    ComPtr<ID3D11RenderTargetView> y_rtv_, uv_rtv_;
    std::vector<Slot> slots_;
    int write_slot_ = 0, read_slot_ = 0;
    // Süreç
    HANDLE process_ = nullptr, job_ = nullptr, stdin_ = nullptr, stdout_ = nullptr, stderr_ = nullptr;
    std::thread writer_, reader_, errors_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Item> queue_;
    std::vector<std::vector<uint8_t>> spare_;
    int queued_video_ = 0;
    bool stop_ = false;
    std::atomic<bool> failed_{false};
    std::string last_error_;
    // Zaman
    double started_ = 0, paused_at_ = 0, paused_total_ = 0;
    long long frames_ = 0, unique_ = 0, audio_frames_ = 0;
    std::vector<long long> track_frames_;
    std::atomic<long long> dropped_{0};
};
