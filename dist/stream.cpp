#define SECURITY_WIN32
#include "stream.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <schannel.h>
#include <security.h>
#include <mferror.h>

#include <algorithm>
#include <condition_variable>
#include <deque>

namespace {

struct Packet {
    uint8_t type = 0;       // 8 ses, 9 görüntü
    uint32_t ms = 0;
    bool key = false;
    std::vector<uint8_t> data;
    double at = 0;          // Geliş anı (yayın gecikmesi için)
};

// --- Bağlantı: düz TCP ya da Windows'un TLS katmanı (SChannel) ---

class Transport {
public:
    ~Transport() { close(); }

    bool open(const std::string& host, int port, bool tls, std::string& error) {
        addrinfo hints{}, *list = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0) {
            error = "Sunucu bulunamadı: " + host;
            return false;
        }
        SOCKET s = INVALID_SOCKET;
        for (addrinfo* a = list; a; a = a->ai_next) {
            s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            DWORD timeout = 10000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
            int on = 1, buffer = 1 << 20;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&on), sizeof(on));
            setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&buffer), sizeof(buffer));
            if (connect(s, a->ai_addr, int(a->ai_addrlen)) == 0) break;
            closesocket(s);
            s = INVALID_SOCKET;
        }
        freeaddrinfo(list);
        if (s == INVALID_SOCKET) {
            error = "Sunucuya bağlanılamadı: " + host + ":" + std::to_string(port);
            return false;
        }
        socket_ = s;
        tls_ = tls;
        return !tls || tls_handshake(host, error);
    }

    void abort() {      // Başka iş parçacığından: bekleyen okuma ve yazma hemen bitsin
        const SOCKET s = socket_;
        if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    }

    void close() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
        socket_ = INVALID_SOCKET;
        if (context_ok_) DeleteSecurityContext(&context_);
        if (cred_ok_) FreeCredentialsHandle(&cred_);
        context_ok_ = cred_ok_ = tls_ = false;
        encrypted_.clear();
        plain_.clear();
    }

    bool send_all(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        if (!tls_) return raw_send(p, size);
        while (size) {
            const size_t part = std::min<size_t>(size, sizes_.cbMaximumMessage);
            std::vector<char> message(sizes_.cbHeader + part + sizes_.cbTrailer);
            memcpy(message.data() + sizes_.cbHeader, p, part);
            SecBuffer buffers[4] = {{sizes_.cbHeader, SECBUFFER_STREAM_HEADER, message.data()},
                                    {ULONG(part), SECBUFFER_DATA, message.data() + sizes_.cbHeader},
                                    {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, message.data() + sizes_.cbHeader + part},
                                    {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&context_, 0, &desc, 0) != SEC_E_OK) return false;
            if (!raw_send(message.data(), size_t(buffers[0].cbBuffer) + buffers[1].cbBuffer + buffers[2].cbBuffer)) return false;
            p += part;
            size -= part;
        }
        return true;
    }

    bool read_all(void* data, size_t size) {
        char* p = static_cast<char*>(data);
        while (size) {
            if (!tls_) {
                const int got = recv(socket_, p, int(std::min<size_t>(size, 65536)), 0);
                if (got <= 0) return false;
                p += got;
                size -= got;
                continue;
            }
            if (plain_.empty() && !fill_plain()) return false;
            const size_t part = std::min(size, plain_.size());
            memcpy(p, plain_.data(), part);
            plain_.erase(plain_.begin(), plain_.begin() + part);
            p += part;
            size -= part;
        }
        return true;
    }

    bool readable() {
        if (!plain_.empty()) return true;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval zero{};
        return select(0, &set, nullptr, nullptr, &zero) > 0;
    }

