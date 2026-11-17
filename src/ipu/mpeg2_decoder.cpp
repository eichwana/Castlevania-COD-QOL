#include "ipu/mpeg2_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

#include "ipu/mpeg2.h"

using namespace mpeg2;

namespace {
constexpr int kStuffing = 0x22, kEscapeMba = 0x23;  // macroblock_address_increment table values
inline uint8_t clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }
}  // namespace

uint32_t Mpeg2Decoder::Bits::peek(int n) const {
  if (n == 0) return 0;
  uint64_t acc = 0;
  const size_t byte = pos / 8;
  for (size_t i = 0; i < 5; ++i) acc = acc << 8 | (byte + i < size ? p[byte + i] : 0);
  return uint32_t(acc >> (40 - (pos % 8) - size_t(n))) & (n == 32 ? 0xFFFFFFFFu : (1u << n) - 1);
}

int Mpeg2Decoder::vlc(Bits& b, const VlcTable& t) {
  const VlcEntry& e = t.e[b.peek(16)];
  if (!e.len) return INT32_MIN;
  b.pos += e.len;
  return e.value;
}

bool Mpeg2Decoder::decode(const uint8_t* data, size_t size) {
  // Every start code in the chunk, in order.
  size_t i = 0;
  auto nextStart = [&](size_t from) {
    for (size_t k = from; k + 3 < size; ++k)
      if (data[k] == 0 && data[k + 1] == 0 && data[k + 2] == 1) return k;
    return size;
  };
  for (i = nextStart(0); i < size;) {
    const size_t end = nextStart(i + 4);
    const uint8_t code = data[i + 3];
    Bits b{data + i + 4, end - (i + 4)};
    bool ok = true;
    if (code == 0xB3) {
      ok = sequenceHeader(b);
    } else if (code == 0xB5) {
      ok = extension(b);
    } else if (code == 0x00) {
      if (havePicture_) finishPicture();
      ok = pictureHeader(b);
    } else if (code >= 0x01 && code <= 0xAF) {
      ok = havePicture_ && slice(b, code - 1);
      if (!havePicture_ && error.empty()) error = "slice before a picture";
    } else if (code == 0xB7) {
      if (havePicture_) finishPicture();
      ended = true;
      flush();
    }  // GOP (B8), user data (B2) and the rest: nothing to do
    if (!ok) return false;
    i = end;
  }
  if (havePicture_) finishPicture();
  return true;
}

bool Mpeg2Decoder::sequenceHeader(Bits& b) {
  width = int(b.get(12));
  height = int(b.get(12));
  b.get(4), b.get(4), b.get(18), b.get(1), b.get(10), b.get(1);  // aspect, rate, bit rate, marker, VBV, constrained
  if (b.get(1)) {
    for (int k = 0; k < 64; ++k) intraQ_[kZigzag[k]] = uint8_t(b.get(8));
  } else {
    std::memcpy(intraQ_, kDefaultIntra, 64);
  }
  if (b.get(1)) {
    for (int k = 0; k < 64; ++k) nonIntraQ_[kZigzag[k]] = uint8_t(b.get(8));
  } else {
    std::memset(nonIntraQ_, 16, 64);
  }
  if (width <= 0 || height <= 0 || width % 16 || height % 16 || width > 1024 || height > 1024) {
    error = std::format("unsupported picture size {}x{}", width, height);
    return false;
  }
  haveSequence_ = true;
  return true;
}

