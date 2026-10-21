#include <winsock2.h>

#include "app/instance_activation.hpp"
#include "core/catalog/catalog.hpp"
#include "core/catalog/catalog_metadata_service.hpp"
#include "core/cache/cache_service.hpp"
#include "core/connection/connection_string.hpp"
#include "core/credentials/credentials.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/launcher/parameter_catalog.hpp"
#include "core/launcher/parameter_help.hpp"
#include "core/launcher/parameter_options.hpp"
#include "core/logging/logging.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/platform/platform_version.hpp"
#include "core/scanner/file_base_scanner.hpp"
#include "core/storage/storage.hpp"
#include "core/update/github_release_client.hpp"
#include "core/update/update_service.hpp"
#include "core/v8i/v8i_file_store.hpp"
#include "core/windows_path.hpp"
#include "ui/cache_size_operation.hpp"
#include "ui/tree_presentation.hpp"
#include "ui/tree_view_controller.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <exception>
#include <cstdint>
#include <mutex>
#include <set>
#include <string_view>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::wcerr << L"FAILED " << __FUNCTION__ << L":" << __LINE__ << L"\n"; ++failures; } } while (false)

std::string ReadBytes(const std::filesystem::path& path) { std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}; }
void WriteBytes(const std::filesystem::path& path, std::string_view text) { std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(text.data(), static_cast<std::streamsize>(text.size())); }
std::filesystem::path Fixture(const wchar_t* name) { return std::filesystem::current_path() / L"tests" / L"fixtures" / name; }
std::filesystem::path Temp(const wchar_t* suffix) { auto path = std::filesystem::temp_directory_path() / (std::wstring(L"ibstart-tests-") + suffix + L"-" + std::to_wstring(GetCurrentProcessId())); std::error_code error; std::filesystem::remove_all(path, error); std::filesystem::create_directories(path); return path; }

class ScopedEnvironmentVariable final {
 public:
  ScopedEnvironmentVariable(const wchar_t* name, const std::filesystem::path& value) : name_(name) {
    const DWORD required = GetEnvironmentVariableW(name_.c_str(), nullptr, 0);
    if (required != 0) {
      previous_.resize(required);
      const DWORD copied = GetEnvironmentVariableW(name_.c_str(), previous_.data(), required);
      previous_.resize(copied);
      had_previous_ = true;
    }
    SetEnvironmentVariableW(name_.c_str(), value.c_str());
  }

  ~ScopedEnvironmentVariable() {
    SetEnvironmentVariableW(name_.c_str(), had_previous_ ? previous_.c_str() : nullptr);
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

 private:
  std::wstring name_;
  std::wstring previous_;
  bool had_previous_{false};
};

std::wstring WindowsSystemErrorMessage(DWORD error) {
  wchar_t buffer[512]{};
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
  if (!length) return {};
  std::wstring message(buffer, length);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
  return message;
}

class LoopbackHttpServer {
 public:
  explicit LoopbackHttpServer(std::string response = {}) : response_(std::move(response)) {
    WSADATA data{};
    const int startup_error = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_error != 0) {
      throw std::runtime_error("WSAStartup failed (Windows error " + std::to_string(startup_error) + ").");
    }
    winsock_started_ = true;
    try {
      listener_.store(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
      if (listener_.load() == INVALID_SOCKET) Fail("socket");

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      if (bind(listener_.load(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        Fail("bind");
      }
      if (listen(listener_.load(), 1) == SOCKET_ERROR) Fail("listen");

      sockaddr_in bound{};
      int bound_size = sizeof(bound);
      if (getsockname(listener_.load(), reinterpret_cast<sockaddr*>(&bound), &bound_size) == SOCKET_ERROR) {
        Fail("getsockname");
      }
      port_ = ntohs(bound.sin_port);
      accept_thread_ = std::thread([this] { AcceptConnection(); });
    } catch (...) {
      Shutdown();
      throw;
    }
  }

  ~LoopbackHttpServer() { Shutdown(); }

  LoopbackHttpServer(const LoopbackHttpServer&) = delete;
  LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  [[nodiscard]] bool WaitForClient(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return client_accepted_ || accept_failed_; });
  }

 private:
  static void CloseSocket(SOCKET socket) noexcept {
    if (socket == INVALID_SOCKET) return;
    shutdown(socket, SD_BOTH);
    closesocket(socket);
  }

  [[noreturn]] void Fail(const char* operation) {
    const int error = WSAGetLastError();
    Shutdown();
    throw std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(error) + ").");
  }

  void AcceptConnection() noexcept {
    const SOCKET listener = listener_.load();
    const SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      std::lock_guard lock(mutex_);
      accept_failed_ = true;
      condition_.notify_all();
      return;
    }

    bool keep_client = false;
    {
      std::lock_guard lock(mutex_);
      if (!stopping_) {
        client_ = client;
        client_accepted_ = true;
        keep_client = true;
      }
      condition_.notify_all();
    }
    if (!keep_client) {
      CloseSocket(client);
      return;
    }

    if (!response_.empty()) {
      // Behave like an HTTP server: read complete request headers before
      // sending the response. Sending immediately after accept can make
      // WinHTTP report ERROR_WINHTTP_INVALID_SERVER_RESPONSE on fast runs.
      const DWORD receive_timeout = 2000;
      if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
          reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)) == SOCKET_ERROR) return;
      std::string headers;
      char buffer[2048]{};
      while (headers.find("\r\n\r\n") == std::string::npos && headers.size() < 8192) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) return;
        headers.append(buffer, static_cast<size_t>(received));
      }
      if (headers.find("\r\n\r\n") == std::string::npos) return;
      size_t offset = 0;
      while (offset < response_.size()) {
        const int sent = send(client, response_.data() + offset,
            static_cast<int>(response_.size() - offset), 0);
        if (sent == SOCKET_ERROR || sent == 0) break;
        offset += static_cast<size_t>(sent);
      }
      return;
    }

    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return stopping_; });
  }

  void Shutdown() noexcept {
    SOCKET listener = listener_.exchange(INVALID_SOCKET);
    SOCKET client = INVALID_SOCKET;
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      client = std::exchange(client_, INVALID_SOCKET);
    }
    CloseSocket(listener);
    CloseSocket(client);
    condition_.notify_all();
    if (accept_thread_.joinable()) accept_thread_.join();
    if (winsock_started_) {
      WSACleanup();
      winsock_started_ = false;
    }
  }

  std::atomic<SOCKET> listener_{INVALID_SOCKET};
  SOCKET client_{INVALID_SOCKET};
  std::uint16_t port_{};
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_{false};
  bool client_accepted_{false};
  bool accept_failed_{false};
  bool winsock_started_{false};
  std::string response_;
  std::thread accept_thread_;
};

std::filesystem::path SystemExecutable(const wchar_t* name) {
  wchar_t systemDirectory[MAX_PATH]{};
  const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
  if (length == 0 || length >= std::size(systemDirectory)) return {};
  return std::filesystem::path(systemDirectory) / name;
}

bool CopySystemExecutable(const std::filesystem::path& target) {
  const auto source = SystemExecutable(L"cmd.exe");
  std::error_code error;
  if (source.empty() || !std::filesystem::is_regular_file(source, error) || error) return false;
  return std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error) && !error;
}

bool CreateJunction(const std::filesystem::path& junction, const std::filesystem::path& target) {
  std::wstring command_line = L"cmd.exe /d /c mklink /J \"" + junction.wstring() + L"\" \"" + target.wstring() + L"\"";
  std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, mutable_command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
  DWORD exit_code = 1;
  if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
  else TerminateProcess(process.hProcess, exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return wait == WAIT_OBJECT_0 && exit_code == 0;
}

DWORD CreateDirectorySymbolicLink(const std::filesystem::path& link, const std::filesystem::path& target) {
  if (CreateSymbolicLinkW(link.c_str(), target.c_str(),
                          SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    return ERROR_SUCCESS;
  }
  const DWORD first_error = GetLastError();
  if (first_error == ERROR_INVALID_PARAMETER &&
      CreateSymbolicLinkW(link.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY)) {
    return ERROR_SUCCESS;
  }
  return GetLastError();
}

void RemoveDirectoryLink(const std::filesystem::path& link) {
  if (!RemoveDirectoryW(link.c_str())) {
    std::error_code error;
    std::filesystem::remove(link, error);
  }
}

bool EntryExistsWithoutFollowingReparsePoint(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::exists(status);
}

void TestCheckMacroAcceptsCommaExpressions() {
  CHECK(std::vector<int>{1, 2, 3} == std::vector<int>{1, 2, 3});
}

void TestV8iRoundTrip() {
  const auto bytes = ReadBytes(Fixture(L"nested-unicode.v8i"));
  const auto document = ibstart::v8i::V8iDocument::ParseUtf8(bytes);
  CHECK(document.encoding == ibstart::v8i::Utf8Encoding::utf8_bom);
  CHECK(document.sections.size() == 4);
  const auto* database = document.Find(L"База с пробелом");
  CHECK(database != nullptr);
  CHECK(database->entry.ValueOr(L"UnknownVendorKey") == L"не менять");
  const auto serialized = document.SerializeUtf8();
  CHECK(serialized == bytes);
  CHECK(serialized.starts_with("\xEF\xBB\xBF"));
  const auto again = ibstart::v8i::V8iDocument::ParseUtf8(serialized);
  const auto* reread = again.Find(L"База с пробелом");
  CHECK(reread && reread->entry.ValueOr(L"UnknownVendorKey") == L"не менять");
  CHECK(reread && reread->entry.ValueOr(L"Connect") == L"File=\"C:\\Рабочие базы\\Моя база\"");
  const auto withoutFinalNewline = ibstart::v8i::V8iDocument::ParseUtf8("[Base]\nConnect=x");
  CHECK(withoutFinalNewline.SerializeUtf8() == "[Base]\nConnect=x");

  constexpr std::string_view interleaved =
      "[Base]\nConnect=x\n; keep between fields\nUnknown=one\n\nFolder=/\n";
  auto interleavedDocument = ibstart::v8i::V8iDocument::ParseUtf8(interleaved);
  CHECK(interleavedDocument.SerializeUtf8() == interleaved);
  interleavedDocument.Find(L"Base")->entry.Set(L"Connect", L"updated");
  CHECK(interleavedDocument.SerializeUtf8() ==
      "[Base]\nConnect=updated\n; keep between fields\nUnknown=one\n\nFolder=/\n");

  auto removableFirstSection = ibstart::v8i::V8iDocument::ParseUtf8(
      "; file header\n[First]\nConnect=x\n[Second]\nConnect=y\n");
  CHECK(removableFirstSection.Remove(L"First"));
  CHECK(removableFirstSection.SerializeUtf8() == "; file header\n[Second]\nConnect=y\n");
}

void TestDemoCatalogFixture() {
  const auto bytes = ReadBytes(Fixture(L"ibases.v8i"));
  CHECK(bytes.find("/P") == std::string::npos);
  CHECK(bytes.find("C:\\Users\\") == std::string::npos);
  CHECK(bytes.find(".example.com") != std::string::npos);

  auto document = ibstart::v8i::V8iDocument::ParseUtf8(bytes);
  CHECK(document.sections.size() == 12);

  ibstart::catalog::Catalog showcase(std::move(document));
  const auto tree = showcase.Tree();
  CHECK(tree.size() == 4);
  CHECK(tree[0].name == L"Файловые базы");
  CHECK(tree[1].name == L"Клиент-серверные базы");
  CHECK(tree[2].name == L"Веб-базы");

  const auto* fileBase = showcase.Find(L"Бухгалтерия предприятия 3.0 — демонстрация");
  CHECK(fileBase != nullptr);
  CHECK(fileBase->ValueOr(L"CustomDemoField") == L"Показать неизвестное поле");

  const auto* serverBase = showcase.Find(L"ERP Управление предприятием — демонстрационная");
  CHECK(serverBase != nullptr);
  CHECK(serverBase->ValueOr(L"Connect") == L"Srvr=\"demo-cluster.example.com\";Ref=\"ERP_Demo\"");

  const auto* modernWeb = showcase.Find(L"Веб-портал демонстрации");
  CHECK(modernWeb != nullptr);
  CHECK(ibstart::catalog::Catalog::IsWebConnection(modernWeb->ValueOr(L"Connect")));

  const auto* legacyWeb = showcase.Find(L"Веб-база в legacy-формате");
  CHECK(legacyWeb != nullptr);
  CHECK(!ibstart::catalog::IsBareWebConnection(legacyWeb->ValueOr(L"Connect")));
}

void TestProductVersion() {
  CHECK(!ibstart::version::value.empty());
  const auto core = std::to_wstring(ibstart::version::major) + L"." + std::to_wstring(ibstart::version::minor) + L"." + std::to_wstring(ibstart::version::patch);
  const auto file = core + L"." + std::to_wstring(ibstart::version::revision);
  CHECK(ibstart::version::value.starts_with(core));
  CHECK(ibstart::version::file_value == file);
  const auto versionAssetPath = L"/" + std::wstring(ibstart::version::github_owner) + L"/" + std::wstring(ibstart::version::github_repository) + L"/releases/latest/download/IBStart.version";
  const auto releasePage = L"https://github.com/" + std::wstring(ibstart::version::github_owner) + L"/" + std::wstring(ibstart::version::github_repository) + L"/releases/";
  CHECK(ibstart::version::github_latest_version_asset_path == versionAssetPath);
  CHECK(ibstart::version::github_release_page_prefix == releasePage);
}

void TestUnicodeCaseInsensitiveSearch() {
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КД2", L"кд") == 0);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"Рабочая КД3", L"кд") == 8);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"Alpha BETA", L"beta") == 6);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"retail3", L"etai") == 1);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КД2", L"д3") == std::wstring_view::npos);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КДКД", L"кд", 2) == 2);
}

void TestConnectionStringParsing() {
  const std::wstring connect = L"  FILE = \"C:\\Базы;Основная\" ; Srvr = \"ignored\" ; Ref = demo ; Custom = keep  ";
  const auto parts = ibstart::connection::Split(connect);
  CHECK(parts.size() == 4);
  CHECK(parts.size() > 0 && parts[0] == L"FILE = \"C:\\Базы;Основная\"");
  CHECK(ibstart::connection::ValueOrEmpty(connect, L"file") == L"C:\\Базы;Основная");
  CHECK(ibstart::connection::ValueOrEmpty(connect, L"SRVR") == L"ignored");
  CHECK(ibstart::connection::ValueOrEmpty(connect, L"missing").empty());
  CHECK(ibstart::connection::QuoteValue(L"A\"B") == L"\"A\"\"B\"");

  const auto quoted = ibstart::connection::Parse(
      L"Key=\"a;b\";Key=\"a\\\"b\";Double=\"a\"\"b\";Custom = keep");
  CHECK(quoted.diagnostics.empty());
  CHECK(quoted.fragments.size() == 4);
  CHECK(quoted.fragments.size() > 1 && quoted.fragments[1].value == L"a\"b");
  CHECK(quoted.fragments.size() > 2 && quoted.fragments[2].value == L"a\"b");
  CHECK(quoted.fragments.size() > 3 && quoted.fragments[3].raw == L"Custom = keep");
  CHECK(ibstart::connection::ValueOrEmpty(L"Key=first;Key=second", L"Key") == L"first");
  const auto malformed = ibstart::connection::Parse(L"File=\"C:\\broken;Ref=base");
  CHECK(!malformed.diagnostics.empty());

  const auto keyedWeb = ibstart::connection::WebUrl(L"WS = \"https://example.test/base;part\" ; WA=1");
  CHECK(keyedWeb && *keyedWeb == L"https://example.test/base;part");
  const auto legacyWeb = ibstart::connection::WebUrl(L" https://example.test/base ; Custom = keep");
  CHECK(legacyWeb && *legacyWeb == L"https://example.test/base");
  CHECK(ibstart::connection::IsBareWebUrl(L" https://example.test/base "));
  CHECK(!ibstart::connection::IsBareWebUrl(L"https://example.test/base;Custom=keep"));
  CHECK(ibstart::connection::IsValidHttpUrl(L"https://example.test/base"));
  CHECK(ibstart::connection::IsValidHttpUrl(L"https://[::1]:8443/base;part?x=1"));
  CHECK(ibstart::connection::IsValidHttpUrl(L"https://[fe80::1%25eth0]/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"ftp://example.test/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://[2001:db8::1"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://2001:db8::1/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://[not-an-ip]/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://example.test:abc/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://example.test:65536/base"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://example.test/base with spaces"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://example.test/base\"quote"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://example.test/base\x01"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https://"));
  CHECK(!ibstart::connection::IsValidHttpUrl(L"https:///base"));
  const auto quotedLegacyWeb = ibstart::connection::WebUrl(
      L"\"https://example.test/base;part?x=1\";Custom=keep");
  CHECK(quotedLegacyWeb && *quotedLegacyWeb == L"https://example.test/base;part?x=1");
  CHECK(!ibstart::connection::WebUrl(L"https://example.test/base;part"));
  CHECK(!ibstart::connection::WebUrl(L"https://example.test/base?x=1;y=2"));
  CHECK(ibstart::connection::IsBareWebUrl(L"\"https://example.test/base;part?x=1\""));
  CHECK(!ibstart::connection::IsBareWebUrl(L"\"https://example.test/base;part?x=1\";Custom=keep"));

  const auto fileConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::file,
      L"https://old.example/base;Custom=keep;Another=fragment",
      L"C:\\new", L"", L"", L"");
  CHECK(fileConnection == L"File=\"C:\\new\";Custom=keep;Another=fragment");
  const auto serverConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::server,
      L"https://old.example/base;Custom=keep;Another=fragment",
      L"", L"", L"cluster", L"database");
  CHECK(serverConnection == L"Srvr=\"cluster\";Ref=\"database\";Custom=keep;Another=fragment");
  const auto webConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::web,
      L"https://old.example/base;Custom=keep;Another=fragment",
      L"", L"https://new.example/base", L"", L"");
  CHECK(webConnection == L"WS=\"https://new.example/base\";Custom=keep;Another=fragment");
  const auto semicolonWebConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::web, L"", L"",
      L"https://new.example/base;part?x=1", L"", L"");
  CHECK(semicolonWebConnection == L"WS=\"https://new.example/base;part?x=1\"");
  const auto quotedWebConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::file,
      L"\"https://old.example/base;part?x=1\";Custom=keep",
      L"C:\\new", L"", L"", L"");
  CHECK(quotedWebConnection == L"File=\"C:\\new\";Custom=keep");
  bool invalidWebConnection = false;
  try {
    static_cast<void>(ibstart::connection::BuildConnection(
        ibstart::connection::ConnectionKind::web, L"", L"", L"https://", L"", L""));
  } catch (const std::invalid_argument&) {
    invalidWebConnection = true;
  }
  CHECK(invalidWebConnection);
  const auto orderedConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::server,
      L"Before=first;File=\"old\";After=second",
      L"", L"", L"cluster", L"database");
  CHECK(orderedConnection == L"Before=first;Srvr=\"cluster\";Ref=\"database\";After=second");
  const auto interleavedConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::server,
      L"Srvr=\"old-cluster\";Custom=between;Ref=\"old-database\";Tail=keep",
      L"", L"", L"cluster", L"database");
  CHECK(interleavedConnection ==
      L"Srvr=\"cluster\";Custom=between;Ref=\"database\";Tail=keep");
  const auto reversedConnection = ibstart::connection::BuildConnection(
      ibstart::connection::ConnectionKind::server,
      L"Ref=\"old-database\";Custom=between;Srvr=\"old-cluster\";Tail=keep",
      L"", L"", L"cluster", L"database");
  CHECK(reversedConnection ==
      L"Ref=\"database\";Custom=between;Srvr=\"cluster\";Tail=keep");
}

void TestNoBomAndCatalog() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(ReadBytes(Fixture(L"no-bom-unknown.v8i")));
  CHECK(document.encoding == ibstart::v8i::Utf8Encoding::utf8);
  ibstart::catalog::Catalog catalog(std::move(document));
  CHECK(catalog.Databases().size() == 1);
  CHECK(ibstart::catalog::Catalog::IsWebConnection(catalog.Databases().front()->ValueOr(L"Connect")));
  CHECK(catalog.DatabaseFor(L"Web").unknown_fields.size() == 1);
  auto architectureDocument = ibstart::v8i::V8iDocument::ParseUtf8(
      "[AppArch base]\nConnect=Srvr=\"server\";Ref=\"base\"\nAppArch=x86_64_prt\nDefaultVersion=8.3.27\nOrderInTree=42\n");
  ibstart::catalog::Catalog architectureCatalog(std::move(architectureDocument));
  CHECK(architectureCatalog.DatabaseFor(L"AppArch base").app_arch == L"x86_64_prt");
  CHECK(architectureCatalog.DatabaseFor(L"AppArch base").default_version == L"8.3.27");
  CHECK(architectureCatalog.DatabaseFor(L"AppArch base").order_in_tree == L"42");
  CHECK(architectureCatalog.DatabaseFor(L"AppArch base").unknown_fields.empty());
}

