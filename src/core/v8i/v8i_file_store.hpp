#pragma once

#include "core/v8i/v8i_document.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace ibstart::v8i {

class ExternalModificationError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class V8iFileStore {
 public:
  explicit V8iFileStore(std::filesystem::path path);

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] V8iDocument Read();
  void Save(const V8iDocument& document);
  [[nodiscard]] std::vector<std::filesystem::path> Backups() const;

 private:
  struct Fingerprint {
    uintmax_t size{};
    std::filesystem::file_time_type write_time{};
    std::uint64_t content_hash{};
    bool operator==(const Fingerprint&) const = default;
  };

  [[nodiscard]] static std::optional<Fingerprint> FingerprintOf(const std::filesystem::path& path);
  void CreateBackup() const;
  void PruneBackups() const;

  std::filesystem::path path_;
  std::optional<Fingerprint> loaded_fingerprint_;
};

}  // namespace ibstart::v8i
