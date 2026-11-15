#include "host/audio.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmreg.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "host/prologic.h"
#include "host/settings.h"

namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}
// The device named in the settings, else Windows' default.
UINT deviceFor(const std::string& name) {
  const std::wstring wanted = widen(name);
  if (wanted.empty()) return WAVE_MAPPER;
  for (UINT i = 0, n = waveOutGetNumDevs(); i < n; ++i) {
    WAVEOUTCAPSW caps{};
    if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR && wanted == caps.szPname) return i;
  }
  return WAVE_MAPPER;  // gone (unplugged): the default
}
}  // namespace

std::vector<std::wstring> audioOutputDevices() {
  std::vector<std::wstring> out;
  for (UINT i = 0, n = waveOutGetNumDevs(); i < n; ++i) {
    WAVEOUTCAPSW caps{};
    if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) out.push_back(caps.szPname);
  }
  return out;
}

struct AudioOut::Impl {
  static constexpr int kBuffers = 4, kFramesPerBuffer = 960;  // 20 ms each at 48 kHz

  HWAVEOUT device = nullptr;
  HANDLE event = nullptr;
  WAVEHDR headers[kBuffers] = {};
  std::vector<int16_t> buffers[kBuffers];
  std::mutex mutex;
  std::deque<int16_t> queue;  // interleaved samples waiting to be played, `channels` per frame
  size_t startQueue = 0, maxQueue = 0;
  bool waiting = true;  // playing silence until startQueue samples are queued
  std::atomic<bool> stop = false;
  std::thread thread;
  int rate = 0, channels = 2;
  std::string openedDevice;  // the settings it was opened with
  bool openedSurround = false;
  std::optional<ProLogicDecoder> decoder;
  std::vector<int16_t> decoded;

  // Hands every finished buffer back to the device, refilled from the queue.
  void run() {
    while (!stop) {
      WaitForSingleObject(event, 50);
      for (int i = 0; i < kBuffers; ++i) {
        WAVEHDR& h = headers[i];
        if (!(h.dwFlags & WHDR_DONE)) continue;
        std::vector<int16_t>& b = buffers[i];
        {
          std::lock_guard lock(mutex);
          if (waiting && queue.size() >= startQueue) waiting = false;
          const size_t n = waiting ? 0 : std::min(queue.size(), b.size());
          if (n < b.size()) waiting = true;  // ran dry: wait for a cushion again
          std::copy(queue.begin(), queue.begin() + ptrdiff_t(n), b.begin());
          std::fill(b.begin() + ptrdiff_t(n), b.end(), int16_t(0));
          queue.erase(queue.begin(), queue.begin() + ptrdiff_t(n));
        }
        h.dwFlags &= ~WHDR_DONE;
        waveOutWrite(device, &h, sizeof(h));
      }
    }
  }

  // Opens `deviceName` with `wanted` channels (6: 5.1); false if the device takes no such sound.
  bool start(UINT id, int wanted) {
    WAVEFORMATEXTENSIBLE f{};
    WAVEFORMATEX& format = f.Format;
    format.nChannels = WORD(wanted);
    format.nSamplesPerSec = DWORD(rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = WORD(2 * wanted);
    format.nAvgBytesPerSec = DWORD(rate) * format.nBlockAlign;
    if (wanted == 2) {
      format.wFormatTag = WAVE_FORMAT_PCM;
    } else {
      format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      f.Samples.wValidBitsPerSample = 16;
      f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT |
                        SPEAKER_BACK_RIGHT;
      f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    if (waveOutOpen(&device, id, &format, DWORD_PTR(event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) return device = nullptr, false;
    channels = wanted;
    startQueue = size_t(rate) * size_t(channels) * kStartQueueMs / 1000;
    maxQueue = size_t(rate) * size_t(channels) * kMaxQueueMs / 1000;
    for (int i = 0; i < kBuffers; ++i) {
      buffers[i].assign(size_t(kFramesPerBuffer) * size_t(channels), 0);
      WAVEHDR& h = headers[i];
      h = {};
      h.lpData = reinterpret_cast<LPSTR>(buffers[i].data());
      h.dwBufferLength = DWORD(buffers[i].size() * 2);
      waveOutPrepareHeader(device, &h, sizeof(h));
      h.dwFlags |= WHDR_DONE;  // filled and queued by the thread
    }
    return true;
  }

  void close() {
    if (!device) return;
    stop = true;
    SetEvent(event);
    if (thread.joinable()) thread.join();
    waveOutReset(device);
    for (WAVEHDR& h : headers) waveOutUnprepareHeader(device, &h, sizeof(h));
    waveOutClose(device);
    device = nullptr;
    stop = false;
    std::lock_guard lock(mutex);
    queue.clear();
    waiting = true;
  }

  bool open() {
    const Settings& s = settings();
    openedDevice = s.audioDevice, openedSurround = s.surround;
    if (!event) event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const UINT id = deviceFor(s.audioDevice);
    // 5.1 if asked for and the device takes it (Windows mixes it down for stereo speakers anyway).
    if (!(s.surround && start(id, 6)) && !start(id, 2)) return false;
    if (channels == 6) decoder.emplace(rate);
    else decoder.reset();
    thread = std::thread([this] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
      run();
    });
    SetEvent(event);
    return true;
  }
};

AudioOut::AudioOut() : impl_(std::make_unique<Impl>()) {}

AudioOut::~AudioOut() {
  impl_->close();
  if (impl_->event) CloseHandle(impl_->event);
}

bool AudioOut::open(int rate) {
  impl_->rate = rate;
  return impl_->open();
}

void AudioOut::follow() {
  Impl& m = *impl_;
  if (m.rate == 0 || (settings().audioDevice == m.openedDevice && settings().surround == m.openedSurround)) return;
  m.close();
  m.open();
}

void AudioOut::push(const int16_t* samples, size_t count) {
  Impl& m = *impl_;
  if (!m.device) return;
  const int v = settings().volume;
  std::lock_guard lock(m.mutex);
  if (m.decoder) {  // Pro Logic II into 5.1, the volume with it
    m.decoded.resize(count / 2 * 6);
    m.decoder->decode(samples, count / 2, m.decoded.data(), float(v) / 100);
    m.queue.insert(m.queue.end(), m.decoded.begin(), m.decoded.end());
  } else if (v != 100) {  // the volume setting
    for (size_t i = 0; i < count; ++i) m.queue.push_back(int16_t(samples[i] * v / 100));
  } else {
    m.queue.insert(m.queue.end(), samples, samples + count);
  }
  if (m.queue.size() > m.maxQueue) {  // whole frames dropped
    const size_t frame = size_t(m.channels), drop = (m.queue.size() - m.maxQueue + frame - 1) / frame * frame;
    m.queue.erase(m.queue.begin(), m.queue.begin() + ptrdiff_t(drop));
  }
}