void TestSafeStore() {
  const auto directory = Temp(L"store"); const auto file = directory / L"ibases.v8i";
  WriteBytes(file, "[Base]\r\nConnect=File=\"C:\\\\base\"\r\nUnknown=x\r\n");
  ibstart::v8i::V8iFileStore store(file); auto document = store.Read(); document.Find(L"Base")->entry.Set(L"Locale", L"ru_RU"); store.Save(document);
  CHECK(std::filesystem::exists(file)); CHECK(store.Backups().size() == 1); CHECK(store.maintenance_warnings().empty()); CHECK(ReadBytes(file).find("Locale=ru_RU") != std::string::npos);
  const auto unrelatedBackup = directory / L"ibases.v8i.bak_notes";
  WriteBytes(unrelatedBackup, "user file");
  CHECK(store.Backups().size() == 1);
  for (int index = 0; index != 6; ++index) { document.Find(L"Base")->entry.Set(L"OrderInList", std::to_wstring(index)); store.Save(document); }
  CHECK(store.Backups().size() == 5); CHECK(store.maintenance_warnings().empty());
  CHECK(std::filesystem::exists(unrelatedBackup)); CHECK(ReadBytes(unrelatedBackup) == "user file");
  bool temporaryLeftBehind = false; for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.path().filename().wstring().find(L".ibstart.tmp.") != std::wstring::npos) temporaryLeftBehind = true;
  CHECK(!temporaryLeftBehind);
  ibstart::v8i::V8iFileStore conflict(file); auto another = conflict.Read();
  const auto unchangedTime = std::filesystem::last_write_time(file); auto external = ReadBytes(file); const auto basePosition = external.find("Base"); CHECK(basePosition != std::string::npos); if (basePosition != std::string::npos) external[basePosition] = 'X'; WriteBytes(file, external); std::filesystem::last_write_time(file, unchangedTime);
  bool caught = false; try { conflict.Save(another); } catch (const ibstart::v8i::ExternalModificationError&) { caught = true; }
  CHECK(caught); CHECK(ReadBytes(file) == external);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestV8iFileSizeLimit() {
  const auto directory = Temp(L"store-size-limit");
  const auto file = directory / L"ibases.v8i";
  WriteBytes(file, "");
  std::filesystem::resize_file(file, ibstart::v8i::kMaxV8iFileSize + 1);

  bool rejected = false;
  std::string diagnostic;
  try {
    ibstart::v8i::V8iFileStore store(file);
    static_cast<void>(store.Read());
  } catch (const std::exception& error) {
    rejected = true;
    diagnostic = error.what();
  }
  CHECK(rejected);
  CHECK(diagnostic.find("safety limit") != std::string::npos);
  CHECK(diagnostic.find("16777217") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestConfirmedV8iOverwrite() {
  const auto directory = Temp(L"store-overwrite");
  const auto file = directory / L"ibases.v8i";
  WriteBytes(file, std::string("invalid-") + static_cast<char>(0xFF));
  ibstart::v8i::V8iFileStore store(file);
  store.AcceptCurrentContentsForOverwrite();
  const auto replacement = ibstart::v8i::V8iDocument::ParseUtf8("[Replacement]\nConnect=x\n");
  store.Save(replacement);
  CHECK(ReadBytes(file) == "[Replacement]\nConnect=x\n");
  CHECK(store.Backups().size() == 1);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCommandBuilderAndSelection() {
  const std::vector<ibstart::domain::PlatformInstallation> platforms = {
    {L"C:\\Program Files\\1cv8\\8.3.9\\bin\\1cv8.exe", L"8.3.9", ibstart::domain::ClientBitness::x64, true},
    {L"C:\\Program Files (x86)\\1cv8\\8.3.24\\bin\\1cv8.exe", L"8.3.24", ibstart::domain::ClientBitness::x86, true},
    {L"C:\\Program Files\\1cv8\\8.3.24\\bin\\1cv8.exe", L"8.3.24", ibstart::domain::ClientBitness::x64, true}};
  ibstart::domain::LaunchOptions options; options.mode = ibstart::domain::LaunchMode::designer; const auto chosen = ibstart::launcher::SelectPlatform(platforms, options);
  CHECK(chosen && chosen->bitness == ibstart::domain::ClientBitness::x64);
  options.bitness = ibstart::domain::ClientBitness::x86; const auto x86 = ibstart::launcher::SelectPlatform(platforms, options); CHECK(x86 && x86->bitness == ibstart::domain::ClientBitness::x86); options.bitness = ibstart::domain::ClientBitness::automatic;
  options.architecture = ibstart::domain::ClientArchitecture::x64; const auto x64 = ibstart::launcher::SelectPlatform(platforms, options); CHECK(x64 && x64->bitness == ibstart::domain::ClientBitness::x64);
  options.architecture = ibstart::domain::ClientArchitecture::x64_priority; const auto priority64 = ibstart::launcher::SelectPlatform(platforms, options); CHECK(priority64 && priority64->bitness == ibstart::domain::ClientBitness::x64);
  options.architecture = ibstart::domain::ClientArchitecture::x86_priority; const auto priority86 = ibstart::launcher::SelectPlatform(platforms, options); CHECK(priority86 && priority86->bitness == ibstart::domain::ClientBitness::x86);
  options.architecture = ibstart::domain::ClientArchitecture::automatic;
  options.version = L"8.3"; const auto versionPrefix = ibstart::launcher::SelectPlatform(platforms, options); CHECK(versionPrefix && versionPrefix->version == L"8.3.24");
  options.version = L"Авто";
  const std::vector<ibstart::domain::PlatformInstallation> largeVersions = {
      {L"C:\\1cv8\\8.3.99999999999999999999\\bin\\1cv8.exe", L"8.3.99999999999999999999", ibstart::domain::ClientBitness::x64, true},
      {L"C:\\1cv8\\8.3.100000000000000000000\\bin\\1cv8.exe", L"8.3.100000000000000000000", ibstart::domain::ClientBitness::x64, true}};
  const auto largestVersion = ibstart::launcher::SelectPlatform(largeVersions, options);
  CHECK(largestVersion && largestVersion->version == L"8.3.100000000000000000000");
  CHECK(!ibstart::platform::IsNewerVersion(L"8.3.027", L"8.3.27"));
  CHECK(!ibstart::platform::IsNewerVersion(L"8.3.27", L"8.3.027"));
  const std::vector<ibstart::domain::PlatformInstallation> equivalentVersions = {
      {L"C:\\1cv8\\8.3.027\\bin\\1cv8.exe", L"8.3.027", ibstart::domain::ClientBitness::x64, true},
      {L"C:\\1cv8\\8.3.27\\bin\\1cv8.exe", L"8.3.27", ibstart::domain::ClientBitness::x86, true}};
  const auto preferredEquivalentVersion = ibstart::launcher::SelectPlatform(equivalentVersions, options);
  CHECK(preferredEquivalentVersion && preferredEquivalentVersion->bitness == ibstart::domain::ClientBitness::x64);

  const auto mixedClientDirectory = Temp(L"mixed-client-selection");
  const auto mixedThinExecutable = mixedClientDirectory / L"1cv8c.exe";
  const auto mixedThickExecutable = mixedClientDirectory / L"1cv8.exe";
  if (!CopySystemExecutable(mixedThinExecutable) || !CopySystemExecutable(mixedThickExecutable)) {
    std::error_code cleanupError;
    std::filesystem::remove_all(mixedClientDirectory, cleanupError);
    return;
  }
  const std::vector<ibstart::domain::PlatformInstallation> mixedClients = {
      {mixedThinExecutable, L"8.3.30", ibstart::domain::ClientBitness::x64, true},
      {mixedThickExecutable, L"8.3.29", ibstart::domain::ClientBitness::x64, true}};
  options.mode = ibstart::domain::LaunchMode::designer;
  options.client_type = ibstart::domain::ClientType::automatic;
  const auto designerPlatform = ibstart::launcher::SelectPlatform(mixedClients, options);
  CHECK(designerPlatform && designerPlatform->executable.filename() == L"1cv8.exe");
  const std::span<const ibstart::domain::PlatformInstallation> standaloneOnly(mixedClients.data(), 1);
  CHECK(!ibstart::launcher::SelectPlatform(standaloneOnly, options));
  options.mode = ibstart::domain::LaunchMode::enterprise;
  options.client_type = ibstart::domain::ClientType::thick;
  const auto thickPlatform = ibstart::launcher::SelectPlatform(mixedClients, options);
  CHECK(thickPlatform && thickPlatform->executable.filename() == L"1cv8.exe");
  options.client_type = ibstart::domain::ClientType::thin;
  const auto thinOnlyPlatform = ibstart::launcher::SelectPlatform(mixedClients, options);
  CHECK(thinOnlyPlatform && thinOnlyPlatform->executable.filename() == L"1cv8c.exe");
  options.client_type = ibstart::domain::ClientType::automatic;
  options.mode = ibstart::domain::LaunchMode::designer;
  std::error_code mixedCleanupError;
  std::filesystem::remove_all(mixedClientDirectory, mixedCleanupError);

  CHECK(ibstart::launcher::ParseAppArchitecture(L"x86_64_prt") == ibstart::domain::ClientArchitecture::x64_priority);
  CHECK(ibstart::launcher::ParseAppArchitecture(L"x86") == ibstart::domain::ClientArchitecture::x86);
  CHECK(!ibstart::launcher::ParseAppArchitecture(L"x64"));
  CHECK(ibstart::launcher::AppArchitectureFromParameters(L"/N Иван /AppArch x86_64_prt") == ibstart::domain::ClientArchitecture::x64_priority);
  CHECK(ibstart::launcher::AppArchitectureFromParameters(L"/AppArch=x86") == ibstart::domain::ClientArchitecture::x86);
  ibstart::domain::Database file; file.connect = L"File=\"C:\\Базы 1С\\Тест\""; file.additional_parameters = L"/N \"Иван Иванов\"";
  const auto fileCommand = ibstart::launcher::BuildCommand(file, *chosen, options);
  CHECK(fileCommand.arguments[0] == L"DESIGNER"); CHECK(fileCommand.arguments[1] == L"/F"); CHECK(fileCommand.arguments[2] == L"C:\\Базы 1С\\Тест");
  ibstart::domain::Database server; server.connect = L"Srvr=\"srv\";Ref=\"base\""; options.mode = ibstart::domain::LaunchMode::enterprise;
  const auto serverCommand = ibstart::launcher::BuildCommand(server, *chosen, options); CHECK(serverCommand.arguments[1] == L"/S"); CHECK(serverCommand.arguments[2] == L"srv\\base");
  CHECK(ibstart::launcher::QuoteWindowsArgument(L"a b\\") == L"\"a b\\\\\"");
  const auto split = ibstart::launcher::SplitCommandArguments(L"/N \"\" /P \"a b\"");
  CHECK(split.size() == 4); CHECK(split.size() > 1 && split[1].empty()); CHECK(split.size() > 3 && split[3] == L"a b");
  ibstart::domain::Database spaced; spaced.connect = L" FILE = \"C:\\base;one\" ;";
  const auto spacedCommand = ibstart::launcher::BuildCommand(spaced, *chosen, options); CHECK(spacedCommand.arguments[1] == L"/F"); CHECK(spacedCommand.arguments[2] == L"C:\\base;one");
  options.client_type = ibstart::domain::ClientType::thin; options.mode = ibstart::domain::LaunchMode::designer;
  bool invalidThinDesigner = false; try { (void)ibstart::launcher::BuildCommand(file, *chosen, options); } catch (const std::invalid_argument&) { invalidThinDesigner = true; } CHECK(invalidThinDesigner);
  options.client_type = ibstart::domain::ClientType::thick; options.mode = ibstart::domain::LaunchMode::enterprise;
  bool invalidStandaloneThick = false; try { (void)ibstart::launcher::BuildCommand(file, mixedClients.front(), options); } catch (const std::invalid_argument&) { invalidStandaloneThick = true; } CHECK(invalidStandaloneThick);

  const auto thinDirectory = Temp(L"thin-web-client");
  const auto thickExecutable = thinDirectory / L"1cv8.exe";
  const auto thinExecutable = thinDirectory / L"1cv8c.exe";
  if (!CopySystemExecutable(thickExecutable) || !CopySystemExecutable(thinExecutable)) {
    std::error_code cleanupError;
    std::filesystem::remove_all(thinDirectory, cleanupError);
    return;
  }
  const ibstart::domain::PlatformInstallation thinPlatform{thickExecutable, L"8.3.27", ibstart::domain::ClientBitness::x64, true};
  ibstart::domain::Database web; web.connect = L"WS = \"https://example.test/base\" ; WA=1";
  options.mode = ibstart::domain::LaunchMode::enterprise;
  options.client_type = ibstart::domain::ClientType::thin;
  const auto webCommand = ibstart::launcher::BuildCommand(web, thinPlatform, options);
  CHECK(webCommand.executable == thinExecutable);
  CHECK(webCommand.arguments.size() == 3);
  CHECK(webCommand.arguments.size() > 2 && webCommand.arguments[0] == L"ENTERPRISE" && webCommand.arguments[1] == L"/WS" && webCommand.arguments[2] == L"https://example.test/base");
  options.client_type = ibstart::domain::ClientType::automatic;
  options.mode = ibstart::domain::LaunchMode::designer;
  bool invalidWebDesigner = false; try { (void)ibstart::launcher::BuildCommand(web, thinPlatform, options); } catch (const std::invalid_argument&) { invalidWebDesigner = true; } CHECK(invalidWebDesigner);
  std::error_code thinCleanupError;
  std::filesystem::remove_all(thinDirectory, thinCleanupError);
}

void TestOneOffLaunchOptions() {
  const std::vector<ibstart::domain::PlatformInstallation> platforms = {
      {L"C:\\Program Files\\1cv8\\8.3.27\\bin\\1cv8.exe", L"8.3.27", ibstart::domain::ClientBitness::x64, true},
      {L"C:\\Program Files (x86)\\1cv8\\8.3.27\\bin\\1cv8.exe", L"8.3.27", ibstart::domain::ClientBitness::x86, true}};
  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::enterprise;
  options.platform_executable = platforms[1].executable;
  const auto selected = ibstart::launcher::SelectPlatform(platforms, options);
  CHECK(selected && selected->executable == platforms[1].executable);

  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\Bases\\Demo\"";
  database.additional_parameters = L"/N OldUser";
  options.user_name = L"Administrator";
  options.password = L"secret value";
  options.override_individual_parameters = true;
  options.individual_parameters.clear();
  const auto command = ibstart::launcher::BuildCommand(database, *selected, options);
  CHECK(command.arguments.size() == 7);
  CHECK(command.arguments[0] == L"ENTERPRISE");
  CHECK(command.arguments[1] == L"/F");
  CHECK(command.arguments[2] == L"C:\\Bases\\Demo");
  CHECK(command.arguments[3] == L"/N");
  CHECK(command.arguments[4] == L"Administrator");
  CHECK(command.arguments[5] == L"/P");
  CHECK(command.arguments[6] == L"secret value");
  CHECK(ibstart::logging::ContainsSecretArguments(command));
  CHECK(ibstart::logging::RedactedCommandLine(command).find(L"secret value") == std::wstring::npos);

  options.password.clear();
  options.user_name.clear();
  options.individual_parameters = L"/AppArch x86";
  const auto overrideCommand = ibstart::launcher::BuildCommand(database, *selected, options);
  CHECK(overrideCommand.arguments.size() == 5);
  CHECK(overrideCommand.arguments[3] == L"/AppArch");
  CHECK(overrideCommand.arguments[4] == L"x86");
}

void TestLaunchParameterConflicts() {
  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\base\"";
  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::enterprise;

  const auto fileSpec = ibstart::launcher::ParseConnectionSpec(database.connect);
  CHECK(fileSpec.kind == ibstart::launcher::ConnectionSpec::Kind::file);
  CHECK(fileSpec.value == L"C:\\base");

  database.connect = L"File=\"C:\\base\";Srvr=server;Ref=base";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.connect = L"\"https://example.test/base;part?x=1\";Custom=keep";
  const auto quotedWebSpec = ibstart::launcher::ParseConnectionSpec(database.connect);
  CHECK(quotedWebSpec.kind == ibstart::launcher::ConnectionSpec::Kind::web);
  CHECK(quotedWebSpec.value == L"https://example.test/base;part?x=1");
  bool ambiguousLegacyWeb = false;
  try { static_cast<void>(ibstart::launcher::ParseConnectionSpec(L"https://example.test/base;part")); }
  catch (const std::invalid_argument&) { ambiguousLegacyWeb = true; }
  CHECK(ambiguousLegacyWeb);
  bool invalidBareWeb = false;
  try { static_cast<void>(ibstart::launcher::ParseConnectionSpec(L"https://")); }
  catch (const std::invalid_argument&) { invalidBareWeb = true; }
  CHECK(invalidBareWeb);
  database.connect = L"File=\"C:\\base\"";

  database.additional_parameters = L"/F C:\\\\other";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  const ibstart::domain::PlatformInstallation platform{L"C:\\1cv8.exe", L"8.3.27",
      ibstart::domain::ClientBitness::x64, true};
  bool rawConnectionOverride = false;
  try { static_cast<void>(ibstart::launcher::BuildCommand(database, platform, options)); }
  catch (const std::invalid_argument&) { rawConnectionOverride = true; }
  CHECK(rawConnectionOverride);
  database.additional_parameters = L"ENTERPRISE";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/N DESIGNER /P CREATEINFOBASE /Execute DESIGNER";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/N /P";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/Execute";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/Proxy proxy /NoProxy";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/AppArch invalid";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/AppArch x86 /AppArch x64";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters = L"/URL https://example.test/base";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.additional_parameters.clear();
  database.app_arch = L"x64";
  const auto invalidDatabaseArchitecture = ibstart::launcher::ValidateLaunchParameters(database, options);
  CHECK(!invalidDatabaseArchitecture.empty());
  CHECK(invalidDatabaseArchitecture.front().find(L"x64") != std::wstring::npos);
  CHECK(database.app_arch == L"x64");
  bool invalidDatabaseArchitectureCommand = false;
  try { static_cast<void>(ibstart::launcher::BuildCommand(database, platform, options)); }
  catch (const std::invalid_argument&) { invalidDatabaseArchitectureCommand = true; }
  CHECK(invalidDatabaseArchitectureCommand);
  database.app_arch = L"  x86_64  ";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch = L"x86";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch = L"x86_prt";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch = L"x86_64_prt";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch = L"win64";
  database.additional_parameters = L"/AppArch x86";
  CHECK(!ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch = L"x86_64";
  CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
  database.app_arch.clear();
  database.additional_parameters.clear();

  ibstart::domain::Database webDatabase;
  webDatabase.connect = L"https://example.test/base;Custom=keep";
  const auto browserUrl = ibstart::launcher::BrowserFallbackUrl(webDatabase, options);
  CHECK(browserUrl && *browserUrl == L"https://example.test/base");
  webDatabase.app_arch = L"win64";
  bool invalidBrowserAppArchitecture = false;
  try { static_cast<void>(ibstart::launcher::BrowserFallbackUrl(webDatabase, options)); }
  catch (const std::invalid_argument&) { invalidBrowserAppArchitecture = true; }
  CHECK(invalidBrowserAppArchitecture);
  webDatabase.app_arch.clear();
  webDatabase.connect = L"https://example.test/base;File=other";
  bool invalidMixedBrowserConnection = false;
  try { static_cast<void>(ibstart::launcher::BrowserFallbackUrl(webDatabase, options)); }
  catch (const std::invalid_argument&) { invalidMixedBrowserConnection = true; }
  CHECK(invalidMixedBrowserConnection);
  webDatabase.connect = L"WS=\"https://example.test/base\";Srvr=other;Ref=database";
  bool invalidTypedBrowserConnection = false;
  try { static_cast<void>(ibstart::launcher::BrowserFallbackUrl(webDatabase, options)); }
  catch (const std::invalid_argument&) { invalidTypedBrowserConnection = true; }
  CHECK(invalidTypedBrowserConnection);
  webDatabase.connect = L"WS=\"https://example.test/base";
  bool invalidSyntaxBrowserConnection = false;
  try { static_cast<void>(ibstart::launcher::BrowserFallbackUrl(webDatabase, options)); }
  catch (const std::invalid_argument&) { invalidSyntaxBrowserConnection = true; }
  CHECK(invalidSyntaxBrowserConnection);
  webDatabase.connect = L"WS=https://";
  bool invalidUrlBrowserConnection = false;
  try { static_cast<void>(ibstart::launcher::BrowserFallbackUrl(webDatabase, options)); }
  catch (const std::invalid_argument&) { invalidUrlBrowserConnection = true; }
  CHECK(invalidUrlBrowserConnection);
  database.additional_parameters.clear();
  const auto fileBrowserUrl = ibstart::launcher::BrowserFallbackUrl(database, options);
  CHECK(!fileBrowserUrl);

  bool invalidArchitecture = false;
  try { static_cast<void>(ibstart::launcher::AppArchitectureFromParameters(L"/AppArch invalid")); }
  catch (const std::invalid_argument&) { invalidArchitecture = true; }
  CHECK(invalidArchitecture);
  bool repeatedArchitecture = false;
  try { static_cast<void>(ibstart::launcher::AppArchitectureFromParameters(L"/AppArch x86 /AppArch x64")); }
  catch (const std::invalid_argument&) { repeatedArchitecture = true; }
  CHECK(repeatedArchitecture);
}

void TestPlatformDiscoveryLargeVersions() {
  const auto root = Temp(L"platform-versions");
  const auto smaller = root / L"8.3.99999999999999999999" / L"bin";
  const auto larger = root / L"8.3.100000000000000000000" / L"bin";
  std::filesystem::create_directories(smaller);
  std::filesystem::create_directories(larger);
  if (!CopySystemExecutable(smaller / L"1cv8.exe") || !CopySystemExecutable(larger / L"1cv8.exe")) {
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return;
  }

  const auto discovered = ibstart::platform::Discover({root}, false);
  const auto smallerPosition = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == smaller / L"1cv8.exe"; });
  const auto largerPosition = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == larger / L"1cv8.exe"; });
  CHECK(smallerPosition != discovered.end());
  CHECK(largerPosition != discovered.end());
  if (smallerPosition != discovered.end() && largerPosition != discovered.end()) CHECK(largerPosition < smallerPosition);

  const auto first_root = root / L"first" / L"8.3.27" / L"bin";
  const auto second_root = root / L"second" / L"8.3.27" / L"bin";
  std::filesystem::create_directories(first_root);
  std::filesystem::create_directories(second_root);
  if (CopySystemExecutable(first_root / L"1cv8.exe") && CopySystemExecutable(second_root / L"1cv8.exe")) {
    const auto priority_discovered = ibstart::platform::Discover({root / L"second", root / L"first"}, false);
    ibstart::domain::LaunchOptions priority_options;
    priority_options.version = L"8.3.27";
    const auto selected = ibstart::launcher::SelectPlatform(priority_discovered, priority_options);
    CHECK(selected && selected->executable == second_root / L"1cv8.exe");
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

void TestStandaloneThinClientDiscovery() {
  constexpr std::wstring_view kVersion = L"8.3.27.1688";
  const auto root = Temp(L"standalone-thin-client");
  const auto bin = root / kVersion / L"bin";
  std::filesystem::create_directories(bin);
  const auto thin = bin / L"1cv8c.exe";
  if (!CopySystemExecutable(thin)) {
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return;
  }

  const auto discovered = ibstart::platform::Discover({root}, false);
  const auto found = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == thin; });
  CHECK(found != discovered.end());
  CHECK(found != discovered.end() && found->version == kVersion);
  CHECK(found != discovered.end() && found->has_thin_client);

  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::enterprise;
  options.client_type = ibstart::domain::ClientType::thin;
  options.version = std::wstring(kVersion);
  const auto selected = ibstart::launcher::SelectPlatform(discovered, options);
  CHECK(selected && selected->executable == thin);

  ibstart::domain::Database web;
  web.connect = L"WS=\"https://example.test/base\"";
  const auto command = selected ? ibstart::launcher::BuildCommand(web, *selected, options) : ibstart::domain::LaunchCommand{};
  CHECK(command.executable == thin);
  CHECK(command.arguments == std::vector<std::wstring>{L"ENTERPRISE", L"/WS", L"https://example.test/base"});

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

void TestThinClientValidation() {
  const auto root = Temp(L"thin-client-validation");
  const auto bin = root / L"8.3.27.1688" / L"bin";
  std::filesystem::create_directories(bin);
  const auto thick = bin / L"1cv8.exe";
  const auto thin = bin / L"1cv8c.exe";
  if (!CopySystemExecutable(thick)) {
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    return;
  }

  std::error_code error;
  if (!std::filesystem::create_directory(thin, error) || error) {
    std::filesystem::remove_all(root, error);
    return;
  }
  CHECK(!ibstart::platform::FindThinClient(thick));
  CHECK(!ibstart::platform::FindThinClient(thin));

  const std::vector<ibstart::domain::PlatformInstallation> candidates = {
      {thick, L"8.3.27.1688", ibstart::domain::ClientBitness::x64, true}};
  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::enterprise;
  options.client_type = ibstart::domain::ClientType::thin;
  options.version = L"8.3.27.1688";
  CHECK(!ibstart::launcher::SelectPlatform(candidates, options));

  ibstart::domain::Database web;
  web.connect = L"WS=\"https://example.test/base\"";
  bool rejectedDirectory = false;
  try { static_cast<void>(ibstart::launcher::BuildCommand(web, candidates.front(), options)); }
  catch (const std::runtime_error&) { rejectedDirectory = true; }
  CHECK(rejectedDirectory);

  std::filesystem::remove_all(thin, error);
  WriteBytes(thin, "not a PE executable");
  CHECK(!ibstart::platform::FindThinClient(thick));
  CHECK(!ibstart::platform::FindThinClient(thin));
  CHECK(!ibstart::launcher::SelectPlatform(candidates, options));
  bool rejectedCorruptedFile = false;
  try { static_cast<void>(ibstart::launcher::BuildCommand(web, candidates.front(), options)); }
  catch (const std::runtime_error&) { rejectedCorruptedFile = true; }
  CHECK(rejectedCorruptedFile);

  std::filesystem::remove_all(root, error);
}

void TestCustomX86PlatformDiscovery() {
  wchar_t wow64Directory[MAX_PATH]{};
  const UINT length = GetSystemWow64DirectoryW(wow64Directory, static_cast<UINT>(std::size(wow64Directory)));
  if (length == 0 || length >= std::size(wow64Directory)) return;
  const auto source = std::filesystem::path(wow64Directory) / L"cmd.exe";
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error) return;

  const auto root = Temp(L"custom-x86-platform");
  const auto bin = root / L"8.3.27.2000" / L"bin";
  std::filesystem::create_directories(bin);
  const auto executable = bin / L"1cv8.exe";
  std::filesystem::copy_file(source, executable, std::filesystem::copy_options::overwrite_existing);

  const auto discovered = ibstart::platform::Discover({root}, false);
  const auto found = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == executable; });
  CHECK(found != discovered.end());
  CHECK(found != discovered.end() && found->bitness == ibstart::domain::ClientBitness::x86);

  std::filesystem::remove_all(root, error);
}

void TestWindowsArgumentQuoting() {
  const std::vector<std::wstring> expected = {
      L"", L"plain", L"with spaces", L"with\ttab", L"embedded\"quote", L"slash-before-quote\\\"",
      L"two-slashes-before-quote\\\\\"", L"trailing slash with space\\", L"two trailing slashes with space\\\\",
      L"\"surrounded by quotes\"", L"русский 😀", L"line\nbreak"};
  std::wstring command = L"IBStart.exe";
  for (const auto& argument : expected) {
    command.push_back(L' ');
    command += ibstart::launcher::QuoteWindowsArgument(argument);
  }
  int count{};
  LPWSTR* values = CommandLineToArgvW(command.c_str(), &count);
  CHECK(values != nullptr);
  if (!values) return;
  CHECK(count == static_cast<int>(expected.size() + 1));
  for (size_t index = 0; index < expected.size() && static_cast<int>(index + 1) < count; ++index) {
    CHECK(values[index + 1] == expected[index]);
  }
  LocalFree(values);
}

void TestCatalogOrderingAndCycles() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8("[Ten]\nOrderInList=10\n[Two]\nOrderInList=2\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const auto tree = catalog.Tree(); CHECK(tree.size() == 2); CHECK(tree.size() > 1 && tree[0].name == L"Two");
  CHECK(!catalog.AddGroup(L" \t"));
  CHECK(!catalog.AddGroup(L"Slash/Group"));
  CHECK(!catalog.AddGroup(L"Line\nGroup"));
  CHECK(!catalog.AddGroup(std::wstring(L"Nul") + L'\0' + L"Group"));
  CHECK(!catalog.AddServerDatabase(L"\r\n", L"Srvr=\"server\";Ref=\"base\""));
  CHECK(!catalog.AddGroup(L"Orphan", L"Missing"));
  CHECK(catalog.AddGroup(L"Parent")); CHECK(catalog.AddGroup(L"Child", L"Parent")); CHECK(!catalog.Move(L"Parent", L"Child", 0));
  CHECK(!catalog.RenameGroup(L"Parent", L"   "));
  CHECK(!catalog.RenameGroup(L"Parent", L"Renamed/Group"));
  CHECK(catalog.AddServerDatabase(L"Database", L"Srvr=\"server\";Ref=\"base\"", L"Parent"));
  CHECK(!catalog.RenameDatabase(L"Database", L"Renamed\nDatabase"));
  const auto url = ibstart::catalog::Catalog::WebUrl(L"WS=\"https://example.test/base\";WA=1");
  CHECK(url && *url == L"https://example.test/base"); CHECK(!ibstart::catalog::Catalog::IsWebConnection(L"WS=not-a-url"));
  CHECK(ibstart::catalog::IsBareWebConnection(L" https://example.test/base "));
  const auto legacyUrl = ibstart::catalog::Catalog::WebUrl(L"https://example.test/base;Custom=keep");
  CHECK(legacyUrl && *legacyUrl == L"https://example.test/base");
  CHECK(!ibstart::catalog::IsBareWebConnection(L"https://example.test/base;Custom=keep"));
  CHECK(!ibstart::catalog::IsBareWebConnection(L"WS=\"https://example.test/base\""));
}

void TestCatalogValidationAndStableSectionIndices() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Duplicate]\nConnect=File=\"C:\\\\first\"\nID=shared\nCustom=one\nCustom=two\n"
      "[duplicate]\nConnect=File=\"C:\\\\second\"\nID=shared\n"
      "[Orphan]\nConnect=File=\"C:\\\\orphan\"\nFolder=/Missing\n"
      "[Parent]\nFolder=/\n"
      "[Child]\nConnect=File=\"C:\\\\child\"\nFolder=/Parent\n");
  ibstart::catalog::Catalog catalog(std::move(document));

  CHECK(!catalog.IsValid());
  CHECK(catalog.Find(L"Duplicate") == nullptr);
  CHECK(catalog.FindBySectionIndex(0) != nullptr);
  CHECK(catalog.FindBySectionIndex(1) != nullptr);
  CHECK(std::any_of(catalog.diagnostics().begin(), catalog.diagnostics().end(), [](const auto& diagnostic) {
    return diagnostic.message.find(L"Повторяющееся имя секции") != std::wstring::npos;
  }));
  CHECK(std::any_of(catalog.diagnostics().begin(), catalog.diagnostics().end(), [](const auto& diagnostic) {
    return diagnostic.message.find(L"Повторяющийся ключ") != std::wstring::npos;
  }));
  CHECK(std::any_of(catalog.diagnostics().begin(), catalog.diagnostics().end(), [](const auto& diagnostic) {
    return diagnostic.message.find(L"Повторяющийся идентификатор") != std::wstring::npos;
  }));
  CHECK(std::any_of(catalog.diagnostics().begin(), catalog.diagnostics().end(), [](const auto& diagnostic) {
    return diagnostic.message.find(L"Не найдена родительская группа") != std::wstring::npos;
  }));

  const auto tree = catalog.Tree();
  const auto child = std::find_if(tree.begin(), tree.end(), [](const auto& item) { return item.name == L"Parent"; });
  CHECK(child != tree.end());
  CHECK(child != tree.end() && child->children.size() == 1 && child->children.front().name == L"Child");
  CHECK(child != tree.end() && child->children.front().section_index == 4);
  CHECK(catalog.Remove(1));
  CHECK(catalog.FindBySectionIndex(1) != nullptr);
  CHECK(catalog.FindBySectionIndex(3) != nullptr);
}

void TestEmptyConnectDiagnosis() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Empty connection]\nConnect=\nID=empty-connection\nCustom=keep\n");
  ibstart::catalog::Catalog catalog(std::move(document));

  const auto* entry = catalog.Find(L"Empty connection");
  CHECK(entry != nullptr && entry->IsDatabase());
  CHECK(catalog.IsValid());
  const auto& diagnostics = catalog.diagnostics();
  const auto empty_connect = std::find_if(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
    return diagnostic.section_index == 0 && !diagnostic.blocking &&
        diagnostic.message.find(L"Пустая строка подключения Connect") != std::wstring::npos;
  });
  CHECK(empty_connect != diagnostics.end());

  const auto database = catalog.DatabaseFor(L"Empty connection");
  CHECK(database.connect.empty());
  ibstart::domain::LaunchOptions options;
  const auto launch_diagnostics = ibstart::launcher::ValidateLaunchParameters(database, options);
  CHECK(!launch_diagnostics.empty());
  CHECK(!launch_diagnostics.empty() && launch_diagnostics.front().find(L"Connect") != std::wstring::npos);

  auto* editable = catalog.Find(L"Empty connection");
  CHECK(editable != nullptr);
  if (editable) editable->Set(L"Connect", L"File=\"C:\\\\fixed\"");
  CHECK(catalog.diagnostics().empty());
  CHECK(catalog.DatabaseFor(L"Empty connection").connect == L"File=\"C:\\\\fixed\"");
}

void TestCatalogSetChildOrder() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Zulu database]\nConnect=File=\"C:\\\\zulu\"\nOrderInList=1\nOrderInTree=1\n"
      "[Alpha folder]\nFolder=/\nOrderInList=2\nOrderInTree=2\n"
      "[Bravo database]\nConnect=File=\"C:\\\\bravo\"\nOrderInList=3\nOrderInTree=3\n"
      "[Nested zulu]\nConnect=File=\"C:\\\\nested-zulu\"\nFolder=/Alpha folder\nOrderInList=1\nOrderInTree=1\n"
      "[Nested alpha]\nConnect=File=\"C:\\\\nested-alpha\"\nFolder=/Alpha folder\nOrderInList=2\nOrderInTree=2\n");
  ibstart::catalog::Catalog catalog(std::move(document));

  CHECK(catalog.SetChildOrder(L"", {L"Alpha folder", L"Bravo database", L"Zulu database"}));
  CHECK(catalog.SetChildOrder(L"Alpha folder", {L"Nested alpha", L"Nested zulu"}));
  const auto tree = catalog.Tree();
  CHECK(tree.size() == 3);
  CHECK(tree.size() > 2 && tree[0].name == L"Alpha folder" && tree[1].name == L"Bravo database" && tree[2].name == L"Zulu database");
  CHECK(!tree.empty() && tree[0].children.size() == 2 && tree[0].children[0].name == L"Nested alpha" && tree[0].children[1].name == L"Nested zulu");
  const auto* nestedAlpha = catalog.Find(L"Nested alpha");
  const auto* nestedZulu = catalog.Find(L"Nested zulu");
  CHECK(nestedAlpha && nestedAlpha->ValueOr(L"OrderInList") == L"1" && nestedAlpha->ValueOr(L"OrderInTree") == L"1");
  CHECK(nestedZulu && nestedZulu->ValueOr(L"OrderInList") == L"2" && nestedZulu->ValueOr(L"OrderInTree") == L"2");

  // Manual catalog order can deliberately place a database before a folder.
  CHECK(catalog.SetChildOrder(L"", {L"Zulu database", L"Alpha folder", L"Bravo database"}));
  const auto manuallyReordered = catalog.Tree();
  CHECK(manuallyReordered.size() > 2 && manuallyReordered[0].name == L"Zulu database" &&
      manuallyReordered[1].name == L"Alpha folder" && manuallyReordered[2].name == L"Bravo database");

  CHECK(!catalog.SetChildOrder(L"Alpha folder", {L"Nested alpha"}));
  CHECK(!catalog.SetChildOrder(L"Alpha folder", {L"Nested alpha", L"Nested alpha"}));
  CHECK(!catalog.SetChildOrder(L"Alpha folder", {L"Nested alpha", L"Zulu database"}));
}

void TestCatalogSortChildrenByName() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Zulu database]\nConnect=File=\"C:\\\\zulu\"\nOrderInList=1\n"
      "[Folder bravo]\nFolder=/\nOrderInList=2\n"
      "[Alpha database]\nConnect=File=\"C:\\\\alpha\"\nOrderInList=3\n"
      "[Folder alpha]\nFolder=/\nOrderInList=4\n"
      "[Nested zulu]\nConnect=File=\"C:\\\\nested-zulu\"\nFolder=/Folder bravo\nOrderInList=1\n"
      "[Nested alpha]\nConnect=File=\"C:\\\\nested-alpha\"\nFolder=/Folder bravo\nOrderInList=2\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const auto names = [](const std::vector<ibstart::catalog::TreeItem>& items) {
    std::vector<std::wstring> result;
    for (const auto& item : items) result.push_back(item.name);
    return result;
  };

  CHECK(catalog.SortChildrenByName(L"", ibstart::catalog::SortDirection::ascending, false));
  const std::vector<std::wstring> ascendingRoot{L"Alpha database", L"Folder alpha", L"Folder bravo", L"Zulu database"};
  CHECK(names(catalog.Tree()) == ascendingRoot);
  CHECK(catalog.SortChildrenByName(L"", ibstart::catalog::SortDirection::descending, true));
  const std::vector<std::wstring> descendingRoot{L"Folder bravo", L"Folder alpha", L"Zulu database", L"Alpha database"};
  CHECK(names(catalog.Tree()) == descendingRoot);
  CHECK(catalog.SortChildrenByName(L"Folder bravo", ibstart::catalog::SortDirection::ascending, false));
  const auto tree = catalog.Tree();
  const auto folder = std::find_if(tree.begin(), tree.end(), [](const auto& item) { return item.name == L"Folder bravo"; });
  const std::vector<std::wstring> ascendingNested{L"Nested alpha", L"Nested zulu"};
  CHECK(folder != tree.end() && names(folder->children) == ascendingNested);
}

void TestInstanceActivationPayload() {
  CHECK(ibstart::app::ShouldLoadCatalogOnStartup(true, false));
  CHECK(ibstart::app::ShouldLoadCatalogOnStartup(false, true));
  CHECK(!ibstart::app::ShouldLoadCatalogOnStartup(false, false));
  const wchar_t valid[] = L"database-id";
  COPYDATASTRUCT data{};
  data.dwData = ibstart::app::kLaunchCopyData;
  data.cbData = sizeof(valid);
  data.lpData = const_cast<wchar_t*>(valid);
  CHECK(ibstart::app::IsValidLaunchIdLength(sizeof(valid) / sizeof(*valid) - 1));
  CHECK(ibstart::app::IsValidLaunchCopyData(&data));

  data.dwData = 0;
  CHECK(!ibstart::app::IsValidLaunchCopyData(&data));
  data.dwData = ibstart::app::kLaunchCopyData;
  data.cbData = sizeof(wchar_t) - 1;
  CHECK(!ibstart::app::IsValidLaunchCopyData(&data));
  data.cbData = static_cast<DWORD>((ibstart::app::kMaximumLaunchIdLength + 2) * sizeof(wchar_t));
  CHECK(!ibstart::app::IsValidLaunchCopyData(&data));
  const wchar_t unterminated[] = {L'x', L'y'};
  data.cbData = sizeof(unterminated);
  data.lpData = const_cast<wchar_t*>(unterminated);
  CHECK(!ibstart::app::IsValidLaunchCopyData(&data));
  CHECK(!ibstart::app::IsValidLaunchIdLength(ibstart::app::kMaximumLaunchIdLength + 1));
}

void TestUpdateVersionsAndVersionFile() {
  CHECK(ibstart::update::CompareVersions(L"0.5.1", L"0.5.1") == 0);
  CHECK(ibstart::update::CompareVersions(L"0.5.1", L"0.6.0") < 0);
  CHECK(ibstart::update::CompareVersions(L"v1.0.0", L"0.9.9") > 0);
  CHECK(ibstart::update::CompareVersions(L"1.0.0-alpha", L"1.0.0-alpha.1") < 0);
  CHECK(ibstart::update::CompareVersions(L"1.0.0-alpha.2", L"1.0.0-alpha.10") < 0);
  CHECK(ibstart::update::CompareVersions(L"1.0.0-beta", L"1.0.0-alpha.99") > 0);
  CHECK(ibstart::update::CompareVersions(L"1.0.0-rc.1", L"1.0.0") < 0);
  bool invalidVersion = false;
  try { static_cast<void>(ibstart::update::CompareVersions(L"1.0", L"1.0.0")); }
  catch (const std::invalid_argument&) { invalidVersion = true; }
  CHECK(invalidVersion);

  const auto expectedPageUrl = std::wstring(ibstart::version::github_release_page_prefix) + L"tag/v0.6.0";
  const auto release = ibstart::update::ParseLatestVersionFile(" \r\n v0.6.0\t");
  CHECK(release.version == L"0.6.0");
  CHECK(release.page_url == expectedPageUrl);
  bool emptyVersionFile = false;
  try {
    static_cast<void>(ibstart::update::ParseLatestVersionFile("\r\n\t "));
  } catch (const std::invalid_argument&) { emptyVersionFile = true; }
  CHECK(emptyVersionFile);
  bool invalidVersionFile = false;
  try {
    static_cast<void>(ibstart::update::ParseLatestVersionFile("0.6"));
  } catch (const std::invalid_argument&) { invalidVersionFile = true; }
  CHECK(invalidVersionFile);

  std::stop_source cancelledUpdate;
  CHECK(cancelledUpdate.request_stop());
  CHECK(!ibstart::update::FetchLatestRelease(cancelledUpdate.get_token()));
}

void TestUpdateCancellationOnHangingConnection() {
  LoopbackHttpServer server;
  std::stop_source stop;
  std::optional<std::string> response;
  std::exception_ptr client_exception;
  const ibstart::update::transport::VersionAssetEndpoint endpoint{
      L"127.0.0.1", server.port(), L"/hang", false, false};
  std::thread client([&] {
    try {
      response = ibstart::update::transport::FetchVersionAsset(endpoint, stop.get_token());
    } catch (...) {
      client_exception = std::current_exception();
    }
  });

  CHECK(server.WaitForClient(std::chrono::seconds(2)));
  const auto cancellation_started = std::chrono::steady_clock::now();
  CHECK(stop.request_stop());
  client.join();
  const auto cancellation_time = std::chrono::steady_clock::now() - cancellation_started;

  CHECK(!client_exception);
  CHECK(!response.has_value());
  CHECK(cancellation_time < std::chrono::seconds(2));
}

void TestUpdateTransportReadsAsyncResponse() {
  LoopbackHttpServer server(
      "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nv0.7.4");
  const ibstart::update::transport::VersionAssetEndpoint endpoint{
      L"127.0.0.1", server.port(), L"/version", false, false};
  std::optional<std::string> response;
  std::exception_ptr client_exception;
  std::thread client([&] {
    try {
      response = ibstart::update::transport::FetchVersionAsset(endpoint);
    } catch (...) {
      client_exception = std::current_exception();
    }
  });

  CHECK(server.WaitForClient(std::chrono::seconds(2)));
  client.join();

  if (client_exception) {
    try { std::rethrow_exception(client_exception); }
    catch (const std::exception& error) { std::cerr << "Loopback response error: " << error.what() << "\n"; }
  }
  CHECK(!client_exception);
  CHECK(response == std::optional<std::string>("v0.7.4"));
}

void TestCatalogSearch() {
  const auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Бухгалтерия]\nConnect=Srvr=cluster-01;Ref=Accounting\nFolder=/Рабочие\n"
      "OrderInList=1\nOrderInTree=2\nCustom=клиент-А\n");
  const auto* entry = document.Find(L"Бухгалтерия");
  CHECK(entry != nullptr);
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"бух"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"CLUSTER-01"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"accounting"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"Рабочие"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"Folder"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"OrderInList"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"OrderInTree"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"клиент-а"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"custom"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"не найдено"));
}

