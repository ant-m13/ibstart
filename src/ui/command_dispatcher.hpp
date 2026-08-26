#pragma once

#include <Windows.h>

#include <cstddef>
#include <functional>
#include <map>
#include <vector>

namespace ibstart::ui {

class CommandDispatcher final {
 public:
  using Handler = std::function<void()>;
  using IndexedHandler = std::function<void(std::size_t)>;

  void Register(UINT command, Handler handler);
  void RegisterRange(UINT first, std::size_t count, IndexedHandler handler);
  [[nodiscard]] bool Dispatch(UINT command) const;

 private:
  struct Range {
    UINT first{};
    UINT count{};
    IndexedHandler handler;
  };

  std::map<UINT, Handler> handlers_;
  std::vector<Range> ranges_;
};

}  // namespace ibstart::ui
