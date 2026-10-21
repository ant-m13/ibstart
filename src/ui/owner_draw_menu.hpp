#pragma once

#include <Windows.h>

#include <memory>
#include <string>
#include <vector>

namespace ibstart::ui {

enum class OwnerDrawMenuIcon {
  // Keep existing IDs stable when adding icons used by separate UI modules.
  standard = 0,
  move_up = 1,
  move_down = 2,
  compact_mode = 3,
  settings = 4,
  tag = 5,
  launch_parameters = 6,
  sort_ascending = 7,
  sort_descending = 8,
  credentials = 9,
  configurations = 10,
};

struct OwnerDrawMenuItem {
  UINT command{};
  HICON icon{};
  std::wstring text;
  std::wstring shortcut;
  OwnerDrawMenuIcon icon_kind{OwnerDrawMenuIcon::standard};
  bool has_submenu{};
  // Places the check in a small badge so it does not obscure the item icon.
  bool checked_badge{};
};

// Keeps item-data pointers stable while a native menu is displayed and owns
// all HICON handles assigned to its visual items.
class OwnerDrawMenuItems {
 public:
  OwnerDrawMenuItems() = default;
  OwnerDrawMenuItems(const OwnerDrawMenuItems&) = delete;
  OwnerDrawMenuItems& operator=(const OwnerDrawMenuItems&) = delete;
  ~OwnerDrawMenuItems();

  OwnerDrawMenuItem& Append(HMENU menu, UINT command, HICON icon, std::wstring text, std::wstring shortcut,
      OwnerDrawMenuIcon icon_kind, bool enabled, bool checked, HMENU submenu = nullptr, bool checked_badge = false);
  [[nodiscard]] const OwnerDrawMenuItem* Find(ULONG_PTR item_data) const noexcept;
  void Clear() noexcept;

 private:
  std::vector<std::unique_ptr<OwnerDrawMenuItem>> items_;
};

class OwnerDrawMenu {
 public:
  [[nodiscard]] static bool Measure(HWND owner, HFONT font, const OwnerDrawMenuItem& item, MEASUREITEMSTRUCT* measure);
  [[nodiscard]] static bool Draw(HFONT font, const OwnerDrawMenuItem& item, const DRAWITEMSTRUCT* draw);
};

}  // namespace ibstart::ui
