#pragma once
// A native MPEG-2 video decoder for the game's movies (progressive 4:2:0 frame pictures, the
// kind on the disc), making the same pictures as the game's libmpeg driving the IPU: the same
// dequantisation, inverse DCT and colour conversion (src/ipu/mpeg2.h), standard motion
// compensation. Used by the native movie player (src/game/native_movie.cpp), which feeds it the
// elementary stream one coded unit at a time and takes finished pictures in display order.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

class Mpeg2Decoder {
public:
  struct Frame {
    std::vector<uint8_t> y, cb, cr;  // planar, the picture's size (chroma half in each direction)
    int type = 0;                    // 1 I, 2 P, 3 B
  };

  // One coded unit, from its start code up to (not including) the next unit's: a sequence header
  // (with its extensions), a GOP header, a picture (header, extensions and all its slices), or the
  // sequence end. Finished pictures go to `ready` in display order. False (and `error`) on a
  // stream it does not support or that is broken.
  bool decode(const uint8_t* data, size_t size);
  // The end of the stream: the last reference picture is shown.
  void flush();

  // The picture in libmpeg's output layout: 16x16 RGB32 macroblocks, column by column.
  void toRgb32(const Frame& f, uint8_t* out) const;

  int width = 0, height = 0;
  std::deque<Frame> ready;
  std::string error;
  bool ended = false;  // the sequence end was decoded

private:
  struct Bits {
    const uint8_t* p;
    size_t size;  // bytes
    size_t pos = 0;  // bits
    uint32_t peek(int n) const;
    uint32_t get(int n) { const uint32_t v = peek(n); pos += size_t(n); return v; }
    bool more() const { return pos < size * 8; }
  };
  bool sequenceHeader(Bits& b);
  bool extension(Bits& b);
  bool pictureHeader(Bits& b);
  bool slice(Bits& b, int row);
  int vlc(Bits& b, const struct VlcTable& t);
  bool block(Bits& b, int16_t* coef, int index, bool intra, int qscale);
  bool motionVector(Bits& b, int s, int& mvx, int& mvy);
  // A macroblock's prediction (16x16, 8x8, 8x8) from one reference picture.
  void predict(uint8_t* y, uint8_t* cb, uint8_t* cr, const Frame& ref, int mbx, int mby, int mvx, int mvy) const;
  void finishPicture();

  uint8_t intraQ_[64] = {}, nonIntraQ_[64] = {};  // natural order
  bool haveSequence_ = false;
  // The current picture.
  int type_ = 0, fcode_[2][2] = {}, dcPrecision_ = 0, structure_ = 3;
  bool framePredDct_ = true, concealment_ = false, qScaleType_ = false, intraVlc_ = false, altScan_ = false;
  bool havePicture_ = false;
  Frame cur_;
  // References: the older (forward) and the newer (backward; shown when the next I/P arrives).
  Frame fwd_, bwd_;
  bool haveFwd_ = false, haveBwd_ = false;
  // Per slice / macroblock state.
  int dcPred_[3] = {}, pmv_[2][2] = {};
};
