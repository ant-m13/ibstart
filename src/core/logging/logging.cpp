#include "core/logging/logging.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <vector>

namespace ibstart::logging {
namespace {

std::wstring Stamp(const wchar_t* format) {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  localtime_s(&local, &now);
  std::wostringstream output;
  output << std::put_time(&local, format);
  return output.str();
}
}  // namespace

std::wstring MaskSecrets(std::wstring_view arguments) {
  std::wstring result(arguments);
  // Both forms are accepted by 1C: /P secret and /P"secret". Generic token/password forms are also masked.
  const std::wregex paired(LR"mask(((?:/(?:Password|P)|--(?:password|token)|-(?:password|token))\s*(?:=\s*)?)("(?:[^"]*)"|[^\s]+))mask", std::regex_constants::icase);
  result = std::regex_replace(result, paired, L"$1***");
  const std::wregex assignment(LR"(((?:password|token|secret)\s*=\s*)(\"(?:[^\"]*)\"|[^\s]+))", std::regex_constants::icase);
  return std::regex_replace(result, assignment, L"$1***");
}

Logger::Logger(std::filesystem::path directory) : directory_(std::move(directory)) {
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  path_ = directory_ / (L"ibstart_" + Stamp(L"%Y%m%d_%H%M%S") + L".log");
  Prune();
}

void Logger::Prune() {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (const auto& item : std::filesystem::directory_iterator(directory_, error)) {
    if (!error && item.is_regular_file() && item.path().extension() == L".log") files.push_back(item.path());
  }
  std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.filename() > right.filename(); });
  for (size_t index = 10; index < files.size(); ++index) std::filesystem::remove(files[index], error);
}

void Logger::Write(std::wstring_view level, std::wstring_view message) {
  std::lock_guard lock(mutex_);
  std::ofstream output(path_, std::ios::binary | std::ios::app);
  if (!output) return;
  const auto line = Stamp(L"%Y-%m-%d %H:%M:%S") + L" [" + std::wstring(level) + L"] " + MaskSecrets(message) + L"\r\n";
  output << utf::ToUtf8(line);
}

void Logger::Info(std::wstring_view message) { Write(L"INFO", message); }
void Logger::Error(std::wstring_view message) { Write(L"ERROR", message); }

}  // namespace ibstart::logging
