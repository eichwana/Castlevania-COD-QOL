#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Sound output: 16-bit stereo samples pushed by the emulation are played through a device
// (waveOut): Windows' default, or the one chosen in the options (Settings::audioDevice). With
// Settings::surround the stereo, as the game's Pro Logic II option encodes it, is decoded into 5.1
// (host/prologic.h). A queue absorbs the jitter between emulated and real time: playback starts
// (and, after running dry, starts again) only once kStartQueueMs are waiting, so frames that
// arrive late or in bursts do not cut the sound into pieces. Samples that would make the queue
// longer than kMaxQueueMs are dropped.
class AudioOut {
public:
  static constexpr int kStartQueueMs = 60, kMaxQueueMs = 150;

  AudioOut();
  ~AudioOut();
  bool open(int rate);
  // Opens the device again if the device or surround setting changed since (from the settings' callback).
  void follow();
  void push(const int16_t* samples, size_t count);  // interleaved left/right

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The sound output devices' names, as the options window lists them (waveOut's, up to 31 characters).
std::vector<std::wstring> audioOutputDevices();
