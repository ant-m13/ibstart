#include "ui/cache_size_operation.hpp"

#include "core/domain/utf.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ibstart::ui::background {
namespace {

std::wstring StableDatabaseId(const domain::Database& database) {
  return database.id.empty() ? database.name : database.id;
}

std::wstring WideErrorText(std::string_view message) noexcept {
  try {
    return utf::FromUtf8(message);
  } catch (...) {
    return std::wstring(message.begin(), message.end());
  }
}

}  // namespace

struct CacheSizeOperation::State {
  mutable std::mutex mutex;
  std::deque<Result> results;
  std::size_t total{};
  std::size_t processed{};
  bool completed{};
  bool cancelled{};
};

CacheSizeOperation::~CacheSizeOperation() {
  StopAndJoin();
}

void CacheSizeOperation::Start(std::vector<domain::Database> databases, HWND notification_window,
    UINT result_message) {
  if (active()) throw std::logic_error("Cache size operation is already active.");

  auto state = std::make_shared<State>();
  state->total = databases.size();
  state_ = state;
  try {
    thread_ = std::jthread([state, notification_window, result_message,
                               databases = std::move(databases)](std::stop_token stop) {
      for (const auto& database : databases) {
        if (stop.stop_requested()) {
          std::lock_guard lock(state->mutex);
          state->cancelled = true;
          break;
        }

        Result result;
        result.database_id = StableDatabaseId(database);
        result.connect = database.connect;
        try {
          result.scan = cache::Scan(database, stop);
        } catch (const std::exception& error) {
          if (stop.stop_requested()) {
            std::lock_guard lock(state->mutex);
            state->cancelled = true;
            break;
          }
          result.scan.complete = false;
          result.scan.errors.push_back(WideErrorText(error.what()));
        } catch (...) {
          if (stop.stop_requested()) {
            std::lock_guard lock(state->mutex);
            state->cancelled = true;
            break;
          }
          result.scan.complete = false;
          result.scan.errors.push_back(L"Неизвестная ошибка сканирования кэша.");
        }

        if (result.scan.cancelled || stop.stop_requested()) {
          std::lock_guard lock(state->mutex);
          state->cancelled = true;
          break;
        }

        {
          std::lock_guard lock(state->mutex);
          ++state->processed;
          result.completed = state->processed;
          result.total = state->total;
          state->results.push_back(std::move(result));
        }
        if (notification_window) PostMessageW(notification_window, result_message, 0, 0);
      }

      {
        std::lock_guard lock(state->mutex);
        state->cancelled = state->cancelled || stop.stop_requested();
        state->completed = true;
      }
      if (notification_window) PostMessageW(notification_window, result_message, 0, 0);
    });
  } catch (...) {
    state_.reset();
    throw;
  }
}

bool CacheSizeOperation::active() const noexcept {
  return static_cast<bool>(state_);
}

bool CacheSizeOperation::completed() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->completed;
}

bool CacheSizeOperation::cancelled() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return state->cancelled;
}

bool CacheSizeOperation::has_pending_results() const {
  const auto state = state_;
  if (!state) return false;
  std::lock_guard lock(state->mutex);
  return !state->results.empty();
}

std::size_t CacheSizeOperation::total() const {
  const auto state = state_;
  if (!state) return 0;
  std::lock_guard lock(state->mutex);
  return state->total;
}

std::size_t CacheSizeOperation::processed() const {
  const auto state = state_;
  if (!state) return 0;
  std::lock_guard lock(state->mutex);
  return state->processed;
}

std::vector<CacheSizeOperation::Result> CacheSizeOperation::TakeResults(std::size_t maximum) {
  std::vector<Result> result;
  if (maximum == 0) return result;
  const auto state = state_;
  if (!state) return result;
  std::lock_guard lock(state->mutex);
  const auto count = std::min(maximum, state->results.size());
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(std::move(state->results.front()));
    state->results.pop_front();
  }
  return result;
}

void CacheSizeOperation::RequestStop() noexcept {
  if (thread_.joinable()) static_cast<void>(thread_.request_stop());
}

void CacheSizeOperation::StopAndJoin() noexcept {
  RequestStop();
  if (thread_.joinable()) thread_.join();
  state_.reset();
}

}  // namespace ibstart::ui::background