private:
    bool raw_send(const char* p, size_t size) {
        while (size) {
            const int sent = send(socket_, p, int(std::min<size_t>(size, 1 << 20)), 0);
            if (sent <= 0) return false;
            p += sent;
            size -= sent;
        }
        return true;
    }

    bool read_more() {
        char chunk[16384];
        const int got = recv(socket_, chunk, sizeof(chunk), 0);
        if (got <= 0) return false;
        encrypted_.insert(encrypted_.end(), chunk, chunk + got);
        return true;
    }

    bool fill_plain() {
        for (;;) {
            if (!encrypted_.empty()) {
                SecBuffer buffers[4] = {{ULONG(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()},
                                        {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
                const SECURITY_STATUS status = DecryptMessage(&context_, &desc, 0, nullptr);
                if (status == SEC_E_OK) {
                    std::vector<char> extra;
                    for (auto& b : buffers) {
                        if (b.BufferType == SECBUFFER_DATA)
                            plain_.insert(plain_.end(), static_cast<char*>(b.pvBuffer), static_cast<char*>(b.pvBuffer) + b.cbBuffer);
                        if (b.BufferType == SECBUFFER_EXTRA)
                            extra.assign(static_cast<char*>(b.pvBuffer), static_cast<char*>(b.pvBuffer) + b.cbBuffer);
                    }
                    encrypted_ = std::move(extra);
                    if (!plain_.empty()) return true;
                    continue;
                }
                if (status != SEC_E_INCOMPLETE_MESSAGE) return false;
            }
            if (!read_more()) return false;
        }
    }

    bool tls_handshake(const std::string& host, std::string& error) {
        error = "Güvenli bağlantı kurulamadı: " + host;
        SCHANNEL_CRED cred{};
        cred.dwVersion = SCHANNEL_CRED_VERSION;
        cred.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &cred, nullptr, nullptr,
                                      &cred_, nullptr) != SEC_E_OK)
            return false;
        cred_ok_ = true;
        const std::wstring name = widen(host);
        const ULONG request = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                              ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        bool first = true;
        for (;;) {
            SecBuffer in[2] = {{ULONG(encrypted_.size()), SECBUFFER_TOKEN, encrypted_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBuffer out[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in}, out_desc{SECBUFFER_VERSION, 1, out};
            ULONG attrs = 0;
            const SECURITY_STATUS status = InitializeSecurityContextW(&cred_, first ? nullptr : &context_, const_cast<wchar_t*>(name.c_str()), request, 0, 0,
                                                                      first ? nullptr : &in_desc, 0, first ? &context_ : nullptr, &out_desc, &attrs, nullptr);
            first = false;
            context_ok_ = true;
            if (out[0].cbBuffer && out[0].pvBuffer) {
                const bool sent = raw_send(static_cast<char*>(out[0].pvBuffer), out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
                if (!sent) return false;
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!read_more()) return false;
                continue;
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) return false;
            std::vector<char> extra;
            if (in[1].BufferType == SECBUFFER_EXTRA)
                extra.assign(encrypted_.end() - in[1].cbBuffer, encrypted_.end());
            encrypted_ = std::move(extra);
            if (status == SEC_E_OK) break;
            if (encrypted_.empty() && !read_more()) return false;
        }
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) return false;
        error.clear();
        return true;
    }

    SOCKET socket_ = INVALID_SOCKET;
    bool tls_ = false, cred_ok_ = false, context_ok_ = false;
    CredHandle cred_{};
    CtxtHandle context_{};
    SecPkgContext_StreamSizes sizes_{};
    std::vector<char> encrypted_, plain_;
};

// --- AMF0: RTMP komutlarının veri biçimi ---

void put_u16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void put_u24(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 16)); put_u16(o, v); }
void put_u32(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 24)); put_u24(o, v); }

