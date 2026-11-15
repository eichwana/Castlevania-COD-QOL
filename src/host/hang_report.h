#pragma once

#include <filesystem>

// When the game stops (no VBlank for 5 seconds while it runs) or its window stops answering Windows,
// writes where every thread of the program is, once, to travamento-<date>.txt in `folder`: the
// functions on each thread's stack (with source lines where codport.pdb is there). A thread of its
// own looks every second.
void startHangWatch(void* window, const std::filesystem::path& folder);
// When the program fails (an exception nothing handles), writes the same, with the failing thread
// first, to falha-<date>.txt in `folder`.
void startCrashReport(const std::filesystem::path& folder);
void hangWatchBeat();               // each VBlank
void hangWatchPause(bool paused);   // while the game waits on purpose (the F1 window, photo mode)