bool Mpeg2Decoder::extension(Bits& b) {
  switch (b.get(4)) {
    case 1: {  // sequence extension
      b.get(8);                    // profile and level
      const bool progressive = b.get(1);
      if (b.get(2) != 1) {  // chroma format
        error = "only 4:2:0 movies are supported";
        return false;
      }
      if (b.get(2) || b.get(2)) {  // size extensions
        error = "unsupported picture size extension";
        return false;
      }
      (void)progressive;
      return true;
    }
    case 3:  // quant matrix extension
      if (b.get(1))
        for (int k = 0; k < 64; ++k) intraQ_[kZigzag[k]] = uint8_t(b.get(8));
      if (b.get(1))
        for (int k = 0; k < 64; ++k) nonIntraQ_[kZigzag[k]] = uint8_t(b.get(8));
      return true;
    case 8:  // picture coding extension
      fcode_[0][0] = int(b.get(4)), fcode_[0][1] = int(b.get(4));
      fcode_[1][0] = int(b.get(4)), fcode_[1][1] = int(b.get(4));
      dcPrecision_ = int(b.get(2));
      structure_ = int(b.get(2));
      b.get(1);  // top field first
      framePredDct_ = b.get(1);
      concealment_ = b.get(1);
      qScaleType_ = b.get(1);
      intraVlc_ = b.get(1);
      altScan_ = b.get(1);
      if (structure_ != 3) {
        error = "field pictures are not supported";
        return false;
      }
      return true;
    default:
      return true;
  }
}

bool Mpeg2Decoder::pictureHeader(Bits& b) {
  if (!haveSequence_) {
    error = "picture before a sequence header";
    return false;
  }
  b.get(10);  // temporal reference
  type_ = int(b.get(3));
  b.get(16);  // VBV delay
  if (type_ < 1 || type_ > 3) {
    error = std::format("unsupported picture type {}", type_);
    return false;
  }
  if ((type_ == 2 || type_ == 3) && (!haveBwd_ || (type_ == 3 && !haveFwd_))) {
    // A P or B picture without its references (a stream starting in the middle): skipped.
    havePicture_ = false;
    return true;
  }
  // MPEG-2 defaults until the picture coding extension says otherwise.
  structure_ = 3, framePredDct_ = true, concealment_ = false, qScaleType_ = false, intraVlc_ = false, altScan_ = false;
  dcPrecision_ = 0;
  cur_.type = type_;
  cur_.y.assign(size_t(width) * size_t(height), 0);
  cur_.cb.assign(size_t(width / 2) * size_t(height / 2), 128);
  cur_.cr.assign(size_t(width / 2) * size_t(height / 2), 128);
  havePicture_ = true;
  return true;
}

// One block's coefficients, dequantised, in natural order: the IPU's BDEC (Ipu::decodeBlock).
bool Mpeg2Decoder::block(Bits& b, int16_t* coef, int index, bool intra, int qscale) {
  const Tables& tb = tables();
  std::memset(coef, 0, 64 * sizeof(int16_t));
  const uint8_t* scan = altScan_ ? kAlternate : kZigzag;
  const uint8_t* w = intra ? intraQ_ : nonIntraQ_;
  int i = 0, sum = 0;
  if (intra) {
    const int cc = index < 4 ? 0 : index - 3;
    const int size = vlc(b, cc ? *tb.dcChroma : *tb.dcLuma);
    if (size == INT32_MIN) return false;
    int diff = 0;
    if (size) {
      diff = int(b.get(size));
      if (diff < (1 << (size - 1))) diff -= (1 << size) - 1;
    }
    dcPred_[cc] += diff;
    coef[0] = int16_t(dcPred_[cc] << (3 - dcPrecision_));
    sum = coef[0];
    i = 1;
  }
  const VlcTable& table = intra && intraVlc_ ? *tb.dct1 : *tb.dct0;
  for (bool first = !intra;; first = false) {
    int run, level;
    if (first && b.peek(1)) {  // "1s": run 0, level 1 as the first coefficient of a non-intra block
      b.pos += 1;
      run = 0;
      level = b.get(1) ? -1 : 1;
    } else {
      const VlcEntry& e = table.e[b.peek(16)];
      if (!e.len) return false;
      b.pos += e.len;
      if (e.value == kEob) break;
      if (e.value == kEscape) {
        run = int(b.get(6));
        level = int(int32_t(b.get(12) << 20) >> 20);
        if (level == 0 || level == -2048) return false;
      } else {
        run = e.value;
        level = b.get(1) ? -e.level : e.level;
      }
    }
    i += run;
    if (i > 63) return false;
    const int pos = scan[i++];
    const int mag = std::abs(level);
    int val = intra ? (mag * qscale * w[pos]) / 16 : ((2 * mag + 1) * qscale * w[pos]) / 32;
    val = std::min(level < 0 ? -val : val, 2047);
    val = std::max(val, -2048);
    coef[pos] = int16_t(val);
    sum += val;
  }
  if ((sum & 1) == 0) coef[63] ^= 1;  // mismatch control
  return true;
}

