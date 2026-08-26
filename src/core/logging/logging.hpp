#pragma once

#include "core/domain/model.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace ibstart::logging {

[[nodiscard]] std::wstring MaskSecrets(std::wstring_view arguments);
[[nodiscard]] std::wstring RedactedCommandLine(const domain::LaunchCommand& command);

class Logger {
 public:
  explicit Logger(std::filesystem::path directory);
  void Info(std::wstring_view message);
  void Error(std::wstring_view message);
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  void Write(std::wstring_view level, std::wstring_view message);
  void Prune();

  std::filesystem::path directory_;
  std::filesystem::path path_;
  std::mutex mutex_;
};

}  // namespace ibstart::logging
