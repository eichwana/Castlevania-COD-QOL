#pragma once
// The game's code, in DLLs Setup builds from the player's disc (src/recomp/gamecode.cpp).
#include <vector>

#include "recomp/runtime.h"

void registerRecompiled(std::vector<rc::Entry>& out);
bool newGameCode(std::vector<rc::Entry>& out);
bool gameCodeIsOld();  // a part of the game's code was built by another version of the port

// How much of the game's code is in use (`loaded` of `parts`; the interpreter runs the rest, several
// times slower) and what the setup says of the parts still missing: being built
// (codport_game.building, there while it builds them; one left by a build that was stopped stays
// until Setup runs again) or that building them failed (codport_game.failed).
// `progress`: how far the build of the missing parts is (0-1), from the setup's log
// (build/setup-rest.log); below 0 when it does not say.
struct GameCodeStatus {
  int loaded = 0, parts = 0;
  bool building = false, failed = false;
  float progress = -1;
};
GameCodeStatus gameCodeStatus();
