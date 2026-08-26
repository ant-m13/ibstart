#include "ui/advanced_database_options_dialog.hpp"

#include "ui/dialog_support.hpp"

#include <CommCtrl.h>

#include <algorithm>
#include <cwctype>
#include <utility>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kAdvancedDatabaseOptionsClass[] = L"IBStart.AdvancedDatabaseOptions";

void Message(HWND owner, std::wstring_view text, std::wstring_view title, UINT type) {
  MessageBoxW(owner, std::wstring(text).c_str(), std::wstring(title).c_str(), type);
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() &&
      CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
          static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring TrimText(std::wstring_view value) {
  size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) ++first;
  size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

std::wstring ReadControlText(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(length));
  return result;
}

void SetComboValue(HWND combo, std::wstring_view value) {
  const std::wstring text(value);
  LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
      reinterpret_cast<LPARAM>(text.c_str()));
  if (index == CB_ERR) index = SendMessageW(combo, CB_ADDSTRING, 0,
      reinterpret_cast<LPARAM>(text.c_str()));
  if (index != CB_ERR) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

std::wstring DefaultApplicationLabel(std::wstring_view value) {
  if (value.empty()) return L"Не задано";
  if (EqualNoCase(value, L"ThickClient")) return L"Толстый клиент";
  if (EqualNoCase(value, L"ThinClient")) return L"Тонкий клиент";
  return std::wstring(value);
}

std::wstring DefaultApplicationValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Не задано")) return {};
  if (EqualNoCase(label, L"Толстый клиент")) return L"ThickClient";
  if (EqualNoCase(label, L"Тонкий клиент")) return L"ThinClient";
  return std::wstring(label);
}

bool IsEnabledFlag(std::wstring_view value) {
  return !value.empty() && !EqualNoCase(value, L"0") && !EqualNoCase(value, L"false") &&
      !EqualNoCase(value, L"no");
}

std::wstring FlagValue(HWND checkbox, std::wstring_view previous) {
  if (SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED) {
    return previous.empty() || !IsEnabledFlag(previous) ? L"1" : std::wstring(previous);
  }
  return L"0";
}

bool IsOptionalOrder(std::wstring_view value) {
  if (value.empty()) return true;
  return std::all_of(value.begin(), value.end(),
      [](wchar_t character) { return std::iswdigit(character) != 0; });
}

enum AdvancedDatabaseOptionsControl : int {
  kAdvancedDefaultApp = 1400,
  kAdvancedDefaultVersion,
  kAdvancedOrderInList,
  kAdvancedOrderInTree,
  kAdvancedExternal,
  kAdvancedLocale,
  kAdvancedParameters,
  kAdvancedHelpDefaultApp,
  kAdvancedHelpDefaultVersion,
  kAdvancedHelpId,
  kAdvancedHelpFolder,
  kAdvancedHelpOrderInList,
  kAdvancedHelpOrderInTree,
  kAdvancedHelpExternal,
  kAdvancedHelpLocale,
  kAdvancedHelpParameters
};

struct AdvancedDatabaseOptionsState {
  HWND default_app{};
  HWND default_version{};
  HWND order_in_list{};
  HWND order_in_tree{};
  HWND external{};
  HWND locale{};
  HWND parameters{};
  HFONT font{};
  HFONT button_font{};
  DatabaseEditorData initial;
  std::optional<DatabaseEditorData> result;
  bool done{false};
};

std::wstring FolderLabel(std::wstring_view folder) {
  return folder.empty() || folder == L"/" ? L"Корневой уровень" : std::wstring(folder);
}

