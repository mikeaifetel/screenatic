// Screenatic's receiver core on Vega OS: mDNS advertising of the AirPlay server (UxPlay, see
// AirPlayTest) on UDP 5353, shared with the system's avahi, and the DLNA MediaRenderer
// (DlnaRenderer): SSDP on UDP 1900 and its HTTP server on TCP 49595 (a fixed port: Vega blocks
// inbound LAN connections to OS-picked ones).
//
// Debug mode (debug builds) also starts what the feasibility probe used: the diagnostics server
// on TCP 7000 (GET /status, /mdns, /msse, /play, /dump.*), the TCP 7100 video ingest and the
// fMP4 queues read by JS (MSE). None of it runs in a release build.

#include "ProbeNative.h"
#include "Timeline.h"
#include "AirPlayTest.h"
#include "DlnaRenderer.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace com::amazon::kepler::turbomodule;

namespace ProbeNativeTurboModule {

namespace {

constexpr int kAirPlayPort = 7000;
constexpr int kDlnaPort = 49595;
constexpr int kVideoPort = 7100;
constexpr size_t kVideoCap = 16 * 1024 * 1024;

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string jsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) o += '?'; else o += c;
    }
  }
  return o;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

struct Lan {
  std::string ifname;
  in_addr ip{};
  bool ok = false;
};

// Prefer wlan*/eth* with a private IPv4 address.
Lan findLan() {
  Lan best;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return best;
  int bestRank = 99;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    auto* sin = reinterpret_cast<sockaddr_in*>(i->ifa_addr);
    uint32_t a = ntohl(sin->sin_addr.s_addr);
    bool priv = (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8;
    if (!priv) continue;
    std::string name = i->ifa_name ? i->ifa_name : "";
    int rank = (name.rfind("wlan", 0) == 0 || name.rfind("eth", 0) == 0) ? 0 : 1;
    if (rank < bestRank) {
      bestRank = rank;
      best.ifname = name;
      best.ip = sin->sin_addr;
      best.ok = true;
    }
  }
  freeifaddrs(list);
  return best;
}

std::string ipStr(in_addr a) {
  char buf[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &a, buf, sizeof buf);
  return buf;
}

// ---- minimal DNS encoding for mDNS ----

void putName(std::vector<uint8_t>& b, const std::string& name) {
  size_t start = 0;
  while (start < name.size()) {
    size_t dot = name.find('.', start);
    // Instance names may contain spaces but not dots, so a plain split is fine here.
    if (dot == std::string::npos) dot = name.size();
    size_t len = dot - start;
    b.push_back(static_cast<uint8_t>(len));
    b.insert(b.end(), name.begin() + start, name.begin() + dot);
    start = dot + 1;
  }
  b.push_back(0);
}

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xFF); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }

void putRecord(std::vector<uint8_t>& b, const std::string& name, uint16_t type, bool flush,
               uint32_t ttl, const std::vector<uint8_t>& rdata) {
  putName(b, name);
  put16(b, type);
  put16(b, flush ? 0x8001 : 0x0001);
  put32(b, ttl);
  put16(b, static_cast<uint16_t>(rdata.size()));
  b.insert(b.end(), rdata.begin(), rdata.end());
}

// Reads a (possibly compressed) name; returns false on malformed input.
bool readName(const uint8_t* p, size_t len, size_t& off, std::string& out) {
  out.clear();
  size_t pos = off;
  bool jumped = false;
  int hops = 0;
  while (pos < len) {
    uint8_t l = p[pos];
    if (l == 0) {
      if (!jumped) off = pos + 1;
      return true;
    }
    if ((l & 0xC0) == 0xC0) {
      if (pos + 1 >= len || ++hops > 16) return false;
      size_t target = ((l & 0x3F) << 8) | p[pos + 1];
      if (!jumped) off = pos + 2;
      jumped = true;
      pos = target;
      continue;
    }
    if (pos + 1 + l > len) return false;
    if (!out.empty()) out += '.';
    out.append(reinterpret_cast<const char*>(p + pos + 1), l);
    pos += 1 + l;
  }
  return false;
}

}  // namespace

class Probe : public std::enable_shared_from_this<Probe> {
public:
  std::string start(const std::string& deviceName, bool debug) {
    std::lock_guard<std::mutex> g(startMu_);
    if (running_) return status();
    debug_ = debug;
    name_ = deviceName.empty() ? "Screenatic TV" : deviceName;
    deviceId_ = persistentDeviceId();
    // Several TVs on one network: "Screenatic TV 3F2A", the last 4 hex digits of this TV's
    // AirPlay device id (random at first launch, then kept in /data).
    std::string hex;
    for (char c : deviceId_) if (c != ':') hex += c;
    if (hex.size() >= 4) name_ += " " + hex.substr(hex.size() - 4);
    lan_ = findLan();
    log(lan_.ok ? "LAN " + lan_.ifname + " " + ipStr(lan_.ip) : "no private IPv4 interface found");
    running_ = true;
    dlna_->start(name_, uuid());
    auto self = shared_from_this();
    threads_.emplace_back([self] { self->mdnsLoop(); });
    threads_.emplace_back([self] { self->tcpLoop(kDlnaPort, "dlna"); });
    threads_.emplace_back([self] { self->ssdpLoop(); });
    if (debug) {
      threads_.emplace_back([self] { self->tcpLoop(kAirPlayPort, "airplay"); });
      threads_.emplace_back([self] { self->tcpLoop(kVideoPort, "video"); });
    }
    return status();
  }

  void stop() {
    std::lock_guard<std::mutex> g(startMu_);
    if (!running_) return;
    running_ = false;
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
    dlna_->stop();
    log("stopped");
  }

  ~Probe() { stop(); }

