#include "ui/launch_options_dialog.hpp"

#include "core/connection/connection_string.hpp"
#include "core/credentials/credentials.hpp"
#include "core/domain/utf.hpp"
#include "core/launcher/command_builder.hpp"
#include "core/logging/logging.hpp"
#include "ui/dialog_support.hpp"

#include <CommCtrl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <initializer_list>
#include <string>
#include <utility>

namespace ibstart::ui::dialog {
namespace {

constexpr wchar_t kLaunchOptionsClass[] = L"IBStart.LaunchOptions";
constexpr UINT_PTR kCopyStatusTimer = 1;

enum LaunchOptionsControl : int {
  kLaunchMode = 1600,
  kLaunchClient,
  kLaunchPlatform,
  kLaunchArchitecture,
  kLaunchUser,
  kLaunchPassword,
  kLaunchCredential,
  kLaunchManageCredentials,
  kLaunchParameters,
  kLaunchPreview,
  kLaunchCopyCommand
};

struct LaunchOptionsState {
  HWND mode{};
  HWND client{};
  HWND platform{};
  HWND architecture{};
  HWND user{};
  HWND password{};
  HWND credential{};
  HWND parameters{};
  HWND preview{};
  HWND notice{};
  HWND status{};
  HFONT font{};
  HFONT button_font{};
  HFONT notice_font{};
  const domain::Database* database{};
  const std::vector<domain::PlatformInstallation>* platforms{};
  std::vector<credentials::Credential> credentials;
  std::function<std::vector<credentials::Credential>(HWND)> manage_credentials;
  domain::LaunchOptions initial;
  std::optional<domain::LaunchOptions> result;
  bool web{};
  bool done{false};
  bool edited_credentials{false};
  bool populating{false};
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
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<size_t>(std::max(copied, 0)));
  return result;
}

bool CopyTextToClipboard(HWND owner, std::wstring_view text) {
  const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
  const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!memory) return false;
  auto* destination = static_cast<wchar_t*>(GlobalLock(memory));
  if (!destination) {
    GlobalFree(memory);
    return false;
  }
  std::copy(text.begin(), text.end(), destination);
  destination[text.size()] = L'\0';
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    return false;
  }
  struct ClipboardCloser {
    ~ClipboardCloser() { CloseClipboard(); }
  } closer;
  if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    return false;
  }
  return true;
}

void SetComboValue(HWND combo, std::wstring_view value) {
  const std::wstring text(value);
  const LRESULT index = SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
      reinterpret_cast<LPARAM>(text.c_str()));
  if (index != CB_ERR) SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

void SetComboData(HWND combo, LPARAM value) {
  const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  for (int index = 0; index < count; ++index) {
    if (SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0) == value) {
      SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
      return;
    }
  }
  SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

std::wstring ReadComboText(HWND combo) {
  const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
  if (index == CB_ERR) return {};
  return ReadControlText(combo);
}

std::wstring ModeLabel(domain::LaunchMode mode) {
  return mode == domain::LaunchMode::designer ? L"Конфигуратор" : L"Предприятие";
}

domain::LaunchMode ModeValue(std::wstring_view label) {
  return EqualNoCase(label, L"Конфигуратор") ? domain::LaunchMode::designer : domain::LaunchMode::enterprise;
}

std::wstring ClientLabel(domain::ClientType type) {
  switch (type) {
    case domain::ClientType::thick: return L"Толстый клиент";
    case domain::ClientType::thin: return L"Тонкий клиент";
    default: return L"Автоматически";
  }
}

domain::ClientType ClientValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Толстый клиент")) return domain::ClientType::thick;
  if (EqualNoCase(label, L"Тонкий клиент")) return domain::ClientType::thin;
  return domain::ClientType::automatic;
}

std::wstring ArchitectureLabel(domain::ClientArchitecture architecture) {
  switch (architecture) {
    case domain::ClientArchitecture::x86: return L"Только 32 (x86)";
    case domain::ClientArchitecture::x64: return L"Только 64 (x86_64)";
    case domain::ClientArchitecture::x86_priority: return L"Приоритет 32 (x86_prt)";
    case domain::ClientArchitecture::x64_priority: return L"Приоритет 64 (x86_64_prt)";
    default: return L"Автоматически";
  }
}

