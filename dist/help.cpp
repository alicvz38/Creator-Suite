// Kullanım kılavuzları ve iletişim sayfası (iki program da kullanır).
#include "ui.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

namespace {

// İletişim bilgileri ekranda görünmez; exe içinde de düz metin durmasın diye derlenirken karıştırılır
template <size_t N>
struct Hidden {
    char data[N];
    static constexpr char mask(size_t i) { return char(0x5A + i * 7); }
    consteval Hidden(const char (&text)[N]) : data{} {
        for (size_t i = 0; i < N; i++) data[i] = char(text[i] ^ mask(i));
    }
    std::string get() const {
        std::string out;
        for (size_t i = 0; i + 1 < N; i++) out += char(data[i] ^ mask(i));
        return out;
    }
};

constexpr Hidden kWhatsApp("https://wa.me/905521743279");
constexpr Hidden kMail("aliceviz0381998@gmail.com");

bool open_link(const std::string& url) {
    return INT_PTR(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

struct Section {
    const char* title;
    std::vector<const char*> lines;
};

// --- Lisans, gizlilik ve telif yazıları ---

const char* kSourceUrl = "";        // Proje sayfası yayımlanınca adresi buraya yazın
const char* kRecordApp = "Kayıt Stüdyosu";
const char* kEditorApp = "Video Düzenleyici";

void legal_page(const char* title, const char* subtitle, const std::vector<Section>& sections) {
    ImGui::SeparatorText(title);
    ImGui::TextDisabled("%s", T(subtitle));
    ImGui::Spacing();
    for (auto& s : sections) {
        ImGui::PushFont(nullptr, 18.f);
        ImGui::TextColored(theme_accent(), "%s", T(s.title));
        ImGui::PopFont();
        for (auto line : s.lines) ImGui::TextWrapped("%s", T(line));
        ImGui::Spacing(); ImGui::Spacing();
    }
}

void license_page(const char* app, bool installed = true) {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Lisans Sözleşmesi");
    ImGui::PopFont();
    ImGui::TextDisabled("%s, GNU Genel Kamu Lisansı sürüm 3 (GPLv3) ile sunulan açık kaynaklı bir programdır.", app);
    ImGui::Spacing();
    if (installed) {
        if (ImGui::Button("Tam lisans metnini aç (İngilizce)")) open_link(narrow(exe_dir() + L"\\licenses\\LICENSE.txt"));
        ImGui::SameLine();
        if (ImGui::Button("Lisanslar klasörünü aç")) open_link(narrow(exe_dir() + L"\\licenses"));
        if (kSourceUrl[0]) {
            ImGui::SameLine();
            if (ImGui::Button("Kaynak kodunu aç")) open_link(kSourceUrl);
        }
    } else {
        ImGui::TextDisabled("İngilizce tam metin, kurulumdan sonra \"licenses\" klasöründe ve Yardım sayfasındadır.");
    }
    ImGui::Spacing();
    ImGui::BeginChild("lisans", ImVec2(0, 0));
    legal_page("Özet", "Bu özet kolay anlaşılması içindir. Bağlayıcı olan, İngilizce tam lisans metnidir.", {
        {"Ücretsiz ve açık kaynak", {
            "Bu program ücretsizdir. Geliştiriciler programı satmaz; lisans ücreti, abonelik, ücretli sürüm ya da uygulama içi satın alma yoktur.",
            "Kurulum dosyasını ücret karşılığı aldıysanız, bu ücret geliştiricilere ulaşmamıştır. Aynı programa ücretsiz olarak ulaşabilirsiniz.",
        }},
        {"Haklarınız", {
            "Programı kişisel ya da ticari her amaçla, istediğiniz kadar bilgisayarda kullanabilirsiniz. Program ile ürettiğiniz kayıtlar ve yayınlar bu lisansa tabi değildir.",
            "Kaynak kodunu inceleyebilir, değiştirebilir ve programı değiştirerek ya da değiştirmeden başkalarıyla paylaşabilirsiniz.",
        }},
        {"Paylaşırken uymanız gerekenler", {
            "Programı ya da değiştirilmiş halini dağıtırsanız aynı GPLv3 lisansını korumalı, kaynak kodunu da alıcılara sunmalısınız.",
            "Telif hakkı bildirimlerini ve lisans metinlerini kaldıramazsınız. Değiştirdiğiniz sürümde değişiklik yaptığınızı belirtmelisiniz.",
            "Program içindeki üçüncü taraf bileşenlerin kendi lisansları geçerlidir. Bunları Telif Hakları sekmesinde bulabilirsiniz.",
        }},
        {"Garanti yoktur", {
            "Program, yasaların izin verdiği ölçüde olduğu gibi ve hiçbir garanti verilmeden sunulur. Belirli bir amaca uygunluğu, hatasız çalışacağı ya da kesintisiz olacağı garanti edilmez.",
            "Geliştiriciler, yasaların izin verdiği ölçüde, programın kullanımından doğan veri kaybı, kayıt ya da yayın kesintisi ve benzeri zararlardan sorumlu değildir. Önemli kayıtlarınızı yedekleyin.",
        }},
        {"Kullanım sorumluluğu", {
            "Kaydettiğiniz ve yayınladığınız içerikten siz sorumlusunuz. Başkalarının görüntü, ses ve eserlerini kullanırken kişilik haklarına ve telif haklarına uymalısınız.",
            "Yayın platformlarının ve oyunların kurallarına uymak size aittir. Ekran crosshair'i gibi araçlar bazı rekabetçi oyunlarda ya da turnuvalarda yasak olabilir.",
        }},
    });
    ImGui::EndChild();
}

void privacy_page(const char* app) {
    const bool record = !strcmp(app, kRecordApp), editor = !strcmp(app, kEditorApp);
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Gizlilik");
    ImGui::PopFont();
    ImGui::TextDisabled("%s verilerinizi bilgisayarınızda tutar ve geliştiricilere hiçbir veri göndermez.", app);
    ImGui::Spacing();
    ImGui::BeginChild("gizlilik", ImVec2(0, 0));
    std::vector<Section> s = {
        {"Toplanan veriler", {
            "Geliştiriciler sizden veri toplamaz. Programda hesap, reklam, kullanım istatistiği, hata raporu gönderimi ya da otomatik güncelleme denetimi yoktur.",
        }},
        {"Bilgisayarınızda saklananlar", {
            editor ? "Projeleriniz ve dışa aktardığınız videolar yalnızca seçtiğiniz klasöre yazılır. Beklenmedik kapanmalara karşı son çalışmanın bir kopyası ayar klasöründe tutulur."
            : record ? "Ayarlarınız, sahneleriniz ve profilleriniz ayar dosyalarında yalnızca bilgisayarınızda saklanır. Kayıtlarınız seçtiğiniz klasöre yazılır."
                   : "Ayarlarınız, crosshair profilleriniz ve FPS göstergesi ayarlarınız ayar dosyalarında yalnızca bilgisayarınızda saklanır.",
            "Programı kaldırırken ayar dosyalarını da silmeyi seçebilirsiniz.",
        }},
    };
    if (editor) {
        s.push_back({"Dosyalarınız", {
            "Program yalnızca eklediğiniz dosyaları okur. Dosyalarınız değiştirilmez ve hiçbir yere gönderilmez; işlemler FFmpeg ile bilgisayarınızda yapılır.",
        }});
    } else if (record) {
        s.push_back({"Donanım bilgileri", {
            "FPS göstergesi kaynağını eklerseniz işlemci, ekran kartı, bellek ve disk değerleri bilgisayarınızda okunur. Bu değerler kayda ya da yayına yalnızca gösterge sahnede görünüyorsa girer.",
            "İşlemci sıcaklığı için gereken PawnIO sürücüsü isteğe bağlıdır; yalnızca kurulumda işaretlerseniz internetten indirilip kurulur.",
        }});
        s.push_back({"Ekran, kamera ve mikrofon", {
            "Program yalnızca sahneye eklediğiniz kaynakları ve açtığınız ses girişlerini kullanır. Önizleme için bu görüntü ve ses bilgisayarınızda anlık işlenir.",
            "Kayıt ya da yayın başlatmadığınız sürece görüntü ve ses diske yazılmaz, hiçbir yere gönderilmez.",
            "Kayıtlara ekrandaki parola, mesaj gibi özel bilgiler de girebilir. Paylaşmadan önce kontrol edin.",
            "Ekran görüntüleri yalnızca siz istediğinizde alınır ve kayıt klasörüne yazılır. Mikrofon filtreleri ve dinleme bilgisayarınızda çalışır; ses hiçbir yere gönderilmez.",
        }});
        s.push_back({"İmleç ve kısayol verisi", {
            "Ayarlar > Kayıt'taki kutular açıksa kayıt sırasında imleç yeri, tıklama anları ve (yalnızca Ctrl, Alt, Win ile basılan) kısayol tuşları kaydın yanına \".input.json\" dosyasına yazılır.",
            "Düz yazı, şifre ve mesajlar kaydedilmez. Bu dosya yalnızca Video Düzenleyici'nin imleç ve tuş efektleri için kullanılır, hiçbir yere gönderilmez; silebilirsiniz.",
        }});
        s.push_back({"Canlı yayın", {
            "Yayın başlatınca görüntü ve ses yalnızca Ayarlar > Yayın bölümünde girdiğiniz sunucuya gönderilir. Program yayın için başka bir sunucuya bağlanmaz.",
            "Yayın platformu verileri kendi gizlilik politikasına göre işler; bundan geliştiriciler sorumlu değildir.",
            "Yayın anahtarınız Windows hesabınıza bağlı olarak şifrelenmiş saklanır ve yalnızca seçtiğiniz sunucuya gönderilir.",
        }});
    } else {
        s.push_back({"Donanım bilgileri", {
            "FPS göstergesi için işlemci, ekran kartı, bellek, disk ve pil değerleri bilgisayarınızda okunur. Bu değerler hiçbir yere gönderilmez ve dosyaya kaydedilmez.",
            "Yönetici yetkisi yalnızca sıcaklık gibi değerleri okumak ve otomatik başlatmayı kurmak için istenir. PawnIO sürücüsü isteğe bağlıdır; yalnızca kurulumda işaretlerseniz internetten indirilip kurulur.",
        }});
        s.push_back({"Oyunlar", {
            "Program oyunların dosyalarını ya da belleğini okumaz, oyunlara kod eklemez. Göstergeler ekranda ayrı bir pencere olarak çizilir.",
        }});
    }
    s.push_back({"Bağlantılar", {
        record || editor ? "İletişim sekmesindeki düğmeler bilgisayarınızdaki tarayıcıyı, WhatsApp'ı ya da e-posta programını açar. Bu hizmetlerin kendi gizlilik koşulları geçerlidir."
               : "İletişim sekmesindeki düğmeler ve PawnIO indirme düğmesi tarayıcınızı, WhatsApp'ı ya da e-posta programını açar. Bu hizmetlerin kendi gizlilik koşulları geçerlidir.",
        "Bize yazdığınızda paylaştığınız bilgiler yalnızca size yanıt vermek için kullanılır.",
    }});
    if (!editor)
        s.push_back({"Otomatik başlatma", {
            "Otomatik başlatma yalnızca siz kutuyu işaretlerseniz açılır, kutuyu kaldırınca kapanır.",
        }});
    legal_page("Gizlilik bildirimi", "Bu bildirim programın şu anki sürümünü anlatır.", s);
    ImGui::EndChild();
}

void copyright_page(const char* app) {
    const bool record = !strcmp(app, kRecordApp) || !strcmp(app, kEditorApp);
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Telif Hakları");
    ImGui::PopFont();
    ImGui::TextDisabled("%s  ·  Telif hakkı © 2026 Ali Ceviz ve Vahdet Göl", app);
    ImGui::Spacing();
    ImGui::BeginChild("telif", ImVec2(0, 0));
    std::vector<Section> s = {
        {"Program", {
            "Kaynak kodu ve arayüz tasarımı Ali Ceviz ve Vahdet Göl'e aittir ve GPLv3 ile lisanslanmıştır.",
            kSourceUrl[0] ? kSourceUrl : "Kaynak kodu talep etmek için İletişim sekmesinden bize ulaşabilirsiniz.",
        }},
        {"Kullanılan açık kaynak bileşenler", {
            "Dear ImGui, arayüz. Telif hakkı © Omar Cornut. MIT Lisansı.",
            "nlohmann/json, ayar dosyaları. Telif hakkı © Niels Lohmann. MIT Lisansı.",
        }},
    };
    s[1].lines.push_back("FFmpeg, video ve ses kodlama (lib\\ffmpeg.exe). Telif hakkı © FFmpeg geliştiricileri. GPLv3. gyan.dev derlemesi 9.0.2; x264, x265, libaom ve libvpx kodlayıcılarını içerir. Kaynak kodu: github.com/FFmpeg/FFmpeg (946fcce07b).");
    s[1].lines.push_back("LibreHardwareMonitor ve birlikte gelen yardımcı kütüphaneler, donanım sensörleri (lib klasörü). Mozilla Kamu Lisansı 2.0 ve her kütüphanenin kendi lisansı.");
    s[1].lines.push_back("Simple Icons, sosyal medya logoları. CC0 1.0.");
    s[1].lines.push_back("Lisans metinleri kurulum klasöründeki \"licenses\" klasöründedir.");
    if (record)
        s.push_back({"Markalar", {
            "YouTube, Twitch, Kick, Instagram, TikTok, X, Facebook ve Discord adları ile logoları kendi sahiplerinin markalarıdır ve yalnızca bu hizmetleri tanıtmak için kullanılır.",
            "Bu program adı geçen şirketlerle bağlantılı değildir; onlar tarafından desteklenmez ya da onaylanmaz.",
        }});
    s.push_back({record ? "Kaydettiğiniz içerik" : "Oyunlar ve markalar", {
        record ? "Program ile oluşturduğunuz kayıtların hakları size aittir. Kayıtlardaki oyun, müzik, film ve benzeri içeriklerin hakları ise sahiplerine aittir; bunları paylaşmadan önce izinleri kontrol edin."
               : "Oyun adları ve görselleri sahiplerine aittir. Bu program hiçbir oyun yapımcısıyla bağlantılı değildir.",
        "Windows, Microsoft Corporation'ın ticari markasıdır.",
    }});
    if (record)
        s.push_back({"Patentler", {
            "H.264, HEVC gibi video biçimleri bazı ülkelerde patent kapsamında olabilir. Program bu konuda bir izin ya da güvence vermez.",
        }});
    legal_page("Bildirimler", "Üçüncü taraf bileşenler kendi sahiplerinin eseridir.", s);
    ImGui::EndChild();
}

}  // namespace

const std::vector<Section>& record_sections() {
    static const std::vector<Section> s = {
        {"Hızlı başlangıç", {
            "Program ilk açıldığında Ekran kaynağı olan bir sahne hazırdır; önizlemede ekranınızı görürsünüz.",
            "Sağ alttaki Kontroller bölümünden \"Kaydı Başlat\"a basın ya da Ctrl + F12 kısayolunu kullanın.",
            "Bitirmek için \"Kaydı Durdur\"a basın. Kayıt, Ayarlar > Kayıt bölümünde seçtiğiniz klasöre MP4 olarak kaydedilir.",
            "Kayıt klasörünü açmak için üstteki Dosya > Kayıt klasörünü aç menüsünü kullanın.",
            "Sağ üstteki Video Düzenleyici düğmesi düzenleme programını açar. Araçlar > Son kaydı Video Düzenleyici'de aç ile kaydınız doğrudan zaman çizelgesine eklenir.",
            "Bölmelerin arasındaki çizgiyi sürükleyerek boyutlarını değiştirin; boyutlar kendiliğinden saklanır. Görünüm menüsünden hazır düzenleri seçebilir ya da kendi düzeninizi adıyla kaydedebilirsiniz.",
        }},
        {"Sahneler", {
            "Sahne, kaynakların bir arada durduğu düzendir. Örneğin Oyun ve Bekleme ekranı için iki ayrı sahne yapabilirsiniz.",
            "Sol üstteki Sahneler bölümünde artı düğmesi yeni sahne ekler, çöp kutusu seçili sahneyi siler, yanındaki düğme adını değiştirir.",
            "Listede bir sahneye tıklayınca ona geçilir; bunu kayıt sürerken de yapabilirsiniz.",
            "Geçişin görünümünü Sahne Geçişi bölümünden seçin: Soldur yumuşak geçer, Kes anında değiştirir.",
        }},
        {"Kaynaklar", {
            "Kaynak, sahnede görünen şeydir. Kaynaklar bölümündeki artı düğmesiyle ekleyin: Ekran, Ekran bölgesi, Pencere, Oyun yakalama, FPS göstergesi, Kamera, Resim, Video, Metin, Sosyal medya, Düz renk, Tarayıcı, Slayt gösterisi, Sahne.",
            "Listede üstte duran kaynak önde görünür. Sıralamayı ok düğmeleriyle değiştirin.",
            "Soldaki onay kutusu kaynağı silmeden gizler.",
            "Ayarlarını açmak için kaynağa çift tıklayın ya da önizlemenin altındaki Özellikler düğmesine basın.",
        }},
        {"Oyun yakalama", {
            "Kaynaklar bölümündeki artıdan Oyun yakalama'yı ekleyin. Özellikler'de oyunu listeden seçin ya da \"Tam ekran oyunu kendiliğinden bul\" seçeneğini bırakın.",
            "Oyunun penceresi Windows üzerinden alınır, oyuna hiçbir şey enjekte edilmez. Bu yüzden hile koruması olan oyunlarda güvenlidir.",
            "Üstte duran başka pencereler yayına girmez. Oyun Araçları'ndaki crosshair ve FPS göstergesi de bu kaynakta görünmez. FPS'in kayıtta ve yayında görünmesi için sahneye FPS göstergesi kaynağını ekleyin.",
            "Oyunu kenarlıksız pencere modunda çalıştırmak en sorunsuz yoldur. Özel tam ekran oyun görüntü vermezse kaynak kendiliğinden ekran yakalamaya geçer.",
        }},
        {"Pencere ve ekran bölgesi", {
            "Pencere kaynağı yalnızca seçtiğiniz programın penceresini alır; önüne gelen başka pencereler ve bildirimler kayda girmez. Özellikler'de pencereyi listeden seçin.",
            "\"Yalnızca pencere içeriği\" açıkken başlık çubuğu ve kenarlar alınmaz. Pencere simge durumuna küçültülürse görüntü gelmez.",
            "Ekran bölgesi kaynağı ekranın bir dikdörtgenini alır. Eklenince program küçülür, kaydedilecek alanı fareyle çizersiniz; Esc vazgeçer. Daha sonra Özellikler > \"Ekranda seç...\" ile yeniden seçebilir ya da değerleri elle yazabilirsiniz.",
        }},
        {"Kaynak filtreleri", {
            "Her kaynağın Özellikler penceresinin altında filtreler vardır. Kayıtta, yayında ve önizlemede aynı anda uygulanır.",
            "Kırpma: kenarlardan yüzde olarak keser. Renk düzeltme: parlaklık, kontrast, doygunluk, gama ve renk tonu.",
            "Yeşil perde: anahtar rengini (yeşil ya da mavi) saydam yapar. Perde tam kaybolmuyorsa benzerliği, kenarlar sertse yumuşaklığı artırın. \"Renk taşmasını azalt\" yüzdeki yeşil yansımayı giderir.",
            "Keskinleştirme bulanık kamerayı netleştirir. Maske kaynağı yuvarlak köşeli ya da daire yapar; daire kamera için kutuyu kare yapın.",
        }},
        {"FPS göstergesi", {
            "Kaynaklar bölümündeki artıdan FPS göstergesi'ni ekleyin. Oyun Araçları'ndaki görünümünüzle başlar, sahnenin parçası olduğu için kayıtta ve yayında görünür.",
            "Önizlemede sürükleyerek istediğiniz yere taşıyın. Neyin görüneceğini ve görünümünü Özellikler penceresinden değiştirin; bu ayarlar Oyun Araçları'nı etkilemez.",
            "CPU sıcaklığı için program yönetici olarak açılmalıdır. Yetki yoksa bazı değerler görünmez.",
        }},
        {"Önizlemede düzenleme", {
            "Önizlemede bir kaynağa tıklayarak seçin ve sürükleyerek taşıyın. Kenarlara ve ortaya yaklaşınca yerine oturur.",
            "Ekran, kamera, resim, video ve düz rengin köşelerindeki kırmızı kareleri çekerek boyutlandırın; oran korunur.",
            "Özellikler penceresindeki \"Tuvali kapla\" kaynağı tüm alana yayar.",
            "Sol üstteki menü düğmesi sol paneli kapatıp önizlemeyi büyütür.",
        }},
        {"Ses", {
            "Ses Karıştırıcı'dan bilgisayar sesini ve mikrofonu açıp kapatın, seviyelerini kaydırıcıyla ayarlayın.",
            "Yeşil çubuk o anki ses seviyesini gösterir. Hareket etmiyorsa o kaynaktan ses gelmiyordur.",
            "Hangi ses aygıtının kullanılacağını Ayarlar > Kayıt bölümünden seçin.",
            "Hoparlör düğmesi sesi susturur; kırmızıysa o kaynak susturulmuştur.",
        }},
        {"Mikrofon filtreleri, dinleme ve bas-konuş", {
            "Ses Karıştırıcı'da mikrofonun yanındaki filtre düğmesi Mikrofon ayarları penceresini açar. Değişiklikler kayıt sürerken de anında geçerlidir.",
            "Kazanç sesi yükseltir ya da alçaltır. Gürültü bastırma fan, klima ve uğultu gibi sürekli sesleri azaltır; açtıktan sonra birkaç saniye sessiz kalın.",
            "Gürültü kapısı konuşmadığınızda mikrofonu kısar. Penceredeki düzey çubuğunda beyaz çizgi açılma, gri çizgi kapanma eşiğidir: konuşurken çubuk beyazı geçmeli, sessizken griyi geçmemeli.",
            "Kompresör yüksek sesleri bastırıp sesinizi dengeler. Dinleme, mikrofonun kayda giden halini kulaklıktan duymanızı sağlar; \"Yalnızca dinle\" seçilirse mikrofon kayda ve yayına girmez.",
            "Bas-konuş açıkken mikrofon yalnızca atanan tuş basılı tutulunca duyulur. Tuşu Ayarlar > Kısayollar'dan atayın; fare yan tuşları da olur.",
        }},
        {"Ses girişleri ve uygulama sesi", {
            "Ses Karıştırıcı'nın altındaki \"Ses kaynağı ekle\" ile en çok 8 ek ses girişi eklenir: ikinci bir mikrofon ya da ses kartı girişi, bir hoparlöre giden ses ya da yalnızca bir programın sesi.",
            "Uygulama sesi yalnızca seçtiğiniz programı alır (örneğin oyun, Discord ya da tarayıcı); diğer sesler girmez. Program kapalıysa açılınca kendiliğinden bağlanır. Windows 10 (2004) ve sonrası gerekir.",
            "Bilgisayar sesi zaten bütün sesleri içerir. Bir programı ayrıca alıyorsanız sesin iki kez girmemesi için bilgisayar sesini kapatın.",
            "Her girişin ayar düğmesinden adını, aygıtını ya da programını değiştirir, kaldırırsınız. Kayıt, yayın ya da tekrar arabelleği açıkken girişler değiştirilemez; ses düzeyi ve susturma her zaman değişir.",
        }},
        {"Çoklu ses parçası", {
            "Ayarlar > Kayıt > \"Her ses kaynağını ayrı parçaya kaydet\" açıkken ilk parça karışım olur; bilgisayar sesi, mikrofon ve her ek giriş ayrı parçaya yazılır.",
            "Video Düzenleyici böyle bir kaydı açınca her parçayı ayrı ses kanalına koyar; oyun sesini, sesinizi ve Discord'u ayrı ayrı ayarlarsınız.",
            "FFmpeg kodlayıcılarında (x264, Quick Sync, NVENC, AMF...) ve MP4, MKV, MOV biçimlerinde çalışır. Windows kodlayıcısında tek parça kaydedilir.",
        }},
        {"Tekrar arabelleği", {
            "Kontroller bölümündeki \"Tekrar\" düğmesi tekrar arabelleğini açar. Açıkken sahnenin son saniyeleri (varsayılan 30) geçici klasörde sürekli tutulur.",
            "Güzel bir an yakaladığınızda \"Tekrarı kaydet\"e ya da Ctrl + F8'e basın: o ana kadarki son saniyeler kayıt klasörüne \"Replay ...mp4\" olarak yazılır.",
            "Süreyi Ayarlar > Kayıt > Tekrar arabelleği bölümünden değiştirin. Ayrı bir kodlayıcı çalıştığı için biraz daha işlemci ya da ekran kartı kullanır; kayıt ve yayınla birlikte açık olabilir.",
        }},
        {"Stüdyo modu", {
            "Sahne Geçişi bölümündeki \"Stüdyo modu\" açılınca önizleme ikiye bölünür: solda düzenlediğiniz sahne, sağda kırmızı çerçeveyle kayda ve yayına giden sahne.",
            "Sahne listesinden seçtiğiniz sahne solda açılır; kaynakları rahatça düzenleyin. Hazır olunca \"Yayına al\" ile geçin. Geçiş türü (Soldur / Kes) burada da geçerlidir.",
            "Kısayollar sayfasından \"Stüdyo modu: önizlemeyi yayına al\" için tuş atayabilirsiniz.",
        }},
        {"Kaydı bölme", {
            "Ayarlar > Kayıt > Kaydı bölme: süreye ya da dosya boyutuna göre seçin. Süre ya da boyut dolunca kayıt durmadan yeni dosyaya geçer.",
            "Uzun yayın kayıtlarında dosyalar küçük kalır, bir sorun olursa yalnızca son parça etkilenir. Durum çubuğunda kaçıncı parçada olduğunuz görünür.",
        }},
        {"İstatistikler", {
            "Araçlar > İstatistikler penceresi işlemci, ekran kartı ve bellek kullanımını, çizim hızını, kayıt ve yayın bit hızını, atlanan kareleri ve kayıt klasöründeki boş alanı gösterir.",
            "Çizim hızı hedefin altına düşerse ya da atlanan kare artarsa satır sarıya döner: FPS'i düşürün ya da daha hafif bir kodlayıcı seçin. Boş alan 5 GB'ın altına inerse kırmızı olur.",
        }},
        {"Tarayıcı kaynağı", {
            "Kaynak ekle > Tarayıcı: bağış ve takipçi uyarıları, sohbet, geri sayım gibi web sayfalarını sahneye ekler. Özellikler'de sayfanın adresini (StreamElements, Streamlabs...) ya da bilgisayarınızdaki bir HTML dosyasını seçin.",
            "Sayfanın saydam yerleri sahnede de saydamdır; uyarılar oyunun üstünde görünür. Sayfa genişliği ve yüksekliği, sayfanın hangi boyutta çizileceğidir; kutuyu önizlemede büyütüp küçültebilirsiniz.",
            "Özel CSS sayfaya stil ekler (varsayılanı arka planı saydam yapar). Sayfa takılırsa \"Sayfayı yenile\"ye basın. Sayfanın sesi hoparlörden çalar; bilgisayar sesi açıksa kayda ve yayına girer.",
            "Microsoft Edge WebView2 Runtime kullanılır; Windows 11'de hazır kuruludur.",
        }},
        {"Sanal kamera", {
            "Kontroller'deki \"Sanal kamera\" düğmesi sahnenizi bir kamera gibi kullanılabilir yapar: Discord, Zoom, Google Meet, Teams gibi uygulamalarda kamera olarak \"Recording Studio Camera\"yı seçin.",
            "İlk kullanımda bir kez yönetici izni istenir; kamera sürücüsü ProgramData klasörüne kopyalanıp Windows'a tanıtılır. Windows 11 gerekir.",
            "Görüntü 1920 x 1080, 30 FPS gider. Kayıt Stüdyosu kapalıyken kamera koyu bir ekran gösterir. Kaldırmak için Araçlar > Sanal kamerayı kaldır.",
        }},
        {"Aynı anda birden fazla platforma yayın", {
            "Ayarlar > Yayın > \"Aynı anda başka platformlara\" bölümünden en çok 4 ek hedef ekleyin (örneğin Twitch ana yayın, YouTube ve Kick ek hedef). Her hedefin kendi sunucu adresi ve yayın anahtarı vardır.",
            "\"Yayını Başlat\" hepsini birlikte başlatır. Bir hedef koparsa yalnızca o kapanır, diğerleri sürer; durumları İstatistikler penceresinde görünür.",
            "Her hedef ayrı kodlanır ve ayrı gönderilir: yükleme hızınız hedef sayısı kadar bit hızını kaldırmalıdır.",
        }},
        {"Yayın gecikmesi", {
            "Ayarlar > Yayın > Yayın gecikmesi: yayın belirlediğiniz saniye kadar geriden gönderilir. Rakiplerin yayınınızdan yerinizi öğrenmesini (stream sniping) zorlaştırır.",
            "Kayıt gecikmeden etkilenmez. Gecikme süresince veri bellekte bekler.",
        }},
        {"Gruplar ve sahne içinde sahne", {
            "\"Sahne\" kaynağı başka bir sahnenin bütün kaynaklarını tek parça olarak gösterir: kamera ve logoyu bir sahnede hazırlayıp her sahneye ekleyin, bir yerde değiştirince her yerde değişir.",
            "Kaynaklar bölümündeki grup düğmesiyle birkaç kaynağı seçip gruplayın; grup tek parça taşınır ve boyutlanır. Grubu seçip aynı düğmeyle çözebilirsiniz.",
        }},
        {"Oynatma listesi ve slayt gösterisi", {
            "Video kaynağının Özellikler'inde \"Listeye video ekle\" ile birden fazla video sırayla oynar; \"Bitince başa sar\" listeyi döngüye alır, \"Karışık sıra\" sırayı karıştırır. Yayın öncesi bekleme ekranı için kullanışlıdır.",
            "Slayt gösterisi kaynağı seçtiğiniz resimleri ya da bir klasördeki resimleri belirlediğiniz sürede, yumuşak geçişle sırayla gösterir.",
        }},
        {"Ses: yalnızca kayıt ya da yalnızca yayın", {
            "Ayarlar > Kayıt > Ses bölümünde bilgisayar sesinin ve mikrofonun nereye gideceğini seçin: kayıt ve yayın, yalnızca kayıt ya da yalnızca yayın. Ek ses kaynaklarında bu seçim ayar düğmesindedir.",
            "Örneğin telifli müziği yalnızca yayına verip kayda almazsanız kaydı sonradan sorunsuz paylaşırsınız.",
        }},
        {"Ekran görüntüsü", {
            "Önizlemenin altındaki \"Ekran görüntüsü\" düğmesi ya da Ctrl + F9 tuvalin o anki karesini PNG olarak kayıt klasörüne kaydeder.",
            "Görüntü, kayda giren sahnenin aynısıdır: filtreler ve gizli kaynaklar dahil.",
        }},
        {"Kayıt ayarları", {
            "Çözünürlük: Ekranla aynı seçeneği en net sonucu verir.",
            "FPS: Oyunlarda 60 FPS, daha küçük dosya için 30 FPS seçin.",
            "Kalite: Yüksek daha net ama daha büyük dosya demektir. Düşük, disk yerini korur.",
            "Kayıt sürerken bu ayarlar değiştirilemez; kayıt bitince yeni kayıtta geçerli olur.",
            "\"Windows başlayınca otomatik başlat\" kutusu programı oturum açılınca görev çubuğunda simge olarak başlatır. Kayıt ya da yayın kendiliğinden başlamaz.",
        }},
        {"Video Düzenleyici için imleç ve kısayol verisi", {
            "\"İmleç ve tıklamaları kaydet\" açıksa kayıt sırasında imlecin yeri ve tıklama anları kaydın yanına \".input.json\" dosyası olarak yazılır. Video Düzenleyici bununla imleç vurgusu ve tıklama halkası çizer.",
            "\"Kısayol tuşlarını kaydet\" açıksa yalnızca Ctrl, Alt ya da Win ile basılan tuşlar, F tuşları ve Esc kaydedilir; yazdığınız metinler ve şifreler kaydedilmez. Video Düzenleyici bunları tuş göstergesi olarak gösterir.",
            "Veri yalnızca ekran kaynağı sahnedeyken toplanır ve hiçbir yere gönderilmez. İstemiyorsanız kutuları kapatın ya da dosyayı silin; video etkilenmez.",
        }},
        {"Profiller", {
            "Profil her şeyi bir arada saklar: sahneler ve kaynaklar, kayıt ve yayın ayarları, ses ayarları, tema ve panel boyutları. Farklı işler için ayrı profiller açın: örneğin oyun yayını, eğitim videosu, sohbet.",
            "Profiller arasında Kontroller bölümündeki profil listesinden ya da menüdeki Profil başlığından geçin. Yaptığınız her değişiklik açık profile kendiliğinden kaydedilir.",
            "Ayarlar > Profiller sayfasında yeni profil açar, çoğaltır, yeniden adlandırır, siler; dışa aktarıp başka bilgisayara taşırsınız. Yeni profil şu anki ayarlarla, boş bir sahneyle başlar; çoğaltınca sahneler de kopyalanır.",
            "Kısayol tuşları bütün profillerde ortaktır. Dışa aktarılan profile yayın anahtarları girmez. Kayıt ya da yayın sürerken profil değiştirilemez.",
        }},        {"Kodlayıcılar", {
            "Kodlayıcıyı Ayarlar > Kayıt > Kodlayıcı bölümünden seçin. Bu bilgisayarda çalışmayanlar listede soluk görünür.",
            "\"En iyisini seç\" bu bilgisayardaki en uygun kodlayıcıyı ve başlangıç ayarlarını seçer.",
            "Donanım kodlayıcılar (Windows, Quick Sync, NVENC, AMF) ekran kartında çalışır, oyunu en az etkiler.",
            "Yazılım kodlayıcılar (x264, x265, AOM AV1, VP9) işlemcide çalışır. Daha küçük dosya ve yüksek kalite verir ama işlemciyi yorar.",
            "Sabit kalite kayıt için en iyisidir: değer düştükçe kalite ve dosya boyutu artar. Yayında her zaman CBR kullanılır.",
            "Ön ayar yavaşladıkça kalite artar, işlemci yükü de artar. Sağ altta \"Atlanan kare\" artarsa daha hızlı bir ön ayar seçin.",
            "MKV kayıt sırasında bozulmaya en dayanıklı biçimdir. MP4 ve MOV parçalı yazılır, program kapansa da o ana kadarki kayıt açılır.",
        }},
        {"Canlı yayın", {
            "Ayarlar > Yayın bölümünden platformunuzu seçin ve yayın anahtarınızı girin. Kick için sunucu adresini de panelinizden kopyalayın.",
            "Sağ alttaki \"Yayını Başlat\"a basın ya da Ctrl + F10 kısayolunu kullanın. Durum Kontroller bölümünde görünür.",
            "Yayın, önizlemede gördüğünüz sahneyi ve sesi gönderir. Kayıt ve yayını aynı anda açabilirsiniz.",
            "\"Kayıt ayarlarıyla aynı\" seçeneği kayıttaki çözünürlük ve FPS değerlerini kullanır. \"Özel\" ile yayına ayrı çözünürlük, FPS ve bit hızı verebilirsiniz.",
            "İnternetiniz yavaşlarsa program kare atlayarak yayını sürdürür; bağlantı koparsa kendiliğinden yeniden dener. Sağ altta bit hızını ve atlanan kareleri görürsünüz.",
            "Yayın anahtarı kanalınızı yönetmeye yarar; kimseyle paylaşmayın.",
        }},
        {"Kısayollar ve ekran", {
            "Ctrl + F12: Kaydı başlat / durdur. Ctrl + F11: Duraklat / devam et. Ctrl + F10: Yayını başlat / durdur. Bunlar program arka plandayken de çalışır.",
            "Ctrl + F9: Ekran görüntüsü. Ayarlar > Kısayollar bölümünden tuşları değiştirebilir; sahneye geçiş, kaynak göster / gizle, mikrofonu ya da bilgisayar sesini susturma ve bas-konuş için de tuş atayabilirsiniz.",
            "F11 tam ekrana geçer, Esc tam ekrandan çıkar.",
            "Tema ve mod Ayarlar > Görünüm bölümünden seçilir: Klasik ya da Bootstrap; Sistem varsayılanı, Koyu ya da Açık. Sistem varsayılanı Windows'un ayarını izler.",
        }},
        {"Sorun giderme", {
            "Önizleme siyahsa: Kaynaklar listesinde Ekran kaynağının işaretli olduğuna bakın.",
            "Kayıtta ses yoksa: Ses Karıştırıcı'da kutunun işaretli, yeşil çubuğun hareketli olduğunu kontrol edin.",
            "Sağ altta \"Atlanan kare\" artıyorsa bilgisayar zorlanıyordur: FPS'i 30'a düşürün, daha hızlı bir ön ayar ya da donanım kodlayıcı seçin.",
            "Crosshair veya FPS göstergesi mi arıyorsunuz? Onlar Oyun Araçları programındadır ve açıkken kayda kendiliğinden girer.",
        }},
    };
    return s;
}

const std::vector<Section>& editor_sections() {
    static const std::vector<Section> s = {
        {"Hızlı başlangıç", {
            "Dosyaları pencereye sürükleyin ya da Medya ekle düğmesini kullanın. Video, ses ve resim eklenebilir.",
            "Medya listesindeki dosyayı zaman çizelgesine sürükleyin ya da çift tıklayın.",
            "Boşluk tuşuyla oynatın. Bitince Dosya > Dışa aktar ile videonuzu oluşturun.",
            "Projenizi Ctrl + S ile kaydedin; daha sonra Dosya > Proje aç ile devam edersiniz.",
            "Bölmelerin arasındaki çizgiyi sürükleyerek boyutlarını değiştirin; boyutlar kendiliğinden saklanır. Görünüm menüsünden hazır düzenleri seçebilir ya da kendi düzeninizi adıyla kaydedebilirsiniz.",
        }},
        {"Zaman çizelgesi", {
            "Zamanı taşımak için yalnızca üstteki cetvele tıklayın. Klibe tıklamak onu seçer; Ctrl ile tıklayarak ya da boş yerden kutu çizerek birden çok klip seçersiniz. Ctrl + A hepsini seçer.",
            "Seçili klipleri sürükleyerek birlikte taşıyın. Sağ tıklayınca bölme, çoğaltma, geçiş ve ayar aktarma menüsü açılır.",
            "Klibin sol ya da sağ kenarını sürükleyerek kırpın. Kenarlar diğer kliplere, işaretçiye ve işaretlere mıknatıs gibi tutar.",
            "Dolu bir yere bırakılan klip eski yerine döner.",
            "Ctrl + tekerlek yakınlaştırır, tekerlek yana kaydırır. Üstteki düğmelerle yeni görüntü ve ses kanalı ekleyin.",
            "Kanal başındaki simgeler: göz kanalı gizler (ses kanalında hoparlör susturur), kilit yanlışlıkla taşımayı önler, zincir mıknatısı açar. Üzerine gelince ne yaptıkları yazar.",
            "Sağ üstteki Kayıt Stüdyosu düğmesi kayıt programına geçer. Kayıt Stüdyosu'nda Araçlar > Son kaydı Video Düzenleyici'de aç, kaydınızı buradaki zaman çizelgesinin sonuna ekler.",
        }},
        {"Aralık seçimi", {
            "Cetvelde Shift tuşunu basılı tutup sürükleyin ya da I ile başını, O ile sonunu işaretçinin olduğu yere koyun. Aralık yeşil görünür; kenarlarını cetvelde sürükleyerek değiştirin.",
            "Aralığı oynat (P) yalnızca o kısmı oynatıp durur; Döngü açıksa başa sarıp tekrar oynatır.",
            "Aralığı sil, aralığı bütün kanallardan keser ve arkasını öne çeker. Aralık dışını sil yalnızca aralığı bırakır. Kilitli kanallar değişmez, Ctrl + Z geri alır.",
            "Dışa aktarma penceresinde \"Yalnızca seçili aralığı dışa aktar\" ile projenin sadece bu kısmını video yapabilirsiniz. Esc aralığı kaldırır.",
        }},
        {"Kesme ve düzenleme", {
            "Klibi seçin, işaretçiyi kesilecek yere getirin ve S tuşuna basın.",
            "Delete klibi siler. Shift + Delete silip arkadaki klipleri öne çeker.",
            "Ctrl + D çoğaltır. Ctrl + C ile kopyalayıp Ctrl + V ile işaretçiye yapıştırın.",
            "Ctrl + Z geri alır, Ctrl + Y yineler. M tuşu işaretçinin olduğu yere işaret koyar.",
        }},
        {"Görüntü, renk ve geçiş", {
            "Özellikler panelinden opaklık, ölçek ve konumu ayarlayın. Seçili klibi önizlemede sürükleyerek de taşıyabilirsiniz.",
            "Parlaklık, karşıtlık ve doygunluğu ayarlayın ya da Canlı, Sinematik, Siyah beyaz hazır görünümlerini seçin.",
            "Yumuşak giriş ve çıkış klibi yavaşça açıp kapatır. İki klibi farklı kanallarda üst üste bindirirseniz biri diğerine karışarak geçer.",
            "Hız 0,25 ile 4 kat arasında ayarlanır; sesin hızı da birlikte değişir.",
        }},
        {"Yazı ve ses", {
            "Yazı ekle ile başlık ekleyin. Yazı tipi, boyut, renk, çizgi ve arka planı Özellikler panelinden değiştirin.",
            "Ses seviyesi %200'e kadar çıkar. Ses girişi ve çıkışı müziği yumuşak açıp kapatır.",
            "\"Sesi ayrı kanala ayır\" videonun sesini bağımsız bir klip yapar; ayrı kesip taşıyabilirsiniz.",
            "Ses noktaları: klibi seçin, üstündeki sarı çizgiye çift tıklayın. Noktayı yukarı aşağı sürükleyerek sesi o anda yükseltin ya da kısın; konuşma sırasında müziği kısmak için idealdir. Sağ tık noktayı siler.",
            "Gürültü azaltma fan, uğultu ve hışırtıyı temizler. Ses dengeleme kısık yerleri yükseltir, bağıran yerleri yumuşatır. İkisi de önizlemede duyulur.",
            "Dışa aktarırken \"Ses yüksekliğini YouTube standardına getir\" videonun genel sesini -14 LUFS seviyesine ayarlar.",
            "Zaman çizelgesindeki klipte sesin dalga şekli görünür, kesim yerini bulmayı kolaylaştırır.",
        }},
        {"Geçişler, açıklamalar ve efektler", {
            "Sol üstteki Kitaplık'ta Geçişler, Açıklamalar ve Efektler sekmeleri vardır.",
            "Geçişi klibin başına ya da sonuna sürükleyin. Art arda duran iki klibin arasındaki geçiş, hangisine konmuş olursa olsun doğrudan birinden diğerine geçer; arkada siyah görünmez. Aradaki küçük boşluğu da geçiş kapatır.",
            "Kanal başındaki Mıknatıs açıkken klipler arasında boşluk kalmaz: sildiğinizde arkadakiler öne gelir, taşıdığınız klip bıraktığınız yere girer.",
            "Üstteki Yapıştır kutusu kliplerin kenarlara ve işaretçiye yapışmasını açar ve kapatır; Alt tuşu basılıyken geçici olarak kapanır.",
            "Açıklamalar: alt başlık, konuşma balonu, dikdörtgen, elips, vurgu çerçevesi, fosforlu vurgu, ok, çizgi ve numara rozeti. Tıklayınca işaretçinin olduğu yere eklenir.",
            "Bulanık ve pikselli alan, altındaki kanallarda görünen kullanıcı adı, sohbet ya da şifre gibi bilgileri gizler. Klibi istediğiniz kanala ve süreye yerleştirin, önizlemede sürükleyerek konumlayın.",
            "Efektler seçili kliplere uygulanır: yavaşça yakınlaşma, bulanıklaştırma, kenar karartma, renk görünümleri, sepya, negatif, yeşil perde, aynalama.",
            "Döndürme, aynalama ve kırpma Özellikler panelindedir.",
            "Uygulanan geçiş ve efektler Özellikler'in üstündeki Uygulananlar listesinde görünür; yanındaki × ile tek tek, Hepsini kaldır ile toptan silinir. Sağ tık menüsünde ve Kitaplık sekmelerinde de kaldırma düğmeleri vardır.",
        }},
        {"Animasyon noktaları", {
            "Klibi seçin, işaretçiyi klibin üzerine getirip Özellikler'de \"İşaretçiye anahtar kare ekle\"ye basın.",
            "İşaretçiyi başka bir ana götürüp ölçeği, konumu, dönüşü ya da opaklığı değiştirin; o ana yeni anahtar kare yazılır ve aradaki hareket kendiliğinden oluşur.",
            "Animasyonlu klipte önizlemede sürükleyerek taşımak da işaretçideki anahtar kareyi değiştirir.",
            "Anahtar kareler zaman çizelgesinde klibin altında elmas olarak görünür: tıklayınca o ana gider, sürükleyince zamanı değişir, sağ tık siler. Özellikler'deki listeden Yumuşak geçişi açıp kapatabilirsiniz.",
        }},
        {"Yakınlaştır ve kaydır", {
            "Kitaplık > Yakınlaştır sekmesini açın. İşaretçiyi yakınlaşmanın başlayacağı ana getirin, \"Alan seç ve yakınlaş\"a basıp önizlemede bölgeyi çizin.",
            "Görüntü, seçtiğiniz süre içinde o bölgeye yumuşakça yakınlaşır. Daha sonraki bir anda \"Uzaklaş\" ile tam görünüme döner.",
            "Seçili klip yoksa işaretçinin altındaki bütün görüntüler birlikte yakınlaşır. Yakınlaşmalar animasyon noktası olduğu için sonradan düzenlenebilir.",
            "Alan seçmekten vazgeçmek için aynı yerdeki \"Vazgeç\" düğmesine basın.",
        }},
        {"Giriş / çıkış hareketi ve daktilo", {
            "Efektler sekmesindeki hareketler seçili klibin hem girişine hem çıkışına uygulanır: aşağıdan yükselerek, yukarıdan inerek, yandan kayarak, büyüyerek, soldan açılarak.",
            "Daktilo yalnızca yazı kliplerinde çalışır: harfler tek tek belirir, çıkışta sondan silinir.",
            "Özellikler > Giriş / çıkış hareketi bölümünden giriş ve çıkış için ayrı hareket ve süre seçebilirsiniz.",
        }},
        {"Gölge, köşe, kenarlık ve maske", {
            "Gölge her görüntüye, yazıya ve şekle eklenebilir; rengini, opaklığını, yumuşaklığını ve kaymasını Özellikler'den ayarlayın.",
            "Yuvarlak köşe, kenarlık ve maske video, resim ve düz renk kliplerinde çalışır. Daire maske kamera görüntüsü için uygundur.",
            "Maskenin boyutu ve yeri klibin yüzdesi olarak ayarlanır; Yumuşak kenar maskenin sınırını yumuşatır.",
        }},
        {"Spot ışığı", {
            "Açıklamalar > Spot ışığı, seçtiğiniz bölgenin dışını karartır. Altındaki kanallara etki eder; bu yüzden en üstteki kanalda olmalıdır.",
            "Biçimini, karartma oranını, yumuşak kenarını ve boyutunu Özellikler'den ayarlayın; önizlemede sürükleyerek taşıyın.",
        }},
        {"İmleç vurgusu, tıklama halkası, tuş göstergesi", {
            "Kayıt Stüdyosu'nda \"İmleç ve tıklamaları kaydet\" açıkken alınan kayıtlarda çalışır; kaydın yanındaki \".input.json\" dosyası kendiliğinden okunur.",
            "Video klibi seçip Efektler sekmesinden ya da Özellikler > İmleç ve klavye bölümünden açın. Vurgunun rengini, boyutunu ve opaklığını ayarlayabilirsiniz.",
            "Tuş göstergesi, kayıtta \"Kısayol tuşlarını kaydet\" açıksa basılan kısayolları ekranın üstünde gösterir.",
        }},
        {"Ses efektleri", {
            "Sesi olan klibi seçip Efektler sekmesinden ya da Özellikler > Ses bölümünden seçin: kalın ses, ince ses, robot, telefon, telsiz, yankı, salon.",
        }},
        {"Altyazı", {
            "Kitaplık > Altyazı sekmesinde \"İşaretçiye altyazı ekle\" ile 3 saniyelik altyazı eklenir; yazısını, başlangıç ve bitişini listeden değiştirin.",
            "SRT dosyası içe aktarılabilir ve dışa aktarılabilir. Görünüm bölümünden yazı boyutunu, rengini, arka planını ve yerini ayarlayın.",
            "\"Altyazıyı videoya yaz\" açıksa altyazı dışa aktarılan videoya işlenir. Dışa aktarırken ayrıca .srt dosyası da kaydedebilirsiniz.",
        }},
        {"Donmuş kare ve ters oynatma", {
            "Bir video klibi seçip işaretçiyi istediğiniz kareye getirin; Efektler > \"Donmuş kare ekle\" o kareyi 3 saniyelik resim olarak araya koyar, arkadaki klipler kayar.",
            "Ters oynatma video ve ses kliplerini sondan başa oynatır. Özellikler > Hız bölümündeki kutudan ya da sağ tık menüsünden açılır.",
            "Önizlemede ters klip iki saniyelik parçalar halinde okunur, arada kısa takılma olabilir; dışa aktarılan videoda takılma olmaz.",
        }},
        {"Ayarları başka kliplere aktarma", {
            "Birden çok klip seçiliyken Özellikler'de yaptığınız her değişiklik seçili kliplerin hepsine uygulanır.",
            "\"Ayarları kopyala\" (Ctrl + Shift + C) klibin görünüm, efekt, geçiş ve ses ayarlarını alır; başka klipleri seçip \"Seçilenlere yapıştır\" (Ctrl + Shift + V) ile aktarın.",
            "Her kaydırıcının yanındaki kutuya değeri elle yazabilirsiniz.",
        }},
        {"Mikrofondan ses kaydı", {
            "Medya sekmesindeki \"Mikrofondan ses kaydet\" düğmesi ya da Ctrl + R kaydı başlatır, tekrar basınca biter.",
            "Kayıt işaretçinin olduğu yerden başlayarak ses kanalına eklenir. \"Kaydederken videoyu oynat\" açıksa videoyu izlerken anlatım yapabilirsiniz; bunun için kulaklık kullanın.",
        }},
        {"Çok parçalı sesli kayıtlar", {
            "Kayıt Stüdyosu'nda \"Her ses kaynağını ayrı parçaya kaydet\" ile alınan kayıtları ekleyince her ses parçası ayrı ses kanalına konur: bilgisayar sesi, mikrofon ve eklediğiniz diğer kaynaklar.",
            "Videonun kendi sesi (karışım) bu durumda kullanılmaz; sesleri kanallardan ayrı ayrı ayarlar, keser ya da susturursunuz.",
        }},
        {"Dışa aktarma", {
            "İlk eklediğiniz video projenin boyutunu ve kare hızını belirler; böylece video yeniden ölçeklenmez ve netliği korunur.",
            "Kalite yüzdesi yükseldikçe görüntü kaynağa yaklaşır. %90 ve üstü gözle fark edilmeyecek kadar yakındır.",
            "Dosya > Dışa aktar ile hazır ayarlardan birini seçin ya da çözünürlük, kodlayıcı ve kaliteyi kendiniz belirleyin.",
            "Kalite değeri düştükçe görüntü iyileşir, dosya büyür. Donanım kodlayıcılar daha hızlıdır, x264 aynı boyutta genelde daha kalitelidir.",
            "Proje > Proje ayarları ile tuvali değiştirin; dikey kısa video ve kare biçimler de vardır.",
            "Önizleme akıcılık için düşük çözünürlüktedir, dışa aktarılan video tam kalitededir.",
        }},
    };
    return s;
}

const std::vector<Section>& game_sections() {
    static const std::vector<Section> s = {
        {"Hızlı başlangıç", {
            "Soldan Crosshair ya da FPS Ölçer sayfasını seçin ve \"Ekranda göster\"e basın.",
            "Pencereyi çarpıyla kapatsanız bile göstergeler ekranda kalır, oyununuza geçebilirsiniz.",
            "Göstergeler tıklamaları engellemez, fare içlerinden geçer.",
        }},
        {"Arka planda çalışma ve kapatma", {
            "Çarpı programı kapatmaz: pencere gizlenir, program saatin yanındaki simgelerde çalışmaya devam eder.",
            "Simgeyi göremiyorsanız görev çubuğundaki yukarı oka tıklayın.",
            "Simgeye sol tıklayınca pencere açılır. Sağ tıklayınca Crosshair ve FPS Ölçer'i açıp kapatabileceğiniz menü gelir.",
            "Programı tamamen kapatmak için sol alttaki Çıkış düğmesini ya da simge menüsündeki Çıkış'ı kullanın. Göstergeler de kapanır.",
        }},
        {"Bilgisayar açılınca otomatik başlatma", {
            "Crosshair ve FPS Ölçer sayfalarının üstündeki \"Bilgisayar açılınca otomatik göster\" kutusunu işaretleyin.",
            "İlk işaretlemede Windows yönetici onayı ister; bir kez \"Evet\" demeniz yeterlidir.",
            "Bilgisayar açıldığında pencere gelmez; program simge olarak başlar ve yalnızca işaretli gösterge açılır.",
            "Program dosyasını başka klasöre taşırsanız kutuyu kaldırıp yeniden işaretleyin.",
        }},
        {"Crosshair ayarlama", {
            "Sağdaki kaydırıcılarla çizgi uzunluğunu, kalınlığını ve merkez boşluğunu ayarlayın. Değişiklik anında uygulanır.",
            "\"Her çizgiyi ayrı ayarla\" kutusunu işaretleyince Üst, Alt, Sol ve Sağ sekmeleri açılır. Her çizginin uzunluğu, kalınlığı ve boşluğu ayrıdır. Uzunluğu 0 yapılan çizgi görünmez.",
            "İç ve dış çizgilerin döndürme bölümleri ayrıdır ve birbirini etkilemez. Kare nokta iç çizgilerle birlikte döner.",
            "\"Tümünü döndür\" o çizgi takımını birlikte çevirir; örneğin 45 derece X şekli verir.",
            "Üst, Alt, Sol ve Sağ satırlarında her çizginin açısı ayrıdır. \"Kendi etrafında\" çizgiyi olduğu yerde çevirir, \"Merkez etrafında\" çizgiyi crosshair'in çevresinde kaydırır. Dördüne aynı kendi açısını verirseniz pervane şekli çıkar.",
            "\"Açıları sıfırla\" sadece bulunduğu bölümün döndürmelerini 0'a getirir, uzunluk ve kalınlıklara dokunmaz.",
            "Nokta, dış çizgiler ve halkayı ayrı ayrı açıp kapatabilirsiniz.",
            "Renk kutusuna tıklayarak renk seçin. Parlak bir oyunda görünmüyorsa Kenarlık seçeneğini açın.",
        }},
        {"Crosshair profilleri", {
            "Beğendiğiniz görünümü kaydetmek için Profil adı alanına bir isim yazıp Kaydet'e basın.",
            "Listedeki bir profile tıklamak onu hemen uygular. Kullanılan profil listede işaretli görünür.",
            "Kaydetmediğiniz değişiklikler profil seçince kaybolur; önce kaydedin.",
            "\"Kodu kopyala\" crosshair'i paylaşım koduna çevirir. Arkadaşınız kodu kopyalayıp \"Koddan al\"a basınca aynısını kullanır.",
            "\"Koddan al\" CS2 (CSGO-... ve CS...) ve Valorant (0;P;...) crosshair kodlarını da kabul eder. Oyunlar çizimi farklı yaptığından sonuç yaklaşıktır; gerekirse kaydırıcılarla ince ayar yapın.",
        }},
        {"Resimden crosshair, zemine göre renk, konum", {
            "\"Resim seç (PNG)\" ile saydam arka planlı kendi crosshair resminizi kullanın. Resmin ortası ekranın ortasına gelir; boyutu yüzde olarak ayarlanır.",
            "Seçilen resim programın Crosshairs klasörüne kopyalanır; asıl dosyayı silseniz de crosshair bozulmaz.",
            "\"Zemine göre renk\" açıkken merkezin çevresindeki zemin ana renge benzerse crosshair ikinci renge geçer. Resimli crosshair'da çalışmaz.",
            "Konum bölümünde \"Oyun penceresinin ortası\" seçiliyse crosshair pencereli oyunlarda ve ikinci ekrandaki oyunlarda da oyunun tam ortasına gelir. Yatay ve dikey kaydırma ile birkaç piksel oynatabilirsiniz.",
        }},
        {"Performans testi", {
            "Oyundayken F6'ya basın; test başlar. Tekrar F6'ya basınca biter ya da ayarlanan sürede kendiliğinden biter.",
            "Sonuçlar Performans testi sayfasında listelenir: ortalama FPS, en düşük %1 ve %0,1, sıcaklıklar.",
            "İki testi işaretleyip karşılaştırabilir, seçtiklerinizi CSV dosyası olarak dışa aktarabilirsiniz.",
        }},
        {"Oyunlar", {
            "Listeye eklediğiniz oyun öndeyken o oyuna özel crosshair profili, göstergeler ve ekran rengi uygulanır; oyundan çıkınca eski haline döner.",
            "Oyun modu oyun çalışırken yüksek performans güç planını açar ve seçtiğiniz programları kapatır; oyun bitince geri açar.",
            "Oyun süresi sekmesi hangi oyunu ne kadar oynadığınızı gösterir. Mola sekmesi uzun oyunlarda hatırlatma yapar.",
        }},
        {"Nişan", {
            "Hassasiyet çevirici: şu anki oyunu, hassasiyetinizi ve DPI'nızı girin, hedef oyunu seçin. Aynı hissi veren hassasiyet ve tam tur için gereken mesafe (cm/360) yazılır.",
            "DPI değiştirince aynı hissi korumak için gereken yeni hassasiyet de aynı sayfadadır.",
            "DPI ölçümü: alanda sol tuşu basılı tutun, fareyi cetvel boyunca yazan mesafe kadar kaydırıp bırakın. Farenin gerçek DPI değeri bulunur.",
            "Nişan antrenmanı: Tek hedef (tepki süresi), Çoklu hedef ve Takip türleri vardır. Yeşil daireye tıklayınca tur başlar, Esc turu iptal eder. Rekorlar aynı süre ve hedef boyutuyla karşılaştırılır.",
        }},
        {"Yayın araçları", {
            "Tuş göstergesi: basılan W A S D (ya da ok tuşları), Shift, Ctrl, boşluk, fare tuşları ve seçtiğiniz ek tuşlar ekranda yanar. Tuşlar kaydedilmez, yalnızca o anki durum gösterilir.",
            "Çalan şarkı: Spotify, tarayıcı ya da medya oynatıcıda çalan şarkının adı ekranın köşesinde görünür.",
            "Kayıt Stüdyosu: kayıt ya da yayın sürerken köşede REC / CANLI yazar. İsterseniz oyun başlayınca Kayıt Stüdyosu'nun tekrar arabelleği kendiliğinden açılır.",
            "Kısayollar sayfasından \"Tekrarı kaydet\" için bir tuş atayın; oyundan çıkmadan son anları kaydedersiniz.",
        }},
        {"FPS Ölçer", {
            "Bilgiler sekmesinden göstermek istediğiniz değerleri seçin: FPS, ekran kartı, işlemci, RAM, disk, pil, saat.",
            "Görünüm sekmesinden yazı tipini, boyutu ve renkleri ayarlayın.",
            "Uyarılar sekmesi FPS düşünce ya da sıcaklık yükselince rengi değiştirir.",
            "Konum / Arka plan sekmesinden ekranın hangi köşesinde duracağını seçin.",
            "Gösterilen FPS, ekrana gelen kare sayısıdır; oyunun kendi FPS değerinden farklı olabilir ve ekran yenileme hızıyla sınırlı kalabilir.",
        }},
        {"Sıcaklık okunmuyorsa", {
            "İşlemci ve disk sıcaklığı için program yönetici olarak çalışmalıdır. FPS Ölçer sayfasındaki \"Yönetici olarak aç\" düğmesini kullanın.",
            "Otomatik başlatma açıksa program bilgisayar açılışında zaten bu yetkiyle başlar.",
            "İşlemci sıcaklığı için ücretsiz PawnIO sürücüsü de gerekir. Kurulu değilse sayfada indirme düğmesi çıkar.",
            "Yanında \"okunamıyor\" yazan değer bilgisayarınızda desteklenmiyor olabilir.",
        }},
        {"Kayıt ve yayında gizleme", {
            "Göstergeler normalde ekran kaydına ve yayına girer.",
            "\"Kayıt ve yayında gizle\" kutusunu işaretlerseniz gösterge sizin ekranınızda görünür ama kayıtta görünmez.",
        }},
        {"Kısayollar", {
            "F9: Crosshair göster / gizle. F10: Crosshair'i kapat.",
            "F7: FPS Ölçer göster / gizle. F8: FPS Ölçer'i kapat.",
            "F6: Performans testini başlat / durdur. Tuş göstergesi ve Kayıt Stüdyosu tekrar kaydı için tuşu siz atarsınız.",
            "Bunlar oyun içindeyken de çalışır. Oyununuzda aynı tuşlar kullanılıyorsa Kısayollar sayfasından değiştirin. \"Kaldır\" kısayolu siler.",
        }},
        {"Tema ve görünüm", {
            "Ayarlar sayfasından Klasik ya da Bootstrap temasını, altından Koyu ya da Açık modu seçin.",
            "Sol alttaki düğme koyu ve açık mod arasında hızlıca geçer.",
            "Seçiminiz kaydedilir, program yeniden açılınca aynı görünümle başlar.",
        }},
        {"Oyunlarda dikkat", {
            "Crosshair ve FPS Ölçer ekrana ayrı bir pencere olarak çizilir, oyun dosyalarına dokunmaz.",
            "Yine de bazı rekabetçi oyunların kuralları ekran crosshair'ini yasaklayabilir. Oynadığınız oyunun kurallarını kontrol edin.",
            "Tam ekran özel (exclusive) modda göstergeler görünmeyebilir; oyunu kenarlıksız pencere moduna alın.",
            "Oyun çözünürlüğü değiştirince crosshair ve FPS göstergesi yeni ekrana göre kendiliğinden yerleşir.",
        }},
    };
    return s;
}

void help_tabs(const char* app, void (*guide_fn)(), const std::function<void()>& developers, int& request) {
    if (!ImGui::BeginTabBar("yardim")) return;
    auto flags = [&](int i) { return request == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    if (ImGui::BeginTabItem("Kullanım Kılavuzu", nullptr, flags(0))) { guide_fn(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("İletişim", nullptr, flags(1))) { contact_page(app); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Geliştiriciler", nullptr, flags(2))) { developers(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Lisans Sözleşmesi", nullptr, flags(3))) { license_page(app); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Gizlilik", nullptr, flags(4))) { privacy_page(app); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Telif Hakları", nullptr, flags(5))) { copyright_page(app); ImGui::EndTabItem(); }
    request = -1;
    ImGui::EndTabBar();
}

void legal_tabs(float height) {
    ImGui::BeginChild("yazilar", ImVec2(0, height));
    if (ImGui::BeginTabBar("kurulum_yazilari")) {
        if (ImGui::BeginTabItem("Lisans")) { license_page(kRecordApp, false); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Gizlilik: Kayıt Stüdyosu")) { privacy_page(kRecordApp); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Gizlilik: Oyun Araçları")) { privacy_page("Oyun Araçları"); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Gizlilik: Video Düzenleyici")) { privacy_page(kEditorApp); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Telif Hakları")) { copyright_page(kRecordApp); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

void thanks_section() {
    ImGui::SeparatorText("Teşekkürler");
    ImGui::TextColored(theme_accent(), "Claude (Anthropic) · claude.ai");
    ImGui::TextWrapped("Bu programlar yazılırken yapay zekâ asistanı Claude'dan destek alındı: kodlama, hata ayıklama, çeviriler ve kullanım kılavuzu. Katkıları için teşekkür ederiz.");
    ImGui::Spacing();
}

void contact_page(const char* app) {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("İletişim");
    ImGui::PopFont();
    ImGui::TextDisabled("Soru, öneri ya da hata bildirimi için bize ulaşın.");
    ImGui::Spacing(); ImGui::Spacing();

    static std::string failed;
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32_WHITE);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(37, 150, 90, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 175, 105, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 125, 75, 255));
    if (ImGui::Button(ICON_CHAT "   WhatsApp'tan yazın", ImVec2(300, 48)))
        failed = open_link(kWhatsApp.get()) ? "" : "WhatsApp bağlantısı açılamadı";
    ImGui::PopStyleColor(4);
    ImGui::Spacing();
    if (ImGui::Button(ICON_MAIL "   E-posta gönderin", ImVec2(300, 48))) {
        const std::string address = kMail.get();
        std::string subject;
        for (const char* c = app; *c; c++) {     // Türkçe karakterler bağlantıda bozulmasın
            char part[8];
            sprintf_s(part, "%%%02X", unsigned(static_cast<unsigned char>(*c)));
            subject += part;
        }
        // Bilgisayarda e-posta programı yoksa Gmail tarayıcıda açılır
        failed.clear();
        if (!open_link("mailto:" + address + "?subject=" + subject) &&
            !open_link("https://mail.google.com/mail/?view=cm&to=" + address + "&su=" + subject))
            failed = "E-posta açılamadı";
    }
    if (!failed.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(theme_error(), "%s", failed.c_str());
    }
}

// --- Kullanım kılavuzu: program içinde açılan pencere ---
// Solda program seçimi, arama ve bölümler; sağda anlatım ve özelliğin pencerede nerede olduğunu gösteren şema.

namespace {

const char* kManualApps[3] = {"Kayıt Stüdyosu", "Video Düzenleyici", "Oyun Araçları"};

const std::vector<Section>& sections_of(int app) {
    return app == 1 ? editor_sections() : app == 2 ? game_sections() : record_sections();
}

// Şemadaki alanlar: x, y, w, h (pencerenin oranı) ve adı
struct Area { const char* key; float x, y, w, h; const char* name; };
const std::vector<Area>& areas_of(int app) {
    static const std::vector<Area> record = {
        {"menu", 0, 0, 1, .07f, "Menü"}, {"scenes", 0, .08f, .22f, .42f, "Sahneler"}, {"sources", 0, .51f, .22f, .42f, "Kaynaklar"},
        {"preview", .23f, .08f, .77f, .54f, "Önizleme"}, {"selbar", .23f, .63f, .77f, .04f, "Seçili kaynak"},
        {"mixer", .23f, .68f, .37f, .25f, "Ses Karıştırıcı"}, {"transition", .61f, .68f, .16f, .25f, "Sahne Geçişi"},
        {"controls", .78f, .68f, .22f, .25f, "Kontroller"}, {"status", 0, .94f, 1, .06f, "Durum çubuğu"}};
    static const std::vector<Area> editor = {
        {"menu", 0, 0, 1, .07f, "Menü"}, {"library", 0, .08f, .26f, .54f, "Kitaplık"}, {"preview", .27f, .08f, .46f, .54f, "Önizleme"},
        {"inspector", .74f, .08f, .26f, .54f, "Özellikler"}, {"timeline", 0, .63f, 1, .31f, "Zaman çizelgesi"}, {"status", 0, .95f, 1, .05f, "Durum çubuğu"}};
    static const std::vector<Area> game = {{"nav", 0, 0, .2f, 1, "Sol menü"}, {"page", .21f, 0, .79f, 1, "Sayfa"}};
    return app == 1 ? editor : app == 2 ? game : record;
}

// Bölüm başlığından şemadaki yer; "settings" ayarlar sayfasıdır
const char* where_of(int app, const std::string& title) {
    static const std::map<std::string, const char*> record = {
        {"Hızlı başlangıç", "controls"}, {"Sahneler", "scenes"}, {"Kaynaklar", "sources"}, {"Oyun yakalama", "sources"},
        {"Pencere ve ekran bölgesi", "sources"}, {"Kaynak filtreleri", "selbar"}, {"FPS göstergesi", "sources"},
        {"Önizlemede düzenleme", "preview"}, {"Ses", "mixer"}, {"Mikrofon filtreleri, dinleme ve bas-konuş", "mixer"},
        {"Ses girişleri ve uygulama sesi", "mixer"}, {"Çoklu ses parçası", "settings"}, {"Tarayıcı kaynağı", "sources"}, {"Sanal kamera", "controls"},
        {"Aynı anda birden fazla platforma yayın", "settings"}, {"Yayın gecikmesi", "settings"}, {"Gruplar ve sahne içinde sahne", "sources"},
        {"Oynatma listesi ve slayt gösterisi", "sources"}, {"Ses: yalnızca kayıt ya da yalnızca yayın", "settings"}, {"Ekran görüntüsü", "selbar"},
        {"Tekrar arabelleği", "controls"}, {"Stüdyo modu", "transition"}, {"Kaydı bölme", "settings"}, {"İstatistikler", "status"},
        {"Kayıt ayarları", "settings"}, {"Video Düzenleyici için imleç ve kısayol verisi", "settings"}, {"Profiller", "settings"},
        {"Kodlayıcılar", "settings"}, {"Canlı yayın", "controls"}, {"Kısayollar ve ekran", "settings"}, {"Sorun giderme", "status"}};
    static const std::map<std::string, const char*> editor = {
        {"Hızlı başlangıç", "library"}, {"Zaman çizelgesi", "timeline"}, {"Aralık seçimi", "timeline"}, {"Kesme ve düzenleme", "timeline"},
        {"Görüntü, renk ve geçiş", "inspector"}, {"Yazı ve ses", "inspector"}, {"Geçişler, açıklamalar ve efektler", "library"},
        {"Animasyon noktaları", "timeline"}, {"Yakınlaştır ve kaydır", "library"}, {"Giriş / çıkış hareketi ve daktilo", "inspector"},
        {"Gölge, köşe, kenarlık ve maske", "inspector"}, {"Spot ışığı", "library"}, {"İmleç vurgusu, tıklama halkası, tuş göstergesi", "inspector"},
        {"Ses efektleri", "inspector"}, {"Altyazı", "timeline"}, {"Donmuş kare ve ters oynatma", "inspector"},
        {"Ayarları başka kliplere aktarma", "inspector"}, {"Mikrofondan ses kaydı", "library"}, {"Çok parçalı sesli kayıtlar", "timeline"},
        {"Dışa aktarma", "menu"}};
    static const std::map<std::string, const char*> game = {
        {"Arka planda çalışma ve kapatma", "nav"}, {"Bilgisayar açılınca otomatik başlatma", "page"}, {"Kısayollar", "nav"}};
    const auto& table = app == 1 ? editor : app == 2 ? game : record;
    auto it = table.find(title);
    if (it != table.end()) return it->second;
    return app == 2 ? "page" : nullptr;
}

void draw_where(int app, const char* where, float width) {
    const float height = width * 0.62f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* d = ImGui::GetWindowDrawList();
    const ImU32 frame = ImGui::GetColorU32(ImGuiCol_Border), area = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImVec4 a = theme_accent();
    const float pulse = 0.55f + 0.35f * sinf(float(ImGui::GetTime()) * 3.f);
    const ImU32 lit = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x, a.y, a.z, pulse)), lit_line = ImGui::ColorConvertFloat4ToU32(a);
    d->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(ImGuiCol_WindowBg), 6);
    d->AddRect(p, ImVec2(p.x + width, p.y + height), frame, 6.f, ImDrawFlags_None, 1.5f);
    const bool settings = where && !strcmp(where, "settings");
    const char* label = nullptr;
    for (auto& r : areas_of(app)) {
        const ImVec2 a0(p.x + 4 + r.x * (width - 8), p.y + 4 + r.y * (height - 8)), a1(a0.x + r.w * (width - 8), a0.y + r.h * (height - 8));
        const bool on = where && !strcmp(where, r.key);
        d->AddRectFilled(a0, a1, on ? lit : area, 3);
        if (on) { d->AddRect(a0, a1, lit_line, 3.f, ImDrawFlags_None, 2.f); label = r.name; }
    }
    if (settings) {     // Ayarlar sayfası: pencerenin gövdesini kaplar
        const ImVec2 a0(p.x + 4, p.y + 4 + .08f * (height - 8)), a1(p.x + width - 4, p.y + height - 4);
        d->AddRectFilled(a0, a1, lit, 3);
        d->AddRect(a0, a1, lit_line, 3.f, ImDrawFlags_None, 2.f);
        label = "Ayarlar";
    }
    ImGui::Dummy(ImVec2(width, height));
    if (label) ImGui::TextDisabled("%s %s", T("Nerede:"), T(label));
}