void ShowAdvancedParameterHelp(HWND owner, int command) {
  std::wstring title;
  std::wstring text;
  switch (command) {
    case kAdvancedHelpDefaultApp:
      title = L"Приложение при автоопределении (DefaultApp)";
      text = L"Используется стандартным стартером 1С, когда «Режим клиента» имеет значение «Автоматически». "
             L"Допустимы тонкий или толстый клиент. Не задавайте это поле, если предпочтение не требуется.";
      break;
    case kAdvancedHelpDefaultVersion:
      title = L"Версия по умолчанию (DefaultVersion)";
      text = L"Версия платформы, фактически выбранная стартером 1С при автоматическом выборе. "
             L"IBStart использует её, если основная «Версия платформы» не задана.";
      break;
    case kAdvancedHelpId:
      title = L"Идентификатор базы (ID)";
      text = L"Уникальный идентификатор записи списка баз. Он создаётся при добавлении базы и показан только для справки. "
             L"Повторяющийся ID может привести к объединению разных записей стандартным стартером.";
      break;
    case kAdvancedHelpFolder:
      title = L"Группа списка (Folder)";
      text = L"Путь к группе в дереве списка баз. В IBStart он меняется перетаскиванием базы или команды перемещения, "
             L"поэтому здесь показан только для справки.";
      break;
    case kAdvancedHelpOrderInList:
      title = L"Порядок в списке (OrderInList)";
      text = L"Числовой порядок записи для плоского списка баз. При перемещении базы IBStart автоматически перенумеровывает записи.";
      break;
    case kAdvancedHelpOrderInTree:
      title = L"Порядок в дереве (OrderInTree)";
      text = L"Числовой порядок базы внутри её группы. IBStart учитывает его при построении дерева и обновляет при перемещении базы.";
      break;
    case kAdvancedHelpExternal:
      title = L"Внешняя запись (External)";
      text = L"Отмечает запись, полученную из подключаемого общего списка баз. Обычно это значение задаётся источником общего списка.";
      break;
    case kAdvancedHelpLocale:
      title = L"Локаль (Locale)";
      text = L"Локаль запуска 1С, например ru_RU. Оставьте пустым, чтобы применялись обычные настройки платформы.";
      break;
    case kAdvancedHelpParameters:
      title = L"Дополнительные параметры запуска (AdditionalParameters)";
      text = L"Необязательные ключи командной строки 1С, передаваемые при запуске выбранной базы. "
             L"Например: /AppArch x86_64_prt. Параметры с паролями хранить здесь небезопасно.";
      break;
    default:
      return;
  }
  Message(owner, text, title, MB_OK | MB_ICONINFORMATION);
}

void CreateAdvancedDatabaseOptionsControls(HWND window, AdvancedDatabaseOptionsState& state) {
  const UINT dpi = GetDpiForWindow(window);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD extended_style, const wchar_t* class_name, std::wstring_view text,
                          DWORD style, int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(extended_style, class_name, std::wstring(text).c_str(),
        WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(width), px(height), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetControlFont(control, font);
    return control;
  };
  const HFONT text_font = state.font;
  const HFONT button_font = state.button_font ? state.button_font : text_font;
  const auto help = [&](int y, int command) {
    create(0, L"BUTTON", L"?", WS_TABSTOP, 582, y, 48, 25, command, button_font);
  };

  create(0, L"BUTTON", L"Автоматический выбор запуска", BS_GROUPBOX, 14, 14, 632, 102, 0, text_font);
  create(0, L"STATIC", L"Клиент при автоопределении:", 0, 28, 42, 190, 20, 0, text_font);
  state.default_app = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      224, 38, 348, 120, kAdvancedDefaultApp, text_font);
  for (const auto* label : {L"Не задано", L"Тонкий клиент", L"Толстый клиент"}) {
    SendMessageW(state.default_app, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.default_app, DefaultApplicationLabel(state.initial.default_app));
  help(38, kAdvancedHelpDefaultApp);
  create(0, L"STATIC", L"Версия по умолчанию:", 0, 28, 76, 190, 20, 0, text_font);
  state.default_version = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.default_version,
      WS_TABSTOP | ES_AUTOHSCROLL, 224, 72, 348, 25, kAdvancedDefaultVersion, text_font);
  help(72, kAdvancedHelpDefaultVersion);

  create(0, L"BUTTON", L"Реквизиты списка", BS_GROUPBOX, 14, 128, 632, 172, 0, text_font);
  create(0, L"STATIC", L"Идентификатор базы:", 0, 28, 156, 156, 20, 0, text_font);
  create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.id, ES_READONLY | ES_AUTOHSCROLL,
      192, 152, 380, 25, 0, text_font);
  help(152, kAdvancedHelpId);
  create(0, L"STATIC", L"Группа списка:", 0, 28, 190, 156, 20, 0, text_font);
  create(WS_EX_CLIENTEDGE, L"EDIT", FolderLabel(state.initial.folder), ES_READONLY | ES_AUTOHSCROLL,
      192, 186, 380, 25, 0, text_font);
  help(186, kAdvancedHelpFolder);
  create(0, L"STATIC", L"Порядок в списке:", 0, 28, 224, 156, 20, 0, text_font);
  state.order_in_list = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.order_in_list,
      WS_TABSTOP | ES_AUTOHSCROLL, 192, 220, 380, 25, kAdvancedOrderInList, text_font);
  help(220, kAdvancedHelpOrderInList);
  create(0, L"STATIC", L"Порядок в дереве:", 0, 28, 258, 156, 20, 0, text_font);
  state.order_in_tree = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.order_in_tree,
      WS_TABSTOP | ES_AUTOHSCROLL, 192, 254, 380, 25, kAdvancedOrderInTree, text_font);
  help(254, kAdvancedHelpOrderInTree);

  create(0, L"BUTTON", L"Другие параметры", BS_GROUPBOX, 14, 312, 632, 130, 0, text_font);
  state.external = create(0, L"BUTTON", L"Внешняя информационная база",
      WS_TABSTOP | BS_AUTOCHECKBOX, 28, 338, 300, 20, kAdvancedExternal, text_font);
  SendMessageW(state.external, BM_SETCHECK,
      IsEnabledFlag(state.initial.external) ? BST_CHECKED : BST_UNCHECKED, 0);
  help(334, kAdvancedHelpExternal);
  create(0, L"STATIC", L"Локаль:", 0, 28, 370, 156, 20, 0, text_font);
  state.locale = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.locale,
      WS_TABSTOP | ES_AUTOHSCROLL, 192, 366, 380, 25, kAdvancedLocale, text_font);
  help(366, kAdvancedHelpLocale);
  create(0, L"STATIC", L"Параметры командной строки:", 0, 28, 404, 184, 20, 0, text_font);
  state.parameters = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.additional_parameters,
      WS_TABSTOP | ES_AUTOHSCROLL, 220, 400, 352, 25, kAdvancedParameters, text_font);
  help(400, kAdvancedHelpParameters);
  create(0, L"BUTTON", L"Сохранить", WS_TABSTOP | BS_DEFPUSHBUTTON,
      430, 462, 110, 28, IDOK, button_font);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 550, 462, 96, 28, IDCANCEL, button_font);
}