domain::ClientArchitecture ArchitectureValue(std::wstring_view label) {
  if (EqualNoCase(label, L"Только 32 (x86)")) return domain::ClientArchitecture::x86;
  if (EqualNoCase(label, L"Только 64 (x86_64)")) return domain::ClientArchitecture::x64;
  if (EqualNoCase(label, L"Приоритет 32 (x86_prt)")) return domain::ClientArchitecture::x86_priority;
  if (EqualNoCase(label, L"Приоритет 64 (x86_64_prt)")) return domain::ClientArchitecture::x64_priority;
  return domain::ClientArchitecture::automatic;
}

std::wstring BitnessLabel(domain::ClientBitness bitness) {
  return bitness == domain::ClientBitness::x86 ? L"x86" : bitness == domain::ClientBitness::x64 ? L"x64" : L"?";
}

std::wstring PlatformLabel(const domain::PlatformInstallation& platform) {
  return platform.version + L" — " + BitnessLabel(platform.bitness) + L" — " + platform.executable.wstring();
}

void UpdateModeControls(LaunchOptionsState& state) {
  const auto mode = ModeValue(ReadComboText(state.mode));
  const bool designer = mode == domain::LaunchMode::designer;
  const bool forced_thin = state.web;
  if (designer || forced_thin) {
    SetComboValue(state.client, forced_thin ? L"Тонкий клиент" : L"Толстый клиент");
    EnableWindow(state.client, FALSE);
  } else {
    EnableWindow(state.client, TRUE);
  }
}

void UpdatePlatformControls(LaunchOptionsState& state) {
  const LRESULT index = SendMessageW(state.platform, CB_GETCURSEL, 0, 0);
  EnableWindow(state.architecture, index == 0);
}

std::optional<domain::LaunchOptions> CollectOptions(LaunchOptionsState& state, std::wstring* error) {
  domain::LaunchOptions result = state.initial;
  result.mode = ModeValue(ReadComboText(state.mode));
  result.client_type = ClientValue(ReadComboText(state.client));
  result.architecture = ArchitectureValue(ReadComboText(state.architecture));
  result.user_name = TrimText(ReadControlText(state.user));
  result.password = ReadControlText(state.password);
  result.individual_parameters = ReadControlText(state.parameters);
  result.override_individual_parameters = true;

  const LRESULT platform_index = SendMessageW(state.platform, CB_GETCURSEL, 0, 0);
  if (platform_index == CB_ERR || platform_index == 0) {
    result.platform_executable.reset();
  } else {
    const auto candidate_index = SendMessageW(state.platform, CB_GETITEMDATA,
        static_cast<WPARAM>(platform_index), 0);
    if (candidate_index < 1 || static_cast<size_t>(candidate_index) > state.platforms->size()) {
      *error = L"Не удалось определить выбранную установку платформы.";
      return std::nullopt;
    }
    result.platform_executable = (*state.platforms)[static_cast<size_t>(candidate_index) - 1].executable;
    result.version = L"Авто";
    result.architecture = domain::ClientArchitecture::automatic;
  }

  if (state.web) {
    if (result.mode == domain::LaunchMode::designer) {
      *error = L"Конфигуратор недоступен для веб-базы.";
      return std::nullopt;
    }
    result.client_type = domain::ClientType::thin;
  } else if (result.mode == domain::LaunchMode::designer) {
    result.client_type = domain::ClientType::thick;
  }
  if (result.client_type == domain::ClientType::web) {
    *error = L"Веб-клиент можно выбрать только для веб-базы.";
    return std::nullopt;
  }
  if (result.password.empty() && !result.user_name.empty()) {
    // An empty password is valid for some bases; leave it out of the command
    // so the 1C client can request it interactively when needed.
  }
  return result;
}

