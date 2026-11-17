// net.h - the smallest TCP, HTTP/1.0 and websocket client skywave needs. Blocking sockets with
// timeouts, waited on in 100 ms slices that look at an abort flag; used only from skywave's worker
// threads, never from process().
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
  // the desktop test build only: the SDK's build links no extra libraries, so Winsock is loaded at run time
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  static const sock_t kNoSock = INVALID_SOCKET;
  namespace sys
  {
  struct Ws2
  {
      decltype(&::WSAStartup) startup; decltype(&::WSAGetLastError) lastError; decltype(&::socket) socket;
      decltype(&::connect) connect; decltype(&::send) send; decltype(&::recv) recv; decltype(&::closesocket) close;
      decltype(&::setsockopt) setsockopt; decltype(&::getaddrinfo) getaddrinfo; decltype(&::freeaddrinfo) freeaddrinfo;
      decltype(&::select) select;
  };
  inline Ws2& ws2()
  {
      static Ws2 w = [] {
          Ws2 f{};
          HMODULE m = LoadLibraryA("ws2_32.dll");
          auto get = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(reinterpret_cast<void*>(GetProcAddress(m, n))); };
          get(f.startup, "WSAStartup"); get(f.lastError, "WSAGetLastError"); get(f.socket, "socket"); get(f.connect, "connect");
          get(f.send, "send"); get(f.recv, "recv"); get(f.close, "closesocket"); get(f.setsockopt, "setsockopt");
          get(f.getaddrinfo, "getaddrinfo"); get(f.freeaddrinfo, "freeaddrinfo"); get(f.select, "select");
          WSADATA d;
          f.startup(MAKEWORD(2, 2), &d);
          return f;
      }();
      return w;
  }
  inline sock_t socket(int a, int b, int c) { return ws2().socket(a, b, c); }
  inline int connect(sock_t s, const sockaddr* a, int n) { return ws2().connect(s, a, n); }
  inline int send(sock_t s, const char* d, int n) { return ws2().send(s, d, n, 0); }
  inline int recv(sock_t s, char* d, int n) { return ws2().recv(s, d, n, 0); }
  inline void close(sock_t s) { ws2().close(s); }
  inline int setsockopt(sock_t s, int l, int o, const char* v, int n) { return ws2().setsockopt(s, l, o, v, n); }
  inline int getaddrinfo(const char* h, const char* p, const addrinfo* hi, addrinfo** r) { return ws2().getaddrinfo(h, p, hi, r); }
  inline void freeaddrinfo(addrinfo* r) { ws2().freeaddrinfo(r); }
  // the desktop test build only: a blocking connect, bounded by the system (Linux below is what ships)
  inline int connectTimed(sock_t s, const sockaddr* a, int n, int, const std::atomic<bool>*) { return ws2().connect(s, a, n) == 0 ? 0 : -1; }
  // 1 readable, 0 timeout, -1 error
  inline int waitReadable(sock_t s, int ms)
  {
      fd_set r; FD_ZERO(&r); FD_SET(s, &r);
      timeval t{ms / 1000, (ms % 1000) * 1000};
      int k = ws2().select(0, &r, nullptr, nullptr, &t);
      return k > 0 ? 1 : k == 0 ? 0 : -1;
  }
  } // namespace sys
#else
  #include <netdb.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  #include <netinet/tcp.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <fcntl.h>
  using sock_t = int;
  static const sock_t kNoSock = -1;
  namespace sys
  {
  inline sock_t socket(int a, int b, int c) { return ::socket(a, b, c); }
  inline int connect(sock_t s, const sockaddr* a, socklen_t n) { return ::connect(s, a, n); }
  inline int send(sock_t s, const char* d, int n) { return (int)::send(s, d, (size_t)n, MSG_NOSIGNAL); }
  inline int recv(sock_t s, char* d, int n) { return (int)::recv(s, d, (size_t)n, 0); }
  inline void close(sock_t s) { ::close(s); }
  inline int setsockopt(sock_t s, int l, int o, const void* v, socklen_t n) { return ::setsockopt(s, l, o, v, n); }
  inline int getaddrinfo(const char* h, const char* p, const addrinfo* hi, addrinfo** r) { return ::getaddrinfo(h, p, hi, r); }
  inline void freeaddrinfo(addrinfo* r) { ::freeaddrinfo(r); }
  // connect within ms, looking at abort every 100 ms: 0 connected, -1 not
  inline int connectTimed(sock_t s, const sockaddr* a, socklen_t n, int ms, const std::atomic<bool>* abort)
  {
      const int flags = ::fcntl(s, F_GETFL, 0);
      ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
      int r = ::connect(s, a, n);
      if (r != 0 && errno == EINPROGRESS)
      {
          r = -1;
          for (int waited = 0; waited < ms && !(abort && abort->load()); waited += 100)
          {
              pollfd f{s, POLLOUT, 0};
              const int k = ::poll(&f, 1, std::min(100, ms - waited));
              if (k < 0 && errno != EINTR) break;
              if (k > 0)
              {
                  int err = 0;
                  socklen_t len = sizeof(err);
                  r = ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0 ? 0 : -1;
                  break;
              }
          }
      }
      ::fcntl(s, F_SETFL, flags);
      return r == 0 ? 0 : -1;
  }
  // 1 readable, 0 timeout, -1 error; poll, not select: the MPC process may hold more than 1024 descriptors
  inline int waitReadable(sock_t s, int ms)
  {
      pollfd f{s, POLLIN, 0};
      int k = ::poll(&f, 1, ms);
      if (k < 0 && errno == EINTR) return 0;
      return k > 0 ? 1 : k == 0 ? 0 : -1;
  }
  } // namespace sys
