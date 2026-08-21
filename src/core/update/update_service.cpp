#include "core/update/update_service.hpp"

#include "core/domain/utf.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ibstart::update {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] = L"/repos/ant-m13/ibstart/releases/latest";
constexpr wchar_t kReleasePagePrefix[] = L"https://github.com/ant-m13/ibstart/releases/";
constexpr wchar_t kRequestHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2026-03-10\r\n";
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

struct PrereleaseIdentifier {
  std::wstring value;
  bool numeric{false};
};

struct SemanticVersion {
  std::wstring major;
  std::wstring minor;
  std::wstring patch;
  std::vector<PrereleaseIdentifier> prerelease;
};

[[noreturn]] void ThrowWinHttpError(std::string_view operation) {
  throw std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ").");
}

[[nodiscard]] bool IsAsciiDigit(wchar_t value) noexcept { return value >= L'0' && value <= L'9'; }
[[nodiscard]] bool IsAsciiLetter(wchar_t value) noexcept {
  return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
}
[[nodiscard]] bool IsPrereleaseCharacter(wchar_t value) noexcept {
  return IsAsciiDigit(value) || IsAsciiLetter(value) || value == L'-';
}
[[nodiscard]] bool IsJsonWhitespace(char value) noexcept {
  return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

void ValidateNumericIdentifier(std::wstring_view value, std::string_view part) {
  if (value.empty() || !std::all_of(value.begin(), value.end(), IsAsciiDigit)) {
    throw std::invalid_argument("Invalid " + std::string(part) + " SemVer identifier.");
  }
  if (value.size() > 1 && value.front() == L'0') {
    throw std::invalid_argument("SemVer numeric identifier has a leading zero.");
  }
}

[[nodiscard]] SemanticVersion ParseVersion(std::wstring_view value) {
  if (value.starts_with(L'v')) value.remove_prefix(1);
  const size_t prereleaseStart = value.find(L'-');
  const std::wstring_view core = value.substr(0, prereleaseStart);
  const size_t firstDot = core.find(L'.');
  const size_t secondDot = firstDot == std::wstring_view::npos ? std::wstring_view::npos : core.find(L'.', firstDot + 1);
  if (firstDot == std::wstring_view::npos || secondDot == std::wstring_view::npos || core.find(L'.', secondDot + 1) != std::wstring_view::npos) {
    throw std::invalid_argument("Version must use major.minor.patch SemVer format.");
  }

  SemanticVersion result{
      std::wstring(core.substr(0, firstDot)),
      std::wstring(core.substr(firstDot + 1, secondDot - firstDot - 1)),
      std::wstring(core.substr(secondDot + 1)),
      {}};
  ValidateNumericIdentifier(result.major, "major");
  ValidateNumericIdentifier(result.minor, "minor");
  ValidateNumericIdentifier(result.patch, "patch");

  if (prereleaseStart == std::wstring_view::npos) return result;
  const std::wstring_view prerelease = value.substr(prereleaseStart + 1);
  if (prerelease.empty()) throw std::invalid_argument("SemVer prerelease identifier is empty.");
  size_t position = 0;
  while (position <= prerelease.size()) {
    const size_t separator = prerelease.find(L'.', position);
    const std::wstring_view identifier = prerelease.substr(position, separator == std::wstring_view::npos ? std::wstring_view::npos : separator - position);
    if (identifier.empty() || !std::all_of(identifier.begin(), identifier.end(), IsPrereleaseCharacter)) {
      throw std::invalid_argument("Invalid SemVer prerelease identifier.");
    }
    const bool numeric = std::all_of(identifier.begin(), identifier.end(), IsAsciiDigit);
    if (numeric && identifier.size() > 1 && identifier.front() == L'0') {
      throw std::invalid_argument("SemVer prerelease numeric identifier has a leading zero.");
    }
    result.prerelease.push_back({std::wstring(identifier), numeric});
    if (separator == std::wstring_view::npos) break;
    position = separator + 1;
  }
  return result;
}

[[nodiscard]] int CompareNumericIdentifier(std::wstring_view left, std::wstring_view right) noexcept {
  if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
  if (left == right) return 0;
  return left < right ? -1 : 1;
}

[[nodiscard]] int CompareTextIdentifier(std::wstring_view left, std::wstring_view right) noexcept {
  if (left == right) return 0;
  return left < right ? -1 : 1;
}

[[nodiscard]] int CompareCore(const SemanticVersion& left, const SemanticVersion& right) noexcept {
  for (const auto& pair : {std::pair{&left.major, &right.major}, std::pair{&left.minor, &right.minor}, std::pair{&left.patch, &right.patch}}) {
    const int result = CompareNumericIdentifier(*pair.first, *pair.second);
    if (result != 0) return result;
  }
  return 0;
}

[[nodiscard]] std::optional<std::string> JsonStringValue(std::string_view input, std::string_view name) {
  const std::string quotedName = "\"" + std::string(name) + "\"";
  size_t position = input.find(quotedName);
  while (position != std::string_view::npos) {
    size_t cursor = position + quotedName.size();
    while (cursor < input.size() && IsJsonWhitespace(input[cursor])) ++cursor;
    if (cursor >= input.size() || input[cursor] != ':') {
      position = input.find(quotedName, position + 1);
      continue;
    }
    ++cursor;
    while (cursor < input.size() && IsJsonWhitespace(input[cursor])) ++cursor;
    if (cursor >= input.size() || input[cursor] != '"') {
      position = input.find(quotedName, position + 1);
      continue;
    }
    ++cursor;
    std::string result;
    while (cursor < input.size()) {
      const char value = input[cursor++];
      if (value == '"') return result;
      if (static_cast<unsigned char>(value) < 0x20) throw std::invalid_argument("Invalid control character in GitHub JSON string.");
      if (value != '\\') {
        result.push_back(value);
        continue;
      }
      if (cursor >= input.size()) throw std::invalid_argument("Incomplete escape sequence in GitHub JSON string.");
      const char escaped = input[cursor++];
      switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': throw std::invalid_argument("Unicode escapes are not supported in GitHub release metadata.");
        default: throw std::invalid_argument("Invalid escape sequence in GitHub JSON string.");
      }
    }
    throw std::invalid_argument("Unterminated GitHub JSON string.");
  }
  return std::nullopt;
}

