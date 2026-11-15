#include "host/hang_report.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<int64_t> gLastBeat{0};  // steady clock, milliseconds
std::atomic<bool> gPaused{false};
int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Stack {
  DWORD id;
  std::wstring name;
  std::vector<DWORD64> frames;
};

// A suspended thread's return addresses, by the unwind data every x64 function has (nothing is
// allocated meanwhile: the thread may hold the heap's lock).
size_t unwind(CONTEXT context, DWORD64* out, size_t max) {
  size_t n = 0;
  while (n < max && context.Rip) {
    out[n++] = context.Rip;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
    if (!function) {  // a leaf function: its return address is on top of the stack
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handlerData = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handlerData, &establisher, nullptr);
  }
  return n;
}

std::vector<Stack> stacks() {
  std::vector<Stack> out;
  const DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  std::vector<DWORD> ids;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    if (te.th32OwnerProcessID == process && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
  CloseHandle(snap);
  for (DWORD id : ids) {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (!t) continue;
    Stack s{id, {}, {}};
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(t, &name)) && name) s.name = name, LocalFree(name);
    std::array<DWORD64, 64> frames{};
    size_t n = 0;
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &context)) n = unwind(context, frames.data(), frames.size());
      ResumeThread(t);
    }
    CloseHandle(t);
    s.frames.assign(frames.begin(), frames.begin() + ptrdiff_t(n));
    out.push_back(std::move(s));
  }
  return out;
}

std::string describe(HANDLE process, DWORD64 address) {
  std::string module = "?";
  HMODULE m = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &m)) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    module = path;
    module = module.substr(module.find_last_of("\\/") + 1);
  }
  std::string text = std::format("{}+{:#x}", module, m ? address - DWORD64(m) : address);
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO), symbol->MaxNameLen = 255;
  DWORD64 offset = 0;
  if (SymFromAddr(process, address, &offset, symbol)) text += std::format("  {}", symbol->Name);
  IMAGEHLP_LINE64 line{sizeof(line)};
  DWORD displacement = 0;
  if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
    std::string file = line.FileName;
    text += std::format("  ({}:{})", file.substr(file.find_last_of("\\/") + 1), line.LineNumber);
  }
  return text;
}

void report(const std::filesystem::path& folder, const char* why, const char* name = "travamento",
            const std::vector<DWORD64>* crashed = nullptr) {
  const std::vector<Stack> all = stacks();  // first, before anything else moves
  const HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  const std::filesystem::path file =
      folder / std::format("{}-{:04}{:02}{:02}-{:02}{:02}{:02}.txt", name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  std::ofstream out(file);
  out << why;
  if (crashed) {
    out << "\n\n== the thread that failed\n";
    for (DWORD64 a : *crashed) out << "   " << describe(process, a) << '\n';
  }
  out << "\nWhere each thread was:\n";
  for (const Stack& s : all) {
    out << std::format("\n== thread {}", s.id);
    if (!s.name.empty()) out << " (" << std::string(s.name.begin(), s.name.end()) << ")";
    out << '\n';
    for (DWORD64 a : s.frames) out << "   " << describe(process, a) << '\n';
  }
  SymCleanup(process);
  std::printf("%s: where it was is in %s\n", why, file.string().c_str());
}

std::filesystem::path gCrashFolder;
LONG WINAPI onCrash(EXCEPTION_POINTERS* e) {
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
  std::array<DWORD64, 64> frames{};
  const size_t n = unwind(*e->ContextRecord, frames.data(), frames.size());
  const std::vector<DWORD64> crashed(frames.begin(), frames.begin() + ptrdiff_t(n));
  const std::string why = std::format("The game failed (exception {:#x} at {:#x}).", uint32_t(e->ExceptionRecord->ExceptionCode),
                                      uint64_t(e->ExceptionRecord->ExceptionAddress));
  report(gCrashFolder, why.c_str(), "falha", &crashed);
  std::fflush(stdout);
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void startCrashReport(const std::filesystem::path& folder) {
  gCrashFolder = folder;
  SetUnhandledExceptionFilter(onCrash);
}

void hangWatchBeat() { gLastBeat = nowMs(); }
void hangWatchPause(bool paused) {
  gPaused = paused;
  if (!paused) gLastBeat = nowMs();
}

void startHangWatch(void* window, const std::filesystem::path& folder) {
  HWND hwnd = static_cast<HWND>(window);
  std::thread([hwnd, folder] {
    SetThreadDescription(GetCurrentThread(), L"hang watch");
    for (;;) {
      Sleep(1000);
      if (!IsWindow(hwnd)) return;
      const bool stopped = !gPaused && gLastBeat > 0 && nowMs() - gLastBeat > 5000;
      const bool hung = IsHungAppWindow(hwnd) != FALSE;
      if (stopped || hung) {  // the game stopped, or Windows already thinks so
        report(folder, hung ? "Windows took the game window for hung." : "The game made no frame for 5 seconds.");
        return;  // once
      }
    }
  }).detach();
}
