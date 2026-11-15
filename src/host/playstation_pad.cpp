#include "host/playstation_pad.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
enum class Kind { None, DualShock4, DualSense };

Kind kindOf(USHORT vendor, USHORT product) {
  if (vendor != 0x054C) return Kind::None;  // Sony
  switch (product) {
    case 0x05C4: case 0x09CC: case 0x0BA0: return Kind::DualShock4;  // DS4 (both models), its USB dongle
    case 0x0CE6: case 0x0DF2: return Kind::DualSense;                 // DualSense, DualSense Edge
  }
  return Kind::None;
}

std::mutex gMutex;
XINPUT_GAMEPAD gState{};
bool gConnected = false;

// The buttons and sticks of a report, from `d` on: the DualShock 4's layout ("A": sticks, buttons,
// then the triggers), which the DualSense also uses for its short Bluetooth report, or the
// DualSense's ("B": sticks, triggers, a counter, then buttons).
void decode(const uint8_t* d, bool layoutB, XINPUT_GAMEPAD& g) {
  const uint8_t lx = d[0], ly = d[1], rx = d[2], ry = d[3];
  const uint8_t b0 = d[layoutB ? 7 : 4], b1 = d[layoutB ? 8 : 5], l2 = d[layoutB ? 4 : 7], r2 = d[layoutB ? 5 : 8];
  WORD w = 0;
  static constexpr WORD kHat[8] = {XINPUT_GAMEPAD_DPAD_UP,
                                   XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
                                   XINPUT_GAMEPAD_DPAD_RIGHT,
                                   XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
                                   XINPUT_GAMEPAD_DPAD_DOWN,
                                   XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
                                   XINPUT_GAMEPAD_DPAD_LEFT,
                                   XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT};
  if ((b0 & 0xF) < 8) w |= kHat[b0 & 0xF];
  if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;  // square
  if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;  // cross
  if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;  // circle
  if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;  // triangle
  if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
  if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
  if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;   // Share / Create
  if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;  // Options
  if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
  if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
  // 0..255 with y down, as XInput's -32768..32767 with y up.
  auto axis = [](int v) { return SHORT(std::clamp(v * 258, -32768, 32767)); };
  g.wButtons = w;
  g.bLeftTrigger = l2, g.bRightTrigger = r2;
  g.sThumbLX = axis(lx - 128), g.sThumbLY = axis(128 - ly);
  g.sThumbRX = axis(rx - 128), g.sThumbRY = axis(128 - ry);
}

// One input report (with its report ID first); false if it is none of the controller's state.
bool parse(Kind kind, bool usb, const uint8_t* r, DWORD n, XINPUT_GAMEPAD& g) {
  if (kind == Kind::DualShock4) {
    if (r[0] == 0x01 && n >= 10) return decode(r + 1, false, g), true;   // USB, and Bluetooth's short report
    if (r[0] == 0x11 && n >= 12) return decode(r + 3, false, g), true;   // Bluetooth's full report
  } else {
    if (r[0] == 0x01 && usb && n >= 11) return decode(r + 1, true, g), true;
    if (r[0] == 0x01 && !usb && n >= 10) return decode(r + 1, false, g), true;  // Bluetooth's short report
    if (r[0] == 0x31 && n >= 12) return decode(r + 2, true, g), true;            // Bluetooth's full report
  }
  return false;
}

struct Device {
  HANDLE handle = INVALID_HANDLE_VALUE;
  Kind kind = Kind::None;
  DWORD reportBytes = 0, outputBytes = 0;
};

// ---- the motors --------------------------------------------------------------------------------
// Output reports as SDL's HIDAPI drivers send them: over USB plain, over Bluetooth longer and
// ending in a CRC-32 of 0xA2 and the report.
struct Output {
  HANDLE handle = INVALID_HANDLE_VALUE;  // a handle of its own, so writing never waits on a read
  Kind kind = Kind::None;
  bool usb = true;
  DWORD bytes = 0;
};
std::mutex gOutMutex;
std::condition_variable gOutChanged;
Output gOut;
uint8_t gWantRight = 0, gWantLeft = 0;
uint64_t gWanted = 0;  // counts requests (and controllers connecting), so that each is sent once

uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

