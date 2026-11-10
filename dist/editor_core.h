// Video Düzenleyici'nin arayüzden bağımsız çekirdeği: proje, medya okuma, FFmpeg akışları, ses çalma, dışa aktarma.
#pragma once
#include "encoder.h"

#include <audioclient.h>
#include <array>
#include <memory>

const int kPreviewHeight = 540;

// Proje bir json: {"canvas": {w, h, fps}, "tracks": [...], "clips": [...], "markers": [...]}.
// tracks dizisinde üstte duran kanal görüntüde de üsttedir.
json new_project();
json new_clip(const std::string& type, const std::string& track, double start, double length);
double project_length(const json& project);
double clip_tail(const json& project, const json& clip);    // Sonraki klibin geçişi sürerken bu klip altında görünmeye devam eder
// Art arda iki klibin arasındaki geçiş, hangisine konmuş olursa olsun bir klipten diğerine doğrudan geçiştir
struct Transition { std::string key; double duration = 0; };
Transition transition_in(const json& project, const json& clip);
Transition transition_out(const json& project, const json& clip);
void pack_magnetic(json& project);
// Aralık işlemleri: kilitli kanallar değişmez
void project_cut(json& project, double time);                     // İşaretçinin altındaki klipler ikiye ayrılır
void project_delete_range(json& project, double from, double to); // Aralık silinir, arkası öne gelir
void project_keep_range(json& project, double from, double to);   // Yalnızca aralık kalır, başa alınır

json* previous_clip(json& project, const json& clip);
// İşaretler bölüm olarak da kullanılır: eski projelerde sayı, yenilerde {t, name}
double marker_time(const json& marker);
std::string marker_name(const json& marker);
std::vector<std::pair<double, std::string>> project_chapters(const json& project);    // Sıralı
std::string youtube_chapters(const json& project);
// Animasyon noktaları: klip içindeki bir anda konum, ölçek, dönüş ve opaklık; aralar yumuşak ya da doğrusal
struct Key { double t = 0, scale = 100, x = 0, y = 0, rotate = 0, opacity = 100; bool smooth = true; };
std::vector<Key> clip_keys(const json& clip);
json clip_at(const json& clip, double local);       // Klibin o andaki görünümü (animasyon uygulanmış)
void set_key(json& clip, double local, const json& view, double tolerance);  // O andaki anahtar kareyi yazar ya da ekler
void split_keys(json& left, json& right, double at);   // Bölünen klibin animasyonu iki parçaya dağılır
// Giriş / çıkış hareketi: klibin o andaki ek kayması (tuval oranı), ölçeği, saydamlığı, soldan açılması ve yazıda görünen harf sayısı
struct Motion { double dx = 0, dy = 0, scale = 1, alpha = 1, reveal = 1; int chars = -1; };
struct MotionInfo { const char* key; const char* name; };
const std::vector<MotionInfo>& motion_list();
Motion clip_motion(const json& clip, double local);
int text_length(const json& clip);
// Yuvarlak köşe, kenarlık ve maske; ölçüler 1080p tuvale göre piksel, unit = tuval yüksekliği / 1080
bool clip_has_frame(const json& clip);
bool clip_can_frame(const json& clip);              // Görüntü, resim ve düz renk
float shape_distance(float qx, float qy, float hx, float hy, float radius, bool ellipse);   // İçeride eksi
struct FrameShape { float cx, cy, hx, hy, radius, feather, border; bool ellipse; };          // Klip pikselinde
FrameShape frame_shape(const json& clip, float w, float h, float unit);
Bitmap frame_mask(const json& clip, int w, int h, float unit);      // Beyaz; saydamlık maske
Bitmap frame_border(const json& clip, int w, int h, float unit);    // Kenarlık çizgisi
Bitmap spot_overlay(const json& clip, int W, int H);                // Spot ışığı: alan dışı karartma
bool clip_reversed(const json& clip);
double source_time(const json& clip, double local);         // Klip içindeki andan kaynak videonun saniyesine
bool clip_local_of(const json& clip, double source, double& local);    // Tersi; klibin dışındaysa false

