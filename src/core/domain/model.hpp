#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ibstart::domain {

enum class ClientBitness { automatic, x64, x86 };
enum class ClientType { automatic, thick, thin, web };
enum class LaunchMode { enterprise, designer, web_client };

struct Field {
  std::wstring key;
  std::wstring value;
};

struct Entry {
  std::wstring name;
  std::vector<Field> fields;  // Ordering, including unknown fields, is significant.

  [[nodiscard]] const Field* Find(std::wstring_view key) const;
  [[nodiscard]] Field* Find(std::wstring_view key);
  [[nodiscard]] std::wstring ValueOr(std::wstring_view key, std::wstring_view fallback = L"") const;
  void Set(std::wstring_view key, std::wstring value);
  [[nodiscard]] bool IsDatabase() const;
  [[nodiscard]] bool IsGroup() const;
};

struct Database {
  std::wstring id;
  std::wstring name;
  std::wstring connect;
  std::wstring folder;
  std::wstring order_in_list;
  std::wstring version;
  std::wstring app;
  std::wstring default_app;
  std::wstring wa;
  std::wstring external;
  std::wstring locale;
  std::wstring client_connection_speed;
  std::wstring additional_parameters;
  std::vector<Field> unknown_fields;
};

struct PlatformInstallation {
  std::filesystem::path executable;
  std::wstring version;
  ClientBitness bitness{ClientBitness::automatic};
  bool has_thin_client{false};
};

struct LaunchOptions {
  LaunchMode mode{LaunchMode::enterprise};
  ClientBitness bitness{ClientBitness::automatic};
  ClientType client_type{ClientType::automatic};
  std::wstring version{L"Авто"};
  std::wstring common_parameters;
  std::wstring individual_parameters;
};

struct LaunchCommand {
  std::filesystem::path executable;
  std::vector<std::wstring> arguments;
  [[nodiscard]] std::wstring CommandLine() const;
};

struct HistoryItem {
  std::wstring database_id;
  std::chrono::system_clock::time_point timestamp{};
  LaunchMode mode{LaunchMode::enterprise};
};

}  // namespace ibstart::domain
