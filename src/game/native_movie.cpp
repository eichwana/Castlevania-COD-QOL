#include "game/native_movie.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>

#include "ee/cpu.h"
#include "ee/kernel.h"
#include "ee/memory.h"
#include "game/hooks.h"
#include "host/settings.h"
#include "ipu/mpeg2_decoder.h"
#include "recomp/runtime.h"

namespace {
uint64_t fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// The video elementary stream of a PSS file (MPEG-2 program stream, video in stream 0xE0).
std::vector<uint8_t> pssVideo(const std::vector<uint8_t>& f) {
  std::vector<uint8_t> es;
  size_t i = 0;
  while (i + 6 <= f.size()) {
    if (f[i] || f[i + 1] || f[i + 2] != 1) {
      ++i;
      continue;
    }
    const uint8_t id = f[i + 3];
    if (id == 0xBA) {  // pack header (MPEG-2: 14 bytes plus stuffing)
      if (i + 14 > f.size()) break;
      i += 14 + (f[i + 13] & 7);
      continue;
    }
    if (id == 0xB9) break;
    const size_t len = size_t(f[i + 4]) << 8 | f[i + 5];
    if (id == 0xE0 && i + 9 <= f.size()) {
      const size_t header = 9 + f[i + 8];
      const size_t end = std::min(f.size(), i + 6 + len);
      if (i + header < end) es.insert(es.end(), f.begin() + ptrdiff_t(i + header), f.begin() + ptrdiff_t(end));
    }
    i += 6 + len;
  }
  return es;
}

bool writeBmp(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h, int mbh) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  const uint32_t image = uint32_t(w * h * 3), fileSize = 54 + image;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { for (int k = 0; k < 4; ++k) hdr[at + k] = uint8_t(v >> (8 * k)); };
  put32(2, fileSize), put32(10, 54), put32(14, 40), put32(18, uint32_t(w)), put32(22, uint32_t(-h));
  hdr[26] = 1, hdr[28] = 24;
  put32(34, image);
  out.write(reinterpret_cast<const char*>(hdr), 54);
  std::vector<uint8_t> row(size_t(w) * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t mb = size_t((x / 16) * mbh + y / 16), at = mb * 1024 + size_t((y % 16) * 16 + x % 16) * 4;
      row[size_t(x) * 3] = rgba[at + 2], row[size_t(x) * 3 + 1] = rgba[at + 1], row[size_t(x) * 3 + 2] = rgba[at];
    }
    out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
  return true;
}
}  // namespace

int mpeg2TestFile(const std::string& pss, const std::string& bmpDir, int every) {
  std::ifstream in(pss, std::ios::binary);
  if (!in) {
    std::printf("cannot open %s\n", pss.c_str());
    return 1;
  }
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::vector<uint8_t> es = pssVideo(file);
  std::printf("%zu bytes of video\n", es.size());
  Mpeg2Decoder dec;
  int shown = 0;
  std::vector<uint8_t> rgba;
  auto drain = [&] {
    while (!dec.ready.empty()) {
      rgba.assign(size_t(dec.width) * size_t(dec.height) * 4, 0);
      dec.toRgb32(dec.ready.front(), rgba.data());
      std::printf("picture %d hash %016llX type %d\n", shown, (unsigned long long)fnv(rgba.data(), rgba.size()), dec.ready.front().type);
      if (!bmpDir.empty() && every > 0 && shown % every == 0)
        writeBmp((std::filesystem::path(bmpDir) / std::format("pic{:05}.bmp", shown)).string(), rgba, dec.width, dec.height, dec.height / 16);
      ++shown;
      dec.ready.pop_front();
    }
  };
  // Units end where the next picture, GOP, sequence header or sequence end starts.
  size_t start = 0;
  for (size_t k = 4; k + 3 < es.size(); ++k) {
    if (es[k] || es[k + 1] || es[k + 2] != 1) continue;
    const uint8_t c = es[k + 3];
    if (c != 0x00 && c != 0xB3 && c != 0xB8 && c != 0xB7) continue;
    if (!dec.decode(es.data() + start, k - start)) {
      std::printf("error: %s\n", dec.error.c_str());
      return 1;
    }
    drain();
    start = k;
  }
  if (!dec.decode(es.data() + start, es.size() - start)) {
    std::printf("error: %s\n", dec.error.c_str());
    return 1;
  }
  dec.flush();
  drain();
  std::printf("%d pictures\n", shown);
  return 0;
}