// A frame motion vector of a frame picture (7.6.3), predicted from and stored in pmv_[s].
bool Mpeg2Decoder::motionVector(Bits& b, int s, int& mvx, int& mvy) {
  int* out[2] = {&mvx, &mvy};
  for (int t = 0; t < 2; ++t) {
    int code = vlc(b, *tables().motion);
    if (code == INT32_MIN) return false;
    if (code && b.get(1)) code = -code;
    const int rSize = fcode_[s][t] - 1;
    if (rSize < 0 || rSize > 8) return false;
    int delta = code;
    if (rSize > 0 && code != 0) {
      const int residual = int(b.get(rSize));
      delta = ((std::abs(code) - 1) << rSize) + residual + 1;
      if (code < 0) delta = -delta;
    }
    const int f = 1 << rSize, low = -16 * f, high = 16 * f - 1, range = 32 * f;
    int v = pmv_[s][t] + delta;
    if (v < low) v += range;
    if (v > high) v -= range;
    pmv_[s][t] = v;
    *out[t] = v;
  }
  return true;
}

namespace {
// A block of the prediction from one reference plane, half-pel by the vector's low bit (7.6.4).
void predictPlane(const uint8_t* ref, int w, int h, int x0, int y0, int bw, int bh, int mvx, int mvy, uint8_t* out, int stride) {
  const int ix = x0 + (mvx >> 1), iy = y0 + (mvy >> 1), hx = mvx & 1, hy = mvy & 1;
  auto at = [&](int x, int y) { return int(ref[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]); };
  for (int y = 0; y < bh; ++y)
    for (int x = 0; x < bw; ++x) {
      const int px = ix + x, py = iy + y;
      int v;
      if (!hx && !hy) v = at(px, py);
      else if (hx && !hy) v = (at(px, py) + at(px + 1, py) + 1) >> 1;
      else if (!hx && hy) v = (at(px, py) + at(px, py + 1) + 1) >> 1;
      else v = (at(px, py) + at(px + 1, py) + at(px, py + 1) + at(px + 1, py + 1) + 2) >> 2;
      out[y * stride + x] = uint8_t(v);
    }
}
}  // namespace

void Mpeg2Decoder::predict(uint8_t* y, uint8_t* cb, uint8_t* cr, const Frame& ref, int mbx, int mby, int mvx, int mvy) const {
  predictPlane(ref.y.data(), width, height, mbx * 16, mby * 16, 16, 16, mvx, mvy, y, 16);
  // Chroma vectors: half the luma ones, truncated toward zero (7.6.3.7).
  const int cx = mvx / 2, cy = mvy / 2;
  predictPlane(ref.cb.data(), width / 2, height / 2, mbx * 8, mby * 8, 8, 8, cx, cy, cb, 8);
  predictPlane(ref.cr.data(), width / 2, height / 2, mbx * 8, mby * 8, 8, 8, cx, cy, cr, 8);
}

