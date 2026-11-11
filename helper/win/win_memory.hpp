#pragma once
// Windows implementation of Platform and ProcessMemory over Win32. Only built for mingw.
#include <memory>

#include "proto/platform.hpp"

namespace moth {

std::unique_ptr<Platform> make_win_platform();

}  // namespace moth
