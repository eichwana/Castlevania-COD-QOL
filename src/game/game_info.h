#pragma once

#include <cstdint>

#include "ee/memory.h"

// What the port reads of the game's state for its own displays (the speedrun timer, achievements,
// Discord): where Hector is and how far along he is. Nothing is written, so replays are not affected.
struct GameInfo {
  bool inGame = false;   // a game started or loaded (past the title's menu), with Hector's data in place
  bool loading = false;  // between places (src/game/loading.h)
  int area = -1, room = -1;
  int level = 0, exp = 0, hp = 0, hpMax = 0, gold = 0;
};

GameInfo readGameInfo(Memory& mem);

// The area's name as the game writes it (its English names, also in its other languages' menus
// for places), UTF-8; empty for an unknown area. Areas 6, 7 and 12 are all Dracula's Castle.
const char* areaName(int area);

// The areas in the order the story visits them (the Select screen's order, a table in the game).
// Index of an area in that order, or -1 for the side towers and unknown areas.
int storyIndex(int area);
bool isDraculasCastle(int area);