#endif

namespace net
{

// how skywave introduces itself to list servers and receivers, so an owner can tell what it is
constexpr const char* kAgent = "skywave-mpc/0.6.2 (KiwiSDR client for the Akai MPC; +https://databurn.org/mpc/)";

class Conn
{
public:
    ~Conn() { close(); }
    // 0 open, -1 no answer, -2 the name did not resolve (no network, or no such host)
    int open(const std::string& host, int port, int timeoutMs)
    {
        close();
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (sys::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -2;
        for (addrinfo* a = res; a; a = a->ai_next)
        {
            sock_t s = sys::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSock) continue;
            setSendTimeout(s, timeoutMs);
            rxTimeoutMs_ = timeoutMs;
            if (sys::connectTimed(s, a->ai_addr, (decltype(a->ai_addrlen))a->ai_addrlen, timeoutMs, abort_) == 0) { s_ = s; break; }
            sys::close(s);
        }
        sys::freeaddrinfo(res);
        if (s_ == kNoSock) return -1;
        int one = 1;
        sys::setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        return 0;
    }
    void close() { if (s_ != kNoSock) { sys::close(s_); s_ = kNoSock; } }
    bool isOpen() const { return s_ != kNoSock; }
    void setTimeoutMs(int ms) { rxTimeoutMs_ = ms; }
    // a flag the waits look at every 100 ms, so the owner can stop a slow call (closing the plugin)
    void setAbort(const std::atomic<bool>* a) { abort_ = a; }

