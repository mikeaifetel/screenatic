#include "Fmp4Muxer.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace ProbeNativeTurboModule {

namespace {

constexpr uint32_t kTimescale = 90000;
constexpr uint32_t kTrackId = 1;

void u8(std::vector<uint8_t>& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
void u16(std::vector<uint8_t>& b, uint32_t v) { u8(b, v >> 8); u8(b, v); }
void u32(std::vector<uint8_t>& b, uint32_t v) { u16(b, v >> 16); u16(b, v); }
void u64(std::vector<uint8_t>& b, uint64_t v) { u32(b, static_cast<uint32_t>(v >> 32)); u32(b, static_cast<uint32_t>(v)); }
void bytes(std::vector<uint8_t>& b, const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
void zeros(std::vector<uint8_t>& b, size_t n) { b.insert(b.end(), n, 0); }

// Box with its size patched in when the builder goes out of scope.
struct Box {
  std::vector<uint8_t>& b;
  size_t start;
  Box(std::vector<uint8_t>& buf, const char* type, int version = -1, uint32_t flags = 0) : b(buf), start(buf.size()) {
    u32(b, 0);
    b.insert(b.end(), type, type + 4);
    if (version >= 0) u32(b, (static_cast<uint32_t>(version) << 24) | flags);
  }
  ~Box() {
    uint32_t size = static_cast<uint32_t>(b.size() - start);
    b[start] = size >> 24; b[start + 1] = size >> 16; b[start + 2] = size >> 8; b[start + 3] = size;
  }
};

void matrix(std::vector<uint8_t>& b) {
  const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  for (uint32_t v : m) u32(b, v);
}

// Splits Annex-B into NAL units (without start codes).
std::vector<std::pair<const uint8_t*, size_t>> nals(const uint8_t* p, size_t n) {
  std::vector<std::pair<const uint8_t*, size_t>> out;
  size_t i = 0, start = SIZE_MAX;
  while (i + 3 <= n) {
    if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 3 < n && p[i + 2] == 0 && p[i + 3] == 1))) {
      size_t sc = p[i + 2] == 1 ? 3 : 4;
      if (start != SIZE_MAX) {
        size_t end = i;
        while (end > start && p[end - 1] == 0) --end;  // trailing zero bytes belong to the next start code
        out.emplace_back(p + start, end - start);
      }
      i += sc;
      start = i;
    } else {
      ++i;
    }
  }
  if (start != SIZE_MAX && start < n) out.emplace_back(p + start, n - start);
  return out;
}

}  // namespace

namespace {
/** Unsigned Exp-Golomb from the start of a NAL payload (emulation prevention ignored: only the
 *  first few bits of a slice header are read). -1 if out of data. */
struct Bits {
  const uint8_t* p; size_t n; size_t bit = 0;
  int get() { if (bit / 8 >= n) return -1; int b = (p[bit / 8] >> (7 - bit % 8)) & 1; ++bit; return b; }
  int64_t ue() {
    int zeros = 0;
    for (int b; (b = get()) == 0;) if (++zeros > 31) return -1;
    int64_t v = 1;
    for (int i = 0; i < zeros; ++i) { int b = get(); if (b < 0) return -1; v = (v << 1) | b; }
    return v - 1;
  }
};
}  // namespace

void Fmp4Muxer::reset() {
  sps_.clear();
  pps_.clear();
  lastPtsUs_ = -1;
  needInit_ = true;
  bytesSinceKey_ = 0;
  holding_ = false;
  stats_ = {};
}

