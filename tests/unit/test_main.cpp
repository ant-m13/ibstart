#include "app/instance_activation.hpp"
#include "core/catalog/catalog.hpp"
#include "core/catalog/catalog_metadata_service.hpp"
#include "core/cache/cache_service.hpp"
#include "core/connection/connection_string.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/logging/logging.hpp"
#include "core/platform/platform_discovery.hpp"
#include "core/platform/platform_version.hpp"
#include "core/scanner/file_base_scanner.hpp"
#include "core/storage/storage.hpp"
#include "core/update/update_service.hpp"
#include "core/v8i/v8i_file_store.hpp"
#include "ui/tree_presentation.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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

  const std::vector<ibstart::domain::PlatformInstallation> mixedClients = {
      {L"C:\\1cv8\\8.3.30\\bin\\1cv8c.exe", L"8.3.30", ibstart::domain::ClientBitness::x64, true},
      {L"C:\\1cv8\\8.3.29\\bin\\1cv8.exe", L"8.3.29", ibstart::domain::ClientBitness::x64, true}};
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
  WriteBytes(thickExecutable, "");
  WriteBytes(thinExecutable, "");
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

void TestPlatformDiscoveryLargeVersions() {
  const auto root = Temp(L"platform-versions");
  const auto smaller = root / L"8.3.99999999999999999999" / L"bin";
  const auto larger = root / L"8.3.100000000000000000000" / L"bin";
  std::filesystem::create_directories(smaller);
  std::filesystem::create_directories(larger);
  WriteBytes(smaller / L"1cv8.exe", "");
  WriteBytes(larger / L"1cv8.exe", "");

  const auto discovered = ibstart::platform::Discover({root}, false);
  const auto smallerPosition = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == smaller / L"1cv8.exe"; });
  const auto largerPosition = std::find_if(discovered.begin(), discovered.end(), [&](const auto& item) { return item.executable == larger / L"1cv8.exe"; });
  CHECK(smallerPosition != discovered.end());
  CHECK(largerPosition != discovered.end());
  if (smallerPosition != discovered.end() && largerPosition != discovered.end()) CHECK(largerPosition < smallerPosition);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

void TestStandaloneThinClientDiscovery() {
  constexpr std::wstring_view kVersion = L"8.3.27.1688";
  const auto root = Temp(L"standalone-thin-client");
  const auto bin = root / kVersion / L"bin";
  std::filesystem::create_directories(bin);
  const auto thin = bin / L"1cv8c.exe";
  WriteBytes(thin, "");

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
  CHECK(!catalog.AddServerDatabase(L"\r\n", L"Srvr=\"server\";Ref=\"base\""));
  CHECK(!catalog.AddGroup(L"Orphan", L"Missing"));
  CHECK(catalog.AddGroup(L"Parent")); CHECK(catalog.AddGroup(L"Child", L"Parent")); CHECK(!catalog.Move(L"Parent", L"Child", 0));
  CHECK(!catalog.RenameGroup(L"Parent", L"   "));
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

void TestCatalogSearch() {
  const auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Бухгалтерия]\nConnect=Srvr=cluster-01;Ref=Accounting\nFolder=/Рабочие\nCustom=клиент-А\n");
  const auto* entry = document.Find(L"Бухгалтерия");
  CHECK(entry != nullptr);
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"бух"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"CLUSTER-01"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"accounting"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"Рабочие"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"клиент-а"));
  CHECK(entry && ibstart::catalog::MatchesSearchText(entry->entry, L"custom"));
  CHECK(entry && !ibstart::catalog::MatchesSearchText(entry->entry, L"не найдено"));
}

