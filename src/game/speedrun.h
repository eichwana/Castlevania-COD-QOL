#pragma once

#include "game/game_info.h"

// The speedrun timer (Settings::speedrunTimer, F7): the game's time since a game was started or
// loaded, without the loads between places (the game only counts frames there, and this port
// skips them in no time), shown in the picture's corner. It starts when Hector's data appears
// after the title's menu and goes back to zero at the title. With Settings::liveSplit, LiveSplit's
// own timer follows it through LiveSplit Server (TCP, localhost:16834): it starts, takes this
// game time, splits the first time each story area is entered and resets at the title.
// Only reads the game; called once per frame on the emulation thread.
void updateSpeedrun(const GameInfo& info, double frameSeconds, bool liveSplitAllowed);
void stopSpeedrun();  // the program is closing
