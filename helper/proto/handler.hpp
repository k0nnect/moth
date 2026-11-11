#pragma once
// One line of JSON from the controlling program in, one line of JSON out. Portable C++20, no OS calls.
#include <memory>
#include <string>

#include "engine/engine.hpp"
#include "proto/platform.hpp"

namespace moth {

class Handler {
 public:
  Handler(Platform& platform, EngineConfig cfg = {});
  ~Handler();
  Handler(const Handler&) = delete;
  Handler& operator=(const Handler&) = delete;

  // One JSON reply, no trailing newline. Never throws.
  std::string handle_line(const std::string& line);
  bool wants_exit() const;  // true after "bye"

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace moth
