#include "core/catalog/catalog.hpp"
#include "core/domain/version.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/logging/logging.hpp"
#include "core/storage/storage.hpp"
#include "core/v8i/v8i_file_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::wcerr << L"FAILED " << __FUNCTION__ << L":" << __LINE__ << L"\n"; ++failures; } } while (false)

std::string ReadBytes(const std::filesystem::path& path) { std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}; }
void WriteBytes(const std::filesystem::path& path, std::string_view text) { std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(text.data(), static_cast<std::streamsize>(text.size())); }
std::filesystem::path Fixture(const wchar_t* name) { return std::filesystem::current_path() / L"tests" / L"fixtures" / name; }
std::filesystem::path Temp(const wchar_t* suffix) { auto path = std::filesystem::temp_directory_path() / (std::wstring(L"ibstart-tests-") + suffix + L"-" + std::to_wstring(GetCurrentProcessId())); std::error_code error; std::filesystem::remove_all(path, error); std::filesystem::create_directories(path); return path; }

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
}

void TestProductVersion() {
  CHECK(!ibstart::version::value.empty());
  const auto core = std::to_wstring(ibstart::version::major) + L"." + std::to_wstring(ibstart::version::minor) + L"." + std::to_wstring(ibstart::version::patch);
  const auto file = core + L"." + std::to_wstring(ibstart::version::revision);
  CHECK(ibstart::version::value.starts_with(core));
  CHECK(ibstart::version::file_value == file);
}

void TestUnicodeCaseInsensitiveSearch() {
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КД2", L"кд") == 0);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"Рабочая КД3", L"кд") == 8);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"Alpha BETA", L"beta") == 6);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КД2", L"д3") == std::wstring_view::npos);
  CHECK(ibstart::utf::FindNoCaseOrdinal(L"КДКД", L"кд", 2) == 2);
}

void TestNoBomAndCatalog() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8(ReadBytes(Fixture(L"no-bom-unknown.v8i")));
  CHECK(document.encoding == ibstart::v8i::Utf8Encoding::utf8);
  ibstart::catalog::Catalog catalog(std::move(document));
  CHECK(catalog.Databases().size() == 1);
  CHECK(ibstart::catalog::Catalog::IsWebConnection(catalog.Databases().front()->ValueOr(L"Connect")));
  CHECK(catalog.DatabaseFor(L"Web").unknown_fields.size() == 1);
}

void TestSafeStore() {
  const auto directory = Temp(L"store"); const auto file = directory / L"ibases.v8i";
  WriteBytes(file, "[Base]\r\nConnect=File=\"C:\\\\base\"\r\nUnknown=x\r\n");
  ibstart::v8i::V8iFileStore store(file); auto document = store.Read(); document.Find(L"Base")->entry.Set(L"Locale", L"ru_RU"); store.Save(document);
  CHECK(std::filesystem::exists(file)); CHECK(store.Backups().size() == 1); CHECK(ReadBytes(file).find("Locale=ru_RU") != std::string::npos);
  for (int index = 0; index != 6; ++index) { document.Find(L"Base")->entry.Set(L"OrderInList", std::to_wstring(index)); store.Save(document); }
  CHECK(store.Backups().size() == 5);
  bool temporaryLeftBehind = false; for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.path().filename().wstring().find(L".ibstart.tmp.") != std::wstring::npos) temporaryLeftBehind = true;
  CHECK(!temporaryLeftBehind);
  ibstart::v8i::V8iFileStore conflict(file); auto another = conflict.Read();
  const auto unchangedTime = std::filesystem::last_write_time(file); auto external = ReadBytes(file); const auto basePosition = external.find("Base"); CHECK(basePosition != std::string::npos); if (basePosition != std::string::npos) external[basePosition] = 'X'; WriteBytes(file, external); std::filesystem::last_write_time(file, unchangedTime);
  bool caught = false; try { conflict.Save(another); } catch (const ibstart::v8i::ExternalModificationError&) { caught = true; }
  CHECK(caught); CHECK(ReadBytes(file) == external);
  std::error_code error; std::filesystem::remove_all(directory, error);
}

void TestCommandBuilderAndSelection() {
  const std::vector<ibstart::domain::PlatformInstallation> platforms = {
    {L"C:\\Program Files\\1cv8\\8.3.9\\bin\\1cv8.exe", L"8.3.9", ibstart::domain::ClientBitness::x64, true},
    {L"C:\\Program Files (x86)\\1cv8\\8.3.24\\bin\\1cv8.exe", L"8.3.24", ibstart::domain::ClientBitness::x86, true},
    {L"C:\\Program Files\\1cv8\\8.3.24\\bin\\1cv8.exe", L"8.3.24", ibstart::domain::ClientBitness::x64, true}};
  ibstart::domain::LaunchOptions options; options.mode = ibstart::domain::LaunchMode::designer; const auto chosen = ibstart::launcher::SelectPlatform(platforms, options);
  CHECK(chosen && chosen->bitness == ibstart::domain::ClientBitness::x64);
  options.bitness = ibstart::domain::ClientBitness::x86; const auto x86 = ibstart::launcher::SelectPlatform(platforms, options); CHECK(x86 && x86->bitness == ibstart::domain::ClientBitness::x86); options.bitness = ibstart::domain::ClientBitness::automatic;
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
}

