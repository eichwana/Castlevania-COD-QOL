#include "host/controls_dialog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "game/achievements.h"
#include "host/audio.h"
#include "host/gamepad.h"
#include "host/hang_report.h"
#include "host/pcsx2_saves.h"
#include "host/update_check.h"
#include "host/settings.h"
#include "host/window.h"
#include "version.h"

// The current look for the list and buttons (common controls 6).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
enum : int { kList = 100, kClear, kDefaults, kClose, kHint, kTabs, kDownload };

// The options page: a check box per setting, the resolution in a list.
enum : int { kFullscreen = 200, kWide, kFilter, kFps, kMinimap, kFastLoading, kSprint, kAttackTurn, kMouseCamera, kInvertY, kInvertX,
             kScale, kScaleLabel, kOptionsNote, kWindowSize, kSensitivity, kLanguage, kInterpolation, kUpdateCheck, kVibration, kBrightness, kVolume, kFastOpening, kXboxIcons, kDeadZone, kStickSpeed, kInvertStickX, kInvertStickY };
// The extras page.
enum : int { kSpeedrunTimer = 300, kLiveSplit, kAchievementNotices, kDiscord, kShowAchievements, kImportCard, kImportFolder, kExportFolder };
// The picture and sound page.
enum : int { kFxaa = 400, kSharpness, kAnisotropy, kCrt, kColourBlind, kHudScale, kVariableRefresh, kLowLatency, kAudioDevice, kSurround };
bool* optionOf(int id) {
  Settings& s = settings();
  switch (id) {
    case kFilter: return &s.textureFilter;
    case kFps: return &s.showFps;
    case kInterpolation: return &s.interpolation;
    case kUpdateCheck: return &s.checkUpdates;
    case kVibration: return &s.vibration;
    case kFastOpening: return &s.fastOpening;
    case kXboxIcons: return &s.xboxIcons;
    case kInvertStickX: return &s.invertStickX;
    case kInvertStickY: return &s.invertStickY;
    case kMinimap: return &s.minimap;
    case kFastLoading: return &s.fastLoading;
    case kSprint: return &s.sprint;
    case kAttackTurn: return &s.attackTurn;
    case kMouseCamera: return &s.mouseCamera;
    case kInvertY: return &s.invertMouseY;
    case kInvertX: return &s.invertMouseX;
    case kSpeedrunTimer: return &s.speedrunTimer;
    case kLiveSplit: return &s.liveSplit;
    case kAchievementNotices: return &s.achievementNotices;
    case kDiscord: return &s.discord;
    case kFxaa: return &s.fxaa;
    case kCrt: return &s.crt;
    case kVariableRefresh: return &s.variableRefresh;
    case kLowLatency: return &s.lowLatency;
    case kSurround: return &s.surround;
  }
  return nullptr;
}

// The list, what is used most first (each is a control of Settings).
constexpr int kRows[Settings::kControls] = {
    15, 1, 14, 13, 12, 11, 9, 8, 10,                               // square, L3, cross, circle, triangle, R1, R2, L2, L1
    Settings::kLeftUp, Settings::kLeftDown, Settings::kLeftLeft, Settings::kLeftRight,
    Settings::kRightUp, Settings::kRightDown, Settings::kRightLeft, Settings::kRightRight,
    0, 3, 4, 5, 6, 7, 2,                                           // select, start, D-pad, R3
};

struct Dialog {
  HWND wnd = nullptr, list = nullptr, hint = nullptr, owner = nullptr, tabs = nullptr;
  std::vector<HWND> controlsPage, optionsPage, extrasPage, lookPage;
  std::vector<std::wstring> audioDevices;  // the output list's entries after the default
  std::vector<std::pair<int, int>> sizes;  // the window size list's entries
  HFONT font = nullptr;
  bool capturing = false;
  int control = -1;
  bool keysAtStart[256] = {};
  bool ignoreClick = false;  // the click that chose a mouse button is not a click on the list
  uint16_t padAtStart = 0;
  bool done = false;
  bool reopen = false;  // the language changed: the window opens again in it, on the options page
};
Dialog* gDialog = nullptr;
int gStartPage = 0;  // the page the window opens on

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}

// The window in Portuguese or English (settings: idioma, the list on the options page).
const wchar_t* T(const wchar_t* pt, const wchar_t* en) { return english() ? en : pt; }

void setHint(const wchar_t* text) { SetWindowTextW(gDialog->hint, text); }

void setCell(int row, int column, const std::wstring& text) {
  LVITEMW item{};
  item.iSubItem = column;
  item.pszText = const_cast<wchar_t*>(text.c_str());
  SendMessageW(gDialog->list, LVM_SETITEMTEXTW, WPARAM(row), LPARAM(&item));
}

void refreshRow(int row) {
  const Settings& s = settings();
  const int c = kRows[row];
  setCell(row, 1, s.keys[c] ? widen(keyLabel(s.keys[c])) : L"—");
  setCell(row, 2, c < Settings::kButtons ? (s.pad[c] ? widen(padLabel(s.pad[c])) : L"—") : T(L"(analógico)", L"(stick)"));
  setCell(row, 3, widen(Settings::ps2Name(c)));
}

void refreshControl(int c) {
  for (int row = 0; row < Settings::kControls; ++row)
    if (kRows[row] == c) refreshRow(row);
}

uint16_t padHeld() {
  XINPUT_STATE state{};
  return readController(state.Gamepad) ? padInputs(state.Gamepad) : 0;
}

int selectedRow() { return int(SendMessageW(gDialog->list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED)); }

