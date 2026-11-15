#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "game/game_info.h"

// The port's achievements: milestones read from the game's own state (areas reached, Hector's
// level, gold), kept in conquistas.txt next to codport.ini. Each one, when it is reached, shows a
// notice at the bottom of the picture for a few seconds (Settings::achievementNotices). Nothing in
// the game changes; replays never unlock any.
void loadAchievements(const std::filesystem::path& file);
// Once per frame, on the emulation thread. `playing`: the game is being played (not a menu, a load,
// a scene), when notices show; they wait otherwise.
void updateAchievements(const GameInfo& info, bool playing);

struct AchievementState {
  std::wstring name, description;
  bool unlocked = false;
  std::string when;  // "2026-11-14 23:10", when unlocked
};
std::vector<AchievementState> achievementList();  // for the options window, any thread
