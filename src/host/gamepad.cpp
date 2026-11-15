#include "host/gamepad.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>

#include "host/playstation_pad.h"
#include "host/settings.h"
#include "host/window.h"
#include "iop/pad.h"

namespace {
enum class Source { None, XInput, PlayStation };
Source gSource = Source::None;  // where readController found the controller

// An XInput stick (-32768..32767, y up) as a DualShock 2 stick (0..255, 0x80 centred, y down).
// The dead zone is round and the range past it is stretched so that pushing the stick most of the
// way reaches the end: Xbox sticks move in a circle and seldom report their full range, while a
// DualShock 2 reaches its corners and the very ends of its axes, which games read as full speed
// (Hector runs only with the stick at its end and walks otherwise).
void stick(SHORT sx, SHORT sy, SHORT deadZone, uint8_t& outX, uint8_t& outY) {
  const double x = sx / 32767.0, y = -sy / 32767.0;
  const double m = std::hypot(x, y), dz = deadZone / 32767.0, full = 0.85;
  if (m <= dz) {
    outX = outY = 0x80;
    return;
  }
  const double r = std::min(1.0, (m - dz) / (full - dz));
  double ux = x / m * r, uy = y / m * r;
  // The circle onto the square: the stick's direction keeps, the corners become reachable.
  const double edge = std::max(std::fabs(ux), std::fabs(uy)) / r;
  ux = std::clamp(ux / edge, -1.0, 1.0), uy = std::clamp(uy / edge, -1.0, 1.0);
  auto toPs2 = [](double v) { return uint8_t(std::clamp(int(std::lround(127.5 + v * 127.5)), 0, 255)); };
  outX = toPs2(ux), outY = toPs2(uy);
}

// The mouse's motion this frame as a turn of the camera (game/mouse_camera.h): sensitivity 5 is
// 0.075 degrees a count, 60 degrees an inch at 800 counts an inch. Right turns the view right,
// forward looks up (unless inverted).
void applyMouse(PadInput& in) {
  int dx, dy;
  takeMouseMotion(dx, dy);
  const Settings& s = settings();
  if (!s.mouseCamera) return;
  const float perCount = 0.015f * std::clamp(s.mouseSensitivity, 1, 10) * 3.14159265f / 180;
  in.turnYaw = float(s.invertMouseX ? -dx : dx) * perCount;
  in.turnPitch = float(s.invertMouseY ? dy : -dy) * perCount;
}
}  // namespace

uint16_t padInputs(const XINPUT_GAMEPAD& g) {
  uint16_t held = g.wButtons & ~(Settings::kLeftTrigger | Settings::kRightTrigger);
  if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) held |= Settings::kLeftTrigger;
  if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) held |= Settings::kRightTrigger;
  return held;
}

bool readController(XINPUT_GAMEPAD& out) {
  XINPUT_STATE state{};
  if (XInputGetState(0, &state) == ERROR_SUCCESS) return gSource = Source::XInput, out = state.Gamepad, true;
  gSource = playstationPad(out) ? Source::PlayStation : Source::None;
  return gSource != Source::None;
}

namespace {
PadRumble gSent;  // what the controller was last told
Source gSentTo = Source::None;

void sendRumble(const PadRumble& r) {
  if (r == gSent && gSource == gSentTo) return;
  if (gSentTo != gSource && gSentTo != Source::None) {  // the controller changed: the old one stops
    if (gSentTo == Source::XInput) {
      XINPUT_VIBRATION off{};
      XInputSetState(0, &off);
    } else {
      playstationRumble(0, 0);
    }
  }
  gSent = r, gSentTo = gSource;
  // The DualShock 2's large motor is the low, strong one (an Xbox controller's left motor); its small
  // one only turns on or off, at full speed (the right, high one).
  const uint8_t high = r.smallMotor ? 255 : 0;
  if (gSource == Source::XInput) {
    XINPUT_VIBRATION v{WORD(r.largeMotor * 257), WORD(high * 257)};
    XInputSetState(0, &v);
  } else if (gSource == Source::PlayStation) {
    playstationRumble(high, r.largeMotor);
  }
}
}  // namespace

void applyRumble() { sendRumble(settings().vibration ? gPadRumble : PadRumble{}); }
void stopRumble() { sendRumble({}); }

void applyGamepad(const PadInput& keyboard) {
  XINPUT_GAMEPAD g{};
  if (!readController(g)) {
    PadInput in = keyboard;
    applyMouse(in);
    gPadInput = in;
    return;
  }
  const uint16_t held = padInputs(g);
  uint16_t buttons = keyboard.buttons;
  const Settings& s = settings();
  for (int b = 0; b < Settings::kButtons; ++b)
    if (s.pad[b] & held) buttons |= uint16_t(1u << b);
  PadInput in = keyboard;
  in.buttons = buttons;
  static constexpr uint16_t kPressure[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000,
                                             0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
  for (int i = 0; i < 12; ++i)
    if (buttons & kPressure[i]) in.pressure[i] = 255;
  uint8_t lx, ly, rx, ry;
  // The dead zone: XInput's own for each stick, or the setting's.
  const SHORT zone = SHORT(std::clamp(s.stickDeadZone, 0, 50) * 32767 / 100);
  stick(g.sThumbLX, g.sThumbLY, s.stickDeadZone ? zone : SHORT(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE), lx, ly);
  stick(g.sThumbRX, g.sThumbRY, s.stickDeadZone ? zone : SHORT(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE), rx, ry);
  // The camera's axes reversed (the right stick's: PS2 values mirrored around the centre).
  if (s.invertStickX) rx = uint8_t(255 - rx);
  if (s.invertStickY) ry = uint8_t(255 - ry);
  if (lx != 0x80 || ly != 0x80) in.lx = lx, in.ly = ly;
  if (rx != 0x80 || ry != 0x80) in.rx = rx, in.ry = ry;
  applyMouse(in);
  gPadInput = in;
}
