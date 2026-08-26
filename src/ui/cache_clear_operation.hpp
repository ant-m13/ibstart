#pragma once

#include "core/cache/cache_service.hpp"

#include <Windows.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ibstart::ui::background {

class CacheClearOperation final {
 public:
  enum class Stage { finding, clearing };

  struct Result {
    Stage stage{Stage::finding};
    std::vector<cache::CacheItem> candidates;
    cache::ClearResult clear_result;
    std::wstring error;
    bool cancelled{false};
  };

  CacheClearOperation() = default;
  ~CacheClearOperation();

  CacheClearOperation(const CacheClearOperation&) = delete;
  CacheClearOperation& operator=(const CacheClearOperation&) = delete;

  void StartFinding(const domain::Database& database, HWND notification_window, UINT completion_message);
  void StartClearing(
      std::vector<cache::CacheItem> candidates, HWND notification_window, UINT completion_message);
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool completed() const;
  [[nodiscard]] bool clearing() const;
  [[nodiscard]] std::optional<Result> TakeResult();
  void RequestStop() noexcept;
  void StopAndJoin() noexcept;

 private:
  struct State;

  std::shared_ptr<State> state_;
  std::jthread thread_;
};

}  // namespace ibstart::ui::background
