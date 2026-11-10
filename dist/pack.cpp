// Derleme aracı: kurulacak dosyaları tek pakette toplayıp sıkıştırır. Klasörlerdeki json'lar pakete girmez,
// pakete sadece adı tek tek verilen dosyalar girer (preset-profiles.json gibi).
// Kullanım: paketle <cikti> <kok klasor> <dosya ya da klasor>...
#define NOMINMAX
#include <windows.h>
#include <compressapi.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void add_file(std::vector<char>& out, const fs::path& file, const std::string& name) {
    std::ifstream f(file, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint32_t name_size = uint32_t(name.size());
    const uint64_t data_size = data.size();
    out.insert(out.end(), reinterpret_cast<const char*>(&name_size), reinterpret_cast<const char*>(&name_size) + 4);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), reinterpret_cast<const char*>(&data_size), reinterpret_cast<const char*>(&data_size) + 8);
    out.insert(out.end(), data.begin(), data.end());
    printf("  %s (%llu)\n", name.c_str(), (unsigned long long)data_size);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    const fs::path root = argv[2];
    std::vector<char> raw;
    int files = 0;
    for (int i = 3; i < argc; i++) {
        const fs::path item = root / argv[i];
        if (fs::is_directory(item)) {
            for (auto& entry : fs::recursive_directory_iterator(item)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext == ".json" || ext == ".tmp") continue;
                add_file(raw, entry.path(), fs::relative(entry.path(), root).u8string().c_str() ? (const char*)fs::relative(entry.path(), root).u8string().c_str() : "");
                files++;
            }
        } else if (fs::is_regular_file(item)) {
            add_file(raw, item, (const char*)fs::path(argv[i]).u8string().c_str());
            files++;
        } else {
            printf("Bulunamadi: %ls\n", argv[i]);
            return 1;
        }
    }
    COMPRESSOR_HANDLE compressor;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) return 1;
    std::vector<char> packed(raw.size() + raw.size() / 8 + 65536);
    SIZE_T size = 0;
    if (!Compress(compressor, raw.data(), raw.size(), packed.data(), packed.size(), &size)) return 1;
    CloseCompressor(compressor);
    std::ofstream o(argv[1], std::ios::binary);
    o.write(packed.data(), size);
    printf("%d dosya, %.1f MB -> %.1f MB\n", files, raw.size() / 1048576.0, size / 1048576.0);
    return 0;
}