  const std::string& deviceId() const { return deviceId_; }
  bool debug() const { return debug_; }
  const std::string& name() const { return name_; }
  /** The DLNA renderer (always there; started by start()). */
  const std::shared_ptr<DlnaRenderer>& dlna() const { return dlna_; }
  std::string lanIp() const { return lan_.ok ? ipStr(lan_.ip) : ""; }

  /** fMP4 from the AirPlay mirror, drained by readVideo() like the :7100 ingest. */
  // An init segment (starts with an ftyp box) opens a new stream: whatever is still queued from
  // the previous session is useless to it, and the reader must see the init first so JS can
  // rebuild its MediaSource.
  static bool startsStream(const std::vector<uint8_t>& b) {
    return b.size() >= 8 && memcmp(b.data() + 4, "ftyp", 4) == 0;
  }

  void pushMedia(std::vector<uint8_t>&& bytes) {
    if (!debug_) return;  // MSE path (debug only): the mirror plays in MssePlayer
    std::lock_guard<std::mutex> g(mu_);
    // Never drop queued video: JS trims the SourceBuffer at keyframe times taken from the muxer's
    // timeline, which only matches what MSE holds if every byte gets appended. (The iPhone also
    // repeats SPS/PPS with every keyframe, so "new stream" isn't a session boundary anyway.)
    videoReceived_ += bytes.size();
    if (video_.size() + bytes.size() > kVideoCap) {
      // JS stopped draining: drop everything; the stream recovers at the next keyframe only if
      // the muxer re-sends its init segment, which happens on the next session.
      videoDropped_ += video_.size();
      video_.clear();
    }
    video_.insert(video_.end(), bytes.begin(), bytes.end());
  }

  /** FLAC fMP4 from the AirPlay mirror, drained by readAudio(). Capped at ~4 s of audio. */
  void pushAudio(std::vector<uint8_t>&& bytes) {
    if (!debug_) return;
    std::lock_guard<std::mutex> g(mu_);
    if (audioOff_) return;  // debug: GET /audio-off
    if (startsStream(bytes)) audio_.clear();
    audioReceived_ += bytes.size();
    if (audio_.size() + bytes.size() > 2 * 1024 * 1024) {
      audioDropped_ += audio_.size();
      audio_.clear();
    }
    audio_.insert(audio_.end(), bytes.begin(), bytes.end());
  }

  std::vector<uint8_t> readAudio(size_t max) {
    std::lock_guard<std::mutex> g(mu_);
    size_t n = std::min(max, audio_.size());
    std::vector<uint8_t> out(audio_.begin(), audio_.begin() + n);
    audio_.erase(audio_.begin(), audio_.begin() + n);
    return out;
  }

  /** A random locally administered MAC as the AirPlay deviceid, kept in /data (the app's storage)
   *  so the TV stays the same receiver for senders across launches. A new id each launch made
   *  the iPhone treat it as a new device every time. */
  static std::string persistentDeviceId() {
    const char* file = "/data/airplay_device_id";
    char buf[32] = {0};
    if (FILE* f = fopen(file, "r")) {
      bool ok = fgets(buf, sizeof buf, f) != nullptr;
      fclose(f);
      std::string id(buf);
      while (!id.empty() && (id.back() == '\n' || id.back() == '\r')) id.pop_back();
      if (ok && id.size() == 17) return id;
    }
    std::mt19937 rng(static_cast<uint32_t>(nowMs()) ^ static_cast<uint32_t>(getpid()));
    char mac[18];
    snprintf(mac, sizeof mac, "02:%02X:%02X:%02X:%02X:%02X", rng() & 0xFF, rng() & 0xFF, rng() & 0xFF,
             rng() & 0xFF, rng() & 0xFF);
    if (FILE* f = fopen(file, "w")) {
      fprintf(f, "%s\n", mac);
      fclose(f);
    }
    return mac;
  }

  /** Clears feature bit 27 (SupportsLegacyPairing) in a "0xLOW,0xHIGH" features value. With it
   *  advertised, an iPhone waited ~5 s after the tap before connecting (FireMirror leaves it out
   *  and connects in under a second); without it the iPhone connects at once and pairs the same
   *  way (pair-setup / pair-verify), mirror picture and sound unchanged. */
  static std::string withoutLegacyPairing(const std::string& v) {
    size_t comma = v.find(',');
    unsigned long low = strtoul(v.substr(0, comma).c_str(), nullptr, 0);
    low &= ~(1ul << 27);
    char buf[16];
    snprintf(buf, sizeof buf, "0x%lX", low);
    return buf + (comma == std::string::npos ? std::string() : v.substr(comma));
  }

  /** For helpers outside the class to write to the on-screen log. */
  void note(const std::string& line) { log(line); }

  /** Advertise the real AirPlay server (UxPlay) instead of the probe's fake record. */
  void setAirPlay(int port, const std::map<std::string, std::string>& txt,
                  const std::map<std::string, std::string>& raopTxt) {
    {
      std::lock_guard<std::mutex> g(mu_);
      airplayPort_ = port;
      airplayTxt_.clear();
      for (auto& [k, v] : txt) airplayTxt_.push_back(k + "=" + (k == "features" ? withoutLegacyPairing(v) : v));
      raopTxt_.clear();
      for (auto& [k, v] : raopTxt) raopTxt_.push_back(k + "=" + (k == "ft" ? withoutLegacyPairing(v) : v));
    }
    reannounce_ = true;
    log("mDNS now advertising UxPlay on :" + std::to_string(port));
  }

  /** JSON text reported by the JS side (player state), returned under "js" in status(). */
  void report(const std::string& section, const std::string& json) {
    std::lock_guard<std::mutex> g(mu_);
    jsReports_[section] = json;
  }

