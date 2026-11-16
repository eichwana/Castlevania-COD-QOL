// The game's recompiled code and compiled VU micro programs live in DLLs next to the program, built
// from the player's own disc by Setup.bat (or recompile.bat), in parts so the game can start
// before all of it is built: codport_game.dll (what the game runs from its start),
// codport_game_vu.dll (the VU programs), codport_game_hot.dll (the rest of what the game runs) and
// codport_game_cold.dll (everything else). The interpreter runs whatever is not there yet; a part
// built while the game runs is loaded then (newGameCode). Without any of them the interpreter runs
// everything, slowly.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ee/vu.h"
#include "recomp/gamecode.h"
#include "recomp/runtime.h"
#include "version.h"

namespace {
std::filesystem::path besideProgram(const wchar_t* name) {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  return std::filesystem::path(std::wstring(path, n)).parent_path() / name;
}

struct Part {
  const wchar_t* dll;
  const wchar_t* ready;    // written by tools/setup/build-game.bat once the DLL is built
  const wchar_t* sources;  // its translated files, under the project's folder (the build's progress)
  HMODULE module = nullptr;
};
Part gParts[] = {{L"codport_game.dll", nullptr, L"recomp/gen/boot"},
                 {L"codport_game_vu.dll", L"codport_game_vu.ready", L"recomp/vu"},
                 {L"codport_game_hot.dll", L"codport_game_hot.ready", L"recomp/gen/hot"},
                 {L"codport_game_cold.dll", L"codport_game_cold.ready", L"recomp/gen/cold"}};

void entriesOf(HMODULE m, std::vector<rc::Entry>& out) {
  using Fn = void (*)(std::vector<rc::Entry>&);
  if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportRecompiled"))) f(out);
}
void vuBlocksOf(HMODULE m, std::vector<VuBlockEntry>& out) {
  using Fn = void (*)(std::vector<VuBlockEntry>&);
  if (auto f = reinterpret_cast<Fn>(GetProcAddress(m, "codportVuBlocks"))) f(out);
}

bool gOld = false;  // a part built by another version of the port was left out

// A part, if it was built by this version of the port: one built by another was made against other
// structures (an update extracted over an older one, before Setup builds the game again), and
// calling it could crash; the interpreter runs its code instead.
HMODULE loadPart(const Part& p) {
  HMODULE m = LoadLibraryW(besideProgram(p.dll).c_str());
  if (!m) return nullptr;
  using Fn = const char* (*)();
  const auto builtBy = reinterpret_cast<Fn>(GetProcAddress(m, "codportBuiltBy"));
  if (builtBy && std::strcmp(builtBy(), CODPORT_VERSION) == 0) return m;
  FreeLibrary(m);
  gOld = true;
  return nullptr;
}

// The parts there at start (a part still being written fails to load, and is loaded later).
void loadAtStart() {
  static bool done = false;
  if (done) return;
  done = true;
  for (Part& p : gParts) p.module = loadPart(p);
}
}  // namespace

bool gameCodeIsOld() {
  loadAtStart();
  return gOld;
}

void registerRecompiled(std::vector<rc::Entry>& out) {
  loadAtStart();
  for (const Part& p : gParts)
    if (p.module) entriesOf(p.module, out);
}

void registerVuBlocks(std::vector<VuBlockEntry>& out) {
  loadAtStart();
  for (const Part& p : gParts)
    if (p.module) vuBlocksOf(p.module, out);
}

// The entry points of the parts built since the last call (the setup builds them while the game
// runs), for the caller to add where nothing is recompiled yet.
bool newGameCode(std::vector<rc::Entry>& out) {
  bool any = false;
  for (Part& p : gParts) {
    if (p.module || !p.ready) continue;
    std::error_code ec;
    if (!std::filesystem::exists(besideProgram(p.ready), ec)) continue;
    if (!(p.module = loadPart(p))) continue;
    entriesOf(p.module, out);
    std::vector<VuBlockEntry> blocks;
    vuBlocksOf(p.module, blocks);
    if (!blocks.empty()) addVuBlocks(blocks);
    any = true;
  }
  return any;
}

namespace {
// How far the build of the parts after the first is: those done, and the one being built as far
// as the setup's log says (a "part <name>" line, then the build's "[n/m]" lines), each part
// weighing its number of translated files (the cold one is most of the work); below 0 without a log.
float buildProgress() {
  std::ifstream log(besideProgram(L"setup-rest.log"));
  if (!log) return -1;
  std::string line, current;
  float fraction = 0;
  while (std::getline(log, line)) {
    if (line.rfind("part ", 0) == 0) {
      current = line.substr(5);
      while (!current.empty() && (current.back() == '\r' || current.back() == ' ')) current.pop_back();
      fraction = 0;
    } else if (unsigned n = 0, m = 0; std::sscanf(line.c_str(), "[%u/%u]", &n, &m) == 2 && m > 0) {
      fraction = float(n) / float(m);
    }
  }
  static std::vector<float> weights;  // files of each part, counted once
  if (weights.empty())
    for (const Part& p : gParts) {
      std::error_code ec;
      float files = 0;
      for (std::filesystem::directory_iterator it(besideProgram(L"").parent_path().parent_path() / p.sources, ec), end; !ec && it != end; it.increment(ec))
        files += it->path().extension() == ".cpp";
      weights.push_back(std::max(files, 1.0f));
    }
  float done = 0, all = 0;
  for (size_t i = 1; i < std::size(gParts); ++i) {
    all += weights[i];
    const std::string name(gParts[i].dll, gParts[i].dll + std::wcslen(gParts[i].dll) - 4);  // without ".dll"
    if (gParts[i].module) done += weights[i];
    else if (name == current) done += weights[i] * fraction;
  }
  return all > 0 ? done / all : -1;
}
}  // namespace

GameCodeStatus gameCodeStatus() {
  loadAtStart();
  GameCodeStatus s;
  for (const Part& p : gParts) s.loaded += p.module != nullptr, ++s.parts;
  std::error_code ec;
  s.building = std::filesystem::exists(besideProgram(L"codport_game.building"), ec);
  s.failed = std::filesystem::exists(besideProgram(L"codport_game.failed"), ec);
  if (s.building && s.loaded < s.parts) s.progress = buildProgress();
  return s;
}