void amf_raw_string(std::vector<uint8_t>& o, const std::string& s) {
    put_u16(o, uint32_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}
void amf_string(std::vector<uint8_t>& o, const std::string& s) { o.push_back(2); amf_raw_string(o, s); }
void amf_number(std::vector<uint8_t>& o, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    o.push_back(0);
    for (int i = 7; i >= 0; i--) o.push_back(uint8_t(bits >> (i * 8)));
}
void amf_bool(std::vector<uint8_t>& o, bool v) { o.push_back(1); o.push_back(v ? 1 : 0); }
void amf_null(std::vector<uint8_t>& o) { o.push_back(5); }
void amf_key(std::vector<uint8_t>& o, const std::string& k) { amf_raw_string(o, k); }
void amf_end(std::vector<uint8_t>& o) { put_u16(o, 0); o.push_back(9); }

// Gelen komuttan gereken kadarını okur: ad, işlem numarası, ilk sayı, "code" ve "description"
struct AmfReply {
    std::string name, code, description;
    double transaction = 0, number = 0;
};

class AmfReader {
public:
    AmfReader(const std::vector<uint8_t>& d, AmfReply& r) : d_(d), r_(r) {}
    void read_all() {
        int index = 0;
        while (p_ < d_.size() && value(index++, "")) {}
    }

private:
    bool has(size_t n) const { return p_ + n <= d_.size(); }
    std::string raw_string(size_t size) {
        if (!has(size)) { p_ = d_.size(); return {}; }
        std::string s(reinterpret_cast<const char*>(d_.data()) + p_, size);
        p_ += size;
        return s;
    }
    size_t u16() {
        if (!has(2)) { p_ = d_.size(); return 0; }
        const size_t v = (size_t(d_[p_]) << 8) | d_[p_ + 1];
        p_ += 2;
        return v;
    }
    bool object_body() {
        while (has(3)) {
            const std::string key = raw_string(u16());
            if (key.empty() && has(1) && d_[p_] == 9) { p_++; return true; }
            if (!value(-1, key)) return false;
        }
        return false;
    }
    bool value(int index, const std::string& key) {
        if (!has(1)) return false;
        const uint8_t type = d_[p_++];
        if (type == 0) {
            if (!has(8)) return false;
            uint64_t bits = 0;
            for (int i = 0; i < 8; i++) bits = (bits << 8) | d_[p_++];
            double v;
            memcpy(&v, &bits, 8);
            if (index == 1) r_.transaction = v;
            if (index >= 2 && !have_number_) { r_.number = v; have_number_ = true; }
            return true;
        }
        if (type == 1) { p_++; return true; }
        if (type == 2) {
            const std::string s = raw_string(u16());
            if (index == 0) r_.name = s;
            if (key == "code") r_.code = s;
            if (key == "description") r_.description = s;
            return true;
        }
        if (type == 3) return object_body();
        if (type == 8) { p_ += 4; return object_body(); }
        if (type == 5 || type == 6) return true;
        return false;
    }
    const std::vector<uint8_t>& d_;
    AmfReply& r_;
    size_t p_ = 0;
    bool have_number_ = false;
};

// --- RTMP oturumu ---

struct Url {
    std::string scheme, host, app;
    int port = 1935;
    bool tls = false;
};

bool parse_url(std::string text, Url& u) {
    while (!text.empty() && (text.back() == '/' || text.back() == ' ')) text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    const size_t scheme = text.find("://");
    if (scheme == std::string::npos) return false;
    u.scheme = text.substr(0, scheme);
    std::transform(u.scheme.begin(), u.scheme.end(), u.scheme.begin(), [](char c) { return char(tolower(c)); });
    if (u.scheme != "rtmp" && u.scheme != "rtmps") return false;
    u.tls = u.scheme == "rtmps";
    u.port = u.tls ? 443 : 1935;
    std::string rest = text.substr(scheme + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) { u.app = rest.substr(slash + 1); rest = rest.substr(0, slash); }
    const size_t colon = rest.find(':');
    if (colon != std::string::npos) { u.port = atoi(rest.c_str() + colon + 1); rest = rest.substr(0, colon); }
    u.host = rest;
    return !u.host.empty() && u.port > 0;
}

class RtmpSession {
public:
    Transport net;

    // fatal: yeniden denemenin anlamı yok (anahtar yanlış gibi)
    bool connect_publish(const StreamConfig& c, std::string& error, bool& fatal) {
        Url u;
        fatal = true;
        if (!parse_url(c.url, u)) { error = "Sunucu adresi geçersiz. rtmp:// ya da rtmps:// ile başlamalı."; return false; }
        fatal = false;
        reset();
        if (!net.open(u.host, u.port, u.tls, error)) return false;
        error = "Sunucu el sıkışmayı tamamlamadı";
        if (!handshake()) return false;
        std::vector<uint8_t> size;
        put_u32(size, kChunkOut);
        if (!send_message(2, 1, 0, 0, size)) return false;
        chunk_out_ = kChunkOut;

        const std::string port = (u.port == (u.tls ? 443 : 1935)) ? "" : ":" + std::to_string(u.port);
        const std::string tc_url = u.scheme + "://" + u.host + port + "/" + u.app;
        std::vector<uint8_t> m;
        amf_string(m, "connect"); amf_number(m, 1);
        m.push_back(3);
        amf_key(m, "app"); amf_string(m, u.app);
        amf_key(m, "type"); amf_string(m, "nonprivate");
        amf_key(m, "flashVer"); amf_string(m, "FMLE/3.0 (compatible; KayitStudyosu)");
        amf_key(m, "tcUrl"); amf_string(m, tc_url);
        if (c.codec != "h264") {      // Enhanced RTMP: sunucuya hangi kodekle geldiğimizi söyle
            amf_key(m, "fourCcList");
            m.push_back(10);
            put_u32(m, 1);
            amf_string(m, c.codec == "hevc" ? "hvc1" : "av01");
        }
        amf_end(m);
        if (!send_message(3, 20, 0, 0, m)) return false;
        AmfReply reply;
        if (!wait_reply(1, reply)) { error = "Sunucu bağlantı isteğine yanıt vermedi"; return false; }
        if (reply.name != "_result") {
            error = "Sunucu bağlantıyı reddetti" + (reply.description.empty() ? "" : ": " + reply.description);
            fatal = true;
            return false;
        }
        auto command = [&](const char* name, double id, const std::string* arg) {
            std::vector<uint8_t> o;
            amf_string(o, name); amf_number(o, id); amf_null(o);
            if (arg) amf_string(o, *arg);
            return send_message(3, 20, 0, 0, o);
        };
        command("releaseStream", 2, &c.key);
        command("FCPublish", 3, &c.key);
        if (!command("createStream", 4, nullptr) || !wait_reply(4, reply) || reply.name != "_result") {
            error = "Yayın kanalı açılamadı";
            return false;
        }
        stream_id_ = uint32_t(reply.number);
        m.clear();
        amf_string(m, "publish"); amf_number(m, 5); amf_null(m);
        amf_string(m, c.key); amf_string(m, "live");
        if (!send_message(4, 20, 0, stream_id_, m)) return false;
        const double deadline = now() + 15;
        for (;;) {
            AmfReply status;
            if (now() > deadline || !read_message(&status)) {
                error = "Sunucu yayını kabul etmedi. Yayın anahtarını kontrol edin.";
                fatal = true;
                return false;
            }
            if (status.name != "onStatus") continue;
            if (status.code == "NetStream.Publish.Start") return send_metadata(c);
            error = "Sunucu yayını reddetti (" + status.code + "). Yayın anahtarını kontrol edin.";
            fatal = true;
            return false;
        }
    }

    bool send_media(uint8_t type, uint32_t ms, const std::vector<uint8_t>& data) {
        return send_message(type == 8 ? 5 : 6, type, ms, stream_id_, data);
    }

    bool pump() {   // Sunucudan gelen ping ve benzeri iletiler
        while (net.readable())
            if (!read_message(nullptr)) return false;
        return true;
    }

private:
    static constexpr uint32_t kChunkOut = 4096;
    struct Incoming {
        uint32_t size = 0, stream = 0, ms = 0, delta = 0;
        uint8_t type = 0;
        bool extended = false;
        std::vector<uint8_t> body;
    };

    void reset() {
        chunk_out_ = chunk_in_ = 128;
        incoming_.clear();
        received_ = acked_ = 0;
        window_ = 2500000;
        stream_id_ = 0;
    }

    bool handshake() {
        std::vector<uint8_t> c(1537, 0);
        c[0] = 3;
        for (size_t i = 9; i < c.size(); i++) c[i] = uint8_t(rand());
        if (!net.send_all(c.data(), c.size())) return false;
        std::vector<uint8_t> s(3073);
        if (!net.read_all(s.data(), s.size())) return false;
        return net.send_all(s.data() + 1, 1536);
    }

    bool send_metadata(const StreamConfig& c) {
        std::vector<uint8_t> m;
        amf_string(m, "@setDataFrame");
        amf_string(m, "onMetaData");
        m.push_back(8);
        put_u32(m, 0);
        amf_key(m, "duration"); amf_number(m, 0);
        amf_key(m, "width"); amf_number(m, c.width);
        amf_key(m, "height"); amf_number(m, c.height);
        amf_key(m, "videodatarate"); amf_number(m, c.video_kbps);
        amf_key(m, "framerate"); amf_number(m, c.fps);
        amf_key(m, "videocodecid"); amf_number(m, c.codec == "hevc" ? 1752589105.0 : c.codec == "av1" ? 1635135537.0 : 7.0);
        if (c.audio) {
            amf_key(m, "audiodatarate"); amf_number(m, c.audio_kbps);
            amf_key(m, "audiosamplerate"); amf_number(m, kAudioRate);
            amf_key(m, "audiosamplesize"); amf_number(m, 16);
            amf_key(m, "stereo"); amf_bool(m, true);
            amf_key(m, "audiocodecid"); amf_number(m, 10);
        }
        amf_key(m, "encoder"); amf_string(m, "Recording Studio");
        amf_end(m);
        return send_message(4, 18, 0, stream_id_, m);
    }

    bool send_message(uint8_t csid, uint8_t type, uint32_t ms, uint32_t stream, const std::vector<uint8_t>& body) {
        const bool extended = ms >= 0xFFFFFF;
        std::vector<uint8_t>& o = out_;
        o.clear();
        o.reserve(body.size() + body.size() / chunk_out_ * 5 + 32);
        o.push_back(csid);
        put_u24(o, extended ? 0xFFFFFF : ms);
        put_u24(o, uint32_t(body.size()));
        o.push_back(type);
        for (int i = 0; i < 4; i++) o.push_back(uint8_t(stream >> (i * 8)));
        if (extended) put_u32(o, ms);
        size_t at = 0;
        for (;;) {
            const size_t part = std::min<size_t>(body.size() - at, chunk_out_);
            o.insert(o.end(), body.begin() + at, body.begin() + at + part);
            at += part;
            if (at >= body.size()) break;
            o.push_back(uint8_t(0xC0 | csid));
            if (extended) put_u32(o, ms);
        }
        return net.send_all(o.data(), o.size());
    }

    bool read_u(uint32_t& v, int bytes) {
        uint8_t b[4];
        if (!net.read_all(b, bytes)) return false;
        v = 0;
        for (int i = 0; i < bytes; i++) v = (v << 8) | b[i];
        received_ += bytes;
        return true;
    }

    // Tam bir ileti gelene kadar parçaları okur
    bool read_message(AmfReply* reply) {
        for (;;) {
            uint32_t basic;
            if (!read_u(basic, 1)) return false;
            const int format = basic >> 6;
            uint32_t csid = basic & 63;
            if (csid == 0) { uint32_t x; if (!read_u(x, 1)) return false; csid = 64 + x; }
            else if (csid == 1) { uint32_t x; if (!read_u(x, 2)) return false; csid = 64 + ((x & 255) << 8) + (x >> 8); }
            Incoming& in = incoming_[csid];
            uint32_t stamp = 0;
            if (format <= 2 && !read_u(stamp, 3)) return false;
            if (format <= 1) {
                uint32_t type;
                if (!read_u(in.size, 3) || !read_u(type, 1)) return false;
                in.type = uint8_t(type);
            }
            if (format == 0) {
                uint8_t b[4];
                if (!net.read_all(b, 4)) return false;
                received_ += 4;
                in.stream = b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
            }
            if (format <= 2) in.extended = stamp == 0xFFFFFF;
            if (in.extended) { uint32_t x; if (!read_u(x, 4)) return false; }
            if (in.size > (1u << 24)) return false;
            const size_t part = std::min<size_t>(in.size - in.body.size(), chunk_in_);
            const size_t have = in.body.size();
            in.body.resize(have + part);
            if (part && !net.read_all(in.body.data() + have, part)) return false;
            received_ += part;
            if (in.body.size() < in.size) continue;
            std::vector<uint8_t> body = std::move(in.body);
            in.body.clear();
            if (!handle(in.type, body, reply)) return false;
            return true;
        }
    }

    bool handle(uint8_t type, const std::vector<uint8_t>& b, AmfReply* reply) {
        auto u32 = [&](size_t at) { return (uint32_t(b[at]) << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3]; };
        if (type == 1 && b.size() >= 4) chunk_in_ = std::max<uint32_t>(u32(0) & 0x7fffffff, 1);
        if (type == 5 && b.size() >= 4) window_ = std::max<uint32_t>(u32(0), 4096);
        if (type == 4 && b.size() >= 6 && b[0] == 0 && b[1] == 6) {     // Ping
            std::vector<uint8_t> pong;
            put_u16(pong, 7);
            pong.insert(pong.end(), b.begin() + 2, b.begin() + 6);
            if (!send_message(2, 4, 0, 0, pong)) return false;
        }
        if (type == 20 && reply) AmfReader(b, *reply).read_all();
        if (received_ - acked_ >= window_) {
            acked_ = received_;
            std::vector<uint8_t> ack;
            put_u32(ack, uint32_t(received_));
            if (!send_message(2, 3, 0, 0, ack)) return false;
        }
        return true;
    }

    bool wait_reply(double id, AmfReply& out) {
        const double deadline = now() + 15;
        while (now() < deadline) {
            AmfReply r;
            if (!read_message(&r)) return false;
            if ((r.name == "_result" || r.name == "_error") && r.transaction == id) { out = r; return true; }
        }
        return false;
    }

    uint32_t chunk_out_ = 128, chunk_in_ = 128, window_ = 2500000, stream_id_ = 0;
    uint64_t received_ = 0, acked_ = 0;
    std::map<uint32_t, Incoming> incoming_;
    std::vector<uint8_t> out_;
};

// --- Kodlayıcı çıktısını alan Media Foundation uçları ---
// Sink Writer normalde dosyaya yazar. Burada dosya yerine paketleri bize veren küçük bir "media sink" takılı:
// böylece kayıttakiyle aynı donanım kodlayıcı ve renk dönüşümü kullanılır.

using SampleHandler = std::function<void(int stream, IMFSample* sample)>;
class PacketSink;

class SinkPort : public IMFStreamSink, public IMFMediaTypeHandler {
public:
    SinkPort(IMFMediaSink* owner, DWORD id, IMFMediaType* type, SampleHandler handler)
        : owner_(owner), id_(id), type_(type), handler_(std::move(handler)) {
        MFCreateEventQueue(&queue_);
    }
    void clock_started() {
        queue_->QueueEventParamVar(MEStreamSinkStarted, GUID_NULL, S_OK, nullptr);
        queue_->QueueEventParamVar(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr);
    }
    void clock_stopped() { queue_->QueueEventParamVar(MEStreamSinkStopped, GUID_NULL, S_OK, nullptr); }
    void shutdown() { if (queue_) queue_->Shutdown(); }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEventGenerator) || iid == __uuidof(IMFStreamSink))
            *out = static_cast<IMFStreamSink*>(this);
        else if (iid == __uuidof(IMFMediaTypeHandler)) *out = static_cast<IMFMediaTypeHandler*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return left;
    }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** e) override { return queue_->GetEvent(flags, e); }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* c, IUnknown* s) override { return queue_->BeginGetEvent(c, s); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return queue_->EndGetEvent(r, e); }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT hr, const PROPVARIANT* v) override { return queue_->QueueEventParamVar(t, g, hr, v); }
    STDMETHODIMP GetMediaSink(IMFMediaSink** out) override { *out = owner_; owner_->AddRef(); return S_OK; }
    STDMETHODIMP GetIdentifier(DWORD* id) override { *id = id_; return S_OK; }
    STDMETHODIMP GetMediaTypeHandler(IMFMediaTypeHandler** out) override { *out = this; AddRef(); return S_OK; }
    STDMETHODIMP ProcessSample(IMFSample* sample) override {
        handler_(int(id_), sample);
        return queue_->QueueEventParamVar(MEStreamSinkRequestSample, GUID_NULL, S_OK, nullptr);
    }
    STDMETHODIMP PlaceMarker(MFSTREAMSINK_MARKER_TYPE, const PROPVARIANT*, const PROPVARIANT* context) override {
        return queue_->QueueEventParamVar(MEStreamSinkMarker, GUID_NULL, S_OK, context);
    }
    STDMETHODIMP Flush() override { return S_OK; }
    // Yalnızca kodlanmış tür kabul edilir: Sink Writer ancak böyle araya kodlayıcı koyar
    bool accepts(IMFMediaType* type) const {
        GUID wanted{}, given{};
        type_->GetGUID(MF_MT_SUBTYPE, &wanted);
        return type && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &given)) && wanted == given;
    }
    STDMETHODIMP IsMediaTypeSupported(IMFMediaType* type, IMFMediaType** closest) override {
        if (closest) *closest = nullptr;
        return accepts(type) ? S_OK : MF_E_INVALIDMEDIATYPE;
    }
    STDMETHODIMP GetMediaTypeCount(DWORD* count) override { *count = 1; return S_OK; }
    STDMETHODIMP GetMediaTypeByIndex(DWORD index, IMFMediaType** out) override {
        if (index) return MF_E_NO_MORE_TYPES;
        return type_.CopyTo(out);
    }
    STDMETHODIMP SetCurrentMediaType(IMFMediaType* type) override {
        if (!accepts(type)) return MF_E_INVALIDMEDIATYPE;
        type_ = type;
        return S_OK;
    }
    STDMETHODIMP GetCurrentMediaType(IMFMediaType** out) override { return type_.CopyTo(out); }
    STDMETHODIMP GetMajorType(GUID* major) override { return type_->GetGUID(MF_MT_MAJOR_TYPE, major); }

