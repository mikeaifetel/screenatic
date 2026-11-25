// DLNA MediaRenderer for Vega: port of protocol-dlna (Android). See DlnaRenderer.h.

#include "DlnaRenderer.h"
#include "Timeline.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

namespace ProbeNativeTurboModule {

namespace {

const char* const kDeviceType = "urn:schemas-upnp-org:device:MediaRenderer:1";
const char* const kAvt = "urn:schemas-upnp-org:service:AVTransport:1";
const char* const kRcs = "urn:schemas-upnp-org:service:RenderingControl:1";
const char* const kCms = "urn:schemas-upnp-org:service:ConnectionManager:1";
const char* const kServerHeader = "Vega/1.0 UPnP/1.0 Screenatic/1.0";
const char* const kVolumeFile = "/data/dlna_volume";

constexpr int64_t kCommandSettleMs = 1500;
constexpr int kIdleAfterEndMs = 8000;
// Windows "Cast to Device" ends a cast with Pause and then goes silent (no Stop, no UNSUBSCRIBE):
// paused or stopped with no request from any controller for this long, leave the screen.
constexpr int64_t kControllerGoneMs = 20000;
constexpr size_t kMaxBody = 1 << 20;
constexpr int kNotifyTimeoutMs = 2000;

/** Path segment per service: /AVTransport/control, /AVTransport/event, /AVTransport/scpd.xml */
const std::vector<std::pair<std::string, std::string>>& services() {
  static const std::vector<std::pair<std::string, std::string>> s = {
      {kAvt, "AVTransport"}, {kRcs, "RenderingControl"}, {kCms, "ConnectionManager"}};
  return s;
}

std::string serviceForPath(const std::string& seg) {
  for (auto& [type, path] : services()) if (path == seg) return type;
  return "";
}

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string jsonEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

/** A JSON number: integers without a fraction, others with milliseconds' precision. */
std::string jsonNum(double v) {
  char b[32];
  if (std::floor(v) == v && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
  else snprintf(b, sizeof b, "%.3f", v);
  return b;
}

// ---------------------------------------------------------------- XML

std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&apos;"; break;
      default: o += c;
    }
  }
  return o;
}

void putUtf8(std::string& o, unsigned long cp) {
  if (cp < 0x80) o += static_cast<char>(cp);
  else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    o += static_cast<char>(0xE0 | (cp >> 12));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    o += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    o += static_cast<char>(0xF0 | (cp >> 18));
    o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    o += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string unescape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') { o += s[i]; continue; }
    size_t semi = s.find(';', i);
    if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
    std::string ent = s.substr(i + 1, semi - i - 1);
    if (ent == "amp") o += '&';
    else if (ent == "lt") o += '<';
    else if (ent == "gt") o += '>';
    else if (ent == "quot") o += '"';
    else if (ent == "apos") o += '\'';
    else if (!ent.empty() && ent[0] == '#') {
      bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
      unsigned long cp = strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
      putUtf8(o, cp);
    } else { o += s[i]; continue; }
    i = semi;
  }
  return o;
}

struct XmlTag {
  std::string qname, local, attrs;
  bool closing = false, selfClosing = false;
  size_t begin = 0, end = 0;  // end: just past '>'
};

// Next tag at or after pos; skips comments, CDATA sections, processing instructions and doctype.
bool nextTag(const std::string& x, size_t& pos, XmlTag& t) {
  while (true) {
    size_t lt = x.find('<', pos);
    if (lt == std::string::npos || lt + 1 >= x.size()) return false;
    if (x.compare(lt, 4, "<!--") == 0) {
      size_t e = x.find("-->", lt + 4);
      if (e == std::string::npos) return false;
      pos = e + 3;
      continue;
    }
    if (x.compare(lt, 9, "<![CDATA[") == 0) {
      size_t e = x.find("]]>", lt + 9);
      if (e == std::string::npos) return false;
      pos = e + 3;
      continue;
    }
    if (x[lt + 1] == '?' || x[lt + 1] == '!') {
      size_t e = x.find('>', lt);
      if (e == std::string::npos) return false;
      pos = e + 1;
      continue;
    }
    size_t gt = lt + 1;
    char quote = 0;
    for (; gt < x.size(); ++gt) {
      char c = x[gt];
      if (quote) { if (c == quote) quote = 0; }
      else if (c == '"' || c == '\'') quote = c;
      else if (c == '>') break;
    }
    if (gt >= x.size()) return false;
    std::string inner = x.substr(lt + 1, gt - lt - 1);
    t = XmlTag();
    t.begin = lt;
    t.end = gt + 1;
    if (!inner.empty() && inner[0] == '/') { t.closing = true; inner.erase(0, 1); }
    if (!inner.empty() && inner.back() == '/') { t.selfClosing = true; inner.pop_back(); }
    size_t sp = inner.find_first_of(" \t\r\n");
    t.qname = trim(inner.substr(0, sp));
    if (sp != std::string::npos) t.attrs = inner.substr(sp);
    size_t colon = t.qname.find(':');
    t.local = colon == std::string::npos ? t.qname : t.qname.substr(colon + 1);
    pos = t.end;
    return true;
  }
}

// Content of the element opened by `start`: its decoded text (CDATA kept as is), or the raw inner
// markup if it has child elements. after: just past its end tag.
std::string elementText(const std::string& x, const XmlTag& start, size_t* after = nullptr) {
  if (start.selfClosing) {
    if (after) *after = start.end;
    return "";
  }
  size_t pos = start.end;
  int depth = 1;
  bool children = false;
  XmlTag t;
  size_t innerEnd = x.size();
  while (nextTag(x, pos, t)) {
    if (t.closing) {
      if (--depth == 0) { innerEnd = t.begin; break; }
    } else if (!t.selfClosing) {
      ++depth;
      children = true;
    } else {
      children = true;
    }
  }
  if (after) *after = depth == 0 ? pos : x.size();
  std::string inner = x.substr(start.end, innerEnd - start.end);
  if (children) return trim(inner);
  std::string out;
  size_t i = 0;
  while (i < inner.size()) {
    size_t c = inner.find("<![CDATA[", i);
    if (c == std::string::npos) { out += unescape(inner.substr(i)); break; }
    out += unescape(inner.substr(i, c - i));
    size_t e = inner.find("]]>", c + 9);
    if (e == std::string::npos) { out += inner.substr(c + 9); break; }
    out += inner.substr(c + 9, e - c - 9);
    i = e + 3;
  }
  return out;
}

std::string attr(const std::string& attrs, const std::string& name) {
  size_t p = 0;
  while ((p = attrs.find(name, p)) != std::string::npos) {
    bool startOk = p == 0 || isspace(static_cast<unsigned char>(attrs[p - 1]));
    size_t q = p + name.size();
    while (q < attrs.size() && isspace(static_cast<unsigned char>(attrs[q]))) ++q;
    if (startOk && q < attrs.size() && attrs[q] == '=') {
      ++q;
      while (q < attrs.size() && isspace(static_cast<unsigned char>(attrs[q]))) ++q;
      if (q < attrs.size() && (attrs[q] == '"' || attrs[q] == '\'')) {
        size_t e = attrs.find(attrs[q], q + 1);
        if (e != std::string::npos) return unescape(attrs.substr(q + 1, e - q - 1));
      }
    }
    p += name.size();
  }
  return "";
}

/** A SOAP request: action name and its (flat) arguments. */
struct SoapCall {
  std::string action;
  std::map<std::string, std::string> args;
};

bool parseSoap(const std::string& body, SoapCall& call) {
  size_t pos = 0;
  XmlTag t;
  bool inBody = false;
  while (nextTag(body, pos, t)) {
    if (t.closing) continue;
    if (!inBody) {
      if (t.local == "Body") inBody = true;
      continue;
    }
    call.action = t.local;
    if (t.selfClosing) return true;
    XmlTag c;
    while (nextTag(body, pos, c)) {
      if (c.closing) break;  // end of the action element
      call.args[c.local] = elementText(body, c, &pos);
    }
    return true;
  }
  return false;
}

