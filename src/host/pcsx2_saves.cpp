#include "host/pcsx2_saves.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <vector>

#include "host/settings.h"

namespace fs = std::filesystem;

namespace {
fs::path gMemcard;

const wchar_t* T(const wchar_t* pt, const wchar_t* en) { return english() ? en : pt; }

constexpr const char* kThisGame = "BESLES-53755";  // the European disc's saves (BESLES-53755cv2eu)
// Other versions of the game, whose saves this European one cannot read.
constexpr const char* kOtherVersions[] = {"BASLUS-21168", "BISLPM-66220", "BISLPM-66221", "BASCUS-", "BIKONAMI"};

bool isThisGame(const std::string& name) { return name.rfind(kThisGame, 0) == 0; }
bool isOtherVersion(const std::string& name) {
  for (const char* p : kOtherVersions)
    if (name.rfind(p, 0) == 0) return true;
  return false;
}

struct SaveFile {
  std::string name;
  std::vector<uint8_t> data;
};
struct Save {
  std::string name;  // the save's folder on the card
  std::vector<SaveFile> files;
};

// A PS2 memory card image as PCSX2 writes it: 512-byte pages (each followed by 16 bytes of error
// correction in an 8,650,752-byte file), 2 pages per cluster, a file allocation table reached
// through two levels of indirection, directories of 512-byte entries.
class CardImage {
public:
  bool open(const fs::path& file, std::wstring& error) {
    std::ifstream in(file, std::ios::binary);
    raw_.assign(std::istreambuf_iterator<char>(in), {});
    if (raw_.size() < 0x200 || std::memcmp(raw_.data(), "Sony PS2 Memory Card Format ", 28) != 0)
      return error = T(L"não é um cartão de memória do PS2 (.ps2)", L"not a PS2 memory card image (.ps2)"), false;
    pageLen_ = u16(0x28), ppc_ = u16(0x2A);
    clusters_ = u32(0x30), allocOffset_ = u32(0x34), root_ = u32(0x3C);
    for (int i = 0; i < 32; ++i) ifc_[i] = u32(0x50 + 4 * i);
    const uint64_t pages = uint64_t(clusters_) * ppc_;
    if (pageLen_ != 512 || ppc_ == 0 || pages == 0 || raw_.size() % pages != 0)
      return error = T(L"formato de cartão que o port não conhece", L"a card layout the port does not know"), false;
    rawPage_ = uint32_t(raw_.size() / pages);
    if (rawPage_ != pageLen_ && rawPage_ != pageLen_ + 16)
      return error = T(L"formato de cartão que o port não conhece", L"a card layout the port does not know"), false;
    clusterSize_ = uint32_t(pageLen_) * ppc_;
    return true;
  }

