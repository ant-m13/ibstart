#pragma once

#include "core/domain/model.hpp"

namespace ibstart::launcher {

// Starts a prepared 1C command and returns once the process has been created.
// The caller retains ownership of the LaunchCommand data.
void Launch(const domain::LaunchCommand& command);

}  // namespace ibstart::launcher