void Fmp4Muxer::push(const uint8_t* data, size_t size, int64_t ptsUs, std::vector<uint8_t>& out) {
  // AVCC sample: 4-byte big-endian length + NAL, parameter sets and AUDs left out (they live in avcC).
  std::vector<uint8_t> sample;
  bool key = false;
  bool counted = false;
  lastSeenPtsUs_ = ptsUs;
  for (auto [p, n] : nals(data, size)) {
    if (n == 0) continue;
    int type = p[0] & 0x1f;
    if ((type == 1 || type == 5) && !counted) {
      counted = true;
      ++stats_.pictures;
      if (type == 5) {
        ++stats_.idr;
      } else {
        Bits b{p + 1, n - 1};
        b.ue();  // first_mb_in_slice
        int64_t st = b.ue();
        if (st >= 0 && (st % 5 == 2 || st % 5 == 4)) ++stats_.nonIdrI;  // I or SI
      }
    } else if (type == 6) {
      ++stats_.sei;
      if (n > 1 && p[1] == 6) ++stats_.seiRecovery;  // first message is a recovery point
    }
    if (type == 7 || type == 8) {
      std::vector<uint8_t> v(p, p + n);
      auto& slot = type == 7 ? sps_ : pps_;
      if (slot != v) {
        slot = std::move(v);
        needInit_ = true;
      }
      continue;
    }
    if (type == 9) continue;  // access unit delimiter
    if (type == 5) key = true;
    u32(sample, static_cast<uint32_t>(n));
    sample.insert(sample.end(), p, p + n);
  }
  if (sps_.size() < 4 || pps_.empty() || sample.empty()) return;
  if (holding_ && !key) {
    ++heldFrames_;
    return;
  }
  // The keyframe ending a hold continues the timeline with a normal frame duration: a long one
  // (the gap since the last frame sent) makes Vega drop every frame after it up to the next
  // keyframe (tested), and frames follow right away anyway.
  const bool resumed = holding_;
  holding_ = false;
  if (needInit_) {
    if (!key) return;  // a new init segment must be followed by a keyframe
    writeInit(out);
    needInit_ = false;
    ++inits_;
  }

  int64_t intervalUs = lastPtsUs_ < 0 || resumed ? 33333 : ptsUs - lastPtsUs_;
  // Upper bound 600 ms: Vega's player resumes from a stall only once ~0.5 s is buffered (measured),
  // so the first frame after an idle screen must carry that much on its own or it never shows.
  intervalUs = std::clamp<int64_t>(intervalUs, 1000, 600000);
  lastPtsUs_ = ptsUs;
  uint32_t duration = static_cast<uint32_t>(intervalUs * kTimescale / 1000000);

  std::vector<uint8_t> moof;
  {
    Box moofBox(moof, "moof");
    {
      Box mfhd(moof, "mfhd", 0);
      u32(moof, ++sequence_);
    }
    Box traf(moof, "traf");
    {
      Box tfhd(moof, "tfhd", 0, 0x020000);  // default-base-is-moof
      u32(moof, kTrackId);
    }
    {
      Box tfdt(moof, "tfdt", 1);
      u64(moof, decodeTime_);
    }
    {
      // data-offset, sample-duration, sample-size, sample-flags present
      Box trun(moof, "trun", 0, 0x000001 | 0x000100 | 0x000200 | 0x000400);
      u32(moof, 1);
      u32(moof, 0);  // data offset, patched below
      u32(moof, duration);
      u32(moof, static_cast<uint32_t>(sample.size()));
      u32(moof, key ? 0x02000000 : 0x01010000);  // depends on nothing / is a non-sync sample
    }
  }
  // data offset = from moof start to the first byte of mdat payload
  uint32_t dataOffset = static_cast<uint32_t>(moof.size() + 8);
  size_t at = moof.size() - 12;  // trun: ..., data_offset, duration, size, flags (4 bytes each)
  at -= 4;
  moof[at] = dataOffset >> 24; moof[at + 1] = dataOffset >> 16; moof[at + 2] = dataOffset >> 8; moof[at + 3] = dataOffset;

  bytes(out, moof);
  u32(out, static_cast<uint32_t>(sample.size() + 8));
  out.insert(out.end(), {'m', 'd', 'a', 't'});
  bytes(out, sample);
  bytesSinceKey_ = key ? sample.size() : bytesSinceKey_ + sample.size();
  if (holdLimit_ > 0 && bytesSinceKey_ > holdLimit_) {
    holding_ = true;
    ++holds_;
  }
  if (key) {
    keyTimes_.push_back(static_cast<double>(decodeTime_) / kTimescale);
    while (keyTimes_.size() > 8) keyTimes_.pop_front();
  }
  decodeTime_ += duration;
  ++frames_;
  if (key) {
    ++keyframes_;
    if (lastKeyPtsUs_ >= 0) lastKeyIntervalSec_ = (ptsUs - lastKeyPtsUs_) / 1e6;
    lastKeyPtsUs_ = ptsUs;
  }
}