    bool sendAll(const void* data, size_t n)
    {
        const char* p = (const char*)data;
        while (n > 0)
        {
            int k = sys::send(s_, p, (int)n);
            if (k <= 0) return false;
            p += k; n -= (size_t)k;
        }
        return true;
    }
    // >0 bytes, 0 closed, -1 timeout, -2 error
    int recvSome(void* buf, int n)
    {
        int w = 0;
        for (int waited = 0; w == 0 && waited < rxTimeoutMs_; waited += 100)   // in slices, so abort is seen
        {
            if (abort_ && abort_->load()) return -2;
            w = sys::waitReadable(s_, std::min(100, rxTimeoutMs_ - waited));
        }
        if (w <= 0) return w == 0 ? -1 : -2;
        int k = sys::recv(s_, (char*)buf, n);
        return k > 0 ? k : k == 0 ? 0 : -2;
    }

private:
    // bounds send() and, on Linux, connect()
    static void setSendTimeout(sock_t s, int ms)
    {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
#else
        timeval t{ms / 1000, (ms % 1000) * 1000};
#endif
        sys::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
    }
    sock_t s_ = kNoSock;
    int rxTimeoutMs_ = 5000;
    const std::atomic<bool>* abort_ = nullptr;
};

// "http://host:port/path" -> parts; port 80 when absent
inline bool splitUrl(const std::string& url, std::string& host, int& port, std::string& path)
{
    std::string u = url;
    if (u.rfind("http://", 0) == 0) u = u.substr(7);
    else if (u.find("://") != std::string::npos) return false;
    size_t slash = u.find('/');
    std::string hp = slash == std::string::npos ? u : u.substr(0, slash);
    path = slash == std::string::npos ? "/" : u.substr(slash);
    size_t colon = hp.rfind(':');
    port = 80;
    if (colon != std::string::npos) { port = std::atoi(hp.c_str() + colon + 1); hp = hp.substr(0, colon); }
    host = hp;
    return !host.empty() && port > 0;
}

// a whole small file over HTTP/1.0 (no chunked encoding); body only
inline bool httpGet(const std::string& url, std::string& body, size_t maxBytes, int timeoutMs, const std::atomic<bool>* abort = nullptr)
{
    std::string host, path;
    int port;
    if (!splitUrl(url, host, port, path)) return false;
    Conn c;
    c.setAbort(abort);
    if (c.open(host, port, timeoutMs) != 0) return false;
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: " + std::string(kAgent) + "\r\nConnection: close\r\n\r\n";
    if (!c.sendAll(req.data(), req.size())) return false;
    std::string all;
    char buf[16384];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4 * timeoutMs);   // a trickle ends too
    for (;;)
    {
        int k = c.recvSome(buf, sizeof(buf));
        if (k == 0) break;
        if (k < 0 || std::chrono::steady_clock::now() > deadline) return false;
        all.append(buf, (size_t)k);
        if (all.size() > maxBytes) return false;
    }
    size_t h = all.find("\r\n\r\n");
    if (h == std::string::npos || (all.compare(0, 9, "HTTP/1.1 ") != 0 && all.compare(0, 9, "HTTP/1.0 ") != 0)) return false;
    if (all.compare(9, 3, "200") != 0) return false;
    std::string head = all.substr(0, h + 2);
    for (char& ch : head) ch = (char)std::tolower((unsigned char)ch);
    const size_t cl = head.find("\r\ncontent-length:");
    if (cl != std::string::npos && all.size() - (h + 4) != (size_t)std::atoll(head.c_str() + cl + 17)) return false;   // cut short
    body = all.substr(h + 4);
    return true;
}

// Client websocket (RFC 6455), text out, whole messages in.
class WebSocket
{
public:
    // 0 ok, else HTTP status (e.g. 307 with redirect filled in as "host:port"), -1 no answer, -2 name not found
    int connect(const std::string& host, int port, const std::string& path, int timeoutMs, std::string& redirectHost, int& redirectPort)
    {
        rx_.clear();
        scanPos_ = 0;
        scanTotal_ = 0;
        if (const int o = c_.open(host, port, timeoutMs)) return o;
        uint8_t key[16];
        for (auto& b : key) b = (uint8_t)rng_();
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + (port == 80 ? "" : ":" + std::to_string(port)) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(key, 16) +
                          "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + std::string(kAgent) + "\r\n\r\n";
        if (!c_.sendAll(req.data(), req.size())) { c_.close(); return -1; }
        std::string head;
        char ch;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);   // the whole reply, not each byte
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos)
        {
            int k = c_.recvSome(&ch, 1);
            if (k <= 0 || std::chrono::steady_clock::now() > deadline) { c_.close(); return -1; }
            head += ch;
        }
        int status = head.size() > 12 ? std::atoi(head.c_str() + 9) : 0;
        if (status == 101) return 0;
        size_t loc = head.find("\r\nLocation: ");
        if (loc != std::string::npos)
        {
            size_t e = head.find("\r\n", loc + 12);
            std::string p;
            splitUrl(head.substr(loc + 12, e - loc - 12), redirectHost, redirectPort, p);
        }
        c_.close();
        return status ? status : -1;
    }
    void close() { c_.close(); rx_.clear(); scanPos_ = 0; scanTotal_ = 0; }
    bool isOpen() const { return c_.isOpen(); }
    void setTimeoutMs(int ms) { c_.setTimeoutMs(ms); }
    void setAbort(const std::atomic<bool>* a) { c_.setAbort(a); }

    bool sendText(const std::string& s) { return sendFrame(0x1, (const uint8_t*)s.data(), s.size()); }

    // 1 message in out, 0 timeout (nothing complete yet), -1 closed or error
    int receive(std::vector<uint8_t>& out)
    {
        for (;;)
        {
            int r = parse(out);
            if (r != 0) return r;
            uint8_t buf[8192];
            int k = c_.recvSome(buf, sizeof(buf));
            if (k == -1) return 0;
            if (k <= 0) return -1;
            rx_.insert(rx_.end(), buf, buf + k);
            if (rx_.size() > kMaxMessage + 14) return -1;   // a message that never ends
        }
    }