std::vector<uint8_t> rumbleReport(const Output& o, uint8_t right, uint8_t left) {
  // As long as the controller's output reports (WriteFile wants exactly that), at least the report.
  const DWORD least = !o.usb ? 78 : o.kind == Kind::DualShock4 ? 32 : 48;
  std::vector<uint8_t> r(std::max<DWORD>(o.bytes, least), 0);
  if (o.kind == Kind::DualShock4) {
    if (o.usb) r[0] = 0x05, r[1] = 0x01, r[4] = right, r[5] = left;  // flags: motors only
    else r[0] = 0x11, r[1] = 0xC4, r[3] = 0x01, r[6] = right, r[7] = left;
  } else {
    if (o.usb) r[0] = 0x02, r[1] = 0x03, r[3] = right, r[4] = left;  // flags: compatible vibration, no audio haptics
    else r[0] = 0x31, r[1] = 0x02, r[2] = 0x03, r[4] = right, r[5] = left;
  }
  if (!o.usb) {
    const uint8_t header = 0xA2;
    const uint32_t crc = crc32(crc32(0, &header, 1), r.data(), 74);
    std::memcpy(r.data() + 74, &crc, 4);
  }
  return r;
}

// Sends each new state of the motors to the controller.
void writeLoop() {
  uint64_t sent = 0;
  for (;;) {
    std::unique_lock lock(gOutMutex);
    gOutChanged.wait(lock, [&] { return gWanted != sent; });
    sent = gWanted;
    const Output o = gOut;
    const uint8_t right = gWantRight, left = gWantLeft;
    lock.unlock();
    if (o.handle == INVALID_HANDLE_VALUE) continue;
    std::vector<uint8_t> report = rumbleReport(o, right, left);
    DWORD written = 0;
    if (!WriteFile(o.handle, report.data(), DWORD(report.size()), &written, nullptr))
      HidD_SetOutputReport(o.handle, report.data(), ULONG(report.size()));
  }
}

void setOutput(const Output& o) {
  std::lock_guard lock(gOutMutex);
  if (gOut.handle != INVALID_HANDLE_VALUE) CloseHandle(gOut.handle);
  gOut = o;
  ++gWanted;  // a controller just connected gets the motors' state
  gOutChanged.notify_one();
}

// The first DualShock 4 or DualSense among the HID devices (its gamepad collection), opened.
Device findController() {
  Device found;
  GUID hid;
  HidD_GetHidGuid(&hid);
  HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return found;
  SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
  for (DWORD i = 0; found.kind == Kind::None && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
    std::vector<uint8_t> buffer(size);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr)) continue;
    HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    HIDD_ATTRIBUTES a{sizeof(a)};
    PHIDP_PREPARSED_DATA data = nullptr;
    HIDP_CAPS caps{};
    const Kind kind = HidD_GetAttributes(h, &a) ? kindOf(a.VendorID, a.ProductID) : Kind::None;
    if (kind != Kind::None && HidD_GetPreparsedData(h, &data)) {
      const bool ok = HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5;  // a game pad
      HidD_FreePreparsedData(data);
      if (ok) {
        found = {h, kind, caps.InputReportByteLength, caps.OutputReportByteLength};
        Output out{CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr), kind,
                   caps.InputReportByteLength == 64, caps.OutputReportByteLength};
        setOutput(out);
        break;
      }
    }
    CloseHandle(h);
  }
  SetupDiDestroyDeviceInfoList(set);
  return found;
}

// Finds the controller (looking again every 2 seconds while there is none) and reads its reports
// as they come (about 250 a second); a failed read is the controller going away.
void readLoop() {
  for (;;) {
    Device d = findController();
    if (d.kind == Kind::None) {
      Sleep(2000);
      continue;
    }
    const bool usb = d.reportBytes == 64;  // Bluetooth reports are longer
    std::vector<uint8_t> report(std::max<DWORD>(d.reportBytes, 64));
    DWORD read = 0;
    while (ReadFile(d.handle, report.data(), DWORD(report.size()), &read, nullptr)) {
      XINPUT_GAMEPAD g{};
      if (read == 0 || !parse(d.kind, usb, report.data(), read, g)) continue;
      std::lock_guard lock(gMutex);
      gState = g, gConnected = true;
    }
    {
      std::lock_guard lock(gMutex);
      gState = {}, gConnected = false;
    }
    setOutput({});
    CloseHandle(d.handle);
    Sleep(500);
  }
}
}  // namespace

bool playstationPad(XINPUT_GAMEPAD& out) {
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread(readLoop).detach();
    std::thread(writeLoop).detach();
  });
  std::lock_guard lock(gMutex);
  if (gConnected) out = gState;
  return gConnected;
}

void playstationRumble(unsigned char right, unsigned char left) {
  std::lock_guard lock(gOutMutex);
  if (right == gWantRight && left == gWantLeft) return;
  gWantRight = right, gWantLeft = left;
  ++gWanted;
  gOutChanged.notify_one();
}