  void setDump(std::function<std::string(const std::string&)> f) {
    std::lock_guard<std::mutex> g(mu_);
    dump_ = std::move(f);
  }

  void setDebugVideo(std::function<void(const uint8_t*, size_t, int64_t, bool)> f) {
    std::lock_guard<std::mutex> g(mu_);
    debugVideo_ = std::move(f);
  }

  void setDebugMsse(std::function<std::string(const std::string&)> f) {
    std::lock_guard<std::mutex> g(mu_);
    debugMsse_ = std::move(f);
  }

  void setDebugPlay(std::function<void(const std::string&)> f) {
    std::lock_guard<std::mutex> g(mu_);
    debugPlay_ = std::move(f);
  }

  /** Extra JSON object appended to status() as "airplay". */
  void setAirPlayStatus(std::function<std::string()> f) {
    std::lock_guard<std::mutex> g(mu_);
    airplayStatus_ = std::move(f);
  }

  std::string status() {
    std::lock_guard<std::mutex> g(mu_);
    std::ostringstream o;
    o << "{\"running\":" << (running_ ? "true" : "false")
      << ",\"ip\":\"" << (lan_.ok ? ipStr(lan_.ip) : "") << "\""
      << ",\"iface\":\"" << jsonEsc(lan_.ifname) << "\""
      << ",\"name\":\"" << jsonEsc(name_) << "\""
      << ",\"ports\":{";
    bool first = true;
    for (auto& [k, v] : portState_) {
      o << (first ? "" : ",") << "\"" << k << "\":\"" << jsonEsc(v) << "\"";
      first = false;
    }
    o << "},\"tcpAccepted\":" << tcpAccepted_
      << ",\"peers\":[";
    first = true;
    for (auto& p : peers_) { o << (first ? "" : ",") << "\"" << jsonEsc(p) << "\""; first = false; }
    o << "],\"mdns\":{\"state\":\"" << jsonEsc(mdnsState_) << "\",\"queriesSeen\":" << mdnsQueries_
      << ",\"answered\":" << mdnsAnswered_ << ",\"airplayQueries\":" << airplayQueries_ << "}"
      << ",\"ssdp\":{\"state\":\"" << jsonEsc(ssdpState_) << "\",\"searchesSeen\":" << ssdpSearches_
      << ",\"answered\":" << ssdpAnswered_ << "}"
      << ",\"video\":{\"received\":" << videoReceived_ << ",\"buffered\":" << video_.size()
      << ",\"dropped\":" << videoDropped_ << "}";
    if (airplayStatus_) o << ",\"airplay\":" << airplayStatus_();
    if (debug_) o << ",\"dlna\":" << dlna_->statusJson();
    o << ",\"js\":{";
    bool firstJs = true;
    for (auto& [k, v] : jsReports_) {
      o << (firstJs ? "" : ",") << "\"" << jsonEsc(k) << "\":" << (v.empty() ? "null" : v);
      firstJs = false;
    }
    o << "}";
    o << ",\"log\":[";
    first = true;
    for (auto& l : log_) { o << (first ? "" : ",") << "\"" << jsonEsc(l) << "\""; first = false; }
    o << "]}";
    return o.str();
  }

  std::vector<uint8_t> readVideo(size_t max) {
    std::lock_guard<std::mutex> g(mu_);
    size_t n = std::min(max, video_.size());
    std::vector<uint8_t> out(video_.begin(), video_.begin() + n);
    video_.erase(video_.begin(), video_.begin() + n);
    return out;
  }

private:
  void log(const std::string& line) {
    std::lock_guard<std::mutex> g(mu_);
    char ts[16];
    snprintf(ts, sizeof ts, "%6.1f ", (nowMs() - t0_) / 1000.0);
    log_.push_back(ts + line);
    while (log_.size() > 40) log_.pop_front();
  }

  void setPort(const std::string& k, const std::string& v) {
    std::lock_guard<std::mutex> g(mu_);
    portState_[k] = v;
  }