[[nodiscard]] std::string ReadResponse(HINTERNET request) {
  std::string response;
  for (;;) {
    DWORD available{};
    if (!WinHttpQueryDataAvailable(request, &available)) ThrowWinHttpError("WinHttpQueryDataAvailable");
    if (available == 0) break;
    if (available > kMaximumResponseSize - response.size()) {
      throw std::runtime_error("GitHub release response exceeds the 1 MiB safety limit.");
    }
    const size_t offset = response.size();
    response.resize(offset + available);
    DWORD read{};
    if (!WinHttpReadData(request, response.data() + offset, available, &read)) ThrowWinHttpError("WinHttpReadData");
    response.resize(offset + read);
  }
  return response;
}

}  // namespace

int CompareVersions(std::wstring_view left, std::wstring_view right) {
  const SemanticVersion parsedLeft = ParseVersion(left);
  const SemanticVersion parsedRight = ParseVersion(right);
  if (const int core = CompareCore(parsedLeft, parsedRight); core != 0) return core;
  if (parsedLeft.prerelease.empty() != parsedRight.prerelease.empty()) return parsedLeft.prerelease.empty() ? 1 : -1;
  const size_t shared = std::min(parsedLeft.prerelease.size(), parsedRight.prerelease.size());
  for (size_t index = 0; index < shared; ++index) {
    const auto& leftIdentifier = parsedLeft.prerelease[index];
    const auto& rightIdentifier = parsedRight.prerelease[index];
    if (leftIdentifier.numeric != rightIdentifier.numeric) return leftIdentifier.numeric ? -1 : 1;
    const int result = leftIdentifier.numeric ? CompareNumericIdentifier(leftIdentifier.value, rightIdentifier.value)
                                              : CompareTextIdentifier(leftIdentifier.value, rightIdentifier.value);
    if (result != 0) return result;
  }
  if (parsedLeft.prerelease.size() == parsedRight.prerelease.size()) return 0;
  return parsedLeft.prerelease.size() < parsedRight.prerelease.size() ? -1 : 1;
}

Release ParseLatestReleaseResponse(std::string_view response) {
  const auto tag = JsonStringValue(response, "tag_name");
  const auto pageUrl = JsonStringValue(response, "html_url");
  if (!tag || !pageUrl) throw std::invalid_argument("GitHub release response does not contain tag_name or html_url.");
  const std::wstring version = utf::FromUtf8(*tag);
  const std::wstring page = utf::FromUtf8(*pageUrl);
  static_cast<void>(ParseVersion(version));
  if (!page.starts_with(kReleasePagePrefix)) {
    throw std::invalid_argument("GitHub release page URL does not belong to the IBStart repository.");
  }
  return {version.starts_with(L'v') ? version.substr(1) : version, page};
}

std::optional<Release> FetchLatestRelease() {
  InternetHandle session(WinHttpOpen(L"IBStart update checker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) ThrowWinHttpError("WinHttpOpen");
  if (!WinHttpSetTimeouts(session.get(), 4000, 4000, 6000, 6000)) ThrowWinHttpError("WinHttpSetTimeouts");

  InternetHandle connection(WinHttpConnect(session.get(), kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connection) ThrowWinHttpError("WinHttpConnect");
  InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", kLatestReleasePath, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request) ThrowWinHttpError("WinHttpOpenRequest");
  if (!WinHttpAddRequestHeaders(request.get(), kRequestHeaders, -1L, WINHTTP_ADDREQ_FLAG_ADD)) {
    ThrowWinHttpError("WinHttpAddRequestHeaders");
  }
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    ThrowWinHttpError("WinHttpSendRequest");
  }
  if (!WinHttpReceiveResponse(request.get(), nullptr)) ThrowWinHttpError("WinHttpReceiveResponse");

  DWORD status{};
  DWORD statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
    ThrowWinHttpError("WinHttpQueryHeaders");
  }
  if (status == kHttpNotFound) return std::nullopt;
  if (status != kHttpOk) throw std::runtime_error("GitHub returned HTTP status " + std::to_string(status) + ".");
  return ParseLatestReleaseResponse(ReadResponse(request.get()));
}

}  // namespace ibstart::update
