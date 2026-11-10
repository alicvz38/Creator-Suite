// Sanal kamera sürücüsü (vcam.dll): Windows 11'in sanal kamera altyapısı (MFCreateVirtualCamera) bu DLL'i kamera hizmetinde açar.
// Kareleri Kayıt Stüdyosu'nun yazdığı paylaşılan bellekten alır ve kamerayı kullanan uygulamalara (Discord, Zoom, Meet...) verir.
// Kayıt Stüdyosu çalışmıyorsa düz koyu bir görüntü verir.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <sddl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

#include "vcam_shared.h"

using Microsoft::WRL::ComPtr;

#ifndef SELFREG_E_CLASS
#define SELFREG_E_CLASS _HRESULT_TYPEDEF_(0x80040201L)
#endif

// Kamera hizmetinin sorduğu denetim arayüzü (ksproxy.h ile aynı tanım)
struct DECLSPEC_UUID("28F54685-06FD-11D2-B27A-00A0C9223196") DECLSPEC_NOVTABLE IKsControl : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG data_length, ULONG* returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG data_length, ULONG* returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(PKSEVENT event, ULONG length, LPVOID data, ULONG data_length, ULONG* returned) = 0;
};

namespace {

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0}, g_locks{0};

// --- Paylaşılan bellekten kare ---
// İki yol denenir: (1) bu süreç Global bellek oluşturur, Kayıt Stüdyosu onu açar; (2) Kayıt Stüdyosu kendi oturumunda
// herkese açık bir bellek oluşturur, bu süreç onu "Session\N\" adıyla açar. Hangisinde yeni kare varsa o okunur.

class FrameReader {
public:
    ~FrameReader() {
        for (auto* v : {&global_, &session_}) {
            if (v->view) UnmapViewOfFile(v->view);
            if (v->mapping) CloseHandle(v->mapping);
        }
    }
    void open() {
        const ULONGLONG t = GetTickCount64();
        if ((global_.view && session_.view) || t < next_try_) return;
        next_try_ = t + 1000;
        if (!diag_ready_) {     // Tanı: bu süreç nasıl çalışıyor
            diag_ready_ = true;
            HANDLE token = nullptr;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                DWORD is_app = 0, size = 0;
                if (GetTokenInformation(token, TokenIsAppContainer, &is_app, sizeof(is_app), &size)) diag_[0] = uint8_t(is_app ? 1 : 0);
                BYTE buffer[64];
                if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
                    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
                    diag_[5] = uint8_t(*GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1) >> 8);
                }
                CloseHandle(token);
            }
            DWORD session = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &session);
            diag_[1] = uint8_t(session);
        }
        if (!global_.view) {
            SECURITY_ATTRIBUTES sa{sizeof(sa)};
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)", SDDL_REVISION_1, &sd, nullptr))
                sa.lpSecurityDescriptor = sd;
            const size_t bytes = vcam_mapping_bytes();
            global_.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, DWORD(uint64_t(bytes) >> 32), DWORD(bytes), kVcamMapping);
            const DWORD error = global_.mapping ? 0 : GetLastError();
            diag_[2] = uint8_t(error); diag_[3] = uint8_t(error >> 8);
            if (sd) LocalFree(sd);
            if (global_.mapping) {
                global_.view = static_cast<uint8_t*>(MapViewOfFile(global_.mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
                if (global_.view) {
                    auto* h = reinterpret_cast<VcamHeader*>(global_.view);
                    if (h->magic != kVcamMagic) { h->width = kVcamWidth; h->height = kVcamHeight; h->slot = 0; h->frame = 0; h->tick = 0; h->magic = kVcamMagic; }
                }
            }
            diag_[6] = global_.view ? 1 : 0;
        }
        if (!session_.view) {
            DWORD sessions[4] = {WTSGetActiveConsoleSessionId(), 1, 2, 3};
            DWORD error = 0;
            for (DWORD id : sessions) {
                if (id == 0xFFFFFFFF) continue;
                const std::wstring name = L"Session\\" + std::to_wstring(id) + L"\\" + (kVcamMapping + 7);
                session_.mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
                if (!session_.mapping) session_.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
                if (session_.mapping) break;
                error = GetLastError();
            }
            if (session_.mapping) {
                session_.view = static_cast<uint8_t*>(MapViewOfFile(session_.mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, vcam_mapping_bytes()));
                session_.writable = session_.view != nullptr;
                if (!session_.view) session_.view = static_cast<uint8_t*>(MapViewOfFile(session_.mapping, FILE_MAP_READ, 0, 0, vcam_mapping_bytes()));
            }
            diag_[4] = uint8_t(session_.view ? 0 : error);
            diag_[7] = session_.view ? 1 : 0;
        }
    }
    // NV12 kareyi hedefe yazar (satır aralığı pitch). Görüntü yoksa koyu ekran (sol üstte tanı baytları).
    void copy(BYTE* dst, LONG pitch) {
        open();
        for (auto* v : {&global_, &session_})      // Kayıt Stüdyosu kameranın kullanıldığını görsün
            if (v->view && (v == &global_ || v->writable)) reinterpret_cast<VcamHeader*>(v->view)->read_tick = LONG64(GetTickCount64());
        const VcamHeader* best = nullptr;
        const uint8_t* base = nullptr;
        for (const auto* v : {&global_, &session_}) {
            if (!v->view) continue;
            const auto* h = reinterpret_cast<const VcamHeader*>(v->view);
            if (h->magic != kVcamMagic || h->frame <= 0 || GetTickCount64() - uint64_t(h->tick) > 2000) continue;
            if (!best || h->tick > best->tick) { best = h; base = v->view; }
        }
        if (!best) {
            for (int y = 0; y < kVcamHeight; y++) memset(dst + size_t(y) * pitch, 28, kVcamWidth);
            for (int y = 0; y < kVcamHeight / 2; y++) {
                BYTE* row = dst + size_t(kVcamHeight + y) * pitch;
                for (int x = 0; x < kVcamWidth; x += 2) { row[x] = 134; row[x + 1] = 124; }
            }
            dst[0] = 0xD1; dst[1] = 0xA6;      // Tanı işareti
            memcpy(dst + 2, diag_, sizeof(diag_));
            return;
        }
        const BYTE* src = base + kVcamHeaderBytes + size_t(best->slot & 1) * kVcamFrameBytes;
        for (int y = 0; y < kVcamHeight * 3 / 2; y++) memcpy(dst + size_t(y) * pitch, src + size_t(y) * kVcamWidth, kVcamWidth);
    }