std::wstring Preview(LaunchOptionsState& state, const domain::LaunchOptions& options) {
  try {
    const auto validation = launcher::ValidateLaunchParameters(*state.database, options);
    if (!validation.empty()) return L"Ошибка проверки:\r\n" + validation.front();
    const auto selected = launcher::SelectPlatform(*state.platforms, options);
    if (!selected) return L"Подходящая установленная платформа не найдена.";
    return logging::RedactedCommandLine(launcher::BuildCommand(*state.database, *selected, options));
  } catch (const std::exception& error) {
    return L"Ошибка построения команды:\r\n" + utf::FromUtf8(error.what());
  }
}

std::optional<std::wstring> FullCommandLine(LaunchOptionsState& state,
    const domain::LaunchOptions& options) {
  const auto validation = launcher::ValidateLaunchParameters(*state.database, options);
  if (!validation.empty()) throw std::invalid_argument(utf::ToUtf8(validation.front()));
  const auto selected = launcher::SelectPlatform(*state.platforms, options);
  if (!selected) return std::nullopt;
  return launcher::BuildCommand(*state.database, *selected, options).CommandLine();
}

void ShowStatus(LaunchOptionsState& state, std::wstring_view text) {
  if (state.status) SetWindowTextW(state.status, std::wstring(text).c_str());
  if (state.status) SetTimer(GetParent(state.status), kCopyStatusTimer, 3500, nullptr);
}

void RefreshPreview(LaunchOptionsState& state) {
  std::wstring error;
  const auto options = CollectOptions(state, &error);
  if (!options) {
    SetWindowTextW(state.preview, error.c_str());
    return;
  }
  SetWindowTextW(state.preview, Preview(state, *options).c_str());
}

void ApplyCredentialSelection(LaunchOptionsState& state) {
  state.populating = true;
  const LRESULT index = SendMessageW(state.credential, CB_GETCURSEL, 0, 0);
  if (index <= 0 || index == CB_ERR) {
    SetWindowTextW(state.user, L"");
    SetWindowTextW(state.password, L"");
    state.edited_credentials = false;
  } else {
    const auto record = static_cast<size_t>(SendMessageW(state.credential, CB_GETITEMDATA,
        static_cast<WPARAM>(index), 0));
    if (record < state.credentials.size()) {
      SetWindowTextW(state.user, state.credentials[record].user_name.c_str());
      SetWindowTextW(state.password, state.credentials[record].password.c_str());
      state.edited_credentials = false;
    }
  }
  SetWindowTextW(state.notice, state.edited_credentials ? L"Изменено для этого запуска." :
      L"Параметры действуют только для этого запуска.");
  RefreshPreview(state);
  state.populating = false;
}

std::wstring SelectedCredentialId(const LaunchOptionsState& state) {
  if (!state.credential) return {};
  const LRESULT index = SendMessageW(state.credential, CB_GETCURSEL, 0, 0);
  if (index <= 0 || index == CB_ERR) return {};
  const auto record = static_cast<size_t>(SendMessageW(state.credential, CB_GETITEMDATA,
      static_cast<WPARAM>(index), 0));
  return record < state.credentials.size() ? state.credentials[record].id : std::wstring();
}

