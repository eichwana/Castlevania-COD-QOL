#include "game/achievements.h"

#include <chrono>
#include <ctime>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <mutex>

#include "host/overlay.h"
#include "host/settings.h"

namespace {
enum class Kind { Area, Level, Gold };
struct Achievement {
  const char* id;
  Kind kind;
  int value;  // the area, the level or the gold
  const wchar_t *namePt, *nameEn;
};
// The story's areas in its order (src/game/game_info.cpp), the side towers, then levels and gold.
constexpr Achievement kAll[] = {
    {"area_baljhet", Kind::Area, 13, L"Baljhet Mountains", L"Baljhet Mountains"},
    {"area_garibaldi", Kind::Area, 2, L"Garibaldi Temple", L"Garibaldi Temple"},
    {"area_mortvia", Kind::Area, 10, L"Mortvia Aqueduct", L"Mortvia Aqueduct"},
    {"area_jigramunt", Kind::Area, 8, L"Forest of Jigramunt", L"Forest of Jigramunt"},
    {"area_cordova", Kind::Area, 11, L"Cordova Town", L"Cordova Town"},
    {"area_eneomaos", Kind::Area, 3, L"Eneomaos Machine Tower", L"Eneomaos Machine Tower"},
    {"area_aiolon", Kind::Area, 4, L"Aiolon Ruins", L"Aiolon Ruins"},
    {"area_corridor", Kind::Area, 9, L"Infinite Corridor", L"Infinite Corridor"},
    {"area_dracula", Kind::Area, 12, L"Dracula's Castle", L"Dracula's Castle"},
    {"area_evermore", Kind::Area, 1, L"Tower of Evermore", L"Tower of Evermore"},
    {"area_eternity", Kind::Area, 0, L"Tower of Eternity", L"Tower of Eternity"},
    {"level_10", Kind::Level, 10, L"Nível 10", L"Level 10"},
    {"level_25", Kind::Level, 25, L"Nível 25", L"Level 25"},
    {"level_50", Kind::Level, 50, L"Nível 50", L"Level 50"},
    {"level_75", Kind::Level, 75, L"Nível 75", L"Level 75"},
    {"level_99", Kind::Level, 99, L"Nível 99", L"Level 99"},
    {"gold_10000", Kind::Gold, 10000, L"10.000 de ouro", L"10,000 gold"},
    {"gold_100000", Kind::Gold, 100000, L"100.000 de ouro", L"100,000 gold"},
};
constexpr size_t kCount = std::size(kAll);

std::mutex gMutex;
std::filesystem::path gFile;
std::map<std::string, std::string> gUnlocked;  // id -> when
std::deque<size_t> gNotices;                    // waiting to be shown
int gNoticeFrame = -1;                          // frames into the one shown, -1: none

constexpr int kNoticeFrames = 330, kFadeInFrames = 15, kFadeOutFrames = 45;  // about 5.5 s at 60

std::wstring description(const Achievement& a) {
  const bool en = english();
  switch (a.kind) {
    case Kind::Area:
      return a.value == 12 ? (en ? L"Reach Dracula's Castle" : L"Chegar ao Castelo do Drácula")
                           : std::wstring(en ? L"Reach " : L"Chegar a ") + (en ? a.nameEn : a.namePt);
    case Kind::Level:
      return std::format(L"{} {}", en ? L"Hector reaches level" : L"Hector chega ao nível", a.value);
    case Kind::Gold:
      return std::wstring(en ? L"Have " : L"Ter ") + (en ? a.nameEn : a.namePt);
  }
  return {};
}

bool reached(const Achievement& a, const GameInfo& g) {
  switch (a.kind) {
    case Kind::Area: return a.value == 12 ? isDraculasCastle(g.area) : g.area == a.value;
    case Kind::Level: return g.level >= a.value;
    case Kind::Gold: return g.gold >= a.value && g.gold < 100000000;
  }
  return false;
}

std::string now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void save() {
  if (gFile.empty()) return;
  std::ofstream out(gFile, std::ios::trunc);
  out << "# Conquistas do port: id=quando / The port's achievements: id=when\n";
  for (const auto& [id, when] : gUnlocked) out << id << '=' << when << '\n';
}
}  // namespace

void loadAchievements(const std::filesystem::path& file) {
  std::lock_guard lock(gMutex);
  gFile = file;
  std::ifstream in(file);
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string id = line.substr(0, eq);
    for (const Achievement& a : kAll)
      if (id == a.id) gUnlocked[id] = line.substr(eq + 1);
  }
}

void updateAchievements(const GameInfo& info, bool playing) {
  std::lock_guard lock(gMutex);
  if (info.inGame && !info.loading) {
    bool changed = false;
    for (size_t i = 0; i < kCount; ++i) {
      const Achievement& a = kAll[i];
      if (gUnlocked.count(a.id) || !reached(a, info)) continue;
      gUnlocked[a.id] = now();
      gNotices.push_back(i);
      changed = true;
    }
    if (changed) save();
  }
  // The notice: one at a time, faded in and out.
  if (gNoticeFrame < 0 && !gNotices.empty()) gNoticeFrame = 0;
  if (gNoticeFrame < 0 || !playing) return setOverlayText(OverlaySlot::Notice, L"");  // (one shown goes on later)
  const Achievement& a = kAll[gNotices.front()];
  if (!settings().achievementNotices) {
    gNotices.clear(), gNoticeFrame = -1;
    return setOverlayText(OverlaySlot::Notice, L"");
  }
  const int f = gNoticeFrame++;
  const float alpha = f < kFadeInFrames ? float(f) / kFadeInFrames
                      : f > kNoticeFrames - kFadeOutFrames ? float(kNoticeFrames - f) / kFadeOutFrames : 1.0f;
  const std::wstring text = std::wstring(english() ? L"Achievement: " : L"Conquista: ") + (english() ? a.nameEn : a.namePt) + L"\n" + description(a);
  setOverlayText(OverlaySlot::Notice, text, alpha);
  if (gNoticeFrame >= kNoticeFrames) gNotices.pop_front(), gNoticeFrame = -1;
}

std::vector<AchievementState> achievementList() {
  std::lock_guard lock(gMutex);
  std::vector<AchievementState> out;
  for (const Achievement& a : kAll) {
    AchievementState s;
    s.name = english() ? a.nameEn : a.namePt;
    s.description = description(a);
    if (auto it = gUnlocked.find(a.id); it != gUnlocked.end()) s.unlocked = true, s.when = it->second;
    out.push_back(std::move(s));
  }
  return out;
}