  void addPeer(const std::string& p) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& e : peers_) if (e == p) return;
    peers_.push_back(p);
    if (peers_.size() > 20) peers_.erase(peers_.begin());
  }

  // Waits until fd is readable, the probe stops, or timeoutMs passes. True if readable.
  bool waitReadable(int fd, int timeoutMs) {
    pollfd p{fd, POLLIN, 0};
    int left = timeoutMs;
    while (running_ && left > 0) {
      int r = poll(&p, 1, std::min(left, 250));
      if (r > 0) return true;
      if (r < 0 && errno != EINTR) return false;
      left -= 250;
    }
    return false;
  }

  void tcpLoop(int port, const std::string& tag) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
      std::string err = std::string("bind/listen failed: ") + strerror(errno);
      setPort(tag, err);
      log(tag + " :" + std::to_string(port) + " " + err);
      close(s);
      return;
    }
    setPort(tag, "listening :" + std::to_string(port));
    log(tag + " listening on :" + std::to_string(port));
    while (running_) {
      if (!waitReadable(s, 1000)) continue;
      sockaddr_in peer{};
      socklen_t pl = sizeof peer;
      int c = accept(s, reinterpret_cast<sockaddr*>(&peer), &pl);
      if (c < 0) continue;
      std::string who = ipStr(peer.sin_addr);
      {
        std::lock_guard<std::mutex> g(mu_);
        tcpAccepted_++;
      }
      addPeer(who + " -> " + tag);
      if (tag == "dlna") {
        // Controllers poll every second, one connection each: no log line here.
        std::thread([d = dlna_, c, who] { d->serve(c, who); }).detach();
        continue;
      }
      log(tag + " connection from " + who);
      if (tag == "video") {
        videoIngest(c, who);
      } else {
        std::thread([self = shared_from_this(), c, tag, who] { self->serveHttp(c, tag, who); }).detach();
      }
    }
    close(s);
  }

  // Reads one request on a debug port and answers it.
  void serveHttp(int c, std::string tag, std::string who) {
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
      if (!waitReadable(c, 3000)) break;
      ssize_t n = recv(c, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.append(buf, n);
    }
    std::string line = req.substr(0, req.find("\r\n"));
    log(tag + " " + who + ": " + line.substr(0, 80));
    std::string body, type = "text/plain; charset=utf-8";
    if (line.find("GET /audio-off") == 0 || line.find("GET /audio-on") == 0) {
      // Debug: mute the mirror's audio stream (isolates audio/video player interactions).
      audioOff_ = line.find("GET /audio-off") == 0;
      body = std::string("mirror audio ") + (audioOff_ ? "off" : "on") + "\n";
    } else if (line.find("GET /dump.") == 0) {
      std::string kind = line.substr(strlen("GET /dump."), line.find(' ', 10) - strlen("GET /dump."));
      std::function<std::string(const std::string&)> f;
      {
        std::lock_guard<std::mutex> g(mu_);
        f = dump_;
      }
      body = f ? f(kind) : std::string();
      type = "application/octet-stream";
    } else if (line.find("GET /play?url=") == 0) {
      // Debug: play an arbitrary URL in the AirPlay video player (format tests without a sender).
      size_t s = strlen("GET /play?url="), e = line.find(' ', s);
      std::string url = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
      std::function<void(const std::string&)> f;
      {
        std::lock_guard<std::mutex> g(mu_);
        f = debugPlay_;
      }
      if (f) f(url);
      body = "queued " + url + "\n";
    } else if (line.find("GET /msse") == 0) {
      // Debug: switch the native video player's sample format (MssePlayer::command).
      size_t s = strlen("GET /msse"), e = line.find(' ', s);
      std::string q = line.substr(s, e == std::string::npos ? std::string::npos : e - s);
      std::function<std::string(const std::string&)> f;
      {
        std::lock_guard<std::mutex> g(mu_);
        f = debugMsse_;
      }
      body = (f ? f(q) : std::string("airplay not started")) + "\n";
    } else if (line.find("GET /mdns") == 0) {
      size_t s = line.find('?'), e = line.find(' ', 9);
      std::string q = (s != std::string::npos && s < e) ? line.substr(s + 1, e - s - 1) : "";
      body = setTxtOverrides(q) + "\n";
    } else if (line.find("GET /status") == 0) {
      // Remote diagnostics: everything on screen, plus what the JS players report.
      body = status();
      type = "application/json";
    } else {
      body = "Screenatic Vega probe: " + tag + " port reached from " + who + "\n";
    }
    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    // Loop: debug dumps are megabytes and send() may take only part of them.
    for (size_t off = 0; off < resp.size();) {
      ssize_t n = send(c, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    close(c);
  }

  /** Reads exactly n bytes (false on EOF/stop). */
  bool readFull(int c, uint8_t* p, size_t n) {
    while (n > 0 && running_) {
      if (!waitReadable(c, 5000)) continue;
      ssize_t r = recv(c, p, n, 0);
      if (r <= 0) return false;
      p += r;
      n -= static_cast<size_t>(r);
    }
    return n == 0;
  }

  /** Debug ingest: "SCRV", then per access unit u32 size, i64 ptsUs (big-endian), Annex-B bytes.
   *  Goes through the AirPlay mirror path (muxer, keyframe hold) instead of straight to MSE. */
  void annexBIngest(int c, const std::string& who) {
    std::function<void(const uint8_t*, size_t, int64_t, bool)> f;
    {
      std::lock_guard<std::mutex> g(mu_);
      f = debugVideo_;
    }
    uint8_t hdr[12];
    std::vector<uint8_t> au;
    uint64_t frames = 0;
    while (f && running_ && readFull(c, hdr, 12)) {
      uint32_t n = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16) | (uint32_t(hdr[2]) << 8) | hdr[3];
      int64_t pts = 0;
      for (int i = 4; i < 12; ++i) pts = (pts << 8) | hdr[i];
      if (n > (16u << 20)) break;
      au.resize(n);
      if (!readFull(c, au.data(), n)) break;
      f(au.data(), n, pts, frames == 0);
      ++frames;
    }
    close(c);
    log("annex-b stream from " + who + " ended: " + std::to_string(frames) + " frames");
  }

  void videoIngest(int c, const std::string& who) {
    {
      uint8_t magic[4];
      if (recv(c, magic, 4, MSG_PEEK | MSG_WAITALL) == 4 && memcmp(magic, "SCRV", 4) == 0) {
        recv(c, magic, 4, 0);
        annexBIngest(c, who);
        return;
      }
    }
    std::vector<uint8_t> buf(64 * 1024);
    int64_t started = nowMs();
    uint64_t total = 0;
    while (running_) {
      if (!waitReadable(c, 5000)) continue;
      ssize_t n = recv(c, buf.data(), buf.size(), 0);
      if (n <= 0) break;
      total += n;
      std::lock_guard<std::mutex> g(mu_);
      videoReceived_ += n;
      video_.insert(video_.end(), buf.begin(), buf.begin() + n);
      if (video_.size() > kVideoCap) {
        size_t drop = video_.size() - kVideoCap;
        video_.erase(video_.begin(), video_.begin() + drop);
        videoDropped_ += drop;
      }
    }
    close(c);
    double secs = (nowMs() - started) / 1000.0;
    log("video stream from " + who + " ended: " + std::to_string(total / 1024) + " KiB in " +
        std::to_string(static_cast<int>(secs)) + " s");
  }

  /** The DLNA device's UDN: stable, from the persistent device id. */
  std::string uuid() {
    std::string id = deviceId_;
    std::string hex;
    for (char ch : id) if (ch != ':') hex += static_cast<char>(tolower(ch));
    return "5c4e0b1a-7e57-4d0b-9a1e-" + hex;
  }

  // ---- mDNS: advertise _airplay._tcp (what iPhone's Screen Mirroring lists) ----

  /** Unique per TV (two Screenatic TVs on one network must not claim the same host name). */
  std::string hostName() {
    std::string hex;
    for (char c : deviceId_) if (c != ':') hex += static_cast<char>(tolower(c));
    return "screenatic-" + (hex.size() > 6 ? hex.substr(hex.size() - 6) : hex) + ".local";
  }
  std::string airplayType() { return "_airplay._tcp.local"; }
  std::string airplayInstance() { return name_ + "." + airplayType(); }
  // _raop._tcp (AirPlay audio) as UxPlay registers it: "<deviceid hex>@<name>", same port. An
  // iPhone browses it too and connected ~6 s after opening Screen Mirroring while it was missing.
  /** Debug (GET /mdns?key=value&...): replace or add TXT keys to compare with other receivers.
   *  features/flags also set the RAOP record's ft/sf; "key=" removes a key; no query = reset. */
  std::vector<std::string> withOverrides(std::vector<std::string> kv, bool raop) {
    std::map<std::string, std::string> ov;
    {
      std::lock_guard<std::mutex> g(mu_);
      ov = txtOverrides_;
    }
    for (const auto& entry : ov) {
      std::string k = entry.first;
      const std::string& v = entry.second;
      if (raop) {
        if (k == "features") k = "ft";
        else if (k == "flags") k = "sf";
        else if (k == "srcvers") k = "vs";
        else if (k != "pk") continue;
      }
      bool found = false;
      for (auto it = kv.begin(); it != kv.end();) {
        if (it->compare(0, k.size() + 1, k + "=") == 0) {
          found = true;
          if (v.empty()) { it = kv.erase(it); continue; }
          *it = k + "=" + v;
        }
        ++it;
      }
      if (!found && !v.empty()) kv.push_back(k + "=" + v);
    }
    return kv;
  }

  std::string setTxtOverrides(const std::string& query) {
    std::map<std::string, std::string> ov;
    size_t i = 0;
    while (i < query.size()) {
      size_t amp = query.find('&', i);
      std::string part = query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
      size_t eq = part.find('=');
      if (eq != std::string::npos && eq > 0) ov[part.substr(0, eq)] = part.substr(eq + 1);
      if (amp == std::string::npos) break;
      i = amp + 1;
    }
    std::string r;
    {
      std::lock_guard<std::mutex> g(mu_);
      txtOverrides_ = ov;
    }
    for (auto& [k, v] : ov) r += k + "=" + (v.empty() ? "(removed)" : v) + " ";
    reannounce_ = true;
    Timeline::add("mDNS TXT overrides: " + (r.empty() ? std::string("none") : r));
    return r.empty() ? "overrides cleared" : r;
  }

  std::string raopType() { return "_raop._tcp.local"; }
  std::string raopInstance() {
    std::string hex;
    for (char ch : deviceId_) if (ch != ':') hex += static_cast<char>(toupper(ch));
    return hex + "@" + name_ + "." + raopType();
  }

  std::vector<uint8_t> txtAirPlay() {
    std::vector<std::string> kv;
    {
      std::lock_guard<std::mutex> g(mu_);
      kv = airplayTxt_;
    }
    if (kv.empty()) {
      kv = {"deviceid=" + deviceId_, "features=0x5A7FFEE6", "flags=0x4", "model=AppleTV3,2",
            "pk=b07727d6f6cd6e08b58ede525ec3cdeaa252ad9f683feb212ef8a205246554e7",
            "pi=2e388006-13ba-4041-9a67-25dd4a43d536", "srcvers=220.68", "vv=2"};
    }
    kv = withOverrides(kv, false);
    std::vector<uint8_t> r;
    for (auto& s : kv) {
      r.push_back(static_cast<uint8_t>(s.size()));
      r.insert(r.end(), s.begin(), s.end());
    }
    return r;
  }

  std::vector<uint8_t> mdnsAnnouncement() {
    std::vector<std::string> raopKv;
    {
      std::lock_guard<std::mutex> g(mu_);
      raopKv = raopTxt_;
    }
    const bool raop = !raopKv.empty();
    raopKv = withOverrides(raopKv, true);  // only once UxPlay runs (the probe has no RAOP server)
    std::vector<uint8_t> b;
    put16(b, 0); put16(b, 0x8400); put16(b, 0); put16(b, raop ? 10 : 6); put16(b, 0); put16(b, 0);
    std::vector<uint8_t> ptr; putName(ptr, airplayInstance());
    putRecord(b, airplayType(), 12, false, 4500, ptr);
    std::vector<uint8_t> svc; putName(svc, airplayType());
    putRecord(b, "_services._dns-sd._udp.local", 12, false, 4500, svc);
    std::vector<uint8_t> srv; put16(srv, 0); put16(srv, 0); put16(srv, airplayPort_.load()); putName(srv, hostName());
    putRecord(b, airplayInstance(), 33, true, 120, srv);
    putRecord(b, airplayInstance(), 16, true, 4500, txtAirPlay());
    std::vector<uint8_t> a(4); memcpy(a.data(), &lan_.ip.s_addr, 4);
    putRecord(b, hostName(), 1, true, 120, a);
    // NSEC (RFC 6762 6.1): the host has an A record only, so senders need not wait for AAAA.
    // (Advertising the IPv6 link-local address made the iPhone connect over IPv6, and the mirror's
    // audio stayed silent; it did not connect any faster either.)
    std::vector<uint8_t> nsec; putName(nsec, hostName());
    nsec.insert(nsec.end(), {0x00, 0x01, 0x40});  // window 0, 1 byte: type 1 (A)
    putRecord(b, hostName(), 47, true, 120, nsec);
    if (raop) {
      std::vector<uint8_t> rptr; putName(rptr, raopInstance());
      putRecord(b, raopType(), 12, false, 4500, rptr);
      std::vector<uint8_t> rsvc; putName(rsvc, raopType());
      putRecord(b, "_services._dns-sd._udp.local", 12, false, 4500, rsvc);
      std::vector<uint8_t> rsrv; put16(rsrv, 0); put16(rsrv, 0); put16(rsrv, airplayPort_.load()); putName(rsrv, hostName());
      putRecord(b, raopInstance(), 33, true, 120, rsrv);
      std::vector<uint8_t> rtxt;
      for (auto& kv : raopKv) {
        rtxt.push_back(static_cast<uint8_t>(kv.size()));
        rtxt.insert(rtxt.end(), kv.begin(), kv.end());
      }
      putRecord(b, raopInstance(), 16, true, 4500, rtxt);
    }
    return b;
  }

  // True if any question is about one of our names.
  bool mdnsAsksForUs(const uint8_t* p, size_t len, bool& unicast, bool& airplay) {
    if (len < 12) return false;
    uint16_t flags = (p[2] << 8) | p[3];
    if (flags & 0x8000) return false;  // a response, not a query
    uint16_t qd = (p[4] << 8) | p[5];
    size_t off = 12;
    bool match = false;
    std::string ours[] = {lower(airplayType()), lower(airplayInstance()), lower(hostName()),
                          "_services._dns-sd._udp.local", lower(raopType()), lower(raopInstance())};
    for (int i = 0; i < qd; i++) {
      std::string n;
      if (!readName(p, len, off, n) || off + 4 > len) return match;
      uint16_t qtype = (p[off] << 8) | p[off + 1];
      uint16_t qclass = (p[off + 2] << 8) | p[off + 3];
      {
        std::string ln = lower(n);
        if (ln.find("airplay") != std::string::npos || ln.find("raop") != std::string::npos ||
            ln.find("screenatic") != std::string::npos || ln == lower(hostName()))
          lastQuestions_ += (lastQuestions_.empty() ? "" : ", ") + n + " t" + std::to_string(qtype) +
                            ((qclass & 0x8000) ? " QU" : "");
      }
      off += 4;
      n = lower(n);
      if (n == lower(airplayType())) airplay = true;
      for (auto& o : ours) if (n == o) { match = true; if (qclass & 0x8000) unicast = true; }
    }
    return match;
  }

  void mdnsLoop() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(5353);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      setMdns(std::string("bind 5353 failed: ") + strerror(errno));
      close(s);
      return;
    }
    ip_mreq mreq{};
    inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
    mreq.imr_interface = lan_.ip;
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
      log(std::string("mDNS join failed: ") + strerror(errno));
    }
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &lan_.ip, sizeof lan_.ip);
    uint8_t ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);

    auto announce = [&](const sockaddr_in& to) {
      auto pkt = mdnsAnnouncement();
      ssize_t n = sendto(s, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
      if (n < 0) log(std::string("mDNS send failed: ") + strerror(errno));
      return n > 0;
    };
    setMdns("bound 5353, announcing " + airplayInstance());
    log("mDNS bound :5353 (shared with system responder)");
    int64_t nextAnnounce = 0;
    int burst = 0;
    std::vector<uint8_t> buf(9000);
    while (running_) {
      int64_t now = nowMs();
      if (reannounce_.exchange(false)) {
        burst = 0;
        nextAnnounce = 0;
      }
      if (now >= nextAnnounce) {
        announce(group);
        nextAnnounce = now + (burst++ < 3 ? 1000 : 60000);
      }
      if (!waitReadable(s, 500)) continue;
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t n = recvfrom(s, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;
      bool unicast = false, airplay = false;
      lastQuestions_.clear();
      bool ours = mdnsAsksForUs(buf.data(), n, unicast, airplay);
      if (!lastQuestions_.empty() && from.sin_addr.s_addr != lan_.ip.s_addr)
        Timeline::add("mDNS query from " + ipStr(from.sin_addr) + ":" + std::to_string(ntohs(from.sin_port)) +
                      " " + lastQuestions_ + (ours ? " -> answered" : " -> not ours"));
      {
        std::lock_guard<std::mutex> g(mu_);
        if (!(buf[2] & 0x80)) mdnsQueries_++;
        if (airplay) airplayQueries_++;
      }
      if (!ours) continue;
      if (airplay) addPeer(ipStr(from.sin_addr) + " -> mDNS airplay query");
      bool sent = announce(group);
      if (unicast || ntohs(from.sin_port) != 5353) sent = announce(from) || sent;
      if (sent) {
        std::lock_guard<std::mutex> g(mu_);
        mdnsAnswered_++;
      }
    }
    close(s);
  }

  void setMdns(const std::string& v) { std::lock_guard<std::mutex> g(mu_); mdnsState_ = v; }
  void setSsdp(const std::string& v) { std::lock_guard<std::mutex> g(mu_); ssdpState_ = v; }

  // ---- SSDP: advertise the DLNA MediaRenderer (Windows "Cast to Device", Android cast apps) ----

  std::string location() { return "http://" + ipStr(lan_.ip) + ":" + std::to_string(kDlnaPort) + "/description.xml"; }

  void ssdpLoop() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(1900);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      setSsdp(std::string("bind 1900 failed: ") + strerror(errno));
      close(s);
      return;
    }
    ip_mreq mreq{};
    inet_pton(AF_INET, "239.255.255.250", &mreq.imr_multiaddr);
    mreq.imr_interface = lan_.ip;
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
      log(std::string("SSDP join failed: ") + strerror(errno));
    }
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &lan_.ip, sizeof lan_.ip);
    uint8_t ttl = 4;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);

    const std::string server = "Vega/1.0 UPnP/1.0 Screenatic/1.0";
    std::vector<std::string> nts = {"upnp:rootdevice", "uuid:" + uuid(), DlnaRenderer::deviceType()};
    for (auto& st : DlnaRenderer::serviceTypes()) nts.push_back(st);
    auto usn = [&](const std::string& nt) { return nt.rfind("uuid:", 0) == 0 ? nt : "uuid:" + uuid() + "::" + nt; };
    auto sendTo = [&](const std::string& msg, const sockaddr_in& to) {
      sendto(s, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    };
    setSsdp("bound 1900, location " + location());
    log("SSDP bound :1900");
    int64_t nextNotify = 0;
    int burst = 0;
    std::vector<uint8_t> buf(4096);
    while (running_) {
      int64_t now = nowMs();
      // Burst at start (UDP is lossy), then every 30 s (well inside max-age 1800).
      if (now >= nextNotify && lan_.ok) {
        for (auto& nt : nts) {
          sendTo("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n"
                 "LOCATION: " + location() + "\r\nNT: " + nt + "\r\nNTS: ssdp:alive\r\n"
                 "SERVER: " + server + "\r\nUSN: " + usn(nt) + "\r\n\r\n", group);
        }
        nextNotify = now + (burst++ < 3 ? 1000 : 30000);
      }
      if (!waitReadable(s, 500)) continue;
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      ssize_t n = recvfrom(s, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
      if (n <= 0) continue;
      std::string msg(reinterpret_cast<char*>(buf.data()), n);
      if (lower(msg.substr(0, 8)) != "m-search") continue;
      {
        std::lock_guard<std::mutex> g(mu_);
        ssdpSearches_++;
      }
      std::string lm = lower(msg);
      if (lm.find("ssdp:discover") == std::string::npos || !lan_.ok) continue;
      size_t stPos = lm.find("\nst:");
      if (stPos == std::string::npos) continue;
      size_t stEnd = msg.find("\r\n", stPos + 1);
      std::string st = msg.substr(stPos + 4, stEnd - stPos - 4);
      while (!st.empty() && st.front() == ' ') st.erase(0, 1);
      std::vector<std::string> targets;
      if (st == "ssdp:all") targets = nts;
      else for (auto& nt : nts) if (nt == st) targets.push_back(nt);
      if (targets.empty()) continue;
      addPeer(ipStr(from.sin_addr) + " -> SSDP search " + st);
      for (auto& t : targets) {
        sendTo("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: " + location() +
               "\r\nSERVER: " + server + "\r\nST: " + t + "\r\nUSN: " + usn(t) + "\r\n\r\n", from);
      }
      std::lock_guard<std::mutex> g(mu_);
      ssdpAnswered_++;
    }
    // Leaving: controllers drop us from their lists right away instead of after max-age.
    for (int i = 0; i < 2; ++i)
      for (auto& nt : nts)
        sendTo("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: " + nt + "\r\nNTS: ssdp:byebye\r\nUSN: " +
               usn(nt) + "\r\n\r\n", group);
    close(s);
  }

  std::mutex startMu_;
  std::mutex mu_;
  std::atomic<bool> running_{false};
  std::vector<std::thread> threads_;
  int64_t t0_ = nowMs();
  std::string name_ = "Screenatic TV";
  bool debug_ = false;
  std::string deviceId_;
  std::string lastQuestions_;  // mdnsLoop thread only: questions of the packet being read
  std::atomic<int> airplayPort_{kAirPlayPort};
  std::vector<std::string> airplayTxt_;  // empty: the probe's placeholder record
  std::vector<std::string> raopTxt_;     // empty until UxPlay runs
  std::map<std::string, std::string> txtOverrides_;  // debug: GET /mdns
  std::atomic<bool> reannounce_{false};
  std::function<std::string()> airplayStatus_;
  std::map<std::string, std::string> jsReports_;
  std::function<void(const std::string&)> debugPlay_;
  std::function<std::string(const std::string&)> debugMsse_;
  std::function<void(const uint8_t*, size_t, int64_t, bool)> debugVideo_;
  std::function<std::string(const std::string&)> dump_;
  std::shared_ptr<DlnaRenderer> dlna_ = std::make_shared<DlnaRenderer>();
  Lan lan_;
  std::map<std::string, std::string> portState_;
  std::vector<std::string> peers_;
  std::deque<std::string> log_;
  uint64_t tcpAccepted_ = 0;
  std::string mdnsState_ = "not started", ssdpState_ = "not started";
  uint64_t mdnsQueries_ = 0, mdnsAnswered_ = 0, airplayQueries_ = 0;
  uint64_t ssdpSearches_ = 0, ssdpAnswered_ = 0;
  std::deque<uint8_t> video_;
  std::deque<uint8_t> audio_;
  std::atomic<bool> audioOff_{false};
  uint64_t audioReceived_ = 0, audioDropped_ = 0;
  uint64_t videoReceived_ = 0, videoDropped_ = 0;
};