void TestTreeFilters() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Рабочие базы]\nFolder=/\n"
      "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nFolder=/Рабочие базы\n"
      "[Beta]\nConnect=File=\"C:\\\\beta\"\nFolder=/Рабочие базы\n"
      "[Other]\nConnect=File=\"C:\\\\other\"\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const ibstart::storage::DatabaseTags tags = {
      {L"Alpha", {L"Production", L"Shared"}},
      {L"Beta", {L"test", L"shared"}},
  };
  const auto available = ibstart::ui::presentation::CollectFilterTags(catalog, tags);
  CHECK(available == std::vector<std::wstring>{L"Production", L"Shared", L"test"});

  const auto tree = catalog.Tree();
  const auto folder = std::find_if(tree.begin(), tree.end(), [](const auto& item) { return item.name == L"Рабочие базы"; });
  CHECK(folder != tree.end());
  if (folder == tree.end()) return;

  CHECK(ibstart::ui::presentation::MatchesSearchFilter(catalog, *folder, L"prod", tags));
  CHECK(!ibstart::ui::presentation::MatchesSearchFilter(catalog, *folder, L"release", tags));
  CHECK(!ibstart::ui::presentation::MatchesSearchFilter(catalog, *folder, L"баз", tags));
  CHECK(ibstart::ui::presentation::FilterTreeItems(
      catalog, tree, L"баз", ibstart::ui::presentation::TreeTagFilter{}, tags, {}).empty());

  ibstart::ui::presentation::TreeTagFilter favorites;
  favorites.kind = ibstart::ui::presentation::TreeTagFilterKind::favorites;
  CHECK(ibstart::ui::presentation::MatchesTagFilter(catalog, *folder, favorites, tags, {L"Alpha"}));
  CHECK(!ibstart::ui::presentation::MatchesTagFilter(catalog, *folder, favorites, tags, {L"Other"}));

  ibstart::ui::presentation::TreeTagFilter tag;
  tag.kind = ibstart::ui::presentation::TreeTagFilterKind::tag;
  tag.tag = L"TEST";
  CHECK(ibstart::ui::presentation::MatchesTagFilter(catalog, *folder, tag, tags, {}));
  tag.tag = L"Unknown";
  CHECK(!ibstart::ui::presentation::MatchesTagFilter(catalog, *folder, tag, tags, {}));
}

HTREEITEM FindTreeItemByName(const ibstart::ui::TreeViewController& controller, HWND tree,
    HTREEITEM first, std::wstring_view name) {
  for (auto current = first; current; current = TreeView_GetNextSibling(tree, current)) {
    if (std::wstring_view(controller.ItemName(current)) == name) return current;
    if (const auto found = FindTreeItemByName(controller, tree, TreeView_GetChild(tree, current), name)) return found;
  }
  return nullptr;
}

bool IsTreeItemExpanded(HWND tree, HTREEITEM item) {
  return item && (TreeView_GetItemState(tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
}

HTREEITEM FindTopLevelTreeItemByData(const ibstart::ui::TreeViewController& controller, HWND tree, LPARAM data) {
  for (auto item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item)) {
    if (controller.ItemData(item) == data) return item;
  }
  return nullptr;
}

void TestSearchExpandsAndRestoresGroups() {
  INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_TREEVIEW_CLASSES};
  CHECK(InitCommonControlsEx(&common_controls) != FALSE);
  const HWND tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
      0, 0, 640, 480, GetDesktopWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(tree != nullptr);
  if (!tree) return;

  {
    ibstart::ui::TreeViewController controller;
    controller.Attach(tree, GetModuleHandleW(nullptr));
    auto document = ibstart::v8i::V8iDocument::ParseUtf8(
        "[Архив]\nFolder=/\n"
        "[Рабочие базы]\nFolder=/\n"
        "[Продажи]\nFolder=/Рабочие базы\n"
        "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nFolder=/Рабочие базы/Продажи\n"
        "[Beta]\nConnect=File=\"C:\\\\beta\"\nFolder=/Архив\n");
    ibstart::catalog::Catalog catalog(std::move(document));
    const ibstart::storage::CatalogState state;
    const ibstart::ui::presentation::TreeTagFilter tag_filter;

    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    const HTREEITEM catalog_root = TreeView_GetRoot(tree);
    const HTREEITEM work_group = FindTreeItemByName(controller, tree, catalog_root, L"Рабочие базы");
    const HTREEITEM nested_group = FindTreeItemByName(controller, tree, catalog_root, L"Продажи");
    const HTREEITEM archive_group = FindTreeItemByName(controller, tree, catalog_root, L"Архив");
    CHECK(catalog_root != nullptr);
    CHECK(work_group != nullptr && !IsTreeItemExpanded(tree, work_group));
    CHECK(nested_group != nullptr && !IsTreeItemExpanded(tree, nested_group));
    CHECK(archive_group != nullptr && !IsTreeItemExpanded(tree, archive_group));
    if (archive_group) {
      TreeView_Expand(tree, archive_group, TVE_EXPAND);
      CHECK(IsTreeItemExpanded(tree, archive_group));
    }

    controller.Populate(catalog, state, {}, L"alpha", tag_filter, false);
    const HTREEITEM filtered_root = TreeView_GetRoot(tree);
    const HTREEITEM filtered_work_group = FindTreeItemByName(controller, tree, filtered_root, L"Рабочие базы");
    const HTREEITEM filtered_nested_group = FindTreeItemByName(controller, tree, filtered_root, L"Продажи");
    CHECK(filtered_work_group != nullptr && IsTreeItemExpanded(tree, filtered_work_group));
    CHECK(filtered_nested_group != nullptr && IsTreeItemExpanded(tree, filtered_nested_group));
    CHECK(FindTreeItemByName(controller, tree, filtered_root, L"Архив") == nullptr);

    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    const HTREEITEM restored_root = TreeView_GetRoot(tree);
    const HTREEITEM restored_work_group = FindTreeItemByName(controller, tree, restored_root, L"Рабочие базы");
    const HTREEITEM restored_nested_group = FindTreeItemByName(controller, tree, restored_root, L"Продажи");
    const HTREEITEM restored_archive_group = FindTreeItemByName(controller, tree, restored_root, L"Архив");
    CHECK(restored_work_group != nullptr && !IsTreeItemExpanded(tree, restored_work_group));
    CHECK(restored_nested_group != nullptr && !IsTreeItemExpanded(tree, restored_nested_group));
    CHECK(restored_archive_group != nullptr && IsTreeItemExpanded(tree, restored_archive_group));
  }
  DestroyWindow(tree);
}

void TestCatalogRefreshRejectsStaleSelection() {
  INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_TREEVIEW_CLASSES};
  CHECK(InitCommonControlsEx(&common_controls) != FALSE);
  const HWND tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
      0, 0, 640, 480, GetDesktopWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(tree != nullptr);
  if (!tree) return;

  {
    ibstart::ui::TreeViewController controller;
    controller.Attach(tree, GetModuleHandleW(nullptr));
    const auto original = ibstart::catalog::Catalog(ibstart::v8i::V8iDocument::ParseUtf8(
        "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nID=alpha-id\n"));
    const auto replacement = ibstart::catalog::Catalog(ibstart::v8i::V8iDocument::ParseUtf8(
        "[Replacement]\nConnect=File=\"C:\\\\replacement\"\nID=replacement-id\n"));
    const ibstart::storage::CatalogState state;
    const ibstart::ui::presentation::TreeTagFilter tag_filter;

    controller.Populate(original, state, {}, L"", tag_filter, true);
    CHECK(controller.SelectItem(static_cast<size_t>(0)));
    CHECK(controller.SelectedSectionIndex(original) == static_cast<size_t>(0));

    // A command from an already-open menu can run after the catalog has been
    // replaced, while the native tree still carries the old section index.
    CHECK(!controller.SelectedSectionIndex(replacement).has_value());

    controller.Populate(replacement, state, {}, L"", tag_filter, true);
    CHECK(!controller.SelectedSectionIndex().has_value());
    CHECK(controller.SelectedName().empty());
  }
  DestroyWindow(tree);
}

void TestVirtualBranchSelectionSurvivesRefresh() {
  INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_TREEVIEW_CLASSES};
  CHECK(InitCommonControlsEx(&common_controls) != FALSE);
  const HWND tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
      0, 0, 640, 480, GetDesktopWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(tree != nullptr);
  if (!tree) return;

  {
    ibstart::ui::TreeViewController controller;
    controller.Attach(tree, GetModuleHandleW(nullptr));
    auto document = ibstart::v8i::V8iDocument::ParseUtf8(
        "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nID=alpha-id\n"
        "[Beta]\nConnect=File=\"C:\\\\beta\"\nID=beta-id\n");
    ibstart::catalog::Catalog catalog(std::move(document));
    ibstart::storage::CatalogState state;
    state.favorites = {L"alpha-id", L"beta-id"};
    state.history = {
        {L"alpha-id", std::chrono::system_clock::from_time_t(200), ibstart::domain::LaunchMode::enterprise},
        {L"beta-id", std::chrono::system_clock::from_time_t(100), ibstart::domain::LaunchMode::designer},
    };
    const ibstart::ui::presentation::TreeTagFilter tag_filter;

    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    CHECK(controller.SelectItemInBranch(L"Alpha", ibstart::ui::TreeViewController::kFavoritesRootItemData));
    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    HTREEITEM selected = TreeView_GetSelection(tree);
    CHECK(selected != nullptr && controller.ItemName(selected) == L"Alpha");
    CHECK(controller.BranchData(selected) == ibstart::ui::TreeViewController::kFavoritesRootItemData);

    state.favorites = {L"beta-id"};
    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    selected = TreeView_GetSelection(tree);
    CHECK(selected != nullptr && controller.ItemName(selected) == L"Alpha");
    CHECK(controller.BranchData(selected) == ibstart::ui::TreeViewController::kCatalogRootItemData);

    const HTREEITEM recent_root = FindTopLevelTreeItemByData(controller, tree,
        ibstart::ui::TreeViewController::kRecentRootItemData);
    CHECK(recent_root != nullptr);
    CHECK(controller.SelectItemInBranch(L"Alpha", ibstart::ui::TreeViewController::kRecentRootItemData));
    std::swap(state.history[0], state.history[1]);
    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    selected = TreeView_GetSelection(tree);
    CHECK(selected != nullptr && controller.ItemName(selected) == L"Alpha");
    CHECK(controller.BranchData(selected) == ibstart::ui::TreeViewController::kRecentRootItemData);

    state.history.erase(state.history.begin() + 1);
    controller.Populate(catalog, state, {}, L"", tag_filter, false);
    selected = TreeView_GetSelection(tree);
    CHECK(selected != nullptr && controller.ItemName(selected) == L"Alpha");
    CHECK(controller.BranchData(selected) == ibstart::ui::TreeViewController::kCatalogRootItemData);
  }
  DestroyWindow(tree);
}

void TestLongCatalogNamesUseFullIdentity() {
  const std::wstring name = std::wstring(600, L'N') + L" — база";
  const std::wstring contents = L"[" + name + L"]\nConnect=File=\"C:\\\\long\"\nID=long-id\n";
  ibstart::catalog::Catalog catalog(ibstart::v8i::V8iDocument::ParseUtf8(ibstart::utf::ToUtf8(contents)));

  const auto* entry = catalog.FindBySectionIndex(0);
  CHECK(entry != nullptr && entry->name == name);
  const auto database = catalog.DatabaseFor(static_cast<size_t>(0));
  CHECK(database.name == name);
  CHECK(database.id == L"long-id");

  INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_TREEVIEW_CLASSES};
  CHECK(InitCommonControlsEx(&common_controls) != FALSE);
  const HWND tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT,
      0, 0, 640, 480, GetDesktopWindow(), nullptr, GetModuleHandleW(nullptr), nullptr);
  CHECK(tree != nullptr);
  if (!tree) return;

  {
    ibstart::ui::TreeViewController controller;
    controller.Attach(tree, GetModuleHandleW(nullptr));
    const ibstart::storage::CatalogState state;
    const ibstart::ui::presentation::TreeTagFilter tag_filter;
    controller.Populate(catalog, state, {}, L"", tag_filter, true);

    const HTREEITEM root = TreeView_GetRoot(tree);
    const HTREEITEM item = root ? TreeView_GetChild(tree, root) : nullptr;
    CHECK(item != nullptr);
    CHECK(item && controller.ItemName(item) == name);
    CHECK(item && controller.SectionIndex(item) == static_cast<size_t>(0));
    CHECK(controller.SelectItem(static_cast<size_t>(0)));
    CHECK(controller.SelectedSectionIndex() == static_cast<size_t>(0));
    CHECK(controller.SelectedName() == name);
  }
  DestroyWindow(tree);
}

void TestRecentDatabaseNames() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nID=alpha-id\n"
      "[Beta]\nConnect=File=\"C:\\\\beta\"\nID=beta-id\n"
      "[Fallback]\nConnect=File=\"C:\\\\fallback\"\n"
      "[Folder]\nFolder=/\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const std::vector<ibstart::domain::HistoryItem> history = {
      {L"beta-id", {}, ibstart::domain::LaunchMode::enterprise},
      {L"missing-id", {}, ibstart::domain::LaunchMode::designer},
      {L"alpha-id", {}, ibstart::domain::LaunchMode::designer},
      {L"Fallback", {}, ibstart::domain::LaunchMode::enterprise},
  };
  CHECK(ibstart::ui::presentation::CollectRecentDatabaseNames(catalog, history) ==
      std::vector<std::wstring>{L"Beta", L"Alpha", L"Fallback"});
}

void TestRelativeLaunchTimeFormatting() {
  using Clock = std::chrono::system_clock;
  const auto now = Clock::from_time_t(1'700'000'000);
  using namespace std::chrono;

  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now, now) == L"только что");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - seconds(59), now) == L"только что");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - minutes(1), now) == L"1 минуту назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - minutes(2), now) == L"2 минуты назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - minutes(5), now) == L"5 минут назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(1), now) == L"1 час назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(2), now) == L"2 часа назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(5), now) == L"5 часов назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(24), now) == L"1 день назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(48), now) == L"2 дня назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now - hours(24 * 5), now) == L"5 дней назад");
  CHECK(ibstart::ui::presentation::FormatRelativeLaunchTime(now + seconds(1), now) == L"только что");
}

void TestStableDatabaseId() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[EmptyId]\nConnect=File=\"C:\\\\empty-id\"\nID=\n"
      "[NoId]\nConnect=File=\"C:\\\\no-id\"\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const auto* entry = catalog.Find(L"EmptyId");
  CHECK(entry != nullptr);
  if (!entry) return;

  CHECK(ibstart::catalog::StableDatabaseId(*entry) == L"EmptyId");
  CHECK(catalog.DatabaseFor(L"EmptyId").id == L"EmptyId");
  CHECK(catalog.FindById(L"EmptyId") == entry);
  CHECK(ibstart::ui::presentation::TagId(*entry) == L"EmptyId");
  const ibstart::storage::DatabaseTags tags = {{L"EmptyId", {L"Fallback"}}};
  CHECK(ibstart::ui::presentation::TagsFor(tags, *entry).size() == 1);
  const std::vector<ibstart::domain::HistoryItem> history = {
      {L"EmptyId", {}, ibstart::domain::LaunchMode::enterprise}};
  CHECK(ibstart::ui::presentation::CollectRecentDatabaseNames(catalog, history) ==
      std::vector<std::wstring>{L"EmptyId"});
  CHECK(catalog.IsValid());
  CHECK(std::any_of(catalog.diagnostics().begin(), catalog.diagnostics().end(), [](const auto& diagnostic) {
    return !diagnostic.blocking && diagnostic.message.find(L"Пустой ID базы") != std::wstring::npos;
  }));
}

void TestCaseInsensitiveDatabaseIdentifiers() {
  const auto directory = Temp(L"case-insensitive-identifiers");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  WriteBytes(layout.root / L"catalog-state.json", R"({
    "favorites": [
      {"favorite": "{ABC}"},
      {"favorite": "{abc}"},
      {"favorite": "other"}
    ],
    "history": [
      {"history_id": "{ABC}", "time": 100, "mode": 0},
      {"history_id": "{abc}", "time": 200, "mode": 1},
      {"history_id": "other", "time": 150, "mode": 2}
    ],
    "last_launches": [
      {"last_launch_id": "{ABC}", "time": 100},
      {"last_launch_id": "{abc}", "time": 250}
    ],
    "tags": [
      {"tag_id": "{ABC}", "values": ["One", "Shared"]},
      {"tag_id": "{abc}", "values": ["two", "one"]}
    ]
  })");

  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Base]\nConnect=File=\"C:\\\\base\"\nID={abc}\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const auto* entry = catalog.FindById(L"{ABC}");
  CHECK(entry != nullptr);
  if (!entry) return;

  const auto state = ibstart::storage::LoadCatalogState(layout);
  CHECK(state.favorites == std::vector<std::wstring>{L"{ABC}", L"other"});
  CHECK(state.history.size() == 2);
  if (!state.history.empty()) {
    CHECK(state.history[0].database_id == L"{abc}");
    CHECK(state.history[0].timestamp == std::chrono::system_clock::from_time_t(200));
    CHECK(state.history[0].mode == ibstart::domain::LaunchMode::designer);
  }

  const auto launch = state.last_launches.find(L"{AbC}");
  CHECK(launch != state.last_launches.end());
  CHECK(launch == state.last_launches.end() ||
      launch->second == std::chrono::system_clock::from_time_t(250));

  const auto tags = state.tags.find(L"{aBc}");
  CHECK(tags != state.tags.end());
  CHECK(tags == state.tags.end() || tags->second == std::vector<std::wstring>{L"One", L"Shared", L"two"});
  CHECK(ibstart::ui::presentation::TagsFor(state.tags, *entry).size() == 3);

  const auto tree = catalog.Tree();
  CHECK(tree.size() == 1);
  if (!tree.empty()) {
    ibstart::ui::presentation::TreeTagFilter favorites;
    favorites.kind = ibstart::ui::presentation::TreeTagFilterKind::favorites;
    CHECK(ibstart::ui::presentation::MatchesTagFilter(catalog, tree.front(), favorites, state.tags, state.favorites));
  }
  CHECK(ibstart::ui::presentation::CollectRecentDatabaseNames(catalog, state.history) ==
      std::vector<std::wstring>{L"Base"});

  ibstart::catalog::CatalogMetadataService service(layout);
  CHECK(!service.ToggleFavorite(L"{aBc}"));
  CHECK(service.Read().favorites == std::vector<std::wstring>{L"other"});
  CHECK(service.ToggleFavorite(L"{aBc}"));
  CHECK(service.Read().favorites == std::vector<std::wstring>{L"{aBc}", L"other"});
  service.SetTags(L"{aBc}", {L"Replacement"});
  // Case-insensitive identifier aliases represent one tag assignment.
  CHECK(service.Read().tags.size() == 1);
  const auto replacement = service.Read().tags.find(L"{ABC}");
  CHECK(replacement != service.Read().tags.end());
  CHECK(replacement == service.Read().tags.end() || replacement->second == std::vector<std::wstring>{L"Replacement"});
  service.RecordLaunch({L"{ABC}", std::chrono::system_clock::from_time_t(300),
      ibstart::domain::LaunchMode::enterprise});
  CHECK(service.Read().history.size() == 2);
  CHECK(!service.Read().history.empty() && service.Read().history.front().database_id == L"{ABC}");
  CHECK(service.Read().last_launches.at(L"{abc}") == std::chrono::system_clock::from_time_t(300));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestStandardFolderPaths() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Root database]\nConnect=File=\"C:\\\\root\"\nFolder=/\n"
      "[Top]\nFolder=/\n"
      "[Child database]\nConnect=Srvr=\"server\";Ref=\"child\"\nFolder=/Top\n"
      "[Nested]\nFolder=/Top\n"
      "[Deep database]\nConnect=File=\"C:\\\\deep\"\nFolder=/Top/Nested\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  CHECK(catalog.Databases().size() == 3);
  const auto tree = catalog.Tree();
  const auto top = std::find_if(tree.begin(), tree.end(), [](const auto& item) { return item.name == L"Top"; });
  CHECK(tree.size() == 2); CHECK(top != tree.end());
  const auto nested = top == tree.end() ? std::vector<ibstart::catalog::TreeItem>::const_iterator{} :
      std::find_if(top->children.begin(), top->children.end(), [](const auto& item) { return item.name == L"Nested"; });
  CHECK(top != tree.end() && top->children.size() == 2); CHECK(top != tree.end() && nested != top->children.end());
  CHECK(top != tree.end() && nested != top->children.end() && nested->children.size() == 1);

  CHECK(catalog.RenameGroup(L"Top", L"Renamed"));
  const auto* nestedEntry = catalog.Find(L"Nested");
  const auto* deepEntry = catalog.Find(L"Deep database");
  CHECK(nestedEntry && nestedEntry->ValueOr(L"Folder") == L"/Renamed");
  CHECK(deepEntry && deepEntry->ValueOr(L"Folder") == L"/Renamed/Nested");
  CHECK(catalog.Move(L"Nested", L"", 0));
  nestedEntry = catalog.Find(L"Nested"); deepEntry = catalog.Find(L"Deep database");
  CHECK(nestedEntry && nestedEntry->ValueOr(L"Folder") == L"/");
  CHECK(deepEntry && deepEntry->ValueOr(L"Folder") == L"/Nested");
  CHECK(catalog.Remove(L"Nested"));
  deepEntry = catalog.Find(L"Deep database");
  CHECK(deepEntry && deepEntry->ValueOr(L"Folder") == L"/");
  CHECK(catalog.AddGroup(L"Added", L"Renamed"));
  const auto* addedEntry = catalog.Find(L"Added");
  CHECK(addedEntry && addedEntry->ValueOr(L"Folder") == L"/Renamed");
  CHECK(catalog.AddServerDatabase(L"Added database", L"Srvr=\"server\";Ref=\"added\"", L"Added"));
  const auto* addedDatabase = catalog.Find(L"Added database");
  CHECK(addedDatabase && addedDatabase->ValueOr(L"Folder") == L"/Renamed/Added");
  CHECK(addedDatabase && addedDatabase->ValueOr(L"ID").size() == 38);
  CHECK(catalog.ParentOf(L"Added database") == L"Added");
  CHECK(catalog.AddServerDatabase(L"Second database", L"Srvr=\"server\";Ref=\"second\"", L"Added"));
  CHECK(catalog.RenameDatabase(L"Second database", L"Renamed database"));
  CHECK(catalog.Find(L"Second database") == nullptr);
  CHECK(catalog.Find(L"Renamed database") != nullptr);
  CHECK(!catalog.RenameDatabase(L"Renamed database", L"Added database"));
  CHECK(catalog.MoveBy(L"Renamed database", -1));
  const auto* secondDatabase = catalog.Find(L"Renamed database");
  addedDatabase = catalog.Find(L"Added database");
  CHECK(secondDatabase && secondDatabase->ValueOr(L"OrderInList") == L"1");
  CHECK(addedDatabase && addedDatabase->ValueOr(L"OrderInList") == L"2");
  CHECK(!catalog.MoveBy(L"Renamed database", -1));

  auto treeOrderDocument = ibstart::v8i::V8iDocument::ParseUtf8(
      "[First]\nConnect=File=\"C:\\\\first\"\nOrderInList=1\nOrderInTree=20\n"
      "[Second]\nConnect=File=\"C:\\\\second\"\nOrderInList=2\nOrderInTree=10\n");
  ibstart::catalog::Catalog treeOrderCatalog(std::move(treeOrderDocument));
  const auto treeOrder = treeOrderCatalog.Tree();
  CHECK(treeOrder.size() == 2);
  CHECK(treeOrder.size() > 1 && treeOrder.front().name == L"Second");

  auto dragDocument = ibstart::v8i::V8iDocument::ParseUtf8(
      "[First]\nConnect=File=\"C:\\\\first\"\nOrderInList=1\n"
      "[Second]\nConnect=File=\"C:\\\\second\"\nOrderInList=2\n"
      "[Third]\nConnect=File=\"C:\\\\third\"\nOrderInList=3\n"
      "[Grouped]\nConnect=File=\"C:\\\\grouped\"\nFolder=/Folder\n"
      "[Folder]\nFolder=/\n");
  ibstart::catalog::Catalog dragCatalog(std::move(dragDocument));
  CHECK(dragCatalog.Move(L"Third", L"", 1));
  const auto reordered = dragCatalog.Tree();
  CHECK(reordered.size() == 4); CHECK(reordered.size() > 2 && reordered[0].name == L"First" && reordered[1].name == L"Third" && reordered[2].name == L"Second");
  CHECK(dragCatalog.Move(L"Grouped", L"", 0));
  const auto* movedToRoot = dragCatalog.Find(L"Grouped");
  CHECK(movedToRoot && movedToRoot->ValueOr(L"Folder") == L"/");
  CHECK(dragCatalog.Move(L"Second", L"Folder", std::numeric_limits<size_t>::max()));
  const auto* movedIntoFolder = dragCatalog.Find(L"Second");
  CHECK(movedIntoFolder && movedIntoFolder->ValueOr(L"Folder") == L"/Folder");
}

