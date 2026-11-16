#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

// The player's settings, kept in codport.ini next to the memory card folder: which keyboard key
// and which Xbox controller input give each control of the PS2 controller, and how the window
// shows the picture. Missing or unreadable entries keep their defaults; the file is rewritten
// whenever a setting changes.
struct Settings {
  // The controls that can be bound, in this order: the 16 buttons (bit i of the PS2 button
  // word), then the left and right sticks pushed up, down, left and right.
  enum : int { kButtons = 16, kLeftUp = 16, kLeftDown, kLeftLeft, kLeftRight, kRightUp, kRightDown, kRightLeft,
               kRightRight, kControls };
  static const char* id(int control);    // name in the file
  static const char* label(int control); // what it does in the game, shown to the player
  static const char* ps2Name(int control);  // its button on the PS2 controller

  // Keyboard: a Windows virtual-key code per control (0 = none). The same key may drive several
  // controls (the arrows give the D-pad and the left stick).
  std::array<uint8_t, kControls> keys{};
  // Controller: per button, an XInput button bit, or kLeftTrigger / kRightTrigger, or 0. The
  // sticks are always the controller's sticks.
  enum : uint16_t { kLeftTrigger = 0x0400, kRightTrigger = 0x0800 };
  std::array<uint16_t, kButtons> pad{};

  std::string iso;  // the game's disc image, UTF-8 (set by the setup, or when the game asks for it)
  bool limitBuildJobs = true;  // Setup compiles only as many files at once as the free memory holds (its own; kept here)
  bool fullscreen = false;
  int windowWidth = 960, windowHeight = 720;  // the window's inside, out of full screen
  // Where the window was last left (its outer top left corner, on any screen); kNoPlace: centred.
  static constexpr int kNoPlace = -1000000;
  int windowX = kNoPlace, windowY = kNoPlace;
  bool wide = false;  // the picture shown 16:9 instead of 4:3
  bool ultrawide = false;  // with wide: as wide as the window when it is wider than 16:9 (up to 32:9)
  int scale = 1;      // drawing resolution, 1-6 times the PS2's (6x takes about 1.5 GB of video memory at most, as measured)
  bool showFps = true;  // frames per second in the top right corner (F9)
  int brightness = 100;  // the picture's brightness, a gamma, in percent (100: as the game draws it)
  int volume = 100;      // the sound's volume, in percent
  std::string audioDevice;  // the sound output device's name (empty: Windows' default)
  bool surround = false;    // the game's Pro Logic II sound decoded into 5.1 (host/prologic.h)
  bool interpolation = false;  // pictures made between the game's, on screens of 100 Hz or more
  bool minimap = true;  // the map in the corner of the screen (F8)
  bool textureFilter = true;  // as the PS2 filters (read at start)
  int anisotropy = 0;  // anisotropic texture filtering, 2-16 samples (0: as the PS2, bilinear)
  // The picture's look, after it is drawn: FXAA on its edges, sharpening in percent, a tube TV's
  // scanlines, colours for colour blindness (1 protanopia, 2 deuteranopia, 3 tritanopia).
  bool fxaa = false, crt = false;
  // Variable refresh screens (G-Sync, FreeSync): each picture shown when it is ready (it tears on
  // other screens). Low latency: the controls read just before each frame runs.
  bool variableRefresh = false, lowLatency = false;
  int hudScale = 100;  // the HUD's size during play in percent, 80-125 (more meets the minimap): each piece toward its corner
  int sharpness = 0, colourBlind = 0;
  bool fastLoading = true;  // between places, no waiting for real time (nor its sound)
  bool fastOpening = true;  // the memory card check and the KONAMI logo at boot, without waiting
  bool sprint = true;  // L3 sprints (read at start)
  bool attackTurn = true;  // with an enemy locked on, attacks turn to it (read at start)
  bool xboxIcons = true;  // the game's PS2 button icons drawn as an Xbox controller's (A, B, X, Y, LB...)
  bool vibration = true;  // the controller vibrates when the game makes the DualShock 2 vibrate
  bool mouseCamera = true;  // the mouse turns the camera (the right stick) while the window holds it
  int mouseSensitivity = 5;  // 1-10
  bool invertMouseY = false;  // pushing the mouse forward looks down instead of up
  bool invertMouseX = false;  // moving the mouse right turns the camera left
  // The controller: its sticks' dead zone in percent (0: XInput's own, 24% and 27%), the right
  // stick's camera speed in percent of the game's (above 100 faster than the game ever turns it,
  // read at start and noted in recordings), and the right stick's axes reversed.
  int stickDeadZone = 0;
  int stickCameraSpeed = 100;
  bool invertStickX = false, invertStickY = false;
  // Extras: the speedrun timer in the corner (F7) and LiveSplit following it (game/speedrun.h), the
  // achievements' notices (game/achievements.h), Discord showing the area and level (host/discord.h)
  // through the Discord application whose id is here (empty: off).
  bool speedrunTimer = false, liveSplit = false, achievementNotices = true, discord = true;
  std::string discordClientId = kDiscordClientId;
  static constexpr const char* kDiscordClientId = "1571365772641435739";  // the port's Discord application
  bool texturePack = true;    // textures/replace used in place of the game's (read at start)
  bool dumpTextures = false;  // the game's textures saved in textures/dump (read at start)
  std::string language;  // of the options window (F1) and the window's title: "pt" or "en"
  bool checkUpdates = true;  // ask GitHub, when the game opens, whether a newer version is out

  static Settings defaults();
  void load(const std::filesystem::path& file);
  void save() const;
  std::filesystem::path file;
};

Settings& settings();  // the ones in use

bool english();  // the options window and the title in English
std::string keyLabel(int vk);          // a key as shown in the options window
std::string padLabel(uint16_t input);  // a controller input as shown there
std::string keyName(int vk);           // as written in the file and shown ("Z", "Enter", "F1")
int keyFromName(const std::string& name);
std::string padName(uint16_t input);   // "A", "LB", "LT"...
uint16_t padFromName(const std::string& name);
