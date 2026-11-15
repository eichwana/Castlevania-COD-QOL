#include "game/speedrun.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include "host/overlay.h"
#include "host/settings.h"

namespace {
// LiveSplit Server: one line per command, sent from a thread of its own so the game never waits on
// the network; it connects when it has something to send, and again after LiveSplit closes.
class LiveSplit {
public:
  ~LiveSplit() { stop(); }
  void send(std::string line) {
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
      queue_.push_back(std::move(line) + "\r\n");
    }
    wake_.notify_one();
  }
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

private:
  void run() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
    SOCKET s = INVALID_SOCKET;
    auto retryAt = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_ || !queue_.empty(); });
      if (stopping_ || queue_.empty()) continue;
      if (s == INVALID_SOCKET) {
        if (std::chrono::steady_clock::now() < retryAt) {
          queue_.clear();
          continue;
        }
        lock.unlock();
        s = connectLocal();
        lock.lock();
        if (s == INVALID_SOCKET) {
          retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
          // LiveSplit is not listening: what it missed is dropped (a split sent late would be
          // wrong); the game time goes again ten times a second anyway.
          queue_.clear();
          continue;
        }
      }
      std::string out;
      for (const std::string& line : queue_) out += line;
      queue_.clear();
      lock.unlock();
      const bool sent = ::send(s, out.data(), int(out.size()), 0) == int(out.size());
      lock.lock();
      if (!sent) closesocket(s), s = INVALID_SOCKET, retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
  }
  static SOCKET connectLocal() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in at{};
    at.sin_family = AF_INET, at.sin_port = htons(16834);
    inet_pton(AF_INET, "127.0.0.1", &at.sin_addr);
    // Not blocking for long: a refused connection on localhost is immediate.
    if (connect(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0) {
      closesocket(s);
      return INVALID_SOCKET;
    }
    const DWORD noDelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    return s;
  }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::string> queue_;
  std::thread thread_;
  bool stopping_ = false;
};
LiveSplit gLiveSplit;

struct Run {
  bool running = false;
  bool wasInGame = false;
  double seconds = 0;
  uint32_t areasSplit = 0;  // story areas already split on (bit per index in the story's order)
  int lastArea = -1;
  int framesSinceSync = 0;
};
Run gRun;

std::string clock(double seconds, bool forLiveSplit) {
  const auto total = uint64_t(seconds * 100);
  const uint64_t cs = total % 100, s = total / 100 % 60, m = total / 6000 % 60, h = total / 360000;
  if (forLiveSplit) return std::format("{}:{:02}:{:02}.{:02}", h, m, s, cs);
  return h ? std::format("{}:{:02}:{:02}.{:02}", h, m, s, cs) : std::format("{}:{:02}.{:02}", m, s, cs);
}
}  // namespace

void updateSpeedrun(const GameInfo& info, double frameSeconds, bool liveSplitAllowed) {
  const Settings& set = settings();
  const bool split = set.liveSplit && liveSplitAllowed;
  Run& r = gRun;
  if (info.inGame && !r.wasInGame) {  // a game begins (new or loaded): from zero
    r = Run{};
    r.running = true;
    if (split) gLiveSplit.send("reset"), gLiveSplit.send("starttimer"), gLiveSplit.send("initgametime"), gLiveSplit.send("pausegametime");
  } else if (!info.inGame && r.wasInGame) {  // back at the title
    r.running = false, r.seconds = 0;
    if (split) gLiveSplit.send("reset");
  }
  r.wasInGame = info.inGame;
  if (r.running && !info.loading) r.seconds += frameSeconds;
  if (r.running && info.area != r.lastArea) {
    // The first time in each story area after the one the run began in: a split.
    const int index = storyIndex(info.area);
    if (r.lastArea >= 0 && index > 0 && !(r.areasSplit & (1u << index))) {
      // Dracula's Castle is one area for the run, in three parts.
      bool seenCastle = false;
      for (int a : {6, 7, 12})
        if (r.areasSplit & (1u << storyIndex(a))) seenCastle = true;
      if (!(isDraculasCastle(info.area) && seenCastle) && split) gLiveSplit.send("split");
      r.areasSplit |= 1u << index;
    }
    if (r.lastArea < 0 && index >= 0) r.areasSplit |= 1u << index;  // where it began
    r.lastArea = info.area;
  }
  if (r.running && split && ++r.framesSinceSync >= 6) {  // LiveSplit shows this time, ten times a second
    r.framesSinceSync = 0;
    gLiveSplit.send("setgametime " + clock(r.seconds, true));
  }
  if (set.speedrunTimer && r.running) {
    const std::string text = clock(r.seconds, false);
    setOverlayText(OverlaySlot::Timer, std::wstring(text.begin(), text.end()));
  } else {
    setOverlayText(OverlaySlot::Timer, L"");
  }
}

void stopSpeedrun() { gLiveSplit.stop(); }
