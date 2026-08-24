#pragma once

#include <Windows.h>

#include <memory>
#include <string>
#include <vector>

namespace ibstart::ui {

enum class OwnerDrawMenuIcon {
  standard,
  move_up,
  move_down,
  compact_mode,
  tag,
  sort_ascending,
  sort_descending,
};

struct OwnerDrawMenuItem {
  UINT command{};
  HICON icon{};
  std::wstring text;
  std::wstring shortcut;
  OwnerDrawMenuIcon icon_kind{OwnerDrawMenuIcon::standard};
  bool has_submenu{};
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
      OwnerDrawMenuIcon icon_kind, bool enabled, bool checked, HMENU submenu = nullptr);
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
