#include "core/update/github_release_client.hpp"

#include "core/domain/version.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ibstart::update::transport {
namespace {

constexpr wchar_t kGitHubHost[] = L"github.com";
constexpr wchar_t kRequestHeaders[] = L"Accept: text/plain\r\n";
constexpr size_t kMaximumResponseSize = 1024 * 1024;
constexpr size_t kReadBufferSize = 16 * 1024;
constexpr DWORD kHttpOk = 200;
constexpr DWORD kHttpNotFound = 404;

class InternetHandle {
 public:
  explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
  ~InternetHandle() { if (value_) WinHttpCloseHandle(value_); }

  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  InternetHandle(InternetHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  InternetHandle& operator=(InternetHandle&& other) noexcept {
    if (this != &other) {
      if (value_) WinHttpCloseHandle(value_);
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HINTERNET get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
  [[nodiscard]] HINTERNET release() noexcept { return std::exchange(value_, nullptr); }

 private:
  HINTERNET value_{};
};

[[noreturn]] void ThrowWinHttpError(std::string_view operation, DWORD error) {
  throw std::runtime_error(std::string(operation) + " failed (Windows error " +
      std::to_string(error) + ").");
}

[[noreturn]] void ThrowWinHttpError(std::string_view operation) {
  ThrowWinHttpError(operation, GetLastError());
}

enum class CompletionKind {
  send_request,
  headers_available,
  data_available,
  read_complete,
  request_error,
};

struct Completion {
  CompletionKind kind{};
  DWORD value{};
  DWORD error{};
};

struct AsyncRequestState {
  std::mutex mutex;
  std::condition_variable condition;
  // WinHTTP requires that a handle is not closed while another thread is
  // inside a WinHTTP call using it. Every request call and handle close uses
  // this mutex; asynchronous callbacks never call WinHTTP themselves.
  std::mutex operation_mutex;
  HINTERNET request{};
  bool closing{false};
  bool handle_closed{false};
  std::optional<Completion> completion;
  std::array<char, kReadBufferSize> read_buffer{};
};

void CALLBACK AsyncStatusCallback(HINTERNET, DWORD_PTR context, DWORD status,
    LPVOID status_information, DWORD status_information_length) noexcept {
  auto* state = reinterpret_cast<AsyncRequestState*>(context);
  if (!state) return;

  try {
    std::lock_guard lock(state->mutex);
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
      state->handle_closed = true;
      state->condition.notify_all();
      return;
    }
    if (state->closing) return;

    Completion completion{};
    switch (status) {
      case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        completion.kind = CompletionKind::send_request;
        break;
      case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        completion.kind = CompletionKind::headers_available;
        break;
      case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        completion.kind = CompletionKind::data_available;
        if (!status_information || status_information_length < sizeof(DWORD)) {
          completion.kind = CompletionKind::request_error;
          completion.error = ERROR_WINHTTP_INTERNAL_ERROR;
        } else {
          completion.value = *static_cast<const DWORD*>(status_information);
        }
        break;
      case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        completion.kind = CompletionKind::read_complete;
        completion.value = status_information_length;
        break;
      case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        completion.kind = CompletionKind::request_error;
        if (status_information && status_information_length >= sizeof(WINHTTP_ASYNC_RESULT)) {
          const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(status_information);
          completion.error = result->dwError;
        } else {
          completion.error = ERROR_WINHTTP_INTERNAL_ERROR;
        }
        break;
      default:
        return;
    }
    state->completion = completion;
    state->condition.notify_all();
  } catch (...) {
    // No exception may escape a WinHTTP callback. The state only contains
    // fixed-size callback data, so this is limited to an exceptional mutex
    // failure for which there is no safe recovery path.
  }
}

void CloseRequest(const std::shared_ptr<AsyncRequestState>& state) noexcept {
  HINTERNET request{};
  {
    std::lock_guard operation_lock(state->operation_mutex);
    {
      std::lock_guard lock(state->mutex);
      state->closing = true;
      state->completion.reset();
      request = std::exchange(state->request, nullptr);
      state->condition.notify_all();
    }
    if (request) static_cast<void>(WinHttpCloseHandle(request));
  }
}

void CloseRequestAndWait(const std::shared_ptr<AsyncRequestState>& state) noexcept {
  CloseRequest(state);
  std::unique_lock lock(state->mutex);
  state->condition.wait(lock, [&] { return state->handle_closed; });
}

[[nodiscard]] bool IsClosing(const std::shared_ptr<AsyncRequestState>& state) {
  std::lock_guard lock(state->mutex);
  return state->closing;
}

template <typename Function>
bool StartAsyncOperation(const std::shared_ptr<AsyncRequestState>& state,
    std::string_view operation, Function&& function) {
  std::lock_guard operation_lock(state->operation_mutex);
  HINTERNET request{};
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || state->handle_closed || !state->request) return false;
    state->completion.reset();
    request = state->request;
  }

  if (function(request)) return true;
  const DWORD error = GetLastError();
  if (error == ERROR_IO_PENDING) return true;
  ThrowWinHttpError(operation, error);
}

[[nodiscard]] std::optional<Completion> WaitForCompletion(
    const std::shared_ptr<AsyncRequestState>& state) {
  std::unique_lock lock(state->mutex);
  state->condition.wait(lock, [&] { return state->closing || state->completion.has_value(); });
  if (state->closing) return std::nullopt;
  const Completion completion = *state->completion;
  state->completion.reset();
  return completion;
}

[[nodiscard]] std::optional<DWORD> QueryResponseStatus(
    const std::shared_ptr<AsyncRequestState>& state) {
  std::lock_guard operation_lock(state->operation_mutex);
  HINTERNET request{};
  {
    std::lock_guard lock(state->mutex);
    if (state->closing || state->handle_closed || !state->request) return std::nullopt;
    request = state->request;
  }

  DWORD status{};
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
    ThrowWinHttpError("WinHttpQueryHeaders");
  }
  return status;
}

