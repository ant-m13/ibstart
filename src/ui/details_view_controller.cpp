#include "ui/details_view_controller.hpp"

#include "core/cache/cache_service.hpp"
#include "core/connection/connection_string.hpp"
#include "ui/tree_presentation.hpp"
#include "ui/tree_view_controller.hpp"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <utility>

namespace ibstart::ui {
namespace {

std::wstring FriendlyFieldName(std::wstring_view key) {
  struct Label {
    std::wstring_view key;
    std::wstring_view text;
  };
  constexpr Label labels[] = {
      {L"Connect", L"Подключение"},
      {L"ID", L"Идентификатор"},
      {L"Folder", L"Группа"},
      {L"OrderInList", L"Порядок"},
      {L"Version", L"Версия платформы"},
      {L"App", L"Приложение"},
      {L"DefaultApp", L"Приложение по умолчанию"},
      {L"WA", L"Аутентификация ОС"},
      {L"External", L"Внешняя"},
      {L"Locale", L"Локаль"},
      {L"ClientConnectionSpeed", L"Скорость соединения"},
      {L"AppArch", L"Разрядность"},
      {L"AdditionalParameters", L"Доп. параметры"},
  };
  for (const auto& label : labels) {
    if (CompareStringOrdinal(key.data(), static_cast<int>(key.size()), label.key.data(),
        static_cast<int>(label.key.size()), TRUE) == CSTR_EQUAL) return std::wstring(label.text);
  }
  return std::wstring(key);
}

std::wstring ConnectionKind(std::wstring_view connect) {
  if (catalog::Catalog::IsWebConnection(connect)) return L"Веб-база";
  if (connection::Value(connect, L"File")) return L"Файловая информационная база";
  if (connection::Value(connect, L"Srvr")) return L"Серверная информационная база";
  return L"Информационная база";
}

std::wstring SingleLine(std::wstring value) {
  std::replace(value.begin(), value.end(), L'\r', L' ');
  std::replace(value.begin(), value.end(), L'\n', L' ');
  std::replace(value.begin(), value.end(), L'\t', L' ');
  return value;
}

struct FileDatabasePassport {
  std::filesystem::path directory;
  std::filesystem::path database_file;
  std::optional<std::uintmax_t> size;
  std::wstring modified;
  bool network_path{false};
};

std::wstring FormatFileModificationTime(const FILETIME& value) {
  FILETIME local{};
  SYSTEMTIME time{};
  if (!FileTimeToLocalFileTime(&value, &local) || !FileTimeToSystemTime(&local, &time)) return {};
  wchar_t text[32]{};
  swprintf_s(text, L"%02u.%02u.%04u %02u:%02u", time.wDay, time.wMonth, time.wYear, time.wHour, time.wMinute);
  return text;
}

bool IsNetworkFilePath(const std::filesystem::path& path) {
  const std::wstring_view native = path.native();
  const bool extended_unc = native.size() >= 8 && native.starts_with(L"\\\\?\\") &&
      _wcsnicmp(native.data() + 4, L"UNC\\", 4) == 0;
  if (extended_unc || (native.starts_with(L"\\\\") && !native.starts_with(L"\\\\?\\")) ||
      native.starts_with(L"//")) return true;
  const auto root = path.root_path();
  return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}

FileDatabasePassport ReadFileDatabasePassport(std::wstring_view connect) {
  FileDatabasePassport result;
  result.directory = connection::ValueOrEmpty(connect, L"File");
  result.database_file = result.directory / L"1Cv8.1CD";
  if (result.directory.empty()) return result;
  result.network_path = IsNetworkFilePath(result.directory);
  if (result.network_path) return result;
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (!GetFileAttributesExW(result.database_file.c_str(), GetFileExInfoStandard, &attributes) ||
      (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return result;
  result.size = (static_cast<std::uintmax_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  result.modified = FormatFileModificationTime(attributes.ftLastWriteTime);
  return result;
}

bool EqualNoCase(std::wstring_view left, std::wstring_view right) {
  return left.size() == right.size() && CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

void DetailsViewController::Attach(Controls controls, HFONT key_font) noexcept {
  controls_ = controls;
  key_font_ = key_font;
}

void DetailsViewController::Display(const catalog::Catalog* database_catalog,
    const catalog::CatalogMetadataService* catalog_metadata, std::wstring_view selected_name,
    bool catalog_root_selected, bool simple_mode, bool cache_operation_active) const {
  const auto* entry = database_catalog ? database_catalog->Find(selected_name) : nullptr;
  if (!controls_.details) {
    UpdateConnection(entry);
    return;
  }

  ListView_DeleteAllItems(controls_.details);
  if (!entry) {
    const std::wstring selected_text(selected_name);
    SetWindowTextW(controls_.title, catalog_root_selected ? TreeViewController::kCatalogRootName.data() :
        selected_text.empty() ? L"Выберите базу или группу" : selected_text.c_str());
    SetWindowTextW(controls_.subtitle, catalog_root_selected ? L"Корневой уровень списка баз" :
        selected_text.empty() ? L"Сведения появятся здесь" : L"Служебный раздел списка");
    EnableWindow(controls_.enterprise, FALSE);
    EnableWindow(controls_.designer, FALSE);
    EnableWindow(controls_.edit, FALSE);
    EnableWindow(controls_.cache, FALSE);
    EnableWindow(controls_.shortcut, FALSE);
    EnableWindow(controls_.remove, FALSE);
    UpdateConnection(nullptr);
    return;
  }

  const std::wstring type = entry->IsDatabase() ?
      ConnectionKind(entry->ValueOr(L"Connect")) : L"Группа списка баз";
  SetWindowTextW(controls_.title, entry->name.c_str());
  const std::wstring subtitle = type + L"  •  Полей: " + std::to_wstring(entry->fields.size());
  SetWindowTextW(controls_.subtitle, subtitle.c_str());
  const auto add_row = [&](std::wstring key, std::wstring value) {
    if (value.empty()) value = L"—";
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(controls_.details);
    item.pszText = key.data();
    const int index = ListView_InsertItem(controls_.details, &item);
    if (index >= 0) ListView_SetItemText(controls_.details, index, 1, value.data());
  };
  const auto add_divider = [&] {
    std::wstring empty;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(controls_.details);
    item.pszText = empty.data();
    ListView_InsertItem(controls_.details, &item);
  };

  add_row(L"Тип", type);
  for (const auto& field : entry->fields) {
    if (_wcsicmp(field.key.c_str(), L"Connect") == 0) continue;
    auto value = SingleLine(field.value);
    if (_wcsicmp(field.key.c_str(), L"Folder") == 0 && (value.empty() || value == L"/")) {
      value = L"Корневой уровень";
    }
    add_row(FriendlyFieldName(field.key), std::move(value));
  }
  if (entry->IsDatabase()) {
    if (catalog_metadata) {
      for (const auto& tag : presentation::TagsFor(catalog_metadata->Read().tags, *entry)) {
        add_row(L"Тег", tag);
      }
    }
    const auto connect = entry->ValueOr(L"Connect");
    if (!connection::ValueOrEmpty(connect, L"File").empty()) {
      const auto passport = ReadFileDatabasePassport(connect);
      add_divider();
      add_row(L"Каталог", passport.directory.wstring());
      add_row(L"Файл 1Cv8.1CD", passport.database_file.wstring());
      if (passport.network_path) {
        add_row(L"Состояние", L"Сетевая папка: сведения не загружаются");
      } else if (passport.size) {
        add_row(L"Размер 1Cv8.1CD", cache::FormatSize(*passport.size));
        add_row(L"Изменён", passport.modified);
      } else {
        add_row(L"Состояние", L"Файл не найден или недоступен");
      }
    } else {
      const auto server = connection::ValueOrEmpty(connect, L"Srvr");
      const auto reference = connection::ValueOrEmpty(connect, L"Ref");
      if (!server.empty() || !reference.empty()) {
        add_divider();
        add_row(L"Сервер 1С", server);
        add_row(L"Имя базы", reference);
      }
    }
  }

  const bool database = entry->IsDatabase();
  const bool web = database && catalog::Catalog::IsWebConnection(entry->ValueOr(L"Connect"));
  const bool launch_available = database && !cache_operation_active;
  EnableWindow(controls_.enterprise, launch_available);
  EnableWindow(controls_.designer, launch_available && !web);
  EnableWindow(controls_.edit, !simple_mode);
  EnableWindow(controls_.remove, !simple_mode);
  EnableWindow(controls_.cache, database && !simple_mode && !cache_operation_active);
  EnableWindow(controls_.shortcut, database && !simple_mode);
  InvalidateRect(controls_.details, nullptr, TRUE);
  UpdateConnection(entry);
}

LRESULT DetailsViewController::Draw(NMLVCUSTOMDRAW* draw, const storage::TagStyles& tag_styles) const {
  if (!draw) return CDRF_DODEFAULT;
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
  if (draw->nmcd.dwDrawStage != (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) return CDRF_DODEFAULT;
  const auto row = static_cast<int>(draw->nmcd.dwItemSpec);
  draw->clrTextBk = row % 2 == 0 ? RGB(242, 248, 249) : RGB(250, 252, 253);
  if (draw->iSubItem == 1 && EqualNoCase(Text(row, 0), L"Тег")) {
    if (const auto* style = presentation::TagStyleFor(tag_styles, Text(row, 1))) {
      draw->clrTextBk = style->background;
      draw->clrText = style->text;
      return CDRF_DODEFAULT;
    }
  }
  if (draw->iSubItem == 0) {
    draw->clrText = RGB(0, 111, 129);
    if (key_font_) {
      SelectObject(draw->nmcd.hdc, key_font_);
      return CDRF_NEWFONT;
    }
  } else {
    draw->clrText = RGB(36, 50, 60);
  }
  return CDRF_DODEFAULT;
}

std::wstring DetailsViewController::Text(int row, int column) const {
  if (!controls_.details) return {};
  std::wstring text(256, L'\0');
  for (;;) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = text.data();
    item.cchTextMax = static_cast<int>(text.size());
    const int copied = static_cast<int>(SendMessageW(controls_.details, LVM_GETITEMTEXTW,
        static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item)));
    if (copied < static_cast<int>(text.size()) - 1) {
      text.resize(static_cast<size_t>(std::max(0, copied)));
      return text;
    }
    text.resize(text.size() * 2);
  }
}

void DetailsViewController::UpdateConnection(const domain::Entry* entry) const {
  if (!controls_.connection) return;
  if (!entry || !entry->IsDatabase()) {
    SetWindowTextW(controls_.connection, L"");
    return;
  }
  const std::wstring connect = entry->ValueOr(L"Connect");
  SetWindowTextW(controls_.connection, connect.c_str());
}

}  // namespace ibstart::ui
