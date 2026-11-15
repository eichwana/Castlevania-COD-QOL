#pragma once
#include <cstdint>

struct PadInput;
struct _XINPUT_GAMEPAD;

// Sets the emulated controller (gPadInput) from the keyboard state merged with the controller
// (readController), if one is connected, its buttons mapped as the settings say (sticks map directly).
void applyGamepad(const PadInput& keyboard);

// The controller's state: the first XInput controller, else a DualShock 4 or DualSense read
// directly (host/playstation_pad.h); false if there is neither.
bool readController(_XINPUT_GAMEPAD& out);

// Makes the controller vibrate as the game makes the DualShock 2 (gPadRumble), if the setting is on:
// called each frame after applyGamepad. stopRumble() stops it (the game paused for the F1 window,
// the program ending).
void applyRumble();
void stopRumble();

// The controller inputs held, as Settings::pad names them: XInput button bits, with the triggers
// as Settings::kLeftTrigger / kRightTrigger.
uint16_t padInputs(const _XINPUT_GAMEPAD& g);
