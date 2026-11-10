// Sanal kamera: Kayıt Stüdyosu ile kamera sürücüsü (vcam.dll) arasındaki paylaşılan bellek.
// vcam.dll Windows'un kamera hizmetinde (Frame Server) çalışır; Kayıt Stüdyosu kareleri bu belleğe yazar.
#pragma once
#include <windows.h>
#include <cstdint>

// {6E3B0F7A-2C4D-4F1B-9A61-5D2E8C7B4A10}
static const GUID kVcamClsid = {0x6e3b0f7a, 0x2c4d, 0x4f1b, {0x9a, 0x61, 0x5d, 0x2e, 0x8c, 0x7b, 0x4a, 0x10}};
static const wchar_t* kVcamClsidText = L"{6E3B0F7A-2C4D-4F1B-9A61-5D2E8C7B4A10}";
static const wchar_t* kVcamMapping = L"Global\\RecordingStudioVirtualCamera";
static const wchar_t* kVcamName = L"Recording Studio Camera";

const int kVcamWidth = 1920, kVcamHeight = 1080, kVcamFps = 30;
const uint32_t kVcamMagic = 0x43565352;    // "RSVC"
const size_t kVcamFrameBytes = size_t(kVcamWidth) * kVcamHeight * 3 / 2;     // NV12
const size_t kVcamHeaderBytes = 4096;

// İki kare yeri: yazar boş olana yazar, sonra "slot"u değiştirir; okuyan "slot"taki kareyi alır
struct VcamHeader {
    uint32_t magic;
    uint32_t width, height;
    volatile LONG slot;
    volatile LONG64 frame;          // Her yeni karede artar
    volatile LONG64 tick;           // Son karenin GetTickCount64 değeri
    volatile LONG64 read_tick;      // Kamera hizmetinin son okuduğu an: bir uygulama kamerayı kullanıyor mu
};

const DWORD kVcamVersion = 2;       // Kurulu sürücü bundan eskiyse yeniden kurulur

inline size_t vcam_mapping_bytes() { return kVcamHeaderBytes + 2 * kVcamFrameBytes; }