  // The saves in the card's root folder.
  std::vector<Save> saves() {
    std::vector<Save> out;
    for (const Entry& dir : directory(root_)) {
      if (!(dir.mode & kDirectory) || dir.name == "." || dir.name == "..") continue;
      Save save{dir.name, {}};
      for (const Entry& f : directory(dir.cluster))
        if ((f.mode & kFile) && !(f.mode & kDirectory)) save.files.push_back({f.name, chain(f.cluster, f.length)});
      out.push_back(std::move(save));
    }
    return out;
  }

private:
  static constexpr uint16_t kExists = 0x8000, kDirectory = 0x0020, kFile = 0x0010;
  struct Entry {
    uint16_t mode;
    uint32_t length, cluster;
    std::string name;
  };
  uint16_t u16(size_t at) const { return at + 2 <= raw_.size() ? uint16_t(raw_[at] | raw_[at + 1] << 8) : 0; }
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= raw_.size()) std::memcpy(&v, raw_.data() + at, 4);
    return v;
  }
  // An absolute cluster's bytes (empty if outside the file).
  std::vector<uint8_t> cluster(uint32_t c) const {
    std::vector<uint8_t> out(clusterSize_);
    for (uint32_t p = 0; p < ppc_; ++p) {
      const uint64_t at = (uint64_t(c) * ppc_ + p) * rawPage_;
      if (at + pageLen_ > raw_.size()) return {};
      std::memcpy(out.data() + size_t(p) * pageLen_, raw_.data() + at, pageLen_);
    }
    return out;
  }
  uint32_t word(const std::vector<uint8_t>& c, uint32_t index) const {
    uint32_t v = 0xFFFFFFFF;
    if ((index + 1) * 4 <= c.size()) std::memcpy(&v, c.data() + index * 4, 4);
    return v;
  }
  // The table's entry for a cluster (counted from the allocation offset): the next one with the
  // top bit set, 0xFFFFFFFF at a chain's end.
  uint32_t fat(uint32_t n) const {
    const uint32_t per = clusterSize_ / 4;
    const uint32_t indirect = n / per, dbl = indirect / per;
    if (dbl >= 32) return 0xFFFFFFFF;
    const uint32_t fatCluster = word(cluster(ifc_[dbl]), indirect % per);
    return word(cluster(fatCluster), n % per);
  }
  std::vector<uint8_t> chain(uint32_t start, uint32_t bytes) const {
    std::vector<uint8_t> out;
    uint32_t c = start;
    for (uint32_t steps = 0; out.size() < bytes && steps < clusters_; ++steps) {
      const std::vector<uint8_t> data = cluster(allocOffset_ + c);
      if (data.empty()) break;
      out.insert(out.end(), data.begin(), data.end());
      const uint32_t next = fat(c);
      if (next == 0xFFFFFFFF || !(next & 0x80000000u)) break;
      c = next & 0x7FFFFFFF;
    }
    out.resize(std::min<size_t>(out.size(), bytes));
    return out;
  }
  std::vector<Entry> directory(uint32_t start) const {
    // The first entry ("."), its length: how many entries the folder has.
    const std::vector<uint8_t> first = chain(start, 512);
    if (first.size() < 512) return {};
    uint32_t count = 0;
    std::memcpy(&count, first.data() + 4, 4);
    count = std::min<uint32_t>(count, 4096);
    const std::vector<uint8_t> all = chain(start, count * 512);
    std::vector<Entry> out;
    for (size_t i = 0; i + 512 <= all.size(); i += 512) {
      Entry e{};
      std::memcpy(&e.mode, all.data() + i, 2);
      std::memcpy(&e.length, all.data() + i + 4, 4);
      std::memcpy(&e.cluster, all.data() + i + 0x10, 4);
      const char* name = reinterpret_cast<const char*>(all.data() + i + 0x40);
      e.name.assign(name, strnlen(name, 32));
      if (e.mode & kExists) out.push_back(std::move(e));
    }
    return out;
  }

  std::vector<uint8_t> raw_;
  uint32_t pageLen_ = 0, ppc_ = 0, rawPage_ = 0, clusterSize_ = 0, clusters_ = 0, allocOffset_ = 0, root_ = 0;
  uint32_t ifc_[32] = {};
};

// A PCSX2 folder memory card: a folder per save, PCSX2's own bookkeeping in _pcsx2_* files.
std::vector<Save> folderSaves(const fs::path& card) {
  std::vector<Save> out;
  std::error_code ec;
  for (const fs::directory_entry& dir : fs::directory_iterator(card, ec)) {
    if (!dir.is_directory()) continue;
    Save save{dir.path().filename().string(), {}};
    for (const fs::directory_entry& f : fs::directory_iterator(dir.path(), ec)) {
      const std::string name = f.path().filename().string();
      if (!f.is_regular_file() || name.rfind("_pcsx2_", 0) == 0) continue;
      std::ifstream in(f.path(), std::ios::binary);
      save.files.push_back({name, std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {})});
    }
    out.push_back(std::move(save));
  }
  return out;
}

