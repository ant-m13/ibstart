#include "ui/context_menu_controller.hpp"

#include "app/resource.h"
#include "ui/command_ids.hpp"

#include <cstddef>
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

}  // namespace

ContextMenuController::~ContextMenuController() {
  Clear();
}

void ContextMenuController::Clear() noexcept {
  items_.Clear();
  instance_ = nullptr;
}

UINT ContextMenuController::Track(HWND owner, HMENU menu, POINT screen) {
  if (!menu) return 0;
  SetForegroundWindow(owner);
  const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
      screen.x, screen.y, owner, nullptr);
  DestroyMenu(menu);
  items_.Clear();
  return command;
}

UINT ContextMenuController::ShowDetails(HWND owner, POINT screen) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return 0;
  AppendMenuW(menu, MF_STRING, kCopyDetailValue, L"Копировать значение\tCtrl+C");
  AppendMenuW(menu, MF_STRING, kCopyDetailPair, L"Копировать параметр и значение");
  return Track(owner, menu, screen);
}

UINT ContextMenuController::ShowTree(HWND owner, POINT screen, const TreeContextMenuState& state) {
  items_.Clear();
  HMENU menu = CreatePopupMenu();
  if (!menu) return 0;

  const auto append_to = [&](HMENU target, bool enabled, bool checked, UINT command,
                             int icon_resource, std::wstring text, std::wstring shortcut = {}) {
    items_.Append(target, command,
        icon_resource == 0 ? nullptr : LoadResourceIcon(instance_, icon_resource, 20),
        std::move(text), std::move(shortcut), MenuIconForCommand(command), enabled, checked);
  };
  const auto append = [&](bool enabled, bool checked, UINT command, int icon_resource,
                          std::wstring text, std::wstring shortcut = {}) {
    append_to(menu, enabled, checked, command, icon_resource, std::move(text), std::move(shortcut));
  };
  const auto append_popup = [&](HMENU submenu, UINT identity, std::wstring text) {
    items_.Append(menu, identity, nullptr, std::move(text), {}, MenuIconForCommand(identity),
        true, false, submenu);
  };
  const auto separator = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };

  if (state.simple_mode) {
    if (state.sort_target) {
      append(true, false, kSortAscending, 0, L"Сортировать по возрастанию");
      append(true, false, kSortDescending, 0, L"Сортировать по убыванию");
      return Track(owner, menu, screen);
    }
    if (state.database) {
      append(state.launch_available, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
      append(state.launch_available && !state.web, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
      separator();
      append(true, false, kEdit, IDI_ACTION_EDIT, L"Изменить…", L"F2");
      append(true, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
      separator();
      append(true, false, kMoveToFolder, IDI_TREE_FOLDER, L"Переместить в папку…");
      append(true, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
      append(true, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
    } else {
      DestroyMenu(menu);
      items_.Clear();
      return 0;
    }
    return Track(owner, menu, screen);
  }

  if (!state.catalog_root) {
    append(state.launch_available, false, kEnterprise, IDI_ACTION_ENTERPRISE, L"Предприятие", L"F3");
    append(state.launch_available && !state.web, false, kDesigner, IDI_ACTION_DESIGNER, L"Конфигуратор", L"F4");
    separator();
    append(state.database, state.favorite, kToggleFavorite, IDI_ACTION_FAVORITE,
        state.favorite ? L"Убрать из избранного" : L"Добавить в избранное", L"Ctrl+Alt+I");
    if (state.database) {
      HMENU tag_menu = CreatePopupMenu();
      if (tag_menu) {
        AppendMenuW(tag_menu, MF_STRING, kEditTags, L"Управление тегами…");
        AppendMenuW(tag_menu, MF_SEPARATOR, 0, nullptr);
        for (std::size_t index = 0; index < state.quick_tags.size(); ++index) {
          AppendMenuW(tag_menu, MF_STRING, kQuickTag1 + static_cast<UINT>(index),
              state.quick_tags[index].c_str());
        }
        if (state.quick_tags.empty()) {
          AppendMenuW(tag_menu, MF_STRING | MF_GRAYED, 0,
              L"Нет доступных тегов для добавления");
        }
        AppendMenuW(tag_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(tag_menu, MF_STRING, kNewTagForSelected, L"Новый тег…");
        AppendMenuW(tag_menu, MF_STRING, kConfigureTagColors, L"Настроить теги…");
        append_popup(tag_menu, kTagsContextMenu, L"Теги");
      }
    }
    append(state.editable, false, kEdit, IDI_ACTION_EDIT, L"Изменить…", L"F2");
    append(state.database, false, kCache, IDI_ACTION_CACHE, L"Очистить кэш…", L"Ctrl+Shift+Del");
    append(state.database, false, kShortcut, IDI_ACTION_SHORTCUT, L"Создать ярлык", L"Ctrl+Shift+S");
    append(state.file, false, kOpenFolder, IDI_TREE_FOLDER, L"Открыть папку", L"Ctrl+Shift+O");
    append(state.recent_root, false, kClearRecent, IDI_ACTION_DELETE, L"Очистить недавние базы…");
    separator();
    append(state.editable, false, kMoveUp, 0, L"Переместить вверх", L"Ctrl+Shift+Up");
    append(state.editable, false, kMoveDown, 0, L"Переместить вниз", L"Ctrl+Shift+Down");
    append(state.editable, false, kMoveToFolder, IDI_TREE_FOLDER, L"Переместить в папку…");
    append(state.editable, false, kDelete, IDI_ACTION_DELETE, L"Удалить…", L"Alt+Shift+Del");
  }
  if (state.sort_target) {
    if (!state.catalog_root) separator();
    append(true, false, kSortAscending, 0, L"Сортировать по возрастанию");
    append(true, false, kSortDescending, 0, L"Сортировать по убыванию");
  }
  separator();
  append(!state.simple_mode, false, kAddDatabase, IDI_ACTION_ADD,
      state.group ? L"Добавить базу в группу…" : L"Добавить базу…", L"Ctrl+Alt+F");
  append(!state.simple_mode, false, kAddGroup, IDI_TREE_FOLDER,
      state.group ? L"Добавить вложенную группу…" : L"Добавить группу…", L"Ctrl+Alt+G");
  separator();
  append(true, false, kRefresh, IDI_ACTION_REFRESH, L"Обновить список", L"F5");
  return Track(owner, menu, screen);
}

bool ContextMenuController::Measure(HWND owner, HFONT font, MEASUREITEMSTRUCT* measure) const {
  if (!measure || measure->CtlType != ODT_MENU) return false;
  const auto* item = items_.Find(measure->itemData);
  return item && OwnerDrawMenu::Measure(owner, font, *item, measure);
}

bool ContextMenuController::Draw(HFONT font, const DRAWITEMSTRUCT* draw) const {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const auto* item = items_.Find(draw->itemData);
  return item && OwnerDrawMenu::Draw(font, *item, draw);
}

}  // namespace ibstart::ui
