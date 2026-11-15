#pragma once

struct _XINPUT_GAMEPAD;

// A DualShock 4 or DualSense read directly (HID, USB or Bluetooth), for when Windows has no XInput
// controller (no Steam Input or DS4Windows turning it into one): its state as an Xbox controller's,
// cross as A, circle as B, square as X, triangle as Y, Share/Create as Back, Options as Start. False
// while none is connected. The first call starts a thread that finds the controller and reads it.
bool playstationPad(_XINPUT_GAMEPAD& out);
// Its motors: `right` the high one (the DualShock 2 small motor), `left` the strong one (its large motor) (0-255). Sent in the background.
void playstationRumble(unsigned char right, unsigned char left);
