#include "host/update_check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <array>
#include <cstdio>
#include <mutex>
#include <thread>

#include "host/settings.h"
#include "host/window.h"
#include "version.h"

namespace {
std::mutex gMutex;
std::string gNewer;

// GitHub's answer for the project's latest release (JSON), empty on any failure.
std::string latestRelease() {
  std::string body;
  HINTERNET session = WinHttpOpen(L"codport", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return body;
  WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
  HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", L"/repos/fasipe/Castlevania-COD-QOL/releases/latest", nullptr,
                                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                 : nullptr;
  DWORD status = 0, size = sizeof(status);
  if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr) &&
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                          WINHTTP_NO_HEADER_INDEX) &&
      status == 200) {
    for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available && body.size() < (1u << 20);) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read) || !read) break;
      body.append(chunk, 0, read);
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return body;
}

// "v1.5.0" (or "1.5") as numbers; false if it is not a version.
bool parseVersion(const std::string& text, std::array<int, 3>& v) {
  v = {0, 0, 0};
  const char* p = text.c_str();
  if (*p == 'v' || *p == 'V') ++p;
  return std::sscanf(p, "%d.%d.%d", &v[0], &v[1], &v[2]) >= 2;
}
}  // namespace

void startUpdateCheck() {
  if (!settings().checkUpdates) return;
  std::thread([] {
    const std::string json = latestRelease();
    const size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos) return;
    const size_t open = json.find('"', json.find(':', key) + 1), close = json.find('"', open + 1);
    if (open == std::string::npos || close == std::string::npos) return;
    const std::string tag = json.substr(open + 1, close - open - 1);
    std::array<int, 3> latest, ours{CODPORT_VERSION_MAJOR, CODPORT_VERSION_MINOR, CODPORT_VERSION_PATCH};
    if (!parseVersion(tag, latest) || latest <= ours) return;
    {
      std::lock_guard lock(gMutex);
      gNewer = std::to_string(latest[0]) + '.' + std::to_string(latest[1]) + '.' + std::to_string(latest[2]);
    }
    std::printf("a newer version of the port is out: %s\n", tag.c_str());
    std::fflush(stdout);
    refreshTitle();
  }).detach();
}

std::string newerVersion() {
  std::lock_guard lock(gMutex);
  return gNewer;
}
