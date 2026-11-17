#pragma once
// MPEG-2 pieces shared by the IPU (src/ipu/ipu.cpp) and the native movie decoder
// (src/ipu/mpeg2_decoder.cpp): the code tables, the inverse DCT and the YCbCr to RGB conversion,
// the same functions so that both make the same pictures.
#include <cstdint>

struct VlcEntry {
  int16_t value = 0;  // decoded value; for DCT tables the run (kEob / kEscape for those codes)
  int8_t level = 0;   // DCT tables: the level
  uint8_t len = 0;    // code length in bits, 0 for an invalid code
};
struct VlcTable {
  VlcEntry e[1 << 16];  // indexed by the next 16 bits of the stream
};

namespace mpeg2 {
constexpr int kEob = -1, kEscape = -2;

struct Tables {
  // Macroblock type flags: 1 intra, 2 pattern, 4 backward, 8 forward, 16 quant; mbType by
  // picture coding type (1 I, 2 P, 3 B).
  VlcTable *mbai, *mbType[4], *cbp, *motion, *dmv, *dcLuma, *dcChroma, *dct0, *dct1;
  Tables();
};
const Tables& tables();

extern const uint8_t kZigzag[64], kAlternate[64], kDefaultIntra[64], kNonLinearQ[32];

// 8x8 inverse DCT, results clamped to -256..255 (as the IPU outputs them).
void idct(const int16_t* in, int16_t* out);
// One macroblock of RAW8 (Y 16x16, Cb 8x8, Cr 8x8) to RGB32 with the IPU's alpha thresholds.
void cscMacroblock(const uint8_t* raw, uint32_t* rgba, uint32_t th0, uint32_t th1);
}  // namespace mpeg2
