#pragma once

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace ProbeNativeTurboModule {

/**
 * Debug: millisecond timeline of the connection steps (RTSP requests, first frame, player start),
 * served in GET /status as "timeline" to see where a mirror connection spends its time.
 */
class Timeline {
public:
  static void add(const std::string& what) {
    // Noise: every HTTP request is also logged as "request ..."; SPS dumps; 2 s heartbeats.
    if (what.compare(0, 22, "httpd request received") == 0 || what.find("/feedback") != std::string::npos ||
        what.find("/playback-info") != std::string::npos || what.compare(0, 15, "raop_rtp_mirror") == 0 ||
        what.compare(0, 9, "remainder") == 0)
      return;
    auto& t = get();
    const double s = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0;
    char ts[24];
    snprintf(ts, sizeof ts, "%.3f ", s);
    std::string line = ts + what.substr(0, 220);
    for (auto& c : line)
      if (c == '\n' || c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) c = ' ';
    std::lock_guard<std::mutex> g(t.mu);
    t.lines.push_back(std::move(line));
    while (t.lines.size() > 400) t.lines.pop_front();
  }
  /** JSON array of "seconds what" strings, oldest first. */
  static std::string json() {
    auto& t = get();
    std::lock_guard<std::mutex> g(t.mu);
    std::string o = "[";
    for (auto& l : t.lines) o += (o.size() > 1 ? ",\"" : "\"") + l + "\"";
    return o + "]";
  }

private:
  std::mutex mu;
  std::deque<std::string> lines;
  static Timeline& get() {
    static Timeline t;
    return t;
  }
};

}  // namespace ProbeNativeTurboModule
