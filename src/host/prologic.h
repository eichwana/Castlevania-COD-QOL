#pragma once

#include <cstddef>
#include <cstdint>

// Dolby Pro Logic II stereo made into 5.1 (Settings::surround). The game, with its sound option on
// Pro Logic II, folds its five channels into two: the centre in phase in both, the two rear
// channels a quarter turn out of phase (-90 degrees in the left, +90 in the right, mostly to their
// own side: 0.8718 and 0.4899). This undoes that fold: a pair of all-pass filter chains gives each
// side a version a quarter turn apart, from which the rears come out free of each other; the
// centre is their sum. A little steering keeps it clean: while one direction dominates (speech in
// the centre, a sound behind), what leaks into the other speakers is taken back out. The bass of
// all of it goes to the subwoofer channel.
class ProLogicDecoder {
public:
  explicit ProLogicDecoder(int rate);
  // `frames` stereo frames in (16-bit, interleaved) to as many 5.1 frames out, in the order Windows
  // expects: front left, front right, centre, subwoofer, back left, back right.
  void decode(const int16_t* stereo, size_t frames, int16_t* out, float volume);

private:
  struct AllPass {  // one second-order all-pass section: y[n] = a2 (x[n] + y[n-2]) - x[n-2]
    float a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float run(float x) {
      const float y = a2 * (x + y2) - x2;
      x2 = x1, x1 = x, y2 = y1, y1 = y;
      return y;
    }
  };
  struct Hilbert {  // the in-phase and the quarter-turn outputs of one channel
    AllPass a[4], b[4];
    float delayedB = 0;
    void run(float x, float& inPhase, float& quarter);
  };
  Hilbert left_, right_;
  float lfe1_ = 0, lfe2_ = 0, lfeK_;            // two one-pole low-passes for the subwoofer
  float powerCentre_ = 0, powerBack_ = 0, powerLeft_ = 0, powerRight_ = 0, powerK_;  // the directions' recent loudness
};
