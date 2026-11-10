// Çalan şarkı: Windows'un medya oturumundan (Spotify, tarayıcı, medya oynatıcı vb.) şarkı adı ve sanatçı.
// Ayrı iş parçacığında birkaç saniyede bir okunur; arayüz yalnızca son değeri alır.
#pragma once
#include <memory>
#include <string>

struct NowPlaying {
    bool playing = false;
    std::string title, artist;
};

class MediaWatcher {
public:
    MediaWatcher();
    ~MediaWatcher();
    void start();
    void stop();
    bool running() const { return impl_ != nullptr; }
    NowPlaying get();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