[[nodiscard]] std::optional<std::string> RunAsyncRequest(
    const std::shared_ptr<AsyncRequestState>& state) {
  if (!StartAsyncOperation(state, "WinHttpSendRequest", [](HINTERNET request) {
        return WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
      })) {
    return std::nullopt;
  }

  std::string response;
  for (;;) {
    const auto completion = WaitForCompletion(state);
    if (!completion) return std::nullopt;

    switch (completion->kind) {
      case CompletionKind::send_request:
        if (!StartAsyncOperation(state, "WinHttpReceiveResponse", [](HINTERNET request) {
              return WinHttpReceiveResponse(request, nullptr);
            })) {
          return std::nullopt;
        }
        break;

      case CompletionKind::headers_available: {
        const auto status = QueryResponseStatus(state);
        if (!status) return std::nullopt;
        if (*status == kHttpNotFound) return std::nullopt;
        if (*status != kHttpOk) {
          throw std::runtime_error("GitHub version asset returned HTTP status " +
              std::to_string(*status) + ".");
        }
        if (!StartAsyncOperation(state, "WinHttpQueryDataAvailable", [](HINTERNET request) {
              return WinHttpQueryDataAvailable(request, nullptr);
            })) {
          return std::nullopt;
        }
        break;
      }

      case CompletionKind::data_available:
        if (completion->value == 0) return response;
        if (completion->value > kMaximumResponseSize - response.size()) {
          throw std::runtime_error("GitHub version asset exceeds the 1 MiB safety limit.");
        }
        if (!StartAsyncOperation(state, "WinHttpReadData", [state, read_size = std::min<DWORD>(
                completion->value, static_cast<DWORD>(state->read_buffer.size()))](HINTERNET request) {
              return WinHttpReadData(request, state->read_buffer.data(), read_size, nullptr);
            })) {
          return std::nullopt;
        }
        break;

      case CompletionKind::read_complete:
        if (completion->value == 0) return response;
        if (completion->value > state->read_buffer.size() ||
            completion->value > kMaximumResponseSize - response.size()) {
          throw std::runtime_error("GitHub version asset exceeds the 1 MiB safety limit.");
        }
        response.append(state->read_buffer.data(), completion->value);
        if (!StartAsyncOperation(state, "WinHttpQueryDataAvailable", [](HINTERNET request) {
              return WinHttpQueryDataAvailable(request, nullptr);
            })) {
          return std::nullopt;
        }
        break;

      case CompletionKind::request_error:
        ThrowWinHttpError("WinHTTP asynchronous request", completion->error);
    }
  }
}

}  // namespace

std::optional<std::string> FetchVersionAsset(const VersionAssetEndpoint& endpoint,
    std::stop_token stop) {
  if (stop.stop_requested()) return std::nullopt;

  const std::wstring host(endpoint.host);
  const std::wstring path(endpoint.path);
  const DWORD access_type = endpoint.use_default_proxy
      ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
  InternetHandle session(WinHttpOpen(L"IBStart update checker", access_type,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
  if (!session) ThrowWinHttpError("WinHttpOpen");
  if (!WinHttpSetTimeouts(session.get(), 4000, 4000, 6000, 6000)) {
    ThrowWinHttpError("WinHttpSetTimeouts");
  }
  if (stop.stop_requested()) return std::nullopt;

  InternetHandle connection(WinHttpConnect(session.get(), host.c_str(),
      static_cast<INTERNET_PORT>(endpoint.port), 0));
  if (!connection) ThrowWinHttpError("WinHttpConnect");
  if (stop.stop_requested()) return std::nullopt;
  InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
      endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request) ThrowWinHttpError("WinHttpOpenRequest");
  DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
      sizeof(redirectPolicy))) {
    ThrowWinHttpError("WinHttpSetOption");
  }
  constexpr DWORD kNullTerminatedHeaderLength = static_cast<DWORD>(-1);
  if (!WinHttpAddRequestHeaders(request.get(), kRequestHeaders, kNullTerminatedHeaderLength,
      WINHTTP_ADDREQ_FLAG_ADD)) {
    ThrowWinHttpError("WinHttpAddRequestHeaders");
  }
  if (stop.stop_requested()) return std::nullopt;

  auto state = std::make_shared<AsyncRequestState>();
  DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state.get());
  if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
    ThrowWinHttpError("WinHttpSetOption");
  }
  constexpr DWORD kCallbackFlags = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
      WINHTTP_CALLBACK_FLAG_HANDLES;
  if (WinHttpSetStatusCallback(request.get(), &AsyncStatusCallback, kCallbackFlags, 0) ==
      WINHTTP_INVALID_STATUS_CALLBACK) {
    ThrowWinHttpError("WinHttpSetStatusCallback");
  }

  state->request = request.release();
  try {
    std::stop_callback cancellation(stop, [state]() noexcept { CloseRequest(state); });
    try {
      const auto response = RunAsyncRequest(state);
      CloseRequestAndWait(state);
      return response;
    } catch (...) {
      const bool cancelled = stop.stop_requested() || IsClosing(state);
      CloseRequestAndWait(state);
      if (cancelled) return std::nullopt;
      throw;
    }
  } catch (...) {
    CloseRequestAndWait(state);
    throw;
  }
}

std::optional<std::string> FetchLatestVersionAsset(std::stop_token stop) {
  return FetchVersionAsset({kGitHubHost, 443, version::github_latest_version_asset_path, true, true}, stop);
}

}  // namespace ibstart::update::transport