std::optional<std::wstring> CollectAdvancedDatabaseOptions(AdvancedDatabaseOptionsState& state) {
  DatabaseEditorData result = state.initial;
  result.default_app = DefaultApplicationValue(ReadControlText(state.default_app));
  result.default_version = TrimText(ReadControlText(state.default_version));
  result.order_in_list = TrimText(ReadControlText(state.order_in_list));
  result.order_in_tree = TrimText(ReadControlText(state.order_in_tree));
  if (!IsOptionalOrder(result.order_in_list) || !IsOptionalOrder(result.order_in_tree)) {
    return L"Порядок в списке и порядок в дереве должны быть целыми неотрицательными числами или пустыми.";
  }
  result.external = FlagValue(state.external, state.initial.external);
  result.locale = TrimText(ReadControlText(state.locale));
  result.additional_parameters = ReadControlText(state.parameters);
  state.result = std::move(result);
  return std::nullopt;
}

LRESULT CALLBACK AdvancedDatabaseOptionsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AdvancedDatabaseOptionsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
    return DialogControlColor(message, wparam, lparam);
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    if (command >= kAdvancedHelpDefaultApp && command <= kAdvancedHelpParameters) {
      ShowAdvancedParameterHelp(window, command);
      return 0;
    }
    if (command == IDOK) {
      if (const auto error = CollectAdvancedDatabaseOptions(*state)) {
        Message(window, *error, L"Проверка данных", MB_OK | MB_ICONWARNING);
        return 0;
      }
      state->done = true;
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

std::optional<DatabaseEditorData> EditAdvancedDatabaseOptions(HWND owner, DatabaseEditorData initial) {
  AdvancedDatabaseOptionsState state;
  state.initial = std::move(initial);
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kAdvancedDatabaseOptionsClass;
    klass.lpfnWndProc = AdvancedDatabaseOptionsProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 660, 510, style, extended_style);
  HWND window = CreateWindowExW(extended_style, kAdvancedDatabaseOptionsClass,
      L"Дополнительные настройки базы", style, CW_USEDEFAULT, CW_USEDEFAULT,
      outer_size.cx, outer_size.cy, owner, nullptr, GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  CreateAdvancedDatabaseOptionsControls(window, state);
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.default_app);
  MSG message{};
  int pump_result = 1;
  while (!state.done && (pump_result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  CloseModalDialog(window, owner);
  if (pump_result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace ibstart::ui::dialog
