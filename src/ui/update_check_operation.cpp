#include "ui/update_check_operation.hpp"

#include "core/domain/utf.hpp"

#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ibstart::ui::background {
namespace {

std::wstring WideErrorText(std::string_view message) noexcept {
  try {
    return utf::FromUtf8(message);
  } catch (...) {
    return std::wstring(message.begin(), message.end());
  }
}

}  // namespace

struct UpdateCheckOperation::State {
  std::mutex mutex;
  Result result;
  bool completed{false};
};

UpdateCheckOperation::~UpdateCheckOperation() {
  StopAndJoin();
}

void UpdateCheckOperation::Start(HWND notification_window, UINT completion_message) {
  if (active()) throw std::logic_error("Update check is already active.");

  auto state = std::make_shared<State>();
  state_ = state;
  try {
    thread_ = std::jthread([state, notification_window, completion_message](std::stop_token stop) {
      Result result;
      try {
        if (!stop.stop_requested()) result.release = update::FetchLatestRelease(stop);
        result.cancelled = stop.stop_requested();
      } catch (const std::exception& error) {
        if (stop.stop_requested()) result.cancelled = true;
        else result.error = WideErrorText(error.what());
      } catch (...) {
        if (stop.stop_requested()) result.cancelled = true;
        else result.error = L"Неизвестная ошибка проверки обновлений.";
      }
      {
        std::lock_guard lock(state->mutex);
        state->result = std::move(result);
        state->completed = true;
      }
      PostMessageW(notification_window, completion_message, 0, 0);
    });
  } catch (...) {
    state_.reset();
    throw;
  }
}

bool UpdateCheckOperation::active() const noexcept {
  return static_cast<bool>(state_);
}

bool UpdateCheckOperation::completed() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->completed;
}

std::optional<UpdateCheckOperation::Result> UpdateCheckOperation::TakeResult() {
  const auto state = state_;
  if (!state) return std::nullopt;

  Result result;
  {
    std::lock_guard lock(state->mutex);
    if (!state->completed) return std::nullopt;
    result = std::move(state->result);
  }
  if (thread_.joinable()) thread_.join();
  state_.reset();
  return result;
}

void UpdateCheckOperation::RequestStop() noexcept {
  if (thread_.joinable()) static_cast<void>(thread_.request_stop());
}

void UpdateCheckOperation::StopAndJoin() noexcept {
  RequestStop();
  if (thread_.joinable()) thread_.join();
  state_.reset();
}

}  // namespace ibstart::ui::background