private:
    virtual ~SinkPort() = default;
    LONG refs_ = 1;
    IMFMediaSink* owner_;
    DWORD id_;
    ComPtr<IMFMediaType> type_;
    SampleHandler handler_;
    ComPtr<IMFMediaEventQueue> queue_;
};

class PacketSink : public IMFMediaSink, public IMFClockStateSink {
public:
    PacketSink(IMFMediaType* video, IMFMediaType* audio, SampleHandler handler) {
        ports_.push_back(new SinkPort(this, 0, video, handler));
        if (audio) ports_.push_back(new SinkPort(this, 1, audio, handler));
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaSink)) *out = static_cast<IMFMediaSink*>(this);
        else if (iid == __uuidof(IMFClockStateSink)) *out = static_cast<IMFClockStateSink*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return left;
    }
    STDMETHODIMP GetCharacteristics(DWORD* flags) override { *flags = MEDIASINK_FIXED_STREAMS | MEDIASINK_RATELESS; return S_OK; }
    STDMETHODIMP AddStreamSink(DWORD, IMFMediaType*, IMFStreamSink**) override { return MF_E_STREAMSINKS_FIXED; }
    STDMETHODIMP RemoveStreamSink(DWORD) override { return MF_E_STREAMSINKS_FIXED; }
    STDMETHODIMP GetStreamSinkCount(DWORD* count) override { *count = DWORD(ports_.size()); return S_OK; }
    STDMETHODIMP GetStreamSinkByIndex(DWORD i, IMFStreamSink** out) override {
        if (i >= ports_.size()) return MF_E_INVALIDINDEX;
        *out = ports_[i];
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetStreamSinkById(DWORD id, IMFStreamSink** out) override {
        if (id >= ports_.size()) return MF_E_INVALIDSTREAMNUMBER;
        *out = ports_[id];
        (*out)->AddRef();
        return S_OK;
    }
    STDMETHODIMP SetPresentationClock(IMFPresentationClock* clock) override {
        if (clock_) clock_->RemoveClockStateSink(this);
        clock_ = clock;
        if (clock_) clock_->AddClockStateSink(this);
        return S_OK;
    }
    STDMETHODIMP GetPresentationClock(IMFPresentationClock** out) override {
        if (!clock_) return MF_E_NO_CLOCK;
        return clock_.CopyTo(out);
    }
    STDMETHODIMP Shutdown() override {
        if (clock_) clock_->RemoveClockStateSink(this);
        clock_.Reset();
        for (auto* p : ports_) p->shutdown();
        return S_OK;
    }
    STDMETHODIMP OnClockStart(MFTIME, LONGLONG) override { for (auto* p : ports_) p->clock_started(); return S_OK; }
    STDMETHODIMP OnClockStop(MFTIME) override { for (auto* p : ports_) p->clock_stopped(); return S_OK; }
    STDMETHODIMP OnClockPause(MFTIME) override { return S_OK; }
    STDMETHODIMP OnClockRestart(MFTIME) override { for (auto* p : ports_) p->clock_started(); return S_OK; }
    STDMETHODIMP OnClockSetRate(MFTIME, float) override { return S_OK; }

private:
    virtual ~PacketSink() { for (auto* p : ports_) p->Release(); }
    LONG refs_ = 1;
    std::vector<SinkPort*> ports_;
    ComPtr<IMFPresentationClock> clock_;
};