void startCapture() {
  const int row = selectedRow();
  if (row < 0) return;
  Dialog& d = *gDialog;
  const int c = kRows[row];
  d.capturing = true, d.control = c;
  for (int vk = 0; vk < 256; ++vk) d.keysAtStart[vk] = GetAsyncKeyState(vk) & 0x8000;
  d.padAtStart = padHeld();
  const std::wstring name = widen(Settings::label(c));
  setHint((c < Settings::kButtons
               ? std::wstring(T(L"Aperte a tecla, o botão do controle ou clique aqui na lista com o botão do mouse para \"",
                                L"Press the key or the controller button, or click here in the list with a mouse button, for \"")) +
                     name + T(L"\" (Esc cancela).", L"\" (Esc cancels).")
               : std::wstring(T(L"Aperte a tecla para \"", L"Press the key for \"")) + name +
                     T(L"\"; no controle é sempre o analógico (Esc cancela).", L"\"; on the controller it is always the stick (Esc cancels)."))
              .c_str());
  SetTimer(d.wnd, 1, 15, nullptr);
}

void finishCapture(const std::wstring& message) {
  KillTimer(gDialog->wnd, 1);
  gDialog->capturing = false;
  setHint(message.c_str());
}

// Saved; says so, and whether another action answers to the same key or button, as both then happen.
void finishAssigned(bool isKey) {
  const Settings& s = settings();
  const int c = gDialog->control;
  s.save();
  refreshControl(c);
  for (int other = 0; other < Settings::kControls; ++other) {
    if (other == c) continue;
    const bool same = isKey ? s.keys[other] == s.keys[c] : other < Settings::kButtons && s.pad[other] == s.pad[c];
    if (same)
      return finishCapture(std::wstring(T(L"Pronto. Atenção: \"", L"Done. Note: \"")) + widen(Settings::label(other)) +
                           T(L"\" usa o mesmo; apertar faz as duas coisas.", L"\" uses the same; pressing it does both."));
  }
  finishCapture(T(L"Pronto.", L"Done."));
}

// Polled while waiting for a key or button: the first one pressed after the wait began.
void pollCapture() {
  Dialog& d = *gDialog;
  Settings& s = settings();
  // Mouse buttons count when clicked over the list (elsewhere they are clicks on the window).
  for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
    const bool held = GetAsyncKeyState(vk) & 0x8000;
    if (!held) d.keysAtStart[vk] = false;
    if (!held || d.keysAtStart[vk]) continue;
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) != d.list) continue;
    s.keys[d.control] = uint8_t(vk);
    d.ignoreClick = vk == VK_LBUTTON;
    return finishAssigned(true);
  }
  for (int vk = 8; vk < 256; ++vk) {
    const bool held = GetAsyncKeyState(vk) & 0x8000;
    if (!held) d.keysAtStart[vk] = false;
    if (!held || d.keysAtStart[vk]) continue;
    if (vk == VK_ESCAPE) return finishCapture(T(L"Cancelado.", L"Cancelled."));
    // The generic Shift/Ctrl/Alt codes are what the game window receives.
    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) continue;
    if (vk == VK_F1 || vk == VK_F10 || vk == VK_F11) return finishCapture(T(L"F1, F10 e F11 são do port; escolha outra tecla.", L"F1, F10 and F11 belong to the port; choose another key."));
    s.keys[d.control] = uint8_t(vk);
    return finishAssigned(true);
  }
  if (d.control >= Settings::kButtons) return;
  const uint16_t held = padHeld(), fresh = held & ~d.padAtStart;
  d.padAtStart &= held;
  for (uint16_t bit = 1; bit; bit <<= 1) {
    if (!(fresh & bit)) continue;
    s.pad[d.control] = bit;
    return finishAssigned(false);
  }
}

HWND button(HWND parent, int id, const wchar_t* text, int x, int y, int w) {
  HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, 30, parent,
                         HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(b, WM_SETFONT, WPARAM(gDialog->font), TRUE);
  return b;
}

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
  HWND c = CreateWindowW(cls, text, WS_CHILD | style, x, y, w, h, gDialog->wnd, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, WPARAM(gDialog->font), TRUE);
  return c;
}

