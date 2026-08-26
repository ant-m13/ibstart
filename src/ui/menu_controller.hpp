#pragma once

#include "core/storage/storage.hpp"
#include "ui/owner_draw_menu.hpp"

#include <Windows.h>

namespace ibstart::ui {

class MenuController final {
 public:
  MenuController() = default;
  ~MenuController();

  MenuController(const MenuController&) = delete;
  MenuController& operator=(const MenuController&) = delete;

  void Create(HWND window, HINSTANCE instance);
  void RefreshFile(const storage::Settings& settings);
  void RefreshMain(const storage::Settings& settings);
  void Clear() noexcept;

  [[nodiscard]] HMENU file_menu() const noexcept { return file_menu_; }
  [[nodiscard]] HMENU view_menu() const noexcept { return view_menu_; }
  [[nodiscard]] HMENU help_menu() const noexcept { return help_menu_; }
  [[nodiscard]] const OwnerDrawMenuItem* Find(ULONG_PTR item_data) const noexcept;

 private:
  HWND window_{};
  HINSTANCE instance_{};
  HMENU menu_{};
  HMENU file_menu_{};
  HMENU view_menu_{};
  HMENU help_menu_{};
  OwnerDrawMenuItems main_menu_items_;
  OwnerDrawMenuItems file_menu_items_;
};

}  // namespace ibstart::ui