void RefreshCredentials(LaunchOptionsState& state, std::wstring_view selected_id = {}) {
  if (!state.credential) return;
  SendMessageW(state.credential, CB_RESETCONTENT, 0, 0);
  SendMessageW(state.credential, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ручной ввод"));
  SendMessageW(state.credential, CB_SETITEMDATA, 0, 0);
  for (size_t i = 0; i < state.credentials.size(); ++i) {
    const auto& item = state.credentials[i];
    const std::wstring label = item.title.empty() ? item.user_name : item.title + L" — " + item.user_name;
    const LRESULT added = SendMessageW(state.credential, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(label.c_str()));
    if (added != CB_ERR) SendMessageW(state.credential, CB_SETITEMDATA,
        static_cast<WPARAM>(added), static_cast<LPARAM>(i));
  }
  int selected = 0;
  if (!selected_id.empty()) {
    for (size_t i = 0; i < state.credentials.size(); ++i) {
      if (EqualNoCase(state.credentials[i].id, selected_id)) { selected = static_cast<int>(i + 1); break; }
    }
  }
  SendMessageW(state.credential, CB_SETCURSEL, selected, 0);
}

void CreateControls(HWND window, LaunchOptionsState& state) {
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
  const HFONT notice_font = state.notice_font ? state.notice_font : text_font;

  state.notice = create(0, L"STATIC", L"Параметры действуют только для этого запуска.",
      0, 28, 4, 420, 16, 0, notice_font);
  create(0, L"BUTTON", L"Режим запуска", BS_GROUPBOX, 14, 24, 632, 62, 0, text_font);
  create(0, L"STATIC", L"Режим:", 0, 28, 47, 86, 20, 0, text_font);
  state.mode = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
      120, 43, 228, 110, kLaunchMode, text_font);
  for (const auto* label : state.web ? std::initializer_list<const wchar_t*>{L"Предприятие"} :
                                      std::initializer_list<const wchar_t*>{L"Предприятие", L"Конфигуратор"}) {
    SendMessageW(state.mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.mode, ModeLabel(state.initial.mode));

  create(0, L"BUTTON", L"Клиент и платформа", BS_GROUPBOX, 14, 96, 632, 108, 0, text_font);
  create(0, L"STATIC", L"Тип клиента:", 0, 28, 122, 126, 20, 0, text_font);
  state.client = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
      160, 118, 190, 100, kLaunchClient, text_font);
  for (const auto* label : {L"Автоматически", L"Толстый клиент", L"Тонкий клиент"}) {
    SendMessageW(state.client, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.client, ClientLabel(state.initial.client_type));
  create(0, L"STATIC", L"Разрядность:", 0, 370, 122, 108, 20, 0, text_font);
  state.architecture = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
      480, 118, 150, 130, kLaunchArchitecture, text_font);
  for (const auto* label : {L"Автоматически", L"Только 32 (x86)", L"Только 64 (x86_64)",
           L"Приоритет 32 (x86_prt)", L"Приоритет 64 (x86_64_prt)"}) {
    SendMessageW(state.architecture, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
  }
  SetComboValue(state.architecture, ArchitectureLabel(state.initial.architecture));
  create(0, L"STATIC", L"Платформа:", 0, 28, 158, 126, 20, 0, text_font);
  state.platform = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      160, 150, 470, 145, kLaunchPlatform, text_font);
  SendMessageW(state.platform, CB_ADDSTRING, 0,
      reinterpret_cast<LPARAM>(L"Автоматически (по настройкам базы)"));
  SendMessageW(state.platform, CB_SETITEMDATA, 0, 0);
  for (size_t index = 0; index < state.platforms->size(); ++index) {
    const auto& platform = (*state.platforms)[index];
    const auto label = PlatformLabel(platform);
    const LRESULT item = SendMessageW(state.platform, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(label.c_str()));
    if (item != CB_ERR) SendMessageW(state.platform, CB_SETITEMDATA,
        static_cast<WPARAM>(item), static_cast<LPARAM>(index + 1));
  }
  SetComboData(state.platform, state.initial.platform_executable ?
      static_cast<LPARAM>(std::distance(state.platforms->begin(), std::find_if(state.platforms->begin(), state.platforms->end(),
          [&](const auto& platform) { return platform.executable == *state.initial.platform_executable; })) + 1) : 0);

  create(0, L"BUTTON", L"Пользователь 1С", BS_GROUPBOX, 14, 216, 632, 126, 0, text_font);
  create(0, L"STATIC", L"Учётная запись:", 0, 28, 244, 154, 20, 0, text_font);
  state.credential = create(0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
      190, 240, 332, 120, kLaunchCredential, text_font);
  create(0, L"BUTTON", L"Управление…", WS_TABSTOP, 530, 240, 100, 25, kLaunchManageCredentials, button_font);
  RefreshCredentials(state);
  create(0, L"STATIC", L"Имя пользователя:", 0, 28, 274, 154, 20, 0, text_font);
  state.user = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.user_name,
      WS_TABSTOP | ES_AUTOHSCROLL, 190, 270, 440, 25, kLaunchUser, text_font);
  create(0, L"STATIC", L"Пароль:", 0, 28, 308, 154, 20, 0, text_font);
  state.password = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.password,
      WS_TABSTOP | ES_AUTOHSCROLL, 190, 304, 440, 25, kLaunchPassword, text_font);

  create(0, L"BUTTON", L"Дополнительные параметры", BS_GROUPBOX, 14, 354, 632, 94, 0, text_font);
  state.parameters = create(WS_EX_CLIENTEDGE, L"EDIT", state.initial.individual_parameters,
      WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
      28, 382, 604, 52, kLaunchParameters, text_font);

  create(0, L"BUTTON", L"Предварительный просмотр команды", BS_GROUPBOX, 14, 458, 632, 94, 0, text_font);
  state.preview = create(WS_EX_CLIENTEDGE, L"EDIT", L"",
      ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
      28, 486, 604, 50, kLaunchPreview, text_font);

  state.status = create(0, L"STATIC", L"", 0, 28, 570, 214, 28, 0, notice_font);
  create(0, L"BUTTON", L"Копировать команду", WS_TABSTOP, 252, 566, 156, 28,
      kLaunchCopyCommand, button_font);
  create(0, L"BUTTON", L"Запустить", WS_TABSTOP | BS_DEFPUSHBUTTON, 418, 566, 100, 28, IDOK, button_font);
  create(0, L"BUTTON", L"Отмена", WS_TABSTOP, 528, 566, 102, 28, IDCANCEL, button_font);
  UpdateModeControls(state);
  UpdatePlatformControls(state);
  RefreshPreview(state);
}