void Fmp4Muxer::writeInit(std::vector<uint8_t>& b) const {
  const uint16_t w = static_cast<uint16_t>(width_ > 0 ? width_ : 1920);
  const uint16_t h = static_cast<uint16_t>(height_ > 0 ? height_ : 1080);
  {
    Box ftyp(b, "ftyp");
    b.insert(b.end(), {'i', 's', 'o', 'm'});
    u32(b, 0x200);
    for (const char* brand : {"isom", "iso6", "avc1", "mp41"}) b.insert(b.end(), brand, brand + 4);
  }
  Box moov(b, "moov");
  {
    Box mvhd(b, "mvhd", 0);
    u32(b, 0); u32(b, 0);       // creation / modification
    u32(b, 1000); u32(b, 0);    // timescale, duration
    u32(b, 0x00010000);         // rate 1.0
    u16(b, 0x0100); zeros(b, 10);  // volume 1.0, reserved
    matrix(b);
    zeros(b, 24);               // pre_defined
    u32(b, kTrackId + 1);       // next_track_ID
  }
  {
    Box trak(b, "trak");
    {
      Box tkhd(b, "tkhd", 0, 0x000003);  // enabled, in movie
      u32(b, 0); u32(b, 0);
      u32(b, kTrackId); u32(b, 0);
      u32(b, 0);                         // duration
      zeros(b, 8);
      u16(b, 0); u16(b, 0); u16(b, 0); u16(b, 0);  // layer, alt group, volume, reserved
      matrix(b);
      u32(b, static_cast<uint32_t>(w) << 16);
      u32(b, static_cast<uint32_t>(h) << 16);
    }
    Box mdia(b, "mdia");
    {
      Box mdhd(b, "mdhd", 0);
      u32(b, 0); u32(b, 0);
      u32(b, kTimescale); u32(b, 0);
      u16(b, 0x55c4);  // language "und"
      u16(b, 0);
    }
    {
      Box hdlr(b, "hdlr", 0);
      u32(b, 0);
      b.insert(b.end(), {'v', 'i', 'd', 'e'});
      zeros(b, 12);
      const char name[] = "Screenatic";
      b.insert(b.end(), name, name + sizeof name);
    }
    Box minf(b, "minf");
    {
      Box vmhd(b, "vmhd", 0, 1);
      zeros(b, 8);
    }
    {
      Box dinf(b, "dinf");
      Box dref(b, "dref", 0);
      u32(b, 1);
      Box url(b, "url ", 0, 1);  // media is in this file
    }
    Box stbl(b, "stbl");
    {
      Box stsd(b, "stsd", 0);
      u32(b, 1);
      Box avc1(b, "avc1");
      zeros(b, 6); u16(b, 1);     // reserved, data_reference_index
      zeros(b, 16);               // pre_defined, reserved
      u16(b, w); u16(b, h);
      u32(b, 0x00480000); u32(b, 0x00480000);  // 72 dpi
      u32(b, 0); u16(b, 1);       // reserved, frame_count
      zeros(b, 32);               // compressorname
      u16(b, 0x0018); u16(b, 0xffff);  // depth, pre_defined
      Box avcC(b, "avcC");
      u8(b, 1);
      u8(b, sps_[1]); u8(b, sps_[2]); u8(b, sps_[3]);  // profile, compatibility, level
      u8(b, 0xff);                // 4-byte NAL lengths
      u8(b, 0xe1);                // one SPS
      u16(b, static_cast<uint32_t>(sps_.size())); bytes(b, sps_);
      u8(b, 1);                   // one PPS
      u16(b, static_cast<uint32_t>(pps_.size())); bytes(b, pps_);
    }
    for (const char* empty : {"stts", "stsc", "stco"}) {
      Box box(b, empty, 0);
      u32(b, 0);
    }
    {
      Box stsz(b, "stsz", 0);
      u32(b, 0); u32(b, 0);
    }
  }
  {
    // Required for fragmented files: declares that the track continues in moof boxes.
    Box mvex(b, "mvex");
    Box trex(b, "trex", 0);
    u32(b, kTrackId);
    u32(b, 1);  // default sample description index
    u32(b, 0); u32(b, 0); u32(b, 0);
  }
}

// ---------------------------------------------------------------- FLAC audio

namespace {
constexpr uint32_t kAudioRate = 44100;
constexpr uint32_t kAudioTrackId = 1;  // its own file / SourceBuffer
}  // namespace

void FlacMp4Muxer::reset() {
  streamInfo_.clear();
  needInit_ = true;
  pendingData_.clear();
  pendingSizes_.clear();
  pendingSamples_.clear();
}

void FlacMp4Muxer::setStreamInfo(const uint8_t* body, size_t size) {
  streamInfo_.assign(body, body + size);
  needInit_ = true;
}

void FlacMp4Muxer::addFrame(const uint8_t* data, size_t size, uint32_t samples, std::vector<uint8_t>& out) {
  if (streamInfo_.size() != 34) return;
  if (needInit_) {
    writeInit(out);
    needInit_ = false;
  }
  pendingData_.insert(pendingData_.end(), data, data + size);
  pendingSizes_.push_back(static_cast<uint32_t>(size));
  pendingSamples_.push_back(samples);
  if (pendingSizes_.size() >= kFramesPerFragment) flush(out);
}

