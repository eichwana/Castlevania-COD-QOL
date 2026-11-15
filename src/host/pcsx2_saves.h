#pragma once

#include <filesystem>
#include <string>

// Saves to and from PCSX2. The port keeps the memory card as a folder (memcard/card0/<save>); PCSX2
// keeps it as an 8 MB card image (.ps2) or, as an option, as a folder too ("folder memory card").
// Only this game's saves (BESLES-53755*) are copied. A save of the same name already in the port's
// card is moved to memcard/backup-<date>/ first, so nothing is lost.

void setMemcardFolder(const std::filesystem::path& folder);  // the port's (memcard/), at start

// From a .ps2 card image or a PCSX2 folder memory card into the port. Returns what happened, for
// the player, in the options window's language.
std::wstring importPcsx2Saves(const std::filesystem::path& source);
// From the port into a PCSX2 folder memory card (PCSX2's memcards\<name>.ps2 folder).
std::wstring exportToPcsx2Folder(const std::filesystem::path& folderCard);
