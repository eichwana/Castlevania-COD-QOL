#include "game/game_info.h"

#include <array>

#include "game/loading.h"

namespace {
// Found by comparing memory dumps of recorded sessions with the status screen (LV, EXP, HP, Gold):
// the player's level and experience, then max HP, max MP (hearts) and HP.
constexpr uint32_t kLevel = 0x00AF2178, kExp = 0x00AF217C, kHpMax = 0x00AF2180, kHp = 0x00AF2188;
constexpr uint32_t kGold = 0x00AFA6B4;
constexpr uint32_t kPlace = 0x00AED050;    // area, room (as src/game/minimap.cpp reads it)
// The game's step: 5 the title, 6 its menu, 9 a new game being set up, 13 and on as the story goes
// (it never comes back down in a session); and its screen: 0x2A the title, 3 the game.
constexpr uint32_t kBootStep = 0x00AEE444, kScreen = 0x00AED86C;
constexpr uint32_t kFirstGameStep = 13, kTitleScreen = 0x2A;

// The area names. Their order in the game's table 0x9527C0 (5, 13, 2, 10, 8, 11, 3, 4, 9, 12, 6, 7)
// matches the story's order of areas, which fixes each one: 5 Abandoned Castle (where a new game
// starts), 13 Baljhet Mountains, 2 Garibaldi Temple, and so on; the map items of 0x9526E8 agree.
// Areas 0 and 1 are the two side towers (their order in the game's text table).
constexpr std::array<const char*, 14> kNames = {
    "Tower of Eternity", "Tower of Evermore", "Garibaldi Temple", "Eneomaos Machine Tower", "Aiolon Ruins",
    "Abandoned Castle",  "Dracula's Castle",  "Dracula's Castle", "Forest of Jigramunt",    "Infinite Corridor",
    "Mortvia Aqueduct",  "Cordova Town",      "Dracula's Castle", "Baljhet Mountains"};
constexpr std::array<int, 12> kStoryOrder = {5, 13, 2, 10, 8, 11, 3, 4, 9, 12, 6, 7};
}  // namespace

GameInfo readGameInfo(Memory& mem) {
  GameInfo g;
  g.level = int(mem.read<uint32_t>(kLevel));
  g.exp = int(mem.read<uint32_t>(kExp));
  g.hpMax = int(mem.read<uint32_t>(kHpMax));
  g.hp = int(mem.read<uint32_t>(kHp));
  g.gold = int(mem.read<uint32_t>(kGold));
  const uint32_t place = mem.read<uint32_t>(kPlace);
  g.area = int(int8_t(place & 0xFF)), g.room = int(int8_t((place >> 8) & 0xFF));
  g.inGame = mem.read<uint32_t>(kBootStep) >= kFirstGameStep && mem.read<uint32_t>(kScreen) != kTitleScreen && g.level >= 1 && g.level <= 99;
  g.loading = gameIsLoading(mem);
  return g;
}

const char* areaName(int area) { return area >= 0 && area < int(kNames.size()) ? kNames[size_t(area)] : ""; }

int storyIndex(int area) {
  for (size_t i = 0; i < kStoryOrder.size(); ++i)
    if (kStoryOrder[i] == area) return int(i);
  return -1;
}

bool isDraculasCastle(int area) { return area == 6 || area == 7 || area == 12; }