/** UPnP time "H:MM:SS". */
std::string formatTime(int64_t ms) {
  int64_t s = std::max<int64_t>(ms, 0) / 1000;
  char b[32];
  snprintf(b, sizeof b, "%lld:%02d:%02d", static_cast<long long>(s / 3600), static_cast<int>((s / 60) % 60),
           static_cast<int>(s % 60));
  return b;
}

/** "H:MM:SS[.fff]" or "MM:SS" or plain seconds -> ms; false if unparseable. */
bool parseTime(const std::string& in, int64_t& ms) {
  std::string t = trim(in);
  std::vector<std::string> parts;
  size_t i = 0;
  while (true) {
    size_t c = t.find(':', i);
    parts.push_back(t.substr(i, c == std::string::npos ? std::string::npos : c - i));
    if (c == std::string::npos) break;
    i = c + 1;
  }
  if (parts.empty() || parts.size() > 3) return false;
  double sec = 0;
  for (auto& p : parts) {
    if (p.empty()) return false;
    char* end = nullptr;
    double v = strtod(p.c_str(), &end);
    if (!end || *end != '\0') return false;
    sec = sec * 60 + v;
  }
  ms = static_cast<int64_t>(sec * 1000);
  return true;
}

/** What a DIDL-Lite item says about the media. */
struct Didl {
  std::string title, artist, album, artworkUrl;
  std::string mediaType;  // "video" / "audio" / "image", empty if it doesn't say
  int64_t durationMs = 0;
};

Didl parseDidl(const std::string& didl) {
  Didl d;
  if (trim(didl).empty()) return d;
  std::string creator, upnpClass, mime;
  size_t pos = 0;
  XmlTag t;
  while (nextTag(didl, pos, t)) {
    if (t.closing) continue;
    const std::string& n = t.local;
    if (n == "title") d.title = trim(elementText(didl, t));
    else if (n == "artist") { if (d.artist.empty()) d.artist = trim(elementText(didl, t)); }
    else if (n == "creator") creator = trim(elementText(didl, t));
    else if (n == "album") d.album = trim(elementText(didl, t));
    else if (n == "albumArtURI") { if (d.artworkUrl.empty()) d.artworkUrl = trim(elementText(didl, t)); }
    else if (n == "class") upnpClass = trim(elementText(didl, t));
    else if (n == "res") {
      if (mime.empty()) {
        std::string pi = attr(t.attrs, "protocolInfo");
        size_t a = pi.find(':');
        size_t b = a == std::string::npos ? a : pi.find(':', a + 1);
        size_t c = b == std::string::npos ? b : pi.find(':', b + 1);
        if (b != std::string::npos) mime = pi.substr(b + 1, c == std::string::npos ? std::string::npos : c - b - 1);
      }
      if (d.durationMs == 0) {
        int64_t ms = 0;
        std::string du = attr(t.attrs, "duration");
        if (!du.empty() && parseTime(du, ms)) d.durationMs = ms;
      }
    }
  }
  if (d.artist.empty()) d.artist = creator;
  if (contains(upnpClass, "imageItem") || mime.rfind("image/", 0) == 0) d.mediaType = "image";
  else if (contains(upnpClass, "audioItem") || mime.rfind("audio/", 0) == 0) d.mediaType = "audio";
  else if (contains(upnpClass, "videoItem") || mime.rfind("video/", 0) == 0) d.mediaType = "video";
  return d;
}

/** Fallback when the sender gives no metadata: guess from the URL's extension. */
std::string guessMediaType(const std::string& url) {
  std::string u = url.substr(0, url.find('?'));
  size_t slash = u.rfind('/');
  size_t dot = u.rfind('.');
  std::string ext = (dot == std::string::npos || (slash != std::string::npos && dot < slash)) ? "" : lower(u.substr(dot + 1));
  for (const char* e : {"jpg", "jpeg", "png", "gif", "webp", "bmp", "heic"}) if (ext == e) return "image";
  for (const char* e : {"mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma"}) if (ext == e) return "audio";
  return "video";
}

/** What we can play (Vega's player formats + images). Phones filter their "cast to" list with this. */
const std::string& sinkProtocolInfo() {
  static const std::string s = [] {
    const char* types[] = {
        "video/mp4", "video/x-matroska", "video/webm", "video/mpeg", "video/mp2t", "video/quicktime",
        "video/x-msvideo", "video/3gpp", "video/x-flv", "application/vnd.apple.mpegurl", "application/x-mpegURL",
        "application/dash+xml", "audio/mpeg", "audio/mp4", "audio/aac", "audio/x-aac", "audio/flac", "audio/x-flac",
        "audio/wav", "audio/x-wav", "audio/L16", "audio/ogg", "audio/opus", "audio/webm", "audio/x-ms-wma",
        "image/jpeg", "image/png", "image/gif", "image/webp", "image/bmp", "image/heic"};
    std::string o;
    for (const char* t : types) o += (o.empty() ? "" : ",") + std::string("http-get:*:") + t + ":*";
    return o;
  }();
  return s;
}

std::string deviceDescription(const std::string& friendlyName, const std::string& uuid) {
  std::string o;
  o += "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
  o += "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">";
  o += "<specVersion><major>1</major><minor>0</minor></specVersion><device>";
  o += std::string("<deviceType>") + kDeviceType + "</deviceType>";
  o += "<friendlyName>" + esc(friendlyName) + "</friendlyName>";
  o += "<manufacturer>Screenatic</manufacturer>";
  o += "<modelDescription>Screenatic TV receiver</modelDescription><modelName>Screenatic</modelName>";
  o += "<modelNumber>1</modelNumber><UDN>uuid:" + uuid + "</UDN>";
  o += "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><serviceList>";
  for (auto& [type, path] : services()) {
    o += "<service><serviceType>" + type + "</serviceType><serviceId>urn:upnp-org:serviceId:" + path + "</serviceId>";
    o += "<SCPDURL>/" + path + "/scpd.xml</SCPDURL><controlURL>/" + path + "/control</controlURL>";
    o += "<eventSubURL>/" + path + "/event</eventSubURL></service>";
  }
  o += "</serviceList></device></root>";
  return o;
}

using Args = std::vector<std::pair<std::string, std::string>>;

std::string soapResponse(const std::string& serviceType, const std::string& action, const Args& out) {
  std::string o;
  o += "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
  o += "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>";
  o += "<u:" + action + "Response xmlns:u=\"" + serviceType + "\">";
  for (auto& [k, v] : out) o += "<" + k + ">" + esc(v) + "</" + k + ">";
  o += "</u:" + action + "Response></s:Body></s:Envelope>";
  return o;
}

std::string soapFault(int code, const std::string& description) {
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
         "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
         "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
         std::to_string(code) + "</errorCode><errorDescription>" + esc(description) +
         "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
}

/** GENA body carrying one LastChange (itself an escaped XML document). */
std::string lastChangeEvent(const std::string& serviceType, const Args& vars) {
  bool rcs = serviceType == kRcs;
  std::string inner = std::string("<Event xmlns=\"") +
                      (rcs ? "urn:schemas-upnp-org:metadata-1-0/RCS/" : "urn:schemas-upnp-org:metadata-1-0/AVT/") +
                      "\"><InstanceID val=\"0\">";
  for (auto& [k, v] : vars) {
    if (rcs) inner += "<" + k + " channel=\"Master\" val=\"" + esc(v) + "\"/>";
    else inner += "<" + k + " val=\"" + esc(v) + "\"/>";
  }
  inner += "</InstanceID></Event>";
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
         "<e:property><LastChange>" + esc(inner) + "</LastChange></e:property></e:propertyset>";
}

// ---------------------------------------------------------------- SCPD
// Actions are written as "in:Arg=StateVar" / "out:Arg=StateVar" to keep this readable.

struct Var {
  std::string name, type = "string";
  bool events = false;
  std::vector<std::string> allowed;
};

Var v(const std::string& name, const std::string& type = "string", bool events = false,
      std::vector<std::string> allowed = {}) {
  return Var{name, type, events, std::move(allowed)};
}

Var va(const std::string& name, std::vector<std::string> allowed) { return v(name, "string", false, std::move(allowed)); }