// Büyük / küçük harf ayrımsız arama (Türkçe harfler dahil)
std::wstring lower(const char* text) {
    std::wstring w = widen(text);
    if (!w.empty()) CharLowerBuffW(w.data(), DWORD(w.size()));
    return w;
}

struct Manual { int app = -1, section = 0; char query[128] = ""; bool scroll_top = false; };
Manual g_manual;

bool section_matches(const Section& s, const std::wstring& q) {
    if (q.empty() || lower(T(s.title)).find(q) != std::wstring::npos) return true;
    for (auto line : s.lines)
        if (lower(T(line)).find(q) != std::wstring::npos) return true;
    return false;
}

}  // namespace

void manual_view(int default_app) {
    Manual& m = g_manual;
    if (m.app < 0) m.app = default_app;
    const std::wstring q = lower(m.query);
    // Üst sıra: program seçimi ve arama
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        const bool on = m.app == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (ImGui::Button(kManualApps[i], ImVec2(170, 0)) && !on) { m.app = i; m.section = 0; m.scroll_top = true; }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##arama", "Kılavuzda ara...", m.query, sizeof(m.query))) m.scroll_top = true;
    ImGui::Spacing();

    const auto& sections = sections_of(m.app);
    std::vector<int> shown;
    for (int i = 0; i < int(sections.size()); i++)
        if (section_matches(sections[i], q)) shown.push_back(i);
    if (!shown.empty() && std::find(shown.begin(), shown.end(), m.section) == shown.end()) m.section = shown[0];

    const float list_w = 270;
    ImGui::BeginChild("kilavuz_liste", ImVec2(list_w, 0), ImGuiChildFlags_Borders);
    if (!q.empty()) ImGui::TextDisabled("%s", TF("%d bölüm bulundu", int(shown.size())));
    for (int i : shown) {
        ImGui::PushID(i);
        if (ImGui::Selectable(sections[i].title, m.section == i)) { m.section = i; m.scroll_top = true; }
        ImGui::PopID();
    }
    if (shown.empty()) ImGui::TextWrapped("Aramaya uyan bölüm yok.");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("kilavuz_metin", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (m.scroll_top) { ImGui::SetScrollY(0); m.scroll_top = false; }
    if (!shown.empty()) {
        const Section& s = sections[m.section];
        const float avail = ImGui::GetContentRegionAvail().x;
        const char* where = where_of(m.app, s.title);
        const float map_w = where ? std::min(280.f, avail * 0.38f) : 0;
        ImGui::TextDisabled("%s", T(kManualApps[m.app]));
        ImGui::PushFont(nullptr, 26.f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - map_w - 16);
        ImGui::TextColored(theme_accent(), "%s", T(s.title));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (where) {    // Şema sağ üstte
            const ImVec2 back = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + avail - map_w, ImGui::GetStyle().WindowPadding.y));
            ImGui::BeginGroup();
            draw_where(m.app, where, map_w);
            ImGui::EndGroup();
            ImGui::SetCursorPos(ImVec2(back.x, std::max(back.y, ImGui::GetCursorPosY()) + 4));
        }
        ImGui::Separator();
        ImGui::Spacing();
        int step = 1;
        for (auto line : s.lines) {
            const bool hit = !q.empty() && lower(T(line)).find(q) != std::wstring::npos;
            ImGui::PushFont(nullptr, 17.f);
            ImGui::TextColored(hit ? theme_warn() : theme_accent(), "%d", step++);
            ImGui::SameLine(36);
            ImGui::TextWrapped("%s", T(line));
            ImGui::PopFont();
            ImGui::Spacing();
        }
        // Önceki / sonraki bölüm
        ImGui::Spacing();
        ImGui::Separator();
        const auto at = std::find(shown.begin(), shown.end(), m.section);
        ImGui::BeginDisabled(at == shown.begin());
        if (ImGui::Button("\xEE\x9C\xAB  Önceki", ImVec2(140, 0)) && at != shown.begin()) { m.section = *(at - 1); m.scroll_top = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 140);
        ImGui::BeginDisabled(at + 1 == shown.end());
        if (ImGui::Button("Sonraki  \xEE\x9C\xAA", ImVec2(140, 0)) && at + 1 != shown.end()) { m.section = *(at + 1); m.scroll_top = true; }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

void manual_window(bool* open, int app) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(1000, 660), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin((std::string(T("Kullanım Kılavuzu")) + "###kilavuz_pencere").c_str(), open, ImGuiWindowFlags_NoCollapse)) manual_view(app);
    ImGui::End();
}

static bool g_manual_open = false;
void manual_open() { g_manual_open = true; }
void manual_frame(int app) {
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) g_manual_open = !g_manual_open;
    manual_window(&g_manual_open, app);
}

// Yardım sekmesindeki "Kullanım Kılavuzu": aynı görünüm sayfanın içinde
static void guide_page(int app) {
    ImGui::PushFont(nullptr, 24.f);
    ImGui::TextUnformatted("Kullanım Kılavuzu");
    ImGui::PopFont();
    ImGui::TextDisabled("Programlar arasında geçebilir, kılavuzda arama yapabilirsiniz. F1 kılavuzu ayrı pencerede açar.");
    ImGui::Spacing();
    ImGui::BeginChild("kilavuz_sayfa", ImVec2(0, 0));
    manual_view(app);
    ImGui::EndChild();
}

void guide_record() { guide_page(0); }
void guide_editor() { guide_page(1); }
void guide_game() { guide_page(2); }