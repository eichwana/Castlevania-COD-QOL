#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Text the port shows over the picture (not the game's): the speedrun timer under the minimap,
// notices (achievements) at the bottom in the middle and, at the top right, that the game's code
// is still being built (with how far). Written as pictures here, with the system's font (accents
// and all), and drawn by the GPU renderer over the window.
enum class OverlaySlot { Timer, Notice, Build, Count };

// Sets a slot's text, on any thread; empty hides it. `alpha` fades it (0-1); `progress` (0-1) adds a
// bar under the text filled that far (below 0: none). A new picture is made only when the text,
// the bar (in steps of 0.5%) or the window's height changes.
void setOverlayText(OverlaySlot slot, const std::wstring& text, float alpha = 1, float progress = -1);

struct OverlayImage {
  int width = 0, height = 0;
  std::vector<uint32_t> rgba;  // straight alpha, red in the low byte
  uint64_t version = 0;        // changes with the picture
  float alpha = 0;             // 0: nothing to draw
};
// For the renderer: the slot's state, with its picture copied only if it is newer than `have`.
OverlayImage overlayImage(OverlaySlot slot, uint64_t have);
