#pragma once

#include "core/update/update_service.hpp"

#include <Windows.h>

#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ibstart::ui::background {

class UpdateCheckOperation final {
 public:
  struct Result {
    std::optional<update::Release> release;
    std::wstring error;
    bool cancelled{false};
  };

  UpdateCheckOperation() = default;
  ~UpdateCheckOperation();

  UpdateCheckOperation(const UpdateCheckOperation&) = delete;
  UpdateCheckOperation& operator=(const UpdateCheckOperation&) = delete;

  void Start(HWND notification_window, UINT completion_message);
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool completed() const;
  [[nodiscard]] std::optional<Result> TakeResult();
  void RequestStop() noexcept;
  void StopAndJoin() noexcept;

 private:
  struct State;

  std::shared_ptr<State> state_;
  std::jthread thread_;
};

}  // namespace ibstart::ui::background