/** DLNA events for JS: into AirPlayTest's queue (the one pollEvents() drains) once it exists,
 *  held here until then. DLNA threads may push at any time, so the pointer is guarded. */
struct DlnaEvents {
  std::mutex mu;
  AirPlayTest* airplay = nullptr;
  std::vector<std::string> pending;

  void push(const std::string& json) {
    std::lock_guard<std::mutex> g(mu);
    if (airplay) airplay->pushEvent(json);
    else if (pending.size() < 64) pending.push_back(json);
  }
  void attach(AirPlayTest* a) {
    std::lock_guard<std::mutex> g(mu);
    airplay = a;
    if (a) for (auto& e : pending) a->pushEvent(std::move(e));
    pending.clear();
  }
  std::string drain() {
    std::lock_guard<std::mutex> g(mu);
    std::string out = "[";
    for (size_t i = 0; i < pending.size(); ++i) out += (i ? "," : "") + pending[i];
    pending.clear();
    return out + "]";
  }
};

ProbeNative::ProbeNative() : probe_(std::make_shared<Probe>()), dlnaEvents_(std::make_shared<DlnaEvents>()) {
  std::weak_ptr<Probe> weak = probe_;
  std::shared_ptr<DlnaEvents> events = dlnaEvents_;
  probe_->dlna()->setCallbacks(
      [weak](const std::string& m) {
        Timeline::add("dlna: " + m);
        if (auto p = weak.lock()) p->note("dlna: " + m);
      },
      [events](const std::string& json) { events->push(json); });
}

