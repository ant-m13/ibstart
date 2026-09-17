#include "ui/cache_clear_operation.hpp"

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

struct CacheClearOperation::State {
  std::mutex mutex;
  Result result;
  bool completed{false};
};

CacheClearOperation::~CacheClearOperation() {
  StopAndJoin();
}

void CacheClearOperation::StartFinding(
    const domain::Database& database, HWND notification_window, UINT completion_message) {
  if (active()) throw std::logic_error("Cache operation is already active.");

  auto state = std::make_shared<State>();
  state->result.stage = Stage::finding;
  state->result.database = database;
  state_ = state;
  try {
    thread_ = std::jthread([state, notification_window, completion_message, database](std::stop_token stop) {
      Result result;
      result.stage = Stage::finding;
      result.database = database;
      try {
        if (!stop.stop_requested()) {
          result.scan_result = cache::Scan(database, stop);
          result.candidates = result.scan_result.items;
        }
        result.cancelled = stop.stop_requested() || result.scan_result.cancelled;
      } catch (const std::exception& error) {
        if (stop.stop_requested()) result.cancelled = true;
        else result.error = WideErrorText(error.what());
      } catch (...) {
        if (stop.stop_requested()) result.cancelled = true;
        else result.error = L"Неизвестная ошибка анализа кэша.";
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

void CacheClearOperation::StartClearing(
    std::vector<cache::CacheItem> candidates, HWND notification_window, UINT completion_message) {
  StartClearing(std::move(candidates), std::nullopt, notification_window, completion_message);
}

void CacheClearOperation::StartClearing(std::vector<cache::CacheItem> candidates,
    domain::Database database, HWND notification_window, UINT completion_message) {
  StartClearing(std::move(candidates), std::optional<domain::Database>(std::move(database)),
      notification_window, completion_message);
}

void CacheClearOperation::StartClearing(std::vector<cache::CacheItem> candidates,
    std::optional<domain::Database> database, HWND notification_window, UINT completion_message) {
  if (active()) throw std::logic_error("Cache operation is already active.");

  auto state = std::make_shared<State>();
  state->result.stage = Stage::clearing;
  state->result.database = database;
  state_ = state;
  try {
    thread_ = std::jthread(
        [state, notification_window, completion_message, candidates = std::move(candidates),
            database = std::move(database)](std::stop_token stop) {
          Result result;
          result.stage = Stage::clearing;
          result.database = database;
          try {
            result.clear_result = cache::Clear(candidates, stop);
            if (!result.clear_result.cancelled && !stop.stop_requested() && database) {
              result.rescan_result = cache::Scan(*database, stop);
              if (result.rescan_result->cancelled) result.cancelled = true;
            }
            result.cancelled = result.cancelled || result.clear_result.cancelled || stop.stop_requested();
          } catch (const std::exception& error) {
            if (stop.stop_requested()) result.cancelled = true;
            else result.error = WideErrorText(error.what());
          } catch (...) {
            if (stop.stop_requested()) result.cancelled = true;
            else result.error = L"Неизвестная ошибка очистки кэша.";
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

bool CacheClearOperation::active() const noexcept {
  return static_cast<bool>(state_);
}

bool CacheClearOperation::completed() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->completed;
}

bool CacheClearOperation::clearing() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->result.stage == Stage::clearing;
}

std::optional<CacheClearOperation::Result> CacheClearOperation::TakeResult() {
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

void CacheClearOperation::RequestStop() noexcept {
  if (thread_.joinable()) static_cast<void>(thread_.request_stop());
}

void CacheClearOperation::StopAndJoin() noexcept {
  RequestStop();
  if (thread_.joinable()) thread_.join();
  state_.reset();
}

}  // namespace ibstart::ui::background
