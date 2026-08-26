#pragma once

#include "ui/owner_draw_menu.hpp"

#include <Windows.h>

#include <span>
#include <string>

namespace ibstart::ui {

struct TreeContextMenuState {
  bool simple_mode{};
  bool sort_target{};
  bool catalog_root{};
  bool database{};
  bool web{};
  bool launch_available{};
  bool group{};
  bool editable{};
  bool file{};
  bool recent_root{};
  bool favorite{};
  std::wstring add_parent;
  std::wstring sort_parent;
  std::span<const std::wstring> quick_tags;
};

class ContextMenuController final {
 public:
  ContextMenuController() = default;
  ~ContextMenuController();

  ContextMenuController(const ContextMenuController&) = delete;
  ContextMenuController& operator=(const ContextMenuController&) = delete;

  void Create(HINSTANCE instance) noexcept { instance_ = instance; }
  void Clear() noexcept;

  [[nodiscard]] UINT ShowTree(HWND owner, POINT screen, const TreeContextMenuState& state);
  [[nodiscard]] UINT ShowDetails(HWND owner, POINT screen);
  [[nodiscard]] bool Measure(HWND owner, HFONT font, MEASUREITEMSTRUCT* measure) const;
  [[nodiscard]] bool Draw(HFONT font, const DRAWITEMSTRUCT* draw) const;

 private:
  [[nodiscard]] UINT Track(HWND owner, HMENU menu, POINT screen);

  HINSTANCE instance_{};
  OwnerDrawMenuItems items_;
};

}  // namespace ibstart::ui