std::string buildScpd(const std::vector<std::pair<std::string, std::vector<std::string>>>& actions,
                      const std::vector<Var>& vars) {
  std::string o = "<?xml version=\"1.0\" encoding=\"utf-8\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">";
  o += "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>";
  for (auto& [name, args] : actions) {
    o += "<action><name>" + name + "</name>";
    if (!args.empty()) {
      o += "<argumentList>";
      for (auto& a : args) {
        size_t colon = a.find(':'), eq = a.find('=');
        o += "<argument><name>" + a.substr(colon + 1, eq - colon - 1) + "</name><direction>" + a.substr(0, colon) +
             "</direction><relatedStateVariable>" + a.substr(eq + 1) + "</relatedStateVariable></argument>";
      }
      o += "</argumentList>";
    }
    o += "</action>";
  }
  o += "</actionList><serviceStateTable>";
  for (auto& var : vars) {
    o += std::string("<stateVariable sendEvents=\"") + (var.events ? "yes" : "no") + "\"><name>" + var.name +
         "</name><dataType>" + var.type + "</dataType>";
    if (var.name == "Volume")
      o += "<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange>";
    if (!var.allowed.empty()) {
      o += "<allowedValueList>";
      for (auto& a : var.allowed) o += "<allowedValue>" + a + "</allowedValue>";
      o += "</allowedValueList>";
    }
    o += "</stateVariable>";
  }
  o += "</serviceStateTable></scpd>";
  return o;
}

const std::string kId = "in:InstanceID=A_ARG_TYPE_InstanceID";
const std::string kCh = "in:Channel=A_ARG_TYPE_Channel";

const std::string& scpdAvTransport() {
  static const std::string s = buildScpd(
      {
          {"SetAVTransportURI", {kId, "in:CurrentURI=AVTransportURI", "in:CurrentURIMetaData=AVTransportURIMetaData"}},
          {"SetNextAVTransportURI", {kId, "in:NextURI=NextAVTransportURI", "in:NextURIMetaData=NextAVTransportURIMetaData"}},
          {"GetMediaInfo",
           {kId, "out:NrTracks=NumberOfTracks", "out:MediaDuration=CurrentMediaDuration", "out:CurrentURI=AVTransportURI",
            "out:CurrentURIMetaData=AVTransportURIMetaData", "out:NextURI=NextAVTransportURI",
            "out:NextURIMetaData=NextAVTransportURIMetaData", "out:PlayMedium=PlaybackStorageMedium",
            "out:RecordMedium=RecordStorageMedium", "out:WriteStatus=RecordMediumWriteStatus"}},
          {"GetTransportInfo",
           {kId, "out:CurrentTransportState=TransportState", "out:CurrentTransportStatus=TransportStatus",
            "out:CurrentSpeed=TransportPlaySpeed"}},
          {"GetPositionInfo",
           {kId, "out:Track=CurrentTrack", "out:TrackDuration=CurrentTrackDuration",
            "out:TrackMetaData=CurrentTrackMetaData", "out:TrackURI=CurrentTrackURI", "out:RelTime=RelativeTimePosition",
            "out:AbsTime=AbsoluteTimePosition", "out:RelCount=RelativeCounterPosition",
            "out:AbsCount=AbsoluteCounterPosition"}},
          {"GetDeviceCapabilities",
           {kId, "out:PlayMedia=PossiblePlaybackStorageMedia", "out:RecMedia=PossibleRecordStorageMedia",
            "out:RecQualityModes=PossibleRecordQualityModes"}},
          {"GetTransportSettings", {kId, "out:PlayMode=CurrentPlayMode", "out:RecQualityMode=CurrentRecordQualityMode"}},
          {"Stop", {kId}},
          {"Play", {kId, "in:Speed=TransportPlaySpeed"}},
          {"Pause", {kId}},
          {"Seek", {kId, "in:Unit=A_ARG_TYPE_SeekMode", "in:Target=A_ARG_TYPE_SeekTarget"}},
          {"Next", {kId}},
          {"Previous", {kId}},
          {"GetCurrentTransportActions", {kId, "out:Actions=CurrentTransportActions"}},
      },
      {
          va("TransportState", {"STOPPED", "PLAYING", "PAUSED_PLAYBACK", "TRANSITIONING", "NO_MEDIA_PRESENT"}),
          va("TransportStatus", {"OK", "ERROR_OCCURRED"}),
          va("PlaybackStorageMedium", {"NONE", "NETWORK"}),
          va("RecordStorageMedium", {"NOT_IMPLEMENTED"}),
          v("PossiblePlaybackStorageMedia"), v("PossibleRecordStorageMedia"),
          va("CurrentPlayMode", {"NORMAL"}),
          va("TransportPlaySpeed", {"1"}),
          va("RecordMediumWriteStatus", {"NOT_IMPLEMENTED"}),
          va("CurrentRecordQualityMode", {"NOT_IMPLEMENTED"}),
          v("PossibleRecordQualityModes"),
          v("NumberOfTracks", "ui4"), v("CurrentTrack", "ui4"),
          v("CurrentTrackDuration"), v("CurrentMediaDuration"), v("CurrentTrackMetaData"), v("CurrentTrackURI"),
          v("AVTransportURI"), v("AVTransportURIMetaData"), v("NextAVTransportURI"), v("NextAVTransportURIMetaData"),
          v("RelativeTimePosition"), v("AbsoluteTimePosition"),
          v("RelativeCounterPosition", "i4"), v("AbsoluteCounterPosition", "i4"),
          v("CurrentTransportActions"),
          v("LastChange", "string", true),
          va("A_ARG_TYPE_SeekMode", {"REL_TIME", "ABS_TIME", "TRACK_NR"}),
          v("A_ARG_TYPE_SeekTarget"),
          v("A_ARG_TYPE_InstanceID", "ui4"),
      });
  return s;
}

const std::string& scpdRenderingControl() {
  static const std::string s = buildScpd(
      {
          {"ListPresets", {kId, "out:CurrentPresetNameList=PresetNameList"}},
          {"SelectPreset", {kId, "in:PresetName=A_ARG_TYPE_PresetName"}},
          {"GetMute", {kId, kCh, "out:CurrentMute=Mute"}},
          {"SetMute", {kId, kCh, "in:DesiredMute=Mute"}},
          {"GetVolume", {kId, kCh, "out:CurrentVolume=Volume"}},
          {"SetVolume", {kId, kCh, "in:DesiredVolume=Volume"}},
      },
      {
          v("LastChange", "string", true),
          v("PresetNameList"),
          v("Mute", "boolean"),
          v("Volume", "ui2"),
          va("A_ARG_TYPE_Channel", {"Master"}),
          v("A_ARG_TYPE_InstanceID", "ui4"),
          va("A_ARG_TYPE_PresetName", {"FactoryDefaults"}),
      });
  return s;
}

const std::string& scpdConnectionManager() {
  static const std::string s = buildScpd(
      {
          {"GetProtocolInfo", {"out:Source=SourceProtocolInfo", "out:Sink=SinkProtocolInfo"}},
          {"GetCurrentConnectionIDs", {"out:ConnectionIDs=CurrentConnectionIDs"}},
          {"GetCurrentConnectionInfo",
           {"in:ConnectionID=A_ARG_TYPE_ConnectionID", "out:RcsID=A_ARG_TYPE_RcsID",
            "out:AVTransportID=A_ARG_TYPE_AVTransportID", "out:ProtocolInfo=A_ARG_TYPE_ProtocolInfo",
            "out:PeerConnectionManager=A_ARG_TYPE_ConnectionManager", "out:PeerConnectionID=A_ARG_TYPE_ConnectionID",
            "out:Direction=A_ARG_TYPE_Direction", "out:Status=A_ARG_TYPE_ConnectionStatus"}},
      },
      {
          v("SourceProtocolInfo", "string", true), v("SinkProtocolInfo", "string", true),
          v("CurrentConnectionIDs", "string", true),
          va("A_ARG_TYPE_ConnectionStatus", {"OK", "ContentFormatMismatch", "InsufficientBandwidth", "UnreliableChannel", "Unknown"}),
          v("A_ARG_TYPE_ConnectionManager"),
          va("A_ARG_TYPE_Direction", {"Input", "Output"}),
          v("A_ARG_TYPE_ProtocolInfo"),
          v("A_ARG_TYPE_ConnectionID", "i4"), v("A_ARG_TYPE_AVTransportID", "i4"), v("A_ARG_TYPE_RcsID", "i4"),
      });
  return s;
}

