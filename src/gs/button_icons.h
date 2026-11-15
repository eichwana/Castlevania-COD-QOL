#pragma once
#include <cstdint>
#include <vector>

// Xbox button icons in place of the PS2's (the game's prompts, "○ : Next"): drawn here, by code, in
// the 4-bit palette of the interface atlas holding them, a 256 x 256 PSMT4 texture the game uploads
// whenever it draws from it (at various places, as 128 x 128 16-bit pixels: recognised by the
// content's hash), whose cells of 24 x 24 pixels hold ○ △ □ (row from y 184, cells 7-9) and
// × L1 L2 R1 R2 (row from y 210, cells 0-4). cross becomes A, circle B, square X, triangle Y, L1 LB,
// L2 LT, R1 RB, R2 RT, as the port's controller defaults map them. Each row is written right after
// the atlas, as one host-to-local transfer.
struct ButtonIconPatch {
  uint32_t x, y, width, height;  // pixels, in the atlas
  std::vector<uint64_t> data;    // PSMT4: two pixels a byte, the first in the low nibble
};
const std::vector<ButtonIconPatch>& xboxButtonIcons();

// The atlas's upload (FNV-1a of its doublewords, in the order the game sends them; 4096 of them) and
// the texture it is read as: width in 64-pixel units, pixel format.
constexpr uint64_t kIconAtlasHash = 0xAF4B6C72097A54C7ull;
constexpr uint32_t kIconAtlasWords = 4096, kIconAtlasBw = 4, kIconAtlasPsm = 0x14;
constexpr uint32_t kIconRowsY = 184;
