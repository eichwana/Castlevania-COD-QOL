#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Text the port shows over the picture (not the game's): the speedrun timer under the minimap
// and notices (achievements) at the bottom in the middle. Written as pictures here, with the
// system's font (accents and all), and drawn by the GPU renderer over the window.
enum class OverlaySlot { Timer, Notice, Count };

// Sets a slot's text, on any thread; empty hides it. `alpha` fades it (0-1). A new picture is made
// only when the text or the window's height changes.
void setOverlayText(OverlaySlot slot, const std::wstring& text, float alpha = 1);

struct OverlayImage {
  int width = 0, height = 0;
  std::vector<uint32_t> rgba;  // straight alpha, red in the low byte
  uint64_t version = 0;        // changes with the picture
  float alpha = 0;             // 0: nothing to draw
};
// For the renderer: the slot's state, with its picture copied only if it is newer than `have`.
OverlayImage overlayImage(OverlaySlot slot, uint64_t have);