const std::string* scpdForPath(const std::string& seg) {
  if (seg == "AVTransport") return &scpdAvTransport();
  if (seg == "RenderingControl") return &scpdRenderingControl();
  if (seg == "ConnectionManager") return &scpdConnectionManager();
  return nullptr;
}

// ---------------------------------------------------------------- HTTP

struct HttpRequest {
  std::string method, path, body, remote;
  std::map<std::string, std::string> headers;  // lower-cased names
  std::string header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? "" : it->second;
  }
  bool has(const std::string& name) const { return headers.count(name) != 0; }
};

struct HttpResponse {
  int status = 200;
  std::string body, contentType;
  std::vector<std::pair<std::string, std::string>> headers;
  static HttpResponse xml(const std::string& text, int status = 200) {
    HttpResponse r;
    r.status = status;
    r.body = text;
    r.contentType = "text/xml; charset=\"utf-8\"";
    return r;
  }
  static HttpResponse code(int status) {
    HttpResponse r;
    r.status = status;
    return r;
  }
};

const char* reason(int code) {
  switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 412: return "Precondition Failed";
    case 500: return "Internal Server Error";
    default: return "Status";
  }
}

bool waitFd(int fd, short events, int timeoutMs) {
  pollfd p{fd, events, 0};
  while (true) {
    int r = poll(&p, 1, timeoutMs);
    if (r > 0) return true;
    if (r < 0 && errno == EINTR) continue;
    return false;
  }
}

bool sendAll(int fd, const std::string& data) {
  for (size_t off = 0; off < data.size();) {
    ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

/** Buffered reads off one connection, each waiting at most timeoutMs. */
struct Reader {
  int fd;
  int timeoutMs;
  std::string buf;
  bool fill() {
    if (!waitFd(fd, POLLIN, timeoutMs)) return false;
    char tmp[4096];
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, static_cast<size_t>(n));
    return true;
  }
  bool line(std::string& out) {
    while (true) {
      size_t nl = buf.find('\n');
      if (nl != std::string::npos) {
        out = buf.substr(0, nl);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        buf.erase(0, nl + 1);
        return true;
      }
      if (buf.size() > 8192 || !fill()) return false;
    }
  }
  bool bytes(size_t n, std::string& out) {
    while (buf.size() < n) if (!fill()) break;
    size_t k = std::min(n, buf.size());
    out.append(buf, 0, k);
    buf.erase(0, k);
    return k == n;
  }
};

bool readRequest(int fd, HttpRequest& req) {
  Reader r{fd, 10000, {}};
  std::string line;
  if (!r.line(line)) return false;
  std::istringstream rl(line);
  std::string target;
  rl >> req.method >> target;
  if (req.method.empty() || target.empty()) return false;
  for (auto& c : req.method) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  req.path = target.substr(0, target.find('?'));
  // Absolute form (http://host:port/path): keep the path.
  if (req.path.rfind("http://", 0) == 0) {
    size_t slash = req.path.find('/', 7);
    req.path = slash == std::string::npos ? "/" : req.path.substr(slash);
  }
  for (int i = 0; i < 100; ++i) {
    if (!r.line(line)) return false;
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon != std::string::npos && colon > 0) req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  if (req.has("content-length")) {
    long len = strtol(req.header("content-length").c_str(), nullptr, 10);
    if (len > 0) r.bytes(std::min(static_cast<size_t>(len), kMaxBody), req.body);
  } else if (contains(lower(req.header("transfer-encoding")), "chunked")) {
    while (req.body.size() < kMaxBody) {
      if (!r.line(line)) break;
      unsigned long size = strtoul(line.substr(0, line.find(';')).c_str(), nullptr, 16);
      if (size == 0) break;
      if (!r.bytes(size, req.body)) break;
      r.line(line);  // CRLF after the chunk
    }
  }
  return true;
}

void writeResponse(int fd, const HttpResponse& res, bool includeBody) {
  std::string h = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n";
  h += std::string("SERVER: ") + kServerHeader + "\r\n";
  h += "Connection: close\r\n";
  h += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
  if (!res.contentType.empty()) h += "Content-Type: " + res.contentType + "\r\n";
  for (auto& [k, val] : res.headers) h += k + ": " + val + "\r\n";
  h += "\r\n";
  if (includeBody) h += res.body;
  sendAll(fd, h);
}

std::string httpDate() {
  time_t t = time(nullptr);
  tm g{};
  gmtime_r(&t, &g);
  char b[64];
  strftime(b, sizeof b, "%a, %d %b %Y %H:%M:%S GMT", &g);
  return b;
}

std::string randomUuid() {
  static std::mutex mu;
  static std::mt19937_64 rng(std::random_device{}() ^ static_cast<uint64_t>(nowMs()));
  uint64_t a, b;
  {
    std::lock_guard<std::mutex> g(mu);
    a = rng();
    b = rng();
  }
  char s[40];
  snprintf(s, sizeof s, "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF),
           static_cast<unsigned>(a & 0x0FFF), static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3FFF)),
           static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
  return s;
}

struct Url {
  std::string host, path, text;
  int port = 80;
};

bool parseUrl(const std::string& s, Url& u) {
  if (lower(s.substr(0, 7)) != "http://") return false;
  std::string rest = s.substr(7);
  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  u.path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = hostPort.rfind(':');
  if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
    u.host = hostPort.substr(0, colon);
    u.port = atoi(hostPort.c_str() + colon + 1);
    if (u.port <= 0) u.port = 80;
  } else {
    u.host = hostPort;
  }
  u.text = s;
  return !u.host.empty();
}

/** Connects with a timeout (an unreachable controller must not hold the event thread long). */
int connectTo(const Url& u, int timeoutMs) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return -1;
  int fd = socket(res->ai_family, SOCK_STREAM, 0);
  if (fd < 0) { freeaddrinfo(res); return -1; }
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (r != 0) {
    if (errno != EINPROGRESS || !waitFd(fd, POLLOUT, timeoutMs)) { close(fd); return -1; }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) { close(fd); return -1; }
  }
  fcntl(fd, F_SETFL, flags);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

/** NOTIFY isn't a method most HTTP clients allow, so write the request by hand. */
bool sendNotify(const Url& u, const std::string& sid, uint64_t seq, const std::string& body) {
  int fd = connectTo(u, kNotifyTimeoutMs);
  if (fd < 0) return false;
  std::string req = "NOTIFY " + u.path + " HTTP/1.1\r\nHOST: " + u.host + ":" + std::to_string(u.port) +
                    "\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: upnp:event\r\nNTS: upnp:propchange\r\n"
                    "SID: " + sid + "\r\nSEQ: " + std::to_string(seq) + "\r\nCONTENT-LENGTH: " +
                    std::to_string(body.size()) + "\r\nCONNECTION: close\r\n\r\n" + body;
  bool ok = sendAll(fd, req);
  if (ok) {
    char c;
    ok = waitFd(fd, POLLIN, kNotifyTimeoutMs) && recv(fd, &c, 1, 0) > 0;  // the status line; content irrelevant
  }
  close(fd);
  return ok;
}

/** One thread running posted tasks, optionally after a delay. */
class Worker {
public:
  void start(const char* name) {
    std::lock_guard<std::mutex> g(mu_);
    if (running_) return;
    running_ = true;
    name_ = name;
    thread_ = std::thread([this] { run(); });
  }
  void stop() {
    {
      std::lock_guard<std::mutex> g(mu_);
      if (!running_) return;
      running_ = false;
      tasks_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void post(std::function<void()> f, int delayMs = 0) {
    {
      std::lock_guard<std::mutex> g(mu_);
      if (!running_) return;
      tasks_.emplace(nowMs() + delayMs, std::move(f));
    }
    cv_.notify_all();
  }
  ~Worker() { stop(); }

private:
  void run() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_) {
      if (tasks_.empty()) { cv_.wait(lk); continue; }
      auto it = tasks_.begin();
      int64_t wait = it->first - nowMs();
      if (wait > 0) { cv_.wait_for(lk, std::chrono::milliseconds(wait)); continue; }
      auto f = std::move(it->second);
      tasks_.erase(it);
      lk.unlock();
      f();
      lk.lock();
    }
  }
  std::mutex mu_;
  std::condition_variable cv_;
  std::multimap<int64_t, std::function<void()>> tasks_;
  std::thread thread_;
  bool running_ = false;
  std::string name_;
};

