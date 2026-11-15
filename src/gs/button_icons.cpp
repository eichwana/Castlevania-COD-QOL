#include "gs/button_icons.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// The atlas palette's entries (the prompts' palette): 0 black, grays from dark to light, and the
// colours of the PS2 symbols.
enum : uint8_t {
  kBlack = 0, kGray32 = 1, kGray55 = 2, kGray38 = 6, kGray72 = 4, kGray108 = 7, kGray128 = 9, kGray150 = 11, kGray191 = 13,
  kGray225 = 14, kWhite = 15,
};
constexpr int kCell = 24;
using Cell = uint8_t[kCell][kCell];

// Bold letters, 7 x 9, for the face buttons; 5 x 7 for the shoulder ones.
constexpr const char* kBig[4][9] = {
    {"..XXX..", ".XX.XX.", "XX...XX", "XX...XX", "XXXXXXX", "XX...XX", "XX...XX", "XX...XX", "XX...XX"},   // A
    {"XXXXXX.", "XX...XX", "XX...XX", "XXXXXX.", "XX...XX", "XX...XX", "XX...XX", "XX...XX", "XXXXXX."},   // B
    {"XX...XX", "XX...XX", ".XX.XX.", ".XX.XX.", "..XXX..", ".XX.XX.", ".XX.XX.", "XX...XX", "XX...XX"},   // X
    {"XX...XX", "XX...XX", ".XX.XX.", ".XX.XX.", "..XXX..", "..XXX..", "..XXX..", "..XXX..", "..XXX.."},   // Y
};
const char* const* smallGlyph(char c) {
  static constexpr const char* L[7] = {"XX...", "XX...", "XX...", "XX...", "XX...", "XX...", "XXXXX"};
  static constexpr const char* B[7] = {"XXXX.", "XX.XX", "XX.XX", "XXXX.", "XX.XX", "XX.XX", "XXXX."};
  static constexpr const char* R[7] = {"XXXX.", "XX.XX", "XX.XX", "XXXX.", "XXX..", "XX.X.", "XX.XX"};
  static constexpr const char* T[7] = {"XXXXX", ".XXX.", ".XXX.", ".XXX.", ".XXX.", ".XXX.", ".XXX."};
  return c == 'L' ? L : c == 'B' ? B : c == 'R' ? R : T;
}

// A face button: a shaded ball lit from the top left, a dark rim, and its letter outlined.
void faceButton(Cell& c, int letter, uint8_t colour) {
  static constexpr uint8_t kRamp[] = {kGray32, kGray38, kGray55, kGray72, kGray108, kGray128, kGray150, kGray191};
  const double cx = 12.0, cy = 11.5, radius = 9.6;
  for (int y = 0; y < kCell; ++y)
    for (int x = 0; x < kCell; ++x) {
      const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = std::hypot(dx, dy);
      if (r > radius + 1.0) c[y][x] = kBlack;
      else if (r > radius) c[y][x] = kGray32;
      else {
        const double nx = dx / radius, ny = dy / radius, nz = std::sqrt(std::max(0.0, 1 - nx * nx - ny * ny));
        const double light = std::clamp(-0.45 * nx - 0.55 * ny + 0.70 * nz, 0.0, 1.0);
        c[y][x] = kRamp[std::min(int(light * 8), 7)];
      }
    }
  const int x0 = 9, y0 = 7;  // the letter's top left, centred on the ball
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 7; ++x)
      if (kBig[letter][y][x] == 'X')
        for (int oy = -1; oy <= 1; ++oy)
          for (int ox = -1; ox <= 1; ++ox) {
            uint8_t& p = c[y0 + y + oy][x0 + x + ox];
            const bool inside = y + oy >= 0 && y + oy < 9 && x + ox >= 0 && x + ox < 7 && kBig[letter][y + oy][x + ox] == 'X';
            if (!inside) p = kGray32;  // outline
          }
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 7; ++x)
      if (kBig[letter][y][x] == 'X') c[y0 + y][x0 + x] = colour;
}

// A shoulder button: a rounded grey key as the game's L1/R1, with its name (LB, LT, RB, RT).
void shoulderButton(Cell& c, char a, char b) {
  std::memset(c, kBlack, sizeof(Cell));
  const int left = 3, right = 22, top = 4, bottom = 19;
  for (int y = top; y <= bottom; ++y)
    for (int x = left; x <= right; ++x) {
      const bool corner = (x == left || x == right) && (y == top || y == bottom);
      if (corner) continue;
      const bool edge = x == left || x == right || y == bottom;
      const int depth = (y - top) * 4 / (bottom - top + 1);  // lighter at the top
      static constexpr uint8_t kBody[4] = {kGray150, kGray128, kGray108, kGray72};
      c[y][x] = edge ? kGray38 : y == top ? kGray191 : kBody[depth];
    }
  const char* const* ga = smallGlyph(a);
  const char* const* gb = smallGlyph(b);
  const int x0 = 6, y0 = 8;
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 5; ++x) {
      if (ga[y][x] == 'X') c[y0 + y][x0 + x] = kGray32;
      if (gb[y][x] == 'X') c[y0 + y][x0 + 7 + x] = kGray32;
    }
}

ButtonIconPatch row(uint32_t x, uint32_t y, const std::vector<const Cell*>& cells) {
  ButtonIconPatch p{x, y, uint32_t(cells.size()) * kCell, kCell, {}};
  std::vector<uint8_t> pixels(size_t(p.width) * p.height);
  for (size_t i = 0; i < cells.size(); ++i)
    for (int yy = 0; yy < kCell; ++yy)
      for (int xx = 0; xx < kCell; ++xx) pixels[size_t(yy) * p.width + i * kCell + size_t(xx)] = (*cells[i])[yy][xx];
  p.data.assign(pixels.size() / 16, 0);
  for (size_t i = 0; i < pixels.size(); ++i) p.data[i / 16] |= uint64_t(pixels[i] & 15) << (4 * (i % 16));
  return p;
}
}  // namespace

const std::vector<ButtonIconPatch>& xboxButtonIcons() {
  static const std::vector<ButtonIconPatch> patches = [] {
    static Cell b, y, x, a, lb, lt, rb, rt;
    // White letters: each icon is drawn with a palette of its own (its symbol's PS2 colour), which
    // shares only the greys with the others, so Xbox colours cannot be had everywhere.
    faceButton(b, 1, kWhite);  // circle
    faceButton(y, 3, kWhite);  // triangle
    faceButton(x, 2, kWhite);  // square
    faceButton(a, 0, kWhite);  // cross
    shoulderButton(lb, 'L', 'B'), shoulderButton(lt, 'L', 'T'), shoulderButton(rb, 'R', 'B'), shoulderButton(rt, 'R', 'T');
    return std::vector<ButtonIconPatch>{row(7 * kCell, kIconRowsY, {&b, &y, &x}), row(0, 210, {&a, &lb, &lt, &rb, &rt})};
  }();
  return patches;
}