std::string stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// Writes a save's folder at `to`; a folder already there is moved into `backup` first.
bool writeSave(const Save& save, const fs::path& to, const fs::path& backup, std::wstring& error) {
  std::error_code ec;
  if (fs::exists(to)) {
    fs::create_directories(backup, ec);
    fs::rename(to, backup / to.filename(), ec);
    if (ec) return error = T(L"não deu para guardar a cópia do save antigo", L"could not keep a copy of the old save"), false;
  }
  fs::create_directories(to, ec);
  for (const SaveFile& f : save.files) {
    std::ofstream out(to / f.name, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
    if (!out) return error = T(L"não deu para escrever o save", L"could not write the save"), false;
  }
  return true;
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}  // namespace

void setMemcardFolder(const fs::path& folder) { gMemcard = folder; }

std::wstring importPcsx2Saves(const fs::path& source) {
  std::vector<Save> saves;
  std::wstring error;
  if (fs::is_directory(source)) {
    saves = folderSaves(source);
  } else {
    CardImage card;
    if (!card.open(source, error)) return T(L"Nada importado: ", L"Nothing imported: ") + error + L".";
    saves = card.saves();
  }
  std::wstring report;
  int copied = 0, others = 0;
  const fs::path backup = gMemcard / ("backup-" + stamp());
  for (const Save& save : saves) {
    if (isOtherVersion(save.name)) ++others;
    if (!isThisGame(save.name) || save.files.empty()) continue;
    if (!writeSave(save, gMemcard / "card0" / save.name, backup, error)) return T(L"Parou no meio: ", L"Stopped halfway: ") + error + L".";
    report += L"  " + widen(save.name) + L"\n";
    ++copied;
  }
  if (copied == 0) {
    std::wstring text = T(L"Não há save deste jogo (versão europeia, BESLES-53755) nesse cartão.",
                          L"There is no save of this game (European version, BESLES-53755) on that card.");
    if (others)
      text += T(L"\nHá saves de outra versão do jogo (americana ou japonesa); a europeia não os lê.",
                L"\nThere are saves of another version of the game (American or Japanese); the European one cannot read them.");
    return text;
  }
  std::wstring text = T(L"Importado para o port:\n", L"Imported into the port:\n") + report;
  if (fs::exists(backup))
    text += T(L"O save que o port tinha foi guardado em ", L"The save the port had was kept in ") + backup.wstring() + L".";
  return text;
}

std::wstring exportToPcsx2Folder(const fs::path& folderCard) {
  if (!fs::exists(folderCard / "_pcsx2_superblock"))
    return T(L"Essa pasta não é um cartão de memória em pasta do PCSX2 (falta o arquivo _pcsx2_superblock). No PCSX2, crie um cartão do tipo \"Pasta\" e escolha a pasta dele, em memcards.",
             L"That folder is not a PCSX2 folder memory card (it has no _pcsx2_superblock file). In PCSX2, create a card of the \"Folder\" type and choose its folder, in memcards.");
  const std::vector<Save> saves = folderSaves(gMemcard / "card0");
  // The saves PCSX2 had go to the port's folder, not into the card (PCSX2 would take them for saves).
  const fs::path backup = gMemcard / ("backup-pcsx2-" + stamp());
  std::wstring report, error;
  int copied = 0;
  for (const Save& save : saves) {
    if (!isThisGame(save.name)) continue;
    if (!writeSave(save, folderCard / save.name, backup, error)) return T(L"Parou no meio: ", L"Stopped halfway: ") + error + L".";
    report += L"  " + widen(save.name) + L"\n";
    ++copied;
  }
  if (copied == 0) return T(L"O port ainda não tem save para exportar.", L"The port has no save to export yet.");
  std::wstring text = T(L"Exportado para o PCSX2:\n", L"Exported to PCSX2:\n") + report;
  if (fs::exists(backup)) text += T(L"O save que estava lá foi guardado em ", L"The save that was there was kept in ") + backup.wstring() + L".";
  return text;
}