ProbeNative::~ProbeNative() noexcept {
  probe_->stop();
  dlnaEvents_->attach(nullptr);  // airplay_ goes away before the renderer's last threads
}

std::string ProbeNative::start(std::string deviceName) { return probe_->start(deviceName, true); }

std::string ProbeNative::startReceiver(std::string deviceName, bool debug) {
  probe_->start(deviceName, debug);
  return airplayStart(probe_->name());
}
std::string ProbeNative::status() { return probe_->status(); }

std::string ProbeNative::airplayStart(std::string deviceName) {
  if (!airplay_) {
    std::weak_ptr<Probe> weak = probe_;
    airplay_ = std::make_unique<AirPlayTest>(
        [weak](const std::string& m) { if (auto p = weak.lock()) p->note("airplay: " + m); },
        [weak](int port, const std::map<std::string, std::string>& txt,
               const std::map<std::string, std::string>& raopTxt) {
          if (auto p = weak.lock()) p->setAirPlay(port, txt, raopTxt);
        },
        [weak](std::vector<uint8_t>&& bytes) {
          if (auto p = weak.lock()) p->pushMedia(std::move(bytes));
        },
        [weak](std::vector<uint8_t>&& bytes) {
          if (auto p = weak.lock()) p->pushAudio(std::move(bytes));
        });
    AirPlayTest* a = airplay_.get();
    probe_->setAirPlayStatus([a] { return a->statusJson(); });
    probe_->setDebugPlay([a](const std::string& url) { a->debugPlay(url); });
    probe_->setDebugMsse([a](const std::string& q) { return a->msseCommand(q); });
    probe_->setDebugVideo([a](const uint8_t* p, size_t n, int64_t pts, bool first) { a->debugVideo(p, n, pts, first); });
    probe_->setDump([a](const std::string& kind) { return a->dump(kind); });
    dlnaEvents_->attach(a);
  }
  return airplay_->start(deviceName, probe_->deviceId(), probe_->lanIp(), probe_->debug());
}
void ProbeNative::stop() { probe_->stop(); }

