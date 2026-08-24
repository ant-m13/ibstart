#include "ui/owner_draw_menu.hpp"

#include <algorithm>
#include <utility>

namespace ibstart::ui {
namespace {

void DrawMoveArrow(HDC context, const OwnerDrawMenuItem& item, int icon_x, int icon_y, bool disabled) {
  const COLORREF color = disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(0, 144, 162);
  const HBRUSH brush = CreateSolidBrush(color);
  const HPEN pen = CreatePen(PS_SOLID, 1, color);
  const auto previous_brush = SelectObject(context, brush);
  const auto previous_pen = SelectObject(context, pen);
  const bool up = item.icon_kind == OwnerDrawMenuIcon::move_up;
  POINT arrow[] = {{icon_x + 10, up ? icon_y + 1 : icon_y + 19}, {icon_x + 2, up ? icon_y + 9 : icon_y + 11},
      {icon_x + 7, up ? icon_y + 9 : icon_y + 11}, {icon_x + 7, up ? icon_y + 19 : icon_y + 1},
      {icon_x + 13, up ? icon_y + 19 : icon_y + 1}, {icon_x + 13, up ? icon_y + 9 : icon_y + 11},
      {icon_x + 18, up ? icon_y + 9 : icon_y + 11}};
  Polygon(context, arrow, 7);
  SelectObject(context, previous_brush);
  SelectObject(context, previous_pen);
  DeleteObject(brush);
  DeleteObject(pen);
}

void DrawCompactModeIcon(HDC context, int icon_x, int icon_y, bool disabled, bool selected) {
  const COLORREF outline = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(0, 144, 162);
  const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : RGB(231, 246, 248);
  const HBRUSH brush = CreateSolidBrush(background);
  const HPEN pen = CreatePen(PS_SOLID, 1, outline);
  const auto previous_brush = SelectObject(context, brush);
  const auto previous_pen = SelectObject(context, pen);
  RoundRect(context, icon_x + 2, icon_y + 3, icon_x + 19, icon_y + 17, 4, 4);
  SelectObject(context, previous_brush);
  SelectObject(context, previous_pen);
  DeleteObject(brush);
  if (!disabled) {
    const auto previous_line_pen = SelectObject(context, pen);
    MoveToEx(context, icon_x + 6, icon_y + 8, nullptr);
    LineTo(context, icon_x + 15, icon_y + 8);
    MoveToEx(context, icon_x + 6, icon_y + 12, nullptr);
    LineTo(context, icon_x + 12, icon_y + 12);
    SelectObject(context, previous_line_pen);
  }
  DeleteObject(pen);
}

void DrawTagIcon(HDC context, int icon_x, int icon_y, bool disabled, bool selected) {
  const COLORREF color = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(0, 144, 162);
  const HBRUSH brush = CreateSolidBrush(color);
  const HPEN pen = CreatePen(PS_SOLID, 1, color);
  const auto previous_brush = SelectObject(context, brush);
  const auto previous_pen = SelectObject(context, pen);
  POINT tag[] = {{icon_x + 2, icon_y + 3}, {icon_x + 11, icon_y + 3}, {icon_x + 18, icon_y + 10}, {icon_x + 11, icon_y + 17}, {icon_x + 2, icon_y + 17}};
  Polygon(context, tag, 5);
  SelectObject(context, previous_brush);
  SelectObject(context, previous_pen);
  const HBRUSH hole_brush = GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU);
  const HPEN hole_pen = CreatePen(PS_SOLID, 1, selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_MENU));
  const auto previous_hole_brush = SelectObject(context, hole_brush);
  const auto previous_hole_pen = SelectObject(context, hole_pen);
  Ellipse(context, icon_x + 5, icon_y + 6, icon_x + 9, icon_y + 10);
  SelectObject(context, previous_hole_brush);
  SelectObject(context, previous_hole_pen);
  DeleteObject(brush);
  DeleteObject(pen);
  DeleteObject(hole_pen);
}

void DrawSortIcon(HDC context, const OwnerDrawMenuItem& item, int icon_x, int icon_y, bool disabled, bool selected) {
  const COLORREF color = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(0, 144, 162);
  const HPEN pen = CreatePen(PS_SOLID, 2, color);
  const auto previous_pen = SelectObject(context, pen);
  const HFONT previous_font = static_cast<HFONT>(SelectObject(context, GetStockObject(DEFAULT_GUI_FONT)));
  SetTextColor(context, color);
  SetBkMode(context, TRANSPARENT);
  const bool ascending = item.icon_kind == OwnerDrawMenuIcon::sort_ascending;
  TextOutW(context, icon_x + 2, icon_y + 1, ascending ? L"А" : L"Я", 1);
  TextOutW(context, icon_x + 2, icon_y + 10, ascending ? L"Я" : L"А", 1);
  MoveToEx(context, icon_x + 17, icon_y + 3, nullptr);
  LineTo(context, icon_x + 17, icon_y + 16);
  MoveToEx(context, icon_x + 14, icon_y + 13, nullptr);
  LineTo(context, icon_x + 17, icon_y + 16);
  LineTo(context, icon_x + 20, icon_y + 13);
  SelectObject(context, previous_font);
  SelectObject(context, previous_pen);
  DeleteObject(pen);
}