private:
    struct View { HANDLE mapping = nullptr; uint8_t* view = nullptr; bool writable = false; };
    View global_, session_;
    ULONGLONG next_try_ = 0;
    bool diag_ready_ = false;
    uint8_t diag_[8] = {};
};
HRESULT make_media_type(IMFMediaType** out) {
    ComPtr<IMFMediaType> t;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, kVcamWidth, kVcamHeight);
    MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, kVcamFps, 1);
    MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, kVcamWidth);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, UINT32(kVcamFrameBytes));
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    *out = t.Detach();
    return S_OK;
}

// --- Akış ---

class Source;

class Stream : public IMFMediaStream2, public IKsControl {
public:
    Stream(Source* source, IMFStreamDescriptor* descriptor) : source_(source), descriptor_(descriptor) {
        MFCreateEventQueue(&queue_);
        MFCreateAttributes(&attributes_, 8);
        // Kamera hizmetinin aradığı akış bilgileri
        attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
        g_objects++;
    }
    ~Stream() { g_objects--; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2))
            *out = static_cast<IMFMediaStream2*>(this);
        else if (riid == __uuidof(IKsControl)) *out = static_cast<IKsControl*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        ComPtr<IMFMediaEventQueue> q;
        { std::lock_guard<std::mutex> g(lock_); if (!queue_) return MF_E_SHUTDOWN; q = queue_; }
        return q->GetEvent(flags, e);
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard<std::mutex> g(lock_);
        return queue_ ? queue_->BeginGetEvent(cb, state) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        std::lock_guard<std::mutex> g(lock_);
        return queue_ ? queue_->EndGetEvent(r, e) : MF_E_SHUTDOWN;
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g2, HRESULT hr, const PROPVARIANT* v) override {
        std::lock_guard<std::mutex> g(lock_);
        return queue_ ? queue_->QueueEventParamVar(t, g2, hr, v) : MF_E_SHUTDOWN;
    }

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** out) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
        if (!out) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        if (!queue_) return MF_E_SHUTDOWN;
        *out = descriptor_.Get();
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        std::lock_guard<std::mutex> g(lock_);
        if (!queue_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateSample(&sample);
        if (SUCCEEDED(hr)) hr = MFCreate2DMediaBuffer(kVcamWidth, kVcamHeight, MFVideoFormat_NV12.Data1, FALSE, &buffer);
        if (FAILED(hr)) return hr;
        ComPtr<IMF2DBuffer2> plane;
        BYTE* scan = nullptr;
        BYTE* start = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&plane)) && SUCCEEDED(plane->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan, &pitch, &start, &length))) {
            reader_.copy(scan, pitch);
            plane->Unlock2D();
        }
        buffer->SetCurrentLength(DWORD(kVcamFrameBytes));
        sample->AddBuffer(buffer.Get());
        const LONGLONG t = MFGetSystemTime();
        sample->SetSampleTime(t);
        sample->SetSampleDuration(10000000 / kVcamFps);
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        if (token) sample->SetUnknown(MFSampleExtension_Token, token);
        return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE value) override {
        std::lock_guard<std::mutex> g(lock_);
        state_ = value;
        return S_OK;
    }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* value) override {
        if (!value) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        *value = state_;
        return S_OK;
    }

    // IKsControl: özel kamera ayarı yok
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    IMFAttributes* attributes() { return attributes_.Get(); }
    void start(const PROPVARIANT* at) {
        std::lock_guard<std::mutex> g(lock_);
        state_ = MF_STREAM_STATE_RUNNING;
        if (queue_) queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, at);
    }
    void stop() {
        std::lock_guard<std::mutex> g(lock_);
        state_ = MF_STREAM_STATE_STOPPED;
        if (queue_) queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    void shutdown() {
        std::lock_guard<std::mutex> g(lock_);
        if (queue_) queue_->Shutdown();
        queue_.Reset();
        source_ = nullptr;
    }

private:
    LONG refs_ = 1;
    std::mutex lock_;
    Source* source_;        // Kaynak akışı yaşattığı için zayıf bağ
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    FrameReader reader_;
};