void TestFileBaseScanRegistration() {
  const auto root = Temp(L"scanner");
  const auto quoted = root / L"quoted";
  const auto plain = root / L"plain";
  std::filesystem::create_directories(quoted);
  std::filesystem::create_directories(plain);
  WriteBytes(quoted / L"1Cv8.1CD", "");
  WriteBytes(plain / L"1Cv8.1CD", "");
  const auto contents = L"[Quoted]\nConnect= FILE = \"" + quoted.wstring() + L"\" ;\n" +
      L"[Plain]\nConnect=File=" + plain.wstring() + L";\n";
  ibstart::catalog::Catalog catalog(ibstart::v8i::V8iDocument::ParseUtf8(ibstart::utf::ToUtf8(contents)));
  std::atomic_bool cancelled = false;
  const auto found = ibstart::scanner::FindFileBases({root}, catalog, cancelled);
  CHECK(found.size() == 2);
  CHECK(std::all_of(found.begin(), found.end(), [](const auto& item) { return item.already_registered; }));
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

void TestFileDatabasePathValidation() {
  const auto root = Temp(L"file-database-validation");
  const auto valid = root / L"valid";
  const auto markerDirectory = root / L"marker-directory";
  std::filesystem::create_directories(valid);
  std::filesystem::create_directories(markerDirectory / L"1Cv8.1CD");
  WriteBytes(valid / L"1Cv8.1CD", "");

  CHECK(ibstart::catalog::CheckFileDatabasePath({}) == ibstart::catalog::FileDatabasePathStatus::missing);
  CHECK(ibstart::catalog::CheckFileDatabasePath(valid) == ibstart::catalog::FileDatabasePathStatus::valid);
  CHECK(ibstart::catalog::CheckFileDatabasePath(root / L"missing") == ibstart::catalog::FileDatabasePathStatus::missing);
  CHECK(ibstart::catalog::CheckFileDatabasePath(markerDirectory) == ibstart::catalog::FileDatabasePathStatus::missing);

  ibstart::catalog::Catalog catalog;
  CHECK(!catalog.AddFileDatabase(L"Missing", root / L"missing"));
  CHECK(catalog.AddFileDatabase(L"Valid", valid));
  CHECK(catalog.Find(L"Valid") != nullptr);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

void TestSecretMasking() {
  const auto masked = ibstart::logging::MaskSecrets(
      L"/N admin /P \"s3cret\" --token=abc password=xyz /Password hunter2 Pwd=db-secret");
  CHECK(masked.find(L"s3cret") == std::wstring::npos); CHECK(masked.find(L"abc") == std::wstring::npos); CHECK(masked.find(L"xyz") == std::wstring::npos); CHECK(masked.find(L"hunter2") == std::wstring::npos); CHECK(masked.find(L"db-secret") == std::wstring::npos); CHECK(masked.find(L"admin") != std::wstring::npos);
  const auto masked1cSecrets = ibstart::logging::MaskSecrets(
      L"/WSP web-secret /AccessToken access-token UC=uc-secret PPasswd=proxy-secret SPwd=server-secret "
      L"DBPwd=db-secret ModifyPassword=new-secret https://example.test/?AccessToken=url-token&UC=url-code");
  for (const auto secret : {L"web-secret", L"access-token", L"uc-secret", L"proxy-secret", L"server-secret",
                            L"db-secret", L"new-secret", L"url-token", L"url-code"}) {
    CHECK(masked1cSecrets.find(secret) == std::wstring::npos);
  }
  const auto maskedUrlUserInfo = ibstart::logging::MaskSecrets(
      L"https://login:url-password@example.test/base?AccessToken=url-token");
  CHECK(maskedUrlUserInfo.find(L"login") == std::wstring::npos);
  CHECK(maskedUrlUserInfo.find(L"url-password") == std::wstring::npos);
  CHECK(maskedUrlUserInfo.find(L"url-token") == std::wstring::npos);
  CHECK(maskedUrlUserInfo.find(L"https://***@example.test/base") != std::wstring::npos);
  const auto atInUrlPath = ibstart::logging::MaskSecrets(L"https://example.test/resource@not-userinfo");
  CHECK(atInUrlPath == L"https://example.test/resource@not-userinfo");
  const auto maskedSafeSwitches = ibstart::logging::MaskSecrets(L"/Path C:\\db /Port 1545 /Profile default");
  CHECK(maskedSafeSwitches.find(L"/Path C:\\db") != std::wstring::npos);
  CHECK(maskedSafeSwitches.find(L"/Port 1545") != std::wstring::npos);
  CHECK(maskedSafeSwitches.find(L"/Profile default") != std::wstring::npos);

  const ibstart::domain::LaunchCommand quotedPassword{
      L"C:\\Program Files\\1cv8\\1cv8.exe", {L"ENTERPRISE", L"/P", L"alpha\"VISIBLE_SUFFIX"}};
  const ibstart::domain::LaunchCommand inlinePassword{
      L"C:\\Program Files\\1cv8\\1cv8.exe", {L"ENTERPRISE", L"/P=alpha VISIBLE_SUFFIX"}};
  const ibstart::domain::LaunchCommand connectionPassword{
      L"C:\\Program Files\\1cv8\\1cv8.exe",
      {L"ENTERPRISE", L"/IBConnection", L"DBSrvr=\"srv\";DB=\"base\";Pwd=\"alpha VISIBLE_SUFFIX\""}};
  for (const auto& command : {quotedPassword, inlinePassword, connectionPassword}) {
    const auto redacted = ibstart::logging::RedactedCommandLine(command);
    CHECK(redacted.find(L"alpha") == std::wstring::npos);
    CHECK(redacted.find(L"VISIBLE_SUFFIX") == std::wstring::npos);
    CHECK(redacted.find(L"***") != std::wstring::npos);
  }

  const ibstart::domain::LaunchCommand safeParameters{L"1cv8.exe", {L"ENTERPRISE", L"/Path", L"/Port", L"/Profile"}};
  const ibstart::domain::LaunchCommand namedPassword{L"1cv8.exe", {L"ENTERPRISE", L"--password=alpha"}};
  const ibstart::domain::LaunchCommand separateToken{L"1cv8.exe", {L"ENTERPRISE", L"--token", L"alpha"}};
  const ibstart::domain::LaunchCommand webSecrets{
      L"1cv8.exe", {L"ENTERPRISE", L"/WS", L"https://example.test/?AccessToken=url-token&UC=url-code"}};
  const ibstart::domain::LaunchCommand webUserInfoSecret{
      L"1cv8.exe", {L"ENTERPRISE", L"/WS", L"https://login:url-password@example.test/base"}};
  const ibstart::domain::LaunchCommand browserUserInfoSecret{
      L"", {L"/WS", L"https://login:url-password@example.test/base"}};
  const ibstart::domain::LaunchCommand connectionSecret{
      L"1cv8.exe", {L"ENTERPRISE", L"/IBConnection", L"DBSrvr=\"srv\";Pwd = \"alpha\""}};
  const ibstart::domain::LaunchCommand connectionWithoutSecret{
      L"1cv8.exe", {L"ENTERPRISE", L"/IBConnection", L"DBSrvr=\"srv\";DB=\"base\""}};
  CHECK(!ibstart::logging::ContainsSecretArguments(safeParameters));
  const auto redactedSafeParameters = ibstart::logging::RedactedCommandLine(safeParameters);
  CHECK(redactedSafeParameters.find(L"/Path") != std::wstring::npos);
  CHECK(redactedSafeParameters.find(L"/Port") != std::wstring::npos);
  CHECK(redactedSafeParameters.find(L"/Profile") != std::wstring::npos);
  CHECK(ibstart::logging::ContainsSecretArguments(namedPassword));
  CHECK(ibstart::logging::ContainsSecretArguments(separateToken));
  CHECK(ibstart::logging::ContainsSecretArguments(webSecrets));
  const auto redactedWeb = ibstart::logging::RedactedCommandLine(webSecrets);
  CHECK(redactedWeb.find(L"url-token") == std::wstring::npos);
  CHECK(redactedWeb.find(L"url-code") == std::wstring::npos);
  CHECK(redactedWeb.find(L"AccessToken=***") != std::wstring::npos);
  CHECK(ibstart::logging::ContainsSecretArguments(webUserInfoSecret));
  CHECK(ibstart::logging::ContainsSecretArguments(browserUserInfoSecret));
  const auto redactedWebUserInfo = ibstart::logging::RedactedCommandLine(webUserInfoSecret);
  CHECK(redactedWebUserInfo.find(L"login") == std::wstring::npos);
  CHECK(redactedWebUserInfo.find(L"url-password") == std::wstring::npos);
  CHECK(redactedWebUserInfo.find(L"https://***@example.test/base") != std::wstring::npos);
  CHECK(ibstart::logging::ContainsSecretArguments(connectionSecret));
  CHECK(!ibstart::logging::ContainsSecretArguments(connectionWithoutSecret));
}

void TestLogPruning() {
  const auto directory = Temp(L"log-pruning");
  for (int index = 0; index < 10; ++index) {
    WriteBytes(directory / (L"ibstart_20260101_0000" + std::to_wstring(index) + L".log"), "old");
  }
  ibstart::logging::Logger logger(directory);
  logger.Info(L"new log entry /WS https://login:log-password@example.test/?AccessToken=log-token");
  const auto log = ReadBytes(logger.path());
  CHECK(log.find("login") == std::string::npos);
  CHECK(log.find("log-password") == std::string::npos);
  CHECK(log.find("log-token") == std::string::npos);
  CHECK(log.find("https://***@example.test") != std::string::npos);
  CHECK(log.find("AccessToken=***") != std::string::npos);
  size_t logs = 0;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.is_regular_file() && item.path().extension() == L".log") ++logs;
  }
  CHECK(logs <= 10);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheSizeFormatting() {
  CHECK(ibstart::cache::FormatSize(0) == L"0 Б");
  CHECK(ibstart::cache::FormatSize(1023) == L"1023 Б");
  CHECK(ibstart::cache::FormatSize(1024) == L"1 КБ");
  CHECK(ibstart::cache::FormatSize(1536) == L"1,5 КБ");
  CHECK(ibstart::cache::FormatSize(284097) == L"277,4 КБ");
  CHECK(ibstart::cache::FormatSize(1024ULL * 1024ULL) == L"1 МБ");
  CHECK(ibstart::cache::FormatSize(1024ULL * 1024ULL * 1024ULL) == L"1 ГБ");
}

void TestCacheScanResult() {
  const auto directory = Temp(L"cache-scan-result");
  const auto local = directory / L"local";
  const auto roaming = directory / L"roaming";
  const auto cache = local / L"1C" / L"1Cv8" / L"scan-id";
  const auto fallback_cache = local / L"1C" / L"1Cv8" / L"fallback-name";
  std::filesystem::create_directories(cache / L"nested");
  WriteBytes(cache / L"first.dat", "12345");
  WriteBytes(cache / L"nested" / L"second.dat", "xy");
  std::filesystem::create_directories(fallback_cache);
  WriteBytes(fallback_cache / L"fallback.dat", "fallback");
  std::filesystem::create_directories(local / L"1C" / L"1Cv8" / L"empty-id");
  ScopedEnvironmentVariable local_environment(L"LOCALAPPDATA", local);
  ScopedEnvironmentVariable roaming_environment(L"APPDATA", roaming);

  ibstart::domain::Database database;
  database.id = L"scan-id";
  const auto result = ibstart::cache::Scan(database);
  CHECK(!result.cancelled);
  CHECK(result.complete);
  CHECK(result.bytes == 7);
  CHECK(result.items.size() == 1);
  CHECK(!result.items.empty() && result.items.front().bytes == 7);

  ibstart::domain::Database empty;
  empty.id = L"empty-id";
  const auto empty_result = ibstart::cache::Scan(empty);
  CHECK(empty_result.complete);
  CHECK(!empty_result.cancelled);
  CHECK(empty_result.bytes == 0);
  CHECK(empty_result.items.size() == 1);

  ibstart::domain::Database fallback;
  fallback.name = L"fallback-name";
  const auto fallback_result = ibstart::cache::Scan(fallback);
  CHECK(fallback_result.complete);
  CHECK(fallback_result.bytes == 8);
  CHECK(fallback_result.items.size() == 1);

  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = ibstart::cache::Scan(database, stop.get_token());
  CHECK(cancelled.cancelled);
  CHECK(!cancelled.complete);
  CHECK(cancelled.items.empty());
  CHECK(cancelled.bytes == 0);

  ibstart::domain::Database unsafe;
  unsafe.id = L"bad/id";
  const auto unsafe_result = ibstart::cache::Scan(unsafe);
  CHECK(!unsafe_result.complete);
  CHECK(!unsafe_result.cancelled);
  CHECK(unsafe_result.items.empty());

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheMetricPersistence() {
  const auto directory = Temp(L"cache-metric-persistence");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);

  ibstart::storage::Settings settings;
  settings.show_cache_size_in_list = false;
  ibstart::storage::SaveSettings(layout, settings);
  const auto loaded_settings = ibstart::storage::LoadSettings(layout);
  CHECK(!loaded_settings.show_cache_size_in_list);

  const auto measured_at = std::chrono::system_clock::from_time_t(1787560200);
  ibstart::storage::CatalogState state;
  state.database_metrics[L"database-id"].cache = {123456789, measured_at, false};
  state.database_metrics[L"empty-metric"] = {};
  ibstart::storage::SaveCatalogState(layout, state);

  const auto persisted = ibstart::storage::LoadCatalogState(layout);
  CHECK(persisted.database_metrics.size() == 1);
  CHECK(persisted.database_metrics.contains(L"database-id"));
  if (persisted.database_metrics.contains(L"database-id")) {
    const auto& metric = *persisted.database_metrics.at(L"database-id").cache;
    CHECK(metric.bytes == 123456789);
    CHECK(metric.measured_at == measured_at);
    CHECK(!metric.complete);
  }
  const auto json = ReadBytes(layout.root / L"catalog-state.json");
  CHECK(json.find("\"database_metrics\"") != std::string::npos);
  CHECK(json.find("\"complete\": 0") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheSizeOperationProcessesQueue() {
  const auto directory = Temp(L"cache-size-operation");
  const auto local = directory / L"local";
  const auto roaming = directory / L"roaming";
  const auto first_cache = local / L"1C" / L"1Cv8" / L"first-id";
  const auto second_cache = local / L"1C" / L"1Cv8" / L"second-id";
  std::filesystem::create_directories(first_cache);
  std::filesystem::create_directories(second_cache);
  WriteBytes(first_cache / L"first.dat", "first");
  WriteBytes(second_cache / L"second.dat", "second");
  ScopedEnvironmentVariable local_environment(L"LOCALAPPDATA", local);
  ScopedEnvironmentVariable roaming_environment(L"APPDATA", roaming);

  ibstart::domain::Database first;
  first.id = L"first-id";
  first.name = L"Первая";
  first.connect = L"File=first";
  ibstart::domain::Database second;
  second.id = L"second-id";
  second.name = L"Вторая";
  second.connect = L"File=second";
  ibstart::domain::Database failed;
  failed.id = L"bad/id";
  failed.name = L"Ошибочная";

  ibstart::ui::background::CacheSizeOperation operation;
  operation.Start({first, failed, second}, nullptr, WM_APP + 91);
  for (int attempt = 0; attempt != 2000 && !operation.completed(); ++attempt) Sleep(1);
  CHECK(operation.completed());
  const auto results = operation.TakeResults(8);
  CHECK(results.size() == 3);
  if (results.size() == 3) {
    CHECK(results[0].database_id == L"first-id");
    CHECK(results[0].completed == 1 && results[0].total == 3);
    CHECK(results[0].scan.complete && results[0].scan.bytes == 5);
    CHECK(results[1].database_id == L"bad/id");
    CHECK(results[1].completed == 2 && results[1].total == 3);
    CHECK(!results[1].scan.complete && !results[1].scan.cancelled);
    CHECK(results[2].database_id == L"second-id");
    CHECK(results[2].completed == 3 && results[2].total == 3);
    CHECK(results[2].scan.complete && results[2].scan.bytes == 6);
  }
  operation.StopAndJoin();

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheReportsLocalizedWindowsError() {
  const auto directory = Temp(L"cache-localized-error");
  const auto local = directory / L"local";
  const auto cache = local / L"1C" / L"1Cv8" / L"locked-cache";
  const auto lockedFile = cache / L"locked.dat";
  std::filesystem::create_directories(cache);
  WriteBytes(lockedFile, "keep");

  const HANDLE lock = CreateFileW(lockedFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(lock != INVALID_HANDLE_VALUE);
  if (lock == INVALID_HANDLE_VALUE) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  const auto result = ibstart::cache::Clear({{cache, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());
  CloseHandle(lock);

  const auto expected = WindowsSystemErrorMessage(ERROR_SHARING_VIOLATION);
  CHECK(!expected.empty());
  // On a localized Windows installation this contains non-ASCII characters;
  // comparing the wide system message catches byte-wise conversions from
  // std::error_code::message().
  CHECK(std::any_of(result.errors.begin(), result.errors.end(), [&](const std::wstring& error) {
    return !expected.empty() && error.find(expected) != std::wstring::npos;
  }));
  CHECK(std::filesystem::exists(lockedFile));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheRejectsLicenseDescendants() {
  const auto directory = Temp(L"cache-license-safety");
  const auto local = directory / L"local";
  const auto protectedDirectory = local / L"1C" / L"1Cv8" / L"licenses" / L"nested";
  std::filesystem::create_directories(protectedDirectory);
  WriteBytes(protectedDirectory / L"license.dat", "keep");

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  const auto result = ibstart::cache::Clear({{protectedDirectory, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(!result.errors.empty());
  CHECK(std::filesystem::exists(protectedDirectory / L"license.dat"));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheIdentifiersDoNotCollide() {
  const auto directory = Temp(L"cache-identifiers");
  const auto local = directory / L"local";
  const std::wstring identifier = L"{11111111-1111-4111-8111-111111111111}";
  const auto validCache = local / L"1C" / L"1Cv8" / identifier;
  std::filesystem::create_directories(validCache);
  WriteBytes(validCache / L"cache.dat", "data");
  std::filesystem::create_directories(local / L"1C" / L"1Cv8" / L"a_b");

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  ibstart::domain::Database valid;
  valid.id = identifier;
  const auto validCandidates = ibstart::cache::CandidatesFor(valid);
  std::stop_source cancelledScan;
  CHECK(cancelledScan.request_stop());
  const auto cancelledCandidates = ibstart::cache::CandidatesFor(valid, cancelledScan.get_token());
  ibstart::domain::Database unsafe;
  unsafe.id = L"a/b";
  const auto unsafeCandidates = ibstart::cache::CandidatesFor(unsafe);
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(std::any_of(validCandidates.begin(), validCandidates.end(), [&](const auto& item) { return item.path == validCache; }));
  CHECK(cancelledCandidates.empty());
  CHECK(unsafeCandidates.empty());
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheRejectsJunctions() {
  const auto directory = Temp(L"cache-junction-safety");
  const auto local = directory / L"local";
  const auto cacheRoot = local / L"1C" / L"1Cv8";
  const auto cache = cacheRoot / L"cache";
  const auto external = directory / L"external";
  const auto junction = cache / L"external-junction";
  const auto realTarget = cacheRoot / L"real-target";
  const auto rootJunction = cacheRoot / L"root-junction";
  std::filesystem::create_directories(cache);
  std::filesystem::create_directories(external);
  std::filesystem::create_directories(realTarget);
  WriteBytes(cache / L"keep.dat", "keep");
  WriteBytes(external / L"do-not-delete.dat", "keep");
  WriteBytes(realTarget / L"do-not-delete.dat", "keep");

  const bool junction_created = CreateJunction(junction, external);
  CHECK(junction_created);
  if (!junction_created) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }
  const bool root_junction_created = CreateJunction(rootJunction, realTarget);
  CHECK(root_junction_created);
  if (!root_junction_created) {
    RemoveDirectoryLink(junction);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  const auto result = ibstart::cache::Clear({{cache, 0}, {rootJunction, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(result.errors.size() == 2);
  CHECK(std::filesystem::exists(cache / L"keep.dat"));
  CHECK(EntryExistsWithoutFollowingReparsePoint(junction));
  CHECK(std::filesystem::exists(external / L"do-not-delete.dat"));
  CHECK(EntryExistsWithoutFollowingReparsePoint(rootJunction));
  CHECK(std::filesystem::exists(realTarget / L"do-not-delete.dat"));

  RemoveDirectoryLink(junction);
  RemoveDirectoryLink(rootJunction);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheAllowsJunctionedTrustedRoot() {
  const auto directory = Temp(L"cache-junctioned-trusted-root");
  const auto physical_local = directory / L"physical-local";
  const auto aliased_local = directory / L"local-alias";
  const auto physical_cache = physical_local / L"1C" / L"1Cv8" / L"valid-cache";
  const auto aliased_cache = aliased_local / L"1C" / L"1Cv8" / L"valid-cache";
  std::filesystem::create_directories(physical_cache);
  WriteBytes(physical_cache / L"cache.dat", "remove");

  const bool junction_created = CreateJunction(aliased_local, physical_local);
  CHECK(junction_created);
  if (!junction_created) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", aliased_local.c_str());
  const auto result = ibstart::cache::Clear({{aliased_cache, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(result.errors.empty());
  CHECK(!std::filesystem::exists(aliased_cache));
  CHECK(!std::filesystem::exists(physical_cache));

  RemoveDirectoryLink(aliased_local);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCacheRejectsSymbolicReparsePoints() {
  const auto directory = Temp(L"cache-symbolic-link-safety");
  const auto local = directory / L"local";
  const auto cache = local / L"1C" / L"1Cv8" / L"cache";
  const auto external = directory / L"external";
  const auto link = cache / L"external-symbolic-link";
  std::filesystem::create_directories(cache);
  std::filesystem::create_directories(external);
  WriteBytes(cache / L"keep.dat", "keep");
  WriteBytes(external / L"do-not-delete.dat", "keep");

  const DWORD link_error = CreateDirectorySymbolicLink(link, external);
  if (link_error != ERROR_SUCCESS) {
    if (link_error != ERROR_PRIVILEGE_NOT_HELD && link_error != ERROR_ACCESS_DENIED &&
        link_error != ERROR_NOT_SUPPORTED && link_error != ERROR_INVALID_PARAMETER &&
        link_error != ERROR_INVALID_FUNCTION) {
      CHECK(false);
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  const auto result = ibstart::cache::Clear({{cache, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(!result.errors.empty());
  CHECK(std::filesystem::exists(cache / L"keep.dat"));
  CHECK(EntryExistsWithoutFollowingReparsePoint(link));
  CHECK(std::filesystem::exists(external / L"do-not-delete.dat"));

  RemoveDirectoryLink(link);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestPortableMode() {
  const auto directory = Temp(L"portable"); const auto executable = directory / L"IBStart.exe"; WriteBytes(executable, ""); WriteBytes(directory / L"IBStart.portable", "");
  const auto layout = ibstart::storage::ResolveLayout(executable); CHECK(layout.portable); CHECK(layout.root == directory / L"data"); ibstart::storage::EnsureWritable(layout);
  WriteBytes(layout.root / L"favorites.json", "[\"Устаревшее избранное\"]\n");
  ibstart::storage::CatalogStateRepository repository(layout);
  CHECK(repository.Read().favorites.empty());
  std::error_code removeLegacyError; std::filesystem::remove(layout.root / L"favorites.json", removeLegacyError);
  ibstart::storage::Settings settings; settings.active_ibases = directory / L"База 😀.v8i"; settings.selected_entry = L"Выбранная база 😀"; settings.simple_mode = true; settings.show_tags_in_list = false; settings.folders_first_when_sorting = false; settings.open_last_list_on_startup = false; settings.restore_last_selection = false; settings.default_client_type = ibstart::domain::ClientType::thin; settings.default_architecture = ibstart::domain::ClientArchitecture::x64_priority; settings.default_platform_version = L"8.3.27"; settings.confirm_destructive_actions = false; settings.show_details_panel = false; settings.show_status_bar = false; settings.tree_density = 2; settings.remember_launch_history = false; settings.launch_history_limit = 5; settings.recent_lists_limit = 3; settings.recent_ibases = {directory / L"Недавняя 1.v8i", directory / L"Недавняя 2.v8i"}; settings.platform_search_paths = {directory / L"Платформа"}; settings.window_width = 1234;
  ibstart::storage::SaveSettings(layout, settings); const auto loaded = ibstart::storage::LoadSettings(layout);
  CHECK(loaded.active_ibases == settings.active_ibases); CHECK(loaded.selected_entry == settings.selected_entry); CHECK(loaded.simple_mode); CHECK(!loaded.show_tags_in_list); CHECK(!loaded.folders_first_when_sorting); CHECK(!loaded.open_last_list_on_startup); CHECK(!loaded.restore_last_selection); CHECK(loaded.default_client_type == ibstart::domain::ClientType::thin); CHECK(loaded.default_architecture == ibstart::domain::ClientArchitecture::x64_priority); CHECK(loaded.default_platform_version == L"8.3.27"); CHECK(!loaded.confirm_destructive_actions); CHECK(!loaded.show_details_panel); CHECK(!loaded.show_status_bar); CHECK(loaded.tree_density == 2); CHECK(!loaded.remember_launch_history); CHECK(loaded.launch_history_limit == 5); CHECK(loaded.recent_lists_limit == 3); CHECK(loaded.recent_ibases == settings.recent_ibases); CHECK(loaded.platform_search_paths == settings.platform_search_paths); CHECK(loaded.window_width == 1234);
  const std::vector<std::wstring> favorites = {L"База 😀", L"Строка\nс переводом"};
  const ibstart::storage::DatabaseTags tags = {{L"id-😀", {L"Продуктив", L"[Клиент] \"А\""}}, {L"id-2", {L"Тест"}}};
  const ibstart::storage::TagStyles tagStyles = {{L"[Клиент] \"А\"", {RGB(236, 217, 245), RGB(75, 20, 95)}}};
  repository.Update([&](ibstart::storage::CatalogState& state) { state.favorites = favorites; });
  repository.Update([&](ibstart::storage::CatalogState& state) { state.tags = tags; });
  repository.Update([&](ibstart::storage::CatalogState& state) { state.tag_styles = tagStyles; });
  CHECK(repository.Read().favorites == favorites); CHECK(repository.Read().tags == tags); CHECK(repository.Read().tag_styles == tagStyles);
  const auto launchTime = std::chrono::system_clock::from_time_t(123456789);
  repository.AppendHistory({L"id-😀", launchTime, ibstart::domain::LaunchMode::designer}); const auto& history = repository.Read().history; CHECK(history.size() == 1); CHECK(!history.empty() && history[0].database_id == L"id-😀");
  const auto launches = repository.Read().last_launches; CHECK(launches.contains(L"id-😀") && launches.at(L"id-😀") == launchTime);
  repository.ClearHistory();
  const auto state = ibstart::storage::LoadCatalogState(layout);
  CHECK(state.history.empty()); CHECK(state.last_launches == launches); CHECK(state.favorites == favorites);
  CHECK(state.tags == tags); CHECK(state.tag_styles == tagStyles);
  CHECK(std::filesystem::is_regular_file(layout.root / L"catalog-state.json"));
  CHECK(!std::filesystem::exists(layout.root / L"history.json")); CHECK(!std::filesystem::exists(layout.root / L"last-launches.json"));
  CHECK(!std::filesystem::exists(layout.root / L"favorites.json")); CHECK(!std::filesystem::exists(layout.root / L"tags.json"));
  CHECK(!std::filesystem::exists(layout.root / L"tag-styles.json")); CHECK(!std::filesystem::exists(layout.root / L"sorting.json"));
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestWindowsPathLimit() {
  constexpr auto limit = ibstart::windows_path::kMaximumPathCharacters;
  const std::wstring prefix = L"C:\\";
  const auto supported = std::filesystem::path(prefix + std::wstring(limit - 1 - prefix.size(), L'x'));
  const auto tooLong = std::filesystem::path(prefix + std::wstring(limit - prefix.size(), L'x'));

  CHECK(supported.native().size() == limit - 1);
  CHECK(ibstart::windows_path::IsWithinLimit(supported));
  CHECK(tooLong.native().size() == limit);
  CHECK(!ibstart::windows_path::IsWithinLimit(tooLong));
  CHECK(ibstart::windows_path::LengthError(tooLong).find(L"32766") != std::wstring::npos);
  CHECK(ibstart::catalog::CheckFileDatabasePath(tooLong) ==
      ibstart::catalog::FileDatabasePathStatus::too_long);

  bool v8iPathRejected = false;
  try {
    ibstart::v8i::V8iFileStore store(tooLong);
  } catch (const std::runtime_error&) {
    v8iPathRejected = true;
  }
  CHECK(v8iPathRejected);

  bool portablePathRejected = false;
  try {
    static_cast<void>(ibstart::storage::ResolveLayout(tooLong));
  } catch (const std::runtime_error&) {
    portablePathRejected = true;
  }
  CHECK(portablePathRejected);
}

void TestStartupUpdateSettings() {
  const auto directory = Temp(L"startup-update-settings");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  CHECK(ibstart::storage::Settings{}.check_updates_on_startup);
  CHECK(ibstart::storage::LoadSettings(layout).check_updates_on_startup);
  // Older profiles and properties of an unsupported type use the enabled default.
  WriteBytes(directory / L"settings.json", "{}");
  CHECK(ibstart::storage::LoadSettings(layout).check_updates_on_startup);
  WriteBytes(directory / L"settings.json", "{\"check_updates_on_startup\":\"0\"}");
  CHECK(ibstart::storage::LoadSettings(layout).check_updates_on_startup);
  auto settings = ibstart::storage::LoadSettings(layout);
  settings.check_updates_on_startup = false;
  ibstart::storage::SaveSettings(layout, settings);
  CHECK(!ibstart::storage::LoadSettings(layout).check_updates_on_startup);

  ibstart::storage::SettingsRepository first(layout), stale(layout);
  auto changed = first.Read();
  auto unrelated = stale.Read();
  changed.check_updates_on_startup = true;
  first.Save(changed);
  unrelated.simple_mode = true;
  stale.Save(unrelated);
  CHECK(ibstart::storage::LoadSettings(layout).check_updates_on_startup);
  CHECK(ibstart::storage::LoadSettings(layout).simple_mode);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestProfileTransfer() {
  const auto directory = Temp(L"profile-transfer");
  const ibstart::storage::StorageLayout source{directory / L"source", false};
  const ibstart::storage::StorageLayout target{directory / L"target", false};
  ibstart::storage::EnsureWritable(source);
  ibstart::storage::Settings source_settings;
  source_settings.active_ibases = L"C:\\catalog.v8i";
  source_settings.check_updates_on_startup = false;
  source_settings.credentials = {{L"source-id", L"Source", L"user", L"secret", ibstart::credentials::ScopeMode::all, {}}};
  ibstart::storage::SaveSettings(source, source_settings);
  ibstart::storage::CatalogState source_state;
  source_state.favorites = {L"source-db"};
  ibstart::storage::SaveCatalogState(source, source_state);

  const auto expect_export_rejected = [&](const std::filesystem::path& destination) {
    bool rejected = false;
    try {
      ibstart::storage::ExportProfile(source, destination, false);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    CHECK(rejected);
  };
  expect_export_rejected(source.root);
  expect_export_rejected(source.root / L".");
  expect_export_rejected(source.root.parent_path() / L"SOURCE");
  CHECK(ibstart::storage::IsSameProfilePath(source.root, source.root / L"."));
  CHECK(ibstart::storage::IsSameProfilePath(source.root, source.root.parent_path() / L"SOURCE"));
  CHECK(!ibstart::storage::IsSameProfilePath(source.root, target.root));
  CHECK(ibstart::storage::LoadSettings(source).credentials == source_settings.credentials);

  ibstart::storage::ExportProfile(source, target.root, false);
  CHECK(ibstart::storage::HasCompleteProfileFiles(target.root));
  CHECK(ibstart::storage::LoadSettings(target).credentials.empty());
  CHECK(!ibstart::storage::LoadSettings(target).check_updates_on_startup);
  CHECK(ibstart::storage::LoadCatalogState(target).favorites == source_state.favorites);

  const ibstart::storage::StorageLayout transaction_target{directory / L"transaction-target", false};
  ibstart::storage::EnsureWritable(transaction_target);
  ibstart::storage::Settings transaction_settings;
  transaction_settings.selected_entry = L"before";
  ibstart::storage::SaveSettings(transaction_target, transaction_settings);
  ibstart::storage::SaveCatalogState(transaction_target, {});
  const auto settings_before_failed_export = ReadBytes(transaction_target.root / L"settings.json");
  std::filesystem::remove(transaction_target.root / L"catalog-state.json");
  std::filesystem::create_directory(transaction_target.root / L"catalog-state.json");
  bool transaction_rejected = false;
  try {
    ibstart::storage::ExportProfile(source, transaction_target.root, false);
  } catch (const std::runtime_error&) {
    transaction_rejected = true;
  }
  CHECK(transaction_rejected);
  CHECK(ReadBytes(transaction_target.root / L"settings.json") == settings_before_failed_export);
  std::error_code transaction_error;
  std::filesystem::remove(transaction_target.root / L"catalog-state.json", transaction_error);

  ibstart::storage::Settings target_settings;
  target_settings.credentials = {{L"target-id", L"Target", L"user", L"target-secret", ibstart::credentials::ScopeMode::all, {}}};
  ibstart::storage::SaveSettings(target, target_settings);
  ibstart::storage::ImportProfile(source.root, target, false);
  CHECK(ibstart::storage::LoadSettings(target).credentials == target_settings.credentials);
  CHECK(!ibstart::storage::LoadSettings(target).check_updates_on_startup);
  ibstart::storage::ImportProfile(source.root, target, true);
  CHECK(ibstart::storage::LoadSettings(target).credentials == source_settings.credentials);

  const auto expect_import_rejected = [&](const std::filesystem::path& partial_root) {
    bool rejected = false;
    try {
      ibstart::storage::ImportProfile(partial_root, target, false);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    CHECK(rejected);
    CHECK(ibstart::storage::LoadSettings(target).credentials == source_settings.credentials);
    CHECK(ibstart::storage::LoadCatalogState(target).favorites == source_state.favorites);
  };
  const ibstart::storage::StorageLayout settings_only{directory / L"settings-only", false};
  ibstart::storage::EnsureWritable(settings_only);
  ibstart::storage::SaveSettings(settings_only, source_settings);
  CHECK(!ibstart::storage::HasCompleteProfileFiles(settings_only.root));
  expect_import_rejected(settings_only.root);

  const ibstart::storage::StorageLayout state_only{directory / L"state-only", false};
  ibstart::storage::EnsureWritable(state_only);
  ibstart::storage::SaveCatalogState(state_only, source_state);
  CHECK(!ibstart::storage::HasCompleteProfileFiles(state_only.root));
  expect_import_rejected(state_only.root);

  const ibstart::storage::StorageLayout malformed{directory / L"malformed", false};
  ibstart::storage::EnsureWritable(malformed);
  WriteBytes(malformed.root / L"settings.json", "{\"active_ibases\": ");
  WriteBytes(malformed.root / L"catalog-state.json", ReadBytes(source.root / L"catalog-state.json"));
  expect_import_rejected(malformed.root);

  const ibstart::storage::StorageLayout unsupported{directory / L"unsupported", false};
  ibstart::storage::EnsureWritable(unsupported);
  WriteBytes(unsupported.root / L"settings.json", "{\"schema_version\": 99}");
  WriteBytes(unsupported.root / L"catalog-state.json", ReadBytes(source.root / L"catalog-state.json"));
  expect_import_rejected(unsupported.root);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestConfigurationCatalog() {
  using namespace ibstart;
  const auto directory = Temp(L"configuration-catalog");
  const storage::StorageLayout layout{directory / L"source", true};
  storage::EnsureWritable(layout);
  const auto list_a = directory / L"a.v8i", list_b = directory / L"b.v8i";
  WriteBytes(list_a, "[Shared]\nConnect=Srvr=server;Ref=base;\n");
  const auto original_list = ReadBytes(list_a);
  catalog::CatalogMetadataService service(layout);
  service.SetConfigurationScope(list_a);
  const auto predefined = service.Read().configurations;
  CHECK(predefined.size() >= 6);
  CHECK(std::filesystem::exists(layout.root / L"configurations.json"));
  const auto id = predefined.front().id;
  service.SetDatabaseConfiguration(L"Shared", id, L" 3.0.142.32 ");
  service.SetConfigurationScope(list_b);
  CHECK(!configuration::FindAssignment(service.Read().database_configurations, service.Read().configuration_scope, L"Shared"));
  service.SetDatabaseConfiguration(L"Shared", id, L"3.0.150.1");
  service.SetConfigurationScope(list_a);
  CHECK(configuration::FindAssignment(service.Read().database_configurations, service.Read().configuration_scope, L"shared")->version == L"3.0.142.32");
  auto edited = predefined.front();
  edited.title = L"Переименованная конфигурация";
  edited.short_name = L"БП тест";
  service.SaveConfiguration(edited);
  CHECK(configuration::Badge(service.Read().configurations,
      configuration::FindAssignment(service.Read().database_configurations, service.Read().configuration_scope, L"Shared")) == L"БП тест · 3.0.142.32");
  bool rejected = false;
  try { service.DeleteConfiguration(id); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);
  service.RemoveDatabaseConfiguration(L"Shared");
  rejected = false;
  try { service.DeleteConfiguration(id); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);  // References in another catalog still block deletion.
  service.SetConfigurationScope(list_b);
  service.RenameDatabaseMetadata(L"Shared", L"Renamed", L"Shared", L"Renamed");
  CHECK(!configuration::FindAssignment(service.Read().database_configurations, service.Read().configuration_scope, L"Shared"));
  CHECK(configuration::FindAssignment(service.Read().database_configurations, service.Read().configuration_scope, L"Renamed"));
  rejected = false;
  try { service.SetDatabaseConfiguration(L"Renamed", id, L"bad\nversion"); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);
  service.SetDatabaseConfiguration(L"VersionOnly", {}, L"custom-beta");
  CHECK(ReadBytes(list_a) == original_list); // Local metadata never writes ibases.v8i.
  storage::SaveSettings(layout, {});
  const auto exported = directory / L"export";
  storage::ExportProfile(layout, exported, false);
  CHECK(std::filesystem::exists(exported / L"configurations.json"));
  const storage::StorageLayout target{directory / L"target", true};
  storage::ImportProfile(exported, target, false);
  const auto transferred = storage::LoadCatalogState(target);
  CHECK(transferred.configurations == service.Read().configurations);
  CHECK(transferred.database_configurations == service.Read().database_configurations);
  const auto before_settings = ReadBytes(target.root / L"settings.json");
  const auto before_state = ReadBytes(target.root / L"catalog-state.json");
  const auto before_definitions = ReadBytes(target.root / L"configurations.json");
  WriteBytes(exported / L"configurations.json", "{\"schema_version\": 99, \"configurations\": []}");
  rejected = false;
  try { storage::ImportProfile(exported, target, false); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);
  CHECK(ReadBytes(target.root / L"settings.json") == before_settings);
  CHECK(ReadBytes(target.root / L"catalog-state.json") == before_state);
  CHECK(ReadBytes(target.root / L"configurations.json") == before_definitions);
  // Importing a legacy two-file profile initializes the dictionary and clears old bindings.
  storage::CatalogState legacy;
  storage::SaveCatalogState({exported, false}, legacy);
  std::filesystem::remove(exported / L"configurations.json");
  storage::ImportProfile(exported, target, false);
  CHECK(storage::LoadCatalogState(target).database_configurations.empty());
  CHECK(storage::LoadCatalogState(target).configurations == configuration::BuiltInDefinitions());
  service.RemoveDatabaseConfiguration(L"Renamed");
  service.DeleteConfiguration(id);
  CHECK(!configuration::Find(service.Read().configurations, id));
  CHECK(!configuration::Find(service.Reload().configurations, id)); // Deleted seeds do not reappear.
  auto custom = configuration::Definition{configuration::NewId(), L"SameName", L"Редакция один", L"Один"};
  service.SaveConfiguration(custom);
  auto another = custom; another.id = configuration::NewId(); another.title = L"Редакция два";
  service.SaveConfiguration(another); // Metadata names are not unique between editions.
  another.title = custom.title;
  rejected = false;
  try { service.SaveConfiguration(another); } catch (const std::runtime_error&) { rejected = true; }
  CHECK(rejected);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestProfileTransferRejectsInterruptedTransaction() {
  using namespace ibstart;
  const auto directory = Temp(L"profile-interrupted-transaction");
  const storage::StorageLayout source{directory / L"source", true};
  const storage::StorageLayout target{directory / L"target", true};
  storage::EnsureWritable(source);
  storage::EnsureWritable(target);
  storage::SaveSettings(source, {});
  storage::SaveCatalogState(source, {});
  storage::SaveSettings(target, {});
  storage::SaveCatalogState(target, {});
  static_cast<void>(storage::LoadCatalogState(target));
  storage::Settings source_settings;
  source_settings.selected_entry = L"imported";
  storage::SaveSettings(source, source_settings);
  const auto settings_before = ReadBytes(target.root / L"settings.json");
  const auto state_before = ReadBytes(target.root / L"catalog-state.json");
  const auto definitions_before = ReadBytes(target.root / L"configurations.json");
  for (const auto name : {L"settings.json.profile-backup.4294967295", L"catalog-state.json.profile-txn.4294967295",
      L"configurations.json.profile-backup.4294967295"}) {
    const auto interrupted = target.root / name;
    WriteBytes(interrupted, "previous process recovery data");
    for (const bool import : {true, false}) {
      bool rejected = false;
      try {
        if (import) storage::ImportProfile(source.root, target, false);
        else storage::ExportProfile(source, target.root, false);
      } catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
      CHECK(ReadBytes(target.root / L"settings.json") == settings_before);
      CHECK(ReadBytes(target.root / L"catalog-state.json") == state_before);
      CHECK(ReadBytes(target.root / L"configurations.json") == definitions_before);
      CHECK(ReadBytes(interrupted) == "previous process recovery data");
    }
    std::filesystem::remove(interrupted);
    const auto source_interrupted = source.root / name;
    WriteBytes(source_interrupted, "previous process recovery data");
    for (const bool import : {true, false}) {
      bool rejected = false;
      try {
        if (import) storage::ImportProfile(source.root, target, false);
        else storage::ExportProfile(source, target.root, false);
      } catch (const std::runtime_error&) { rejected = true; }
      CHECK(rejected);
      CHECK(ReadBytes(target.root / L"settings.json") == settings_before);
      CHECK(ReadBytes(source_interrupted) == "previous process recovery data");
    }
    std::filesystem::remove(source_interrupted);
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestConfigurationSearchAndIsolation() {
  using namespace ibstart;
  const auto directory = Temp(L"configuration-search");
  const auto scope = directory / L"ibases.v8i";
  catalog::Catalog bases(v8i::V8iDocument::ParseUtf8("[Group]\n[One]\nConnect=Srvr=s;Ref=one;\nFolder=Group\n[Two]\nConnect=Srvr=s;Ref=two;\nFolder=Group\n"));
  storage::CatalogState state;
  state.configuration_scope = configuration::NormalizeCatalogPath(scope);
  state.configurations = {{L"config-id", L"УправлениеТорговлей", L"Управление торговлей, редакция 11", L"УТ11"}};
  state.database_configurations = {{state.configuration_scope, L"One", L"config-id", L"11.5.22.123"}};
  ui::presentation::TreeTagFilter filter;
  for (const auto* query : {L"УТ11", L"управлениеторговлей", L"редакция 11", L"11.5.22"}) {
    const auto items = ui::presentation::FilterTreeItems(bases, bases.Tree(), query, filter, state.tags, {}, &state);
    CHECK(items.size() == 1 && items.front().children.size() == 1 && items.front().children.front().name == L"One");
  }
  // A matching tag on another child cannot retain an empty parent group.
  state.tags[L"Two"] = {L"Продуктив"};
  filter = {ui::presentation::TreeTagFilterKind::tag, L"Продуктив"};
  CHECK(ui::presentation::FilterTreeItems(bases, bases.Tree(), L"УТ11", filter, state.tags, {}, &state).empty());
  state.tags[L"One"] = {L"Продуктив"};
  CHECK(!ui::presentation::FilterTreeItems(bases, bases.Tree(), L"УТ11", filter, state.tags, {}, &state).empty());
  state.configuration_scope = configuration::NormalizeCatalogPath(directory / L"other.v8i");
  CHECK(ui::presentation::FilterTreeItems(bases, bases.Tree(), L"УТ11", filter, state.tags, {}, &state).empty());
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCatalogStateRepository() {
  const auto directory = Temp(L"catalog-state-repository");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  ibstart::storage::CatalogStateRepository repository(layout);
  CHECK(repository.Read().favorites.empty());

  const auto timestamp = std::chrono::system_clock::from_time_t(123456789);
  repository.Update([](ibstart::storage::CatalogState& state) {
    state.favorites = {L"Основная база"};
    state.tags[L"database-id"] = {L"Продуктив"};
  });
  repository.AppendHistory({L"database-id", timestamp, ibstart::domain::LaunchMode::enterprise});
  repository.Update([](ibstart::storage::CatalogState& state) {
    state.tag_styles[L"Продуктив"] = {RGB(1, 2, 3), RGB(4, 5, 6)};
  });

  const auto persisted = ibstart::storage::LoadCatalogState(layout);
  CHECK(persisted.favorites == std::vector<std::wstring>{L"Основная база"});
  CHECK(persisted.tags.contains(L"database-id"));
  CHECK(persisted.tag_styles.contains(L"Продуктив"));
  CHECK(persisted.history.size() == 1 && persisted.history.front().database_id == L"database-id");
  CHECK(persisted.last_launches.contains(L"database-id"));

  repository.RemoveHistory(L"DATABASE-ID");
  CHECK(repository.Read().history.empty());
  CHECK(repository.Read().last_launches.contains(L"database-id"));

  repository.ClearHistory();
  CHECK(repository.Read().history.empty());
  CHECK(repository.Read().last_launches.contains(L"database-id"));

  ibstart::storage::CatalogState external;
  external.favorites = {L"Внешнее изменение"};
  ibstart::storage::SaveCatalogState(layout, external);
  CHECK(repository.Read().favorites == std::vector<std::wstring>{L"Основная база"});
  CHECK(repository.Reload().favorites == std::vector<std::wstring>{L"Внешнее изменение"});

  ibstart::storage::CatalogState replacement;
  replacement.favorites = {L"Принятое состояние"};
  repository.Adopt(replacement);
  CHECK(repository.Read().favorites == std::vector<std::wstring>{L"Принятое состояние"});

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestStorageFileSizeLimit() {
  const auto directory = Temp(L"storage-size-limit");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);

  const auto settings = layout.root / L"settings.json";
  WriteBytes(settings, "{}");
  std::filesystem::resize_file(settings, ibstart::storage::kMaxStorageFileSize + 1);
  bool settingsRejected = false;
  std::string settingsDiagnostic;
  try {
    static_cast<void>(ibstart::storage::LoadSettings(layout));
  } catch (const std::exception& error) {
    settingsRejected = true;
    settingsDiagnostic = error.what();
  }
  CHECK(settingsRejected);
  CHECK(settingsDiagnostic.find("safety limit") != std::string::npos);
  CHECK(settingsDiagnostic.find("4194305") != std::string::npos);

  const auto state = layout.root / L"catalog-state.json";
  WriteBytes(state, "{}");
  std::filesystem::resize_file(state, ibstart::storage::kMaxStorageFileSize + 1);
  bool stateRejected = false;
  try {
    static_cast<void>(ibstart::storage::LoadCatalogState(layout));
  } catch (const std::exception&) {
    stateRejected = true;
  }
  CHECK(stateRejected);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestStorageRepositoryMergesStaleSnapshots() {
  const auto directory = Temp(L"storage-repository-merge");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  CHECK(ibstart::storage::StorageMutexName(layout).starts_with(L"Global\\IBStart.Storage."));
  CHECK(ibstart::storage::InstanceMutexName(layout).starts_with(L"Global\\IBStart.Instance."));
  CHECK(ibstart::storage::StorageMutexName(layout) != ibstart::storage::StorageMutexName({directory / L"other", true}));
  CHECK(ibstart::storage::InstanceIdentity(layout) != ibstart::storage::InstanceIdentity({directory / L"other", true}));

  ibstart::storage::CatalogStateRepository firstState(layout);
  ibstart::storage::CatalogStateRepository secondState(layout);
  static_cast<void>(firstState.Read());
  static_cast<void>(secondState.Read());
  std::atomic<bool> firstEntered{false};
  std::atomic<bool> updateFailed{false};
  std::thread firstUpdate([&] {
    try {
      firstState.Update([&](ibstart::storage::CatalogState& state) {
        firstEntered = true;
        Sleep(100);
        state.favorites = {L"Из первого репозитория"};
      });
    } catch (...) {
      updateFailed = true;
    }
  });
  while (!firstEntered && !updateFailed) Sleep(1);
  std::thread secondUpdate([&] {
    try {
      secondState.Update([](ibstart::storage::CatalogState& state) {
        state.tags[L"database-id"] = {L"Из второго репозитория"};
      });
    } catch (...) {
      updateFailed = true;
    }
  });
  firstUpdate.join();
  secondUpdate.join();
  CHECK(!updateFailed);

  const auto mergedState = ibstart::storage::LoadCatalogState(layout);
  CHECK(mergedState.favorites == std::vector<std::wstring>{L"Из первого репозитория"});
  CHECK(mergedState.tags.contains(L"database-id"));
  CHECK(mergedState.tags.at(L"database-id") == std::vector<std::wstring>{L"Из второго репозитория"});

  bool conflictRejected = false;
  try {
    firstState.Update([&](ibstart::storage::CatalogState& state) {
      WriteBytes(layout.root / L"catalog-state.json", R"({"favorites":[{"favorite":"Внешний процесс"}]})");
      state.favorites = {L"Устаревшая запись"};
    });
  } catch (const ibstart::storage::StorageConflictError&) {
    conflictRejected = true;
  }
  CHECK(conflictRejected);
  CHECK(ibstart::storage::LoadCatalogState(layout).favorites == std::vector<std::wstring>{L"Внешний процесс"});

  ibstart::storage::SettingsRepository firstSettings(layout);
  ibstart::storage::SettingsRepository secondSettings(layout);
  const auto firstSnapshot = firstSettings.Read();
  const auto secondSnapshot = secondSettings.Read();
  auto firstSettingsUpdate = firstSnapshot;
  firstSettingsUpdate.simple_mode = true;
  firstSettings.Save(firstSettingsUpdate);
  auto secondSettingsUpdate = secondSnapshot;
  secondSettingsUpdate.show_tags_in_list = false;
  secondSettings.Save(secondSettingsUpdate);

  const auto mergedSettings = ibstart::storage::LoadSettings(layout);
  CHECK(mergedSettings.simple_mode);
  CHECK(!mergedSettings.show_tags_in_list);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCatalogMetadataService() {
  const auto directory = Temp(L"catalog-metadata-service");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  ibstart::catalog::CatalogMetadataService service(layout);

  CHECK(service.ToggleFavorite(L"Основная база"));
  CHECK(!service.ToggleFavorite(L"Основная база"));
  for (unsigned index = 0; index != 10; ++index) CHECK(service.ToggleFavorite(L"База " + std::to_wstring(index)));
  CHECK(service.Read().favorites.size() == ibstart::catalog::CatalogMetadataService::kMaxFavorites);
  CHECK(service.Read().favorites.front() == L"База 9");
  CHECK(std::find(service.Read().favorites.begin(), service.Read().favorites.end(), L"База 0") == service.Read().favorites.end());

  service.SetTags(L"old-id", {L"Продуктив"});
  CHECK(!service.AddTag(L"old-id", L"продуктив"));
  CHECK(service.AddTag(L"old-id", L"Тест"));
  service.ReplaceTagConfiguration(service.Read().tags, {{L"Продуктив", {RGB(1, 2, 3), RGB(4, 5, 6)}}});
  service.RenameDatabaseMetadata(L"База 9", L"Переименованная база", L"old-id", L"new-id");
  CHECK(service.Read().favorites.front() == L"Переименованная база");
  CHECK(!service.Read().tags.contains(L"old-id"));
  CHECK(service.Read().tags.at(L"new-id") == std::vector<std::wstring>{L"Продуктив", L"Тест"});
  CHECK(service.Read().tag_styles.contains(L"Продуктив"));
  service.SetTags(L"id-only-old", {L"ID обновлён"});
  service.RenameDatabaseMetadata(L"Без переименования", L"Без переименования", L"id-only-old", L"id-only-new");
  CHECK(!service.Read().tags.contains(L"id-only-old"));
  CHECK(service.Read().tags.contains(L"id-only-new"));
  CHECK(service.RemoveTags(L"new-id"));
  CHECK(!service.RemoveTags(L"new-id"));

  const auto timestamp = std::chrono::system_clock::from_time_t(123456789);
  service.RecordLaunch({L"new-id", timestamp, ibstart::domain::LaunchMode::designer});
  CHECK(service.Read().history.size() == 1);
  CHECK(service.Read().last_launches.contains(L"new-id"));
  service.ClearHistory();
  CHECK(service.Read().history.empty());
  CHECK(service.Read().last_launches.contains(L"new-id"));

  const auto persisted = ibstart::storage::LoadCatalogState(layout);
  CHECK(persisted.favorites == service.Read().favorites);
  CHECK(persisted.tags == service.Read().tags);
  CHECK(persisted.tag_styles == service.Read().tag_styles);
  CHECK(persisted.history.empty());
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCatalogMetadataRenamePreservesFallbackHistory() {
  const auto directory = Temp(L"catalog-metadata-rename-history");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  ibstart::catalog::CatalogMetadataService service(layout);

  const std::wstring previous_name = L"База до переименования";
  const std::wstring updated_name = L"База после переименования";
  const auto previous_timestamp = std::chrono::system_clock::from_time_t(223456789);
  const auto updated_timestamp = std::chrono::system_clock::from_time_t(123456789);

  service.SetTags(previous_name, {L"Общий", L"Старый"});
  service.SetTags(updated_name, {L"Общий", L"Новый"});
  CHECK(service.ToggleFavorite(previous_name));
  CHECK(service.ToggleFavorite(updated_name));
  service.RecordLaunch({previous_name, previous_timestamp, ibstart::domain::LaunchMode::enterprise});
  service.RecordLaunch({updated_name, updated_timestamp, ibstart::domain::LaunchMode::designer});

  service.RenameDatabaseMetadata(previous_name, updated_name, previous_name, updated_name);

  const auto& state = service.Read();
  CHECK(state.favorites == std::vector<std::wstring>{updated_name});
  CHECK(!state.tags.contains(previous_name));
  const auto updated_tags = state.tags.find(updated_name);
  CHECK(updated_tags != state.tags.end());
  if (updated_tags != state.tags.end()) {
    CHECK(updated_tags->second == std::vector<std::wstring>{L"Общий", L"Новый", L"Старый"});
  }
  CHECK(state.history.size() == 1 && state.history.front().database_id == updated_name &&
      state.history.front().timestamp == previous_timestamp &&
      state.history.front().mode == ibstart::domain::LaunchMode::enterprise);
  CHECK(!state.last_launches.contains(previous_name));
  CHECK(state.last_launches.contains(updated_name));
  CHECK(state.last_launches.at(updated_name) == previous_timestamp);

  const auto persisted = ibstart::storage::LoadCatalogState(layout);
  CHECK(persisted.favorites == state.favorites);
  CHECK(persisted.tags == state.tags);
  CHECK(persisted.history.size() == 1 && persisted.history.front().database_id == updated_name &&
      persisted.history.front().timestamp == previous_timestamp &&
      persisted.history.front().mode == ibstart::domain::LaunchMode::enterprise);
  CHECK(persisted.last_launches == state.last_launches);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCatalogMetadataExplicitIdChangeKeepsHistoryKey() {
  const auto directory = Temp(L"catalog-metadata-explicit-id");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  ibstart::catalog::CatalogMetadataService service(layout);

  const auto timestamp = std::chrono::system_clock::from_time_t(123456789);
  service.RecordLaunch({L"explicit-id-old", timestamp, ibstart::domain::LaunchMode::enterprise});
  service.RenameDatabaseMetadata(L"База", L"База", L"explicit-id-old", L"explicit-id-new");

  const auto& state = service.Read();
  CHECK(state.history.size() == 1);
  CHECK(state.history.front().database_id == L"explicit-id-old");
  CHECK(state.last_launches.contains(L"explicit-id-old"));
  CHECK(!state.last_launches.contains(L"explicit-id-new"));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCatalogMetadataCacheInvalidation() {
  const auto directory = Temp(L"catalog-metadata-cache-invalidation");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  ibstart::catalog::CatalogMetadataService service(layout);
  const auto timestamp = std::chrono::system_clock::from_time_t(123456789);

  service.SetCacheMetric(L"stable-id", {10, timestamp, true});
  service.RenameDatabaseMetadata(L"База", L"Новое имя", L"stable-id", L"stable-id",
      L"File=old", L"File=new");
  CHECK(!service.Read().database_metrics.contains(L"stable-id"));

  service.SetCacheMetric(L"stable-id", {20, timestamp, true});
  service.RenameDatabaseMetadata(L"База", L"Новое имя", L"stable-id", L"stable-id",
      L"File=same", L"File=same");
  CHECK(service.Read().database_metrics.contains(L"stable-id"));

  service.SetCacheMetric(L"new-id", {30, timestamp, true});
  service.SetCacheMetric(L"old-id", {40, timestamp, true});
  service.RenameDatabaseMetadata(L"База", L"База", L"old-id", L"new-id",
      L"Srvr=old", L"Srvr=old");
  CHECK(!service.Read().database_metrics.contains(L"old-id"));
  CHECK(!service.Read().database_metrics.contains(L"new-id"));

  service.SetCacheMetric(L"Старое имя", {50, timestamp, true});
  service.SetCacheMetric(L"Новое имя", {60, timestamp, true});
  service.RenameDatabaseMetadata(L"Старое имя", L"Новое имя", L"Старое имя", L"Новое имя",
      L"Srvr=server;Ref=base", L"Srvr=server;Ref=base");
  CHECK(!service.Read().database_metrics.contains(L"Старое имя"));
  CHECK(!service.Read().database_metrics.contains(L"Новое имя"));

  service.SetCacheMetric(L"remove-id", {70, timestamp, false});
  service.RemoveCacheMetric(L"remove-id");
  CHECK(!service.Read().database_metrics.contains(L"remove-id"));

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestV8iSaveRejectsActiveWriter() {
  const auto directory = Temp(L"store-lock");
  const auto file = directory / L"ibases.v8i";
  const std::string original = "[Base]\r\nConnect=File=\"C:\\\\base\"\r\n";
  WriteBytes(file, original);
  ibstart::v8i::V8iFileStore store(file);
  auto document = store.Read();
  document.Find(L"Base")->entry.Set(L"Locale", L"ru_RU");

  const HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(writer != INVALID_HANDLE_VALUE);
  bool rejected = false;
  if (writer != INVALID_HANDLE_VALUE) {
    try {
      store.Save(document);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    CloseHandle(writer);
  }
  CHECK(rejected);
  CHECK(ReadBytes(file) == original);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestV8iConcurrentSavesConflict() {
  const auto directory = Temp(L"store-concurrent");
  const auto file = directory / L"ibases.v8i";
  WriteBytes(file, "[Base]\r\nConnect=File=\"C:\\\\base\"\r\n");
  ibstart::v8i::V8iFileStore first(file);
  ibstart::v8i::V8iFileStore second(file);
  auto firstDocument = first.Read();
  auto secondDocument = second.Read();
  firstDocument.Find(L"Base")->entry.Set(L"Locale", L"first");
  secondDocument.Find(L"Base")->entry.Set(L"Locale", L"second");

  std::atomic_int ready{0};
  std::atomic_bool start{false};
  std::atomic_int saved{0};
  std::atomic_int conflicts{0};
  std::atomic_int otherErrors{0};
  const auto save = [&](ibstart::v8i::V8iFileStore& store, const ibstart::v8i::V8iDocument& document) {
    ++ready;
    while (!start.load()) std::this_thread::yield();
    try {
      store.Save(document);
      ++saved;
    } catch (const ibstart::v8i::ExternalModificationError&) {
      ++conflicts;
    } catch (...) {
      ++otherErrors;
    }
  };
  std::thread firstThread(save, std::ref(first), std::cref(firstDocument));
  std::thread secondThread(save, std::ref(second), std::cref(secondDocument));
  while (ready.load() != 2) std::this_thread::yield();
  start = true;
  firstThread.join();
  secondThread.join();

  CHECK(saved == 1);
  CHECK(conflicts == 1);
  CHECK(otherErrors == 0);
  const auto persisted = ReadBytes(file);
  CHECK((persisted.find("Locale=first") != std::string::npos) !=
      (persisted.find("Locale=second") != std::string::npos));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestV8iLineEndingRoundTrips() {
  const std::vector<std::pair<std::string_view, std::wstring>> cases = {
      {"lf", L"[Base]\nConnect=x\n"},
      {"lf-no-trailing", L"[Base]\nConnect=x"},
      {"crlf", L"[Base]\r\nConnect=x\r\n"},
      {"crlf-no-trailing", L"[Base]\r\nConnect=x"},
      {"cr", L"[Base]\rConnect=x\r"},
      {"cr-no-trailing", L"[Base]\rConnect=x"},
      {"mixed", L"[Base]\r\nConnect=x\n[Other]\rConnect=y"},
  };
  for (const auto& [name, expected] : cases) {
    const auto document = ibstart::v8i::V8iDocument::ParseUtf8(ibstart::utf::ToUtf8(expected));
    CHECK(document.SerializeUtf8() == ibstart::utf::ToUtf8(expected));
    CHECK(document.sections.size() == 2 || name != "mixed");
  }

  const auto mixed = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Base]\r\nConnect=x\n[Other]\rConnect=y");
  CHECK(mixed.line_endings == std::vector<std::wstring>{L"\r\n", L"\n", L"\r"});
  CHECK(mixed.diagnostics.size() == 1);
  CHECK(!mixed.trailing_newline);
  CHECK(mixed.SerializeUtf8() == "[Base]\r\nConnect=x\n[Other]\rConnect=y");
}

void TestV8iStructuralLineEndingOwnership() {
  constexpr std::string_view source =
      "[First]\r\nA=1\n[Second]\r\nB=2\n[Third]\nC=3\r\n[Fourth]\rD=4";
  constexpr std::string_view after_insert =
      "[First]\r\nA=1\n[Second]\r\nB=2\n[Inserted]\r\nConnect=x\r\n"
      "[Third]\nC=3\r\n[Fourth]\rD=4";
  constexpr std::string_view after_remove =
      "[First]\r\nA=1\n[Third]\nC=3\r\n[Fourth]\rD=4";

  auto inserted = ibstart::v8i::V8iDocument::ParseUtf8(source);
  ibstart::v8i::Section new_section;
  new_section.entry.name = L"Inserted";
  new_section.entry.fields.push_back({L"Connect", L"x"});
  inserted.sections.insert(inserted.sections.begin() + 2, std::move(new_section));
  CHECK(inserted.SerializeUtf8() == after_insert);

  auto removed = ibstart::v8i::V8iDocument::ParseUtf8(source);
  CHECK(removed.Remove(L"Second"));
  CHECK(removed.SerializeUtf8() == after_remove);
}

void TestCacheContinuesAfterCandidateError() {
  const auto directory = Temp(L"cache-continues-after-error");
  const auto local = directory / L"local";
  const auto protectedDirectory = local / L"1C" / L"1Cv8" / L"licenses" / L"nested";
  const auto validCache = local / L"1C" / L"1Cv8" / L"valid-cache";
  std::filesystem::create_directories(protectedDirectory);
  std::filesystem::create_directories(validCache);
  WriteBytes(protectedDirectory / L"license.dat", "keep");
  WriteBytes(validCache / L"cache.dat", "remove");

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  const auto result = ibstart::cache::Clear({{protectedDirectory, 0}, {validCache, 0}});
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(!result.errors.empty());
  CHECK(std::filesystem::exists(protectedDirectory / L"license.dat"));
  CHECK(!std::filesystem::exists(validCache));
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

std::filesystem::path CurrentExecutable() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0 || static_cast<size_t>(length) >= path.size()) throw std::runtime_error("Cannot determine test executable path.");
  path.resize(length);
  return path;
}

void TestCacheContinuesWithActiveOneCProcess() {
  const auto directory = Temp(L"cache-active-process");
  const auto local = directory / L"local";
  const auto blockedCache = local / L"1C" / L"1Cv8" / L"blocked-cache";
  const auto validCache = local / L"1C" / L"1Cv8" / L"valid-cache";
  std::filesystem::create_directories(blockedCache);
  std::filesystem::create_directories(validCache);
  const auto lockedFile = blockedCache / L"locked.dat";
  WriteBytes(lockedFile, "keep");
  WriteBytes(validCache / L"cache.dat", "remove");

  const auto helper = directory / L"1cv8.exe";
  std::filesystem::copy_file(CurrentExecutable(), helper, std::filesystem::copy_options::overwrite_existing);
  std::wstring commandLine = L"\"" + helper.wstring() + L"\" --cache-process-helper";
  std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(helper.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process);
  CHECK(started != FALSE);
  if (!started) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return;
  }

  bool detected = false;
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (ibstart::cache::HasActiveOneCProcess()) {
      detected = true;
      break;
    }
    Sleep(10);
  }
  CHECK(detected);

  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  std::wstring previous(required, L'\0');
  if (required != 0) {
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), required);
    previous.resize(copied);
  }
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  HANDLE lock = CreateFileW(lockedFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(lock != INVALID_HANDLE_VALUE);
  const auto result = ibstart::cache::Clear({{blockedCache, 0}, {validCache, 0}});
  if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
  SetEnvironmentVariableW(L"LOCALAPPDATA", required == 0 ? nullptr : previous.c_str());

  CHECK(result.active_one_c_process);
  CHECK(!result.errors.empty());
  CHECK(std::filesystem::exists(blockedCache));
  CHECK(!std::filesystem::exists(validCache));

  TerminateProcess(process.hProcess, 0);
  WaitForSingleObject(process.hProcess, 5000);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestV8iExternalWriterRaceAtCommitBoundary() {
  const auto directory = Temp(L"store-commit-race");
  const auto file = directory / L"ibases.v8i";
  const std::string original = "[Base]\r\nConnect=File=\"C:\\\\base\"\r\n";
  const std::string external = "[External]\r\nConnect=File=\"C:\\\\external\"\r\n";
  WriteBytes(file, original);
  ibstart::v8i::V8iFileStore store(file);
  auto document = store.Read();
  document.Find(L"Base")->entry.Set(L"Locale", L"first");

  bool caught = false;
  try {
    store.Save(document, [&] {
      std::thread externalWriter([&] { WriteBytes(file, external); });
      externalWriter.join();
    });
  } catch (const ibstart::v8i::ExternalModificationError&) {
    caught = true;
  }

  CHECK(caught);
  CHECK(ReadBytes(file) == external);
  CHECK(store.Backups().size() == 1);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestV8iConcurrentSaveAtCommitBoundary() {
  const auto directory = Temp(L"store-commit-race-cooperating");
  const auto file = directory / L"ibases.v8i";
  WriteBytes(file, "[Base]\r\nConnect=File=\"C:\\\\base\"\r\n");
  ibstart::v8i::V8iFileStore first(file);
  ibstart::v8i::V8iFileStore second(file);
  auto firstDocument = first.Read();
  auto secondDocument = second.Read();
  firstDocument.Find(L"Base")->entry.Set(L"Locale", L"first");
  secondDocument.Find(L"Base")->entry.Set(L"Locale", L"second");

  std::atomic_bool competitorStarted{false};
  std::atomic_int competitorConflicts{0};
  std::atomic_int competitorErrors{0};
  std::thread competitor;
  first.Save(firstDocument, [&] {
    competitor = std::thread([&] {
      competitorStarted = true;
      try {
        second.Save(secondDocument);
      } catch (const ibstart::v8i::ExternalModificationError&) {
        ++competitorConflicts;
      } catch (...) {
        ++competitorErrors;
      }
    });
    while (!competitorStarted.load()) std::this_thread::yield();
  });
  competitor.join();

  CHECK(competitorConflicts == 1);
  CHECK(competitorErrors == 0);
  const auto persisted = ReadBytes(file);
  CHECK(persisted.find("Locale=first") != std::string::npos);
  CHECK(persisted.find("Locale=second") == std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestStorageSkipsMalformedRecords() {
  const auto directory = Temp(L"malformed-storage");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);

  WriteBytes(layout.root / L"settings.json", R"({
    "active_ibases": "C:\\valid.v8i",
    "selected_entry": "Valid database",
    "simple_mode": 1,
    "recent_lists_limit": 0,
    "recent_lists": [{"recent_list": "bad\q"}, {"recent_list": "C:\\recent.v8i"}],
    "platform_paths": [{"platform_path": "C:\\platform"}],
    "unknown_settings": {"recent_list": "C:\\unexpected.v8i", "platform_path": "C:\\unexpected-platform"}
  })");
  const auto settings = ibstart::storage::LoadSettings(layout);
  CHECK(settings.active_ibases == L"C:\\valid.v8i");
  CHECK(settings.selected_entry == L"Valid database");
  CHECK(settings.simple_mode);
  CHECK(settings.recent_lists_limit == 1);
  CHECK(settings.recent_ibases == std::vector<std::filesystem::path>{L"C:\\recent.v8i"});
  CHECK(settings.platform_search_paths == std::vector<std::filesystem::path>{L"C:\\platform"});

  WriteBytes(layout.root / L"settings.json", R"({"simple_mode": 1} trailing data)");
  CHECK(!ibstart::storage::LoadSettings(layout).simple_mode);

  WriteBytes(layout.root / L"catalog-state.json", R"({
    "favorites": [{"favorite": "preserved"}],
    "history": [
      {"history_id": "bad\q", "time": 1, "mode": 0},
      {"mode": 1, "history_id": "valid-history", "time": 2}
    ],
    "last_launches": [{"last_launch_id": "valid-launch", "time": 3}],
    "tags": [
      {"tag_id": "bad-tags", "values": ["bad\q"]},
      {"values": ["First", "Second"], "tag_id": "valid-tags"}
    ],
    "tag_styles": [{"tag_style": "bad\q", "background": 1, "text": 2}, {"text": 4, "tag_style": "valid-style", "background": 3}],
    "unknown_metadata": {
      "favorite": "unexpected-favorite",
      "history_id": "unexpected-history",
      "time": 5,
      "mode": 0,
      "last_launch_id": "unexpected-launch",
      "tag_id": "unexpected-tags",
      "values": ["unexpected"],
      "tag_style": "unexpected-style",
      "background": 1,
      "text": 2
    },
    "sorting": {"default_sort_mode": 2, "folders": [{"folder": "valid-folder", "mode": 2}]}
  })");
  const auto state = ibstart::storage::LoadCatalogState(layout);
  CHECK(state.favorites == std::vector<std::wstring>{L"preserved"});
  CHECK(state.history.size() == 1 && state.history[0].database_id == L"valid-history");
  CHECK(state.last_launches.contains(L"valid-launch"));
  CHECK(!state.last_launches.contains(L"unexpected-launch"));
  CHECK(state.tags.size() == 1 && state.tags.contains(L"valid-tags"));
  CHECK(state.tags.at(L"valid-tags") == std::vector<std::wstring>{L"First", L"Second"});
  CHECK(state.tag_styles.size() == 1 && state.tag_styles.contains(L"valid-style"));
  ibstart::storage::SaveCatalogState(layout, state);
  CHECK(ReadBytes(layout.root / L"catalog-state.json").find("\"sorting\"") == std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCatalogStateNormalization() {
  const auto directory = Temp(L"catalog-state-normalization");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);

  std::string json = R"({
    "favorites": [
      {"favorite": ""},
      {"favorite": 42},
      {"favorite": "Favorite 0"},
      {"favorite": "favorite 0"})";
  for (unsigned index = 1; index != 11; ++index) {
    json += ", {\"favorite\": \"Favorite " + std::to_string(index) + "\"}";
  }
  json += R"(],
    "history": [
      {"history_id": "", "time": 1, "mode": 0},
      {"history_id": "bad-mode", "time": 2, "mode": 3},
      {"history_id": "bad-time", "time": "not-a-number", "mode": 0},
      {"history_id": "duplicate", "time": 100, "mode": 0},
      {"history_id": "DUPLICATE", "time": 200, "mode": 1})";
  for (unsigned index = 0; index != 21; ++index) {
    json += ", {\"history_id\": \"history-" + std::to_string(index) +
        "\", \"time\": " + std::to_string(300 + index) + ", \"mode\": 0}";
  }
  json += "]\n}";
  WriteBytes(layout.root / L"catalog-state.json", json);

  const auto loaded = ibstart::storage::LoadCatalogState(layout);
  CHECK(loaded.favorites.size() == 9);
  CHECK(!loaded.favorites.empty() && loaded.favorites.front() == L"Favorite 0");
  CHECK(!loaded.favorites.empty() && loaded.favorites.back() == L"Favorite 8");
  CHECK(std::find(loaded.favorites.begin(), loaded.favorites.end(), L"Favorite 9") == loaded.favorites.end());
  CHECK(loaded.history.size() == 20);
  CHECK(!loaded.history.empty() && loaded.history.front().database_id == L"DUPLICATE");
  CHECK(!loaded.history.empty() && loaded.history.front().timestamp == std::chrono::system_clock::from_time_t(200));
  CHECK(!loaded.history.empty() && loaded.history.front().mode == ibstart::domain::LaunchMode::designer);
  CHECK(!loaded.history.empty() && loaded.history.back().database_id == L"history-18");
  CHECK(std::none_of(loaded.history.begin(), loaded.history.end(), [](const auto& item) {
    return item.database_id.empty() || static_cast<int>(item.mode) < 0 || static_cast<int>(item.mode) > 2;
  }));

  ibstart::storage::CatalogState dirty;
  dirty.favorites = {L"", L"Saved 0", L"saved 0"};
  for (unsigned index = 1; index != 12; ++index) dirty.favorites.push_back(L"Saved " + std::to_wstring(index));
  dirty.history = {{L"", {}, ibstart::domain::LaunchMode::enterprise},
      {L"bad-mode", {}, static_cast<ibstart::domain::LaunchMode>(3)}};
  for (unsigned index = 0; index != 21; ++index) {
    dirty.history.push_back({L"saved-" + std::to_wstring(index),
        std::chrono::system_clock::from_time_t(400 + index), ibstart::domain::LaunchMode::enterprise});
  }
  dirty.history.push_back({L"SAVED-0", std::chrono::system_clock::from_time_t(999),
      ibstart::domain::LaunchMode::web_client});
  ibstart::storage::SaveCatalogState(layout, dirty);

  const auto persistedJson = ReadBytes(layout.root / L"catalog-state.json");
  const auto count = [&persistedJson](std::string_view token) {
    std::size_t result = 0;
    for (std::size_t position = persistedJson.find(token); position != std::string::npos;
        position = persistedJson.find(token, position + token.size())) {
      ++result;
    }
    return result;
  };
  CHECK(count(R"("favorite":)") == 9);
  CHECK(count(R"("history_id":)") == 20);

  ibstart::storage::CatalogStateRepository repository(layout);
  repository.Update([](ibstart::storage::CatalogState& state) {
    state.favorites.push_back(L"Update 0");
    state.favorites.push_back(L"update 0");
    state.history.push_back({L"Update history", {}, ibstart::domain::LaunchMode::enterprise});
  });
  CHECK(repository.Read().favorites.size() == 9);
  CHECK(repository.Read().history.size() == 20);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestStorageRejectsUnreadableDataPath() {
  const auto directory = Temp(L"unreadable-storage");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::EnsureWritable(layout);
  std::filesystem::create_directory(layout.root / L"settings.json");

  bool rejected = false;
  try {
    static_cast<void>(ibstart::storage::LoadSettings(layout));
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  CHECK(rejected);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

void TestCredentialsScopesAndRoundTrip() {
  ibstart::v8i::V8iDocument document;
  ibstart::v8i::Section parent; parent.entry.name = L"Parent"; parent.entry.Set(L"ID", L"group-id");
  ibstart::v8i::Section child; child.entry.name = L"Child"; child.entry.Set(L"Folder", L"/Parent");
  ibstart::v8i::Section database; database.entry.name = L"Database"; database.entry.Set(L"ID", L"db-id");
  database.entry.Set(L"Folder", L"/Parent/Child"); database.entry.Set(L"Connect", L"Srvr=\"server\";Ref=\"db\"");
  ibstart::v8i::Section no_id; no_id.entry.name = L"Legacy"; no_id.entry.Set(L"Connect", L"Srvr=\"server\";Ref=\"legacy\"");
  document.sections = {parent, child, database, no_id};
  const ibstart::catalog::Catalog catalog(document);
  const auto path = std::filesystem::path(L"C:/Catalog/../Catalog/list.v8i");
  const auto db = catalog.DatabaseFor(L"Database");
  using namespace ibstart::credentials;
  Credential group{L"group", L"Group credential", L"user", L" p a s s \" \\ \n", ScopeMode::selected,
      {MakeTarget(path, catalog.document().sections[0].entry)}};
  Credential direct{L"direct", L"Direct credential", L"user2", L"secret", ScopeMode::selected,
      {MakeTarget(path, catalog.document().sections[2].entry)}};
  Credential legacy{L"legacy", L"Legacy credential", L"old", L"old-pass", ScopeMode::selected,
      {{NormalizeCatalogPath(path), TargetKind::database, {}, L"Legacy"}}};
  Credential invalid{L"invalid", L"Invalid credential", L"bad", L"bad", ScopeMode::invalid, {}};
  const auto applicable = Applicable({invalid, group, direct, legacy}, L"c:/catalog/list.v8i", catalog, db);
  CHECK(applicable.size() == 2);
  CHECK(applicable[0].id == L"direct" && applicable[1].id == L"group");
  CHECK(!Applicable({legacy}, path, catalog, db).size());
  const auto legacy_db = catalog.DatabaseFor(L"Legacy");
  CHECK(Applicable({legacy}, L"C:/CATALOG/list.v8i", catalog, legacy_db).size() == 1);
  CHECK(SameCatalogPath(L"C:/Каталог/./list.v8i", L"c:\\КАТАЛОГ\\list.v8i"));
  CHECK(SameCatalogPath(L"\\\\?\\C:\\Каталог\\list.v8i", L"c:/Каталог/list.v8i"));

  const auto directory = Temp(L"credentials-roundtrip");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::Settings settings; settings.credentials = {group, direct, legacy, invalid};
  ibstart::storage::SaveSettings(layout, settings);
  const auto loaded = ibstart::storage::LoadSettings(layout);
  CHECK(loaded.credentials == settings.credentials);
  CHECK(ReadBytes(layout.root / L"settings.json").find("p a s s \\\" \\\\ \\n") != std::string::npos);
  auto renamed = settings.credentials;
  auto before = catalog.document().sections[0].entry; auto after = before; after.name = L"Renamed";
  RenameTargets(renamed, path, before, after);
  CHECK(renamed[0].targets[0].last_known_name == L"Renamed");
  CHECK(renamed[0].targets[0].entry_id == L"group-id");
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCredentialConcurrentMerges() {
  const auto directory = Temp(L"credentials-merge");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::Settings initial;
  initial.credentials = {{L"one", L"One", L"u", L"p"}, {L"two", L"Two", L"u", L"p"}};
  ibstart::storage::SaveSettings(layout, initial);
  ibstart::storage::SettingsRepository first(layout), second(layout);
  auto first_value = first.Read(); auto second_value = second.Read();
  first_value.credentials[0].password = L"first";
  second_value.credentials[1].password = L"second";
  first.Save(first_value); second.Save(second_value);
  auto merged = ibstart::storage::LoadSettings(layout);
  CHECK(merged.credentials[0].password == L"first" && merged.credentials[1].password == L"second");

  auto conflict_value = first.Read(); auto conflict_other = second.Read();
  conflict_value.credentials[0].password = L"conflict-a";
  conflict_other.credentials[0].password = L"conflict-b";
  first.Save(conflict_value);
  bool conflict = false;
  try { second.Save(conflict_other); } catch (const ibstart::storage::StorageConflictError&) { conflict = true; }
  CHECK(conflict);

  auto delete_value = first.Read(); auto edit_value = second.Read();
  delete_value.credentials.erase(delete_value.credentials.begin() + 1);
  edit_value.credentials[1].title = L"Edited";
  first.Save(delete_value);
  conflict = false;
  try { second.Save(edit_value); } catch (const ibstart::storage::StorageConflictError&) { conflict = true; }
  CHECK(conflict);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCredentialApplicabilityBoundaries() {
  ibstart::v8i::V8iDocument document;
  ibstart::v8i::Section first; first.entry.name = L"First"; first.entry.Set(L"ID", L"g1");
  ibstart::v8i::Section second; second.entry.name = L"Second"; second.entry.Set(L"ID", L"g2"); second.entry.Set(L"Folder", L"/First");
  ibstart::v8i::Section database; database.entry.name = L"DB"; database.entry.Set(L"ID", L"db"); database.entry.Set(L"Folder", L"/First/Second"); database.entry.Set(L"Connect", L"Srvr=\"s\";Ref=\"d\"");
  document.sections = {first, second, database};
  const ibstart::catalog::Catalog catalog(document);
  const auto db = catalog.DatabaseFor(L"DB");
  const auto path = std::filesystem::path(L"C:/lists/main.v8i");
  using namespace ibstart::credentials;
  const Credential group{L"group", L"Group", L"u", L"p", ScopeMode::selected, {MakeTarget(path, document.sections[0].entry)}};
  const Credential direct{L"direct", L"Direct", L"u", L"p", ScopeMode::selected, {MakeTarget(path, document.sections[2].entry)}};
  const Credential missing{L"missing", L"Missing", L"u", L"p", ScopeMode::selected, {{path, TargetKind::database, L"other", L"DB"}}};
  const Credential global{L"global", L"Global", L"u", L"p", ScopeMode::all, {}};
  const auto result = Applicable({missing, group, direct, global}, path, catalog, db);
  CHECK(result.size() == 3);
  if (result.size() == 3) CHECK(result[0].id == L"direct" && result[1].id == L"group" && result[2].id == L"global");

  auto moved = catalog;
  CHECK(moved.Move(L"DB", L"", 0));
  const auto moved_db = moved.DatabaseFor(L"DB");
  CHECK(Applicable({group}, path, moved, moved_db).empty());
  CHECK(moved.Move(L"DB", L"First", 0));
  CHECK(!Applicable({group}, path, moved, moved.DatabaseFor(L"DB")).empty());

  auto renamed = catalog;
  CHECK(renamed.RenameDatabase(L"DB", L"Renamed"));
  CHECK(!Applicable({direct}, path, renamed, renamed.DatabaseFor(L"Renamed")).empty());
  CHECK(Applicable({missing}, path, catalog, db).empty());

  ibstart::v8i::Section duplicate = first; duplicate.entry.name = L"Other"; duplicate.entry.Set(L"Folder", L"/");
  ibstart::v8i::V8iDocument ambiguous_doc = document; ambiguous_doc.sections.push_back(duplicate);
  const ibstart::catalog::Catalog ambiguous(ambiguous_doc);
  CHECK(Applicable({group}, path, ambiguous, ambiguous.DatabaseFor(L"DB")).empty());
}

void TestCredentialSettingsRejectInvalidScopeAndDuplicateIds() {
  const auto directory = Temp(L"credentials-invalid-settings");
  const ibstart::storage::StorageLayout layout{directory, true};
  WriteBytes(layout.root / L"settings.json", R"({"credentials_schema_version":1,"credentials":[
    {"id":"", "title":"Blank", "user_name":"u", "password":"p", "scope":{"mode":"all"}},
    {"id":"same", "title":"One", "user_name":"u", "password":"p", "scope":{"mode":"unknown"}},
    {"id":"SAME", "title":"Two", "user_name":"u", "password":"пароль", "scope":{"mode":"all"}}
  ]})");
  const auto loaded = ibstart::storage::LoadSettings(layout);
  CHECK(loaded.credentials.size() == 3);
  CHECK(std::all_of(loaded.credentials.begin(), loaded.credentials.end(), [](const auto& c) {
    return c.scope == ibstart::credentials::ScopeMode::invalid;
  }));
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCredentialConcurrentDeletionAndAdditionConflicts() {
  const auto directory = Temp(L"credentials-concurrent-edits");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::Settings initial;
  initial.credentials = {{L"delete", L"Delete", L"u", L"p"}};
  ibstart::storage::SaveSettings(layout, initial);
  ibstart::storage::SettingsRepository first(layout), second(layout);
  auto a = first.Read(); auto b = second.Read();
  a.credentials.clear(); b.credentials.clear();
  first.Save(a);
  bool deletion_conflict = false;
  try { second.Save(b); } catch (const ibstart::storage::StorageConflictError&) { deletion_conflict = true; }
  CHECK(!deletion_conflict);

  initial.credentials.clear(); ibstart::storage::SaveSettings(layout, initial);
  ibstart::storage::SettingsRepository left(layout), right(layout);
  auto left_value = left.Read(); auto right_value = right.Read();
  left_value.credentials.push_back({L"left", L"Same title", L"u", L"p"});
  right_value.credentials.push_back({L"right", L"Same title", L"u", L"p"});
  left.Save(left_value);
  bool addition_conflict = false;
  try { right.Save(right_value); } catch (const ibstart::storage::StorageConflictError&) { addition_conflict = true; }
  CHECK(addition_conflict);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCredentialStaleSettingsPreservePasswordAndFutureSchemaInactive() {
  const auto directory = Temp(L"credentials-stale-settings");
  const ibstart::storage::StorageLayout layout{directory, true};
  ibstart::storage::Settings initial;
  initial.credentials = {{L"good", L"Good", L"u", L"old"}, {L"bad", L"Bad", L"u", L"p", ibstart::credentials::ScopeMode::invalid}};
  ibstart::storage::SaveSettings(layout, initial);
  ibstart::storage::SettingsRepository latest(layout), stale(layout);
  auto latest_value = latest.Read(); auto stale_value = stale.Read();
  latest_value.credentials[0].password = L"latest пароль";
  latest.Save(latest_value);
  stale_value.window_width += 1;
  stale.Save(stale_value);
  const auto merged = ibstart::storage::LoadSettings(layout);
  CHECK(merged.credentials.size() == 2);
  if (merged.credentials.size() == 2) {
    CHECK(merged.credentials[0].password == L"latest пароль");
    CHECK(merged.credentials[1].scope == ibstart::credentials::ScopeMode::invalid);
  }
  WriteBytes(layout.root / L"settings.json", R"({"credentials_schema_version":99,"credentials":[{"id":"future","title":"Future","user_name":"u","password":"пароль","scope":{"mode":"all"}}]})");
  const auto future = ibstart::storage::LoadSettings(layout);
  CHECK(future.credentials.size() == 1);
  if (future.credentials.size() == 1) CHECK(future.credentials[0].scope == ibstart::credentials::ScopeMode::invalid);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCredentialGroupIncludesNewNestedDatabase() {
  ibstart::v8i::V8iDocument document;
  ibstart::v8i::Section group; group.entry.name = L"Group"; group.entry.Set(L"ID", L"group");
  ibstart::v8i::Section first; first.entry.name = L"First"; first.entry.Set(L"ID", L"first"); first.entry.Set(L"Folder", L"/Group"); first.entry.Set(L"Connect", L"Srvr=\"s\";Ref=\"first\"");
  document.sections = {group, first};
  const auto path = std::filesystem::path(L"C:/lists/nested.v8i");
  const ibstart::credentials::Credential credential{L"group", L"Group", L"u", L"p", ibstart::credentials::ScopeMode::selected,
      {ibstart::credentials::MakeTarget(path, document.sections[0].entry)}};
  ibstart::catalog::Catalog catalog(document);
  CHECK(!ibstart::credentials::Applicable({credential}, path, catalog, catalog.DatabaseFor(L"First")).empty());
  CHECK(catalog.AddServerDatabase(L"Second", L"Srvr=\"s\";Ref=\"second\"", L"Group"));
  CHECK(!ibstart::credentials::Applicable({credential}, path, catalog, catalog.DatabaseFor(L"Second")).empty());
}
}

namespace {
const ibstart::launcher::ParameterDefinition& Parameter(std::wstring_view key) {
  const auto catalog = ibstart::launcher::LaunchParameterCatalog();
  const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& def) { return def.key == key; });
  if (found == catalog.end()) throw std::runtime_error("Missing test parameter");
  return *found;
}

void TestParameterBuilderPreservesText() {
  using namespace ibstart::launcher;
  const std::wstring original = L"  /Future=alpha\t /C\"старое значение\"  /Out \"C:\\Logs\\a b.txt\" -NoTruncate \r\n";
  const auto updated = SetLaunchParameter(original, Parameter(L"/C"), L"новое \"значение\" \\");
  const auto fragments = FindParameterFragments(updated, Parameter(L"/C"));
  CHECK(fragments.size() == 1);
  CHECK(fragments.front().value == L"новое \"значение\" \\");
  CHECK(updated.starts_with(L"  /Future=alpha\t "));
  CHECK(updated.ends_with(L"  /Out \"C:\\Logs\\a b.txt\" -NoTruncate \r\n"));
  CHECK(SplitCommandArguments(updated)[2] == L"новое \"значение\" \\");
  const auto out = FindParameterFragments(original, Parameter(L"/Out"));
  CHECK(out.size() == 1 && out.front().structured && out.front().option);
  CHECK(out.front().value == L"C:\\Logs\\a b.txt");
  CHECK(SetLaunchParameter(L"/Future x  ", Parameter(L"/L"), L"ru") == L"/Future x  /L ru");
  CHECK(SetLaunchParameter(L" /WA- ", Parameter(L"/WA"), L"+") == L" /WA+ ");
  CHECK(SetLaunchParameter(L"/C old", Parameter(L"/C"), L"") == L"/C \"\"");
  CHECK(ValidateParameterDraft(L"/C \"\"").empty());
  CHECK(!ValidateParameterDraft(L"/C").empty());
  CHECK(ValidateParameterDraft(L"/C \"/Proxy\" /NoProxy").empty());
  CHECK(SetLaunchParameter(L"", Parameter(L"/C"), L"/AppArch") == L"/C \"/AppArch\"");
  CHECK(!AppArchitectureFromParameters(L"/C \"/AppArch\""));
  for (const auto text : {L"/N \"/AppArch\"", L"/P \"/AppArch=x86\"",
      L"/Out \"/AppArch\"", L"/ConfigurationRepositoryP \"/AppArch=x86\""}) {
    CHECK(!AppArchitectureFromParameters(text));
    CHECK(AppArchitectureFromParameters(std::wstring(text) + L" /AppArch x86_64") ==
        ibstart::domain::ClientArchitecture::x64);
  }
  const auto positioned = TokenizeCommandArguments(L"  /C \"a b\"\t/F x");
  CHECK(positioned.size() == 4);
  CHECK(positioned[1].begin == 5 && positioned[1].end == 10 && positioned[1].quoted);
  CHECK(positioned[1].value == L"a b");
  CHECK(FindParameterFragments(L"/C \"/L\"", Parameter(L"/L")).empty());
  CHECK(ValidateParameterDraft(L"/Future -Nested \"unknown\\value\"").empty());
  for (const auto text : {L"/L ru /l en", L"/L ru -FutureOption", L"/L \"broken"}) {
    bool rejected = false;
    try { static_cast<void>(SetLaunchParameter(text, Parameter(L"/L"), L"de")); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
  CHECK(!ValidateParameterDraft(L"/L ru /l en").empty());
  CHECK(!ValidateParameterDraft(L"/Theme -Blue").empty());
  CHECK(!ValidateParameterDraft(L"/Proxy -PSrv proxy -PPort 3128 /NoProxy").empty());
  CHECK(!ValidateParameterDraft(L"/HttpsForceTLS1_1 /HttpsForceTLS1_2").empty());
  const auto secret_errors = ValidateParameterDraft(L"/P \"synthetic-private-value");
  CHECK(!secret_errors.empty());
  CHECK(secret_errors.front().find(L"synthetic-private-value") == std::wstring::npos);
}

void TestParameterVersionProfiles() {
  using namespace ibstart::launcher;
  const auto versions = LaunchParameterVersions();
  CHECK(versions.size() == 24);
  CHECK(versions.front() == L"8.3.6" && versions.back() == L"8.5.4");
  CHECK(std::find(versions.begin(), versions.end(), L"8.5.2") == versions.end());
  CHECK(std::find(versions.begin(), versions.end(), L"8.5.3") == versions.end());
  CHECK(ParameterVersionFamily(L"8.3.26.1540") == L"8.3.26");
  CHECK(ParameterVersionFamily(L"8.3").empty());
  CHECK(ParameterVersionFamily(L"8.3.26unknown").empty());
  CHECK(ParameterDocumentationUrl(L"8.5.4") == L"https://its.1c.ru/db/v854doc/bookmark/adm/TI000000493");
  CHECK(ParameterVersionAvailability(L"/Theme", L"8.3.27") == ParameterAvailability::unavailable);
  CHECK(ParameterVersionAvailability(L"/theme", L"8.5.1.100") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/Theme", L"8.5.4") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/Theme", L"8.5.3") == ParameterAvailability::unverified);
  CHECK(ParameterVersionAvailability(L"/ResetSavedAuth", L"8.3.25") == ParameterAvailability::unavailable);
  CHECK(ParameterVersionAvailability(L"/ResetSavedAuth", L"8.3.26") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/MainWindowMode", L"8.3.15") == ParameterAvailability::unavailable);
  CHECK(ParameterVersionAvailability(L"/MainWindowMode", L"8.3.16") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/DisableLocalSpeechToText", L"8.3.26") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/DisableLocalSpeechToText", L"8.3.27") == ParameterAvailability::unavailable);
  CHECK(ParameterChoicesForVersion(Parameter(L"/Theme"), L"8.5.1") == L"-Dark|-Light");
  CHECK(ParameterChoicesForVersion(Parameter(L"/TComp"), L"8.3.12") == L"-None|-Deflate|-SDC");
  CHECK(ParameterChoicesForVersion(Parameter(L"/TComp"), L"8.3.26").find(L"-lz4") != std::wstring::npos);
  const ParameterEditorContext old_context{ibstart::domain::LaunchMode::enterprise,
      ibstart::domain::ClientType::thick, L"8.3.26"};
  CHECK(!ValidateParameterDraft(L"/Theme -Dark", old_context).empty());
  CHECK(ValidateParameterDraft(L"/L ru /C \"a b\"", old_context).empty());
  for (const auto& version : versions) {
    CHECK(ParameterVersionAvailability(L"/C", version) == ParameterAvailability::available);
    CHECK(ParameterVersionAvailability(L"/Out", version) == ParameterAvailability::available);
  }
  const auto catalog = LaunchParameterCatalog();
  for (size_t i = 0; i < catalog.size(); ++i) {
    CHECK(!catalog[i].key.empty() && !catalog[i].description.empty() && !catalog[i].syntax.empty());
    CHECK(std::none_of(catalog.begin() + static_cast<ptrdiff_t>(i + 1), catalog.end(),
        [&](const auto& def) { return def.key == catalog[i].key; }));
    if (catalog[i].kind != ParameterValueKind::reference)
      for (const auto& version : versions)
        CHECK(ParameterVersionAvailability(catalog[i].key, version) != ParameterAvailability::unverified);
  }
}

void TestParameterEditingActions() {
  using namespace ibstart::launcher;
  const ParameterEditorContext additional{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  auto standalone = additional;
  standalone.allow_connection_parameters = true;
  const std::pair<std::wstring_view, std::wstring_view> connections[] = {
      {L"/F", L"C:\\Synthetic base"}, {L"/S", L"synthetic-server\\Synthetic base"},
      {L"/WS", L"https://example.com/synthetic"}, {L"/IBName", L"Synthetic base"},
      {L"/IBConnectionString", L"File=\"C:\\Synthetic base\";Usr=\"synthetic user\";"}};
  const std::wstring prefix = L" /Future one\t";
  for (const auto& [key, value] : connections) {
    const auto& definition = Parameter(key);
    CHECK(definition.kind == ParameterValueKind::text);
    CHECK(ParameterContextNotice(definition, standalone).empty());
    CHECK(!ParameterContextNotice(definition, additional).empty());
    const auto draft = SetLaunchParameter(prefix, definition, value);
    const auto fragments = FindParameterFragments(draft, definition);
    CHECK(fragments.size() == 1 && fragments.front().structured && fragments.front().value == value);
    CHECK(ValidateParameterDraft(draft, standalone).empty());
    CHECK(!ValidateParameterDraft(draft, additional).empty());
    CHECK(RemoveLaunchParameter(draft, definition) == prefix);
    CHECK(!ValidateParameterDraft(std::wstring(key), standalone).empty());
  }
  const auto& flag = Parameter(L"/DisableStartupMessages");
  const std::wstring flagged = L" \t/C \"/DisableStartupMessages\"  /DisableStartupMessages\t/Future value  ";
  CHECK(RemoveLaunchParameter(flagged, flag) == L" \t/C \"/DisableStartupMessages\"  \t/Future value  ");
  CHECK(RemoveLaunchParameter(L"/DisableStartupMessages", flag).empty());
  CHECK(RemoveLaunchParameter(L" /DisableStartupMessages \t", flag) == L"  \t");
  CHECK(RemoveLaunchParameter(L"/C \"/DisableStartupMessages\"", flag) == L"/C \"/DisableStartupMessages\"");
  CHECK(RemoveLaunchParameter(L" /L ru\t/C \"synthetic value\"  ", Parameter(L"/L")) == L" \t/C \"synthetic value\"  ");
  CHECK(RemoveLaunchParameter(L" /Out \"C:\\Synthetic log.txt\" -NoTruncate\t/C synthetic", Parameter(L"/Out")) == L" \t/C synthetic");
  CHECK(RemoveLaunchParameter(L"/F", Parameter(L"/F")).empty());
  for (const auto draft : {L"/DisableStartupMessages /disablestartupmessages", L"/DisableStartupMessages -FutureOption one",
      L"/DisableStartupMessages /C \"unclosed"}) {
    bool rejected = false;
    try { static_cast<void>(RemoveLaunchParameter(draft, flag)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
}

void TestParameterHelpAndDefaults() {
  using namespace ibstart::launcher;
  const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  const auto detail = ParameterHelpText(Parameter(L"/TComp"), context);
  CHECK(detail.find(L"Zstandard") != std::wstring::npos);
  CHECK(detail.find(L"По умолчанию: -zstd") != std::wstring::npos);
  const auto old_detail = BuiltinParameterHelp(Parameter(L"/TComp"), L"8.3.21.100");
  CHECK(old_detail.find(L"-lz4") == std::wstring::npos);
  CHECK(old_detail.find(L"-zstd") == std::wstring::npos);
  CHECK(old_detail.find(L"По умолчанию: -SDC") != std::wstring::npos);
  CHECK(BuiltinParameterHelp(Parameter(L"/TComp"), L"8.3.22").find(L"По умолчанию: -zstd") != std::wstring::npos);
  CHECK(BuiltinParameterHelp(Parameter(L"/TComp"), L"8.5.3").find(L"По умолчанию") == std::wstring::npos);
  CHECK(detail.find(L"Это не проверка") == std::wstring::npos);
  CHECK(detail.find(L"Актуальная стабильная") == std::wstring::npos);
  CHECK(ParameterHelpText(Parameter(L"/Out"), context).find(L"-NoTruncate") != std::wstring::npos);
  CHECK(ParameterHelpText(Parameter(L"/LoadCfg"), context).find(L"-Extension") != std::wstring::npos);
  for (const auto& definition : LaunchParameterCatalog()) {
    CHECK(!ParameterExplanation(definition.key).empty());
    CHECK(ParameterExplanation(definition.key) != definition.description);
    for (const auto& version : LaunchParameterVersions()) {
      const auto text = BuiltinParameterHelp(definition, version);
      const auto options = ParameterNestedOptions(definition.key, version);
      if (definition.key == L"/AppAutoCheckVersion" && (version == L"8.3.6" || version == L"8.3.7"))
        CHECK(text.find(L"описана без знака режима") != std::wstring::npos);
      else CHECK(text.find(ParameterExplanation(definition.key)) != std::wstring::npos);
      if (!options.empty() && definition.choices.empty()) for (const auto& option : options) {
        CHECK(!NestedParameterExplanation(definition.key, option.key, version).empty());
        CHECK(text.find(L"• " + std::wstring(option.syntax) + L"\r\n") != std::wstring::npos);
        for (const auto value : NestedParameterValues(definition.key, option.key, version))
          CHECK(!NestedValueExplanation(value).empty());
      }
    }
  }
  CHECK(ParameterFirstDocumentedVersion(L"/theme") == L"8.5.1");
  CHECK(ParameterFirstDocumentedVersion(L"/MainWindowMode") == L"8.3.16");
  CHECK(ParameterFirstDocumentedVersion(L"/Future").empty());
  CHECK(ParameterDocumentedVersions(L"/Theme") == L"8.5.1, 8.5.4");
  CHECK(ParameterHelpText(Parameter(L"/Theme"), {{}, ibstart::domain::ClientType::automatic, L"8.3.26"})
      .find(L"Впервые описана в доступных справочниках: 8.5.1") != std::wstring::npos);
  const std::vector<ibstart::domain::PlatformInstallation> installed{
      {L"C:\\synthetic-old.exe", L"8.3.9.100"},
      {L"C:\\synthetic-new.exe", L"8.3.27.200"},
      {L"C:\\synthetic-stable.exe", L"8.5.1.100"}};
  CHECK(DefaultParameterReferenceVersion(L"", installed) == L"8.5.1.100");
  CHECK(DefaultParameterReferenceVersion(L"Авто", installed) == L"8.5.1.100");
  CHECK(DefaultParameterReferenceVersion(L"8.3", installed) == L"8.3.27.200");
  CHECK(DefaultParameterReferenceVersion(L"8.3.9", installed) == L"8.3.9");
  CHECK(DefaultParameterReferenceVersion(L"8.5.3", installed) == L"8.5.3");
  CHECK(DefaultParameterReferenceVersion(L"", {}) == L"8.5.1");
  CHECK(DefaultParameterReferenceVersion(L"8.3", {}) == L"8.3");
}

void TestDocumentedParameterSemantics() {
  using namespace ibstart::launcher;
  CHECK(LaunchParameterCatalog().size() == 166);
  const auto help = [](std::wstring_view command, std::wstring_view version = L"8.5.1") {
    return BuiltinParameterHelp(Parameter(command), version);
  };
  const auto delay = help(L"/EmulateServerCallDelay");
  CHECK(delay.find(L"в секундах") != std::wstring::npos);
  CHECK(delay.find(L"1 Кбайт") != std::wstring::npos);
  CHECK(delay.find(L"1.45") != std::wstring::npos);
  CHECK(delay.find(L"0.45") != std::wstring::npos);
  CHECK(delay.find(L"0.15") != std::wstring::npos);
  CHECK(delay.find(L"9.99") != std::wstring::npos);
  CHECK(NestedParameterExplanation(L"/HttpsCert", L"-auto", L"8.5.1").find(L"-choose") != std::wstring::npos);
  CHECK(NestedParameterExplanation(L"/UILogRecorder", L"-TPort", L"8.5.1").find(L"1538") != std::wstring::npos);
  CHECK(NestedParameterExplanation(L"/UILogRecorder", L"-TPort", L"8.3.6").find(L"мобильном клиенте") == std::wstring::npos);
  CHECK(NestedParameterExplanation(L"/ConfigurationRepositoryUnLock", L"-force", L"8.5.1") !=
      NestedParameterExplanation(L"/ConfigurationRepositoryCommit", L"-force", L"8.5.1"));
  CHECK(help(L"/MobileAppWriteFile", L"8.3.12").find(L"XML-файл") != std::wstring::npos);
  CHECK(help(L"/MobileAppWriteFile", L"8.3.14").find(L"сопутствующими материалами") != std::wstring::npos);
  CHECK(help(L"/MobileAppWriteFile").find(L"создании базы на мобильном устройстве") != std::wstring::npos);
  CHECK(help(L"/MobileClientWriteFile", L"8.3.14").find(L"внешних компонент") != std::wstring::npos);
  CHECK(help(L"/MobileClientWriteFile").find(L"внешних компонент") == std::wstring::npos);
  CHECK(ContextParameterExplanation(L"/MobileAppWriteFile", L"", L"8.5.3").empty());
  CHECK(help(L"Web: query").find(L"MainWindowMode=Kiosk") != std::wstring::npos);
  CHECK(help(L"Web: query").find(L"OidcSelectedProvider") != std::wstring::npos);
  CHECK(help(L"Web: query", L"8.3.6").find(L"OidcSelectedProvider") == std::wstring::npos);
  CHECK(help(L"Mobile: launch", L"8.3.6").find(L"нет отдельного перечня") != std::wstring::npos);
  CHECK(help(L"Mobile: launch").find(L"нет отдельного перечня") == std::wstring::npos);
  CHECK(ParameterFirstDocumentedVersion(L"/DisableUnrecoverableErrorMessage") == L"8.3.17");
  CHECK(ParameterFirstDocumentedVersion(L"/DisableHomePageForms") == L"8.3.20");
  CHECK(ParameterFirstDocumentedVersion(L"/AddToList") == L"8.3.11");
  CHECK(ParameterVersionAvailability(L"/AddInList", L"8.3.10") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/AddInList", L"8.3.11") == ParameterAvailability::unavailable);
  CHECK(DocumentedParameterSyntax(L"/UseTemplate", L"8.5.1").find(L"[<имя файла шаблона>]") != std::wstring::npos);
  const ParameterEditorContext designer{ibstart::domain::LaunchMode::designer, ibstart::domain::ClientType::automatic, L"8.5.1"};
  CHECK(!NestedParameterRequired(L"/CreateTemplateListFile", L"", designer.version, L"/CreateTemplateListFile"));
  CHECK(ValidateParameterDraft(L"/CreateTemplateListFile", designer).empty());
  CHECK(ValidateParameterDraft(L"/CreateDistributivePackage dir -File description.xml -MakeSetup", designer).empty());
  CHECK(help(L"/CreateDistributivePackage").find(L"updsetup.zip") != std::wstring::npos);
  const auto& speech = Parameter(L"/DisableLocalSpeechToText");
  CHECK(ParameterChoicesForVersion(speech, L"8.3.23").empty());
  CHECK(ParameterChoicesForVersion(speech, L"8.3.24") == L"+|-");
  CHECK(SetLaunchParameter(L"", speech, L"") == L"/DisableLocalSpeechToText");
  CHECK(SetLaunchParameter(L"", speech, L"-") == L"/DisableLocalSpeechToText-");
  CHECK(ValidateParameterDraft(L"/DisableLocalSpeechToText", {{}, ibstart::domain::ClientType::automatic, L"8.3.23"}).empty());
  CHECK(!ValidateParameterDraft(L"/DisableLocalSpeechToText-", {{}, ibstart::domain::ClientType::automatic, L"8.3.23"}).empty());
  CHECK(ValidateParameterDraft(L"/DisableLocalSpeechToText-", {{}, ibstart::domain::ClientType::automatic, L"8.3.24"}).empty());
}

void TestRepositoryConnectionParameters() {
  using namespace ibstart::launcher;
  const ParameterEditorContext designer{ibstart::domain::LaunchMode::designer,
      ibstart::domain::ClientType::thick, L"8.5.1"};
  const ParameterEditorContext enterprise{ibstart::domain::LaunchMode::enterprise,
      ibstart::domain::ClientType::thick, L"8.5.1"};
  std::wstring draft = L"/L ru";
  const std::wstring address = L"C:\\synthetic repositories\\Main";
  for (const auto key : {L"/ConfigurationRepositoryF", L"/ConfigurationRepositoryN", L"/ConfigurationRepositoryP"}) {
    CHECK(Parameter(key).kind == ParameterValueKind::text);
    CHECK(ParameterContextNotice(Parameter(key), designer).empty());
    CHECK(!ParameterContextNotice(Parameter(key), enterprise).empty());
  }
  draft = SetLaunchParameter(draft, Parameter(L"/ConfigurationRepositoryF"), address);
  draft = SetLaunchParameter(draft, Parameter(L"/ConfigurationRepositoryN"), L"synthetic user");
  draft = SetLaunchParameter(draft, Parameter(L"/ConfigurationRepositoryP"), L"synthetic repo password");
  CHECK(ValidateParameterDraft(draft, designer).empty());
  CHECK(!ValidateParameterDraft(draft, enterprise).empty());
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryF", designer).empty());
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryF one /configurationrepositoryf two", designer).empty());
  CHECK(FindParameterFragments(draft, Parameter(L"/ConfigurationRepositoryF")).front().value == address);
  CHECK(ParameterHelpText(Parameter(L"/ConfigurationRepositoryF"), designer).find(L"нет отдельных полей") == std::wstring::npos);
  const auto empty_password = SetLaunchParameter(draft, Parameter(L"/ConfigurationRepositoryP"), L"");
  CHECK(ValidateParameterDraft(empty_password, designer).empty());
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryP", designer).empty());
  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\synthetic-base\"";
  database.additional_parameters = draft;
  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::designer;
  const ibstart::domain::PlatformInstallation platform{L"C:\\synthetic-platform\\1cv8.exe", L"8.5.1.100"};
  const auto command = BuildCommand(database, platform, options);
  CHECK(command.arguments.front() == L"DESIGNER");
  const auto position = std::find(command.arguments.begin(), command.arguments.end(), L"/ConfigurationRepositoryF");
  CHECK(position != command.arguments.end());
  CHECK(position != command.arguments.end() && position + 1 != command.arguments.end() && *(position + 1) == address);
  CHECK(ibstart::logging::ContainsSecretArguments(command));
  CHECK(ibstart::logging::RedactedCommandLine(command).find(L"synthetic repo password") == std::wstring::npos);
  for (const auto text : {L"/ConfigurationRepositoryP \"synthetic repo password\"", L"/configurationrepositoryp=synthetic-secret"}) {
    const auto masked = ibstart::logging::MaskSecrets(text);
    CHECK(masked.find(L"synthetic") == std::wstring::npos);
    CHECK(masked.find(L"***") != std::wstring::npos);
  }
}

void TestNestedOptionOrderAndRequirements() {
  using namespace ibstart::launcher;
  const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  const auto& proxy = Parameter(L"/Proxy");
  const auto options = ParameterNestedOptions(proxy.key, context.version);
  CHECK(ValidateParameterDraft(L"/Proxy", context).front().find(L"-PSrv") != std::wstring::npos);
  CHECK(options.size() == 4);
  CHECK(options[0].key == L"-PSrv" && options[1].key == L"-PPort" &&
      options[2].key == L"-PUser" && options[3].key == L"-PPwd");
  std::wstring text;
  text = SetNestedParameter(text, proxy, options[3], L"synthetic-password");
  CHECK(!ValidateParameterDraft(text, context).empty());
  CHECK(NestedParameterRequired(proxy.key, L"-PUser", context.version, text));
  text = SetNestedParameter(text, proxy, options[1], L"30");
  text = SetNestedParameter(text, proxy, options[2], L"synthetic-user");
  text = SetNestedParameter(text, proxy, options[0], L"http://proxy");
  CHECK(text == L"/Proxy -PSrv http://proxy -PPort 30 -PUser synthetic-user -PPwd synthetic-password");
  CHECK(ValidateParameterDraft(text, context).empty());
  CHECK(!NestedParameterRequired(proxy.key, L"-PUser", context.version, L""));
  CHECK(NestedParameterRequired(proxy.key, L"-PSrv", context.version, L""));
  const auto repaired = SetNestedParameter(
      L" /Proxy -PPort 30 -PSrv http://proxy -PPwd \"synthetic pass\" -PUser user  /L ru ",
      proxy, options[1], L"31");
  CHECK(repaired == L" /Proxy -PSrv http://proxy -PPort 31 -PUser user -PPwd \"synthetic pass\"  /L ru ");
  const auto unknown = SetNestedParameter(L"/Proxy -PPwd \"synthetic pass\" -Future \"keep this\" -PUser user -PPort 30 -PSrv http://proxy",
      proxy, options[1], L"31");
  CHECK(unknown.find(L"-Future \"keep this\"") != std::wstring::npos);
  CHECK(ReadNestedParameter(unknown, proxy, options[3]) == std::optional<std::wstring>(L"synthetic pass"));
  const auto help = ParameterHelpText(proxy, context);
  CHECK(help.find(L"Обязательный параметр.") != std::wstring::npos);
  CHECK(help.find(L"Параметр требуется при использовании группы: -PPwd или -PUser.") != std::wstring::npos);
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext profile{{}, ibstart::domain::ClientType::automatic, version};
    CHECK(!ValidateParameterDraft(L"/Proxy", profile).empty());
    CHECK(!ValidateParameterDraft(L"/Proxy -PSrv http://proxy", profile).empty());
    CHECK(!ValidateParameterDraft(L"/Proxy -PPort 30", profile).empty());
    CHECK(ValidateParameterDraft(L"/Proxy -PSrv http://proxy -PPort 30", profile).empty());
    CHECK(!ValidateParameterDraft(L"/Proxy -PSrv http://proxy -PPort 30 -PPwd synthetic", profile).empty());
    CHECK(ValidateParameterDraft(L"/Proxy -PSrv http://proxy -PPort 30 -PUser user -PPwd \"\"", profile).empty());
    for (const auto& def : LaunchParameterCatalog()) {
      if (def.kind != ParameterValueKind::reference) continue;
      auto nested = ParameterNestedOptions(def.key, version);
      if (nested.empty()) continue;
      // These two selectors have documented first-wins semantics and require
      // manual editing if used together. Test a non-ambiguous draft instead.
      if (def.key == L"/LoadConfigFromFiles")
        std::erase_if(nested, [](const auto& option) { return option.key == L"-files"; });
      if (def.key == L"/CreateDistributionFiles")
        std::erase_if(nested, [](const auto& option) { return option.key == L"-v"; });
      std::vector<std::wstring> spellings;
      for (const auto& option : nested) {
        std::wstring spelling(option.key);
        if (option.style != OptionStyle::flag) {
          const auto choices = NestedParameterChoices(def.key, option.key, version);
          const auto value = choices.empty() ? L"synthetic value" : choices.substr(0, choices.find(L'|'));
          spelling += option.style == OptionStyle::space ? L" " : option.style == OptionStyle::equals ? L"=" : option.style == OptionStyle::colon ? L":" : L"";
          spelling += QuoteWindowsArgument(value);
        }
        spellings.push_back(std::move(spelling));
      }
      std::wstring reversed(def.key);
      for (auto it = spellings.rbegin(); it != spellings.rend(); ++it) reversed += L" " + *it;
      const auto& first = nested.front();
      const auto choices = NestedParameterChoices(def.key, first.key, version);
      const auto value = first.style == OptionStyle::flag ? L"" : choices.empty() ? L"synthetic value" : choices.substr(0, choices.find(L'|'));
      std::wstring ordered;
      try { ordered = SetNestedParameter(reversed, def, first, value); }
      catch (const std::invalid_argument&) {
        std::wcerr << L"Option ordering fixture: " << def.key << L" " << version << L"\n";
        throw;
      }
      size_t previous = 0;
      for (const auto& spelling : spellings) {
        const auto current = ordered.find(L" " + spelling, previous);
        CHECK(current != std::wstring::npos);
        if (current != std::wstring::npos) previous = current + spelling.size() + 1;
      }
      CHECK(SetNestedParameter(ordered, def, first, value) == ordered);
    }
  }
  for (const auto& invalid : {
      L"/MergeCfg cfg.cf", L"/UpdateCfg cfg.cfu", L"/CompareCfg -ReportFile report.txt",
      L"/SignCfg -SignedFile signed.cfe", L"/ConfigurationRepositoryAddUser -User user -Pwd synthetic",
      L"/ConfigurationRepositoryCopyUsers -User user -Pwd synthetic",
      L"/DumpConfigToFiles dir -JobsCount 4", L"/UpdateDBCfg -Visible", L"/ManageCfgSupport -force",
      L"/MergeCfg cfg.cf -Settings settings.xml -EnableSupport -DisableSupport",
      L"/IBCheckAndRepair -TestOnly -BadRefClear"})
    CHECK(!ValidateParameterDraft(invalid, context).empty());
  CHECK(ValidateParameterDraft(L"/MergeCfg cfg.cf -Settings settings.xml", context).empty());
  CHECK(ValidateParameterDraft(L"/ConfigurationRepositoryAddUser -User user -Pwd \"\" -Rights ReadOnly", context).empty());
  CHECK(ValidateParameterDraft(L"/DumpConfigToFiles dir -Server -JobsCount 4", context).empty());
  CHECK(ValidateParameterDraft(L"/LoadConfigFromFiles dir", context).empty());
  CHECK(!ValidateParameterDraft(L"/LoadConfigFromFiles dir -files one.xml", context).empty());
  CHECK(ValidateParameterDraft(L"/LoadConfigFromFiles dir -files one.xml -Format Hierarchical", context).empty());
  CHECK(!ValidateParameterDraft(L"/LoadConfigFromFiles dir -listFile list.txt", context).empty());
  CHECK(ValidateParameterDraft(L"/LoadConfigFromFiles dir -listFile list.txt -Format Plain", context).empty());
  CHECK(NestedParameterRequired(L"/LoadConfigFromFiles", L"-Format", context.version, L"/LoadConfigFromFiles dir -files one.xml"));
  CHECK(!NestedParameterRequired(L"/LoadConfigFromFiles", L"-Format", context.version, L"/LoadConfigFromFiles dir"));
  for (const auto& version : LaunchParameterVersions()) {
    CHECK(ParameterPrimaryOperandLabel(L"/LoadConfigFromFiles", version) == L"каталог загрузки");
    const auto version_help = BuiltinParameterHelp(Parameter(L"/LoadConfigFromFiles"), version);
    CHECK(version_help.find(L"расширение нельзя загрузить вместо основной конфигурации и наоборот") != std::wstring::npos);
    if (version != L"8.3.6" && version != L"8.5.4") {
      CHECK(version_help.find(L"Полная загрузка: платформа сама распознаёт") != std::wstring::npos);
      CHECK(version_help.find(L"Частичная загрузка: автоматического распознавания нет") != std::wstring::npos);
    } else CHECK(version_help.find(L"формат необходимо задать явно") == std::wstring::npos);
  }
  CHECK(BuiltinParameterHelp(Parameter(L"/LoadConfigFromFiles"), L"8.5.4").find(L"В справочнике 8.5.4 есть расхождение") != std::wstring::npos);
  CHECK(ParameterPrimaryOperandLabel(L"/debug", L"8.5.1") == L"режим");
  CHECK(ParameterPrimaryOperandLabel(L"/DumpExternalDataProcessorOrReportToFiles", L"8.5.1") == L"корневой файл выгрузки; внешняя обработка (отчет)");
  CHECK(ParameterPrimaryOperandLabel(L"/Proxy", L"8.5.1").empty());
  CHECK(ParameterPrimaryOperandLabel(L"/LoadConfigFromFiles", L"8.5.3").empty());
  CHECK(!ValidateParameterDraft(L"/DumpIB", context).empty());
  CHECK(ValidateParameterDraft(L"/DumpIB backup.dt", context).empty());
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositorySetLabel -v 2 -comment text", context).empty());
  CHECK(ValidateParameterDraft(L"/ConfigurationRepositorySetLabel -v 2 label -comment text", context).empty());
  CHECK(ValidateParameterDraft(L"/ConfigurationRepositorySetLabel -v 2 -name label -comment text", context).empty());
  CHECK(ValidateParameterDraft(L"/LoadConfigFromFiles -Archive archive.zip", context).empty());
  CHECK(!ValidateParameterDraft(L"/LoadConfigFromFiles dir -Archive archive.zip", context).empty());
  CHECK(ValidateParameterDraft(L"/CreateDistributionFiles -cfufile update.cfu -f base.cf -v 1.0", context).empty());
  CHECK(!ValidateParameterDraft(L"/CreateDistributionFiles -cfufile update.cfu", context).empty());
  CHECK(!NestedParameterRequired(proxy.key, L"-PSrv", L"8.5.3", L"/Proxy"));
  const auto updated = ParameterNestedOptions(L"/UpdateDBCfg", L"8.5.4");
  const auto dynamic = std::find_if(updated.begin(), updated.end(), [](const auto& item) { return item.key == L"-Dynamic"; });
  const auto& update = Parameter(L"/UpdateDBCfg");
  CHECK(!ReadNestedParameter(L"/UpdateDBCfg -DynamicRestruct+", update, *dynamic));
  CHECK(SetNestedParameter(L"/UpdateDBCfg -DynamicRestruct+ -Dynamic-", update, *dynamic, L"+") ==
      L"/UpdateDBCfg -Dynamic+ -DynamicRestruct+");
  const auto& load = Parameter(L"/LoadConfigFromFiles");
  const auto load_options = ParameterNestedOptions(load.key, context.version);
  const auto no_check = std::find_if(load_options.begin(), load_options.end(), [](const auto& option) { return option.key == L"-NoCheck"; });
  bool precedence_rejected = false;
  try { static_cast<void>(SetNestedParameter(L"/LoadConfigFromFiles dir -listFile a.txt -files b.xml", load, *no_check, L"")); }
  catch (const std::invalid_argument&) { precedence_rejected = true; }
  CHECK(precedence_rejected);
  const auto& distribution = Parameter(L"/CreateDistributionFiles");
  const auto distribution_options = ParameterNestedOptions(distribution.key, context.version);
  const auto output = std::find_if(distribution_options.begin(), distribution_options.end(), [](const auto& option) { return option.key == L"-cffile"; });
  bool repeated_sources_rejected = false;
  try { static_cast<void>(SetNestedParameter(L"/CreateDistributionFiles -cfufile update.cfu -v 1.0 -f base.cf", distribution, *output, L"output.cf")); }
  catch (const std::invalid_argument&) { repeated_sources_rejected = true; }
  CHECK(repeated_sources_rejected);
}

void TestOperandDomains() {
  using namespace ibstart::launcher;
  const auto& debug = Parameter(L"/debug");
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, version};
    const auto options = ParameterNestedOptions(debug.key, version);
    if (version == L"8.3.6") {
      CHECK(options.empty());
      CHECK(NestedParameterChoices(debug.key, L"", version).empty());
      continue;
    }
    CHECK(options.size() == 1 && options.front().key == L"-attach");
    CHECK(NestedParameterChoices(debug.key, L"", version) == L"-tcp|-http");
    const NestedParameterDefinition mode{L"", L"режим", OptionStyle::space, true, 0, options.front().profile};
    CHECK(!NestedParameterRequired(debug.key, L"", version, L"/debug"));
    CHECK(!ReadNestedParameter(L"/debug", debug, mode));
    CHECK(ValidateParameterDraft(L"/debug", context).empty());
    CHECK(ValidateParameterDraft(L"/debug -tcp", context).empty());
    CHECK(ValidateParameterDraft(L"/debug -http -attach", context).empty());
    CHECK(!ValidateParameterDraft(L"/debug -tcp -attach", context).empty());
    CHECK(ValidateParameterDraft(L"/debug -attach", context).empty());
    CHECK(!ValidateParameterDraft(L"/debug -tcp -http", context).empty());
    CHECK(!ValidateParameterDraft(L"/debug smtp", context).empty());
    auto text = SetNestedParameter(L" /L ru  /Future keep ", debug, mode, L"-tcp");
    CHECK(text == L" /L ru  /Future keep /debug -tcp");
    text = SetNestedParameter(text, debug, mode, L"-http");
    text = SetNestedParameter(text, debug, options.front(), L"");
    CHECK(text.ends_with(L"/debug -http -attach"));
    CHECK(ReadNestedParameter(text, debug, mode) == std::optional<std::wstring>(L"-http"));
    CHECK(ReadNestedParameter(L"/debug -attach -http", debug, mode) == std::optional<std::wstring>(L"-http"));
    CHECK(SetNestedParameter(L"/debug -http -attach -Future value /L ru", debug, mode, L"-tcp") ==
        L"/debug -tcp -attach -Future value /L ru");
    CHECK(SetNestedParameter(L"/debug -http -attach", debug, mode, L"", false) == L"/debug  -attach");
    for (const auto value : {L"tcp", L"smtp", L"-tcp -http"}) {
      bool rejected = false;
      try { static_cast<void>(SetNestedParameter(L"/debug", debug, mode, value)); }
      catch (const std::invalid_argument&) { rejected = true; }
      CHECK(rejected);
    }
    bool ambiguous = false;
    try { static_cast<void>(SetNestedParameter(L"/debug -tcp -http", debug, mode, L"-http")); }
    catch (const std::invalid_argument&) { ambiguous = true; }
    CHECK(ambiguous);
    for (const auto ambiguous_text : {L"/debug -Future \"-http\"", L"/debug -Future -tcp"}) {
      bool preserved = false;
      try { static_cast<void>(SetNestedParameter(ambiguous_text, debug, mode, L"-tcp")); }
      catch (const std::invalid_argument&) { preserved = true; }
      CHECK(preserved);
      preserved = false;
      try { static_cast<void>(ReadNestedParameter(ambiguous_text, debug, mode)); }
      catch (const std::invalid_argument&) { preserved = true; }
      CHECK(preserved);
    }
    CHECK(NestedParameterChoices(L"/CompareCfg", L"-FirstName", version).empty());
    CHECK(NestedParameterChoices(L"/CompareCfg", L"-SecondName", version).empty());
    CHECK(NestedParameterChoices(L"/IBCheckAndRepair", L"-LogIntegrity", version).empty());
    CHECK(NestedParameterChoices(L"/ModuleGetSourceCodeAccess", L"", version).empty());
    for (const auto& command : LaunchParameterCatalog()) {
      if (command.kind != ParameterValueKind::reference) continue;
      for (const auto& option : ParameterNestedOptions(command.key, version)) {
        const auto choices = NestedParameterChoices(command.key, option.key, version);
        if (choices.empty()) continue;
        const auto value = choices.substr(0, choices.find(L'|'));
        const auto draft = SetNestedParameter(L"", command, option, value);
        CHECK(ReadNestedParameter(draft, command, option) == std::optional<std::wstring>(value));
        bool invalid = false;
        try { static_cast<void>(SetNestedParameter(draft, command, option, L"synthetic-invalid-choice")); }
        catch (const std::invalid_argument&) { invalid = true; }
        CHECK(invalid);
      }
    }
  }
  CHECK(NestedParameterChoices(L"/CompareCfg", L"-SecondConfigurationType", L"8.3.8") ==
      NestedParameterChoices(L"/CompareCfg", L"-FirstConfigurationType", L"8.3.8"));
  CHECK(NestedParameterChoices(L"/CompareCfg", L"-FirstConfigurationType", L"8.3.8").find(L"ExtensionConfigurationRepository") == std::wstring::npos);
  CHECK(NestedParameterChoices(L"/CompareCfg", L"-FirstConfigurationType", L"8.5.1").find(L"ExtensionConfigurationRepository") != std::wstring::npos);
  CHECK(NestedParameterChoices(L"/ConfigurationRepositoryReport", L"-ReportFormat", L"8.5.1") == L"txt|mxl");
  CHECK(NestedParameterChoices(L"/DumpConfigToFiles", L"-Format", L"8.5.4").empty());
  CHECK(NestedParameterChoices(L"/SignCfg", L"-SignType", L"8.3.26").empty());
  CHECK(NestedParameterChoices(L"/SignCfg", L"-SignType", L"8.3.27").empty());
  CHECK(NestedParameterChoices(L"/SignCfg", L"-SignType", L"8.5.1") == L"ConfigurationPatch|Copyright");
  CHECK(NestedParameterChoices(L"/debug", L"", L"8.5.3").empty());
  CHECK(ParameterChoicesForVersion(Parameter(L"/AppAutoCheckVersion"), L"8.3.6").empty());
  CHECK(ParameterChoicesForVersion(Parameter(L"/AppAutoCheckVersion"), L"8.3.8") == L"+|-");
  const ParameterEditorContext old{{}, ibstart::domain::ClientType::automatic, L"8.3.6"};
  CHECK(ValidateParameterDraft(L"/AppAutoCheckVersion", old).empty());
  CHECK(!ValidateParameterDraft(L"/AppAutoCheckVersion-", old).empty());
  const ParameterEditorContext stable{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  CHECK(ValidateParameterDraft(L"/UpdateDBCfg -Dynamic", stable).empty());
  CHECK(!ValidateParameterDraft(L"/UpdateDBCfg -Dynamicinvalid", stable).empty());
  CHECK(ValidateParameterDraft(L"/AppAutoCheckVersion", stable).empty());
  CHECK(ValidateParameterDraft(L"/AppAutoInstallLastVersion+", stable).empty());
  CHECK(!ValidateParameterDraft(L"/AppAutoInstallLastVersion", stable).empty());
  CHECK(SetLaunchParameter(L"", Parameter(L"/AppAutoCheckVersion"), L"-") == L"/AppAutoCheckVersion-");
  CHECK(SetLaunchParameter(L"", Parameter(L"/AppAutoInstallLastVersion"), L"+") == L"/AppAutoInstallLastVersion+");
}

void TestReviewedLaunchSemantics() {
  using namespace ibstart::launcher;
  const auto option = [](std::wstring_view command, std::wstring_view key, std::wstring_view version) {
    const auto options = ParameterNestedOptions(command, version);
    const auto found = std::find_if(options.begin(), options.end(), [&](const auto& item) { return item.key == key; });
    if (found == options.end()) throw std::runtime_error("Missing reviewed option");
    return *found;
  };
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, version};
    CHECK(!ValidateParameterDraft(L"/DeleteCfg", context).empty());
    CHECK(ValidateParameterDraft(L"/DeleteCfg -AllExtensions", context).empty());
    CHECK(ValidateParameterDraft(L"/DeleteCfg -Extension Synthetic", context).empty());
    if (ParameterVersionAvailability(L"/ManageCfgSupport", version) == ParameterAvailability::available) {
      CHECK(!ValidateParameterDraft(L"/ManageCfgSupport", context).empty());
      CHECK(NestedParameterRequired(L"/ManageCfgSupport", L"-disableSupport", version, L"/ManageCfgSupport"));
      CHECK(ValidateParameterDraft(L"/ManageCfgSupport -disableSupport", context).empty());
    }
    CHECK(!ValidateParameterDraft(L"/CreateDistributive dir -File setup.xml -MakeSetup -MakeFiles", context).empty());
    CHECK(ValidateParameterDraft(L"/CreateDistributive dir -File setup.xml -MakeSetup", context).empty());
    if (ParameterVersionAvailability(L"/CreateDistributivePackage", version) == ParameterAvailability::available)
      CHECK(!ValidateParameterDraft(L"/CreateDistributivePackage dir -File setup.xml -MakeSetup -MakeFiles", context).empty());
    if (ParameterVersionAvailability(L"/CheckCanApplyConfigurationExtensions", version) == ParameterAvailability::available) {
      CHECK(!ValidateParameterDraft(L"/CheckCanApplyConfigurationExtensions -AllZones -Extension Synthetic", context).empty());
      CHECK(!ValidateParameterDraft(L"/CheckCanApplyConfigurationExtensions -AllZones -Z 1", context).empty());
      CHECK(ValidateParameterDraft(L"/CheckCanApplyConfigurationExtensions -Extension Synthetic", context).empty());
    }
    if (version != L"8.3.6")
      CHECK(!ValidateParameterDraft(L"/LoadConfigFromFiles dir -AllExtensions -files one.xml -Format Plain", context).empty());
    const auto rights = NestedParameterChoices(L"/ConfigurationRepositoryAddUser", L"-Rights", version);
    CHECK(rights == L"ReadOnly|LockObjects|ManageConfigurationVersions|Administration");
    const auto selected = SetNestedParameter(L"", Parameter(L"/ConfigurationRepositoryAddUser"),
        option(L"/ConfigurationRepositoryAddUser", L"-Rights", version), L"ManageConfigurationVersions");
    CHECK(selected.ends_with(L"-Rights ManageConfigurationVersions"));
    const auto certificate = NestedParameterExplanation(L"/HttpsCert", L"-auto", version);
    CHECK(certificate.find(L"Windows") != std::wstring_view::npos);
    CHECK(certificate.find(L"Linux") == std::wstring_view::npos);
    CHECK((certificate.find(L"macOS") != std::wstring_view::npos) == (version != L"8.3.6" && version != L"8.3.7" &&
        version != L"8.3.8" && version != L"8.3.9" && version != L"8.3.10" && version != L"8.3.11" && version != L"8.3.12"));
    const auto mobile = BuiltinParameterHelp(Parameter(L"Mobile: launch"), version);
    if (version != L"8.3.6" && version != L"8.3.7" && version != L"8.3.8" && version != L"8.3.9" && version != L"8.3.10" && version != L"8.3.11")
      for (const auto marker : {L"• /O —", L"• /VL —", L"• /URL —", L"• /HttpsForceTLS1_0 —", L"• /DisableStartupMessages —"})
        CHECK(mobile.find(marker) != std::wstring::npos);
  }
  const ParameterEditorContext stable{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  CHECK(!ValidateParameterDraft(L"/MergeCfg cfg.cf -Settings setup.xml -Extension Synthetic -EnableSupport", stable).empty());
  CHECK(!ValidateParameterDraft(L"/IBCheckAndRepair -LogIntegrity /Z 1", stable).empty());
  CHECK(ValidateParameterDraft(L"/IBCheckAndRepair -LogIntegrity -Z:1", stable).empty());
  CHECK(SetNestedParameter(L"/LoadConfigFromFiles dir", Parameter(L"/LoadConfigFromFiles"),
      option(L"/LoadConfigFromFiles", L"-files", stable.version), L"one.xml,two.xml") == L"/LoadConfigFromFiles dir -files \"one.xml,two.xml\"");
  CHECK(SetNestedParameter(L"", Parameter(L"/ConfigurationRepositoryCommit"),
      option(L"/ConfigurationRepositoryCommit", L"-comment", stable.version), L"synthetic") == L"/ConfigurationRepositoryCommit -comment \"synthetic\"");
  CHECK(SetNestedParameter(L"", Parameter(L"/ConfigurationRepositorySetLabel"),
      option(L"/ConfigurationRepositorySetLabel", L"-name", stable.version), L"synthetic") == L"/ConfigurationRepositorySetLabel -name \"synthetic\"");
  CHECK(ReadNestedParameter(L"/ConfigurationRepositorySetLabel -name \"synthetic\"", Parameter(L"/ConfigurationRepositorySetLabel"),
      option(L"/ConfigurationRepositorySetLabel", L"-name", stable.version)) == std::optional<std::wstring>(L"synthetic"));
  CHECK(FindParameterFragments(L"/SLev0", Parameter(L"/SLev")).size() == 1);
  CHECK(FindParameterFragments(L"/ONormal", Parameter(L"/O")).size() == 1);
  CHECK(FindParameterFragments(L"/OIDA+ /OtherFuture", Parameter(L"/O")).empty());
  const auto password_old = NestedParameterExplanation(L"/ConfigurationRepositoryAddUser", L"-Pwd", L"8.3.6");
  CHECK(password_old.find(L"политик") == std::wstring_view::npos);
  CHECK(NestedParameterExplanation(L"/ConfigurationRepositoryAddUser", L"-Pwd", stable.version).find(L"политик") != std::wstring_view::npos);
  CHECK(NestedParameterExplanation(L"/CompareCfg", L"-FirstName", stable.version).find(L"-FirstConfigurationType") != std::wstring_view::npos);
  CHECK(ParameterPrimaryOperandLabel(L"/DisableExtensions", L"8.5.4") == L"имена расширений через запятую");
  CHECK(ParameterPrimaryOperandLabel(L"/DisableExtensions", L"8.5.1").empty());
  CHECK(ParameterPrimaryOperandLabel(L"/LoadConfigFiles", L"8.3.6") == L"каталог загрузки");
  const auto all = option(L"/DisableExtensions", L"-AllExtensions", L"8.5.4");
  const NestedParameterDefinition names{L"", L"имена расширений через запятую", OptionStyle::space, true, 0, all.profile};
  CHECK(SetNestedParameter(L"", Parameter(L"/DisableExtensions"), names, L"SyntheticOne,SyntheticTwo") ==
      L"/DisableExtensions SyntheticOne,SyntheticTwo");
}

void TestZeroArgumentCommandEditors() {
  using namespace ibstart::launcher;
  size_t checked{};
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, version};
    for (const auto& definition : LaunchParameterCatalog()) {
      const auto syntax = DocumentedParameterSyntax(definition.key, version);
      if (syntax.empty() || syntax.size() != definition.key.size()) continue;
      if (definition.kind != ParameterValueKind::reference && definition.kind != ParameterValueKind::flag &&
          definition.kind != ParameterValueKind::signed_switch) continue;
      const auto editor = ParameterEditorDefinition(definition, version);
      CHECK(editor.kind == ParameterValueKind::flag);
      CHECK(editor.choices.empty());
      const std::wstring before = L" /L ru ";
      const auto added = SetLaunchParameter(before, editor, L"");
      CHECK(added == before + std::wstring(editor.key));
      CHECK(ValidateParameterDraft(added, context).empty());
      const auto with_tail = added + L" /Future keep";
      CHECK(SetLaunchParameter(with_tail, editor, L"") == with_tail);
      CHECK(RemoveLaunchParameter(with_tail, editor) == before + L" /Future keep");
      CHECK(!ValidateParameterDraft(std::wstring(editor.key) + L" " + std::wstring(editor.key), context).empty());
      CHECK(ParameterHelpText(definition, context).find(L"нет отдельных полей") == std::wstring::npos);
      ++checked;
    }
  }
  CHECK(checked > 0);
  for (const auto key : {L"ENTERPRISE", L"DESIGNER", L"CREATEINFOBASE", L"CREATEINFOBASE: connection", L"Web: query", L"Mobile: launch"})
    CHECK(ParameterEditorDefinition(Parameter(key), L"8.5.1").kind == ParameterValueKind::reference);
  CHECK(ParameterEditorDefinition(Parameter(L"/ClearCache"), L"8.5.3").kind == ParameterValueKind::reference);
  CHECK(ParameterEditorDefinition(Parameter(L"/RegServer"), L"8.3.6").kind == ParameterValueKind::flag);
  CHECK(ParameterEditorDefinition(Parameter(L"/RegServer"), L"8.5.1").kind == ParameterValueKind::reference);
  CHECK(ParameterEditorDefinition(Parameter(L"/UnregServer"), L"8.5.1").kind == ParameterValueKind::flag);
  CHECK(ParameterVersionAvailability(L"/DisplayAllFunctions", L"8.3.12") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/DisplayAllFunctions", L"8.3.16") == ParameterAvailability::available);
  CHECK(ParameterVersionAvailability(L"/DisplayAllFunctions", L"8.3.17") == ParameterAvailability::unavailable);
  CHECK(ParameterVersionAvailability(L"/TechnicalSpecialistMode", L"8.3.17") == ParameterAvailability::available);
  const auto performance = ParameterEditorDefinition(Parameter(L"DisplayPerformance"), L"8.5.1");
  const auto privileged = ParameterEditorDefinition(Parameter(L"UsePrivilegedMode"), L"8.5.1");
  CHECK(FindParameterFragments(L"/C DisplayPerformance", performance).empty());
  CHECK(RemoveLaunchParameter(L"/C DisplayPerformance", performance) == L"/C DisplayPerformance");
  CHECK(SetLaunchParameter(L"", Parameter(L"/C"), L"DisplayPerformance") == L"/C \"DisplayPerformance\"");
  auto pair = SetLaunchParameter(L"/L ru", performance, L"");
  pair = SetLaunchParameter(pair, privileged, L"");
  CHECK(pair == L"/L ru DisplayPerformance UsePrivilegedMode");
  CHECK(RemoveLaunchParameter(pair, performance) == L"/L ru  UsePrivilegedMode");
  CHECK(FindParameterFragments(L"/LoadCfg DisplayPerformance", performance).empty());
  bool rejected = false;
  try { static_cast<void>(SetLaunchParameter(L"/LoadCfg synthetic.cf -Future keep", performance, L"")); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  rejected = false;
  try { static_cast<void>(RemoveLaunchParameter(L"/ClearCache -Future keep",
      ParameterEditorDefinition(Parameter(L"/ClearCache"), L"8.5.1"))); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
}

void TestAddCommandWithoutArguments() {
  using namespace ibstart::launcher;
  size_t checked{};
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, version, true};
    for (const auto& definition : LaunchParameterCatalog()) {
      if (DocumentedParameterSyntax(definition.key, version).empty() ||
          ParameterVersionAvailability(definition.key, version) != ParameterAvailability::available) continue;
      const std::wstring before = definition.key == L"/L" ? L" /C \"synthetic\" " : L" /L ru ";
      const auto draft = AddLaunchParameterCommand(before, definition, context);
      CHECK(draft == before + std::wstring(definition.key));
      CHECK(AddLaunchParameterCommand(draft, definition, context) == draft);
      const auto with_unknown = draft + L" -Future keep";
      CHECK(AddLaunchParameterCommand(with_unknown, definition, context) == with_unknown);
      ++checked;
    }
  }
  CHECK(checked > 0);
  const ParameterEditorContext stable{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  CHECK(ValidateParameterDraft(AddLaunchParameterCommand(L"", Parameter(L"/RegServer"), stable), stable).empty());
  CHECK(!ValidateParameterDraft(AddLaunchParameterCommand(L"", Parameter(L"/EmailAuth"), stable), stable).empty());
  for (const auto key : {L"/DisplayAllFunctions", L"/F", L"ENTERPRISE", L"DESIGNER", L"CREATEINFOBASE", L"Web: query", L"Mobile: launch"}) {
    bool rejected = false;
    try { static_cast<void>(AddLaunchParameterCommand(L"", Parameter(key), stable)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
  bool rejected = false;
  try { static_cast<void>(AddLaunchParameterCommand(L"", Parameter(L"/DumpIB"),
      {ibstart::domain::LaunchMode::enterprise, ibstart::domain::ClientType::thick, L"8.5.1"})); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
}

void TestPrimaryOnlyCommandEditors() {
  using namespace ibstart::launcher;
  size_t checked{};
  for (const auto& version : LaunchParameterVersions()) {
    const ParameterEditorContext context{{}, ibstart::domain::ClientType::automatic, version};
    const auto profile = ParameterVersionProfile(version);
    CHECK(profile != 0);
    for (const auto& command : LaunchParameterCatalog()) {
      if (command.kind != ParameterValueKind::reference || !ParameterNestedOptions(command.key, version).empty()) continue;
      const auto label = ParameterPrimaryOperandLabel(command.key, version);
      if (NestedParameterRequired(command.key, L"", version, command.key)) CHECK(!label.empty());
      if (label.empty()) continue;
      ++checked;
      const NestedParameterDefinition operand{L"", label, OptionStyle::space, true, 0, profile};
      CHECK(ParameterHelpText(command, context).find(L"нет отдельных полей") == std::wstring::npos);
      const auto value = label.find(L';') == std::wstring::npos ? L"\"synthetic value\"" : L"\"first value\" \"second value\"";
      const auto draft = SetNestedParameter(L"/L ru", command, operand, value);
      CHECK(ReadNestedParameter(draft, command, operand) == std::optional<std::wstring>(value));
      const auto changed = SetNestedParameter(draft + L" /Future untouched", command, operand, L"replacement");
      CHECK(changed == L"/L ru " + std::wstring(command.key) + L" replacement /Future untouched");
      if (NestedParameterRequired(command.key, L"", version, command.key))
        CHECK(!ValidateParameterDraft(command.key, context).empty());
    }
  }
  CHECK(checked > 0);
  CHECK(ParameterVersionProfile(L"8.5.1.1150") == ParameterVersionProfile(L"8.5.1"));
  CHECK(ParameterVersionProfile(L"8.5.3") == 0);
  CHECK(ParameterVersionProfile(L"8.3") == 0);
  CHECK(ParameterPrimaryOperandLabel(L"/EmailAuth", L"8.5.1") == L"email");
  CHECK(ParameterPrimaryOperandLabel(L"/EmailAuth", L"8.3.26").empty());
  const ParameterEditorContext stable{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  CHECK(!ValidateParameterDraft(L"/EmailAuth", stable).empty());
  CHECK(ValidateParameterDraft(L"/EmailAuth user@example.com", stable).empty());
}

void TestSinglePrimaryOperandQuoting() {
  using namespace ibstart::launcher;
  std::set<std::wstring> commands;
  size_t profiles{};
  for (const auto& version : LaunchParameterVersions()) {
    for (const auto& command : LaunchParameterCatalog()) {
      if (ParameterEditorDefinition(command, version).kind != ParameterValueKind::reference) continue;
      const auto label = ParameterPrimaryOperandLabel(command.key, version);
      if (label.empty() || label.find(L';') != std::wstring::npos ||
          !NestedParameterChoices(command.key, L"", version).empty()) continue;
      CHECK(ParameterPrimaryOperandCount(command.key, version) == 1);
      const NestedParameterDefinition operand{L"", label, OptionStyle::space, true, 0, ParameterVersionProfile(version)};
      commands.insert(std::wstring(command.key));
      ++profiles;
      for (const auto value : {L"C:\\work files\\dump.cf", L"\\\\server\\shared folder\\",
          L"Каталог\\имя файла.cf", L"value with \"quotes\" and \\"}) {
        const auto draft = SetNestedParameter(L"", command, operand, value);
        const auto tokens = SplitCommandArguments(draft);
        CHECK(tokens.size() == 2);
        if (tokens.size() == 2) CHECK(tokens[1] == value);
        const auto explicit_quotes = SetNestedParameter(L"", command, operand, QuoteWindowsArgument(value));
        CHECK(explicit_quotes == draft);
        CHECK(SetNestedParameter(L"", command, operand, L"  " + QuoteWindowsArgument(value) + L"\t") == draft);
        const auto read = ReadNestedParameter(draft, command, operand);
        CHECK(read.has_value());
        if (read) CHECK(SetNestedParameter(draft, command, operand, *read) == draft);
      }
      const auto updated = SetNestedParameter(L"  /L ru  " + std::wstring(command.key) +
          L" old.value -Future \"keep this\"  /C unchanged ", command, operand, L"C:\\new dir\\file.cf");
      CHECK(updated == L"  /L ru  " + std::wstring(command.key) +
          L" \"C:\\new dir\\file.cf\" -Future \"keep this\"  /C unchanged ");
    }
  }
  CHECK(commands.size() == 40);
  CHECK(profiles == 716);
  std::cout << "Single-primary quoting checked: " << commands.size() << " commands, " << profiles << " command/profile pairs\n";

  const auto& dump = Parameter(L"/DumpCfg");
  const NestedParameterDefinition operand{L"", L"file", OptionStyle::space, true, 0, ParameterVersionProfile(L"8.5.1")};
  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\synthetic-base\"";
  database.additional_parameters = SetNestedParameter(L"", dump, operand, L"C:\\work files\\dump.cf");
  ibstart::domain::PlatformInstallation platform;
  platform.executable = L"C:\\synthetic-platform\\1cv8.exe";
  platform.version = L"8.5.1.100";
  ibstart::domain::LaunchOptions options;
  options.mode = ibstart::domain::LaunchMode::designer;
  const auto launch = BuildCommand(database, platform, options);
  CHECK(launch.arguments.back() == L"C:\\work files\\dump.cf");
  CHECK(launch.arguments.size() == 5);

  for (const auto key : {L"/DumpExternalDataProcessorOrReportToFiles", L"/LoadExternalDataProcessorOrReportFromFiles"}) {
    const auto& command = Parameter(key);
    for (const auto& version : LaunchParameterVersions()) {
      const auto label = ParameterPrimaryOperandLabel(key, version);
      if (label.empty()) continue;
      CHECK(ParameterPrimaryOperandCount(key, version) == 2);
      const NestedParameterDefinition multiple{L"", label, OptionStyle::space, true, 0, ParameterVersionProfile(version)};
      const std::wstring values = L"\"C:\\root folder\\root.xml\" \"C:\\processor folder\\test.epf\"";
      const auto draft = SetNestedParameter(L"", command, multiple, values);
      CHECK(draft == std::wstring(key) + L" " + values);
      const auto tokens = SplitCommandArguments(draft);
      CHECK(tokens.size() == 3);
      if (tokens.size() == 3) {
        CHECK(tokens[1] == L"C:\\root folder\\root.xml");
        CHECK(tokens[2] == L"C:\\processor folder\\test.epf");
      }
      CHECK(ReadNestedParameter(draft, command, multiple) == std::optional<std::wstring>(values));
    }
  }
  bool rejected = false;
  try { static_cast<void>(SetNestedParameter(L"", dump, operand, L"\"one.cf\" \"two.cf\"")); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  CHECK(ParameterPrimaryOperandCount(L"/debug", L"8.5.1") == 1);
  CHECK(ParameterPrimaryOperandCount(L"/DumpCfg", L"8.5.3") == 0);
}

void TestOtherEditorsQuoteValues() {
  using namespace ibstart::launcher;
  size_t simple_checked{}, nested_checked{};
  const std::wstring value = L"C:\\folder with spaces\\quoted\"value\\";
  for (const auto& command : LaunchParameterCatalog()) {
    if ((command.kind == ParameterValueKind::text || command.kind == ParameterValueKind::file) && command.choices.empty()) {
      const auto tokens = SplitCommandArguments(SetLaunchParameter(L"", command, value));
      CHECK(tokens.size() == 2);
      if (tokens.size() == 2) CHECK(tokens[1] == value);
      ++simple_checked;
    }
    for (const auto& version : LaunchParameterVersions()) {
      for (const auto& option : ParameterNestedOptions(command.key, version)) {
        if (option.style == OptionStyle::flag || !NestedParameterChoices(command.key, option.key, version).empty()) continue;
        const auto draft = SetNestedParameter(L"", command, option, value);
        CHECK(ReadNestedParameter(draft, command, option) == std::optional<std::wstring>(value));
        ++nested_checked;
      }
    }
  }
  CHECK(simple_checked > 0 && nested_checked > 0);
  std::cout << "Other quoted editors checked: " << simple_checked << " simple commands, "
            << nested_checked << " named-option/profile pairs\n";
}

void TestNestedCommandOptions() {
  using namespace ibstart::launcher;
  const auto& create = Parameter(L"/ConfigurationRepositoryCreate");
  const auto options = ParameterNestedOptions(create.key, L"8.5.1");
  CHECK(options.size() == 7);
  const auto option = [&](std::wstring_view key) {
    const auto found = std::find_if(options.begin(), options.end(), [&](const auto& entry) { return entry.key == key; });
    if (found == options.end()) throw std::runtime_error("Missing nested option fixture");
    return *found;
  };
  const ParameterEditorContext context{ibstart::domain::LaunchMode::designer, ibstart::domain::ClientType::thick, L"8.5.1"};
  const ParameterEditorContext enterprise{ibstart::domain::LaunchMode::enterprise, ibstart::domain::ClientType::thick, L"8.5.1"};
  const ParameterEditorContext standalone{{}, ibstart::domain::ClientType::automatic, L"8.5.1"};
  for (const auto key : {L"/ConfigurationRepositoryCreate", L"/DumpCfg", L"/CheckConfig"}) {
    const auto& command = Parameter(key);
    CHECK(ParameterContextNotice(command, context).empty());
    CHECK(!ParameterContextNotice(command, enterprise).empty());
    CHECK(ParameterContextNotice(command, standalone).empty());
  }
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryCreate -NoBind", enterprise).empty());
  CHECK(ValidateParameterDraft(L"/ConfigurationRepositoryCreate -NoBind", standalone).empty());
  CHECK(!ValidateParameterDraft(L"/DumpCfg cfg.cf -Extension X", enterprise).empty());
  CHECK(ValidateParameterDraft(L"/DumpCfg cfg.cf -Extension X", context).empty());
  CHECK(ParameterContextNotice(Parameter(L"/DumpIB"), context).empty());
  CHECK(ValidateParameterDraft(L"/DumpIB backup.dt", context).empty());
  CHECK(ParameterHelpText(Parameter(L"/DumpIB"), context).find(L"нет отдельных полей") == std::wstring::npos);
  const auto help = ParameterHelpText(create, context);
  CHECK(help.find(L"<опции>") == std::wstring::npos);
  CHECK(help.find(L"\r\n\r\n• -ChangesAllowedRule <значение>\r\n") != std::wstring::npos);
  CHECK(help.find(L"\r\n    ObjectIsEditableSupportEnabled —") != std::wstring::npos);
  CHECK(help.find(L"нет отдельных полей") == std::wstring::npos);
  std::wstring draft = L" /ConfigurationRepositoryCreate -Future \"keep this\"  /L ru ";
  draft = SetNestedParameter(draft, create, option(L"-NoBind"), L"");
  draft = SetNestedParameter(draft, create, option(L"-MinPasswordLenght"), L"12");
  draft = SetNestedParameter(draft, create, option(L"-CheckPasswordComplexity"), L"");
  draft = SetNestedParameter(draft, create, option(L"-Extension"), L"Synthetic extension");
  CHECK(draft.find(L"-Future \"keep this\"") != std::wstring::npos);
  CHECK(draft.ends_with(L"  /L ru "));
  CHECK(ReadNestedParameter(draft, create, option(L"-NoBind")).has_value());
  CHECK(ReadNestedParameter(draft, create, option(L"-MinPasswordLenght")) == std::optional<std::wstring>(L"12"));
  CHECK(ReadNestedParameter(draft, create, option(L"-Extension")) == std::optional<std::wstring>(L"Synthetic extension"));
  CHECK(ValidateParameterDraft(draft, context).empty());
  draft = SetNestedParameter(draft, create, option(L"-AllowConfigurationChanges"), L"");
  CHECK(!ValidateParameterDraft(draft, context).empty());
  draft = SetNestedParameter(draft, create, option(L"-ChangesAllowedRule"), L"ObjectNotEditable");
  draft = SetNestedParameter(draft, create, option(L"-ChangesNotRecommendedRule"), L"ObjectNotSupported");
  CHECK(ValidateParameterDraft(draft, context).empty());
  const auto removed = SetNestedParameter(draft, create, option(L"-NoBind"), L"", false);
  CHECK(!ReadNestedParameter(removed, create, option(L"-NoBind")));
  CHECK(ReadNestedParameter(removed, create, option(L"-Extension")) == std::optional<std::wstring>(L"Synthetic extension"));
  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\synthetic-base\"";
  database.additional_parameters = draft;
  ibstart::domain::LaunchOptions launch_options;
  launch_options.mode = ibstart::domain::LaunchMode::designer;
  const auto launch = BuildCommand(database,
      {L"C:\\synthetic-platform\\1cv8.exe", L"8.5.1.100"}, launch_options);
  CHECK(std::count(launch.arguments.begin(), launch.arguments.end(), L"/ConfigurationRepositoryCreate") == 1);
  CHECK(std::find(launch.arguments.begin(), launch.arguments.end(), L"Synthetic extension") != launch.arguments.end());
  CHECK(std::find(launch.arguments.begin(), launch.arguments.end(), L"-CheckPasswordComplexity") != launch.arguments.end());
  for (const auto& bad : {draft + L" /ConfigurationRepositoryCreate", std::wstring(L"/ConfigurationRepositoryCreate -NoBind -NoBind")}) {
    bool rejected = false;
    try { static_cast<void>(SetNestedParameter(bad, create, option(L"-NoBind"), L"")); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryCreate -MinPasswordLenght nope", context).empty());
  bool combined_rule_rejected = false;
  try { static_cast<void>(SetNestedParameter(L"", create, option(L"-ChangesAllowedRule"), L"ObjectNotEditable|ObjectIsEditableSupportEnabled")); }
  catch (const std::invalid_argument&) { combined_rule_rejected = true; }
  CHECK(combined_rule_rejected);
  CHECK(!ValidateParameterDraft(L"/ConfigurationRepositoryCreate -Extension", context).empty());
  const auto& predefined = Parameter(L"/SetPredefinedDataUpdate");
  CHECK(ValidateParameterDraft(L"/SetPredefinedDataUpdate", context).empty());
  CHECK(!ValidateParameterDraft(L"/SetPredefinedDataUpdate -Auto -UpdateAutomatically", context).empty());
  CHECK(SetLaunchParameter(L"", predefined, L"") == L"/SetPredefinedDataUpdate");
  const auto predefined_help = ParameterHelpText(predefined, context);
  CHECK(predefined_help.find(L"\r\n\r\n• -Auto\r\n") != std::wstring::npos);
  CHECK(predefined_help.find(L"главного узла") != std::wstring::npos);
  CHECK(predefined_help.find(L"периферийного") != std::wstring::npos);
  CHECK(predefined_help.find(L"не создаются") != std::wstring::npos);
  const NestedParameterDefinition primary{L"", L"", OptionStyle::space, true};
  const auto& dump = Parameter(L"/DumpCfg");
  const auto changed = SetNestedParameter(L"/DumpCfg \"C:\\old cfg.cf\" -Extension X -Future keep", dump, primary, L"\"C:\\new cfg.cf\"");
  CHECK(changed == L"/DumpCfg \"C:\\new cfg.cf\" -Extension X -Future keep");
  bool attached_parent_rejected = false;
  try { static_cast<void>(SetNestedParameter(L"/DumpCfg\"C:\\old cfg.cf\" -Extension X", dump, primary, L"new.cf")); }
  catch (const std::invalid_argument&) { attached_parent_rejected = true; }
  CHECK(attached_parent_rejected);
  CHECK(ValidateParameterDraft(L"/DumpCfg\"C:\\old cfg.cf\" -Extension X", context).empty());
  const auto emulate = ParameterNestedOptions(L"/EmulateServerCallDelay", L"8.5.1");
  const auto call = std::find_if(emulate.begin(), emulate.end(), [](const auto& entry) { return entry.key == L"-Call"; });
  CHECK(call != emulate.end());
  if (call != emulate.end()) {
    const auto attached = SetNestedParameter(L"", Parameter(L"/EmulateServerCallDelay"), *call, L"123");
    CHECK(attached == L"/EmulateServerCallDelay -Call123");
    CHECK(ReadNestedParameter(attached, Parameter(L"/EmulateServerCallDelay"), *call) == std::optional<std::wstring>(L"123"));
  }
  CHECK(ParameterNestedOptions(create.key, L"8.5.3").empty());
  CHECK(DocumentedParameterSyntax(create.key, L"8.5.3").empty());
  CHECK(!ValidateParameterDraft(L"/RestoreIB synthetic.dt -UseBinaryDataStorage+", context).empty());
  CHECK(ValidateParameterDraft(L"/RestoreIB synthetic.dt -UseBinaryDataStorage+",
      {ibstart::domain::LaunchMode::designer, ibstart::domain::ClientType::thick, L"8.5.4"}).empty());
  CHECK(ValidateParameterDraft(L"/RestoreIB synthetic.dt -FutureStorage keep", context).empty());
}

void TestLaunchChecksActualParameterVersion() {
  ibstart::domain::Database database;
  database.connect = L"File=\"C:\\synthetic-base\"";
  database.additional_parameters = L"/Theme -Dark";
  ibstart::domain::PlatformInstallation platform;
  platform.version = L"8.3.26.1540";
  platform.executable = L"C:\\synthetic-platform\\1cv8.exe";
  ibstart::domain::LaunchOptions options;
  bool rejected = false;
  try { static_cast<void>(ibstart::launcher::BuildCommand(database, platform, options)); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
  platform.version = L"8.5.1.100";
  CHECK(!ibstart::launcher::BuildCommand(database, platform, options).arguments.empty());
  for (const auto value : {L"DESIGNER", L"/AppArch", L"/Proxy"}) {
    database.additional_parameters = ibstart::launcher::SetLaunchParameter(L"", Parameter(L"/C"), value);
    CHECK(ibstart::launcher::ValidateLaunchParameters(database, options).empty());
    const auto command = ibstart::launcher::BuildCommand(database, platform, options);
    CHECK(command.arguments.back() == value);
  }
}

void TestBuilderEmptyPasswordLaunch() {
  using namespace ibstart;
  domain::Database database;
  database.connect = L"File=\"C:\\synthetic-base\"";
  domain::PlatformInstallation platform;
  platform.executable = L"C:\\synthetic-platform\\1cv8.exe";
  platform.version = L"8.5.1.100";
  domain::LaunchOptions options;
  database.additional_parameters = launcher::SetLaunchParameter(L"", Parameter(L"/P"), L"");
  CHECK(launcher::ValidateParameterDraft(database.additional_parameters).empty());
  CHECK(launcher::ValidateLaunchParameters(database, options).empty());
  const auto command = launcher::BuildCommand(database, platform, options);
  CHECK(command.arguments.size() >= 2);
  CHECK(command.arguments[command.arguments.size() - 2] == L"/P");
  CHECK(command.arguments.back().empty());
  for (const auto invalid : {L"/P", L"/P /L ru", L"/N \"\""}) {
    database.additional_parameters = invalid;
    CHECK(!launcher::ValidateLaunchParameters(database, options).empty());
  }
}
}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc == 2 && std::wstring_view(argv[1]) == L"--cache-process-helper") {
    Sleep(30000);
    return 0;
  }

  const auto run = [](const wchar_t* name, const auto& test) {
    std::wcout << L"Running " << name << L"..." << std::endl;
    try { test(); }
    catch (const std::exception& error) { std::cerr << "UNCAUGHT " << error.what() << "\n"; ++failures; }
    catch (...) { std::wcerr << L"UNCAUGHT unknown exception\n"; ++failures; }
  };
  run(L"V8iRoundTrip", TestV8iRoundTrip);
  run(L"CheckMacroAcceptsCommaExpressions", TestCheckMacroAcceptsCommaExpressions);
  run(L"DemoCatalogFixture", TestDemoCatalogFixture);
  run(L"ProductVersion", TestProductVersion);
  run(L"UpdateVersionsAndVersionFile", TestUpdateVersionsAndVersionFile);
  run(L"UpdateCancellationOnHangingConnection", TestUpdateCancellationOnHangingConnection);
  run(L"UpdateTransportReadsAsyncResponse", TestUpdateTransportReadsAsyncResponse);
  run(L"UnicodeCaseInsensitiveSearch", TestUnicodeCaseInsensitiveSearch);
  run(L"ConnectionStringParsing", TestConnectionStringParsing);
  run(L"InstanceActivationPayload", TestInstanceActivationPayload);
  run(L"CatalogSearch", TestCatalogSearch);
  run(L"TreeFilters", TestTreeFilters);
  run(L"SearchExpandsAndRestoresGroups", TestSearchExpandsAndRestoresGroups);
  run(L"CatalogRefreshRejectsStaleSelection", TestCatalogRefreshRejectsStaleSelection);
  run(L"VirtualBranchSelectionSurvivesRefresh", TestVirtualBranchSelectionSurvivesRefresh);
  run(L"LongCatalogNamesUseFullIdentity", TestLongCatalogNamesUseFullIdentity);
  run(L"RecentDatabaseNames", TestRecentDatabaseNames);
  run(L"RelativeLaunchTimeFormatting", TestRelativeLaunchTimeFormatting);
  run(L"StableDatabaseId", TestStableDatabaseId);
  run(L"CaseInsensitiveDatabaseIdentifiers", TestCaseInsensitiveDatabaseIdentifiers);
  run(L"NoBomAndCatalog", TestNoBomAndCatalog);
  run(L"V8iLineEndingRoundTrips", TestV8iLineEndingRoundTrips);
  run(L"V8iStructuralLineEndingOwnership", TestV8iStructuralLineEndingOwnership);
  run(L"SafeStore", TestSafeStore);
  run(L"V8iFileSizeLimit", TestV8iFileSizeLimit);
  run(L"ConfirmedV8iOverwrite", TestConfirmedV8iOverwrite);
  run(L"V8iSaveRejectsActiveWriter", TestV8iSaveRejectsActiveWriter);
  run(L"V8iConcurrentSavesConflict", TestV8iConcurrentSavesConflict);
  run(L"V8iExternalWriterRaceAtCommitBoundary", TestV8iExternalWriterRaceAtCommitBoundary);
  run(L"V8iConcurrentSaveAtCommitBoundary", TestV8iConcurrentSaveAtCommitBoundary);
  run(L"CommandBuilderAndSelection", TestCommandBuilderAndSelection);
  run(L"OneOffLaunchOptions", TestOneOffLaunchOptions);
  run(L"LaunchParameterConflicts", TestLaunchParameterConflicts);
  run(L"ParameterBuilderPreservesText", TestParameterBuilderPreservesText);
  run(L"ParameterEditingActions", TestParameterEditingActions);
  run(L"ParameterVersionProfiles", TestParameterVersionProfiles);
  run(L"ParameterHelpAndDefaults", TestParameterHelpAndDefaults);
  run(L"DocumentedParameterSemantics", TestDocumentedParameterSemantics);
  run(L"RepositoryConnectionParameters", TestRepositoryConnectionParameters);
  run(L"NestedCommandOptions", TestNestedCommandOptions);
  run(L"NestedOptionOrderAndRequirements", TestNestedOptionOrderAndRequirements);
  run(L"OperandDomains", TestOperandDomains);
  run(L"ReviewedLaunchSemantics", TestReviewedLaunchSemantics);
  run(L"PrimaryOnlyCommandEditors", TestPrimaryOnlyCommandEditors);
  run(L"SinglePrimaryOperandQuoting", TestSinglePrimaryOperandQuoting);
  run(L"OtherEditorsQuoteValues", TestOtherEditorsQuoteValues);
  run(L"ZeroArgumentCommandEditors", TestZeroArgumentCommandEditors);
  run(L"AddCommandWithoutArguments", TestAddCommandWithoutArguments);
  run(L"LaunchChecksActualParameterVersion", TestLaunchChecksActualParameterVersion);
  run(L"BuilderEmptyPasswordLaunch", TestBuilderEmptyPasswordLaunch);
  run(L"PlatformDiscoveryLargeVersions", TestPlatformDiscoveryLargeVersions);
  run(L"StandaloneThinClientDiscovery", TestStandaloneThinClientDiscovery);
  run(L"ThinClientValidation", TestThinClientValidation);
  run(L"CustomX86PlatformDiscovery", TestCustomX86PlatformDiscovery);
  run(L"WindowsArgumentQuoting", TestWindowsArgumentQuoting);
  run(L"CatalogOrderingAndCycles", TestCatalogOrderingAndCycles);
  run(L"CatalogValidationAndStableSectionIndices", TestCatalogValidationAndStableSectionIndices);
  run(L"EmptyConnectDiagnosis", TestEmptyConnectDiagnosis);
  run(L"CatalogSetChildOrder", TestCatalogSetChildOrder);
  run(L"CatalogSortChildrenByName", TestCatalogSortChildrenByName);
  run(L"StandardFolderPaths", TestStandardFolderPaths);
  run(L"FileBaseScanRegistration", TestFileBaseScanRegistration);
  run(L"FileDatabasePathValidation", TestFileDatabasePathValidation);
  run(L"SecretMasking", TestSecretMasking);
  run(L"LogPruning", TestLogPruning);
  run(L"CacheSizeFormatting", TestCacheSizeFormatting);
  run(L"CacheScanResult", TestCacheScanResult);
  run(L"CacheMetricPersistence", TestCacheMetricPersistence);
  run(L"CacheSizeOperationProcessesQueue", TestCacheSizeOperationProcessesQueue);
  run(L"CacheReportsLocalizedWindowsError", TestCacheReportsLocalizedWindowsError);
  run(L"CacheRejectsLicenseDescendants", TestCacheRejectsLicenseDescendants);
  run(L"CacheRejectsJunctions", TestCacheRejectsJunctions);
  run(L"CacheAllowsJunctionedTrustedRoot", TestCacheAllowsJunctionedTrustedRoot);
  run(L"CacheRejectsSymbolicReparsePoints", TestCacheRejectsSymbolicReparsePoints);
  run(L"CacheContinuesAfterCandidateError", TestCacheContinuesAfterCandidateError);
  run(L"CacheContinuesWithActiveOneCProcess", TestCacheContinuesWithActiveOneCProcess);
  run(L"CacheIdentifiersDoNotCollide", TestCacheIdentifiersDoNotCollide);
  run(L"PortableMode", TestPortableMode);
  run(L"ProfileTransfer", TestProfileTransfer);
  run(L"ProfileTransferRejectsInterruptedTransaction", TestProfileTransferRejectsInterruptedTransaction);
  run(L"StartupUpdateSettings", TestStartupUpdateSettings);
  run(L"WindowsPathLimit", TestWindowsPathLimit);
  run(L"StorageFileSizeLimit", TestStorageFileSizeLimit);
  run(L"ConfigurationCatalog", TestConfigurationCatalog);
  run(L"ConfigurationSearchAndIsolation", TestConfigurationSearchAndIsolation);
  run(L"CatalogStateRepository", TestCatalogStateRepository);
  run(L"StorageRepositoryMergesStaleSnapshots", TestStorageRepositoryMergesStaleSnapshots);
  run(L"CatalogMetadataService", TestCatalogMetadataService);
  run(L"CatalogMetadataRenamePreservesFallbackHistory", TestCatalogMetadataRenamePreservesFallbackHistory);
  run(L"CatalogMetadataExplicitIdChangeKeepsHistoryKey", TestCatalogMetadataExplicitIdChangeKeepsHistoryKey);
  run(L"CatalogMetadataCacheInvalidation", TestCatalogMetadataCacheInvalidation);
  run(L"StorageSkipsMalformedRecords", TestStorageSkipsMalformedRecords);
  run(L"CatalogStateNormalization", TestCatalogStateNormalization);
  run(L"StorageRejectsUnreadableDataPath", TestStorageRejectsUnreadableDataPath);
  run(L"CredentialsScopesAndRoundTrip", TestCredentialsScopesAndRoundTrip);
  run(L"CredentialConcurrentMerges", TestCredentialConcurrentMerges);
  run(L"CredentialApplicabilityBoundaries", TestCredentialApplicabilityBoundaries);
  run(L"CredentialSettingsRejectInvalidScopeAndDuplicateIds", TestCredentialSettingsRejectInvalidScopeAndDuplicateIds);
  run(L"CredentialConcurrentDeletionAndAdditionConflicts", TestCredentialConcurrentDeletionAndAdditionConflicts);
  run(L"CredentialStaleSettingsPreservePasswordAndFutureSchemaInactive", TestCredentialStaleSettingsPreservePasswordAndFutureSchemaInactive);
  run(L"CredentialGroupIncludesNewNestedDatabase", TestCredentialGroupIncludesNewNestedDatabase);
  if (failures) { std::wcerr << failures << L" test(s) failed\n"; return 1; }
  std::wcout << L"All IBStart unit tests passed\n"; return 0;
}
