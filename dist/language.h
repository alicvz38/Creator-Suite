// Dil sistemi: kaynak metinler Türkçe yazılır, T() seçili dildeki karşılığını verir.
// Çeviriler exe yanındaki dil\en.json, dil\de.json, dil\fr.json dosyalarındadır: {"Türkçe metin": "çeviri"}.
// Karşılığı olmayan metin Türkçe kalır. Seçim üç programda ortaktır (data_dir()\dil.json).
#pragma once
#include "common.h"

// script: yazı tipi gereken alfabe ("" Segoe UI yeterli, "zh", "ja", "ko", "th", "hi"); rtl: sağdan sola yazılır
struct LanguageInfo { const char* code; const char* native; const char* turkish; const char* script = ""; bool rtl = false; };
const std::vector<LanguageInfo>& language_list();       // Türkçe adına göre alfabetik
const LanguageInfo* language_info(const std::string& code);
std::string language_native(const LanguageInfo& l);     // Dilin kendi adı, ekranda doğru görünecek biçimde (sağdan sola dillerde çevrilmiş)
void lang_set_font_hook(void (*hook)());                // Arayüz: dil değişince gereken yazı tipini kare dışında yükler

void lang_init();
void lang_tick();                       // Başka programda dil değişirse birkaç saniye içinde uygulanır
std::string lang_setting();             // "system" ya da dil kodu
std::string lang_code();                // Uygulanan dil
std::string system_language();          // Windows'un dili (desteklenmiyorsa en)
void lang_set(const std::string& setting);
bool splash_enabled();
void splash_set_enabled(bool on);

const char* T(const char* text);
std::string T(const std::string& text);
std::wstring TW(const char* text);      // Pencere başlıkları ve ileti kutuları için
std::string TF(const char* format, ...);        // Çevirip printf gibi doldurur