bool Mpeg2Decoder::slice(Bits& b, int row) {
  const Tables& tb = tables();
  const int mbw = width / 16, mbh = height / 16;
  if (row >= mbh) {
    error = std::format("slice row {} below the picture", row);
    return false;
  }
  int qcode = int(b.get(5));
  while (b.peek(1)) b.get(1), b.get(8);  // intra_slice_flag and extra information
  b.get(1);
  auto resetDc = [&] { dcPred_[0] = dcPred_[1] = dcPred_[2] = 128 << dcPrecision_; };
  resetDc();
  std::memset(pmv_, 0, sizeof pmv_);
  int addr = row * mbw - 1;
  bool first = true;
  int prevFlags = 0, prevMv[2][2] = {};  // for skipped macroblocks in B pictures
  auto fail = [&](const char* what) {
    if (error.empty()) error = std::format("{} at macroblock {} (row {})", what, addr, row);
    return false;
  };
  // Prediction of a macroblock into y/cb/cr (16x16, 8x8, 8x8) from the directions in `flags`.
  auto prediction = [&](int flags, const int mv[2][2], int mbx, int mby, uint8_t* y, uint8_t* cb, uint8_t* cr) {
    const bool f = flags & 8, bk = flags & 4;
    if (type_ == 2) {  // P: forward always (zero vector without motion)
      predict(y, cb, cr, bwd_, mbx, mby, f ? mv[0][0] : 0, f ? mv[0][1] : 0);
    } else if (f && bk) {  // both: the average of the two, rounded up (7.6.7.1)
      uint8_t y2[256], cb2[64], cr2[64];
      predict(y, cb, cr, fwd_, mbx, mby, mv[0][0], mv[0][1]);
      predict(y2, cb2, cr2, bwd_, mbx, mby, mv[1][0], mv[1][1]);
      for (int k = 0; k < 256; ++k) y[k] = uint8_t((y[k] + y2[k] + 1) >> 1);
      for (int k = 0; k < 64; ++k) cb[k] = uint8_t((cb[k] + cb2[k] + 1) >> 1), cr[k] = uint8_t((cr[k] + cr2[k] + 1) >> 1);
    } else if (bk) {
      predict(y, cb, cr, bwd_, mbx, mby, mv[1][0], mv[1][1]);
    } else {
      predict(y, cb, cr, fwd_, mbx, mby, mv[0][0], mv[0][1]);
    }
  };
  auto store = [&](int mbx, int mby, const uint8_t* y, const uint8_t* cb, const uint8_t* cr) {
    for (int r = 0; r < 16; ++r) std::memcpy(&cur_.y[size_t(mby * 16 + r) * size_t(width) + size_t(mbx * 16)], y + r * 16, 16);
    for (int r = 0; r < 8; ++r) {
      std::memcpy(&cur_.cb[size_t(mby * 8 + r) * size_t(width / 2) + size_t(mbx * 8)], cb + r * 8, 8);
      std::memcpy(&cur_.cr[size_t(mby * 8 + r) * size_t(width / 2) + size_t(mbx * 8)], cr + r * 8, 8);
    }
  };
  for (;;) {
    int inc = 0;
    for (;;) {
      const int v = vlc(b, *tb.mbai);
      if (v == INT32_MIN) return fail("bad macroblock address");
      if (v == kStuffing) continue;
      if (v == kEscapeMba) {
        inc += 33;
        continue;
      }
      inc += v;
      break;
    }
    if (first) {
      addr += inc;
    } else {
      // Skipped macroblocks: the prediction alone (P: zero forward vector; B: the previous one's).
      for (int k = 1; k < inc; ++k) {
        const int a = addr + k;
        if (a >= mbw * mbh) return fail("skipped past the picture");
        uint8_t y[256], cb[64], cr[64];
        if (type_ == 2) {
          std::memset(pmv_, 0, sizeof pmv_);
          const int zero[2][2] = {};
          prediction(8, zero, a % mbw, a / mbw, y, cb, cr);
        } else if (type_ == 3) {
          if (prevFlags & 1) return fail("skipped macroblock after an intra one");
          prediction(prevFlags, prevMv, a % mbw, a / mbw, y, cb, cr);
        } else {
          return fail("skipped macroblock in an I picture");
        }
        store(a % mbw, a / mbw, y, cb, cr);
      }
      if (inc > 1) resetDc();
      addr += inc;
    }
    first = false;
    if (addr >= mbw * mbh) return fail("macroblock past the picture");
    const int mbx = addr % mbw, mby = addr / mbw;

    const int flags = vlc(b, *tb.mbType[type_]);
    if (flags == INT32_MIN) return fail("bad macroblock type");
    const bool intra = flags & 1, quant = flags & 16, fwd = flags & 8, bwd = flags & 4, pattern = flags & 2;
    if ((fwd || bwd) && !framePredDct_ && b.get(2) != 2) return fail("only frame motion is supported");
    bool fieldDct = false;
    if (!framePredDct_ && (intra || pattern)) fieldDct = b.get(1);
    if (quant) qcode = int(b.get(5));
    const int qscale = qScaleType_ ? kNonLinearQ[qcode] : qcode * 2;
    int mv[2][2] = {};
    if (fwd || (intra && concealment_))
      if (!motionVector(b, 0, mv[0][0], mv[0][1])) return fail("bad forward vector");
    if (bwd)
      if (!motionVector(b, 1, mv[1][0], mv[1][1])) return fail("bad backward vector");
    if (intra && concealment_) b.get(1);
    int cbp = 0x3F;
    if (!intra) {
      cbp = pattern ? vlc(b, *tb.cbp) : 0;
      if (cbp == INT32_MIN) return fail("bad coded block pattern");
    }
    // Residuals (or pixels, for intra), as the IPU's BDEC makes them.
    int16_t mb[384] = {};
    for (int k = 0; k < 6; ++k) {
      if (!(cbp & (0x20 >> k))) continue;
      int16_t coef[64], pix[64];
      if (!block(b, coef, k, intra, qscale)) return fail("bad block");
      idct(coef, pix);
      for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
          int16_t* dst;
          if (k < 4) {
            const int r = fieldDct ? 2 * y + (k >> 1) : y + 8 * (k >> 1);
            dst = &mb[r * 16 + 8 * (k & 1) + x];
          } else {
            dst = &mb[256 + 64 * (k - 4) + y * 8 + x];
          }
          *dst = pix[y * 8 + x];
        }
    }
    uint8_t y[256], cb[64], cr[64];
    if (intra) {
      for (int k = 0; k < 256; ++k) y[k] = clamp8(mb[k]);
      for (int k = 0; k < 64; ++k) cb[k] = clamp8(mb[256 + k]), cr[k] = clamp8(mb[320 + k]);
      if (!concealment_) std::memset(pmv_, 0, sizeof pmv_);
    } else {
      resetDc();
      if (type_ == 2 && !fwd) std::memset(pmv_, 0, sizeof pmv_);
      prediction(flags, mv, mbx, mby, y, cb, cr);
      for (int k = 0; k < 256; ++k) y[k] = clamp8(y[k] + mb[k]);
      for (int k = 0; k < 64; ++k) cb[k] = clamp8(cb[k] + mb[256 + k]), cr[k] = clamp8(cr[k] + mb[320 + k]);
    }
    store(mbx, mby, y, cb, cr);
    prevFlags = flags;
    std::memcpy(prevMv, mv, sizeof mv);
    // The slice ends where only zero bits are left before the next start code (the data's end).
    const size_t left = b.size * 8 > b.pos ? b.size * 8 - b.pos : 0;
    if (left == 0 || b.peek(int(std::min<size_t>(left, 23))) == 0) break;
  }
  return true;
}

