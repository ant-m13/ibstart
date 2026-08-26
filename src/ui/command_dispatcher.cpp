#include "ui/command_dispatcher.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ibstart::ui {

void CommandDispatcher::Register(UINT command, Handler handler) {
  if (!handler) throw std::invalid_argument("Command handler cannot be empty.");
  handlers_[command] = std::move(handler);
}

void CommandDispatcher::RegisterRange(UINT first, std::size_t count, IndexedHandler handler) {
  if (count == 0 || count - 1 > static_cast<std::size_t>(std::numeric_limits<UINT>::max() - first) || !handler) {
    throw std::invalid_argument("Invalid command range or handler.");
  }
  ranges_.push_back({first, static_cast<UINT>(count), std::move(handler)});
}

bool CommandDispatcher::Dispatch(UINT command) const {
  if (const auto found = handlers_.find(command); found != handlers_.end()) {
    found->second();
    return true;
  }
  for (const auto& range : ranges_) {
    if (command >= range.first && command - range.first < range.count) {
      range.handler(static_cast<std::size_t>(command - range.first));
      return true;
    }
  }
  return false;
}

}  // namespace ibstart::ui
