#include "host/prologic.h"

#include <algorithm>
#include <cmath>

namespace {
// The two all-pass chains whose outputs stay 90 degrees apart from about 20 Hz to 20 kHz at 48 kHz
// (Olli Niemitalo's coefficients); the second chain's output is one sample late.
constexpr float kA[4] = {0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f};
constexpr float kB[4] = {0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f};
constexpr float kNear = 0.8718f, kFar = 0.4899f;    // a rear channel's share in its own side and in the other
constexpr float kSeparation = kNear * kNear - kFar * kFar;  // what is left of a rear after the unfold
constexpr float kCentre = 0.7071f;

int16_t clip(float v) { return int16_t(std::clamp(v, -32768.0f, 32767.0f)); }
}  // namespace

void ProLogicDecoder::Hilbert::run(float x, float& inPhase, float& quarter) {
  float p = x, q = x;
  for (AllPass& s : a) p = s.run(p);
  for (AllPass& s : b) q = s.run(q);
  inPhase = p;
  quarter = delayedB;  // b lags a by a quarter turn once delayed a sample
  delayedB = q;
}

ProLogicDecoder::ProLogicDecoder(int rate) {
  for (int i = 0; i < 4; ++i) {
    left_.a[i].a2 = right_.a[i].a2 = kA[i] * kA[i];
    left_.b[i].a2 = right_.b[i].a2 = kB[i] * kB[i];
  }
  lfeK_ = 1 - std::exp(-2 * 3.14159265f * 120.0f / float(rate));  // 120 Hz
  powerK_ = 1 - std::exp(-1.0f / (0.05f * float(rate)));          // 50 ms
}

void ProLogicDecoder::decode(const int16_t* stereo, size_t frames, int16_t* out, float volume) {
  for (size_t i = 0; i < frames; ++i) {
    const float lt = stereo[2 * i], rt = stereo[2 * i + 1];
    float li, lq, ri, rq;
    left_.run(lt, li, lq);
    right_.run(rt, ri, rq);
    // The rears, unfolded from the quarter-turn versions: each comes out alone (the other cancels),
    // at kSeparation of its level.
    float backLeft = (kNear * lq + kFar * rq) / kSeparation;
    float backRight = -(kFar * lq + kNear * rq) / kSeparation;
    float centre = kCentre * (li + ri);
    // Steering: where the sound is, from the recent loudness of the sum and the difference (centre
    // or behind) and of the two sides (left or right). A passive unfold leaks every front sound into
    // the rears and the centre; these take the leak back out while one direction dominates.
    const float sum = li + ri, diff = li - ri;
    powerCentre_ += (sum * sum - powerCentre_) * powerK_;
    powerBack_ += (diff * diff - powerBack_) * powerK_;
    powerLeft_ += (li * li - powerLeft_) * powerK_;
    powerRight_ += (ri * ri - powerRight_) * powerK_;
    const float towardCentre = std::max(0.0f, (powerCentre_ - powerBack_) / (powerCentre_ + powerBack_ + 1));  // 0 to 1
    const float towardBack = std::max(0.0f, (powerBack_ - powerCentre_) / (powerCentre_ + powerBack_ + 1));
    const float toSide = std::abs(powerLeft_ - powerRight_) / (powerLeft_ + powerRight_ + 1);
    // The rears: open while a sound is behind, a little for what is all around, shut for speech
    // and for a sound at one front side (one side loud, but not behind).
    const float back = (0.35f + 0.65f * towardBack) * (1 - 0.9f * towardCentre) * (1 - toSide * (1 - towardBack));
    backLeft *= back, backRight *= back;
    // The fronts: speech goes to the centre alone; a sound behind leaves them.
    float frontLeft = li - kCentre * centre * towardCentre, frontRight = ri - kCentre * centre * towardCentre;
    frontLeft *= 1 - 0.8f * towardBack, frontRight *= 1 - 0.8f * towardBack;
    centre *= (1 - 0.7f * toSide) * (1 - 0.6f * towardBack);
    lfe1_ += (0.5f * (li + ri) - lfe1_) * lfeK_;
    lfe2_ += (lfe1_ - lfe2_) * lfeK_;
    int16_t* o = out + 6 * i;
    o[0] = clip(frontLeft * volume), o[1] = clip(frontRight * volume), o[2] = clip(centre * volume);
    o[3] = clip(lfe2_ * 0.7f * volume), o[4] = clip(backLeft * volume), o[5] = clip(backRight * volume);
  }
}