struct UpnpError {
  int code;
  std::string message;
};

/** Polled every second by controllers; not worth a log line. */
bool polled(const std::string& action) {
  return action == "GetPositionInfo" || action == "GetTransportInfo" || action == "GetVolume" ||
         action == "GetMute" || action == "GetMediaInfo";
}

}  // namespace

// ---------------------------------------------------------------- renderer

struct DlnaRenderer::Impl : std::enable_shared_from_this<DlnaRenderer::Impl> {
  Log log;
  Emit emit;
  std::atomic<bool> running{false};
  Worker gena;   // NOTIFY delivery
  Worker timer;  // idle after the end

  // ---- transport state (guarded by mu; never held while doing I/O or calling out) ----
  std::mutex mu;
  std::string name = "Screenatic TV", uuid;
  std::string uri, uriMeta, nextUri, nextMeta;  // empty uri: none
  std::string transport = "NO_MEDIA_PRESENT";
  struct Status {
    int64_t durationMs = 0, positionMs = 0;
    bool playing = false, ready = false, buffering = false, ended = false, failed = false;
  };
  bool hasStatus = false;
  Status status;
  std::string mediaType = "video";
  int64_t metaDurationMs = 0;
  std::string controller = "DLNA";
  int volume = 100;
  bool muted = false;
  uint64_t idleGen = 0;
  /** After Play/Pause, player reports from before the command lands are ignored until this time. */
  int64_t commandAt = 0;
  bool wantPlaying = true;
  /** The JS player is showing our URI (Android: EngineState.Connected). */
  bool onScreen = false;
  int64_t lastControlMs = 0;  // last SOAP request from a controller
  // Debug: the last requests of any kind (polled ones too), for GET :7000/status.
  std::mutex recentMu;
  std::deque<std::string> recent;
  void remember(const std::string& line) {
    std::lock_guard<std::mutex> g(recentMu);
    std::string stamp = std::to_string(nowMs() / 100 % 100000 / 10.0).substr(0, 7);
    if (!recent.empty() && recent.back().size() > 8 && recent.back().substr(8) == line) recent.back() = stamp + " " + line;
    else if (recent.size() > 1 && recent[recent.size() - 2].substr(8) == line) {
      // alternating pair (GetPositionInfo / GetTransportInfo): refresh in place
      recent[recent.size() - 2] = stamp + " " + line;
    } else recent.push_back(stamp + " " + line);
    while (recent.size() > 400) recent.pop_front();
  }
  std::string screenUrl;

  // ---- GENA subscriptions (guarded by subMu) ----
  struct Sub {
    std::string service;
    std::vector<Url> callbacks;
    int64_t expiresAt = 0;
    uint64_t seq = 0;
  };
  std::mutex subMu;
  std::map<std::string, Sub> subs;

  void note(const std::string& m) {
    if (log) log(m);
  }

  // ---- events for JS ----

  void emitCmd(int cmd, double value) {
    if (emit) emit("{\"type\":\"cmd\",\"source\":\"dlna\",\"cmd\":" + std::to_string(cmd) + ",\"value\":" + jsonNum(value) + "}");
  }

  void emitVolume(float gain) {
    if (emit) emit("{\"type\":\"volume\",\"source\":\"dlna\",\"gain\":" + jsonNum(gain) + "}");
  }

  /** UPnP 0..100 -> player gain with a perceptual curve (linear slider positions feel too loud at the top). */
  float playerVolumeLocked() const {
    if (muted) return 0.f;
    float v = volume / 100.f;
    return v * v;
  }

  void loadVolume() {
    int v = 100;
    if (FILE* f = fopen(kVolumeFile, "r")) {
      if (fscanf(f, "%d", &v) != 1) v = 100;
      fclose(f);
    }
    if (v < 0 || v > 100) v = 100;
    std::lock_guard<std::mutex> g(mu);
    volume = v;
  }

  void saveVolume(int v) {
    if (FILE* f = fopen(kVolumeFile, "w")) {
      fprintf(f, "%d\n", v);
      fclose(f);
    }
  }

  // ---------------------------------------------------------------- HTTP routing

  HttpResponse route(const HttpRequest& req) {
    std::vector<std::string> seg;
    {
      std::string p = req.path;
      while (!p.empty() && p.front() == '/') p.erase(0, 1);
      while (!p.empty() && p.back() == '/') p.pop_back();
      size_t i = 0;
      while (true) {
        size_t s = p.find('/', i);
        seg.push_back(p.substr(i, s == std::string::npos ? std::string::npos : s - i));
        if (s == std::string::npos) break;
        i = s + 1;
      }
    }
    if (req.path == "/description.xml") {
      std::string n, u;
      {
        std::lock_guard<std::mutex> g(mu);
        n = name;
        u = uuid;
      }
      return HttpResponse::xml(deviceDescription(n, u));
    }
    if (seg.size() == 2 && seg[1] == "scpd.xml") {
      const std::string* s = scpdForPath(seg[0]);
      return s ? HttpResponse::xml(*s) : HttpResponse::code(404);
    }
    if (seg.size() == 2 && seg[1] == "control" && req.method == "POST") return control(seg[0], req);
    if (seg.size() == 2 && seg[1] == "event") {
      std::string type = serviceForPath(seg[0]);
      if (type.empty()) return HttpResponse::code(404);
      return genaHandle(req, type);
    }
    return HttpResponse::code(404);
  }

  HttpResponse control(const std::string& service, const HttpRequest& req) {
    std::string type = serviceForPath(service);
    if (type.empty()) return HttpResponse::code(404);
    SoapCall call;
    if (!parseSoap(req.body, call) || call.action.empty()) return HttpResponse::xml(soapFault(401, "Invalid Action"), 500);
    {
      std::lock_guard<std::mutex> g(mu);
      lastControlMs = nowMs();
    }
    if (!polled(call.action)) {
      std::string a;
      for (auto& [k, val] : call.args)
        if (k != "CurrentURIMetaData" && k != "NextURIMetaData" && k != "InstanceID") a += " " + k + "=" + val;
      note(service + "." + call.action + a);
    }
    try {
      Args out;
      if (type == kAvt) out = avTransport(call, req);
      else if (type == kRcs) out = renderingControl(call);
      else out = connectionManager(call);
      return HttpResponse::xml(soapResponse(type, call.action, out));
    } catch (const UpnpError& e) {
      note(call.action + " -> " + std::to_string(e.code) + " " + e.message);
      return HttpResponse::xml(soapFault(e.code, e.message), 500);
    }
  }

  // ---------------------------------------------------------------- AVTransport

  std::string arg(const SoapCall& c, const std::string& k) {
    auto it = c.args.find(k);
    return it == c.args.end() ? "" : it->second;
  }

