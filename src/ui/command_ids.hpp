#pragma once

#include <Windows.h>

namespace ibstart::ui::commands {

enum Id : int {
  kEnterprise = 100,
  kDesigner,
  kLaunchWithParameters,
  kEdit,
  kCache,
  kShortcut,
  kDelete,
  kAddDatabase,
  kAddGroup,
  kOpenList,
  kRefresh,
  kSimpleMode,
  kToggleFavorite,
  kFocusSearch,
  kClearSearch,
  kCheckForUpdates,
  kAbout,
  kMoveUp,
  kMoveDown,
  kOpenFolder,
  kClearRecent,
  kRemoveRecentDatabase,
  kCopyDetailValue,
  kCopyDetailPair,
  kEditTags,
  kConfigureTagColors,
  kSortAscending,
  kSortDescending,
  kToggleFoldersFirstWhenSorting,
  kMoveToFolder,
  kOpenStandardList,
  kShowTagsInList,
  kCacheSize,
  kCacheSizeAll,
  kCacheSizeFolder,
  kShowCacheSizeInList,
  kNewTagForSelected,
  kExit,
  kConfigureApplicationSettings,
  kConfigureCredentials,
  kFavorite1 = 200,
};

inline constexpr UINT kRecentList1 = 300;
inline constexpr UINT kRemoveRecentList1 = 320;
inline constexpr UINT kQuickTag1 = 400;
inline constexpr UINT kTagsContextMenu = 250;
inline constexpr UINT kRecentListsMenu = 299;

}  // namespace ibstart::ui::commands
