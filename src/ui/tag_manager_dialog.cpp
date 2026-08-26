#include "ui/tag_manager_dialog.hpp"

#include "ui/dialog_support.hpp"
#include "ui/tree_presentation.hpp"

#include <commdlg.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kClassName[] = L"IBStart.TagManager";

enum Control : int {
  kList = 1700,
  kName,
  kBackground,
  kText,
  kBackgroundPalette,
  kTextPalette,
  kPreview,
  kNew,
  kSave,
  kDelete
};

struct State {
  HWND list{};
  HWND name{};
  HWND background{};
  HWND text{};
  HWND preview{};
  HFONT font{};
  HFONT button_font{};
  std::array<COLORREF, 16> custom_colors{};
  storage::DatabaseTags tags;
  storage::TagStyles styles;
  std::wstring selected;
  std::optional<TagManagerResult> result;
  bool done{false};
};

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

std::wstring ColorText(COLORREF color) {
  wchar_t value[8]{};
  swprintf_s(value, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
  return value;
}

std::optional<COLORREF> ParseColorText(std::wstring_view value) {
  const std::wstring trimmed = TrimText(value);
  std::wstring_view text = trimmed;
  if (!text.empty() && text.front() == L'#') text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  const auto digit = [](wchar_t character) -> int {
    if (character >= L'0' && character <= L'9') return character - L'0';
    if (character >= L'a' && character <= L'f') return character - L'a' + 10;
    if (character >= L'A' && character <= L'F') return character - L'A' + 10;
    return -1;
  };
  const auto byte = [&](size_t index) -> std::optional<BYTE> {
    const int high = digit(text[index]);
    const int low = digit(text[index + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    return static_cast<BYTE>(high * 16 + low);
  };
  const auto red = byte(0);
  const auto green = byte(2);
  const auto blue = byte(4);
  if (!red || !green || !blue) return std::nullopt;
  return RGB(*red, *green, *blue);
}

std::wstring ListBoxText(HWND list, int index) {
  if (!list || index == LB_ERR) return {};
  const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
  if (length == LB_ERR) return {};
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  if (SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index),
          reinterpret_cast<LPARAM>(value.data())) == LB_ERR) return {};
  value.resize(static_cast<size_t>(length));
  return value;
}

void UpdatePreview(const State& state) {
  if (state.preview) InvalidateRect(state.preview, nullptr, TRUE);
}

void SetFields(State& state, std::wstring_view name) {
  state.selected = std::wstring(name);
  const auto* style = presentation::TagStyleFor(state.styles, name);
  const storage::TagStyle value = style ? *style : storage::TagStyle{};
  SetWindowTextW(state.name, std::wstring(name).c_str());
  SetWindowTextW(state.background, ColorText(value.background).c_str());
  SetWindowTextW(state.text, ColorText(value.text).c_str());
  UpdatePreview(state);
}

void ChooseTagColor(HWND window, State& state, HWND field, COLORREF fallback) {
  CHOOSECOLORW choice{};
  choice.lStructSize = sizeof(choice);
  choice.hwndOwner = window;
  choice.rgbResult = ParseColorText(ReadControlText(field)).value_or(fallback);
  choice.lpCustColors = state.custom_colors.data();
  choice.Flags = CC_FULLOPEN | CC_RGBINIT;
  if (!ChooseColorW(&choice)) return;
  SetWindowTextW(field, ColorText(choice.rgbResult).c_str());
  UpdatePreview(state);
}

void DrawPreview(const DRAWITEMSTRUCT& draw, const State& state) {
  RECT preview = draw.rcItem;
  FillRect(draw.hDC, &preview, GetSysColorBrush(COLOR_WINDOW));
  const storage::TagStyle defaults{};
  const COLORREF background = ParseColorText(ReadControlText(state.background)).value_or(defaults.background);
  const COLORREF text = ParseColorText(ReadControlText(state.text)).value_or(defaults.text);
  std::wstring label = TrimText(ReadControlText(state.name));
  if (label.empty()) label = L"Название тега";

  const HFONT font = state.button_font ? state.button_font : state.font;
  const HGDIOBJ previous_font = font ? SelectObject(draw.hDC, font) : nullptr;
  SetBkMode(draw.hDC, TRANSPARENT);
  SIZE text_size{};
  GetTextExtentPoint32W(draw.hDC, label.c_str(), static_cast<int>(label.size()), &text_size);
  const int preview_width = static_cast<int>(preview.right - preview.left);
  const int preview_height = static_cast<int>(preview.bottom - preview.top);
  const int text_width = static_cast<int>(text_size.cx);
  const int text_height = static_cast<int>(text_size.cy);
  const int width = std::min(preview_width - 16, std::max(88, text_width + 22));
  const int height = std::min(preview_height - 10, std::max(22, text_height + 8));
  const int left = static_cast<int>(preview.left) + (preview_width - width) / 2;
  const int top = static_cast<int>(preview.top) + (preview_height - height) / 2;
  RECT tag{left, top, left + width, top + height};
  const HBRUSH brush = CreateSolidBrush(background);
  const HPEN pen = CreatePen(PS_SOLID, 1, background);
  const HGDIOBJ previous_brush = SelectObject(draw.hDC, brush);
  const HGDIOBJ previous_pen = SelectObject(draw.hDC, pen);
  RoundRect(draw.hDC, tag.left, tag.top, tag.right, tag.bottom, height, height);
  SetTextColor(draw.hDC, text);
  DrawTextW(draw.hDC, label.c_str(), static_cast<int>(label.size()), &tag,
      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(draw.hDC, previous_brush);
  SelectObject(draw.hDC, previous_pen);
  if (previous_font) SelectObject(draw.hDC, previous_font);
  DeleteObject(brush);
  DeleteObject(pen);
}

void RefreshList(State& state, std::wstring_view selected = {}) {
  const auto tags = presentation::KnownTags(state.tags, state.styles);
  SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
  int selection = LB_ERR;
  for (const auto& tag : tags) {
    const int index = static_cast<int>(
        SendMessageW(state.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tag.c_str())));
    if (selection == LB_ERR && EqualNoCase(tag, selected)) selection = index;
  }
  if (selection != LB_ERR) {
    SendMessageW(state.list, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    SetFields(state, ListBoxText(state.list, selection));
  } else {
    SetFields(state, L"");
  }
}

bool SaveEntry(HWND window, State& state) {
  const std::wstring name = TrimText(ReadControlText(state.name));
  const auto background = ParseColorText(ReadControlText(state.background));
  const auto text = ParseColorText(ReadControlText(state.text));
  if (name.empty()) {
    Message(window, L"Укажите название тега.", L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!background || !text) {
    Message(window, L"Цвета указываются в виде #RRGGBB, например #E2F2F4.",
        L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }
  const auto known = presentation::KnownTags(state.tags, state.styles);
  const auto existing = std::find_if(known.begin(), known.end(),
      [&](const auto& tag) { return EqualNoCase(tag, name); });
  if (existing != known.end() && (state.selected.empty() || !EqualNoCase(state.selected, name))) {
    Message(window, L"Тег с таким названием уже есть.", L"Настройка тегов", MB_OK | MB_ICONWARNING);
    return false;
  }

  if (!state.selected.empty() && !EqualNoCase(state.selected, name)) {
    for (auto& [_, values] : state.tags) {
      for (auto& tag : values) {
        if (EqualNoCase(tag, state.selected)) tag = name;
      }
    }
    presentation::EraseTagStyle(state.styles, state.selected);
  }
  presentation::EraseTagStyle(state.styles, name);
  state.styles[name] = {*background, *text};
  RefreshList(state, name);
  return true;
}

void DeleteEntry(HWND window, State& state) {
  if (state.selected.empty()) return;
  const std::wstring message = L"Удалить тег «" + state.selected + L"» у всех баз и из настроек?";
  if (MessageBoxW(window, message.c_str(), L"Настройка тегов", MB_YESNO | MB_ICONWARNING) != IDYES) return;
  for (auto it = state.tags.begin(); it != state.tags.end();) {
    auto& values = it->second;
    values.erase(std::remove_if(values.begin(), values.end(),
                     [&](const auto& tag) { return EqualNoCase(tag, state.selected); }),
        values.end());
    if (values.empty()) it = state.tags.erase(it);
    else ++it;
  }
  presentation::EraseTagStyle(state.styles, state.selected);
  RefreshList(state);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
    return TRUE;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
    return DialogControlColor(message, wparam, lparam);
  }
  if (message == WM_DRAWITEM && state) {
    const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (draw && draw->CtlID == kPreview) {
      DrawPreview(*draw, *state);
      return TRUE;
    }
  }
  if (message == WM_COMMAND && state) {
    const int command = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (command == kList && notification == LBN_SELCHANGE) {
      const int selection = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
      SetFields(*state, ListBoxText(state->list, selection));
      return 0;
    }
    if ((command == kName || command == kBackground || command == kText) && notification == EN_CHANGE) {
      UpdatePreview(*state);
      return 0;
    }
    if (command == kBackgroundPalette) {
      ChooseTagColor(window, *state, state->background, storage::TagStyle{}.background);
      return 0;
    }
    if (command == kTextPalette) {
      ChooseTagColor(window, *state, state->text, storage::TagStyle{}.text);
      return 0;
    }
    if (command == kNew) {
      SendMessageW(state->list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
      SetFields(*state, L"");
      SetFocus(state->name);
      return 0;
    }
    if (command == kSave) {
      SaveEntry(window, *state);
      return 0;
    }
    if (command == kDelete) {
      DeleteEntry(window, *state);
      return 0;
    }
    if (command == IDOK) {
      state->result = TagManagerResult{state->tags, state->styles};
      state->done = true;
      DestroyWindow(window);
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      DestroyWindow(window);
      return 0;
    }
  }
  if (message == WM_CLOSE && state) {
    state->done = true;
    DestroyWindow(window);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

std::optional<TagManagerResult> EditTagManager(
    HWND owner, const storage::DatabaseTags& tags, const storage::TagStyles& styles) {
  State state;
  state.tags = tags;
  state.styles = styles;
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kClassName;
    klass.lpfnWndProc = WindowProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  if (owner) EnableWindow(owner, FALSE);
  const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 650, 322, style, extended_style);
  HWND window = CreateWindowExW(extended_style, kClassName, L"Настройка тегов", style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr,
      GetModuleHandleW(nullptr), &state);
  if (!window) {
    RestoreModalOwner(owner);
    return std::nullopt;
  }
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  const auto px = [dpi](int logical) { return ScaleForDpi(logical, dpi); };
  const auto create = [&](DWORD extended_style, const wchar_t* class_name, std::wstring_view text,
                          DWORD control_style, int x, int y, int width, int height, int id, HFONT font) {
    const HWND control = CreateWindowExW(extended_style, class_name, std::wstring(text).c_str(),
        WS_CHILD | WS_VISIBLE | control_style, px(x), px(y), px(width), px(height), window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SetControlFont(control, font);
    return control;
  };
  const HFONT button_font = state.button_font ? state.button_font : state.font;
  create(0, L"STATIC", L"Теги", 0, 10, 10, 230, 18, 0, state.font);
  state.list = create(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
      10, 30, 230, 234, kList, state.font);
  create(0, L"STATIC", L"Параметры тега", 0, 270, 10, 200, 18, 0, state.font);
  create(0, L"STATIC", L"Название", 0, 270, 32, 370, 18, 0, state.font);
  state.name = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
      270, 51, 370, 25, kName, state.font);
  create(0, L"STATIC", L"Цвет фона", 0, 270, 87, 190, 18, 0, state.font);
  state.background = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
      270, 106, 190, 25, kBackground, state.font);
  create(0, L"BUTTON", L"Выбрать…", WS_TABSTOP, 470, 105, 170, 27, kBackgroundPalette, button_font);
  create(0, L"STATIC", L"Цвет текста", 0, 270, 143, 190, 18, 0, state.font);
  state.text = create(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
      270, 162, 190, 25, kText, state.font);
  create(0, L"BUTTON", L"Выбрать…", WS_TABSTOP, 470, 161, 170, 27, kTextPalette, button_font);
  create(0, L"STATIC", L"Предпросмотр", 0, 270, 199, 370, 18, 0, state.font);
  state.preview = create(WS_EX_CLIENTEDGE, L"STATIC", L"", SS_OWNERDRAW,
      270, 218, 370, 47, kPreview, state.font);
  create(0, L"BUTTON", L"Новый", WS_TABSTOP, 10, 278, 108, 28, kNew, button_font);
  create(0, L"BUTTON", L"Удалить", WS_TABSTOP, 128, 278, 112, 28, kDelete, button_font);
  create(0, L"BUTTON", L"Сохранить тег", WS_TABSTOP, 270, 278, 140, 28, kSave, button_font);
  create(0, L"BUTTON", L"Готово", WS_TABSTOP | BS_DEFPUSHBUTTON,
      448, 278, 92, 28, IDOK, button_font);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 548, 278, 92, 28, IDCANCEL, button_font);
  RefreshList(state);
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  SetFocus(state.list);
  MSG message{};
  int result = 1;
  while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(window)) DestroyWindow(window);
  if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
  RestoreModalOwner(owner);
  if (state.font) DeleteObject(state.font);
  if (state.button_font) DeleteObject(state.button_font);
  return state.result;
}

}  // namespace ibstart::ui::dialog