  Args avTransport(const SoapCall& call, const HttpRequest& req) {
    const std::string& a = call.action;
    if (a == "SetAVTransportURI") {
      std::string newUri = trim(arg(call, "CurrentURI"));
      if (newUri.empty()) throw UpnpError{402, "Invalid Args"};
      bool playingNow;
      {
        std::lock_guard<std::mutex> g(mu);
        playingNow = onScreen;
        uri = newUri;
        uriMeta = arg(call, "CurrentURIMetaData");
        nextUri.clear();
        nextMeta.clear();
        controller = controllerName(req);
        hasStatus = false;
        transport = "STOPPED";
      }
      // Already showing something: switch right away (what TVs do; many controllers send Play next anyway).
      if (playingNow) connect();
      else publish(kAvt);
      return {};
    }
    if (a == "SetNextAVTransportURI") {
      std::lock_guard<std::mutex> g(mu);
      nextUri = trim(arg(call, "NextURI"));
      nextMeta = arg(call, "NextURIMetaData");
      return {};
    }
    if (a == "Play") {
      bool same, ended, image;
      {
        std::lock_guard<std::mutex> g(mu);
        if (uri.empty()) throw UpnpError{701, "Transition not available"};
        same = onScreen && screenUrl == uri;
        ended = hasStatus && status.ended;
        image = mediaType == "image";
      }
      if (same) {
        cancelIdleTimeout();
        if (ended) emitCmd(0, 0);  // replay
        markCommand(true);
        emitCmd(1, 1);
        setTransport(image ? "PLAYING" : "TRANSITIONING");
      } else {
        connect();
      }
      return {};
    }
    if (a == "Pause") {
      {
        std::lock_guard<std::mutex> g(mu);
        if (!onScreen) throw UpnpError{701, "Transition not available"};
      }
      markCommand(false);
      emitCmd(1, 0);
      setTransport("PAUSED_PLAYBACK");
      return {};
    }
    if (a == "Stop") {
      cancelIdleTimeout();
      bool wasOnScreen;
      {
        std::lock_guard<std::mutex> g(mu);
        hasStatus = false;
        transport = uri.empty() ? "NO_MEDIA_PRESENT" : "STOPPED";
        wasOnScreen = onScreen;
        onScreen = false;
        screenUrl.clear();
      }
      if (wasOnScreen) emitCmd(2, 0);
      publish(kAvt);
      return {};
    }
    if (a == "Seek") {
      std::string unit = arg(call, "Unit");
      if (unit == "REL_TIME" || unit == "ABS_TIME") {
        int64_t ms = 0;
        if (!parseTime(arg(call, "Target"), ms)) throw UpnpError{711, "Illegal seek target"};
        emitCmd(0, ms / 1000.0);
      }
      return {};
    }
    if (a == "Next" || a == "Previous") return {};  // single-item queue: the controller manages its playlist
    if (a == "GetTransportInfo") {
      std::lock_guard<std::mutex> g(mu);
      return {{"CurrentTransportState", transport}, {"CurrentTransportStatus", transportStatusLocked()}, {"CurrentSpeed", "1"}};
    }
    if (a == "GetPositionInfo") {
      std::lock_guard<std::mutex> g(mu);
      int64_t dur = hasStatus && status.durationMs > 0 ? status.durationMs : metaDurationMs;
      int64_t pos = hasStatus ? status.positionMs : 0;
      bool has = !uri.empty();
      return {{"Track", has ? "1" : "0"},           {"TrackDuration", formatTime(dur)},
              {"TrackMetaData", uriMeta},           {"TrackURI", uri},
              {"RelTime", formatTime(pos)},         {"AbsTime", formatTime(pos)},
              {"RelCount", "2147483647"},           {"AbsCount", "2147483647"}};
    }
    if (a == "GetMediaInfo") {
      std::lock_guard<std::mutex> g(mu);
      int64_t dur = hasStatus && status.durationMs > 0 ? status.durationMs : metaDurationMs;
      bool has = !uri.empty();
      return {{"NrTracks", has ? "1" : "0"},
              {"MediaDuration", formatTime(dur)},
              {"CurrentURI", uri},
              {"CurrentURIMetaData", uriMeta},
              {"NextURI", nextUri},
              {"NextURIMetaData", nextMeta},
              {"PlayMedium", has ? "NETWORK" : "NONE"},
              {"RecordMedium", "NOT_IMPLEMENTED"},
              {"WriteStatus", "NOT_IMPLEMENTED"}};
    }
    if (a == "GetDeviceCapabilities")
      return {{"PlayMedia", "NETWORK"}, {"RecMedia", "NOT_IMPLEMENTED"}, {"RecQualityModes", "NOT_IMPLEMENTED"}};
    if (a == "GetTransportSettings") return {{"PlayMode", "NORMAL"}, {"RecQualityMode", "NOT_IMPLEMENTED"}};
    if (a == "GetCurrentTransportActions") {
      std::lock_guard<std::mutex> g(mu);
      return {{"Actions", transportActionsLocked()}};
    }
    throw UpnpError{401, "Invalid Action"};
  }

  /** Show the current URI: a play event for JS (the player is keyed by URL). */
  void connect() {
    cancelIdleTimeout();
    std::string u, meta, ctl;
    {
      std::lock_guard<std::mutex> g(mu);
      if (uri.empty()) return;
      u = uri;
      meta = uriMeta;
      ctl = controller;
    }
    Didl didl = parseDidl(meta);
    std::string type = didl.mediaType.empty() ? guessMediaType(u) : didl.mediaType;
    markCommand(true);
    bool same;
    float gain;
    {
      std::lock_guard<std::mutex> g(mu);
      mediaType = type;
      metaDurationMs = didl.durationMs;
      hasStatus = false;
      transport = type == "image" ? "PLAYING" : "TRANSITIONING";
      same = onScreen && screenUrl == u;
      onScreen = true;
      screenUrl = u;
      gain = playerVolumeLocked();
    }
    note("play " + type + " from " + ctl + ": " + (didl.title.empty() ? u : didl.title));
    if (same) {
      // Same URL already on screen: the player is keyed by URL and would just carry on, so restart it.
      emitCmd(0, 0);
      emitCmd(1, 1);
    } else if (emit) {
      emit("{\"type\":\"play\",\"source\":\"dlna\",\"url\":\"" + jsonEscape(u) + "\",\"mediaType\":\"" + type +
           "\",\"title\":\"" + jsonEscape(didl.title) + "\",\"artist\":\"" + jsonEscape(didl.artist) +
           "\",\"album\":\"" + jsonEscape(didl.album) + "\",\"artworkUrl\":\"" + jsonEscape(didl.artworkUrl) +
           "\",\"durationMs\":" + std::to_string(didl.durationMs) + ",\"startMs\":0,\"gain\":" + jsonNum(gain) +
           ",\"client\":\"" + jsonEscape(ctl) + "\"}");
    }
    publish(kAvt);
  }

