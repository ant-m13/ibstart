#include "ui/menu_controller.hpp"

#include "app/resource.h"
#include "ui/command_ids.hpp"

#include <utility>

namespace ibstart::ui {
using namespace commands;
namespace {

OwnerDrawMenuIcon MenuIconForCommand(UINT command) {
  switch (command) {
    case kMoveUp: return OwnerDrawMenuIcon::move_up;
    case kMoveDown: return OwnerDrawMenuIcon::move_down;
    case kSimpleMode: return OwnerDrawMenuIcon::compact_mode;
    case kTagsContextMenu:
    case kEditTags:
    case kConfigureTagColors:
    case kShowTagsInList: return OwnerDrawMenuIcon::tag;
    case kSortAscending: return OwnerDrawMenuIcon::sort_ascending;
    case kSortDescending: return OwnerDrawMenuIcon::sort_descending;
    default: return OwnerDrawMenuIcon::standard;
  }
}

HICON LoadResourceIcon(HINSTANCE instance, int resource, int size) {
  return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource), IMAGE_ICON,
      size, size, LR_DEFAULTCOLOR));
}

void ClearMenu(HMENU menu) {
  if (!menu) return;
  while (GetMenuItemCount(menu) > 0) RemoveMenu(menu, 0, MF_BYPOSITION);
}

}  // namespace

MenuController::~MenuController() {
  Clear();
}

void MenuController::Create(HWND window, HINSTANCE instance) {
  if (menu_) return;
  window_ = window;
  instance_ = instance;
  menu_ = CreateMenu();
  file_menu_ = CreatePopupMenu();
  view_menu_ = CreatePopupMenu();
  help_menu_ = CreatePopupMenu();
}

void MenuController::Clear() noexcept {
  main_menu_items_.Clear();
  file_menu_items_.Clear();
  if (window_ && IsWindow(window_)) SetMenu(window_, nullptr);
  if (menu_) DestroyMenu(menu_);
  menu_ = nullptr;
  file_menu_ = nullptr;
  view_menu_ = nullptr;
  help_menu_ = nullptr;
  window_ = nullptr;
  instance_ = nullptr;
}

void MenuController::RefreshFile(const storage::Settings& settings) {
  if (!file_menu_) return;
  while (GetMenuItemCount(file_menu_) > 0) {
    const HMENU submenu = GetSubMenu(file_menu_, 0);
    RemoveMenu(file_menu_, 0, MF_BYPOSITION);
    if (submenu) DestroyMenu(submenu);
  }
  file_menu_items_.Clear();
  const auto append = [&](bool enabled, bool checked, UINT command, int icon_resource,
                          std::wstring text, std::wstring shortcut = {}) {
    file_menu_items_.Append(file_menu_, command,
        icon_resource == 0 ? nullptr : LoadResourceIcon(instance_, icon_resource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), enabled, checked);
  };
  const auto append_popup = [&](HMENU submenu, UINT identity, int icon_resource, std::wstring text) {
    file_menu_items_.Append(file_menu_, identity,
        icon_resource == 0 ? nullptr : LoadResourceIcon(instance_, icon_resource, 20),
        std::move(text), {}, MenuIconForCommand(identity), true, false, submenu);
  };
  append(true, false, kOpenList, IDI_TREE_FOLDER, L"Открыть список баз…", L"Ctrl+O");
  append(true, false, kOpenStandardList, IDI_TREE_FOLDER, L"Открыть стандартный список 1С");
  HMENU recent = CreatePopupMenu();
  if (recent) {
    size_t count = 0;
    for (const auto& path : settings.recent_ibases) {
      if (count >= 9) break;
      const UINT command = kRecentList1 + static_cast<UINT>(count++);
      AppendMenuW(recent, MF_STRING, command, path.wstring().c_str());
    }
    if (count == 0) AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, L"Нет недавно открытых списков");
    append_popup(recent, kRecentListsMenu, IDI_ACTION_REFRESH, L"Недавно открытые списки");
  }
  if (!settings.simple_mode) {
    AppendMenuW(file_menu_, MF_SEPARATOR, 0, nullptr);
    append(true, false, kAddDatabase, IDI_ACTION_ADD, L"Добавить базу…", L"Ctrl+Alt+F");
    append(true, false, kAddGroup, IDI_TREE_FOLDER, L"Добавить группу…", L"Ctrl+Alt+G");
    AppendMenuW(file_menu_, MF_SEPARATOR, 0, nullptr);
    append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список", L"F5");
  }
  AppendMenuW(file_menu_, MF_SEPARATOR, 0, nullptr);
  append(true, false, kExit, IDI_ACTION_EXIT, L"Выход", L"Alt+F4");
}

