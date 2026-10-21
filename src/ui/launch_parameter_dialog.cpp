#include "ui/launch_parameter_dialog.hpp"

#include "core/launcher/command_builder.hpp"
#include "core/launcher/parameter_help.hpp"
#include "core/launcher/parameter_options.hpp"
#include "ui/dialog_support.hpp"

#include <CommCtrl.h>
#include <Richedit.h>
#include <ShObjIdl.h>
#include <Shellapi.h>

#include <algorithm>
#include <cwctype>
#include <utility>
#include <vector>

namespace ibstart::ui::dialog {
namespace {
constexpr wchar_t kClass[] = L"IBStart.LaunchParameterBuilder";
constexpr UINT kRefreshVersion = WM_APP + 31;
enum Control : int {
  kSearch = 1800, kCategory, kVersion, kReference, kDescription, kValue,
  kBrowse, kOption, kInsert, kText, kStatus, kDocumentation, kCopy, kNestedSelector, kRemove, kAddCommand
};
struct State {
  HWND search{}, category{}, version{}, reference{}, description{}, value_label{}, value{}, browse{};
  HWND option{}, insert{}, text{}, status{}, documentation{}, command_label{}, version_label{};
  HWND search_label{}, category_label{}, text_label{}, ok{}, cancel{};
  HWND copy{};
  HWND nested_label{}, nested_selector{}, remove{}, add_command{};
  std::vector<launcher::NestedParameterDefinition> nested;
  std::wstring nested_command;
  std::wstring primary_label;
  HFONT font{};
  launcher::ParameterEditorContext context;
  std::wstring initial;
  std::optional<std::wstring> result;
  std::vector<size_t> visible;
  size_t selected{0};
  bool done{false};
  bool updating{false};
  bool version_refresh_pending{false};
  bool lock_version{false};
  bool value_choice{false};
  ParameterDialogPurpose purpose{ParameterDialogPurpose::return_to_field};
};

std::wstring Read(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(std::max(copied, 0)));
  return result;
}

bool IdentifierCharacter(wchar_t character) {
  return (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z') ||
      (character >= L'0' && character <= L'9') || character == L'_' || character == L'@';
}

bool IsRichDescription(HWND control) {
  wchar_t klass[32]{};
  GetClassNameW(control, klass, 32);
  return std::wstring_view(klass) == MSFTEDIT_CLASS;
}

std::wstring ReadDescription(HWND control) {
  if (!IsRichDescription(control)) return Read(control);
  // Rich Edit positions count CR as one character. GT_RAWTEXT keeps offsets
  // aligned even after the original CRLF paragraphs have been normalized.
  GETTEXTLENGTHEX length{GTL_PRECISE | GTL_NUMCHARS, 1200};
  const auto count = SendMessageW(control, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
  std::wstring text(static_cast<size_t>(std::max<LRESULT>(count, 0)) + 1, L'\0');
  GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_RAWTEXT, 1200, nullptr, nullptr};
  text.resize(static_cast<size_t>(SendMessageW(control, EM_GETTEXTEX,
      reinterpret_cast<WPARAM>(&get), reinterpret_cast<LPARAM>(text.data()))));
  return text;
}

void FormatDescription(State& state) {
  if (!IsRichDescription(state.description)) return;
  const auto text = ReadDescription(state.description);
  CHARRANGE selection{};
  POINT scroll{};
  SendMessageW(state.description, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
  SendMessageW(state.description, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  const bool visible = IsWindowVisible(state.description) != FALSE;
  if (visible) SendMessageW(state.description, WM_SETREDRAW, FALSE, 0);
  HIGHCONTRASTW contrast{sizeof(contrast)};
  const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
      (contrast.dwFlags & HCF_HIGHCONTRASTON);
  const COLORREF background = GetSysColor(COLOR_WINDOW), foreground = GetSysColor(COLOR_WINDOWTEXT);
  const bool dark = (GetRValue(background) * 299 + GetGValue(background) * 587 + GetBValue(background) * 114) < 128000;
  const COLORREF accent = high_contrast ? foreground : dark ? RGB(255, 144, 144) : RGB(160, 32, 32);
  SendMessageW(state.description, EM_SETBKGNDCOLOR, 0, background);
  CHARFORMAT2W base{};
  base.cbSize = sizeof(base);
  base.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_COLOR;
  base.crTextColor = foreground;
  SendMessageW(state.description, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&base));
  const auto format = [&](size_t start, size_t end, bool command) {
    CHARRANGE range{static_cast<LONG>(start), static_cast<LONG>(end)};
    SendMessageW(state.description, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
    CHARFORMAT2W style{};
    style.cbSize = sizeof(style);
    style.dwMask = command ? CFM_BOLD : CFM_COLOR | CFM_BOLD;
    style.dwEffects = command || high_contrast ? CFE_BOLD : 0;
    style.crTextColor = accent;
    SendMessageW(state.description, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&style));
  };
  const auto catalog = launcher::LaunchParameterCatalog();
  if (state.selected < catalog.size()) {
    // Section headings have no slash; ordinary commands are handled below.
    if (!text.empty() && text.front() != L'/') {
      const auto end = text.find_first_of(L" <\r\n");
      const auto key = catalog[state.selected].key;
      format(0, text.starts_with(key) ? key.size() : end == std::wstring::npos ? text.size() : end, true);
    }
    for (size_t i = 0; i < text.size();) {
      if (text[i] == L'<') {
        const auto end = text.find(L'>', i + 1);
        if (end != std::wstring::npos) { format(i, end + 1, false); i = end + 1; continue; }
      }
      const bool boundary = i == 0 || (!IdentifierCharacter(text[i - 1]) &&
          text[i - 1] != L'/' && text[i - 1] != L'\\' && text[i - 1] != L':');
      if (boundary && (text[i] == L'/' || text[i] == L'-') && i + 1 < text.size() && IdentifierCharacter(text[i + 1])) {
        size_t end = i + 1;
        while (end < text.size() && IdentifierCharacter(text[end])) ++end;
        if (text[i] == L'-') format(i, end, false);
        else {
          const auto token = std::wstring_view(text).substr(i, end - i);
          if (std::any_of(catalog.begin(), catalog.end(), [&](const auto& definition) {
            return token.size() == definition.key.size() && std::equal(token.begin(), token.end(), definition.key.begin(),
                [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
          })) format(i, end, true);
        }
        i = end;
        continue;
      }
      // Named fields/values in the connection, web and enum sections.
      if (text[i] == L'•' && i + 2 < text.size() && text[i + 1] == L' ' && IdentifierCharacter(text[i + 2])) {
        const auto end = text.find_first_of(L" \r\n", i + 2);
        format(i + 2, end == std::wstring::npos ? text.size() : end, false);
      }
      ++i;
    }
  }
  SendMessageW(state.description, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
  SendMessageW(state.description, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
  if (visible) {
    SendMessageW(state.description, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.description, nullptr, FALSE);
  }
}

void SetDescription(State& state, const std::wstring& text) {
  SetWindowTextW(state.description, text.c_str());
  FormatDescription(state);
}

bool Contains(std::wstring_view text, std::wstring_view query) {
  return std::search(text.begin(), text.end(), query.begin(), query.end(),
      [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end();
}

launcher::ParameterEditorContext Context(const State& state) {
  auto context = state.context;
  auto text = Read(state.version);
  text.resize(text.find(L' ') == std::wstring::npos ? text.size() : text.find(L' '));
  context.version = text;
  return context;
}

bool IsSimpleFlag(const launcher::ParameterDefinition& def, std::wstring_view version) {
  return launcher::ParameterEditorDefinition(def, version).kind == launcher::ParameterValueKind::flag;
}

void RefreshDirectActions(State& state) {
  const auto catalog = launcher::LaunchParameterCatalog();
  EnableWindow(state.add_command, FALSE);
  SetWindowTextW(state.add_command, L"Добавить команду в строку");
  if (state.selected < catalog.size()) {
    const auto& command = catalog[state.selected];
    const auto context = Context(state);
    const bool available = launcher::ParameterVersionAvailability(command.key, context.version) == launcher::ParameterAvailability::available &&
        !launcher::DocumentedParameterSyntax(command.key, context.version).empty() &&
        launcher::ParameterContextNotice(command, context).empty();
    try {
      const bool present = !launcher::FindParameterFragments(Read(state.text), command).empty();
      SetWindowTextW(state.add_command, present ? L"Команда уже в строке" : L"Добавить команду в строку");
      EnableWindow(state.add_command, available && !present);
    } catch (const std::exception&) {
      // Do not append to an incompletely quoted draft.
    }
  }
  if (state.selected >= catalog.size() || !state.nested.empty()) return;
  const auto context = Context(state);
  const auto def = launcher::ParameterEditorDefinition(catalog[state.selected], context.version);
  if (def.kind == launcher::ParameterValueKind::reference) return;
  const bool flag = IsSimpleFlag(def, context.version);
  const bool can_insert = launcher::ParameterContextNotice(def, context).empty() &&
      launcher::ParameterVersionAvailability(def.key, context.version) != launcher::ParameterAvailability::unavailable;
  try {
    const auto fragments = launcher::FindParameterFragments(Read(state.text), def);
    const bool present = !fragments.empty();
    const bool structured = fragments.size() == 1 && fragments.front().structured;
    SetWindowTextW(state.insert, present ? flag ? L"Удалить из строки" : L"Изменить в строке" : L"Добавить в строку");
    EnableWindow(state.insert, present ? structured && (flag || can_insert) : can_insert);
    ShowWindow(state.remove, !flag && present ? SW_SHOW : SW_HIDE);
    EnableWindow(state.remove, structured);
  } catch (const std::exception&) {
    EnableWindow(state.insert, FALSE);
    EnableWindow(state.remove, FALSE);
  }
}

void RefreshRequiredMarkers(State& state) {
  if (state.nested.empty()) return;
  const auto selected = SendMessageW(state.nested_selector, CB_GETCURSEL, 0, 0);
  const auto text = Read(state.text);
  const auto context = Context(state);
  try {
    for (size_t i = 0; i < state.nested.size(); ++i) {
      const auto& option = state.nested[i];
      std::wstring label(option.syntax);
      if (launcher::NestedParameterRequired(state.nested_command, option.key, context.version, text)) label += L" *";
      const auto length = SendMessageW(state.nested_selector, CB_GETLBTEXTLEN, i, 0);
      std::wstring old(static_cast<size_t>(std::max<LRESULT>(length, 0)) + 1, L'\0');
      if (length >= 0) SendMessageW(state.nested_selector, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(old.data()));
      old.resize(static_cast<size_t>(std::max<LRESULT>(length, 0)));
      if (old == label) continue;
      SendMessageW(state.nested_selector, CB_DELETESTRING, i, 0);
      SendMessageW(state.nested_selector, CB_INSERTSTRING, i, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(state.nested_selector, CB_SETCURSEL, selected, 0);
  } catch (const std::exception&) {
    // Leave labels intact while an incomplete quoted value is being typed.
  }
}

void Validate(State& state) {
  if (state.updating) return;
  RefreshRequiredMarkers(state);
  const auto errors = launcher::ValidateParameterDraft(Read(state.text), Context(state));
  const std::wstring message = errors.empty() ? state.purpose == ParameterDialogPurpose::standalone ?
      L"Параметры не сохраняются. Скопируйте строку для самостоятельного использования." :
      L"Строка возвращается только после «Применить». Неизвестные команды сохраняются." : errors.front();
  SetWindowTextW(state.status, message.c_str());
  EnableWindow(state.ok, state.purpose == ParameterDialogPurpose::standalone || errors.empty());
  RefreshDirectActions(state);
}

std::optional<std::wstring> PickOutputFile(HWND owner) {
  IFileSaveDialog* dialog{};
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&dialog)))) return std::nullopt;
  struct Release { IFileSaveDialog* value; ~Release() { value->Release(); } } release{dialog};
  if (FAILED(dialog->SetTitle(L"Файл служебных сообщений"))) return std::nullopt;
  DWORD options{};
  if (FAILED(dialog->GetOptions(&options)) ||
      FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT)) ||
      FAILED(dialog->Show(owner))) return std::nullopt;
  IShellItem* item{};
  if (FAILED(dialog->GetResult(&item))) return std::nullopt;
  PWSTR path{};
  const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
  item->Release();
  if (FAILED(result) || !path) return std::nullopt;
  std::wstring value(path);
  CoTaskMemFree(path);
  return value;
}

void ShowDefinition(State& state, bool reveal_nested = false) {
  const auto catalog = launcher::LaunchParameterCatalog();
  if (state.selected >= catalog.size()) return;
  const auto context = Context(state);
  const auto def = launcher::ParameterEditorDefinition(catalog[state.selected], context.version);
  std::wstring detail = launcher::ParameterHelpText(def, context);
  const auto notice = launcher::ParameterContextNotice(def, context);
  const auto available = launcher::ParameterVersionAvailability(def.key, context.version);
  SetDescription(state, detail);

  std::wstring previous_option;
  const auto previous_index = SendMessageW(state.nested_selector, CB_GETCURSEL, 0, 0);
  if (state.nested_command == def.key && previous_index >= 0 && static_cast<size_t>(previous_index) < state.nested.size())
    previous_option = state.nested[static_cast<size_t>(previous_index)].key;
  state.nested = def.kind == launcher::ParameterValueKind::reference ?
      launcher::ParameterNestedOptions(def.key, context.version) : std::vector<launcher::NestedParameterDefinition>{};
  state.primary_label = launcher::ParameterPrimaryOperandLabel(def.key, context.version);
  if (def.kind == launcher::ParameterValueKind::reference && !state.primary_label.empty())
    state.nested.insert(state.nested.begin(), {L"", state.primary_label, launcher::OptionStyle::space, true, 0,
        launcher::ParameterVersionProfile(context.version)});
  state.nested_command = def.key;
  SendMessageW(state.nested_selector, CB_RESETCONTENT, 0, 0);
  size_t selected_option{};
  for (size_t i = 0; i < state.nested.size(); ++i) {
    const std::wstring label(state.nested[i].syntax);
    SendMessageW(state.nested_selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (state.nested[i].key == previous_option) selected_option = i;
  }
  SendMessageW(state.nested_selector, CB_SETCURSEL, selected_option, 0);
  RefreshRequiredMarkers(state);
  const auto* nested = state.nested.empty() ? nullptr : &state.nested[selected_option];
  ShowWindow(state.nested_label, nested ? SW_SHOW : SW_HIDE);
  ShowWindow(state.nested_selector, nested ? SW_SHOW : SW_HIDE);
  ShowWindow(state.remove, nested ? SW_SHOW : SW_HIDE);
  SetWindowTextW(state.remove, nested ? L"Убрать" : L"Удалить из строки");
  const auto nested_choices = nested ? launcher::NestedParameterChoices(def.key, nested->key, context.version) : std::wstring{};
  const bool flag = nested ? nested->style == launcher::OptionStyle::flag : IsSimpleFlag(def, context.version);
  const bool choice = !flag && (nested ? !nested_choices.empty() : def.kind == launcher::ParameterValueKind::choice ||
      def.kind == launcher::ParameterValueKind::signed_switch);
  state.value_choice = choice;
  const bool editable = notice.empty() && available != launcher::ParameterAvailability::unavailable &&
      (def.kind != launcher::ParameterValueKind::reference || nested);
  if (state.value) DestroyWindow(state.value);
  state.value = CreateWindowExW(choice ? 0 : WS_EX_CLIENTEDGE, choice ? WC_COMBOBOXW : L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | (choice ? CBS_DROPDOWNLIST | WS_VSCROLL : ES_AUTOHSCROLL),
      0, 0, 0, 0, GetParent(state.text), reinterpret_cast<HMENU>(static_cast<INT_PTR>(kValue)),
      GetModuleHandleW(nullptr), nullptr);
  SetControlFont(state.value, state.font);
  if (choice) {
    const auto choices = nested ? nested_choices : launcher::ParameterChoicesForVersion(def, context.version);
    std::wstring_view options = choices;
    while (!options.empty()) {
      const size_t end = options.find(L'|');
      const std::wstring value(options.substr(0, end));
      SendMessageW(state.value, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
      if (end == std::wstring_view::npos) break;
      options.remove_prefix(end + 1);
    }
    SendMessageW(state.value, CB_SETCURSEL, 0, 0);
  }
  ShowWindow(state.value, flag ? SW_HIDE : SW_SHOW);
  ShowWindow(state.value_label, flag ? SW_HIDE : SW_SHOW);
  const std::wstring value_label = nested && nested->key.empty() ?
      state.primary_label.find(L';') == std::wstring::npos ? state.primary_label + L":" :
      L"Аргументы (пути с пробелами — в кавычках):" : L"Значение:";
  SetWindowTextW(state.value_label, value_label.c_str());
  if (nested && nested->key.empty() && !choice) {
    detail += L"\r\n\r\nЗначения с пробелами заключайте в двойные кавычки.";
    SetDescription(state, detail);
  }
  ShowWindow(state.browse, def.kind == launcher::ParameterValueKind::file ? SW_SHOW : SW_HIDE);
  ShowWindow(state.option, def.option.empty() ? SW_HIDE : SW_SHOW);
  SetWindowTextW(state.option, def.option == L"-NoTruncate" ? L"Дописывать файл (-NoTruncate)" : L"");
  SendMessageW(state.option, BM_SETCHECK, BST_UNCHECKED, 0);
  EnableWindow(state.value, editable);
  EnableWindow(state.browse, editable);
  EnableWindow(state.option, editable);
  EnableWindow(state.insert, editable);
  SetWindowTextW(state.insert, nested ? L"Добавить параметр" : L"Добавить в строку");
  EnableWindow(state.remove, FALSE);
  try {
    if (nested) {
      const auto value = launcher::ReadNestedParameter(Read(state.text), def, *nested);
      if (value) {
        if (choice) {
          const auto index = SendMessageW(state.value, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(value->c_str()));
          SendMessageW(state.value, CB_SETCURSEL, index, 0);
        } else SetWindowTextW(state.value, value->c_str());
        SetWindowTextW(state.insert, flag ? L"Параметр добавлен" : L"Изменить параметр");
        EnableWindow(state.insert, editable && !flag);
        EnableWindow(state.remove, editable);
      }
    } else {
      const auto fragments = launcher::FindParameterFragments(Read(state.text), def);
      if (fragments.size() == 1 && fragments.front().structured) {
        const auto& item = fragments.front();
        if (choice) {
          const LRESULT index = SendMessageW(state.value, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
              reinterpret_cast<LPARAM>(item.value.c_str()));
          SendMessageW(state.value, CB_SETCURSEL, index, 0);
        } else SetWindowTextW(state.value, item.value.c_str());
        SendMessageW(state.option, BM_SETCHECK, item.option ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(state.insert, L"Изменить в строке");
      } else if (!fragments.empty()) {
        EnableWindow(state.insert, FALSE);
        detail += L"\r\n\r\nВ строке есть повторения или сложные опции. Отредактируйте этот фрагмент вручную.";
        SetDescription(state, detail);
      }
    }
  } catch (const std::exception&) {
    EnableWindow(state.insert, FALSE);
    detail += L"\r\n\r\nЭтот фрагмент нельзя однозначно изменить через поля. Отредактируйте его в итоговой строке вручную; исходный текст сохранён.";
    SetDescription(state, detail);
  }
  RefreshDirectActions(state);
  // Keep the replacement editor in the intended keyboard navigation order.
  SetWindowPos(state.value, state.value_label, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  SendMessageW(GetParent(state.text), WM_SIZE, 0, 0);
  if (reveal_nested && nested && !nested->key.empty()) {
    // Search the displayed text, using the same character offsets as the
    // control. Original CRLF offsets drift past headings in Rich Edit.
    const auto displayed = ReadDescription(state.description);
    const auto position = displayed.find(L"• " + std::wstring(nested->syntax) + L"\r");
    if (position != std::wstring::npos) {
      const auto line = IsRichDescription(state.description) ?
          SendMessageW(state.description, EM_EXLINEFROMCHAR, 0, static_cast<LPARAM>(position)) :
          SendMessageW(state.description, EM_LINEFROMCHAR, position, 0);
      const auto first = SendMessageW(state.description, EM_GETFIRSTVISIBLELINE, 0, 0);
      SendMessageW(state.description, EM_LINESCROLL, 0, line - first);
    }
  }
}

void Populate(State& state) {
  const auto query = Read(state.search);
  const auto category = Read(state.category);
  const auto context = Context(state);
  const auto catalog = launcher::LaunchParameterCatalog();
  state.updating = true;
  ListView_DeleteAllItems(state.reference);
  state.visible.clear();
  for (size_t i = 0; i < catalog.size(); ++i) {
    const auto& def = catalog[i];
    if (category != L"Все категории" && category != def.category) continue;
    const auto syntax = launcher::DocumentedParameterSyntax(def.key, context.version);
    if (!query.empty() && !Contains(def.key, query) && !Contains(def.description, query) &&
        !Contains(def.syntax, query) && !Contains(syntax, query) &&
        !Contains(launcher::BuiltinParameterHelp(def, context.version), query)) continue;
    const int row = static_cast<int>(state.visible.size());
    state.visible.push_back(i);
    std::wstring key(def.key), description(def.description);
    if (launcher::ParameterVersionAvailability(def.key, context.version) == launcher::ParameterAvailability::unavailable)
      description = L"[Нет в справочнике версии] " + description;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = key.data();
    ListView_InsertItem(state.reference, &item);
    ListView_SetItemText(state.reference, row, 1, description.data());
  }
  if (!state.visible.empty()) {
    const auto previous = std::find(state.visible.begin(), state.visible.end(), state.selected);
    const int row = previous == state.visible.end() ? 0 : static_cast<int>(previous - state.visible.begin());
    state.selected = state.visible[static_cast<size_t>(row)];
    ListView_SetItemState(state.reference, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    state.updating = false;
    ShowDefinition(state);
  } else {
    state.updating = false;
    state.selected = catalog.size();
    SetDescription(state, L"По вашему запросу команды не найдены.");
    EnableWindow(state.insert, FALSE);
    EnableWindow(state.add_command, FALSE);
    ShowWindow(state.remove, SW_HIDE);
    EnableWindow(state.remove, FALSE);
  }
}

void Layout(HWND window, State& state) {
  RECT rect{};
  GetClientRect(window, &rect);
  const UINT dpi = GetDpiForWindow(window);
  const auto px = [dpi](int value) { return ScaleForDpi(value, dpi); };
  const int width = MulDiv(rect.right, 96, static_cast<int>(dpi));
  const int height = MulDiv(rect.bottom, 96, static_cast<int>(dpi));
  const auto move = [&](HWND control, int x, int y, int w, int h) {
    RECT current{};
    GetWindowRect(control, &current);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&current), 2);
    const int cx = px(std::max(w, 1)), cy = px(std::max(h, 1));
    if (current.left == px(x) && current.top == px(y) && current.right - current.left == cx &&
        current.bottom - current.top == cy) return;
    // Resizing an editable Win32 combo can complete a prefix to its matching
    // list item. Preserve the edit buffer and selection during layout too.
    const auto version_text = control == state.version ? Read(control) : std::wstring{};
    const auto selection = control == state.version ? SendMessageW(control, CB_GETEDITSEL, 0, 0) : 0;
    MoveWindow(control, px(x), px(y), cx, cy, TRUE);
    if (control == state.version) {
      if (Read(control) != version_text) SetWindowTextW(control, version_text.c_str());
      SendMessageW(control, CB_SETEDITSEL, 0, selection);
    }
  };
  const int left = (width - 48) * 48 / 100;
  const int right_x = left + 30;
  const int right_width = width - right_x - 16;
  const int raw_y = height - 198;
  const int middle_height = std::max(110, raw_y - 100);
  move(state.search_label, 16, 12, 300, 20);
  move(state.search, 16, 34, left, 25);
  move(state.category_label, right_x, 12, 180, 20);
  move(state.category, right_x, 34, right_width, 250);
  move(state.command_label, 16, 72, left, 20);
  move(state.reference, 16, 96, left, middle_height - 36);
  move(state.add_command, 16, 96 + middle_height - 28, left, 28);
  ListView_SetColumnWidth(state.reference, 0, px(165));
  ListView_SetColumnWidth(state.reference, 1, std::max(px(150), px(left - 170)));
  move(state.version_label, right_x, 72, 150, 20);
  move(state.version, right_x + 152, 68, right_width - 152, 250);
  const int edit_y = raw_y - 95;
  move(state.description, right_x, 102, right_width, std::max(50, edit_y - (state.nested.empty() ? 126 : 180)));
  move(state.nested_label, right_x, edit_y - 76, right_width, 20);
  move(state.nested_selector, right_x, edit_y - 54, right_width, 240);
  move(state.value_label, right_x, edit_y - 20, right_width, 18);
  const bool file = state.selected < launcher::LaunchParameterCatalog().size() &&
      launcher::LaunchParameterCatalog()[state.selected].kind == launcher::ParameterValueKind::file;
  move(state.value, right_x, edit_y, right_width - (file ? 44 : 0),
      state.selected < launcher::LaunchParameterCatalog().size() &&
      (state.value_choice || launcher::LaunchParameterCatalog()[state.selected].kind == launcher::ParameterValueKind::choice ||
       launcher::LaunchParameterCatalog()[state.selected].kind == launcher::ParameterValueKind::signed_switch) ? 220 : 25);
  move(state.browse, width - 54, edit_y, 38, 25);
  move(state.option, right_x, edit_y + 30, right_width, 22);
  move(state.insert, right_x, edit_y + 58, 180, 28);
  move(state.remove, right_x + 188, edit_y + 58, right_width - 188, 28);
  move(state.text_label, 16, raw_y, width - 32, 20);
  move(state.text, 16, raw_y + 24, width - 32, 84);
  move(state.status, 16, height - 84, width - 32, 35);
  move(state.documentation, 16, height - 40, 180, 28);
  move(state.copy, 208, height - 40, 152, 28);
  move(state.ok, width - (state.purpose == ParameterDialogPurpose::standalone ? 126 : 242), height - 40, 110, 28);
  move(state.cancel, width - 122, height - 40, 106, 28);
}

void CreateControls(HWND window, State& state) {
  const auto create = [&](DWORD ex, const wchar_t* klass, const wchar_t* label, DWORD style, int id) {
    HWND control = CreateWindowExW(ex, klass, label, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, state.font);
    return control;
  };
  state.search_label = create(0, L"STATIC", L"Поиск по команде и описанию", 0, 0);
  state.search = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, kSearch);
  state.category_label = create(0, L"STATIC", L"Категория", 0, 0);
  state.category = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kCategory);
  SendMessageW(state.category, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все категории"));
  std::vector<std::wstring_view> categories;
  for (const auto& def : launcher::LaunchParameterCatalog()) {
    if (std::find(categories.begin(), categories.end(), def.category) != categories.end()) continue;
    categories.push_back(def.category);
    SendMessageW(state.category, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::wstring(def.category).c_str()));
  }
  SendMessageW(state.category, CB_SETCURSEL, 0, 0);
  state.command_label = create(0, L"STATIC", L"Команды и описания", 0, 0);
  state.reference = create(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kReference);
  ListView_SetExtendedListViewStyle(state.reference, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH;
  column.cx = 160;
  wchar_t command[] = L"Команда", description[] = L"Описание";
  column.pszText = command;
  ListView_InsertColumn(state.reference, 0, &column);
  column.pszText = description;
  column.cx = 300;
  ListView_InsertColumn(state.reference, 1, &column);
  state.version_label = create(0, L"STATIC", L"Версия справочника:", 0, 0);
  state.version = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, kVersion);
  for (const auto& version : launcher::LaunchParameterVersions()) {
    const auto label = version == L"8.5.4" ? version + L" (тестовая)" : version;
    SendMessageW(state.version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  auto initial_version = launcher::ParameterVersionFamily(state.context.version);
  if (initial_version.empty()) initial_version = state.context.version;
  SetWindowTextW(state.version, (initial_version == L"8.5.4" ? initial_version + L" (тестовая)" : initial_version).c_str());
  if (state.lock_version) EnableWindow(state.version, FALSE);
  // Keep the system control module registered for all builder instances.
  static const HMODULE rich_edit = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  state.description = create(WS_EX_CLIENTEDGE, rich_edit ? MSFTEDIT_CLASS : L"EDIT", L"",
      WS_TABSTOP | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, kDescription);
  SendMessageW(state.description, rich_edit ? EM_EXLIMITTEXT : EM_SETLIMITTEXT,
      rich_edit ? 0 : 1024 * 1024, rich_edit ? 1024 * 1024 : 0);
  state.nested_label = create(0, L"STATIC", L"Параметр команды (* — обязателен):", 0, 0);
  state.nested_selector = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kNestedSelector);
  state.remove = create(0, L"BUTTON", L"Убрать", WS_TABSTOP, kRemove);
  state.value_label = create(0, L"STATIC", L"Значение:", 0, 0);
  state.browse = create(0, L"BUTTON", L"…", WS_TABSTOP, kBrowse);
  state.option = create(0, L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, kOption);
  state.insert = create(0, L"BUTTON", L"Добавить в строку", WS_TABSTOP, kInsert);
  state.add_command = create(0, L"BUTTON", L"Добавить команду в строку", WS_TABSTOP, kAddCommand);
  state.text_label = create(0, L"STATIC", L"Итоговая строка параметров (можно редактировать вручную)", 0, 0);
  state.text = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.c_str(),
      WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, kText);
  SendMessageW(state.text, EM_SETLIMITTEXT, 32767, 0);
  state.status = create(0, L"STATIC", L"", 0, kStatus);
  state.documentation = create(0, L"BUTTON", L"Документация ИТС", WS_TABSTOP, kDocumentation);
  state.copy = create(0, L"BUTTON", L"Копировать строку", WS_TABSTOP, kCopy);
  state.ok = create(0, L"BUTTON", state.purpose == ParameterDialogPurpose::standalone ? L"Закрыть" : L"Применить",
      WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
  state.cancel = create(0, L"BUTTON", L"Отмена", WS_TABSTOP, IDCANCEL);
  if (state.purpose == ParameterDialogPurpose::standalone) ShowWindow(state.cancel, SW_HIDE);
  Populate(state);
  Validate(state);
  Layout(window, state);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }
  if (!state) return DefWindowProcW(window, message, wparam, lparam);
  if ((message == WM_SYSCOLORCHANGE || message == WM_SETTINGCHANGE) && state->description) {
    FormatDescription(*state);
  }
  if (message == kRefreshVersion) {
    state->version_refresh_pending = false;
    Populate(*state);
    Validate(*state);
    return 0;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) return DialogControlColor(message, wparam, lparam);
  if (message == WM_SIZE && state->text) { Layout(window, *state); return 0; }
  if (message == WM_GETMINMAXINFO) {
    const auto minimum = DialogOuterSize(window, 740, 560, WS_OVERLAPPEDWINDOW, WS_EX_DLGMODALFRAME);
    auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
    info->ptMinTrackSize = {minimum.cx, minimum.cy};
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
      info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
      info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    return 0;
  }
  if (message == WM_DPICHANGED) {
    const auto* bounds = reinterpret_cast<RECT*>(lparam);
    SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
        bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
    const HFONT old = state->font;
    state->font = CreateUiFont(window, 9, FW_NORMAL);
    for (HWND control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) SetControlFont(control, state->font);
    FormatDescription(*state);
    if (old) DeleteObject(old);
    Layout(window, *state);
    return 0;
  }
  if (message == WM_NOTIFY && !state->updating) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header->idFrom == kReference && header->code == LVN_ITEMCHANGED) {
      const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
      if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) &&
          change->iItem >= 0 && static_cast<size_t>(change->iItem) < state->visible.size()) {
        state->selected = state->visible[static_cast<size_t>(change->iItem)];
        ShowDefinition(*state);
      }
    }
    if (header->idFrom == kReference && header->code == LVN_GETINFOTIPW) {
      auto* info = reinterpret_cast<NMLVGETINFOTIPW*>(lparam);
      if (info->iItem >= 0 && static_cast<size_t>(info->iItem) < state->visible.size()) {
        const auto& def = launcher::LaunchParameterCatalog()[state->visible[static_cast<size_t>(info->iItem)]];
        const auto syntax = launcher::DocumentedParameterSyntax(def.key, Context(*state).version);
        const std::wstring tip = std::wstring(syntax.empty() ? def.syntax : syntax) + L"\n" + std::wstring(def.description);
        wcsncpy_s(info->pszText, static_cast<size_t>(info->cchTextMax), tip.c_str(), _TRUNCATE);
      }
    }
    return 0;
  }
  if (message == WM_COMMAND) {
    const int command = LOWORD(wparam), event = HIWORD(wparam);
    if (command == kSearch && event == EN_CHANGE) { Populate(*state); return 0; }
    if (command == kCategory && event == CBN_SELCHANGE) { Populate(*state); return 0; }
    if (command == kNestedSelector && event == CBN_SELCHANGE) { ShowDefinition(*state, true); return 0; }
    if (command == kVersion && (event == CBN_SELCHANGE || event == CBN_EDITCHANGE)) {
      // Let the combo finish processing a keystroke/list selection before
      // refreshing the reference. Never write a matched list item into its
      // edit control: doing so expands prefixes and resets the caret.
      if (!state->version_refresh_pending) {
        state->version_refresh_pending = true;
        PostMessageW(window, kRefreshVersion, 0, 0);
      }
      return 0;
    }
    if (command == kText && event == EN_CHANGE) { Validate(*state); return 0; }
    if (command == kBrowse) {
      if (const auto path = PickOutputFile(window)) SetWindowTextW(state->value, path->c_str());
      return 0;
    }
    if (command == kAddCommand && state->selected < launcher::LaunchParameterCatalog().size()) {
      try {
        const auto& definition = launcher::LaunchParameterCatalog()[state->selected];
        const auto draft = launcher::AddLaunchParameterCommand(Read(state->text), definition, Context(*state));
        SetWindowTextW(state->text, draft.c_str());
        Validate(*state);
        ShowDefinition(*state);
      } catch (const std::exception&) {
        SetWindowTextW(state->status, L"Команду нельзя добавить в выбранном контексте или в неоднозначную строку.");
      }
      return 0;
    }
    if ((command == kInsert || command == kRemove) && state->selected < launcher::LaunchParameterCatalog().size()) {
      try {
        const auto def = launcher::ParameterEditorDefinition(
            launcher::LaunchParameterCatalog()[state->selected], Context(*state).version);
        const auto value = Read(state->value);
        const auto index = SendMessageW(state->nested_selector, CB_GETCURSEL, 0, 0);
        const auto text = Read(state->text);
        const auto updated = !state->nested.empty() && index >= 0 && static_cast<size_t>(index) < state->nested.size() ?
            launcher::SetNestedParameter(text, def, state->nested[static_cast<size_t>(index)], value, command == kInsert) :
            command == kRemove || (IsSimpleFlag(def, Context(*state).version) && !launcher::FindParameterFragments(text, def).empty()) ?
            launcher::RemoveLaunchParameter(text, def) : launcher::SetLaunchParameter(text, def, value,
                SendMessageW(state->option, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SetWindowTextW(state->text, updated.c_str());
        // WM_SETTEXT on a multiline EDIT does not send EN_CHANGE.
        Validate(*state);
        ShowDefinition(*state, !state->nested.empty());
      } catch (const std::exception&) {
        SetWindowTextW(state->status, L"Проверьте значение. Повторяющиеся и сложные фрагменты редактируются вручную.");
      }
      return 0;
    }
    if (command == kDocumentation) {
      const auto url = launcher::ParameterDocumentationUrl(Context(*state).version);
      if (!url.empty()) ShellExecuteW(window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      else SetWindowTextW(state->status, L"Выберите версию платформы для открытия документации ИТС.");
      return 0;
    }
    if (command == kCopy) {
      SetWindowTextW(state->status, CopyTextToClipboard(window, Read(state->text)) ?
          L"Строка параметров скопирована." : L"Не удалось скопировать строку. Повторите попытку.");
      return 0;
    }
    if (command == IDOK) {
      if (state->purpose == ParameterDialogPurpose::standalone) { state->done = true; return 0; }
      const auto text = Read(state->text);
      const auto errors = launcher::ValidateParameterDraft(text, Context(*state));
      if (!errors.empty()) { SetWindowTextW(state->status, errors.front().c_str()); return 0; }
      state->result = text;
      state->done = true;
      return 0;
    }
    if (command == IDCANCEL) { state->done = true; return 0; }
  }
  if (message == WM_CLOSE) { state->done = true; return 0; }
  return DefWindowProcW(window, message, wparam, lparam);
}
}  // namespace

std::optional<std::wstring> EditLaunchParameters(HWND owner, std::wstring initial,
    launcher::ParameterEditorContext context, ParameterDialogPurpose purpose,
    std::span<const domain::PlatformInstallation> installations) {
  State state;
  state.initial = std::move(initial);
  state.lock_version = context.mode.has_value() && !launcher::ParameterVersionFamily(context.version).empty();
  context.version = launcher::DefaultParameterReferenceVersion(context.version, installations);
  context.allow_connection_parameters = purpose == ParameterDialogPurpose::standalone;
  state.context = std::move(context);
  state.purpose = purpose;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kClass;
    klass.lpfnWndProc = Proc;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX;
  constexpr DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  SIZE size = DialogOuterSize(owner, 1100, 800, style, ex);
  MONITORINFO monitor{sizeof(monitor)};
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) {
    size.cx = std::min(size.cx, monitor.rcWork.right - monitor.rcWork.left);
    size.cy = std::min(size.cy, monitor.rcWork.bottom - monitor.rcWork.top);
  }
  HWND window = CreateWindowExW(ex, kClass, L"Конструктор параметров запуска", style,
      CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  CreateControls(window, state);
  PositionDialogNearOwner(window, owner);
  DisableModalOwner(owner);
  ShowWindow(window, SW_SHOW);
  SetFocus(state.search);
  MSG message{};
  int pump = 1;
  while (!state.done && (pump = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  CloseModalDialog(window, owner);
  if (pump == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (state.font) DeleteObject(state.font);
  return state.result;
}
}  // namespace ibstart::ui::dialog