void DrawCheckMark(HDC context, const OwnerDrawMenuItem& item, int icon_x, int icon_y, bool selected) {
  const bool badge = item.checked_badge || item.icon_kind == OwnerDrawMenuIcon::tag;
  if (badge) {
    const COLORREF border = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 103, 117);
    const HBRUSH badge_brush = CreateSolidBrush(RGB(255, 255, 255));
    const HPEN badge_pen = CreatePen(PS_SOLID, 1, border);
    const auto previous_brush = SelectObject(context, badge_brush);
    const auto previous_pen = SelectObject(context, badge_pen);
    Ellipse(context, icon_x + 11, icon_y + 10, icon_x + 22, icon_y + 21);
    SelectObject(context, previous_brush);
    SelectObject(context, previous_pen);
    DeleteObject(badge_brush);
    DeleteObject(badge_pen);
  }
  const COLORREF color = badge ? RGB(0, 103, 117) : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 103, 117);
  const HPEN pen = CreatePen(PS_SOLID, 2, color);
  const auto previous_pen = SelectObject(context, pen);
  POINT check[] = {{icon_x + 4, icon_y + 11}, {icon_x + 8, icon_y + 15}, {icon_x + 17, icon_y + 6}};
  if (badge) {
    check[0] = {icon_x + 13, icon_y + 15};
    check[1] = {icon_x + 16, icon_y + 18};
    check[2] = {icon_x + 20, icon_y + 13};
  }
  Polyline(context, check, 3);
  SelectObject(context, previous_pen);
  DeleteObject(pen);
}

}  // namespace

OwnerDrawMenuItems::~OwnerDrawMenuItems() { Clear(); }

OwnerDrawMenuItem& OwnerDrawMenuItems::Append(HMENU menu, UINT command, HICON icon, std::wstring text, std::wstring shortcut,
    OwnerDrawMenuIcon icon_kind, bool enabled, bool checked, HMENU submenu, bool checked_badge) {
  auto visual = std::make_unique<OwnerDrawMenuItem>(OwnerDrawMenuItem{
      command, icon, std::move(text), std::move(shortcut), icon_kind, submenu != nullptr, checked_badge});
  const auto* item_data = visual.get();
  items_.push_back(std::move(visual));
  MENUITEMINFOW item{};
  item.cbSize = sizeof(item);
  item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
  if (submenu) {
    item.fMask |= MIIM_SUBMENU;
    item.hSubMenu = submenu;
  }
  item.fType = MFT_OWNERDRAW;
  item.wID = command;
  item.fState = (enabled ? MFS_ENABLED : MFS_DISABLED) | (checked ? MFS_CHECKED : 0);
  item.dwItemData = reinterpret_cast<ULONG_PTR>(item_data);
  InsertMenuItemW(menu, static_cast<UINT>(GetMenuItemCount(menu)), TRUE, &item);
  return *items_.back();
}

const OwnerDrawMenuItem* OwnerDrawMenuItems::Find(ULONG_PTR item_data) const noexcept {
  const auto found = std::find_if(items_.begin(), items_.end(), [item_data](const auto& item) {
    return reinterpret_cast<ULONG_PTR>(item.get()) == item_data;
  });
  return found == items_.end() ? nullptr : found->get();
}

void OwnerDrawMenuItems::Clear() noexcept {
  for (const auto& item : items_) if (item->icon) DestroyIcon(item->icon);
  items_.clear();
}

bool OwnerDrawMenu::Measure(HWND owner, HFONT font, const OwnerDrawMenuItem& item, MEASUREITEMSTRUCT* measure) {
  if (!measure || measure->CtlType != ODT_MENU) return false;
  HDC context = GetDC(owner);
  if (!context) return false;
  const HFONT selected_font = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  const auto previous = SelectObject(context, selected_font);
  SIZE title_size{};
  SIZE shortcut_size{};
  GetTextExtentPoint32W(context, item.text.c_str(), static_cast<int>(item.text.size()), &title_size);
  if (!item.shortcut.empty()) GetTextExtentPoint32W(context, item.shortcut.c_str(), static_cast<int>(item.shortcut.size()), &shortcut_size);
  SelectObject(context, previous);
  ReleaseDC(owner, context);
  measure->itemHeight = 28;
  measure->itemWidth = std::max<UINT>(210u, static_cast<UINT>(title_size.cx) + static_cast<UINT>(shortcut_size.cx) + 66u);
  return true;
}