// --- Kaynak ---

class Source : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
public:
    Source() { g_objects++; }
    ~Source() {
        if (stream_) stream_->Release();
        g_objects--;
    }

    HRESULT init() {
        HRESULT hr = MFCreateEventQueue(&queue_);
        if (FAILED(hr)) return hr;
        MFCreateAttributes(&attributes_, 4);
        ComPtr<IMFMediaType> type;
        if (FAILED(hr = make_media_type(&type))) return hr;
        IMFMediaType* types[] = {type.Get()};
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(hr = MFCreateStreamDescriptor(0, 1, types, &sd))) return hr;
        ComPtr<IMFMediaTypeHandler> handler;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler))) handler->SetCurrentMediaType(type.Get());
        stream_ = new (std::nothrow) Stream(this, sd.Get());
        if (!stream_) return E_OUTOFMEMORY;
        // Akış bilgileri tanımlayıcıya da yazılır
        sd->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
        sd->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        sd->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        sd->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
        IMFStreamDescriptor* list[] = {sd.Get()};
        if (FAILED(hr = MFCreatePresentationDescriptor(1, list, &descriptor_))) return hr;
        descriptor_->SelectStream(0);
        return S_OK;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx))
            *out = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == __uuidof(IMFGetService)) *out = static_cast<IMFGetService*>(this);
        else if (riid == __uuidof(IKsControl)) *out = static_cast<IKsControl*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return r;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override {
        ComPtr<IMFMediaEventQueue> q;
        { std::lock_guard<std::mutex> g(lock_); if (shutdown_) return MF_E_SHUTDOWN; q = queue_; }
        return q->GetEvent(flags, e);
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override {
        std::lock_guard<std::mutex> g(lock_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->BeginGetEvent(cb, state);
    }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override {
        std::lock_guard<std::mutex> g(lock_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->EndGetEvent(r, e);
    }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g2, HRESULT hr, const PROPVARIANT* v) override {
        std::lock_guard<std::mutex> g(lock_);
        return shutdown_ ? MF_E_SHUTDOWN : queue_->QueueEventParamVar(t, g2, hr, v);
    }

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override {
        if (!c) return E_POINTER;
        *c = MFMEDIASOURCE_IS_LIVE;
        return S_OK;
    }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
        if (!out) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        return descriptor_->Clone(out);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* format, const PROPVARIANT* at) override {
        if (!pd || !at) return E_INVALIDARG;
        if (format && *format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        std::lock_guard<std::mutex> g(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd)) && selected) {
            ComPtr<IUnknown> unknown;
            stream_->QueryInterface(IID_PPV_ARGS(&unknown));
            queue_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, unknown.Get());
            started_ = true;
            PROPVARIANT time;
            PropVariantInit(&time);
            time.vt = VT_I8;
            time.hVal.QuadPart = MFGetSystemTime();
            stream_->start(&time);
            queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
        }
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        std::lock_guard<std::mutex> g(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        stream_->stop();
        return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        std::lock_guard<std::mutex> g(lock_);
        if (shutdown_) return MF_E_SHUTDOWN;
        shutdown_ = true;
        queue_->Shutdown();
        stream_->shutdown();
        return S_OK;
    }

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
        if (!out) return E_POINTER;
        *out = attributes_.Get();
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
        if (!out) return E_POINTER;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        *out = stream_->attributes();
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }

    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* out) override {
        if (out) *out = nullptr;
        return MF_E_UNSUPPORTED_SERVICE;
    }

    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

