#pragma once
#include <string>

// Whether a newer release of the port is on GitHub: asked once, in the background, when the game
// opens (unless the setting avisar_versao_nova is off). The request is GitHub's public "latest
// release" of the project; nothing about the player is sent. When the answer comes, the window's
// title says so (refreshTitle) and the F1 window offers the download.
void startUpdateCheck();
// The newer version ("1.5.0"), or empty: not known (yet), not newer, or no connection.
std::string newerVersion();
inline constexpr const wchar_t* kReleasesUrl = L"https://github.com/fasipe/Castlevania-COD-QOL/releases/latest";
