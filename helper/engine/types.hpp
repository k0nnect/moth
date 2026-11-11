#pragma once
// Shared value types for the scan engine. Portable C++20, no OS calls.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace moth {

enum class ValueType : uint8_t { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64 };

size_t width_of(ValueType t);
bool is_float(ValueType t);
bool is_signed(ValueType t);  // true for signed integers and floats
bool parse_type(std::string_view s, ValueType& out);

struct EngineError : std::runtime_error {
  std::string code;
  EngineError(std::string c, std::string msg) : std::runtime_error(std::move(msg)), code(std::move(c)) {}
};

}  // namespace moth