private:
    LONG refs_ = 1;
    std::mutex lock_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    Stream* stream_ = nullptr;
    bool shutdown_ = false, started_ = false;
};

STDMETHODIMP Stream::GetMediaSource(IMFMediaSource** out) {
    if (!out) return E_POINTER;
    std::lock_guard<std::mutex> g(lock_);
    if (!source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(out));
}

// --- Etkinleştirici: kamera hizmeti bu sınıfı oluşturur, kaynağı ondan ister ---

class Activator : public IMFActivate {
public:
    Activator() { MFCreateAttributes(&store_, 4); g_objects++; }
    ~Activator() { g_objects--; }

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) *out = static_cast<IMFActivate*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (!r) delete this;
        return r;
    }

    STDMETHODIMP ActivateObject(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        std::lock_guard<std::mutex> g(lock_);
        if (!source_) {
            auto* s = new (std::nothrow) Source();
            if (!s) return E_OUTOFMEMORY;
            const HRESULT hr = s->init();
            if (FAILED(hr)) { s->Release(); return hr; }
            source_.Attach(static_cast<IMFMediaSourceEx*>(s));
        }
        return source_->QueryInterface(riid, out);
    }
    STDMETHODIMP ShutdownObject() override {
        std::lock_guard<std::mutex> g(lock_);
        if (source_) source_->Shutdown();
        source_.Reset();
        return S_OK;
    }
    STDMETHODIMP DetachObject() override {
        std::lock_guard<std::mutex> g(lock_);
        source_.Reset();
        return S_OK;
    }

    // IMFAttributes: iç depoya aktarılır
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return store_->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return store_->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return store_->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return store_->Compare(a, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return store_->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return store_->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return store_->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return store_->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* v) override { return store_->GetStringLength(k, v); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return store_->GetString(k, v, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return store_->GetAllocatedString(k, v, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* v) override { return store_->GetBlobSize(k, v); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* v, UINT32 n, UINT32* l) override { return store_->GetBlob(k, v, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** v, UINT32* l) override { return store_->GetAllocatedBlob(k, v, l); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* v) override { return store_->GetUnknown(k, r, v); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return store_->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return store_->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return store_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return store_->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return store_->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return store_->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return store_->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return store_->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* v, UINT32 n) override { return store_->SetBlob(k, v, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* v) override { return store_->SetUnknown(k, v); }
    STDMETHODIMP LockStore() override { return store_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return store_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return store_->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return store_->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return store_->CopyAllItems(d); }

private:
    LONG refs_ = 1;
    std::mutex lock_;
    ComPtr<IMFAttributes> store_;
    ComPtr<IMFMediaSourceEx> source_;
};

class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) { *out = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* a = new (std::nothrow) Activator();
        if (!a) return E_OUTOFMEMORY;
        const HRESULT hr = a->QueryInterface(riid, out);
        a->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) g_locks++; else g_locks--;
        return S_OK;
    }
};

Factory g_factory;

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    if (clsid != kVcamClsid) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// Kayıt (yönetici gerekir): HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVcamClsidText + L"\\InprocServer32";
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return SELFREG_E_CLASS;
    RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path), DWORD((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t* model = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(model), DWORD((wcslen(model) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kVcamClsidText;
    const LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND ? S_OK : SELFREG_E_CLASS;
}