// Kayıt Stüdyosu'nun kaydın yanına yazdığı imleç, tıklama ve kısayol verisi ("<video>.input.json")
struct InputData {
    int w = 0, h = 0;
    std::vector<std::array<float, 3>> cursor;       // sn, x, y (kayıt tuvali pikseli)
    std::vector<std::array<float, 4>> clicks;       // sn, x, y, düğme (0 sol, 1 sağ)
    std::vector<std::pair<double, std::string>> keys;
};
std::shared_ptr<const InputData> input_data(const std::string& video);     // Yoksa nullptr
bool cursor_at(const InputData& d, double source, float& x, float& y);
bool input_point(const json& clip, const InputData& d, float sx, float sy, double local, float W, float H, float& x, float& y);  // Tuval pikseli
bool clip_uses_input(const json& clip);
Bitmap halo_bitmap(int size, uint32_t rgb);                 // Yumuşak kenarlı daire
Bitmap ring_frames(int size, uint32_t rgb, int frames);     // Yan yana büyüyen halka kareleri, sonda boş kare
Bitmap key_bitmap(const std::string& text, float unit);     // Tuş göstergesi kutusu
// Renk filtresi: .cube dosyası (kırmızı en hızlı değişen)
struct Lut { int size = 0; std::vector<float> rgb; };
bool load_cube(const std::wstring& path, Lut& out);
std::string filter_escape(const std::wstring& path);        // FFmpeg filtresi içinde dosya yolu
// Ses efektleri
struct VoiceInfo { const char* key; const char* name; const char* filter; };
const std::vector<VoiceInfo>& voice_list();
// Altyazı: proje["captions"] = [{start, end, text}], biçim proje["caption_style"]
json default_caption_style();
Bitmap caption_bitmap(const std::string& text, const json& style, float unit);
json parse_srt(const std::string& text);
std::string write_srt(const json& captions);
struct TransitionInfo { const char* key; const char* name; int mask; int slide; };  // mask: gölgelendiricideki tür, slide: 1 sol 2 sağ 3 yukarı 4 aşağı
const std::vector<TransitionInfo>& transition_list();
const TransitionInfo* find_transition(const std::string& key);
const std::vector<std::string>& style_keys();               // Bir klipten diğerine aktarılabilen ayarlar
bool clip_is_visual_still(const json& clip);                // Yazı ve şekil: boyutu çizimden gelir
Bitmap render_still(const json& clip);                      // Yazı ya da şeklin 1080p ölçeğindeki resmi
const json* find_track(const json& project, const std::string& id);
bool clip_has_video(const json& clip);
bool clip_has_audio(const json& clip);

struct MediaInfo {
    double duration = 0, fps = 30;
    int width = 0, height = 0;
    bool video = false, audio = false, image = false;
    std::vector<std::string> audio_titles;      // Her ses parçasının adı (yoksa boş); Kayıt Stüdyosu çoklu parçada ilki "Mix"
};
bool probe_media(const std::wstring& path, MediaInfo& out, std::string& error);

struct Placement { float x = 0, y = 0, w = 0, h = 0; };     // Tuval pikseli
Placement clip_placement(const json& clip, float W, float H);     // Kaynağı tuvale sığdırır, ölçek ve konumu uygular

// Çalışan bir FFmpeg süreci. Program kapanırsa süreç de kapanır.
class Process {
public:
    ~Process() { kill(); }
    bool start(const std::wstring& command, bool capture_stderr = false);
    size_t read(void* data, size_t size);       // Tam olarak size bayt ya da süreç bitene kadar okur
    size_t read_some(void* data, size_t size);
    std::string error_text();
    int wait();
    void kill();
    bool running() const { return process_ != nullptr; }

private:
    HANDLE process_ = nullptr, job_ = nullptr, out_ = nullptr, err_ = nullptr;
    std::thread err_thread_;
    std::mutex lock_;
    std::string err_text_;
};

// Önizlemede bir klibin kareleri. FFmpeg arkada çözer, en çok birkaç kare bekletir.
class VideoFeed {
public:
    ~VideoFeed();
    // reverse_span > 0: source_time'dan geriye bu kadar saniye ters oynatılır
    void start(const std::wstring& file, double source_time, double speed, int fps, int w, int h, double reverse_span = 0);
    bool take(int index, std::vector<uint8_t>& out);    // index. kareyi ya da ondan önceki son kareyi alır

private:
    void loop();
    Process proc_;
    std::thread thread_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::pair<int, std::vector<uint8_t>>> frames_;
    std::atomic<bool> halt_{false};
    size_t frame_size_ = 0;
};

// Mikrofondan WAV dosyasına kayıt
class MicRecorder {
public:
    ~MicRecorder() { stop(); }
    bool start(const std::wstring& path);
    double stop();                  // Kaydın süresini döner
    bool running() const { return thread_.joinable(); }
    double seconds() const { return double(frames_.load()) / 48000.0; }
    float level() const { return level_; }

private:
    void loop(std::wstring path);
    std::thread thread_;
    std::atomic<bool> halt_{false};
    std::atomic<long long> frames_{0};
    std::atomic<float> level_{0};
};

// Zaman çizelgesinin sesini bir noktadan itibaren çalar; önizlemenin saati de budur
class AudioPlayer {
public:
    ~AudioPlayer() { stop(); }
    void start(const std::wstring& command, double from);
    void stop();
    double position() const;        // Zaman çizelgesindeki konum (saniye)
    bool started() const { return started_; }

private:
    void loop(std::wstring command);
    std::thread thread_;
    std::atomic<bool> halt_{false}, started_{false};
    std::atomic<long long> played_{0};
    double from_ = 0;
};

std::wstring audio_preview_command(const json& project, double from);

struct ExportJob {
    std::atomic<float> progress{0};
    std::atomic<bool> done{false}, cancel{false};
    std::string error;
    std::wstring path;
    std::thread thread;
};
// project["export_mode"]: video (varsayılan), gif, frame (tek kare PNG), audio (mp3 / wav / m4a)
std::wstring export_command(const json& project, const EncodeConfig& config, const std::wstring& work_folder, std::string& error);
std::wstring export_audio_command(const json& project, const EncodeConfig& config, const std::wstring& work_folder, std::string& error);
void export_start(ExportJob& job, const json& project, const EncodeConfig& config);
std::vector<float> read_waveform(const std::wstring& file, double duration, int stream = 0);     // Saniyede 100 tepe değeri
bool read_thumbnail(const std::wstring& file, double time, int w, int h, Bitmap& out);