LRESULT CALLBACK LaunchOptionsProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<LaunchOptionsState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
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
    if (command == kLaunchMode && HIWORD(wparam) == CBN_SELCHANGE) {
      UpdateModeControls(*state);
      RefreshPreview(*state);
      return 0;
    }
    if (command == kLaunchPlatform && HIWORD(wparam) == CBN_SELCHANGE) {
      UpdatePlatformControls(*state);
      RefreshPreview(*state);
      return 0;
    }
    if (command == kLaunchCredential && HIWORD(wparam) == CBN_SELCHANGE) {
      ApplyCredentialSelection(*state);
      return 0;
    }
    if (command == kLaunchManageCredentials && HIWORD(wparam) == BN_CLICKED) {
      if (state->manage_credentials) {
        const auto selected_id = SelectedCredentialId(*state);
        std::optional<credentials::Credential> old_record;
        for (const auto& record : state->credentials) if (EqualNoCase(record.id, selected_id)) old_record = record;
        std::vector<credentials::Credential> updated;
        try { updated = state->manage_credentials(window); }
        catch (const std::exception& error) {
          Message(window, L"Не удалось обновить учётные записи.\n\n" + utf::FromUtf8(error.what()),
              L"Учётные записи", MB_OK | MB_ICONERROR);
          return 0;
        }
        catch (...) {
          Message(window, L"Не удалось обновить учётные записи.", L"Учётные записи", MB_OK | MB_ICONERROR);
          return 0;
        }
        state->credentials = updated;
        RefreshCredentials(*state, selected_id);
        if (!selected_id.empty() && SelectedCredentialId(*state).empty()) {
          ApplyCredentialSelection(*state);
        } else if (!selected_id.empty()) {
          const auto selected = std::find_if(state->credentials.begin(), state->credentials.end(),
              [&](const auto& record) { return EqualNoCase(record.id, selected_id); });
          if (selected != state->credentials.end() && (!old_record || *selected != *old_record)) {
            ApplyCredentialSelection(*state);
          }
        }
        RefreshPreview(*state);
      }
      return 0;
    }
    if ((command == kLaunchClient || command == kLaunchArchitecture) &&
        HIWORD(wparam) == CBN_SELCHANGE) {
      RefreshPreview(*state);
      return 0;
    }
    if ((command == kLaunchUser || command == kLaunchPassword || command == kLaunchParameters) &&
        HIWORD(wparam) == EN_CHANGE) {
      if ((command == kLaunchUser || command == kLaunchPassword) && !state->populating) {
        state->edited_credentials = true;
        SetWindowTextW(state->notice, L"Изменено для этого запуска.");
      }
      RefreshPreview(*state);
      return 0;
    }
    if (command == kLaunchCopyCommand) {
      std::wstring error;
      const auto options = CollectOptions(*state, &error);
      if (!options) {
        ShowStatus(*state, error);
        return 0;
      }
      try {
        const auto command_line = FullCommandLine(*state, *options);
        if (!command_line) {
          const auto preview = Preview(*state, *options);
          SetWindowTextW(state->preview, preview.c_str());
          ShowStatus(*state, L"Подходящая платформа не найдена.");
        } else if (!CopyTextToClipboard(window, *command_line)) {
          ShowStatus(*state, L"Не удалось скопировать команду.");
        } else {
          // The editor keeps the redacted preview on screen, while the
          // explicit copy action returns the exact command requested by the
          // user, including any credentials entered for this launch.
          ShowStatus(*state, L"Команда скопирована.");
        }
      } catch (const std::exception& error) {
        const auto detail = utf::FromUtf8(error.what());
        SetWindowTextW(state->preview, (L"Ошибка копирования команды:\r\n" + detail).c_str());
        ShowStatus(*state, L"Не удалось подготовить команду.");
      }
      return 0;
    }
    if (command == IDOK) {
      std::wstring error;
      const auto options = CollectOptions(*state, &error);
      if (!options) {
        Message(window, error, L"Проверка запуска", MB_OK | MB_ICONWARNING);
        return 0;
      }
      const auto preview = Preview(*state, *options);
      if (preview.rfind(L"Ошибка", 0) == 0 || preview.rfind(L"Подходящая", 0) == 0) {
        SetWindowTextW(state->preview, preview.c_str());
        Message(window, preview, L"Проверка запуска", MB_OK | MB_ICONWARNING);
        return 0;
      }
      state->result = *options;
      state->done = true;
      return 0;
    }
    if (command == IDCANCEL) {
      state->done = true;
      return 0;
    }
  }
  if (message == WM_TIMER && state && wparam == kCopyStatusTimer) {
    KillTimer(window, kCopyStatusTimer);
    SetWindowTextW(state->status, L"");
    return 0;
  }
  if (message == WM_CLOSE && state) {
    KillTimer(window, kCopyStatusTimer);
    state->done = true;
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

std::optional<domain::LaunchOptions> EditLaunchOptions(HWND owner, const domain::Database& database,
    const std::vector<domain::PlatformInstallation>& platforms, domain::LaunchOptions initial,
    std::vector<credentials::Credential> credential_records,
    std::function<std::vector<credentials::Credential>(HWND)> manage_credentials) {
  LaunchOptionsState state;
  state.database = &database;
  state.platforms = &platforms;
  state.credentials = std::move(credential_records);
  state.manage_credentials = std::move(manage_credentials);
  state.initial = std::move(initial);
  state.web = connection::WebUrl(database.connect).has_value();
  if (state.web) {
    state.initial.mode = domain::LaunchMode::enterprise;
    state.initial.client_type = domain::ClientType::thin;
  }
  static ATOM atom = [] {
    WNDCLASSW klass{};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = kLaunchOptionsClass;
    klass.lpfnWndProc = LaunchOptionsProc;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassW(&klass);
  }();
  (void)atom;
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  const SIZE outer_size = DialogOuterSize(owner, 660, 614, style, extended_style);
  const std::wstring title = L"Запуск с параметрами — " + database.name;
  HWND window = CreateWindowExW(extended_style, kLaunchOptionsClass, title.c_str(), style,
      CW_USEDEFAULT, CW_USEDEFAULT, outer_size.cx, outer_size.cy, owner, nullptr,
      GetModuleHandleW(nullptr), &state);
  if (!window) return std::nullopt;
  state.font = CreateUiFont(window, 9, FW_NORMAL);
  state.button_font = CreateUiFont(window, 9, FW_NORMAL);
  CreateControls(window, state);
  PositionDialogNearOwner(window, owner);
  ShowWindow(window, SW_SHOW);
  DisableModalOwner(owner);
  SetFocus(state.mode);
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