void FlacMp4Muxer::flush(std::vector<uint8_t>& out) {
  if (pendingSizes_.empty()) return;
  std::vector<uint8_t> moof;
  size_t offsetAt = 0;
  {
    Box moofBox(moof, "moof");
    {
      Box mfhd(moof, "mfhd", 0);
      u32(moof, ++sequence_);
    }
    Box traf(moof, "traf");
    {
      Box tfhd(moof, "tfhd", 0, 0x020000);
      u32(moof, kAudioTrackId);
    }
    {
      Box tfdt(moof, "tfdt", 1);
      u64(moof, decodeTime_);
    }
    {
      Box trun(moof, "trun", 0, 0x000001 | 0x000100 | 0x000200);  // data offset, durations, sizes
      u32(moof, static_cast<uint32_t>(pendingSizes_.size()));
      offsetAt = moof.size();
      u32(moof, 0);
      for (size_t i = 0; i < pendingSizes_.size(); ++i) {
        u32(moof, pendingSamples_[i]);
        u32(moof, pendingSizes_[i]);
      }
    }
  }
  uint32_t dataOffset = static_cast<uint32_t>(moof.size() + 8);
  moof[offsetAt] = dataOffset >> 24; moof[offsetAt + 1] = dataOffset >> 16;
  moof[offsetAt + 2] = dataOffset >> 8; moof[offsetAt + 3] = dataOffset;
  bytes(out, moof);
  u32(out, static_cast<uint32_t>(pendingData_.size() + 8));
  out.insert(out.end(), {'m', 'd', 'a', 't'});
  bytes(out, pendingData_);
  for (uint32_t s : pendingSamples_) decodeTime_ += s;
  pendingData_.clear();
  pendingSizes_.clear();
  pendingSamples_.clear();
  ++fragments_;
}

void FlacMp4Muxer::writeInit(std::vector<uint8_t>& b) const {
  {
    Box ftyp(b, "ftyp");
    b.insert(b.end(), {'i', 's', 'o', 'm'});
    u32(b, 0x200);
    for (const char* brand : {"isom", "iso6", "mp41"}) b.insert(b.end(), brand, brand + 4);
  }
  Box moov(b, "moov");
  {
    Box mvhd(b, "mvhd", 0);
    u32(b, 0); u32(b, 0);
    u32(b, 1000); u32(b, 0);
    u32(b, 0x00010000);
    u16(b, 0x0100); zeros(b, 10);
    matrix(b);
    zeros(b, 24);
    u32(b, kAudioTrackId + 1);
  }
  {
    Box trak(b, "trak");
    {
      Box tkhd(b, "tkhd", 0, 0x000003);
      u32(b, 0); u32(b, 0);
      u32(b, kAudioTrackId); u32(b, 0);
      u32(b, 0);
      zeros(b, 8);
      u16(b, 0); u16(b, 0); u16(b, 0x0100); u16(b, 0);  // layer, alt group, volume 1.0
      matrix(b);
      u32(b, 0); u32(b, 0);
    }
    Box mdia(b, "mdia");
    {
      Box mdhd(b, "mdhd", 0);
      u32(b, 0); u32(b, 0);
      u32(b, kAudioRate); u32(b, 0);
      u16(b, 0x55c4);
      u16(b, 0);
    }
    {
      Box hdlr(b, "hdlr", 0);
      u32(b, 0);
      b.insert(b.end(), {'s', 'o', 'u', 'n'});
      zeros(b, 12);
      const char name[] = "Screenatic";
      b.insert(b.end(), name, name + sizeof name);
    }
    Box minf(b, "minf");
    {
      Box smhd(b, "smhd", 0);
      u16(b, 0); u16(b, 0);
    }
    {
      Box dinf(b, "dinf");
      Box dref(b, "dref", 0);
      u32(b, 1);
      Box url(b, "url ", 0, 1);
    }
    Box stbl(b, "stbl");
    {
      Box stsd(b, "stsd", 0);
      u32(b, 1);
      Box flac(b, "fLaC");
      zeros(b, 6); u16(b, 1);         // reserved, data_reference_index
      zeros(b, 8);                    // reserved
      u16(b, 2); u16(b, 16);          // channels, sample size
      zeros(b, 4);                    // pre_defined, reserved
      u32(b, kAudioRate << 16);       // sample rate 16.16
      Box dfla(b, "dfLa", 0);
      u8(b, 0x80);                    // last metadata block, type 0 = STREAMINFO
      u8(b, 0); u8(b, 0); u8(b, 34);  // length
      bytes(b, streamInfo_);
    }
    for (const char* empty : {"stts", "stsc", "stco"}) {
      Box box(b, empty, 0);
      u32(b, 0);
    }
    {
      Box stsz(b, "stsz", 0);
      u32(b, 0); u32(b, 0);
    }
  }
  {
    Box mvex(b, "mvex");
    Box trex(b, "trex", 0);
    u32(b, kAudioTrackId);
    u32(b, 1);
    u32(b, 0); u32(b, 0); u32(b, 0);
  }
}

}  // namespace ProbeNativeTurboModule
