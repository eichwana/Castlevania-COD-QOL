#pragma once

#include <string>

// Discord Rich Presence (Settings::discord): "Playing Castlevania: Curse of Darkness" with the
// area Hector is in and his level, through the Discord app's local pipe (\\.\pipe\discord-ipc-N).
// It needs a Discord application's id (Settings::discordClientId; its name is what Discord shows
// as the game). A thread of its own talks to Discord; nothing waits for it, and nothing happens
// when Discord is not running.
// `details`/`state`: the two lines under the game's name (UTF-8); empty details: at the menus.
void setDiscordPresence(const std::string& details, const std::string& state);
void stopDiscord();
