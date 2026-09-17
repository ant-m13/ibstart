#pragma once

#include "core/cache/cache_service.hpp"

#include <Windows.h>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ibstart::ui::background {

// Sequential, cancellable cache scan for an immutable snapshot of catalog
// databases.  The worker never reads Catalog or HWND-owned UI state; it only
// posts a notification after appending a value to the protected result queue.
class CacheSizeOperation final {
 public:
  struct Result {
    std::wstring database_id;
    std::wstring connect;
    cache::ScanResult scan;
    std::size_t completed{};
    std::size_t total{};
  };

  CacheSizeOperation() = default;
  ~CacheSizeOperation();

  CacheSizeOperation(const CacheSizeOperation&) = delete;
  CacheSizeOperation& operator=(const CacheSizeOperation&) = delete;

  void Start(std::vector<domain::Database> databases, HWND notification_window,
      UINT result_message);
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool completed() const;
  [[nodiscard]] bool cancelled() const;
  [[nodiscard]] bool has_pending_results() const;
  [[nodiscard]] std::size_t total() const;
  [[nodiscard]] std::size_t processed() const;
  [[nodiscard]] std::vector<Result> TakeResults(std::size_t maximum);
  void RequestStop() noexcept;
  void StopAndJoin() noexcept;

 private:
  struct State;

  std::shared_ptr<State> state_;
  std::jthread thread_;
};

}  // namespace ibstart::ui::background
