#include "engine/block_scan.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <type_traits>

namespace moth {

namespace {

template <size_t W>
uint64_t load(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, W);  // little-endian host
  return v;
}

// first offset whose absolute address is a multiple of align
size_t first_aligned(uint64_t base, uint32_t align) { return static_cast<size_t>((align - base % align) % align); }

template <size_t W>
void scan_eq(const uint8_t* data, size_t end, uint64_t base, const GroupSpec& g, std::vector<uint32_t>& out) {
  const size_t step = g.align;
  const uint64_t xa = g.exclude_align;
  const uint64_t want = load<W>(reinterpret_cast<const uint8_t*>(&g.pred.bits));
  for (size_t o = first_aligned(base, g.align); o < end; o += step) {
    if (load<W>(data + o) != want) continue;
    if (xa != 0 && (base + o) % xa == 0) continue;
    out.push_back(static_cast<uint32_t>(o));
  }
}

// A range test on integer keys. key() maps a value's bits to an unsigned key ordered like the
// values (floats: negatives reversed below positives, NaNs past both infinities), so the values
// strictly between lo and hi are exactly the keys in one interval, found once per scan_block call
// by binary search with the same double comparison matches() makes. The loop then needs no
// floating point, which matters most for the x86 helper running under Rosetta.
template <class T>
struct RangeKeys {
  using U = std::conditional_t<sizeof(T) == 1, uint8_t,
            std::conditional_t<sizeof(T) == 2, uint16_t, std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>>;
  static constexpr U kSign = U(U{1} << (sizeof(U) * 8 - 1));

  static U key(U bits) {
    if constexpr (std::is_floating_point_v<T>) return bits ^ U(U(-U(bits >> (sizeof(U) * 8 - 1))) | kSign);
    else if constexpr (std::is_signed_v<T>) return bits ^ kSign;
    else return bits;
  }
  static double value(U key) {
    U bits = key;
    if constexpr (std::is_floating_point_v<T>) bits = (key & kSign) ? U(key ^ kSign) : U(~key);
    else if constexpr (std::is_signed_v<T>) bits = key ^ kSign;
    T v;
    std::memcpy(&v, &bits, sizeof(T));
    return static_cast<double>(v);
  }
  // the keys of values that are not NaN: from -inf to +inf for floats, every key otherwise
  static U lowest() {
    if constexpr (std::is_floating_point_v<T>) return key(U(kSign | std::bit_cast<U>(std::numeric_limits<T>::infinity())));
    else return 0;
  }
  static U highest() {
    if constexpr (std::is_floating_point_v<T>) return key(std::bit_cast<U>(std::numeric_limits<T>::infinity()));
    else return U(~U{0});
  }

  // Sets [first, last] to the keys whose values v have lo < v < hi; false if there are none.
  static bool interval(double lo, double hi, U& first, U& last) {
    U a = lowest(), b = highest();
    if (!(value(b) > lo) || !(value(a) < hi)) return false;
    while (a < b) {  // the first key above lo
      const U m = U(a + U(b - a) / 2);
      if (value(m) > lo) b = m;
      else a = U(m + 1);
    }
    first = a;
    a = lowest(), b = highest();
    while (a < b) {  // the last key below hi
      const U m = U(a + U(b - a) / 2 + 1);
      if (value(m) < hi) a = m;
      else b = U(m - 1);
    }
    last = a;
    return first <= last;
  }
};

template <class T>
void scan_range(const uint8_t* data, size_t end, uint64_t base, const GroupSpec& g, std::vector<uint32_t>& out) {
  using K = RangeKeys<T>;
  using U = typename K::U;
  U first, last;
  if (!K::interval(g.pred.lo, g.pred.hi, first, last)) return;
  const U width = U(last - first);
  const size_t step = g.align;
  const uint64_t xa = g.exclude_align;
  for (size_t o = first_aligned(base, g.align); o < end; o += step) {
    U bits;
    std::memcpy(&bits, data + o, sizeof(U));  // little-endian host
    if (U(K::key(bits) - first) > width) continue;
    if (xa != 0 && (base + o) % xa == 0) continue;
    out.push_back(static_cast<uint32_t>(o));
  }
}

void scan_group(const uint8_t* data, size_t end, uint64_t base, const GroupSpec& g, std::vector<uint32_t>& out) {
  if (g.pred.op == Pred::Eq) {
    switch (width_of(g.type)) {
      case 1: scan_eq<1>(data, end, base, g, out); break;
      case 2: scan_eq<2>(data, end, base, g, out); break;
      case 4: scan_eq<4>(data, end, base, g, out); break;
      default: scan_eq<8>(data, end, base, g, out); break;
    }
    return;
  }
  switch (g.type) {
    case ValueType::I8: scan_range<int8_t>(data, end, base, g, out); break;
    case ValueType::U8: scan_range<uint8_t>(data, end, base, g, out); break;
    case ValueType::I16: scan_range<int16_t>(data, end, base, g, out); break;
    case ValueType::U16: scan_range<uint16_t>(data, end, base, g, out); break;
    case ValueType::I32: scan_range<int32_t>(data, end, base, g, out); break;
    case ValueType::U32: scan_range<uint32_t>(data, end, base, g, out); break;
    case ValueType::I64: scan_range<int64_t>(data, end, base, g, out); break;
    case ValueType::U64: scan_range<uint64_t>(data, end, base, g, out); break;
    case ValueType::F32: scan_range<float>(data, end, base, g, out); break;
    case ValueType::F64: scan_range<double>(data, end, base, g, out); break;
  }
}

}  // namespace

void scan_block(const uint8_t* data, size_t len, size_t limit, uint64_t base,
                std::span<const GroupSpec> groups, std::vector<std::vector<uint32_t>>& out) {
  for (const auto& g : groups) {
    if (g.pred.op != Pred::Eq && g.pred.op != Pred::Range)
      throw EngineError("bad_request", "previous-value predicate cannot scan a single buffer");
    if (g.align == 0) throw EngineError("bad_request", "align must be positive");
  }
  if (out.size() < groups.size()) out.resize(groups.size());
  for (size_t i = 0; i < groups.size(); i++) {
    const GroupSpec& g = groups[i];
    const size_t w = width_of(g.type);
    if (len < w) continue;
    const size_t end = limit < len - w + 1 ? limit : len - w + 1;  // o < limit and o + w <= len
    scan_group(data, end, base, g, out[i]);
  }
}

}  // namespace moth