// Annex-B biçimindeki H.264'ü NAL birimlerine ayırır
void split_nals(const uint8_t* d, size_t n, std::vector<std::pair<const uint8_t*, size_t>>& out) {
    size_t start = SIZE_MAX;
    for (size_t i = 0; i + 3 <= n;) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && d[end - 1] == 0) end--;
                out.push_back({d + start, end - start});
            }
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) out.push_back({d + start, n - start});
}

}  // namespace

// --- Yayıncı ---

struct Streamer::Impl {
    StreamConfig config;
    std::unique_ptr<FrameOutput> encoder;
    std::thread thread;
    std::atomic<bool> halt{false}, failed{false};
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Packet> queue;
    std::vector<uint8_t> avc_config;
    std::vector<uint8_t> raw_video_header;      // FFmpeg'den gelen hazır başlık
    std::vector<uint8_t> flv;                   // FFmpeg'den gelen, henüz işlenmemiş FLV verisi
    size_t flv_at = 0;
    bool flv_started = false;
    RtmpSession session;
    StreamStats stats;
    double started = 0;
    long long sent_bytes = 0;

    void on_sample(int stream, IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        BYTE* data = nullptr;
        DWORD size = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &size))) return;
        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        Packet p;
        p.ms = uint32_t(time / 10000);
        if (stream == 1) {
            p.type = 8;
            p.data.reserve(size + 2);
            p.data.push_back(0xAF);     // AAC, 48 kHz, 16 bit, stereo
            p.data.push_back(1);
            p.data.insert(p.data.end(), data, data + size);
        } else {
            p.type = 9;
            std::vector<std::pair<const uint8_t*, size_t>> nals;
            split_nals(data, size, nals);
            p.data.reserve(size + 16);
            p.data.resize(5);
            const uint8_t* sps = nullptr;
            const uint8_t* pps = nullptr;
            size_t sps_size = 0, pps_size = 0;
            for (auto& [n, length] : nals) {
                const int type = n[0] & 31;
                if (type == 7) { sps = n; sps_size = length; continue; }
                if (type == 8) { pps = n; pps_size = length; continue; }
                if (type == 9) continue;    // Erişim sınırlayıcı RTMP'de gerekmez
                if (type == 5) p.key = true;
                put_u32(p.data, uint32_t(length));
                p.data.insert(p.data.end(), n, n + length);
            }
            p.data[0] = p.key ? 0x17 : 0x27;
            p.data[1] = 1;
            if (sps && pps && sps_size >= 4) {
                std::vector<uint8_t> c = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1};
                put_u16(c, uint32_t(sps_size));
                c.insert(c.end(), sps, sps + sps_size);
                c.push_back(1);
                put_u16(c, uint32_t(pps_size));
                c.insert(c.end(), pps, pps + pps_size);
                std::lock_guard<std::mutex> g(lock);
                avc_config = std::move(c);
            }
        }
        buffer->Unlock();
        p.at = now();
        std::lock_guard<std::mutex> g(lock);
        queue.push_back(std::move(p));
        wake.notify_one();
    }

    // FFmpeg'in FLV çıktısını etiketlere ayırır; etiket içeriği RTMP'ye aynen gider
    void on_flv(const uint8_t* data, size_t size) {
        flv.insert(flv.end(), data, data + size);
        if (!flv_started) {
            if (flv.size() < 13) return;
            flv_at = 13;        // Dosya başlığı ve ilk boyut alanı
            flv_started = true;
        }
        while (flv.size() - flv_at >= 11) {
            const uint8_t* h = flv.data() + flv_at;
            const size_t body = (size_t(h[1]) << 16) | (size_t(h[2]) << 8) | h[3];
            if (flv.size() - flv_at < 11 + body + 4) break;
            const uint32_t ms = (uint32_t(h[7]) << 24) | (uint32_t(h[4]) << 16) | (uint32_t(h[5]) << 8) | h[6];
            flv_tag(h[0], ms, h + 11, body);
            flv_at += 11 + body + 4;
        }
        if (flv_at > (1u << 20)) { flv.erase(flv.begin(), flv.begin() + flv_at); flv_at = 0; }
    }

    void flv_tag(uint8_t type, uint32_t ms, const uint8_t* d, size_t n) {
        if ((type != 8 && type != 9) || n < 2) return;
        if (type == 8 && d[1] == 0) return;     // AAC başlığı bağlanınca ayrıca gönderilir
        const bool enhanced = type == 9 && (d[0] & 0x80);
        const int packet = enhanced ? d[0] & 15 : d[1];
        if (type == 9 && packet == 0) {         // Kodek başlığı: yeniden bağlanınca tekrar gerekir
            std::lock_guard<std::mutex> g(lock);
            raw_video_header.assign(d, d + n);
            return;
        }
        if (type == 9 && !enhanced && packet == 2) return;      // Akış sonu işareti
        Packet p;
        p.type = type;
        p.ms = ms;
        p.key = type == 9 && ((d[0] >> 4) & 7) == 1;
        p.data.assign(d, d + n);
        p.at = now();
        std::lock_guard<std::mutex> g(lock);
        queue.push_back(std::move(p));
        wake.notify_one();
    }

    void set_state(StreamStats::State state) {
        std::lock_guard<std::mutex> g(lock);
        stats.state = state;
    }

    // Biriken veriyi son anahtar kareye kadar atar: yayın gecikmesi büyümesin
    bool trim_to_latest_key() {
        std::lock_guard<std::mutex> g(lock);
        for (size_t i = queue.size(); i-- > 0;) {
            if (queue[i].type == 9 && queue[i].key) {
                for (size_t k = 0; k < i; k++) stats.dropped += queue[k].type == 9 ? 1 : 0;
                queue.erase(queue.begin(), queue.begin() + i);
                return true;
            }
        }
        for (auto& p : queue) stats.dropped += p.type == 9 ? 1 : 0;
        queue.clear();
        return false;
    }

    void run() {
        int failures = 0;
        bool ever_live = false;
        uint32_t base = 0;
        bool have_base = false;
        while (!halt) {
            set_state(ever_live ? StreamStats::Reconnecting : StreamStats::Connecting);
            std::string error;
            bool fatal = false;
            if (!session.connect_publish(config, error, fatal)) {
                session.net.close();
                if (halt) break;
                if (fatal || ++failures >= 5) {
                    std::lock_guard<std::mutex> g(lock);
                    stats.error = error;
                    failed = true;
                    return;
                }
                for (int i = 0; i < 30 && !halt; i++) { Sleep(100); trim_to_latest_key(); }
                continue;
            }
            failures = 0;
            ever_live = true;
            set_state(StreamStats::Live);
            if (config.audio && !session.send_media(8, 0, {0xAF, 0, 0x11, 0x90})) {   // AAC-LC, 48 kHz, stereo
                session.net.close();
                continue;
            }
            bool video_header = false, wait_key = true;
            double meter_time = now();
            long long meter_bytes = 0;
            bool healthy = true;
            while (!halt && healthy) {
                Packet p;
                {
                    std::unique_lock<std::mutex> g(lock);
                    const double delay = config.delay_ms / 1000.0;
                    // Gecikmede paket, geliş anından "delay" saniye sonra hazır olur
                    wake.wait_for(g, std::chrono::milliseconds(delay > 0 ? 10 : 100),
                                  [&] { return (!queue.empty() && queue.front().at + delay <= now()) || halt; });
                    // Ses ve görüntü paketleri sıra değiştirebilir: fark işaretli hesaplanır
                    const bool backlog = queue.size() > 1 && int64_t(queue.back().ms) - int64_t(queue.front().ms) > 3000 + config.delay_ms;
                    if (backlog) {
                        g.unlock();
                        if (!trim_to_latest_key()) wait_key = true;
                        g.lock();
                    }
                    if (!queue.empty() && queue.front().at + delay <= now()) { p = std::move(queue.front()); queue.pop_front(); }
                }
                if (encoder->failed()) {       // Kodlayıcı durdu: yeniden bağlanmanın anlamı yok
                    session.net.close();
                    std::lock_guard<std::mutex> g(lock);
                    stats.error = encoder->failure();
                    failed = true;
                    return;
                }
                healthy = session.pump();
                if (healthy && !p.data.empty()) {
                    if (p.type == 9 && wait_key) {
                        if (!p.key) { std::lock_guard<std::mutex> g(lock); stats.dropped++; p.data.clear(); }
                        else wait_key = false;
                    }
                    if (p.type == 8 && wait_key) p.data.clear();    // Ses, görüntü başlayana kadar beklemesin diye atılır
                    if (!p.data.empty()) {
                        if (!have_base) { base = p.ms; have_base = true; }
                        const uint32_t ms = p.ms >= base ? p.ms - base : 0;
                        if (p.type == 9 && !video_header) {
                            std::vector<uint8_t> header = {0x17, 0, 0, 0, 0};
                            {
                                std::lock_guard<std::mutex> g(lock);
                                if (!raw_video_header.empty()) header = raw_video_header;
                                else header.insert(header.end(), avc_config.begin(), avc_config.end());
                            }
                            healthy = header.size() > 5 && session.send_media(9, ms, header);
                            video_header = true;
                        }
                        if (healthy) healthy = session.send_media(p.type, ms, p.data);
                        meter_bytes += p.data.size();
                    }
                }
                const double t = now();
                if (t - meter_time >= 1) {
                    std::lock_guard<std::mutex> g(lock);
                    stats.kbps = meter_bytes * 8 / 1000.0 / (t - meter_time);
                    meter_bytes = 0;
                    meter_time = t;
                }
            }
            session.net.close();
        }
    }
};