bool OwnerDrawMenu::Draw(HFONT font, const OwnerDrawMenuItem& item, const DRAWITEMSTRUCT* draw) {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
  const bool selected = (draw->itemState & ODS_SELECTED) != 0 && !disabled;
  const bool checked = (draw->itemState & ODS_CHECKED) != 0;
  const int saved = SaveDC(draw->hDC);
  FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));
  const int icon_x = draw->rcItem.left + 7;
  const int icon_y = draw->rcItem.top + (static_cast<int>(draw->rcItem.bottom - draw->rcItem.top) - 20) / 2;
  switch (item.icon_kind) {
    case OwnerDrawMenuIcon::move_up:
    case OwnerDrawMenuIcon::move_down: DrawMoveArrow(draw->hDC, item, icon_x, icon_y, disabled); break;
    case OwnerDrawMenuIcon::compact_mode: DrawCompactModeIcon(draw->hDC, icon_x, icon_y, disabled, selected); break;
    case OwnerDrawMenuIcon::tag: DrawTagIcon(draw->hDC, icon_x, icon_y, disabled, selected); break;
    case OwnerDrawMenuIcon::sort_ascending:
    case OwnerDrawMenuIcon::sort_descending: DrawSortIcon(draw->hDC, item, icon_x, icon_y, disabled, selected); break;
    case OwnerDrawMenuIcon::standard:
      if (item.icon) {
        if (disabled) DrawStateW(draw->hDC, nullptr, nullptr, reinterpret_cast<LPARAM>(item.icon), 0, icon_x, icon_y, 20, 20, DST_ICON | DSS_DISABLED);
        else DrawIconEx(draw->hDC, icon_x, icon_y, item.icon, 20, 20, 0, nullptr, DI_NORMAL);
      }
      break;
  }
  if (checked) DrawCheckMark(draw->hDC, item, icon_x, icon_y, selected);
  const HFONT selected_font = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SelectObject(draw->hDC, selected_font);
  SetBkMode(draw->hDC, TRANSPARENT);
  RECT text_rect = draw->rcItem;
  text_rect.left += 35;
  text_rect.right -= 10;
  if (item.has_submenu) text_rect.right -= 16;
  if (!item.shortcut.empty()) {
    SIZE shortcut_size{};
    GetTextExtentPoint32W(draw->hDC, item.shortcut.c_str(), static_cast<int>(item.shortcut.size()), &shortcut_size);
    RECT shortcut_rect = draw->rcItem;
    shortcut_rect.right -= 10;
    shortcut_rect.left = std::max(text_rect.left + 64, shortcut_rect.right - shortcut_size.cx);
    text_rect.right = shortcut_rect.left - 12;
    const COLORREF shortcut_color = disabled ? GetSysColor(COLOR_GRAYTEXT) : selected ? RGB(218, 242, 255) : RGB(91, 109, 121);
    SetTextColor(draw->hDC, shortcut_color);
    DrawTextW(draw->hDC, item.shortcut.c_str(), static_cast<int>(item.shortcut.size()), &shortcut_rect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
  }
  SetTextColor(draw->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : (disabled ? COLOR_GRAYTEXT : COLOR_MENUTEXT)));
  DrawTextW(draw->hDC, item.text.c_str(), static_cast<int>(item.text.size()), &text_rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (item.has_submenu) {
    const COLORREF arrow_color = disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT);
    const HPEN pen = CreatePen(PS_SOLID, 1, arrow_color);
    const auto previous_pen = SelectObject(draw->hDC, pen);
    const int center_y = (static_cast<int>(draw->rcItem.top) + static_cast<int>(draw->rcItem.bottom)) / 2;
    MoveToEx(draw->hDC, draw->rcItem.right - 15, center_y - 4, nullptr);
    LineTo(draw->hDC, draw->rcItem.right - 10, center_y);
    LineTo(draw->hDC, draw->rcItem.right - 15, center_y + 4);
    SelectObject(draw->hDC, previous_pen);
    DeleteObject(pen);
  }
  if (draw->itemState & ODS_FOCUS) DrawFocusRect(draw->hDC, &draw->rcItem);
  RestoreDC(draw->hDC, saved);
  return true;
}

}  // namespace ibstart::ui