void createOptions() {
  Dialog& d = *gDialog;
  const Settings& s = settings();
  struct Box {
    int id;
    const wchar_t* text;
    bool on;
  };
  const Box image[] = {{kFullscreen, T(L"Tela cheia (também Alt+Enter ou F11)", L"Full screen (also Alt+Enter or F11)"), s.fullscreen},
                       {kWide, T(L"Proporção da imagem (também F10):", L"Picture proportion (also F10):"), s.wide},
                       {kFilter, T(L"Texturas suavizadas, como o PS2 desenha", L"Smoothed textures, as the PS2 draws them"), s.textureFilter},
                       {kFps, T(L"Quadros por segundo no canto (também F9)", L"Frames per second in the corner (also F9)"), s.showFps},
                       {kInterpolation, T(L"Interpolação: quadros entre os do jogo em telas de 100 Hz ou mais", L"Interpolation: frames between the game's on 100 Hz or faster screens"),
                        s.interpolation}};
  const Box game[] = {{kMinimap, T(L"Minimapa no canto da tela (também F8)", L"Minimap in the corner of the screen (also F8)"), s.minimap},
                      {kFastLoading, T(L"Carregamento rápido nas portas (a música para durante ele)", L"Fast loading at doors (the music stops during it)"), s.fastLoading},
                      {kFastOpening, T(L"Abertura rápida (a checagem do cartão e o logo da KONAMI passam na hora)",
                                       L"Fast opening (the memory card check and the KONAMI logo go by at once)"), s.fastOpening},
                      {kSprint, T(L"Correr mais rápido com L3", L"Run faster with L3"), s.sprint},
                      {kAttackTurn, T(L"Ataques viram para o inimigo travado", L"Attacks turn to the locked-on enemy"), s.attackTurn},
                      {kVibration, T(L"Vibração do controle (quando o jogo faz o DualShock 2 vibrar)", L"Controller vibration (when the game makes the DualShock 2 vibrate)"),
                       s.vibration},
                      {kXboxIcons, T(L"Ícones de botão do Xbox nos avisos (A, B, X, Y, LB...; desligado, os do PS2)",
                                     L"Xbox button icons in prompts (A, B, X, Y, LB...; off, the PS2's)"), s.xboxIcons},
                      {kUpdateCheck, T(L"Avisar quando sair uma versão nova do port (pergunta ao GitHub ao abrir o jogo)",
                                       L"Tell me when a new version of the port is out (asks GitHub when the game opens)"), s.checkUpdates}};
  int y = 52;
  d.optionsPage.push_back(child(L"STATIC", T(L"Imagem", L"Picture"), 0, -1, 28, y, 300, 20));
  d.optionsPage.push_back(child(L"STATIC", L"Idioma / Language:", 0, -1, 344, y, 120, 22));
  HWND language = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, kLanguage, 468, y - 4, 160, 100);
  for (const wchar_t* option : {L"Português", L"English"}) SendMessageW(language, CB_ADDSTRING, 0, LPARAM(option));
  SendMessageW(language, CB_SETCURSEL, english() ? 1 : 0, 0);
  d.optionsPage.push_back(language);
  y += 26;
  for (const Box& b : image) {
    if (b.id == kWide) {  // the proportion: a list (4:3, 16:9, ultrawide) in the check box's place
      d.optionsPage.push_back(child(L"STATIC", b.text, 0, -1, 40, y + 3, 300, 22));
      HWND aspect = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST, kWide, 344, y, 220, 200);
      for (const wchar_t* option : {T(L"4:3 (a do PS2)", L"4:3 (the PS2's)"), L"16:9", T(L"Ultrawide (a largura da tela)", L"Ultrawide (the screen's width)")})
        SendMessageW(aspect, CB_ADDSTRING, 0, LPARAM(option));
      SendMessageW(aspect, CB_SETCURSEL, WPARAM(!s.wide ? 0 : s.ultrawide ? 2 : 1), 0);
      d.optionsPage.push_back(aspect);
      y += 26;
      continue;
    }
    HWND c = child(L"BUTTON", b.text, WS_TABSTOP | BS_AUTOCHECKBOX, b.id, 40, y, 600, 24);
    SendMessageW(c, BM_SETCHECK, b.on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.optionsPage.push_back(c);
    y += 24;
  }
  // The window's size: the usual ones that fit on this screen, and the one it has if it is none of them.
  d.optionsPage.push_back(child(L"STATIC", T(L"Janela:", L"Window:"), 0, -1, 40, y + 4, 100, 22));
  HWND sizeList = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kWindowSize, 144, y, 150, 300);
  d.sizes = windowSizes(d.owner);
  const std::pair<int, int> current{s.windowWidth, s.windowHeight};
  if (std::find(d.sizes.begin(), d.sizes.end(), current) == d.sizes.end()) d.sizes.insert(d.sizes.begin(), current);
  for (const auto& [w, h] : d.sizes) {
    const std::wstring text = std::to_wstring(w) + L" x " + std::to_wstring(h);
    SendMessageW(sizeList, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(sizeList, CB_SETCURSEL, WPARAM(std::find(d.sizes.begin(), d.sizes.end(), current) - d.sizes.begin()), 0);
  d.optionsPage.push_back(sizeList);
  d.optionsPage.push_back(child(L"STATIC", T(L"Resolução:", L"Resolution:"), 0, kScaleLabel, 344, y + 4, 70, 22));
  HWND scale = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kScale, 414, y, 150, 200);
  for (const wchar_t* option : {T(L"1x (a do PS2)", L"1x (the PS2's)"), L"2x", L"3x", L"4x", L"5x",
                                L"6x"})
    SendMessageW(scale, CB_ADDSTRING, 0, LPARAM(option));
  SendMessageW(scale, CB_SETCURSEL, WPARAM(std::clamp(s.scale, 1, 6) - 1), 0);
  d.optionsPage.push_back(scale);
  y += 34;
  // Brightness 70-150% and volume 0-100%, in steps of 10, side by side.
  d.optionsPage.push_back(child(L"STATIC", T(L"Brilho:", L"Brightness:"), 0, -1, 40, y + 4, 100, 22));
  HWND brightness = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kBrightness, 144, y, 150, 300);
  for (int b = 70; b <= 150; b += 10) {
    const std::wstring text = std::to_wstring(b) + L"%" + (b == 100 ? T(L" (padrão)", L" (default)") : L"");
    SendMessageW(brightness, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(brightness, CB_SETCURSEL, WPARAM((std::clamp(s.brightness, 70, 150) - 70) / 10), 0);
  d.optionsPage.push_back(brightness);
  d.optionsPage.push_back(child(L"STATIC", T(L"Volume:", L"Volume:"), 0, -1, 344, y + 4, 70, 22));
  HWND volume = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kVolume, 414, y, 150, 300);
  for (int v = 0; v <= 100; v += 10) {
    const std::wstring text = v == 0 ? std::wstring(T(L"Sem som", L"Muted")) : std::to_wstring(v) + L"%";
    SendMessageW(volume, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(volume, CB_SETCURSEL, WPARAM(std::clamp(s.volume, 0, 100) / 10), 0);
  d.optionsPage.push_back(volume);
  y += 34;
  d.optionsPage.push_back(child(L"STATIC", T(L"Jogo", L"Game"), 0, -1, 28, y, 600, 20));
  y += 26;
  for (const Box& b : game) {
    HWND c = child(L"BUTTON", b.text, WS_TABSTOP | BS_AUTOCHECKBOX, b.id, 40, y, 600, 24);
    SendMessageW(c, BM_SETCHECK, b.on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.optionsPage.push_back(c);
    y += 24;
  }
  y += 6;
  d.optionsPage.push_back(child(L"STATIC", L"Mouse", 0, -1, 28, y, 600, 20));
  y += 26;
  HWND mouse = child(L"BUTTON", T(L"Câmera com o mouse (Esc solta o cursor, um clique na janela prende de novo)", L"Camera with the mouse (Esc lets the cursor go, a click in the window holds it again)"),
                     WS_TABSTOP | BS_AUTOCHECKBOX, kMouseCamera, 40, y, 600, 24);
  SendMessageW(mouse, BM_SETCHECK, s.mouseCamera ? BST_CHECKED : BST_UNCHECKED, 0);
  d.optionsPage.push_back(mouse);
  y += 30;
  d.optionsPage.push_back(child(L"STATIC", T(L"Sensibilidade:", L"Sensitivity:"), 0, -1, 40, y + 4, 100, 22));
  HWND sensitivity = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kSensitivity, 144, y, 150, 300);
  for (int i = 1; i <= 10; ++i) {
    const std::wstring text = std::to_wstring(i) + (i == 5 ? T(L" (padrão)", L" (default)") : L"");
    SendMessageW(sensitivity, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(sensitivity, CB_SETCURSEL, WPARAM(std::clamp(s.mouseSensitivity, 1, 10) - 1), 0);
  d.optionsPage.push_back(sensitivity);
  // Each axis inverted on its own: horizontal (right turns left), vertical (forward looks down).
  for (auto [id, text, on, x, w] : {std::tuple{kInvertX, T(L"Inverter horizontal", L"Invert horizontal"), s.invertMouseX, 344, 150},
                                    std::tuple{kInvertY, T(L"Inverter vertical", L"Invert vertical"), s.invertMouseY, 500, 160}}) {
    HWND c = child(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, x, y + 1, w, 24);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.optionsPage.push_back(c);
  }
  y += 34;
  d.optionsPage.push_back(child(L"STATIC", T(L"As mudanças valem na hora e ficam salvas no codport.ini.", L"Changes apply at once and are saved in codport.ini."), 0, kOptionsNote, 28, y + 2, 600, 22));
}

// The extras page: the speedrun timer, achievements, Discord, saves from and to PCSX2.
void createExtras() {
  Dialog& d = *gDialog;
  const Settings& s = settings();
  int y = 52;
  auto heading = [&](const wchar_t* text) {
    d.extrasPage.push_back(child(L"STATIC", text, 0, -1, 28, y, 600, 20));
    y += 26;
  };
  auto box = [&](int id, const wchar_t* text, bool on) {
    HWND c = child(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, 40, y, 620, 24);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.extrasPage.push_back(c);
    y += 24;
  };
  auto note = [&](const wchar_t* text, int lines) {
    d.extrasPage.push_back(child(L"STATIC", text, 0, -1, 40, y + 2, 620, 20 * lines));
    y += 20 * lines + 6;
  };
  heading(L"Speedrun");
  box(kSpeedrunTimer, T(L"Cronômetro no canto (também F7): o tempo do jogo, sem os carregamentos entre lugares",
                        L"Timer in the corner (also F7): the game's time, without the loads between places"), s.speedrunTimer);
  box(kLiveSplit, T(L"O LiveSplit segue o cronômetro: começa, divide a cada área nova da história, zera no título",
                    L"LiveSplit follows the timer: it starts, splits at each new story area, resets at the title"), s.liveSplit);
  note(T(L"No LiveSplit: botão direito > Control > Start TCP Server (porta 16834). Ele usa o tempo de jogo (Game Time).",
         L"In LiveSplit: right click > Control > Start TCP Server (port 16834). It uses the game time (Game Time)."), 1);
  y += 6;
  heading(T(L"Conquistas", L"Achievements"));
  box(kAchievementNotices, T(L"Aviso na tela quando uma conquista é alcançada", L"A notice on the screen when an achievement is reached"),
      s.achievementNotices);
  d.extrasPage.push_back(button(d.wnd, kShowAchievements, T(L"Ver as conquistas", L"See the achievements"), 40, y + 2, 180));
  y += 42;
  heading(L"Discord");
  box(kDiscord, T(L"Mostrar no Discord a área em que o Hector está e o nível dele", L"Show on Discord the area Hector is in and his level"), s.discord);
  if (s.discordClientId.empty())
    note(T(L"Falta o id do aplicativo do Discord: ponha em discord_id_aplicativo, no codport.ini.",
           L"The Discord application's id is missing: put it in discord_id_aplicativo, in codport.ini."), 1);
  y += 6;
  heading(T(L"Saves do PCSX2", L"PCSX2 saves"));
  note(T(L"Traz o save deste jogo de um cartão do PCSX2 para o port, ou leva o do port para o PCSX2. O save que já\n"
         L"estiver no lugar é guardado antes numa pasta backup, dentro de memcard.",
         L"Brings this game's save from a PCSX2 card into the port, or takes the port's to PCSX2. A save already\n"
         L"in its place is kept first in a backup folder, inside memcard."), 2);
  d.extrasPage.push_back(button(d.wnd, kImportCard, T(L"Importar cartão .ps2...", L"Import a .ps2 card..."), 40, y, 196));
  d.extrasPage.push_back(button(d.wnd, kImportFolder, T(L"Importar cartão em pasta...", L"Import a folder card..."), 244, y, 196));
  d.extrasPage.push_back(button(d.wnd, kExportFolder, T(L"Exportar para cartão em pasta...", L"Export to a folder card..."), 448, y, 212));
  y += 38;
  note(T(L"Para levar ao PCSX2, crie nele um cartão do tipo Pasta (Folder) e escolha a pasta desse cartão, em memcards.",
         L"To take it to PCSX2, create a Folder type card there and choose that card's folder, in memcards."), 1);
}

// The picture and sound page: what is done to the picture after the game draws it, the screen's
// pace, the sound's output.
void createLook() {
  Dialog& d = *gDialog;
  const Settings& s = settings();
  int y = 52;
  auto heading = [&](const wchar_t* text) {
    d.lookPage.push_back(child(L"STATIC", text, 0, -1, 28, y, 600, 20));
    y += 26;
  };
  auto box = [&](int id, const wchar_t* text, bool on) {
    HWND c = child(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, 40, y, 620, 24);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.lookPage.push_back(c);
    y += 24;
  };
  // A labelled list: at the left (x 40) or the right half (x 344) of a row.
  auto list = [&](int id, const wchar_t* label, std::initializer_list<std::wstring> options, int selected, int x) {
    d.lookPage.push_back(child(L"STATIC", label, 0, -1, x, y + 4, 140, 22));
    HWND c = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id, x + 144, y, 160, 300);
    for (const std::wstring& o : options) SendMessageW(c, CB_ADDSTRING, 0, LPARAM(o.c_str()));
    SendMessageW(c, CB_SETCURSEL, WPARAM(selected), 0);
    d.lookPage.push_back(c);
  };
  heading(T(L"Imagem", L"Picture"));
  box(kFxaa, T(L"Anti-aliasing FXAA (suaviza os serrilhados; bom para quem não pode subir a resolução)",
               L"FXAA anti-aliasing (smooths jagged edges; good when the resolution cannot go up)"), s.fxaa);
  box(kCrt, T(L"Filtro de TV de tubo: linhas e máscara de cor", L"Tube TV filter: scanlines and a colour mask"), s.crt);
  y += 6;
  std::initializer_list<std::wstring> percent = {T(L"Desligada", L"Off"), L"10%", L"20%", L"30%", L"40%", L"50%", L"60%", L"70%", L"80%", L"90%", L"100%"};
  list(kSharpness, T(L"Nitidez:", L"Sharpness:"), percent, std::clamp(s.sharpness, 0, 100) / 10, 40);
  const int af = s.anisotropy >= 16 ? 4 : s.anisotropy >= 8 ? 3 : s.anisotropy >= 4 ? 2 : s.anisotropy >= 2 ? 1 : 0;
  list(kAnisotropy, T(L"Filtro anisotrópico:", L"Anisotropic filter:"), {T(L"Desligado (o do PS2)", L"Off (the PS2's)"), L"2x", L"4x", L"8x", L"16x"}, af, 344);
  y += 34;
  list(kColourBlind, T(L"Cores para daltonismo:", L"Colour blindness:"),
       {T(L"Nenhum", L"None"), T(L"Protanopia", L"Protanopia"), T(L"Deuteranopia", L"Deuteranopia"), T(L"Tritanopia", L"Tritanopia")},
       std::clamp(s.colourBlind, 0, 3), 40);
  {
    static constexpr int kSizes[] = {80, 90, 100, 110, 125};
    int at = 2;
    for (int i = 0; i < 5; ++i)
      if (s.hudScale == kSizes[i]) at = i;
    list(kHudScale, T(L"Tamanho da HUD:", L"HUD size:"), {L"80%", L"90%", T(L"100% (a do jogo)", L"100% (the game's)"), L"110%", L"125%"}, at, 344);
  }
  y += 40;
  heading(T(L"Tela", L"Screen"));
  box(kVariableRefresh, T(L"Monitor com G-Sync ou FreeSync: cada imagem aparece assim que fica pronta (em outros rasga)",
                          L"G-Sync or FreeSync screen: each picture shows as soon as it is ready (it tears on others)"), s.variableRefresh);
  box(kLowLatency, T(L"Baixa latência: os controles são lidos logo antes de cada quadro do jogo",
                     L"Low latency: the controls are read just before each of the game's frames"), s.lowLatency);
  y += 10;
  heading(T(L"Som", L"Sound"));
  d.audioDevices = audioOutputDevices();
  int current = 0;
  for (size_t i = 0; i < d.audioDevices.size(); ++i)
    if (widen(s.audioDevice) == d.audioDevices[i]) current = int(i) + 1;
  d.lookPage.push_back(child(L"STATIC", T(L"Saída:", L"Output:"), 0, -1, 40, y + 4, 100, 22));
  HWND out = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kAudioDevice, 144, y, 420, 300);
  SendMessageW(out, CB_ADDSTRING, 0, LPARAM(T(L"A padrão do Windows", L"Windows' default")));
  for (const std::wstring& name : d.audioDevices) SendMessageW(out, CB_ADDSTRING, 0, LPARAM(name.c_str()));
  SendMessageW(out, CB_SETCURSEL, WPARAM(current), 0);
  d.lookPage.push_back(out);
  y += 34;
  box(kSurround, T(L"Som 5.1: o Pro Logic II do jogo aberto em 6 canais (escolha Pro Logic II no som do jogo)",
                   L"5.1 sound: the game's Pro Logic II opened into 6 channels (choose Pro Logic II in the game's sound)"), s.surround);
  y += 10;
  d.lookPage.push_back(child(L"STATIC", T(L"F6 no jogo: modo de foto (o jogo para, sem HUD; roda do mouse aproxima, setas movem, F12 salva).",
                                          L"F6 in game: photo mode (the game stops, no HUD; the wheel zooms, the arrows move, F12 saves)."),
                             0, -1, 28, y + 2, 640, 22));
}

void showPage(int page) {
  Dialog& d = *gDialog;
  for (HWND c : d.lookPage) ShowWindow(c, page == 2 ? SW_SHOW : SW_HIDE);
  for (HWND c : d.controlsPage) ShowWindow(c, page == 0 ? SW_SHOW : SW_HIDE);
  for (HWND c : d.optionsPage) ShowWindow(c, page == 1 ? SW_SHOW : SW_HIDE);
  for (HWND c : d.extrasPage) ShowWindow(c, page == 3 ? SW_SHOW : SW_HIDE);
}

// A .ps2 file or a folder, chosen by the player; empty if cancelled.
std::filesystem::path pickCard(bool folder) {
  if (!folder) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = gDialog->wnd;
    ofn.lpstrFilter = L"PCSX2 (*.ps2)\0*.ps2\0*.*\0*.*\0";
    ofn.lpstrFile = file, ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::filesystem::path(file) : std::filesystem::path();
  }
  std::filesystem::path chosen;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->Show(gDialog->wnd)) && SUCCEEDED(dialog->GetResult(&item))) {
      wchar_t* path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) chosen = path, CoTaskMemFree(path);
      item->Release();
    }
    dialog->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return chosen;
}