void TestTreeFilters() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(
      "[Folder]\nFolder=/\n"
      "[Alpha]\nConnect=File=\"C:\\\\alpha\"\nFolder=/Folder\n"
      "[Beta]\nConnect=File=\"C:\\\\beta\"\nFolder=/Folder\n"
      "[Other]\nConnect=File=\"C:\\\\other\"\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const ibstart::storage::DatabaseTags tags = {
      {L"Alpha", {L"Production", L"Shared"}},
      {L"Beta", {L"test", L"shared"}},
  };
  const auto available = ibstart::ui::presentation::CollectFilterTags(catalog, tags);
  CHECK(available == std::vector<std::wstring>{L"Production", L"Shared", L"test"});

  const auto tree = catalog.Tree();
  const auto folder = std::find_if(tree.begin(), tree.end(), [](const auto& item) { return item.name == L"Folder"; });
  CHECK(folder != tree.end());
  if (folder == tree.end()) return;

  CHECK(ibstart::ui::presentation::MatchesSearchFilter(catalog, *folder, L"prod", tags));
  CHECK(!ibstart::ui::presentation::MatchesSearchFilter(catalog, *folder, L"release", tags));

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
  CHECK(ibstart::logging::ContainsSecretArguments(connectionSecret));
  CHECK(!ibstart::logging::ContainsSecretArguments(connectionWithoutSecret));
}