// ---- The game's movie player, with the picture decoding done natively ----------------------
//
// The game (FUN_00776c60) sets up libmpeg on the sceMpeg at 0x0093AFC0 and a video ring at
// base+0x690B8 (base: the word at 0x009B97B0), which its demultiplexer fills with the video
// elementary stream (callback 0x00777218) and which libmpeg normally empties through the IPU's
// DMA (callbacks 0x00777950/0x00777990/0x007779C8/0x00777A00). Here the decoder thread's
// sceMpegGetPicture and sceMpegIsEnd are replaced: the stream is read from the ring directly,
// decoded by Mpeg2Decoder and the ring's sectors given back as libmpeg would, so the game's own
// reading, sound, display and skipping are untouched.
namespace {
constexpr uint32_t kBasePointer = 0x009B97B0, kRingOffset = 0x690B8, kSceMpeg = 0x0093AFC0;
constexpr uint32_t kGetPicture = 0x009107C0, kIsEnd = 0x009108E0, kMovieSetup = 0x00777A50, kYield = 0x00776C00;
constexpr uint32_t kMovieTeardown = 0x00776F60;  // after the movie (FUN_007767A0)

struct Ring {  // the game's video ring (FUN_00779628)
  uint32_t addr;
  Memory& m;
  uint32_t word(int i) const { return m.read<uint32_t>(addr + 4 * uint32_t(i)); }
  void setWord(int i, uint32_t v) { m.write<uint32_t>(addr + 4 * uint32_t(i), v); }
  uint32_t buffer() const { return word(0); }
  uint32_t sectors() const { return word(2); }
  uint32_t size() const { return word(6); }
  uint64_t written() const { return m.read<uint64_t>(addr + 0x48); }  // bytes put in since the start
  uint8_t at(uint64_t pos) const { return m.read<uint8_t>(buffer() + uint32_t(pos % size())); }
};

struct Player {
  Mpeg2Decoder dec;
  uint64_t consumed = 0;   // stream bytes taken from the ring
  uint64_t scanned = 0;    // where the search for the end of the next unit goes on from
  uint32_t shown = 0;      // pictures returned
  bool active = false;     // a movie was set up since the decoder last started
  bool failed = false;
  std::vector<uint8_t> unit;
  // A sceMpegGetPicture waiting for data (the thread yields, then calls again).
  bool waiting = false;
  uint32_t buffer = 0, ra = 0;
} gPlayer;

Cpu::RecompFn gSetup = nullptr, gGetPicture = nullptr, gIsEnd = nullptr, gTeardown = nullptr;

Ring ring(Cpu& c) { return Ring{c.mem.read<uint32_t>(kBasePointer) + kRingOffset, c.mem}; }

// Gives the ring back everything before the sector the reading position is in, keeping where the
// writer is: libmpeg's DMA callbacks keep [3] (first sector still held), [4] (sectors held) and
// [5] (bytes after them), which the demultiplexer's free-space test reads.
void release(Ring& r, uint64_t consumed) {
  const uint64_t written = r.written(), sectorStart = consumed / 0x800 * 0x800;
  const uint64_t held = written - sectorStart;
  r.setWord(3, uint32_t((sectorStart / 0x800) % r.sectors()));
  r.setWord(4, uint32_t(held / 0x800));
  r.setWord(5, uint32_t(held % 0x800));
}

// The timestamp queue (FUN_0077A330): the entry covering a picture's start is taken out.
void takeTimestamp(Ring& r, uint64_t pictureStart) {
  Memory& m = r.m;
  const uint32_t q = r.addr;
  const int32_t count = m.read<int32_t>(q + 0x58), capacity = m.read<int32_t>(q + 0x54), next = m.read<int32_t>(q + 0x5C);
  const uint32_t entries = m.read<uint32_t>(q + 0x50), size = r.size();
  if (count <= 0 || capacity <= 0 || size == 0) return;
  const uint32_t pos = uint32_t(pictureStart % size);
  for (int i = 0; i < count; ++i) {
    const uint32_t e = entries + uint32_t(((next - count + capacity + i) % capacity) * 0x18);
    const uint32_t offset = m.read<uint32_t>(e + 0x10), length = m.read<uint32_t>(e + 0x14);
    if ((pos + size - offset % size) % size < length) {
      m.write<uint64_t>(e, ~0ull), m.write<uint64_t>(e + 8, ~0ull);
      m.write<int32_t>(q + 0x58, count - 1);
      return;
    }
  }
}

// The next coded unit's bytes (sequence header, GOP, picture with its slices, or sequence end), if
// the ring holds all of it; false while its end has not arrived.
bool nextUnit(Ring& r, Player& p) {
  const uint64_t written = r.written();
  if (written - p.consumed < 4) return false;
  // Units start at a start code; anything before the first one is skipped.
  auto codeAt = [&](uint64_t pos) { return r.at(pos) == 0 && r.at(pos + 1) == 0 && r.at(pos + 2) == 1; };
  while (p.consumed + 4 <= written && !codeAt(p.consumed)) ++p.consumed;
  if (p.consumed + 4 > written) return false;
  const uint8_t first = r.at(p.consumed + 3);
  uint64_t end = 0;
  if (first == 0xB7) {
    end = p.consumed + 4;
  } else {
    for (uint64_t k = std::max(p.scanned, p.consumed + 4); k + 4 <= written; ++k) {
      if (!codeAt(k)) continue;
      const uint8_t c = r.at(k + 3);
      if (c == 0x00 || c == 0xB3 || c == 0xB8 || c == 0xB7) {
        end = k;
        break;
      }
    }
    if (!end) {
      p.scanned = written > 3 ? written - 3 : 0;
      return false;
    }
  }
  p.unit.resize(size_t(end - p.consumed));
  for (size_t k = 0; k < p.unit.size(); ++k) p.unit[k] = r.at(p.consumed + k);
  if (first == 0x00) takeTimestamp(r, p.consumed);
  p.consumed = end;
  p.scanned = end;
  return true;
}

// Each movie's setup: the option is read then, so a change in F1 applies from the next movie
// (CODPORT_NATIVE_MOVIES=1 turns it on for runs without the settings, such as replays).
uint32_t setup(Cpu& c, uint32_t entry) {
  gPlayer = Player{};
  gPlayer.active = settings().nativeMovies || std::getenv("CODPORT_NATIVE_MOVIES");
  // With the decoding this light the player's two threads mostly wait, yielding to each other.
  if (c.kernel) c.kernel->yieldsSkipIdle = gPlayer.active;
  return gSetup(c, entry);
}

uint32_t teardown(Cpu& c, uint32_t entry) {
  gPlayer.active = false;
  if (c.kernel) c.kernel->yieldsSkipIdle = false;
  return gTeardown(c, entry);
}

// sceMpegGetPicture(mp, rgb32 buffer, macroblocks): the next picture in display order.
uint32_t getPicture(Cpu& c, uint32_t entry) {
  Player& p = gPlayer;
  if (!p.active) return gGetPicture(c, entry);
  if (!p.waiting) p.buffer = uint32_t(c.s.gpr[5].ud[0]), p.ra = uint32_t(c.s.gpr[31].ud[0]);
  p.waiting = false;
  Ring r = ring(c);
  const bool free = c.kernel && c.kernel->semaphoreFree(int(r.word(0x10)));
  while (free && !p.failed && p.dec.ready.empty() && !p.dec.ended) {
    if (!nextUnit(r, p)) {
      // Not all of the next unit is there yet: let the demultiplexer run, then try again.
      release(r, p.consumed);
      p.waiting = true;
      c.s.gpr[31].ud[0] = rc::sx32(kGetPicture);
      return rc::leave(c, 0, kYield);
    }
    if (!p.dec.decode(p.unit.data(), p.unit.size())) {
      std::printf("native movie decoding failed: %s\n", p.dec.error.c_str());
      p.failed = true;
      break;
    }
  }
  if (!free && p.dec.ready.empty() && !p.failed) {  // the demultiplexer is in the ring: later
    p.waiting = true;
    c.s.gpr[31].ud[0] = rc::sx32(kGetPicture);
    return rc::leave(c, 0, kYield);
  }
  if (free) release(r, p.consumed);
  int32_t result = -1;
  if (!p.dec.ready.empty()) {
    if (uint8_t* out = c.mem.hostPtr(p.buffer, uint32_t(p.dec.width * p.dec.height * 4))) p.dec.toRgb32(p.dec.ready.front(), out);
    p.dec.ready.pop_front();
    c.mem.write<int32_t>(kSceMpeg + 0, p.dec.width);
    c.mem.write<int32_t>(kSceMpeg + 4, p.dec.height);
    c.mem.write<int32_t>(kSceMpeg + 8, int32_t(p.shown++));
    result = 1;
  }
  c.s.gpr[2].ud[0] = rc::sx32(uint32_t(result));
  return rc::leave(c, 0, p.ra);
}

// sceMpegIsEnd(mp): the sequence end was decoded and every picture shown.
uint32_t isEnd(Cpu& c, uint32_t entry) {
  Player& p = gPlayer;
  if (!p.active) return gIsEnd(c, entry);
  c.s.gpr[2].ud[0] = p.failed || (p.dec.ended && p.dec.ready.empty()) ? 1 : 0;
  return rc::leave(c, 0, uint32_t(c.s.gpr[31].ud[0]));
}
}  // namespace

void installNativeMovie(Cpu& cpu) {
  Cpu::RecompFn& s = recompSlot(cpu, kMovieSetup);
  Cpu::RecompFn& g = recompSlot(cpu, kGetPicture);
  Cpu::RecompFn& e = recompSlot(cpu, kIsEnd);
  Cpu::RecompFn& t = recompSlot(cpu, kMovieTeardown);
  if (!s || !g || !e || !t) return;
  gTeardown = t, t = teardown;
  gSetup = s, s = setup;
  gGetPicture = g, g = getPicture;
  gIsEnd = e, e = isEnd;
}
