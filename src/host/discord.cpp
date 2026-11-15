#include "host/discord.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "host/settings.h"

namespace {
std::string escaped(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (uint8_t(c) < 0x20) out += std::format("\\u{:04x}", int(uint8_t(c)));
    else out += c;
  }
  return out;
}

class Discord {
public:
  ~Discord() { stop(); }
  void set(const std::string& details, const std::string& state) {
    std::lock_guard lock(mutex_);
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    details_ = details, state_ = state;
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
  // A frame: opcode, length, then the JSON (little-endian, as Discord's own library writes it).
  bool write(uint32_t op, const std::string& json) {
    std::vector<char> frame(8 + json.size());
    const uint32_t length = uint32_t(json.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, json.data(), json.size());
    DWORD written = 0;
    return WriteFile(pipe_, frame.data(), DWORD(frame.size()), &written, nullptr) && written == frame.size();
  }
  // Discord's answers are read and dropped (none is needed), so its side of the pipe never fills;
  // false when it closed the pipe (Discord quit, or did not accept the application's id).
  bool drain() {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return false;
    while (available > 0) {
      char buffer[4096];
      DWORD got = 0;
      if (!ReadFile(pipe_, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr)) return false;
      available -= std::min(available, got);
      // In the console, what matters of the answers: connected, or why not.
      const std::string_view text(buffer, got);
      if (text.find("\"evt\":\"READY\"") != std::string_view::npos) std::printf("discord: connected\n");
      if (const size_t at = text.find("\"message\":\""); at != std::string_view::npos && text.find("\"evt\":\"ERROR\"") != std::string_view::npos)
        std::printf("discord: %.*s\n", int(std::min<size_t>(120, text.size() - at)), text.data() + at);
    }
    return true;
  }
  void close() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);  // Discord clears the presence
    pipe_ = INVALID_HANDLE_VALUE;
    sent_.clear();
  }
  bool open(const std::string& clientId) {
    for (int i = 0; i < 10 && pipe_ == INVALID_HANDLE_VALUE; ++i) {
      const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
      pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!write(0, std::format(R"({{"v":1,"client_id":"{}"}})", escaped(clientId)))) return close(), false;
    return true;
  }
  void run() {
    const int64_t started = int64_t(std::time(nullptr));
    auto nextAttempt = std::chrono::steady_clock::now();
    auto nextUpdate = nextAttempt;
    uint64_t nonce = 0;
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
      if (stopping_) break;
      const Settings& s = settings();
      const std::string clientId = s.discordClientId;
      const bool wanted = s.discord && !clientId.empty();
      const std::string details = details_, state = state_;
      lock.unlock();
      const auto now = std::chrono::steady_clock::now();
      if (!wanted) close();
      else if (pipe_ == INVALID_HANDLE_VALUE && now >= nextAttempt && !open(clientId)) nextAttempt = now + std::chrono::seconds(10);
      if (pipe_ != INVALID_HANDLE_VALUE && !drain()) close(), nextAttempt = now + std::chrono::seconds(10);
      // Discord takes an update every 15 seconds at most: the latest then.
      const std::string json = details.empty() ? std::string(R"("state":"Menu")") : std::format(R"("details":"{}","state":"{}")", escaped(details), escaped(state));
      if (pipe_ != INVALID_HANDLE_VALUE && json != sent_ && now >= nextUpdate) {
        const std::string message = std::format(R"({{"cmd":"SET_ACTIVITY","args":{{"pid":{},"activity":{{{},"timestamps":{{"start":{}}}}}}},"nonce":"{}"}})",
                                                GetCurrentProcessId(), json, started, ++nonce);
        if (write(1, message)) sent_ = json, nextUpdate = now + std::chrono::seconds(15);
        else close(), nextAttempt = now + std::chrono::seconds(10);
      }
      lock.lock();
    }
    lock.unlock();
    close();
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread thread_;
  bool stopping_ = false;
  std::string details_, state_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;  // only the thread's
  std::string sent_;                    // the activity Discord has
};
Discord gDiscord;
}  // namespace

void setDiscordPresence(const std::string& details, const std::string& state) { gDiscord.set(details, state); }
void stopDiscord() { gDiscord.stop(); }