void MenuController::RefreshMain(const storage::Settings& settings) {
  if (!menu_ || !file_menu_ || !view_menu_ || !help_menu_) return;
  ClearMenu(view_menu_);
  ClearMenu(help_menu_);
  main_menu_items_.Clear();
  const auto append = [&](HMENU target, UINT command, int icon_resource, std::wstring text,
                          std::wstring shortcut = {}, bool checked = false) {
    main_menu_items_.Append(target, command,
        icon_resource == 0 ? nullptr : LoadResourceIcon(instance_, icon_resource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), true, checked,
        nullptr, command == kToggleFoldersFirstWhenSorting);
  };
  if (settings.simple_mode) {
    append(view_menu_, kSimpleMode, 0, L"Выйти из простого режима", L"Ctrl+Alt+M", true);
  } else {
    append(view_menu_, kToggleFavorite, IDI_ACTION_FAVORITE,
        L"Добавить/убрать из избранного", L"Ctrl+Alt+I");
    append(view_menu_, kToggleFoldersFirstWhenSorting, IDI_TREE_FOLDER,
        L"Папки всегда сверху при сортировке", {}, settings.folders_first_when_sorting);
    append(view_menu_, kEditTags, 0, L"Управление тегами выбранной базы…");
    append(view_menu_, kConfigureTagColors, 0, L"Настроить теги…");
    append(view_menu_, kShowTagsInList, 0, L"Показывать теги в списке баз", {}, settings.show_tags_in_list);
    append(view_menu_, kClearRecent, IDI_ACTION_DELETE, L"Очистить недавние базы…");
    append(view_menu_, kSimpleMode, 0, L"Простой режим", L"Ctrl+Alt+M");
    append(help_menu_, kCheckForUpdates, IDI_ACTION_UPDATE, L"Проверить обновления…");
    AppendMenuW(help_menu_, MF_SEPARATOR, 0, nullptr);
    append(help_menu_, kAbout, IDI_IBSTART, L"О программе…", L"F1");
  }
  ClearMenu(menu_);
  if (!settings.simple_mode) AppendMenuW(menu_, MF_POPUP,
      reinterpret_cast<UINT_PTR>(file_menu_), L"Файл");
  AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu_),
      settings.simple_mode ? L"Режим" : L"Вид");
  if (!settings.simple_mode) AppendMenuW(menu_, MF_POPUP,
      reinterpret_cast<UINT_PTR>(help_menu_), L"Справка");
  SetMenu(window_, menu_);
  DrawMenuBar(window_);
}

const OwnerDrawMenuItem* MenuController::Find(ULONG_PTR item_data) const noexcept {
  if (const auto* item = main_menu_items_.Find(item_data)) return item;
  return file_menu_items_.Find(item_data);
}

bool MenuController::Measure(HWND owner, HFONT font, MEASUREITEMSTRUCT* measure) const {
  if (!measure || measure->CtlType != ODT_MENU) return false;
  const auto* item = Find(measure->itemData);
  return item && OwnerDrawMenu::Measure(owner, font, *item, measure);
}

bool MenuController::Draw(HFONT font, const DRAWITEMSTRUCT* draw) const {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const auto* item = Find(draw->itemData);
  return item && OwnerDrawMenu::Draw(font, *item, draw);
}

}  // namespace ibstart::ui