void TestCatalogOrderingAndCycles() {
  auto document = ibstart::v8i::V8iDocument::ParseUtf8("[Ten]\nOrderInList=10\n[Two]\nOrderInList=2\n");
  ibstart::catalog::Catalog catalog(std::move(document));
  const auto tree = catalog.Tree(); CHECK(tree.size() == 2); CHECK(tree.size() > 1 && tree[0].name == L"Two");
  CHECK(!catalog.AddGroup(L"Orphan", L"Missing"));
  CHECK(catalog.AddGroup(L"Parent")); CHECK(catalog.AddGroup(L"Child", L"Parent")); CHECK(!catalog.Move(L"Parent", L"Child", 0));
  const auto url = ibstart::catalog::Catalog::WebUrl(L"WS=\"https://example.test/base\";WA=1");
  CHECK(url && *url == L"https://example.test/base"); CHECK(!ibstart::catalog::Catalog::IsWebConnection(L"WS=not-a-url"));
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
}

void TestSecretMasking() {
  const auto masked = ibstart::logging::MaskSecrets(L"/N admin /P \"s3cret\" --token=abc password=xyz /Password hunter2");
  CHECK(masked.find(L"s3cret") == std::wstring::npos); CHECK(masked.find(L"abc") == std::wstring::npos); CHECK(masked.find(L"xyz") == std::wstring::npos); CHECK(masked.find(L"hunter2") == std::wstring::npos); CHECK(masked.find(L"admin") != std::wstring::npos);
}

void TestPortableMode() {
  const auto directory = Temp(L"portable"); const auto executable = directory / L"IBStart.exe"; WriteBytes(executable, ""); WriteBytes(directory / L"IBStart.portable", "");
  const auto layout = ibstart::storage::ResolveLayout(executable); CHECK(layout.portable); CHECK(layout.root == directory / L"data"); ibstart::storage::EnsureWritable(layout);
  ibstart::storage::Settings settings; settings.active_ibases = directory / L"База 😀.v8i"; settings.simple_mode = true; settings.platform_search_paths = {directory / L"Платформа"}; settings.window_width = 1234;
  ibstart::storage::SaveSettings(layout, settings); const auto loaded = ibstart::storage::LoadSettings(layout);
  CHECK(loaded.active_ibases == settings.active_ibases); CHECK(loaded.simple_mode); CHECK(loaded.platform_search_paths == settings.platform_search_paths); CHECK(loaded.window_width == 1234);
  const std::vector<std::wstring> favorites = {L"База 😀", L"Строка\nс переводом"}; ibstart::storage::SaveFavorites(layout, favorites); CHECK(ibstart::storage::LoadFavorites(layout) == favorites);
  ibstart::storage::AppendHistory(layout, {L"id-😀", std::chrono::system_clock::now(), ibstart::domain::LaunchMode::designer}); const auto history = ibstart::storage::LoadHistory(layout); CHECK(history.size() == 1); CHECK(!history.empty() && history[0].database_id == L"id-😀");
  std::error_code error; std::filesystem::remove_all(directory, error);
}
}

int wmain() {
  const auto run = [](const wchar_t* name, const auto& test) {
    std::wcout << L"Running " << name << L"..." << std::endl;
    try { test(); }
    catch (const std::exception& error) { std::cerr << "UNCAUGHT " << error.what() << "\n"; ++failures; }
    catch (...) { std::wcerr << L"UNCAUGHT unknown exception\n"; ++failures; }
  };
  run(L"V8iRoundTrip", TestV8iRoundTrip);
  run(L"ProductVersion", TestProductVersion);
  run(L"UnicodeCaseInsensitiveSearch", TestUnicodeCaseInsensitiveSearch);
  run(L"NoBomAndCatalog", TestNoBomAndCatalog);
  run(L"SafeStore", TestSafeStore);
  run(L"CommandBuilderAndSelection", TestCommandBuilderAndSelection);
  run(L"CatalogOrderingAndCycles", TestCatalogOrderingAndCycles);
  run(L"StandardFolderPaths", TestStandardFolderPaths);
  run(L"SecretMasking", TestSecretMasking);
  run(L"PortableMode", TestPortableMode);
  if (failures) { std::wcerr << failures << L" test(s) failed\n"; return 1; }
  std::wcout << L"All IBStart unit tests passed\n"; return 0;
}
