#include "core/update/github_release_client.hpp"

#include "core/domain/version.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ibstart::update::transport {
namespace {

constexpr wchar_t kGitHubHost[] = L"github.com";
constexpr wchar_t kRequestHeaders[] = L"Accept: text/plain\r\n";
constexpr size_t kMaximumResponseSize = 1024 * 1024;
constexpr DWORD kHttpOk = 200;
constexpr DWORD kHttpNotFound = 404;

class InternetHandle {
 public:
  explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
  ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }

  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  InternetHandle(InternetHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  InternetHandle& operator=(InternetHandle&& other) noexcept {
    if (this != &other) {
      if (value_) WinHttpCloseHandle(value_);
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HINTERNET get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }

 private:
  HINTERNET value_{};
};

[[noreturn]] void ThrowWinHttpError(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + " failed (Windows error " +
      std::to_string(GetLastError()) + ").");
}

[[nodiscard]] std::string ReadResponse(HINTERNET request, std::stop_token stop) {
  std::string response;
  for (;;) {
    if (stop.stop_requested()) return {};
    DWORD available{};
    if (!WinHttpQueryDataAvailable(request, &available)) {
      ThrowWinHttpError("WinHttpQueryDataAvailable");
    }
    if (stop.stop_requested()) return {};
    if (available == 0) break;
    if (available > kMaximumResponseSize - response.size()) {
      throw std::runtime_error("GitHub version asset exceeds the 1 MiB safety limit.");
    }
    const size_t offset = response.size();
    response.resize(offset + available);
    DWORD read{};
    if (!WinHttpReadData(request, response.data() + offset, available, &read)) {
      ThrowWinHttpError("WinHttpReadData");
    }
    if (stop.stop_requested()) return {};
    response.resize(offset + read);
  }
  return response;
}

}  // namespace

std::optional<std::string> FetchLatestVersionAsset(std::stop_token stop) {
  if (stop.stop_requested()) return std::nullopt;
  InternetHandle session(WinHttpOpen(L"IBStart update checker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) ThrowWinHttpError("WinHttpOpen");
  if (!WinHttpSetTimeouts(session.get(), 4000, 4000, 6000, 6000)) {
    ThrowWinHttpError("WinHttpSetTimeouts");
  }
  if (stop.stop_requested()) return std::nullopt;

  InternetHandle connection(WinHttpConnect(session.get(), kGitHubHost,
      INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) ThrowWinHttpError("WinHttpConnect");
  if (stop.stop_requested()) return std::nullopt;
  InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET",
      version::github_latest_version_asset_path.data(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) ThrowWinHttpError("WinHttpOpenRequest");
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
      sizeof(redirectPolicy))) {
    ThrowWinHttpError("WinHttpSetOption");
  }
  if (stop.stop_requested()) return std::nullopt;
  constexpr DWORD kNullTerminatedHeaderLength = static_cast<DWORD>(-1);
  if (!WinHttpAddRequestHeaders(request.get(), kRequestHeaders, kNullTerminatedHeaderLength,
      WINHTTP_ADDREQ_FLAG_ADD)) {
    ThrowWinHttpError("WinHttpAddRequestHeaders");
  }
  if (stop.stop_requested()) return std::nullopt;
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    ThrowWinHttpError("WinHttpSendRequest");
  }
  if (stop.stop_requested()) return std::nullopt;
  if (!WinHttpReceiveResponse(request.get(), nullptr)) {
    ThrowWinHttpError("WinHttpReceiveResponse");
  }
  if (stop.stop_requested()) return std::nullopt;

  DWORD status{};
  DWORD statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    ThrowWinHttpError("WinHttpQueryHeaders");
  }
  if (stop.stop_requested()) return std::nullopt;
  if (status == kHttpNotFound) return std::nullopt;
  if (status != kHttpOk) {
    throw std::runtime_error("GitHub version asset returned HTTP status " +
        std::to_string(status) + ".");
  }
  const auto response = ReadResponse(request.get(), stop);
  if (stop.stop_requested()) return std::nullopt;
  return response;
}

}  // namespace ibstart::update::transport