void Mpeg2Decoder::finishPicture() {
  havePicture_ = false;
  if (type_ == 3) {
    ready.push_back(std::move(cur_));
    return;
  }
  // I or P: the newer reference so far is shown now, and this one becomes the newer.
  if (haveBwd_) {
    ready.push_back(bwd_);
    fwd_ = std::move(bwd_);
    haveFwd_ = true;
  }
  bwd_ = std::move(cur_);
  haveBwd_ = true;
}

void Mpeg2Decoder::flush() {
  if (haveBwd_) ready.push_back(bwd_);
  haveBwd_ = haveFwd_ = false;
}

void Mpeg2Decoder::toRgb32(const Frame& f, uint8_t* out) const {
  const int mbw = width / 16, mbh = height / 16;
  for (int col = 0; col < mbw; ++col)
    for (int row = 0; row < mbh; ++row) {
      uint8_t raw[384];
      for (int r = 0; r < 16; ++r) std::memcpy(raw + r * 16, &f.y[size_t(row * 16 + r) * size_t(width) + size_t(col * 16)], 16);
      for (int r = 0; r < 8; ++r) {
        std::memcpy(raw + 256 + r * 8, &f.cb[size_t(row * 8 + r) * size_t(width / 2) + size_t(col * 8)], 8);
        std::memcpy(raw + 320 + r * 8, &f.cr[size_t(row * 8 + r) * size_t(width / 2) + size_t(col * 8)], 8);
      }
      cscMacroblock(raw, reinterpret_cast<uint32_t*>(out + size_t(col * mbh + row) * 1024), 0, 0);
    }
}