  /** Track finished: play the queued next URI, else leave the screen after a moment (controllers
   *  queue the next item then). */
  void onEnded() {
    bool hasNext = false;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!nextUri.empty()) {
        uri = nextUri;
        uriMeta = nextMeta;
        nextUri.clear();
        nextMeta.clear();
        hasNext = true;
      }
    }
    if (hasNext) {
      connect();
      return;
    }
    uint64_t gen = cancelIdleTimeout();
    std::weak_ptr<Impl> weak = shared_from_this();
    timer.post([weak, gen] {
      auto d = weak.lock();
      if (!d) return;
      bool leave;
      {
        std::lock_guard<std::mutex> g(d->mu);
        leave = gen == d->idleGen && d->hasStatus && d->status.ended && d->onScreen;
        if (leave) {
          d->onScreen = false;
          d->screenUrl.clear();
        }
      }
      if (leave) {
        d->note("idle after the end");
        d->emitCmd(2, 0);
      }
    }, kIdleAfterEndMs);
  }

  void markCommand(bool playing) {
    std::lock_guard<std::mutex> g(mu);
    wantPlaying = playing;
    commandAt = nowMs();
  }

  /** Every 2 s: a paused or stopped item whose controller went quiet leaves the screen. */
  void watchController() {
    bool leave = false;
    int64_t quietMs = 0;
    {
      std::lock_guard<std::mutex> g(mu);
      quietMs = nowMs() - lastControlMs;
      // While playing, only for Windows: its player polls every second while its window is open
      // and closing the window sends nothing, so the video played on to the end. Phone apps may
      // stop polling in the background (screen off, music) and must keep playing.
      const bool playing = transport == "PLAYING" || transport == "TRANSITIONING";
      const bool windows = controller == "Windows PC";  // polls every second: 10 s of silence is enough
      if (onScreen && (!playing || windows) && quietMs > (windows ? kControllerGoneMs / 2 : kControllerGoneMs)) {
        leave = true;
        onScreen = false;
        screenUrl.clear();
        hasStatus = false;
        transport = "STOPPED";
      }
    }
    if (leave) {
      note("controller quiet for " + std::to_string(quietMs / 1000) + " s: leaving the screen");
      emitCmd(2, 0);
      publish(kAvt);
      // (Windows still shows "disconnecting" after closing its player mid-cast: it only
      // unsubscribes and sends nothing more. An SSDP byebye/alive didn't clear that either.)
    }
    std::weak_ptr<Impl> weak = shared_from_this();
    timer.post([weak] { if (auto d = weak.lock()) d->watchController(); }, 2000);
  }

  /** Cancels a pending idle timeout; returns the generation a new one should carry. */
  uint64_t cancelIdleTimeout() {
    std::lock_guard<std::mutex> g(mu);
    return ++idleGen;
  }

  void setTransport(const std::string& t) {
    bool changed;
    {
      std::lock_guard<std::mutex> g(mu);
      changed = transport != t;
      transport = t;
    }
    if (changed) publish(kAvt);
  }

  void reportPlayback(const Status& s, bool closed) {
    if (closed) {
      // Another sender took over or the user left: like Android's disconnectClient().
      {
        std::lock_guard<std::mutex> g(mu);
        if (!onScreen) return;
        onScreen = false;
        screenUrl.clear();
        transport = "STOPPED";
        hasStatus = false;
      }
      cancelIdleTimeout();
      note("player closed");
      publish(kAvt);
      return;
    }
    bool ended = false, changed = false;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!onScreen) return;  // a late report after Stop
      // Images have no player state: the transport stays PLAYING unless the viewer fails.
      if (mediaType == "image" && !s.failed) return;
      hasStatus = true;
      status = s;
      // A report still showing the old play/pause state right after a command: don't flip-flop.
      if (nowMs() - commandAt < kCommandSettleMs && !s.ended && !s.failed && s.playing != wantPlaying) return;
      std::string t;
      if (s.failed || s.ended) t = "STOPPED";
      else if (s.playing && s.buffering) t = "TRANSITIONING";
      else if (s.playing) t = "PLAYING";
      else if (s.ready) t = "PAUSED_PLAYBACK";
      else t = "TRANSITIONING";
      ended = s.ended && transport != "STOPPED";
      changed = t != transport;
      transport = t;
    }
    if (changed) publish(kAvt);
    if (ended) onEnded();
  }

  // ---------------------------------------------------------------- RenderingControl / ConnectionManager

  Args renderingControl(const SoapCall& call) {
    const std::string& a = call.action;
    if (a == "GetVolume") {
      std::lock_guard<std::mutex> g(mu);
      return {{"CurrentVolume", std::to_string(volume)}};
    }
    if (a == "SetVolume") {
      std::string s = trim(arg(call, "DesiredVolume"));
      char* end = nullptr;
      long val = strtol(s.c_str(), &end, 10);
      if (s.empty() || !end || *end != '\0' || val < 0 || val > 100) throw UpnpError{402, "Invalid Args"};
      float gain;
      {
        std::lock_guard<std::mutex> g(mu);
        volume = static_cast<int>(val);
        if (val > 0) muted = false;
        gain = playerVolumeLocked();
      }
      saveVolume(static_cast<int>(val));
      emitVolume(gain);
      publish(kRcs);
      return {};
    }
    if (a == "GetMute") {
      std::lock_guard<std::mutex> g(mu);
      return {{"CurrentMute", muted ? "1" : "0"}};
    }
    if (a == "SetMute") {
      std::string m = lower(trim(arg(call, "DesiredMute")));
      bool on = m == "1" || m == "true" || m == "yes";
      float gain;
      {
        std::lock_guard<std::mutex> g(mu);
        muted = on;
        gain = playerVolumeLocked();
      }
      emitVolume(gain);
      publish(kRcs);
      return {};
    }
    if (a == "ListPresets") return {{"CurrentPresetNameList", "FactoryDefaults"}};
    if (a == "SelectPreset") return {};
    throw UpnpError{401, "Invalid Action"};
  }

  Args connectionManager(const SoapCall& call) {
    const std::string& a = call.action;
    if (a == "GetProtocolInfo") return {{"Source", ""}, {"Sink", sinkProtocolInfo()}};
    if (a == "GetCurrentConnectionIDs") return {{"ConnectionIDs", "0"}};
    if (a == "GetCurrentConnectionInfo")
      return {{"RcsID", "0"},       {"AVTransportID", "0"}, {"ProtocolInfo", ""}, {"PeerConnectionManager", ""},
              {"PeerConnectionID", "-1"}, {"Direction", "Input"}, {"Status", "OK"}};
    throw UpnpError{401, "Invalid Action"};
  }

  // ---------------------------------------------------------------- GENA
  // Controllers SUBSCRIBE to a service's event URL and we NOTIFY them with LastChange so their
  // play/pause button and volume slider follow the TV. Position is polled, not evented.

  /** Full current state for a service, sent as the initial event and after every change. */
  std::string eventSnapshot(const std::string& serviceType) {
    std::lock_guard<std::mutex> g(mu);
    if (serviceType == kAvt) {
      std::string dur = formatTime(hasStatus && status.durationMs > 0 ? status.durationMs : metaDurationMs);
      bool has = !uri.empty();
      return lastChangeEvent(kAvt, {{"TransportState", transport},
                                    {"TransportStatus", transportStatusLocked()},
                                    {"CurrentTransportActions", transportActionsLocked()},
                                    {"AVTransportURI", uri},
                                    {"CurrentTrackURI", uri},
                                    {"NumberOfTracks", has ? "1" : "0"},
                                    {"CurrentTrack", has ? "1" : "0"},
                                    {"CurrentTrackDuration", dur},
                                    {"CurrentMediaDuration", dur},
                                    {"CurrentPlayMode", "NORMAL"},
                                    {"TransportPlaySpeed", "1"}});
    }
    if (serviceType == kRcs)
      return lastChangeEvent(kRcs, {{"Volume", std::to_string(volume)}, {"Mute", muted ? "1" : "0"}});
    return "";  // ConnectionManager: nothing changes
  }

  std::string transportStatusLocked() const { return hasStatus && status.failed ? "ERROR_OCCURRED" : "OK"; }

  std::string transportActionsLocked() const {
    if (transport == "PLAYING" || transport == "TRANSITIONING") return "Pause,Stop,Seek";
    if (transport == "PAUSED_PLAYBACK") return "Play,Stop,Seek";
    if (transport == "STOPPED") return "Play";
    return "";
  }

  HttpResponse genaOk(const std::string& sid, long timeoutSec) {
    HttpResponse r;
    r.headers = {{"SID", sid}, {"TIMEOUT", "Second-" + std::to_string(timeoutSec)}, {"DATE", httpDate()}};
    return r;
  }

  HttpResponse genaHandle(const HttpRequest& req, const std::string& serviceType) {
    std::string sid = req.header("sid");
    long timeoutSec = 1800;
    {
      std::string t = req.header("timeout");
      size_t p = lower(t).find("second-");
      if (p != std::string::npos) {
        char* end = nullptr;
        long v = strtol(t.c_str() + p + 7, &end, 10);
        if (end && end != t.c_str() + p + 7) timeoutSec = std::min(3600L, std::max(60L, v));
      }
    }
    if (req.method == "SUBSCRIBE") {
      if (!sid.empty()) {
        std::lock_guard<std::mutex> g(subMu);
        auto it = subs.find(sid);
        if (it == subs.end()) {
          note("GENA renew of unknown " + sid + " -> 412");
          return HttpResponse::code(412);
        }
        it->second.expiresAt = nowMs() + timeoutSec * 1000;
        return genaOk(sid, timeoutSec);
      }
      std::vector<Url> callbacks;
      std::string cb = req.header("callback");
      size_t p = 0;
      while ((p = cb.find('<', p)) != std::string::npos) {
        size_t e = cb.find('>', p);
        if (e == std::string::npos) break;
        Url u;
        if (parseUrl(cb.substr(p + 1, e - p - 1), u)) callbacks.push_back(u);
        p = e + 1;
      }
      if (callbacks.empty() || req.header("nt") != "upnp:event") return HttpResponse::code(412);
      std::string newSid = "uuid:" + randomUuid();
      note("GENA subscribe " + serviceType.substr(serviceType.rfind(':', serviceType.size() - 3) + 1) + " from " + req.remote);
      {
        std::lock_guard<std::mutex> g(subMu);
        subs[newSid] = Sub{serviceType, callbacks, nowMs() + timeoutSec * 1000, 0};
      }
      // Initial event must follow the SUBSCRIBE response.
      std::weak_ptr<Impl> weak = shared_from_this();
      gena.post([weak, newSid, serviceType] {
        if (auto d = weak.lock()) {
          std::string body = d->eventSnapshot(serviceType);
          if (!body.empty()) d->notify(newSid, body);
        }
      }, 200);
      return genaOk(newSid, timeoutSec);
    }
    if (req.method == "UNSUBSCRIBE") {
      // Unknown SIDs (e.g. from before an app restart) get 200 too: the controller only wants
      // the subscription gone, and an error left Windows stuck "disconnecting".
      bool known;
      {
        std::lock_guard<std::mutex> g(subMu);
        known = !sid.empty() && subs.erase(sid);
      }
      note(std::string("GENA unsubscribe ") + (known ? "" : "(unknown) ") + sid);
      return HttpResponse::code(200);
    }
    return HttpResponse::code(405);
  }

  /** Push the current state of serviceType to its subscribers (on the event thread). */
  void publish(const std::string& serviceType) {
    std::weak_ptr<Impl> weak = shared_from_this();
    gena.post([weak, serviceType] {
      auto d = weak.lock();
      if (!d) return;
      std::string body = d->eventSnapshot(serviceType);
      if (body.empty()) return;
      std::vector<std::string> sids;
      {
        std::lock_guard<std::mutex> g(d->subMu);
        int64_t now = nowMs();
        for (auto it = d->subs.begin(); it != d->subs.end();) {
          if (it->second.expiresAt < now) { it = d->subs.erase(it); continue; }
          if (it->second.service == serviceType) sids.push_back(it->first);
          ++it;
        }
      }
      for (auto& sid : sids) d->notify(sid, body);
    });
  }

  void notify(const std::string& sid, const std::string& body) {
    std::vector<Url> callbacks;
    uint64_t seq;
    {
      std::lock_guard<std::mutex> g(subMu);
      auto it = subs.find(sid);
      if (it == subs.end()) return;
      callbacks = it->second.callbacks;
      seq = it->second.seq++;
    }
    for (auto& u : callbacks)
      if (sendNotify(u, sid, seq, body)) return;
    Timeline::add("dlna: event delivery failed for " + sid);
  }

  // ---------------------------------------------------------------- helpers

  /** Shown on the TV ("Playing from ..."): the controller's product name, else its address. */
  static std::string controllerName(const HttpRequest& req) {
    std::string ua = lower(req.header("user-agent"));
    if (contains(ua, "windows")) return "Windows PC";
    if (contains(ua, "bubbleupnp")) return "BubbleUPnP";
    if (contains(ua, "android")) return "Android";
    return req.remote.empty() ? "DLNA" : req.remote;
  }

  std::string statusJson() {
    std::ostringstream o;
    {
      std::lock_guard<std::mutex> g(mu);
      o << "{\"transport\":\"" << transport << "\",\"uri\":\"" << jsonEscape(uri) << "\",\"nextUri\":\""
        << jsonEscape(nextUri) << "\",\"mediaType\":\"" << mediaType << "\",\"onScreen\":" << (onScreen ? "true" : "false")
        << ",\"controller\":\"" << jsonEscape(controller) << "\",\"volume\":" << volume
        << ",\"muted\":" << (muted ? "true" : "false") << ",\"positionMs\":" << (hasStatus ? status.positionMs : 0)
        << ",\"durationMs\":" << (hasStatus ? status.durationMs : metaDurationMs)
        << ",\"sinceControlSec\":" << (lastControlMs ? (nowMs() - lastControlMs) / 1000 : -1);
    }
    {
      std::lock_guard<std::mutex> g(recentMu);
      o << ",\"recent\":[";
      bool first = true;
      for (auto& r : recent) {
        o << (first ? "" : ",") << "\"" << jsonEscape(r) << "\"";
        first = false;
      }
      o << "]";
    }
    o << ",\"subscriptions\":[";
    {
      std::lock_guard<std::mutex> g(subMu);
      int64_t now = nowMs();
      bool first = true;
      for (auto& [sid, s] : subs) {
        o << (first ? "" : ",") << "{\"sid\":\"" << jsonEscape(sid) << "\",\"service\":\"" << jsonEscape(s.service)
          << "\",\"callback\":\"" << jsonEscape(s.callbacks.empty() ? "" : s.callbacks[0].text)
          << "\",\"expiresInSec\":" << (s.expiresAt - now) / 1000 << ",\"seq\":" << s.seq << "}";
        first = false;
      }
    }
    o << "]}";
    return o.str();
  }
};

