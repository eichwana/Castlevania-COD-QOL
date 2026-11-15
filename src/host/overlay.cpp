#include "host/overlay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

#include "host/window.h"

namespace {
struct Slot {
  std::wstring text;
  int forHeight = 0;
  OverlayImage image;
};
std::mutex gMutex;
std::array<Slot, size_t(OverlaySlot::Count)> gSlots;
uint64_t gVersions = 0;

// The text, white on a dark rounded box, its letters smoothed by GDI on black (their coverage is
// the red channel's value).
void render(OverlaySlot which, const std::wstring& text, int windowHeight, OverlayImage& out) {
  const int px = std::max(14, windowHeight / (which == OverlaySlot::Timer ? 26 : 30));
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT font = CreateFontW(-px, 0, 0, 0, which == OverlaySlot::Timer ? FW_BOLD : FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                           which == OverlaySlot::Timer ? L"Consolas" : L"Segoe UI");
  HGDIOBJ oldFont = SelectObject(dc, font);
  RECT measure{0, 0, 0, 0};
  DrawTextW(dc, text.c_str(), int(text.size()), &measure, DT_CALCRECT | DT_NOPREFIX);
  const int padX = px * 2 / 3, padY = px / 3;
  const int w = measure.right + padX * 2, h = measure.bottom + padY * 2;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ oldBmp = SelectObject(dc, bmp);
  RECT all{0, 0, w, h};
  FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  RECT at{padX, padY, w - padX, h - padY};
  DrawTextW(dc, text.c_str(), int(text.size()), &at, DT_NOPREFIX);
  GdiFlush();
  out.width = w, out.height = h;
  out.rgba.assign(size_t(w) * size_t(h), 0);
  const uint32_t* src = static_cast<const uint32_t*>(bits);
  const int radius = std::max(2, px / 3);
  // Text colour: the timer a warm white, notices gold like the game's own trims.
  const float tr = 1.0f, tg = which == OverlaySlot::Timer ? 0.97f : 0.86f,
              tb = which == OverlaySlot::Timer ? 0.9f : 0.45f;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      // The box: 60% black, its corners rounded.
      const int cx = x < radius ? radius - x : x >= w - radius ? x - (w - radius - 1) : 0;
      const int cy = y < radius ? radius - y : y >= h - radius ? y - (h - radius - 1) : 0;
      const float inside = std::clamp(float(radius) + 0.5f - std::sqrt(float(cx * cx + cy * cy)), 0.0f, 1.0f);
      const float box = (cx && cy ? inside : 1.0f) * 0.62f;
      const float cover = float(src[size_t(y) * size_t(w) + size_t(x)] & 0xFF) / 255.0f;
      const float a = cover + box * (1 - cover);
      if (a <= 0) continue;
      const float r = tr * cover / a, g = tg * cover / a, b = tb * cover / a;  // the box is black
      out.rgba[size_t(y) * size_t(w) + size_t(x)] = uint32_t(r * 255 + 0.5f) | uint32_t(g * 255 + 0.5f) << 8 | uint32_t(b * 255 + 0.5f) << 16 |
                                                    uint32_t(a * 255 + 0.5f) << 24;
    }
  SelectObject(dc, oldBmp);
  SelectObject(dc, oldFont);
  DeleteObject(bmp);
  DeleteObject(font);
  DeleteDC(dc);
}
}  // namespace

void setOverlayText(OverlaySlot which, const std::wstring& text, float alpha) {
  const int height = windowClientHeight();
  std::lock_guard lock(gMutex);
  Slot& s = gSlots[size_t(which)];
  s.image.alpha = text.empty() ? 0 : std::clamp(alpha, 0.0f, 1.0f);
  if (text.empty() || (text == s.text && height == s.forHeight)) return;
  s.text = text, s.forHeight = height;
  render(which, text, height, s.image);
  s.image.version = ++gVersions;
}

OverlayImage overlayImage(OverlaySlot which, uint64_t have) {
  std::lock_guard lock(gMutex);
  const Slot& s = gSlots[size_t(which)];
  OverlayImage out;
  out.width = s.image.width, out.height = s.image.height, out.version = s.image.version, out.alpha = s.image.alpha;
  if (s.image.version != have) out.rgba = s.image.rgba;
  return out;
}
