# Creator Suite

İçerik üreticileri ve oyuncular için Windows uygulama paketi: **kayıt ve canlı yayın**, **video düzenleme** ve **oyun araçları** tek pakette. C++20, Direct3D 11 ve Dear ImGui ile yazılmıştır.

*A Windows suite for creators and gamers — recording & live streaming, video editing and in-game tools. Written in C++20 with Direct3D 11 and Dear ImGui. UI available in 30+ languages.*

## Programlar

| Program | Açıklama |
|---|---|
| **Recording Studio** | Ekran / oyun kaydı ve canlı yayın (RTMP), sahneler, tarayıcı kaynağı, sanal kamera |
| **Video Editor** | Zaman çizelgeli video düzenleyici ve dışa aktarma |
| **Game Tools** | Nişangâh (crosshair), FPS ve donanım göstergesi, performans testleri, mola hatırlatıcı |

Arayüz 30'dan fazla dilde kullanılabilir. Kullanım kılavuzları (Türkçe, İngilizce, Almanca, Fransızca): [`dist/manuals`](dist/manuals)

## İndirme

[**Releases**](../../releases/latest) sayfasından:

- **Kurulum:** `Creator-Suite-Setup.exe`, kurulum sihirbazı ile yükler
- **Portable:** `Creator-Suite-Portable.zip`, kurulum gerektirmez; klasöre çıkarıp çalıştırın
- **Kaynak kodu:** her sürümle birlikte zip / tar.gz olarak

Gereksinim: Windows 10 / 11 (64 bit), DirectX 11 destekli ekran kartı.

## Kaynaktan derleme

Kaynak kodu [`dist/`](dist) klasöründedir.

1. **Visual Studio 2022** (C++ masaüstü geliştirme iş yükü) kurun.
2. FFmpeg ve donanım izleme kütüphaneleri boyutları nedeniyle depoda yoktur: Portable sürümdeki `lib` klasörünü `dist/lib` olarak kopyalayın.
3. Derleyin:
   ```bat
   cd dist
   build.bat hizli    :: yalnızca programlar (Recording Studio, Game Tools, Video Editor, vcam.dll)
   build.bat          :: programlar + kurulum dosyası (Setup)
   ```
   Programlar bir üst klasöre derlenir. Tam derleme, kurulum paketine konacak hazır nişangâh profillerini üst klasördeki `crosshair-profiles.json` dosyasından alır; Portable sürümdeki `preset-profiles.json` dosyasını bu adla kopyalayabilirsiniz.

## Lisans

Bu proje **GNU GPL v3** ile lisanslanmıştır, ayrıntılar için [LICENSE](LICENSE). Kullanılan üçüncü taraf bileşenlerin lisansları [`dist/licenses`](dist/licenses) klasöründedir: FFmpeg (GPL v3), Dear ImGui (MIT), nlohmann/json (MIT), LibreHardwareMonitor (MPL 2.0), WebView2 SDK, Simple Icons.
