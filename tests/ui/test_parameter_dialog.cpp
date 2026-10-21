#include "ui/launch_parameter_dialog.hpp"
#include "ui/launch_options_dialog.hpp"
#include "ui/database_editor_dialog.hpp"
#include "ui/configuration_catalog_dialog.hpp"
#include "core/launcher/parameter_options.hpp"

#include <Windows.h>
#include <CommCtrl.h>
#include <Richedit.h>
#include <objbase.h>

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {
int failures{};
std::filesystem::path capture_directory;
void Check(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAILED " << message << '\n'; ++failures; }
}
std::wstring Text(HWND control) {
  std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
  value.resize(static_cast<size_t>(GetWindowTextW(control, value.data(), static_cast<int>(value.size()))));
  return value;
}
std::wstring RichText(HWND control) {
  GETTEXTLENGTHEX length{GTL_PRECISE | GTL_NUMCHARS, 1200};
  const auto count = SendMessageW(control, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
  std::wstring value(static_cast<size_t>(std::max<LRESULT>(count, 0)) + 1, L'\0');
  GETTEXTEX get{static_cast<DWORD>(value.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
  value.resize(static_cast<size_t>(SendMessageW(control, EM_GETTEXTEX,
      reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(value.data()))));
  return value;
}
CHARFORMAT2W FormatAt(HWND control, std::wstring_view token, size_t from = 0) {
  const auto text = RichText(control);
  const auto offset = text.find(token, from);
  Check(offset != std::wstring::npos, "Formatted token exists");
  if (offset == std::wstring::npos) return {};
  CHARRANGE range{static_cast<LONG>(offset), static_cast<LONG>(offset + token.size())};
  SendMessageW(control, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
  CHARFORMAT2W result{};
  result.cbSize = sizeof(result);
  SendMessageW(control, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&result));
  return result;
}
void CheckHelpHeadingVisible(HWND control, std::wstring_view heading, bool wrapped = false) {
  const auto text = RichText(control);
  const auto offset = text.find(heading);
  Check(offset != std::wstring::npos, "Selected parameter heading exists");
  if (offset == std::wstring::npos) return;
  POINTL start{}, end{}, body{};
  SendMessageW(control, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&start), static_cast<LPARAM>(offset));
  SendMessageW(control, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&end), static_cast<LPARAM>(offset + heading.size() - 1));
  const auto paragraph = text.find(L'\r', offset);
  SendMessageW(control, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&body), static_cast<LPARAM>(paragraph + 1));
  RECT content{};
  SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&content));
  Check(start.y >= content.top && start.y < content.bottom, "Parameter heading starts inside the visible description");
  Check(end.y >= start.y && end.y < content.bottom, "Entire parameter heading is visible");
  Check(body.y >= end.y && body.y < content.bottom, "Description begins below the visible parameter heading");
  if (wrapped) Check(end.y > start.y, "Long parameter heading really wraps in this fixture");
}
void Capture(HWND window, const wchar_t* name) {
  if (capture_directory.empty()) return;
  RECT rect{};
  GetWindowRect(window, &rect);
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  const HDC screen = GetDC(window), context = CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
  void* pixels{};
  const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (bitmap) {
    const auto previous = SelectObject(context, bitmap);
    RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE);
    Check(PrintWindow(window, context, 2) != FALSE, "Capture actual test dialog");
    GdiFlush();
    const DWORD bytes = static_cast<DWORD>(width * height * 4);
    BITMAPFILEHEADER header{0x4d42, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bytes,
        0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    std::ofstream file(capture_directory / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(static_cast<const char*>(pixels), bytes);
    Check(file.good(), "Write optional UI evidence");
    SelectObject(context, previous);
    DeleteObject(bitmap);
  } else Check(false, "Create capture bitmap");
  DeleteDC(context);
  ReleaseDC(window, screen);
}
void CheckActionRow(HWND builder) {
  RECT command{}, parameter{}, remove{}, reference{}, draft{};
  GetWindowRect(GetDlgItem(builder, 1815), &command);
  GetWindowRect(GetDlgItem(builder, 1808), &parameter);
  GetWindowRect(GetDlgItem(builder, 1814), &remove);
  GetWindowRect(GetDlgItem(builder, 1803), &reference);
  GetWindowRect(GetDlgItem(builder, 1809), &draft);
  Check(command.top == parameter.top && command.bottom == parameter.bottom,
      "Command and parameter action buttons share top and bottom edges");
  if (IsWindowVisible(GetDlgItem(builder, 1814)))
    Check(command.top == remove.top && command.bottom == remove.bottom,
        "Remove action shares the same button row");
  Check(reference.bottom < command.top, "Reference list does not overlap the action row");
  Check(command.bottom < draft.top, "Action row stays above the result field");
}
void CheckDirectActions(HWND builder, HWND list) {
  const HWND draft = GetDlgItem(builder, 1809), action = GetDlgItem(builder, 1808), remove = GetDlgItem(builder, 1814);
  const auto original = Text(draft);
  const auto set_draft = [&](const wchar_t* text) {
    SetWindowTextW(draft, text);
    SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1809, EN_CHANGE), reinterpret_cast<LPARAM>(draft));
  };
  const auto choose = [&](const wchar_t* key) {
    LVFINDINFOW find{};
    find.flags = LVFI_STRING;
    find.psz = key;
    const int row = ListView_FindItem(list, -1, &find);
    Check(row >= 0, "Direct action command exists");
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, row, FALSE);
  };
  set_draft(L" \t/C \"/DisableStartupMessages\"  /DisableStartupMessages\t/Future value  ");
  choose(L"/DisableStartupMessages");
  Check(Text(action) == L"Удалить из строки" && IsWindowEnabled(action), "Existing flag offers deletion");
  Check(!IsWindowVisible(remove), "Flag has one unambiguous action");
  Capture(builder, L"existing-flag.bmp");
  SendMessageW(action, BM_CLICK, 0, 0);
  Check(Text(draft) == L" \t/C \"/DisableStartupMessages\"  \t/Future value  ", "Removing flag preserves quoted literal, unknown command and whitespace");
  Check(Text(action) == L"Добавить в строку", "Removed flag can be added again");
  SendMessageW(action, BM_CLICK, 0, 0);
  Check(Text(draft) == L" \t/C \"/DisableStartupMessages\"  \t/Future value  /DisableStartupMessages", "Flag can be reinserted without changing surrounding text");
  set_draft(L"/C synthetic");
  Check(Text(action) == L"Добавить в строку", "Manual removal updates flag action immediately");
  set_draft(L"/DisableStartupMessages");
  Check(Text(action) == L"Удалить из строки", "Manual insertion updates flag action immediately");
  SendMessageW(action, BM_CLICK, 0, 0);
  Check(Text(draft).empty(), "Single flag can clear the draft");
  for (const auto text : {L"/DisableStartupMessages /disablestartupmessages", L"/DisableStartupMessages -FutureOption one"}) {
    set_draft(text);
    Check(!IsWindowEnabled(action), "Repeated or complex flag is left for manual editing");
    Check(Text(draft) == text, "Ambiguous input is preserved");
  }
  set_draft(L" /L en\t/C \"synthetic value\"  ");
  choose(L"/L");
  Check(Text(action) == L"Изменить в строке" && IsWindowVisible(remove) && IsWindowEnabled(remove),
      "Value command offers modification and separate deletion");
  SendMessageW(remove, BM_CLICK, 0, 0);
  Check(Text(draft) == L" \t/C \"synthetic value\"  ", "Deleting value command preserves its neighbor");
  set_draft(L"");
  choose(L"/ModifyPassword");
  CheckActionRow(builder);
  Capture(builder, L"action-row-simple.bmp");
  choose(L"/ConvertFiles");
  CheckActionRow(builder);
  Capture(builder, L"action-row-nested.bmp");
  choose(L"/L");
  set_draft(original.c_str());
}
HWND ThreadWindow(std::wstring_view name) {
  struct Search { std::wstring_view name; HWND result{}; } search{name};
  EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
    auto& item = *reinterpret_cast<Search*>(data);
    wchar_t klass[128]{};
    GetClassNameW(window, klass, 128);
    if (item.name == klass) { item.result = window; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}
HWND ChildWithText(HWND parent, std::wstring_view text) {
  struct Search { std::wstring_view text; HWND result{}; } search{text};
  EnumChildWindows(parent, [](HWND control, LPARAM data) -> BOOL {
    auto& item = *reinterpret_cast<Search*>(data);
    if (Text(control) == item.text) { item.result = control; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.result;
}
void Click(HWND parent, std::wstring_view text) {
  const HWND control = ChildWithText(parent, text);
  Check(control != nullptr, "Expected dialog button");
  if (control) SendMessageW(control, BM_CLICK, 0, 0);
}

enum class Scenario { identity, cancel, close, empty, repair, versions, standalone, launch, database_save, database_cancel, config_identity, config_save, config_cancel, config_manager, config_manager_cancel };
struct Driver {
  Scenario scenario{};
  int phase{};
  int ticks{};
  UINT_PTR timer{};
} driver;
const std::wstring initial = L" /L ru\t/C \"synthetic input\"  ";

void CALLBACK Drive(HWND, UINT, UINT_PTR, DWORD) {
  if (++driver.ticks > 300) {
    Check(false, "Dialog driver timeout");
    KillTimer(nullptr, driver.timer);
    PostQuitMessage(1);
    return;
  }
  const bool configuration_editor = driver.scenario == Scenario::config_identity || driver.scenario == Scenario::config_save || driver.scenario == Scenario::config_cancel;
  if (configuration_editor) {
    const auto window = ThreadWindow(L"IBStart.DatabaseEditor");
    if (!window || driver.phase++) return;
    const auto combo = GetDlgItem(window, 1015), version = GetDlgItem(window, 1016);
    Check(combo && version, "Configuration and version controls exist");
    if (driver.scenario != Scenario::config_identity) {
      SetWindowTextW(combo, L"УТ11");
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(1015, CBN_EDITCHANGE), reinterpret_cast<LPARAM>(combo));
      const auto count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
      Check(count >= 2 && count < 6, "Configuration selector searches across names and abbreviations");
      SendMessageW(combo, CB_SETCURSEL, 1, 0);
      SetWindowTextW(version, L"manual-beta");
    }
    RECT client{}, control{};
    GetClientRect(window, &client); GetWindowRect(GetDlgItem(window, IDOK), &control);
    MapWindowPoints(HWND_DESKTOP, window, reinterpret_cast<POINT*>(&control), 2);
    Check(control.bottom <= client.bottom && control.right <= client.right, "Save button fits expanded database editor");
    Capture(window, L"configuration-database.bmp");
    SendMessageW(window, WM_COMMAND, driver.scenario == Scenario::config_cancel ? IDCANCEL : IDOK, 0);
    return;
  }
  if (driver.scenario == Scenario::config_manager || driver.scenario == Scenario::config_manager_cancel) {
    const auto window = ThreadWindow(L"IBStart.ConfigurationCatalog");
    if (!window) return;
    if (driver.phase == 0) {
      SetWindowTextW(GetDlgItem(window, 1800), L"УТ11");
      Check(ListView_GetItemCount(GetDlgItem(window, 1801)) >= 1, "Dictionary search finds predefined configurations");
      Check(!GetDlgItem(window, 1802), "Dictionary has no inline name editor");
      ++driver.phase;
      SendMessageW(window, WM_COMMAND, 1805, 0);
      return;
    }
    if (driver.phase == 1) {
      const auto editor = ThreadWindow(L"IBStart.ConfigurationEditor");
      if (!editor) return;
      Check(!IsWindowEnabled(window) && GetWindow(editor, GW_OWNER) == window, "Record editor is modal to dictionary");
      Check(Text(editor) == L"Добавить конфигурацию", "Add opens a separate titled editor");
      for (const auto field : {1802, 1803, 1804})
        Check(Text(GetDlgItem(editor, field)).empty(), "New configuration starts with empty fields");
      SetWindowTextW(GetDlgItem(editor, 1802), L"SyntheticConfiguration");
      SetWindowTextW(GetDlgItem(editor, 1803), L"Synthetic configuration");
      SetWindowTextW(GetDlgItem(editor, 1804), L"SYN");
      Capture(editor, L"configuration-record-add.bmp");
      ++driver.phase;
      SendMessageW(editor, WM_COMMAND, driver.scenario == Scenario::config_manager ? IDOK : IDCANCEL, 0);
      return;
    }
    if (driver.phase != 2 || ThreadWindow(L"IBStart.ConfigurationEditor")) return;
    ++driver.phase;
    Check(IsWindowEnabled(window), "Closing record editor re-enables dictionary");
    if (driver.scenario == Scenario::config_manager) {
      Check(IsWindowEnabled(GetDlgItem(window, IDOK)), "Saved new dictionary entry can be selected");
      const auto list = GetDlgItem(window, 1801);
      const int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
      Check(row >= 0, "Saved dictionary entry stays selected in table");
      Capture(window, L"configuration-catalog.bmp");
      NMITEMACTIVATE click{};
      click.hdr = {list, 1801, NM_DBLCLK}; click.iItem = row;
      SendMessageW(window, WM_NOTIFY, 1801, reinterpret_cast<LPARAM>(&click));
    } else SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
    return;
  }
  const bool launch = driver.scenario == Scenario::launch;
  const bool database = driver.scenario == Scenario::database_save || driver.scenario == Scenario::database_cancel;
  if ((launch || database) && driver.phase == 0) {
    const HWND parent = ThreadWindow(launch ? L"IBStart.LaunchOptions" : L"IBStart.DatabaseEditor");
    if (!parent) return;
    ++driver.phase;
    Click(parent, launch ? L"Конструктор…" : L"Дополнительные настройки…");
    return;
  }
  if (database && driver.phase == 1) {
    const HWND advanced = ThreadWindow(L"IBStart.AdvancedDatabaseOptions");
    if (!advanced) return;
    ++driver.phase;
    Click(advanced, L"Конструктор…");
    return;
  }
  const int builder_phase = database ? 2 : launch ? 1 : 0;
  if (driver.phase == builder_phase) {
    const HWND builder = ThreadWindow(L"IBStart.LaunchParameterBuilder");
    if (!builder) return;
    ++driver.phase;
    Check(Text(builder) == L"Конструктор параметров запуска", "Builder window title");
    // Stable native control IDs let this test drive the real public modal API,
    // without adding test hooks or changing any real catalog/profile files.
    HWND list = GetDlgItem(builder, 1803);
    wchar_t header_text[128]{};
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    column.pszText = header_text;
    column.cchTextMax = 128;
    ListView_GetColumn(list, 0, &column);
    Check(std::wstring_view(header_text) == L"Команда", "Command column visible");
    ListView_GetColumn(list, 1, &column);
    Check(std::wstring_view(header_text) == L"Описание", "Description column visible");
    if (driver.scenario == Scenario::standalone) {
      Check(Text(GetDlgItem(builder, 1802)) == L"8.3.27", "Standalone defaults to highest installed version");
      Check(ChildWithText(builder, L"Копировать строку") != nullptr, "Standalone copy action exists");
      Check(ChildWithText(builder, L"Закрыть") != nullptr, "Standalone close action exists");
      Check(ChildWithText(builder, L"Применить") == nullptr, "Standalone has no destination apply action");
      for (const auto key : {L"/F", L"/S", L"/WS", L"/IBName", L"/IBConnectionString"}) {
        SetWindowTextW(GetDlgItem(builder, 1809), L"");
        LVFINDINFOW find{};
        find.flags = LVFI_STRING;
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(IsWindowEnabled(GetDlgItem(builder, 1808)) && IsWindowEnabled(GetDlgItem(builder, 1805)), "Standalone connection command is editable");
        const wchar_t* value = std::wstring_view(key) == L"/F" ? L"C:\\Synthetic base" :
            std::wstring_view(key) == L"/S" ? L"synthetic-server\\Synthetic base" :
            std::wstring_view(key) == L"/WS" ? L"https://example.com/synthetic" :
            std::wstring_view(key) == L"/IBName" ? L"Synthetic base" : L"File=\"C:\\Synthetic base\";";
        SetWindowTextW(GetDlgItem(builder, 1805), value);
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1808)) == L"Изменить в строке", "Connection command can be modified after insertion");
        if (std::wstring_view(key) == L"/F") {
          Check(Text(GetDlgItem(builder, 1809)) == L"/F \"C:\\Synthetic base\"", "Directory with spaces is quoted");
          Capture(builder, L"standalone-connection.bmp");
        }
        if (std::wstring_view(key) == L"/S") Check(Text(GetDlgItem(builder, 1809)) == L"/S \"synthetic-server\\Synthetic base\"", "Server address with spaces is quoted");
        Check(Text(GetDlgItem(builder, 1810)).find(L"Подключение задаётся") == std::wstring::npos, "Standalone has no selected-base restriction");
        SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)).empty(), "Connection command can be removed");
      }
      CheckDirectActions(builder, list);
      SetWindowTextW(GetDlgItem(builder, 1809), L"/C \"invalid draft");
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Standalone can close without saving an invalid draft");
    }
    if (driver.scenario == Scenario::versions) {
      LVFINDINFOW find{};
      find.flags = LVFI_STRING;
      find.psz = L"/Theme";
      const int row = ListView_FindItem(list, -1, &find);
      ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      HWND version = GetDlgItem(builder, 1802);
      const auto refresh = [&] {
        MSG message{};
        while (PeekMessageW(&message, builder, WM_APP, 0xBFFF, PM_REMOVE)) DispatchMessageW(&message);
      };
      const auto choose_version = [&](const wchar_t* label) {
        const LRESULT index = SendMessageW(version, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(label));
        Check(index != CB_ERR, "Requested version profile exists");
        SendMessageW(version, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1802, CBN_SELCHANGE), reinterpret_cast<LPARAM>(version));
        refresh();
      };
      choose_version(L"8.3.26");
      Check(!IsWindowEnabled(GetDlgItem(builder, 1808)), "Theme insertion disabled in old profile");
      choose_version(L"8.5.1");
      Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Theme insertion enabled in stable 8.5 profile");
      Check(Text(GetDlgItem(builder, 1805)) == L"-Dark", "Theme value uses version-specific grammar");
      choose_version(L"8.5.4 (тестовая)");
      Check(Text(version).find(L"тестовая") != std::wstring::npos, "Test platform clearly labeled");
      COMBOBOXINFO combo{sizeof(combo)};
      Check(GetComboBoxInfo(version, &combo) != FALSE, "Version combo has native edit information");
      SetFocus(combo.hwndItem);
      SendMessageW(combo.hwndItem, EM_SETSEL, 0, -1);
      std::wstring typed;
      for (const wchar_t character : std::wstring(L"8.3.26")) {
        SendMessageW(combo.hwndItem, WM_CHAR, character, 0);
        typed += character;
        refresh();
        Check(Text(version) == typed, "Typing version prefix does not autocomplete or reset text");
        Check(GetFocus() == combo.hwndItem, "Version typing preserves input focus");
      }
      find.psz = L"/TComp";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_version(L"8.3.21");
      auto description = Text(GetDlgItem(builder, 1804));
      Check(description.find(L"По умолчанию: -SDC") != std::wstring::npos, "Embedded old compression default displayed");
      Check(description.find(L"Zstandard") == std::wstring::npos, "Old description excludes new compression algorithm");
      choose_version(L"8.5.1");
      description = Text(GetDlgItem(builder, 1804));
      Check(description.find(L"Zstandard") != std::wstring::npos, "Embedded compression option explanation displayed");
      Check(description.find(L"По умолчанию: -zstd") != std::wstring::npos, "Embedded new compression default displayed");
      find.psz = L"/DisableLocalSpeechToText";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_version(L"8.3.23");
      Check(!IsWindowVisible(GetDlgItem(builder, 1805)), "Original speech command is a flag");
      choose_version(L"8.3.24");
      Check(IsWindowVisible(GetDlgItem(builder, 1805)) && SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 2,
          "Later speech command exposes signed modes");
      choose_version(L"8.5.1");
      find.psz = L"/LoadCfg";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(Text(GetDlgItem(builder, 1804)).find(L"-Extension") != std::wstring::npos,
          "Embedded help also describes reference-only batch commands");
      for (const auto key : {L"/ConfigurationRepositoryF", L"/ConfigurationRepositoryN", L"/ConfigurationRepositoryP"}) {
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(IsWindowEnabled(GetDlgItem(builder, 1805)), "Repository connection value editor enabled");
        Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Repository connection insert button enabled");
        Check(Text(GetDlgItem(builder, 1804)).find(L"нет отдельных полей") == std::wstring::npos,
            "Repository connection does not show unsupported-editor notice");
        SetWindowTextW(GetDlgItem(builder, 1805), L"synthetic repository value");
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)).find(std::wstring(key) + L" \"synthetic repository value\"") != std::wstring::npos,
            "Repository connection value inserted and quoted by real form");
      }
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/ConfigurationRepositoryCreate";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(SendMessageW(GetDlgItem(builder, 1813), CB_GETCOUNT, 0, 0) == 7, "All seven repository creation options exposed");
      const auto choose_option = [&](const wchar_t* label) {
        const auto selector = GetDlgItem(builder, 1813);
        auto index = SendMessageW(selector, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(label));
        if (index < 0) {
          const auto required_label = std::wstring(label) + L" *";
          index = SendMessageW(selector, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(required_label.c_str()));
        }
        Check(index != CB_ERR, "Nested option present in selector");
        SendMessageW(selector, CB_SETCURSEL, index, 0);
        SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1813, CBN_SELCHANGE), reinterpret_cast<LPARAM>(selector));
      };
      const auto choose_value = [&](const wchar_t* value) {
        const auto control = GetDlgItem(builder, 1805);
        const auto index = SendMessageW(control, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value));
        Check(index != CB_ERR, "Documented operand value is selectable");
        SendMessageW(control, CB_SETCURSEL, index, 0);
      };
      size_t zero_argument_profiles_checked{};
      std::set<std::wstring> zero_argument_commands;
      for (const auto& family : ibstart::launcher::LaunchParameterVersions()) {
        const auto version_label = family == L"8.5.4" ? family + L" (тестовая)" : family;
        choose_version(version_label.c_str());
        for (const auto& definition : ibstart::launcher::LaunchParameterCatalog()) {
          const auto editor = ibstart::launcher::ParameterEditorDefinition(definition, family);
          if (editor.kind != ibstart::launcher::ParameterValueKind::flag ||
              (definition.kind != ibstart::launcher::ParameterValueKind::reference && definition.key != L"/DisplayAllFunctions") ||
              ibstart::launcher::ParameterVersionAvailability(definition.key, family) != ibstart::launcher::ParameterAvailability::available)
            continue;
          const auto key = std::wstring(definition.key);
          find.psz = key.c_str();
          ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
          Check(!IsWindowVisible(GetDlgItem(builder, 1805)), "Zero-argument command has no value input");
          Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Documented zero-argument command can be inserted");
          Check(Text(GetDlgItem(builder, 1804)).find(L"нет отдельных полей") == std::wstring::npos,
              "Zero-argument command does not show unsupported-editor notice");
          ++zero_argument_profiles_checked;
          if (zero_argument_commands.insert(key).second) {
            SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
            Check(Text(GetDlgItem(builder, 1809)) == initial + key, "Zero-argument command is inserted as one token");
            Check(Text(GetDlgItem(builder, 1808)) == L"Удалить из строки", "Existing zero-argument command offers removal");
            SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
            Check(Text(GetDlgItem(builder, 1809)) == initial, "Removing zero-argument command preserves neighboring text");
          }
        }
      }
      std::cout << "Zero-argument editors checked: " << zero_argument_commands.size() << " commands, "
                << zero_argument_profiles_checked << " command/profile pairs\n";
      choose_version(L"8.3.16");
      find.psz = L"/DisplayAllFunctions";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Historical DisplayAllFunctions is available in 8.3.16");
      choose_version(L"8.5.1");
      Check(!IsWindowEnabled(GetDlgItem(builder, 1808)), "Historical DisplayAllFunctions remains unavailable in 8.5.1");
      Check(!IsWindowEnabled(GetDlgItem(builder, 1815)), "Whole-command insertion also respects the version");
      find.psz = L"/TechnicalSpecialistMode";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Modern technical specialist command is available");
      for (const auto key : {L"/ClearCache", L"/UnregServer"}) {
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)) == initial + key, "Requested command can be added without arguments");
        Capture(builder, std::wstring_view(key) == L"/ClearCache" ? L"clear-cache-command.bmp" : L"unreg-server-command.bmp");
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      }
      find.psz = L"/RegServer";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(IsWindowEnabled(GetDlgItem(builder, 1815)), "Command with optional nested parameters can be added alone");
      SendMessageW(GetDlgItem(builder, 1815), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)) == initial + L"/RegServer", "Whole-command button does not add an optional parameter");
      Check(!IsWindowEnabled(GetDlgItem(builder, 1815)), "Whole-command button does not offer duplicate insertion");
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Bare RegServer is a complete valid command");
      Capture(builder, L"reg-server-no-options.bmp");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/EmailAuth";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      SendMessageW(GetDlgItem(builder, 1815), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)) == initial + L"/EmailAuth", "Required values may be filled after adding a command");
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Missing required email still prevents applying the draft");
      SetWindowTextW(GetDlgItem(builder, 1805), L"user@example.com");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Filling the required operand completes the command");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      for (const auto key : {L"ENTERPRISE", L"DESIGNER", L"Web: query", L"Mobile: launch"}) {
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(!IsWindowEnabled(GetDlgItem(builder, 1815)), "Mode and reference groups are not CLI command tokens");
      }
      size_t primary_profiles_checked{};
      std::set<std::wstring> primary_commands;
      for (const auto& family : ibstart::launcher::LaunchParameterVersions()) {
        const auto version_label = family == L"8.5.4" ? family + L" (тестовая)" : family;
        choose_version(version_label.c_str());
        for (const auto& definition : ibstart::launcher::LaunchParameterCatalog()) {
          if (definition.kind != ibstart::launcher::ParameterValueKind::reference ||
              !ibstart::launcher::ParameterNestedOptions(definition.key, family).empty()) continue;
          const auto label = ibstart::launcher::ParameterPrimaryOperandLabel(definition.key, family);
          if (label.empty()) continue;
          const auto key = std::wstring(definition.key);
          find.psz = key.c_str();
          const auto command_row = ListView_FindItem(list, -1, &find);
          Check(command_row >= 0, "Primary-only command is present in catalogue");
          if (command_row < 0) continue;
          ListView_SetItemState(list, command_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
          Check(IsWindowEnabled(GetDlgItem(builder, 1805)) && IsWindowVisible(GetDlgItem(builder, 1805)),
              "Primary-only command has a visible editable value in every documented profile");
          Check(IsWindowEnabled(GetDlgItem(builder, 1808)), "Primary-only command can be inserted");
          Check(SendMessageW(GetDlgItem(builder, 1813), CB_GETCOUNT, 0, 0) == 1,
              "Primary operand appears without requiring named nested options");
          Check(Text(GetDlgItem(builder, 1804)).find(L"нет отдельных полей") == std::wstring::npos,
              "Available primary editor does not show unsupported-editor notice");
          ++primary_profiles_checked;
          primary_commands.insert(key);
        }
      }
      Check(primary_profiles_checked > 0, "All-profile primary editor audit ran");
      std::cout << "Primary-only editors checked: " << primary_commands.size() << " commands, "
                << primary_profiles_checked << " command/profile pairs\n";
      choose_version(L"8.5.1");
      for (const auto key : {L"/DumpCfg", L"/LoadCfg", L"/DumpIB", L"/RestoreIB", L"/LoadConfigFromFiles",
          L"/ConvertFiles", L"/DumpConfigToFiles", L"/AgentBaseDir", L"/SignCfg"}) {
        choose_version(std::wstring_view(key) == L"/SignCfg" ? L"8.3.15" : L"8.5.1");
        SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(Text(GetDlgItem(builder, 1804)).find(L"Кавычки для пробелов добавляются автоматически") != std::wstring::npos,
            "Single-value primary editor explains automatic quoting");
        SetWindowTextW(GetDlgItem(builder, 1805), L"C:\\synthetic folder\\имя файла.cf");
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)) == initial + key + L" \"C:\\synthetic folder\\имя файла.cf\"",
            "Real form quotes an unquoted primary path as one argument and preserves surrounding text");
        if (std::wstring_view(key) == L"/DumpCfg") Capture(builder, L"primary-path-quoted.bmp");
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)) == initial + key + L" \"C:\\synthetic folder\\имя файла.cf\"",
            "Applying an already quoted primary value again is stable");
      }
      choose_version(L"8.5.1");
      for (const auto key : {L"/DumpExternalDataProcessorOrReportToFiles", L"/LoadExternalDataProcessorOrReportFromFiles"}) {
        SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        const std::wstring values = L"\"C:\\root folder\\root.xml\" \"C:\\processor folder\\test.epf\"";
        SetWindowTextW(GetDlgItem(builder, 1805), values.c_str());
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)) == initial + key + L" " + values,
            "Real form preserves two explicitly quoted primary operands as separate arguments");
      }
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      choose_version(L"8.3.26");
      find.psz = L"/EmailAuth";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(!IsWindowEnabled(GetDlgItem(builder, 1808)), "Email auth remains unavailable in an older profile");
      choose_version(L"8.5.1");
      choose_option(L"email");
      Check(Text(GetDlgItem(builder, 1813)) == L"email *", "Email operand has its documented name and required marker");
      SetWindowTextW(GetDlgItem(builder, 1805), L"first@example.com");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)) == initial + L"/EmailAuth first@example.com", "Email auth is inserted with its operand");
      SetWindowTextW(GetDlgItem(builder, 1805), L"user@example.com");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)) == initial + L"/EmailAuth user@example.com", "Editing email replaces the previous operand");
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Valid email draft can be applied");
      Capture(builder, L"email-auth.bmp");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/ConfigurationRepositoryAddUser";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_option(L"-User <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"synthetic-user");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-Pwd <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"synthetic-password");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-Rights <значение>");
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 4, "Repository rights select one of four documented values");
      choose_value(L"Administration");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"-Rights Administration") != std::wstring::npos, "Selected repository right is serialized");
      choose_value(L"ReadOnly");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"-Rights ReadOnly") != std::wstring::npos &&
          Text(GetDlgItem(builder, 1809)).find(L"Administration") == std::wstring::npos, "Changing right replaces the previous selection");
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Complete repository user draft with one right can be applied");
      Capture(builder, L"repository-user-rights.bmp");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      choose_version(L"8.5.4 (тестовая)");
      find.psz = L"/DisableExtensions";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_option(L"имена расширений через запятую");
      SetWindowTextW(GetDlgItem(builder, 1805), L"SyntheticOne,SyntheticTwo");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"/DisableExtensions SyntheticOne,SyntheticTwo") != std::wstring::npos,
          "Non-angle-bracket operand syntax exposes the extension names editor");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      choose_version(L"8.5.1");
      find.psz = L"/debug";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(SendMessageW(GetDlgItem(builder, 1813), CB_GETCOUNT, 0, 0) == 2,
          "Debug has one optional mode operand and one attach flag");
      choose_option(L"режим");
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 2, "Debug mode selects one of two transports");
      Check(Text(GetDlgItem(builder, 1804)).find(L"-tcp —") != std::wstring::npos, "Mode help explains the transport alternatives");
      choose_value(L"-http");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-attach");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"/debug -http -attach") != std::wstring::npos, "Debug serializes HTTP mode then attach");
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "HTTP attach draft can be applied");
      Capture(builder, L"debug-http-attach.bmp");
      choose_option(L"режим");
      Capture(builder, L"debug-mode.bmp");
      choose_value(L"-tcp");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "TCP attach mismatch cannot be applied");
      Check(Text(GetDlgItem(builder, 1810)).find(L"-http") != std::wstring::npos, "Attach mismatch explains required HTTP mode");
      choose_option(L"-attach");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Removing attach repairs TCP draft");
      choose_option(L"режим");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Debug without optional mode remains valid");
      SetWindowTextW(GetDlgItem(builder, 1809), L"/debug -tcp -http");
      choose_option(L"режим");
      Check(!IsWindowEnabled(GetDlgItem(builder, 1808)), "Ambiguous transports cannot be overwritten by form");
      Check(Text(GetDlgItem(builder, 1809)) == L"/debug -tcp -http", "Ambiguous draft is preserved");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/CompareCfg";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_option(L"-FirstConfigurationType <значение>");
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 8, "Configuration type has a finite choice editor");
      choose_option(L"-FirstName <значение>");
      Check((GetWindowLongPtrW(GetDlgItem(builder, 1805), GWL_STYLE) & ES_AUTOHSCROLL) != 0,
          "Configuration name remains a free-form field");
      for (const auto key : {L"/AppAutoCheckVersion", L"/AppAutoInstallLastVersion"}) {
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 2, "Starter mode has two signed choices");
        choose_value(L"-");
        SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
        Check(Text(GetDlgItem(builder, 1809)).find(std::wstring(key) + L"-") != std::wstring::npos, "Starter mode uses attached sign");
      }
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/AppAutoCheckVersion";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_version(L"8.3.6");
      Check(!IsWindowVisible(GetDlgItem(builder, 1805)), "Earliest automatic version command has no documented sign");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).ends_with(L"/AppAutoCheckVersion"), "Earliest command inserts without a sign");
      choose_version(L"8.3.8");
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 2, "Version choice appears only when documented");
      choose_version(L"8.5.1");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/ConfigurationRepositoryCreate";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_option(L"-NoBind");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-MinPasswordLenght <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"12");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"/ConfigurationRepositoryCreate -NoBind -MinPasswordLenght 12") != std::wstring::npos,
          "Several nested parameters belong to one command and are not quoted as one operand");
      choose_option(L"-CheckPasswordComplexity");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-Extension <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"Synthetic extension");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-ChangesAllowedRule <значение>");
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 3, "Three support rules selectable");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Incomplete dependent support options cannot be applied");
      choose_option(L"-AllowConfigurationChanges");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-ChangesNotRecommendedRule <значение>");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Complete support option group can be applied");
      Check(Text(GetDlgItem(builder, 1809)).find(L"-Extension \"Synthetic extension\"") != std::wstring::npos,
          "Extension value stays one quoted operand among all seven options");
      choose_option(L"-NoBind");
      Check(Text(GetDlgItem(builder, 1808)) == L"Параметр добавлен" && !IsWindowEnabled(GetDlgItem(builder, 1808)) &&
          IsWindowEnabled(GetDlgItem(builder, 1814)), "Existing nested flag offers removal without a no-op change action");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"-NoBind") == std::wstring::npos, "Removing one nested flag preserves the other flags");
      Check(Text(GetDlgItem(builder, 1809)).find(L"-MinPasswordLenght 12") != std::wstring::npos, "Sibling parameter retained");
      find.psz = L"/SetPredefinedDataUpdate";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(SendMessageW(GetDlgItem(builder, 1805), CB_GETCOUNT, 0, 0) == 3, "Predefined data update has three alternative modes");
      Check(Text(GetDlgItem(builder, 1804)).find(L"периферийного") != std::wstring::npos, "Default mode explains main and peripheral nodes");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"/SetPredefinedDataUpdate -Auto") != std::wstring::npos, "Predefined mode inserted by form");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/Proxy";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(Text(GetDlgItem(builder, 1813)) == L"-PSrv <значение> *", "Proxy starts with required server in documented order");
      Check(Text(GetDlgItem(builder, 1804)).find(L"Обязательный параметр.") != std::wstring::npos, "Required option is explained in command help");
      choose_option(L"-PPwd <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"synthetic-password");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Proxy without required server and port cannot be applied");
      const auto required_user = SendMessageW(GetDlgItem(builder, 1813), CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
          reinterpret_cast<LPARAM>(L"-PUser <значение> *"));
      Check(required_user >= 0, "Password activates required user marker in selector");
      choose_option(L"-PPort <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"30");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-PUser <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"synthetic-user");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      choose_option(L"-PSrv <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"http://proxy");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Complete proxy can be applied");
      Check(Text(GetDlgItem(builder, 1809)) == initial + L"/Proxy -PSrv http://proxy -PPort 30 -PUser synthetic-user -PPwd synthetic-password",
          "Real form orders proxy values regardless of selection order and preserves surrounding text");
      choose_option(L"-PUser <значение>");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Removing the user while keeping password exposes dependency");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/LoadConfigFromFiles";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(Text(GetDlgItem(builder, 1813)) == L"каталог загрузки *", "Primary operand uses its documented name and required marker");
      Check(ChildWithText(builder, L"каталог загрузки:") != nullptr, "Value field uses primary operand name");
      Check(Text(GetDlgItem(builder, 1804)).find(L"Частичная загрузка: автоматического распознавания нет") != std::wstring::npos,
          "Load help explains full and partial format detection");
      SetWindowTextW(GetDlgItem(builder, 1805), L"\"C:\\synthetic dir\"");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).find(L"/LoadConfigFromFiles \"C:\\synthetic dir\"") != std::wstring::npos,
          "Renamed primary operand still inserts quoted directory correctly");
      choose_option(L"-files <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"one.xml");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Partial load requires explicit format");
      choose_option(L"-Format <значение>");
      Check(Text(GetDlgItem(builder, 1813)) == L"-Format <значение> *", "Partial load activates format required marker");
      choose_value(L"Hierarchical");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Partial load with explicit format can be applied");
      choose_option(L"-NoCheck");
      CheckHelpHeadingVisible(GetDlgItem(builder, 1804), L"• -NoCheck");
      Capture(GetDlgItem(builder, 1804), L"no-check-heading.bmp");
      choose_option(L"-updateConfigDumpInfo");
      CheckHelpHeadingVisible(GetDlgItem(builder, 1804), L"• -updateConfigDumpInfo");
      choose_option(L"-NoCheck");
      CheckHelpHeadingVisible(GetDlgItem(builder, 1804), L"• -NoCheck");
      choose_option(L"-Archive <значение>");
      SetWindowTextW(GetDlgItem(builder, 1805), L"archive.zip");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Archive and directory cannot be combined");
      choose_option(L"каталог загрузки");
      Check(Text(GetDlgItem(builder, 1813)) == L"каталог загрузки", "Directory marker reflects archive alternative");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(IsWindowEnabled(GetDlgItem(builder, IDOK)), "Archive can replace named directory operand");
      find.psz = L"/CreateTemplateListFile";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(Text(GetDlgItem(builder, 1813)) == L"имя файла", "Default template list name is optional despite header notation");
      find.psz = L"/CreateDistributivePackage";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      choose_option(L"-PackageFileName <значение>");
      Check(Text(GetDlgItem(builder, 1813)) == L"-PackageFileName <значение>", "Default package archive name is optional");
      find.psz = L"/LoadConfigFiles";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      const HWND help = GetDlgItem(builder, 1804);
      wchar_t help_class[32]{};
      GetClassNameW(help, help_class, 32);
      Check(std::wstring_view(help_class) == MSFTEDIT_CLASS, "Description uses the system Unicode Rich Edit control");
      Check((FormatAt(help, L"/LoadConfigFiles").dwEffects & CFE_BOLD) != 0, "Command name is bold");
      const auto option_format = FormatAt(help, L"-Module");
      HIGHCONTRASTW contrast{sizeof(contrast)};
      const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
          (contrast.dwFlags & HCF_HIGHCONTRASTON);
      Check(high_contrast ? option_format.crTextColor == GetSysColor(COLOR_WINDOWTEXT) && (option_format.dwEffects & CFE_BOLD) :
          option_format.crTextColor != GetSysColor(COLOR_WINDOWTEXT), "Nested parameter uses an accessible accent");
      Check(FormatAt(help, L"<каталог загрузки>").crTextColor == option_format.crTextColor, "Operand placeholder uses parameter color");
      const auto help_text = RichText(help);
      const auto paragraph = help_text.find(L"\r\r");
      Check(paragraph != std::wstring::npos, "Formatted help preserves paragraphs");
      Check(FormatAt(help, L"-AllWritable", paragraph).crTextColor == option_format.crTextColor,
          "Formatting uses correct character positions after Cyrillic text and paragraph breaks");
      Check((FormatAt(help, L"Загружает").dwEffects & CFE_BOLD) == 0, "Explanatory prose stays regular");
      SendMessageW(help, EM_LINESCROLL, 0, 5);
      CHARRANGE saved_selection{};
      SendMessageW(help, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved_selection));
      POINT saved_scroll{}, current_scroll{};
      SendMessageW(help, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&saved_scroll));
      Check(saved_scroll.y > 0, "Formatting test uses a scrolled description");
      SetFocus(GetDlgItem(builder, 1800));
      SendMessageW(builder, WM_SYSCOLORCHANGE, 0, 0);
      CHARRANGE current_selection{};
      SendMessageW(help, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&current_selection));
      SendMessageW(help, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&current_scroll));
      Check(saved_selection.cpMin == current_selection.cpMin && saved_selection.cpMax == current_selection.cpMax,
          "Reformatting preserves text selection");
      Check(saved_scroll.x == current_scroll.x && saved_scroll.y == current_scroll.y, "Reformatting preserves scroll position");
      Check(GetFocus() == GetDlgItem(builder, 1800), "Reformatting does not take keyboard focus");
      SendMessageW(help, WM_CHAR, L'x', 0);
      Check(RichText(help) == help_text, "Description remains read-only and formatting preserves all Unicode text");
      FormatAt(help, L"-Module");
      wchar_t selected_text[64]{};
      SendMessageW(help, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(selected_text));
      Check(std::wstring_view(selected_text) == L"-Module", "Selection exposes plain command text for copying");
      SendMessageW(help, EM_SETSEL, 0, 0);
      SendMessageW(help, WM_VSCROLL, SB_TOP, 0);
      if (!capture_directory.empty()) {
        SetWindowTextW(GetDlgItem(builder, 1809), L"/LoadConfigFiles \"C:\\Synthetic source\" -Module -Template");
        SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1809, EN_CHANGE), reinterpret_cast<LPARAM>(GetDlgItem(builder, 1809)));
        SetWindowTextW(GetDlgItem(builder, 1800), L"/LoadConfigFiles");
      }
      Capture(builder, L"load-config-files.bmp");
      Capture(help, L"description.bmp");
      SetWindowTextW(GetDlgItem(builder, 1800), L"");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      find.psz = L"/ConfigurationRepositoryCreate";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      SetWindowTextW(GetDlgItem(builder, 1800), L"-MinPasswordLenght");
      Check(ListView_GetItemCount(list) == 1, "Search also finds nested options in the versioned command syntax");
      SetWindowTextW(GetDlgItem(builder, 1800), L"OidcSelectedProvider");
      find.psz = L"Web: query";
      Check(ListView_GetItemCount(list) == 1 && ListView_FindItem(list, -1, &find) >= 0,
          "Search finds fields inside full versioned reference help");
      Check((FormatAt(GetDlgItem(builder, 1804), L"Web: query").dwEffects & CFE_BOLD) != 0,
          "Section heading is bold");
      SetWindowTextW(GetDlgItem(builder, 1800), L"__missing_help_command__");
      Check(ListView_GetItemCount(list) == 0, "No-result search clears the reference");
      const auto empty_format = FormatAt(GetDlgItem(builder, 1804), L"По вашему запросу");
      Check(!(empty_format.dwEffects & CFE_BOLD) && empty_format.crTextColor == GetSysColor(COLOR_WINDOWTEXT),
          "No-result message resets previous heading and accent formatting");
      SetWindowTextW(GetDlgItem(builder, 1800), L"");
      Check(Text(GetDlgItem(builder, 1809)) == initial, "Changing version preserves input");
      RECT old{};
      GetWindowRect(builder, &old);
      const UINT dpi = GetDpiForWindow(builder);
      SetWindowPos(builder, nullptr, 0, 0, MulDiv(780, static_cast<int>(dpi), 96),
          MulDiv(610, static_cast<int>(dpi), 96), SWP_NOMOVE | SWP_NOZORDER);
      RECT client{};
      GetClientRect(builder, &client);
      CheckActionRow(builder);
      for (HWND child = GetWindow(builder, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!IsWindowVisible(child)) continue;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, builder, reinterpret_cast<POINT*>(&bounds), 2);
        Check(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
            "Controls stay within resized client area");
      }
      // Exercise the anchor with a heading that physically wraps, without
      // changing the user's DPI or accessibility settings.
      SetWindowPos(builder, nullptr, 0, 0, MulDiv(780, static_cast<int>(dpi), 96),
          old.bottom - old.top, SWP_NOMOVE | SWP_NOZORDER);
      find.psz = L"/IBCheckAndRepair";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      const auto original_font = reinterpret_cast<HFONT>(SendMessageW(help, WM_GETFONT, 0, 0));
      LOGFONTW large_font{};
      GetObjectW(original_font, sizeof(large_font), &large_font);
      large_font.lfHeight = -MulDiv(28, static_cast<int>(dpi), 72);
      const HFONT wrapping_font = CreateFontIndirectW(&large_font);
      Check(wrapping_font != nullptr, "Create larger description font for wrapping regression");
      SendMessageW(help, WM_SETFONT, reinterpret_cast<WPARAM>(wrapping_font), TRUE);
      choose_option(L"-ConfigurationExtensionsLogIntegrity");
      CheckHelpHeadingVisible(help, L"• -ConfigurationExtensionsLogIntegrity", true);
      Capture(help, L"wrapped-heading.bmp");
      SendMessageW(help, WM_SETFONT, reinterpret_cast<WPARAM>(original_font), TRUE);
      if (wrapping_font) DeleteObject(wrapping_font);
      SendMessageW(builder, WM_SYSCOLORCHANGE, 0, 0);
      SetWindowPos(builder, nullptr, 0, 0, old.right - old.left, old.bottom - old.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (launch || database) {
      LVFINDINFOW find{};
      find.flags = LVFI_STRING;
      for (const auto key : {L"/F", L"/S", L"/WS", L"/IBName", L"/IBConnectionString"}) {
        find.psz = key;
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(!IsWindowEnabled(GetDlgItem(builder, 1808)) && !IsWindowEnabled(GetDlgItem(builder, 1805)), "Additional parameters cannot override base connection");
        Check(Text(GetDlgItem(builder, 1804)).find(L"Подключение задаётся в настройках базы") != std::wstring::npos, "Connection restriction explains its reason");
      }
      SetWindowTextW(GetDlgItem(builder, 1809), L"/F \"C:\\Synthetic base\"");
      SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1809, EN_CHANGE), reinterpret_cast<LPARAM>(GetDlgItem(builder, 1809)));
      Check(!IsWindowEnabled(GetDlgItem(builder, IDOK)), "Manually entered connection cannot be applied to additional parameters");
      find.psz = L"/F";
      ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      Check(!IsWindowEnabled(GetDlgItem(builder, 1808)) && IsWindowEnabled(GetDlgItem(builder, 1814)),
          "Restricted connection can be removed to repair a draft without allowing modification");
      SendMessageW(GetDlgItem(builder, 1814), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)).empty() && IsWindowEnabled(GetDlgItem(builder, IDOK)),
          "Removing a restricted connection restores a valid additional-parameter draft");
      SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
      SendMessageW(builder, WM_COMMAND, MAKEWPARAM(1809, EN_CHANGE), reinterpret_cast<LPARAM>(GetDlgItem(builder, 1809)));
      CheckDirectActions(builder, list);
      if (launch) {
        find.psz = L"/ConfigurationRepositoryCreate";
        ListView_SetItemState(list, ListView_FindItem(list, -1, &find), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        Check(!IsWindowEnabled(GetDlgItem(builder, 1808)), "Designer nested command cannot be inserted into Enterprise launch");
        Check(Text(GetDlgItem(builder, 1804)).find(L"не соответствует выбранному режиму") != std::wstring::npos,
            "Designer command explains incompatible launch mode");
        Check(Text(GetDlgItem(builder, 1809)) == initial, "Selecting incompatible command preserves original parameters");
      }
      find.psz = L"/L";
      const int row = ListView_FindItem(list, -1, &find);
      Check(row >= 0, "Language command in catalog");
      ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      SetWindowTextW(GetDlgItem(builder, 1805), L"en");
      SendMessageW(GetDlgItem(builder, 1808), BM_CLICK, 0, 0);
      Check(Text(GetDlgItem(builder, 1809)) == L" /L en\t/C \"synthetic input\"  ", "Structured edit preserves surrounding text");
      if (launch) Check(!IsWindowEnabled(GetDlgItem(builder, 1802)), "Launch profile fixed to selected installation");
    } else if (driver.scenario == Scenario::empty) SetWindowTextW(GetDlgItem(builder, 1809), L"");
    else if (driver.scenario == Scenario::repair) SetWindowTextW(GetDlgItem(builder, 1809), initial.c_str());
    else if (driver.scenario == Scenario::cancel) SetWindowTextW(GetDlgItem(builder, 1809), L"/L de");
    if (driver.scenario == Scenario::close) SendMessageW(builder, WM_CLOSE, 0, 0);
    else SendMessageW(builder, WM_COMMAND, driver.scenario == Scenario::cancel ? IDCANCEL : IDOK, 0);
    return;
  }
  if (launch && driver.phase == 2) {
    const HWND parent = ThreadWindow(L"IBStart.LaunchOptions");
    if (!parent) return;
    ++driver.phase;
    Check(ChildWithText(parent, L" /L en\t/C \"synthetic input\"  ") != nullptr,
        "Builder result returned to one-off field");
    SendMessageW(parent, WM_COMMAND, IDOK, 0);
    return;
  }
  if (database && driver.phase == 3) {
    const HWND advanced = ThreadWindow(L"IBStart.AdvancedDatabaseOptions");
    if (!advanced) return;
    ++driver.phase;
    Check(ChildWithText(advanced, L" /L en\t/C \"synthetic input\"  ") != nullptr,
        "Builder result returned to persistent field");
    SendMessageW(advanced, WM_COMMAND, IDOK, 0);
    return;
  }
  if (database && driver.phase == 4) {
    const HWND parent = ThreadWindow(L"IBStart.DatabaseEditor");
    if (!parent) return;
    ++driver.phase;
    SendMessageW(parent, WM_COMMAND, driver.scenario == Scenario::database_cancel ? IDCANCEL : IDOK, 0);
  }
}
void Start(Scenario scenario) {
  driver = {scenario};
  driver.timer = SetTimer(nullptr, 0, 15, Drive);
}
void Stop() { KillTimer(nullptr, driver.timer); }
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--capture") {
    capture_directory = argv[2];
    std::filesystem::create_directories(capture_directory);
  }
  // UI contract tests run on a private desktop without switching the user's
  // input desktop. Human keyboard/mouse input cannot race these modal dialogs.
  const HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
  const std::wstring desktop_name = L"IBStart.ParameterTests." + std::to_wstring(GetCurrentProcessId());
  HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0,
      DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
  if (!desktop || !SetThreadDesktop(desktop)) {
    std::cerr << "Cannot create isolated UI test desktop: " << GetLastError() << '\n';
    if (desktop) CloseDesktop(desktop);
    return 1;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&controls);
  HWND owner = CreateWindowExW(0, L"STATIC", L"IBStart isolated UI contract", WS_OVERLAPPEDWINDOW,
      0, 0, 500, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(owner != nullptr, "Test owner created");
  for (const auto scenario : {Scenario::identity, Scenario::cancel, Scenario::close, Scenario::empty, Scenario::repair, Scenario::versions}) {
    Start(scenario);
    const auto result = ibstart::ui::dialog::EditLaunchParameters(owner,
        scenario == Scenario::repair ? L"/C \"broken" : initial);
    Stop();
    if (scenario == Scenario::cancel || scenario == Scenario::close) Check(!result, "Cancel or close discards draft");
    else Check(result == std::optional<std::wstring>(scenario == Scenario::empty ? L"" : initial),
        "Identity, clear or invalid-text repair returns exact string");
  }
  Start(Scenario::standalone);
  const std::vector<ibstart::domain::PlatformInstallation> older_platforms{
      {L"C:\\synthetic-platform-old\\1cv8.exe", L"8.3.9.100"},
      {L"C:\\synthetic-platform-new\\1cv8.exe", L"8.3.27.200"}};
  const auto standalone = ibstart::ui::dialog::EditLaunchParameters(owner, {}, {},
      ibstart::ui::dialog::ParameterDialogPurpose::standalone, older_platforms);
  Stop();
  Check(!standalone, "Standalone returns no result to persist");
  const std::vector<ibstart::domain::PlatformInstallation> platforms{{
      L"C:\\synthetic-platform\\1cv8.exe", L"8.5.1.100", ibstart::domain::ClientBitness::x64}};
  ibstart::domain::Database database;
  database.name = L"Synthetic UI fixture";
  database.connect = L"Srvr=\"test-server\";Ref=\"test-base\";";
  database.additional_parameters = initial;
  ibstart::domain::LaunchOptions options;
  options.individual_parameters = initial;
  options.override_individual_parameters = true;
  options.client_type = ibstart::domain::ClientType::thick;
  Start(Scenario::launch);
  const auto launch_result = ibstart::ui::dialog::EditLaunchOptions(owner, database, platforms, options);
  Stop();
  Check(launch_result && launch_result->individual_parameters == L" /L en\t/C \"synthetic input\"  ",
      "One-off editor returns modified parameters");
  Check(database.additional_parameters == initial, "One-off editing does not mutate database");
  ibstart::ui::dialog::DatabaseEditorData data;
  data.name = database.name;
  data.connect = database.connect;
  data.additional_parameters = initial;
  data.version = L"8.5.1";
  for (const auto scenario : {Scenario::database_save, Scenario::database_cancel}) {
    Start(scenario);
    const auto result = ibstart::ui::dialog::EditDatabase(owner, L"Synthetic database editor", data, platforms);
    Stop();
    if (scenario == Scenario::database_save)
      Check(result && result->additional_parameters == L" /L en\t/C \"synthetic input\"  ",
          "Persistent editor returns modified parameters after all confirmations");
    else Check(!result, "Outer cancel discards nested confirmed draft");
    Check(data.additional_parameters == initial, "Input model unchanged until caller applies result");
  }
  const auto profile_root = std::filesystem::temp_directory_path() / (L"ibstart-configuration-ui-" + std::to_wstring(GetCurrentProcessId()));
  const ibstart::storage::StorageLayout profile{profile_root, true};
  ibstart::catalog::CatalogMetadataService metadata(profile);
  metadata.SetConfigurationScope(profile_root / L"synthetic.v8i");
  const auto original_definitions = metadata.Read().configurations;
  Start(Scenario::config_manager_cancel);
  Check(!ibstart::ui::dialog::EditConfigurations(owner, metadata, true), "Closing dictionary discards unsaved entry");
  Stop();
  Check(metadata.Read().configurations == original_definitions, "Unsaved dictionary form does not mutate profile");
  Start(Scenario::config_manager);
  const auto created = ibstart::ui::dialog::EditConfigurations(owner, metadata, true);
  Stop();
  Check(created && ibstart::configuration::Find(metadata.Read().configurations, *created), "Dictionary stores and returns new entry");
  for (const auto scenario : {Scenario::config_identity, Scenario::config_save, Scenario::config_cancel}) {
    Start(scenario);
    const auto result = ibstart::ui::dialog::EditDatabase(owner, L"Configuration UI contract", data, platforms, &metadata);
    Stop();
    if (scenario == Scenario::config_cancel) Check(!result, "Cancel discards local configuration fields");
    else if (scenario == Scenario::config_identity) Check(result && *result == data, "Opening and saving metadata preserves every launch field exactly");
    else {
      Check(result && !result->configuration_id.empty() && result->configuration_version == L"manual-beta", "Base editor returns chosen configuration and manual version");
      if (result) {
        auto launch_only = *result;
        launch_only.configuration_id = data.configuration_id;
        launch_only.configuration_version = data.configuration_version;
        Check(launch_only == data, "Changing configuration preserves constructor and launch settings");
      }
    }
  }
  std::error_code cleanup_error;
  std::filesystem::remove_all(profile_root, cleanup_error);
  DestroyWindow(owner);
  if (SUCCEEDED(com)) CoUninitialize();
  SetThreadDesktop(previous_desktop);
  CloseDesktop(desktop);
  if (failures) return 1;
  std::cout << "All parameter dialog contracts passed (no client launched; isolated temporary profile)\n";
  return 0;
}