void ProbeNative::report(std::string section, std::string json) { probe_->report(section, json); }

std::string ProbeNative::pollEvents() { return airplay_ ? airplay_->pollEvents() : dlnaEvents_->drain(); }

std::string ProbeNative::msseAttach(std::string windowHandle) {
  if (!airplay_) return "airplay not started";
  // "cmd:<query>" controls the player instead (e.g. cmd:suspend=1 while URL playback runs).
  if (windowHandle.rfind("cmd:", 0) == 0) return airplay_->msseCommand(windowHandle.substr(4));
  return airplay_->msseAttach(windowHandle);
}

void ProbeNative::setPlayback(std::string csv) {
  // "duration,position,rate,ready,buffering,ended,failed,source,closed" (seconds; flags 0/1;
  // source "airplay" or "dlna"). The old 6-field form is AirPlay.
  std::vector<std::string> f;
  size_t i = 0;
  while (true) {
    size_t c = csv.find(',', i);
    f.push_back(csv.substr(i, c == std::string::npos ? std::string::npos : c - i));
    if (c == std::string::npos) break;
    i = c + 1;
  }
  auto num = [&](size_t k) { return k < f.size() ? strtod(f[k].c_str(), nullptr) : 0.0; };
  std::string source = f.size() > 7 ? f[7] : "airplay";
  while (!source.empty() && isspace(static_cast<unsigned char>(source.back()))) source.pop_back();
  while (!source.empty() && isspace(static_cast<unsigned char>(source.front()))) source.erase(0, 1);
  if (source == "dlna") {
    probe_->dlna()->reportPlayback(num(0), num(1), num(2), num(3) != 0, num(4) != 0, num(5) != 0, num(6) != 0,
                                   num(8) != 0);
  } else if (airplay_) {
    airplay_->setPlayback(num(0), num(1), num(2), num(3) != 0, num(4) != 0, num(5) != 0);
  }
}

ArrayBuffer ProbeNative::readAudio(double maxBytes) {
  auto bytes = probe_->readAudio(static_cast<size_t>(maxBytes > 0 ? maxBytes : 0));
  return ArrayBuffer(std::make_shared<std::vector<uint8_t>>(std::move(bytes)));
}

ArrayBuffer ProbeNative::readVideo(double maxBytes) {
  auto bytes = probe_->readVideo(static_cast<size_t>(maxBytes > 0 ? maxBytes : 0));
  return ArrayBuffer(std::make_shared<std::vector<uint8_t>>(std::move(bytes)));
}

}  // namespace ProbeNativeTurboModule