const char* DlnaRenderer::deviceType() { return kDeviceType; }

std::vector<std::string> DlnaRenderer::serviceTypes() { return {kAvt, kRcs, kCms}; }

DlnaRenderer::DlnaRenderer() : d_(std::make_shared<Impl>()) {}

DlnaRenderer::~DlnaRenderer() { stop(); }

void DlnaRenderer::setCallbacks(Log log, Emit emit) {
  d_->log = std::move(log);
  d_->emit = std::move(emit);
}

void DlnaRenderer::start(const std::string& friendlyName, const std::string& uuid) {
  if (d_->running.exchange(true)) return;
  {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->name = friendlyName;
    d_->uuid = uuid;
  }
  d_->loadVolume();
  d_->gena.start("dlna-gena");
  d_->timer.start("dlna-timer");
  d_->watchController();
  d_->note("DLNA renderer uuid " + uuid);
}

void DlnaRenderer::stop() {
  if (!d_->running.exchange(false)) return;
  d_->gena.stop();
  d_->timer.stop();
  {
    std::lock_guard<std::mutex> g(d_->subMu);
    d_->subs.clear();
  }
  std::lock_guard<std::mutex> g(d_->mu);
  d_->transport = d_->uri.empty() ? "NO_MEDIA_PRESENT" : "STOPPED";
}

void DlnaRenderer::serve(int fd, const std::string& remoteIp) {
  std::shared_ptr<Impl> d = d_;
  HttpRequest req;
  req.remote = remoteIp;
  if (readRequest(fd, req)) {
    HttpResponse res;
    try {
      res = d->running ? d->route(req) : HttpResponse::code(404);
    } catch (const std::exception& e) {
      d->note("handler failed for " + req.method + " " + req.path + ": " + e.what());
      res = HttpResponse::code(500);
    } catch (...) {
      res = HttpResponse::code(500);
    }
    std::string action = req.header("soapaction");
    size_t h = action.rfind('#');
    if (h != std::string::npos) action = action.substr(h + 1);
    while (!action.empty() && (action.back() == '"' || action.back() == ' ')) action.pop_back();
    // Polled actions every second would push everything else out: keep only the last of a run.
    std::string line = remoteIp + " " + req.method + " " + req.path + (action.empty() ? "" : " " + action) + " -> " +
                       std::to_string(res.status);
    if (res.status >= 400 && !req.body.empty()) line += " body: " + req.body.substr(0, 300);
    d->remember(line);
    writeResponse(fd, res, req.method != "HEAD");
  }
  close(fd);
}

void DlnaRenderer::reportPlayback(double durationSec, double positionSec, double rate, bool ready, bool buffering,
                                  bool ended, bool failed, bool closed) {
  // Live streams report an infinite duration; NaN before the player knows.
  auto ms = [](double sec) { return std::isfinite(sec) && sec > 0 ? static_cast<int64_t>(sec * 1000) : int64_t(0); };
  Impl::Status s;
  s.durationMs = ms(durationSec);
  s.positionMs = ms(positionSec);
  s.playing = rate > 0;
  s.ready = ready;
  s.buffering = buffering;
  s.ended = ended;
  s.failed = failed;
  d_->reportPlayback(s, closed);
}

std::string DlnaRenderer::statusJson() { return d_->statusJson(); }

}  // namespace ProbeNativeTurboModule
