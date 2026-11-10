// Canlı yayın: Media Foundation kodlayıcısından gelen H.264 ve AAC verisini RTMP / RTMPS ile sunucuya gönderir.
// Ağ işi kendi iş parçacığındadır; internet yavaşlasa da çizim ve kayıt beklemez.
#pragma once
#include "encoder.h"

struct StreamConfig {
    std::string url, key;
    int width = 1920, height = 1080, fps = 60, video_kbps = 4500, audio_kbps = 160;
    bool audio = true;
    bool use_ffmpeg = false;    // Windows kodlayıcısı yerine FFmpeg kodlayıcısı
    std::string codec = "h264";  // h264, hevc, av1 (son ikisi Enhanced RTMP ile gider)
    int delay_ms = 0;           // Yayın gecikmesi: veri bu kadar bekletilip gönderilir
    EncodeConfig encode;
};

struct StreamStats {
    enum State { Off, Connecting, Live, Reconnecting } state = Off;
    double kbps = 0;
    long long dropped = 0;
    std::string error;
};

class Streamer {
public:
    Streamer();
    ~Streamer();
    bool start(ID3D11Device* device, const StreamConfig& config, std::string& error);
    void write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame);     // frame yayın çözünürlüğünde olmalı
    void write_audio(const int16_t* samples, int frames);
    void stop();
    StreamStats stats();
    bool failed();              // Bağlantı kalıcı olarak koptu
    double elapsed() const;
    double next_frame_time() const;
    int fps() const;
    long long frames() const;
    long long encoder_dropped() const;      // Kodlayıcı yetişemediği için atlanan kare

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

const int kStreamMaxAutoKbps = 6000;    // "Kayıtla aynı" modunda platform sınırını aşmasın