Streamer::Streamer() : impl_(std::make_unique<Impl>()) {}
Streamer::~Streamer() { stop(); }

bool Streamer::start(ID3D11Device* device, const StreamConfig& config, std::string& error) {
    static const int wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }();
    if (wsa != 0) { error = "Ağ başlatılamadı"; return false; }
    Impl* m = impl_.get();
    m->config = config;
    if (config.use_ffmpeg) {
        auto out = std::make_unique<FfmpegOutput>();
        if (!out->open(device, config.encode, error, [m](const uint8_t* d, size_t n) { m->on_flv(d, n); })) return false;
        m->encoder = std::move(out);
    } else {
        auto handler = [m](int stream, IMFSample* sample) { m->on_sample(stream, sample); };
        auto make_sink = [handler](IMFMediaType* video, IMFMediaType* audio) -> IMFMediaSink* {
            return new PacketSink(video, audio, handler);
        };
        auto out = std::make_unique<Recorder>();
        if (!out->open(device, L"", config.width, config.height, config.fps, "", config.audio_kbps, config.audio, error, make_sink,
                       config.video_kbps))
            return false;
        m->encoder = std::move(out);
    }
    m->started = now();
    m->thread = std::thread(&Impl::run, m);
    return true;
}

void Streamer::write_video(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame) { impl_->encoder->write_video(ctx, frame); }
void Streamer::write_audio(const int16_t* samples, int frames) { impl_->encoder->write_audio(samples, frames); }

void Streamer::stop() {
    Impl* m = impl_.get();
    if (m->thread.joinable()) {
        m->halt = true;
        m->session.net.abort();
        m->wake.notify_all();
        m->thread.join();
    }
    std::string ignored;
    if (m->encoder) m->encoder->finish(ignored);
    std::lock_guard<std::mutex> g(m->lock);
    m->stats.state = StreamStats::Off;
}

StreamStats Streamer::stats() {
    std::lock_guard<std::mutex> g(impl_->lock);
    return impl_->stats;
}

bool Streamer::failed() { return impl_->failed; }
double Streamer::elapsed() const { return impl_->encoder->elapsed(); }
double Streamer::next_frame_time() const { return impl_->encoder->next_frame_time(); }
int Streamer::fps() const { return impl_->config.fps; }
long long Streamer::frames() const { return impl_->encoder->frames(); }
long long Streamer::encoder_dropped() const { return impl_->encoder->dropped(); }