void showAchievements() {
  const std::vector<AchievementState> list = achievementList();
  size_t got = 0;
  std::wstring text;
  for (const AchievementState& a : list) {
    got += a.unlocked;
    text += (a.unlocked ? L"\u2714  " : L"\u25CB  ") + a.name + L" - " + a.description;
    if (a.unlocked) text += L"  (" + std::wstring(a.when.begin(), a.when.end()) + L")";
    text += L"\n";
  }
  const std::wstring head = std::to_wstring(got) + T(L" de ", L" of ") + std::to_wstring(list.size()) + L"\n\n";
  MessageBoxW(gDialog->wnd, (head + text).c_str(), T(L"Conquistas", L"Achievements"), MB_OK);
}

// A box of the options page clicked: the setting follows it at once.
void optionClicked(int id, bool on) {
  Settings& s = settings();
  if (id == kFullscreen) return setFullscreen(gDialog->owner, on);
  if (id == kWide) return setWide(gDialog->owner, on);
  bool* option = optionOf(id);
  if (!option) return;
  *option = on;
  s.save();
  if (id == kFps) InvalidateRect(gDialog->owner, nullptr, TRUE);
  settingsChanged();
}

void create(HWND wnd) {
  Dialog& d = *gDialog;
  d.wnd = wnd;
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
  d.font = CreateFontIndirectW(&metrics.lfMessageFont);
  // The tabs, behind what each page shows.
  d.tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 6, 6, 668, 624, wnd, HMENU(INT_PTR(kTabs)),
                         GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.tabs, WM_SETFONT, WPARAM(d.font), TRUE);
  const wchar_t* tabNames[] = {T(L"Controles", L"Controls"), T(L"Opções", L"Options"), T(L"Imagem e som", L"Picture and sound"), L"Extras"};
  for (int i = 0; i < 4; ++i) {
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<wchar_t*>(tabNames[i]);
    SendMessageW(d.tabs, TCM_INSERTITEMW, WPARAM(i), LPARAM(&tab));
  }
  d.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                           16, 40, 648, 468, wnd, HMENU(INT_PTR(kList)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.list, WM_SETFONT, WPARAM(d.font), TRUE);
  SendMessageW(d.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  const wchar_t* titles[] = {T(L"Ação", L"Action"), T(L"Teclado", L"Keyboard"), T(L"Controle (Xbox)", L"Controller (Xbox)"),
                             T(L"No PS2", L"On the PS2")};
  const int widths[] = {296, 100, 110, 116};
  for (int i = 0; i < 4; ++i) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(titles[i]);
    col.cx = widths[i];
    SendMessageW(d.list, LVM_INSERTCOLUMNW, WPARAM(i), LPARAM(&col));
  }
  for (int row = 0; row < Settings::kControls; ++row) {
    const std::wstring label = widen(Settings::label(kRows[row]));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(label.c_str());
    SendMessageW(d.list, LVM_INSERTITEMW, 0, LPARAM(&item));
    refreshRow(row);
  }
  d.hint = CreateWindowW(L"STATIC", T(L"Clique na ação que quer mudar e aperte a tecla ou o botão do controle.", L"Click the action to change and press the key or the controller button."),
                         WS_CHILD | WS_VISIBLE, 16, 582, 648, 40, wnd, HMENU(INT_PTR(kHint)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.hint, WM_SETFONT, WPARAM(d.font), TRUE);
  d.controlsPage = {d.list, d.hint, button(wnd, kClear, T(L"Limpar a ação", L"Clear action"), 12, 638, 120),
                    button(wnd, kDefaults, T(L"Restaurar padrão", L"Restore defaults"), 138, 638, 140)};
  // The controller's sticks: dead zone, the right one's camera speed, its axes reversed.
  {
    const Settings& s = settings();
    d.controlsPage.push_back(child(L"STATIC", T(L"Zona morta dos analógicos:", L"Sticks' dead zone:"), 0, -1, 16, 520, 180, 22));
    HWND zone = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kDeadZone, 196, 516, 130, 300);
    for (int z = 0; z <= 40; z += 5) {
      const std::wstring text = z == 0 ? std::wstring(T(L"Padrão (24%)", L"Default (24%)")) : std::to_wstring(z) + L"%";
      SendMessageW(zone, CB_ADDSTRING, 0, LPARAM(text.c_str()));
    }
    SendMessageW(zone, CB_SETCURSEL, WPARAM(std::clamp(s.stickDeadZone, 0, 40) / 5), 0);
    d.controlsPage.push_back(zone);
    d.controlsPage.push_back(child(L"STATIC", T(L"Câmera no analógico direito:", L"Right stick camera speed:"), 0, -1, 344, 520, 180, 22));
    HWND speed = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kStickSpeed, 528, 516, 130, 300);
    for (int v = 50; v <= 300; v += 25) {
      const std::wstring text = std::to_wstring(v) + L"%" + (v == 100 ? T(L" (padrão)", L" (default)") : L"");
      SendMessageW(speed, CB_ADDSTRING, 0, LPARAM(text.c_str()));
    }
    SendMessageW(speed, CB_SETCURSEL, WPARAM((std::clamp(s.stickCameraSpeed, 50, 300) - 50) / 25), 0);
    d.controlsPage.push_back(speed);
    for (auto [id, text, on, x] : {std::tuple{kInvertStickX, T(L"Inverter a câmera na horizontal", L"Invert the camera horizontally"), s.invertStickX, 16},
                                   std::tuple{kInvertStickY, T(L"Inverter a câmera na vertical", L"Invert the camera vertically"), s.invertStickY, 344}}) {
      HWND c = child(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, id, x, 548, 310, 24);
      SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
      d.controlsPage.push_back(c);
    }
  }
  button(wnd, kClose, T(L"Salvar", L"Save"), 578, 638, 90);
  if (const std::string v = newerVersion(); !v.empty()) {  // a newer release on GitHub: its page, a click away
    const std::wstring text = T(L"Baixar a versão nova (", L"Download the new version (") + std::wstring(v.begin(), v.end()) + L")";
    button(wnd, kDownload, text.c_str(), 292, 638, 276);
  }
  createOptions();
  createLook();
  createExtras();
  SendMessageW(d.tabs, TCM_SETCURSEL, WPARAM(gStartPage), 0);
  showPage(gStartPage);
}

LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: create(wnd); return 0;
    case WM_TIMER: pollCapture(); return 0;
    case WM_CTLCOLORSTATIC:  // labels and check boxes sit on the tab's page, not on the window's grey
      SetBkColor(HDC(wp), GetSysColor(COLOR_WINDOW));
      return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    case WM_COMMAND:
      if (gDialog->capturing && (LOWORD(wp) == kClear || LOWORD(wp) == kDefaults)) finishCapture(L"");
      if ((LOWORD(wp) == kFullscreen || optionOf(LOWORD(wp))) && HIWORD(wp) == BN_CLICKED)
        return optionClicked(LOWORD(wp), SendMessageW(HWND(lp), BM_GETCHECK, 0, 0) == BST_CHECKED), 0;
      if (LOWORD(wp) == kWindowSize && HIWORD(wp) == CBN_SELCHANGE) {
        const size_t i = size_t(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        if (i < gDialog->sizes.size()) setWindowSize(gDialog->owner, gDialog->sizes[i].first, gDialog->sizes[i].second);
        return 0;
      }
      if (LOWORD(wp) == kLanguage && HIWORD(wp) == CBN_SELCHANGE) {
        settings().language = SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0) == 1 ? "en" : "pt";
        settings().save();
        settingsChanged();
        gDialog->reopen = true;
        DestroyWindow(wnd);
        return 0;
      }
      if (LOWORD(wp) == kWide && HIWORD(wp) == CBN_SELCHANGE) {
        const int i = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        settings().ultrawide = i == 2;
        setWide(gDialog->owner, i != 0);
        return 0;
      }
      if ((LOWORD(wp) == kSharpness || LOWORD(wp) == kAnisotropy || LOWORD(wp) == kColourBlind || LOWORD(wp) == kHudScale ||
           LOWORD(wp) == kAudioDevice) &&
          HIWORD(wp) == CBN_SELCHANGE) {
        const int i = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        Settings& s = settings();
        switch (LOWORD(wp)) {
          case kSharpness: s.sharpness = 10 * i; break;
          case kAnisotropy: s.anisotropy = i == 0 ? 0 : 1 << i; break;
          case kColourBlind: s.colourBlind = i; break;
          case kHudScale: s.hudScale = std::array<int, 5>{80, 90, 100, 110, 125}[size_t(std::clamp(i, 0, 4))]; break;
          case kAudioDevice: {
            const std::wstring name = i > 0 && size_t(i) <= gDialog->audioDevices.size() ? gDialog->audioDevices[size_t(i) - 1] : L"";
            s.audioDevice.assign(size_t(WideCharToMultiByte(CP_UTF8, 0, name.c_str(), int(name.size()), nullptr, 0, nullptr, nullptr)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, name.c_str(), int(name.size()), s.audioDevice.data(), int(s.audioDevice.size()), nullptr, nullptr);
            break;
          }
        }
        s.save();
        return 0;
      }
      if ((LOWORD(wp) == kDeadZone || LOWORD(wp) == kStickSpeed) && HIWORD(wp) == CBN_SELCHANGE) {
        const int i = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        if (LOWORD(wp) == kDeadZone) settings().stickDeadZone = 5 * i;
        else settings().stickCameraSpeed = 50 + 25 * i;
        settings().save();
        settingsChanged();  // the camera speed is noted in a recording
        return 0;
      }
      if ((LOWORD(wp) == kBrightness || LOWORD(wp) == kVolume) && HIWORD(wp) == CBN_SELCHANGE) {
        const int i = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        (LOWORD(wp) == kBrightness ? settings().brightness : settings().volume) = LOWORD(wp) == kBrightness ? 70 + 10 * i : 10 * i;
        settings().save();
        InvalidateRect(gDialog->owner, nullptr, TRUE);  // the software picture shows it at once too
        return 0;
      }
      if (LOWORD(wp) == kSensitivity && HIWORD(wp) == CBN_SELCHANGE) {
        settings().mouseSensitivity = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0)) + 1;
        settings().save();
        return 0;
      }
      if (LOWORD(wp) == kScale && HIWORD(wp) == CBN_SELCHANGE) {
        settings().scale = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0)) + 1;
        settings().save();
        settingsChanged();  // the graphics card made again at it
        return 0;
      }
      switch (LOWORD(wp)) {
        case kClear:
          if (const int row = selectedRow(); row >= 0) {
            const int c = kRows[row];
            settings().keys[c] = 0;
            if (c < Settings::kButtons) settings().pad[c] = 0;
            settings().save();
            refreshRow(row);
            setHint(T(L"Sem tecla nem botão.", L"No key or button."));
          }
          break;
        case kDefaults: {
          const Settings def = Settings::defaults();
          settings().keys = def.keys, settings().pad = def.pad;
          settings().save();
          for (int row = 0; row < Settings::kControls; ++row) refreshRow(row);
          setHint(T(L"Teclas e botões voltaram ao padrão.", L"Keys and buttons are back to the defaults."));
          break;
        }
        case kShowAchievements:
          showAchievements();
          break;
        case kImportCard:
        case kImportFolder:
          if (const std::filesystem::path card = pickCard(LOWORD(wp) == kImportFolder); !card.empty())
            MessageBoxW(wnd, importPcsx2Saves(card).c_str(), T(L"Saves do PCSX2", L"PCSX2 saves"), MB_OK);
          break;
        case kExportFolder:
          if (const std::filesystem::path card = pickCard(true); !card.empty())
            MessageBoxW(wnd, exportToPcsx2Folder(card).c_str(), T(L"Saves do PCSX2", L"PCSX2 saves"), MB_OK);
          break;
        case kDownload:
          ShellExecuteW(nullptr, L"open", kReleasesUrl, nullptr, nullptr, SW_SHOWNORMAL);
          break;
        case kClose:  // everything is in use already: written once more, and the window closes
          settings().save();
          settingsChanged();
          DestroyWindow(wnd);
          break;
      }
      return 0;
    case WM_NOTIFY: {
      const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
      if (n->idFrom == kList && (n->code == NM_CLICK || n->code == NM_RETURN)) {
        if (gDialog->ignoreClick && n->code == NM_CLICK) gDialog->ignoreClick = false;
        else startCapture();
      }
      if (n->idFrom == kTabs && n->code == TCN_SELCHANGE) {
        if (gDialog->capturing) finishCapture(L"");
        showPage(int(SendMessageW(gDialog->tabs, TCM_GETCURSEL, 0, 0)));
      }
      return 0;
    }
    case WM_CLOSE: DestroyWindow(wnd); return 0;
    case WM_DESTROY:
      KillTimer(wnd, 1);
      DeleteObject(gDialog->font);
      gDialog->done = true;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}
}  // namespace