private:
    static constexpr uint64_t kMaxMessage = 16u << 20;

    // the frame at pos: its header length h and payload length n; false while it is not all here
    bool frameAt(size_t pos, size_t& h, uint64_t& n) const
    {
        if (rx_.size() - pos < 2) return false;
        const uint8_t b1 = rx_[pos + 1];
        n = b1 & 127;
        h = 2;
        if (n == 126) { if (rx_.size() - pos < 4) return false; n = ((uint64_t)rx_[pos + 2] << 8) | rx_[pos + 3]; h = 4; }
        else if (n == 127)
        {
            if (rx_.size() - pos < 10) return false;
            n = 0;
            for (int i = 0; i < 8; ++i) n = (n << 8) | rx_[pos + 2 + i];
            h = 10;
        }
        if (b1 & 0x80) h += 4;
        return n <= kMaxMessage && rx_.size() - pos >= h + n;
    }

    // payload of the frame at pos, unmasked, onto out
    void payload(size_t pos, size_t h, uint64_t n, std::vector<uint8_t>& out) const
    {
        const size_t at = out.size();
        out.insert(out.end(), rx_.begin() + (long)(pos + h), rx_.begin() + (long)(pos + h + n));
        if (rx_[pos + 1] & 0x80)
            for (size_t i = 0; i < n; ++i) out[at + i] ^= rx_[pos + h - 4 + (i & 3)];
    }

    // One whole message from rx_, joining continuation frames; pings, which may come between them,
    // are answered and taken out. The scan resumes where the last call stopped, and the payload is
    // copied once, when the last frame is in: a long or endless message costs no more than its bytes.
    int parse(std::vector<uint8_t>& out)
    {
        for (;;)
        {
            size_t h;
            uint64_t n;
            const size_t pos = scanPos_;
            if (rx_.size() - pos >= 10 && (rx_[pos + 1] & 127) == 127)   // an absurd length fails now, not when it has all come
            {
                uint64_t len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | rx_[pos + 2 + i];
                if (len > kMaxMessage) return -1;
            }
            if (!frameAt(pos, h, n)) return 0;
            const int op = rx_[pos] & 15;
            if (op == 0x8) return -1;
            if (op & 0x8)                                   // a control frame: at most 125 bytes, a ping answered, then gone
            {
                if (n > 125) return -1;
                std::vector<uint8_t> data;
                payload(pos, h, n, data);
                if (op == 0x9) sendFrame(0xA, data.data(), data.size());
                rx_.erase(rx_.begin() + (long)pos, rx_.begin() + (long)(pos + h + n));
                continue;
            }
            scanTotal_ += n;
            if (scanTotal_ > kMaxMessage) return -1;
            scanPos_ = pos + h + (size_t)n;
            if (!(rx_[pos] & 0x80)) continue;
            std::vector<uint8_t> msg;
            msg.reserve((size_t)scanTotal_);
            for (size_t p = 0; p < scanPos_;)
            {
                frameAt(p, h, n);
                payload(p, h, n, msg);
                p += h + (size_t)n;
            }
            rx_.erase(rx_.begin(), rx_.begin() + (long)scanPos_);
            scanPos_ = 0;
            scanTotal_ = 0;
            out.swap(msg);
            return 1;
        }
    }

    bool sendFrame(int op, const uint8_t* d, size_t n)
    {
        std::vector<uint8_t> f;
        f.push_back((uint8_t)(0x80 | op));
        if (n < 126) f.push_back((uint8_t)(0x80 | n));
        else { f.push_back(0x80 | 126); f.push_back((uint8_t)(n >> 8)); f.push_back((uint8_t)n); }
        uint8_t mask[4];
        for (auto& b : mask) b = (uint8_t)rng_();
        f.insert(f.end(), mask, mask + 4);
        for (size_t i = 0; i < n; ++i) f.push_back(d[i] ^ mask[i & 3]);
        return c_.sendAll(f.data(), f.size());
    }

    static std::string base64(const uint8_t* d, size_t n)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string s;
        for (size_t i = 0; i < n; i += 3)
        {
            uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
            s += t[(v >> 18) & 63]; s += t[(v >> 12) & 63];
            s += i + 1 < n ? t[(v >> 6) & 63] : '=';
            s += i + 2 < n ? t[v & 63] : '=';
        }
        return s;
    }

    Conn c_;
    std::vector<uint8_t> rx_;
    size_t scanPos_ = 0;                                    // rx_ up to here is whole data frames of one message
    uint64_t scanTotal_ = 0;                                // and their payload
    std::minstd_rand rng_{std::random_device{}()};
};

} // namespace net