void TestLogPruning() {
  const auto directory = Temp(L"log-pruning");
  for (int index = 0; index < 10; ++index) {
    WriteBytes(directory / (L"ibstart_20260101_0000" + std::to_wstring(index) + L".log"), "old");
  }
  ibstart::logging::Logger logger(directory);
  logger.Info(L"new log entry /WS https://example.test/?AccessToken=log-token");
  const auto log = ReadBytes(logger.path());
  CHECK(log.find("log-token") == std::string::npos);
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

void TestPortableMode() {
  const auto directory = Temp(L"portable"); const auto executable = directory / L"IBStart.exe"; WriteBytes(executable, ""); WriteBytes(directory / L"IBStart.portable", "");
  const auto layout = ibstart::storage::ResolveLayout(executable); CHECK(layout.portable); CHECK(layout.root == directory / L"data"); ibstart::storage::EnsureWritable(layout);
  WriteBytes(layout.root / L"favorites.json", "[\"Устаревшее избранное\"]\n");
  ibstart::storage::CatalogStateRepository repository(layout);
  CHECK(repository.Read().favorites.empty());
  std::error_code removeLegacyError; std::filesystem::remove(layout.root / L"favorites.json", removeLegacyError);
  ibstart::storage::Settings settings; settings.active_ibases = directory / L"База 😀.v8i"; settings.selected_entry = L"Выбранная база 😀"; settings.simple_mode = true; settings.show_tags_in_list = false; settings.folders_first_when_sorting = false; settings.recent_ibases = {directory / L"Недавняя 1.v8i", directory / L"Недавняя 2.v8i"}; settings.platform_search_paths = {directory / L"Платформа"}; settings.window_width = 1234;
  ibstart::storage::SaveSettings(layout, settings); const auto loaded = ibstart::storage::LoadSettings(layout);
  CHECK(loaded.active_ibases == settings.active_ibases); CHECK(loaded.selected_entry == settings.selected_entry); CHECK(loaded.simple_mode); CHECK(!loaded.show_tags_in_list); CHECK(!loaded.folders_first_when_sorting); CHECK(loaded.recent_ibases == settings.recent_ibases); CHECK(loaded.platform_search_paths == settings.platform_search_paths); CHECK(loaded.window_width == 1234);
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

  repository.ClearHistory();
  CHECK(repository.Read().history.empty());
  CHECK(repository.Read().last_launches.contains(L"database-id"));

  ibstart::storage::CatalogState external;
  external.favorites = {L"Внешнее изменение"};
  ibstart::storage::SaveCatalogState(layout, external);
  CHECK(repository.Read().favorites == std::vector<std::wstring>{L"Основная база"});
  CHECK(repository.Reload().favorites == std::vector<std::wstring>{L"Внешнее изменение"});

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
    "recent_lists": [{"recent_list": "bad\q"}, {"recent_list": "C:\\recent.v8i"}],
    "platform_paths": [{"platform_path": "C:\\platform"}],
    "unknown_settings": {"recent_list": "C:\\unexpected.v8i", "platform_path": "C:\\unexpected-platform"}
  })");
  const auto settings = ibstart::storage::LoadSettings(layout);
  CHECK(settings.active_ibases == L"C:\\valid.v8i");
  CHECK(settings.selected_entry == L"Valid database");
  CHECK(settings.simple_mode);
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
}

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
  run(L"UnicodeCaseInsensitiveSearch", TestUnicodeCaseInsensitiveSearch);
  run(L"ConnectionStringParsing", TestConnectionStringParsing);
  run(L"InstanceActivationPayload", TestInstanceActivationPayload);
  run(L"CatalogSearch", TestCatalogSearch);
  run(L"TreeFilters", TestTreeFilters);
  run(L"RecentDatabaseNames", TestRecentDatabaseNames);
  run(L"StableDatabaseId", TestStableDatabaseId);
  run(L"NoBomAndCatalog", TestNoBomAndCatalog);
  run(L"V8iLineEndingRoundTrips", TestV8iLineEndingRoundTrips);
  run(L"SafeStore", TestSafeStore);
  run(L"ConfirmedV8iOverwrite", TestConfirmedV8iOverwrite);
  run(L"V8iSaveRejectsActiveWriter", TestV8iSaveRejectsActiveWriter);
  run(L"V8iConcurrentSavesConflict", TestV8iConcurrentSavesConflict);
  run(L"V8iExternalWriterRaceAtCommitBoundary", TestV8iExternalWriterRaceAtCommitBoundary);
  run(L"V8iConcurrentSaveAtCommitBoundary", TestV8iConcurrentSaveAtCommitBoundary);
  run(L"CommandBuilderAndSelection", TestCommandBuilderAndSelection);
  run(L"PlatformDiscoveryLargeVersions", TestPlatformDiscoveryLargeVersions);
  run(L"StandaloneThinClientDiscovery", TestStandaloneThinClientDiscovery);
  run(L"CustomX86PlatformDiscovery", TestCustomX86PlatformDiscovery);
  run(L"WindowsArgumentQuoting", TestWindowsArgumentQuoting);
  run(L"CatalogOrderingAndCycles", TestCatalogOrderingAndCycles);
  run(L"CatalogValidationAndStableSectionIndices", TestCatalogValidationAndStableSectionIndices);
  run(L"CatalogSetChildOrder", TestCatalogSetChildOrder);
  run(L"CatalogSortChildrenByName", TestCatalogSortChildrenByName);
  run(L"StandardFolderPaths", TestStandardFolderPaths);
  run(L"FileBaseScanRegistration", TestFileBaseScanRegistration);
  run(L"SecretMasking", TestSecretMasking);
  run(L"LogPruning", TestLogPruning);
  run(L"CacheSizeFormatting", TestCacheSizeFormatting);
  run(L"CacheRejectsLicenseDescendants", TestCacheRejectsLicenseDescendants);
  run(L"CacheContinuesAfterCandidateError", TestCacheContinuesAfterCandidateError);
  run(L"CacheContinuesWithActiveOneCProcess", TestCacheContinuesWithActiveOneCProcess);
  run(L"CacheIdentifiersDoNotCollide", TestCacheIdentifiersDoNotCollide);
  run(L"PortableMode", TestPortableMode);
  run(L"CatalogStateRepository", TestCatalogStateRepository);
  run(L"CatalogMetadataService", TestCatalogMetadataService);
  run(L"StorageSkipsMalformedRecords", TestStorageSkipsMalformedRecords);
  run(L"StorageRejectsUnreadableDataPath", TestStorageRejectsUnreadableDataPath);
  if (failures) { std::wcerr << failures << L" test(s) failed\n"; return 1; }
  std::wcout << L"All IBStart unit tests passed\n"; return 0;
}