void showControlsDialog(void* ownerHandle) {
  HWND owner = static_cast<HWND>(ownerHandle);
  static bool registered = [] {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"codport-controls";
    return RegisterClassW(&wc) != 0;
  }();
  if (!registered || gDialog) return;
  stopRumble();  // the game waits while this window is open
  hangWatchPause(true);
  Dialog dialog;
  gDialog = &dialog;
  dialog.owner = owner;
  RECT r{0, 0, 680, 680};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
  AdjustWindowRect(&r, style, FALSE);
  RECT o{};
  GetWindowRect(owner, &o);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const std::wstring caption = T(L"Opções e controles - versão ", L"Options and controls - version ") + std::wstring(L"" CODPORT_VERSION);
  HWND wnd = CreateWindowW(L"codport-controls", caption.c_str(), style, (o.left + o.right - w) / 2,
                           (o.top + o.bottom - h) / 2, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!wnd) {
    gDialog = nullptr;
    return;
  }
  // Modal: the game window (and so the game) waits until this one closes.
  EnableWindow(owner, FALSE);
  MSG msg;
  while (!dialog.done) {
    if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {  // the program is closing: let the game's loop see it
      PostQuitMessage(0);
      DestroyWindow(wnd);
      break;
    }
    if (!IsDialogMessageW(wnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  gDialog = nullptr;
  if (dialog.reopen) {  // in the language just chosen, on the options page
    gStartPage = 1;
    showControlsDialog(owner);
    gStartPage = 0;
    return;
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  hangWatchPause(false);
}
