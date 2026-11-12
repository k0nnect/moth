#include "proto/handler.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

#include "nlohmann/json.hpp"

namespace moth {
namespace {

using json = nlohmann::json;

constexpr uint64_t kPeekCap = 1000;
constexpr uint64_t kMaxBudgetMb = 1u << 20;

[[noreturn]] void bad(const std::string& msg) { throw EngineError("bad_request", msg); }

// ---- field access ----
const json& field(const json& o, const char* k) {
  auto it = o.find(k);
  if (it == o.end()) bad(std::string("missing field \"") + k + "\"");
  return *it;
}
const std::string& str_of(const json& v, const char* k) {
  if (!v.is_string()) bad(std::string("\"") + k + "\" must be a string");
  return v.get_ref<const std::string&>();
}
const std::string& get_str(const json& o, const char* k) { return str_of(field(o, k), k); }
uint64_t uint_of(const json& v, const char* k) {
  if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<int64_t>() >= 0))
    bad(std::string("\"") + k + "\" must be a non-negative integer");
  return v.get<uint64_t>();
}
uint64_t get_uint(const json& o, const char* k) { return uint_of(field(o, k), k); }
bool get_bool(const json& o, const char* k) {
  const json& v = field(o, k);
  if (!v.is_boolean()) bad(std::string("\"") + k + "\" must be true or false");
  return v.get<bool>();
}
const json& get_arr(const json& o, const char* k) {
  const json& v = field(o, k);
  if (!v.is_array()) bad(std::string("\"") + k + "\" must be an array");
  return v;
}
const json& get_obj(const json& o, const char* k) {
  const json& v = field(o, k);
  if (!v.is_object()) bad(std::string("\"") + k + "\" must be an object");
  return v;
}

ValueType get_type(const json& o, const char* k = "type") {
  ValueType t;
  if (!parse_type(get_str(o, k), t)) bad("unknown type \"" + get_str(o, k) + "\"");
  return t;
}

uint32_t get_pid(const json& o) {
  const uint64_t p = get_uint(o, "pid");
  if (p > std::numeric_limits<uint32_t>::max()) bad("\"pid\" out of range");
  return static_cast<uint32_t>(p);
}

// ---- values ----
uint64_t width_mask(ValueType t) {
  const size_t w = width_of(t);
  return w >= 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * w)) - 1;
}

// A decimal integer string, checked against the type's width and signedness.
uint64_t parse_int(const std::string& s, ValueType t) {
  size_t i = 0;
  bool neg = false;
  if (!s.empty() && s[0] == '-') {
    neg = true;
    i = 1;
  }
  if (i >= s.size()) bad("\"" + s + "\" is not a decimal integer");
  if (neg && !is_signed(t)) bad("negative value for an unsigned type");
  uint64_t mag = 0;
  for (; i < s.size(); i++) {
    if (s[i] < '0' || s[i] > '9') bad("\"" + s + "\" is not a decimal integer");
    const uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (mag > (std::numeric_limits<uint64_t>::max() - d) / 10) bad("value out of range");
    mag = mag * 10 + d;
  }
  const unsigned w = static_cast<unsigned>(8 * width_of(t));
  if (is_signed(t)) {
    const uint64_t lim = uint64_t{1} << (w - 1);  // magnitude of the minimum
    if (neg ? mag > lim : mag > lim - 1) bad("value out of range for the type");
    return (neg ? ~mag + 1 : mag) & width_mask(t);
  }
  if (mag > width_mask(t)) bad("value out of range for the type");
  return mag;
}

uint64_t float_bits(double d, ValueType t) {
  if (!std::isfinite(d)) bad("float value must be finite");
  uint64_t b = 0;
  if (t == ValueType::F32) {
    if (std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) bad("value out of range for f32");
    const float f = static_cast<float>(d);
    std::memcpy(&b, &f, sizeof f);
  } else {
    std::memcpy(&b, &d, sizeof d);
  }
  return b;
}

// A wire value: a decimal string for integer types, a JSON number for float types.
uint64_t parse_value(const json& v, ValueType t) {
  if (is_float(t)) {
    if (!v.is_number()) bad("\"v\" must be a number for a float type");
    return float_bits(v.get<double>(), t);
  }
  if (!v.is_string()) bad("\"v\" must be a decimal string for an integer type");
  return parse_int(v.get_ref<const std::string&>(), t);
}

// A float as a JSON number, or as "nan", "inf" or "-inf", which JSON numbers cannot be.
json format_float(double d) {
  if (std::isnan(d)) return "nan";
  if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
  return d;
}

json format_value(ValueType t, uint64_t bits) {
  if (t == ValueType::F32) {
    float f;
    const uint32_t b = static_cast<uint32_t>(bits);
    std::memcpy(&f, &b, sizeof f);
    return format_float(static_cast<double>(f));
  }
  if (t == ValueType::F64) {
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return format_float(d);
  }
  bits &= width_mask(t);
  if (is_signed(t)) {
    const unsigned w = static_cast<unsigned>(8 * width_of(t));
    int64_t v = static_cast<int64_t>(bits);
    if (w < 64 && (bits >> (w - 1)) & 1) v = static_cast<int64_t>(bits | ~width_mask(t));
    return std::to_string(v);
  }
  return std::to_string(bits);
}

// "0x" and 1 to max_digits hex digits; what names the field in the error.
uint64_t parse_hex(const std::string& s, size_t max_digits, const std::string& what) {
  if (s.size() < 3 || s.size() > 2 + max_digits || s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
    bad(what + " must be a 0x hex string of at most " + std::to_string(max_digits) + " digits");
  uint64_t a = 0;
  for (size_t i = 2; i < s.size(); i++) {
    const char c = s[i];
    uint64_t d;
    if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
    else bad(what + " must be a 0x hex string");
    a = a << 4 | d;
  }
  return a;
}

uint64_t parse_addr(const std::string& s) { return parse_hex(s, 16, "address"); }

// A value's raw bytes: "0x" and up to two hex digits per byte of the type.
uint64_t parse_bits(const json& v, ValueType t) { return parse_hex(str_of(v, "bits"), 2 * width_of(t), "\"bits\""); }

std::string hex_addr(uint64_t a) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  do {
    s.insert(s.begin(), digits[a & 15]);
    a >>= 4;
  } while (a != 0);
  return "0x" + s;
}

// A value's raw bytes as "0x" and two lowercase hex digits per byte of the type.
std::string hex_bits(uint64_t bits, ValueType t) {
  static const char* digits = "0123456789abcdef";
  std::string s = "0x";
  for (size_t i = 2 * width_of(t); i-- > 0;) s.push_back(digits[(bits >> (4 * i)) & 15]);
  return s;
}

// A predicate for a group of type t. allow_prev: previous-value predicates, and a range with
// "moved":true, are accepted (narrow only).
Pred parse_pred(const json& p, ValueType t, bool allow_prev) {
  if (!p.is_object()) bad("\"pred\" must be an object");
  const std::string& op = get_str(p, "op");
  Pred out{Pred::Eq};
  if (op == "eq") {
    if (is_float(t)) bad("an eq predicate needs an integer type; use range for floats");
    out.op = Pred::Eq;
    out.bits = parse_value(field(p, "v"), t);
  } else if (op == "range") {
    if (!is_float(t)) bad("a range predicate needs a float type; use eq for integers");
    const json& lo = field(p, "lo");
    const json& hi = field(p, "hi");
    if (!lo.is_number() || !hi.is_number()) bad("\"lo\" and \"hi\" must be numbers");
    out.op = Pred::Range;
    out.lo = lo.get<double>();
    out.hi = hi.get<double>();
    // optional: a floor on the magnitude, and moved (narrow only)
    if (auto f = p.find("floor"); f != p.end()) {
      if (!f->is_number() || !(f->get<double>() > 0)) bad("\"floor\" must be a number above 0");
      out.floor = f->get<double>();
    }
    if (auto m = p.find("moved"); m != p.end()) {
      if (!m->is_boolean()) bad("\"moved\" must be true or false");
      out.moved = m->get<bool>();
      if (out.moved && !allow_prev) bad("\"moved\" is not allowed in a scan: there is no last value yet");
    }
  } else if (op == "lt_prev" || op == "gt_prev" || op == "eq_prev" || op == "ne_prev") {
    if (!allow_prev) bad("a previous-value predicate is not allowed in a scan");
    out.op = op == "lt_prev" ? Pred::LtPrev : op == "gt_prev" ? Pred::GtPrev : op == "eq_prev" ? Pred::EqPrev : Pred::NePrev;
  } else {
    bad("unknown predicate \"" + op + "\"");
  }
  return out;
}

json info_json(const GroupInfo& g) {
  return json{{"id", g.id}, {"count", g.count}, {"tracked", g.tracked}, {"has_values", g.has_values}};
}
json infos_json(const std::vector<GroupInfo>& gs) {
  json a = json::array();
  for (const auto& g : gs) a.push_back(info_json(g));
  return a;
}
json stats_json(const ScanStats& s) {
  return json{{"regions", s.regions}, {"bytes", s.bytes}, {"unreadable_bytes", s.unreadable_bytes}, {"lost", s.lost}, {"ms", s.ms}};
}

}  // namespace

struct Handler::Impl {
  Platform& plat;
  EngineConfig cfg;
  bool exit = false;
  bool attached = false;
  ProcInfo proc{0, "", 0};
  std::unique_ptr<ProcessMemory> mem;  // declared before engine: the engine refers to it
  std::unique_ptr<Engine> eng;
  std::map<std::string, ValueType> types;  // type of every group the engine holds

  Impl(Platform& p, EngineConfig c) : plat(p), cfg(c) {}

  void detach() {
    eng.reset();
    mem.reset();
    types.clear();
    attached = false;
  }
  void need_attached() const {
    if (!attached) throw EngineError("not_attached", "no process attached");
  }
  ValueType type_of(const std::string& id) const {
    auto it = types.find(id);
    if (it == types.end()) throw EngineError("no_such_group", "no group \"" + id + "\"");
    return it->second;
  }
  // Forgets the types of groups the engine no longer holds.
  void sync_types() {
    if (!eng) return;
    std::map<std::string, ValueType> kept;
    for (const auto& g : eng->groups()) {
      auto it = types.find(g.id);
      if (it != types.end()) kept.emplace(g.id, it->second);
    }
    types = std::move(kept);
  }

  json run(const json& req, const std::string& op) {
    json r = json::object();
    if (op == "ps") {
      json a = json::array();
      for (const auto& p : plat.processes()) a.push_back(json{{"pid", p.pid}, {"name", p.name}});
      r["processes"] = a;
    } else if (op == "modules") {
      r["modules"] = plat.modules(get_pid(req));
    } else if (op == "attach") {
      const uint32_t pid = get_pid(req);
      detach();
      ProcInfo info{pid, "", 0};
      auto m = plat.open(pid, info);
      mem = std::move(m);
      eng = std::make_unique<Engine>(*mem, cfg);
      proc = info;
      attached = true;
      r["pid"] = proc.pid;
      r["name"] = proc.name;
      r["bits"] = proc.bits;
    } else if (op == "detach") {
      detach();
    } else if (op == "bye") {
      exit = true;
    } else if (op == "status") {
      const bool live = attached && mem->alive();  // a process that has exited is not shown as attached
      r["attached"] = live ? json{{"pid", proc.pid}, {"name", proc.name}, {"bits", proc.bits}} : json(nullptr);
      r["groups"] = live ? infos_json(eng->groups()) : json::array();
    } else if (op == "scan") {
      scan(req, r);
    } else if (op == "narrow") {
      narrow(req, r);
    } else if (op == "drop") {
      need_attached();
      std::vector<std::string> ids;
      for (const auto& v : get_arr(req, "ids")) ids.push_back(str_of(v, "ids"));
      eng->drop(ids);
      sync_types();
    } else if (op == "peek") {
      need_attached();
      const std::string& id = get_str(req, "group");
      // positions from an index (offset) or from an address (from)
      const auto from = req.find("from");
      if (from != req.end() && req.contains("offset")) bad("give \"offset\" or \"from\", not both");
      if (from == req.end() && !req.contains("offset")) bad("missing field \"offset\" or \"from\"");
      const uint64_t start = from != req.end() ? parse_addr(str_of(*from, "from")) : get_uint(req, "offset");
      uint64_t limit = get_uint(req, "limit");
      if (limit > kPeekCap) limit = kPeekCap;
      // optional: holes, a position that cannot be read given in its place with "v":null
      bool holes = false;
      if (auto hl = req.find("holes"); hl != req.end()) {
        if (!hl->is_boolean()) bad("\"holes\" must be true or false");
        holes = hl->get<bool>();
      }
      const ValueType t = type_of(id);
      json items = json::array();
      const std::vector<Item> got = from != req.end() ? eng->peek_from(id, start, limit, holes) : eng->peek(id, start, limit, holes);
      for (const Item& it : got)
        items.push_back(json{{"addr", hex_addr(it.addr)}, {"v", it.readable ? format_value(t, it.bits) : json(nullptr)}});
      r["items"] = items;
    } else if (op == "read") {
      need_attached();
      const uint64_t addr = parse_addr(get_str(req, "addr"));
      const ValueType t = get_type(req);
      uint64_t bits = 0;
      if (!eng->read_value(addr, t, bits)) throw EngineError("read_failed", "cannot read " + hex_addr(addr));
      r["v"] = format_value(t, bits);
    } else if (op == "write") {
      need_attached();
      const uint64_t addr = parse_addr(get_str(req, "addr"));
      const ValueType t = get_type(req);
      // a value, or raw bytes (to put back exactly what a write replaced)
      const auto by_bits = req.find("bits");
      if (by_bits != req.end() && req.contains("v")) bad("give \"v\" or \"bits\", not both");
      const uint64_t bits = by_bits != req.end() ? parse_bits(*by_bits, t) : parse_value(field(req, "v"), t);
      uint64_t prev = 0;
      if (!eng->write_value(addr, t, bits, prev)) throw EngineError("write_failed", "cannot write " + hex_addr(addr));
      r["prev"] = format_value(t, prev);
      r["prev_bits"] = hex_bits(prev, t);
    } else {
      throw EngineError("unknown_op", "unknown op \"" + op + "\"");
    }
    return r;
  }

  void scan(const json& req, json& r) {
    need_attached();
    const bool keep = get_bool(req, "keep");
    std::vector<GroupSpec> specs;
    for (const json& g : get_arr(req, "groups")) {
      if (!g.is_object()) bad("each group must be an object");
      const ValueType t = get_type(g);
      const uint64_t align = get_uint(g, "align"), excl = get_uint(g, "exclude_align");
      if (align > std::numeric_limits<uint32_t>::max() || excl > std::numeric_limits<uint32_t>::max()) bad("align out of range");
      specs.push_back(GroupSpec{get_str(g, "id"), t, static_cast<uint32_t>(align), static_cast<uint32_t>(excl), parse_pred(get_obj(g, "pred"), t, false)});
    }
    // A hunt starts with a keep=false scan: its budget_mb (or the configured budget) holds for
    // the hunt, and keep=true scans without budget_mb carry it on.
    const uint64_t before = eng->budget_bytes();
    auto bud = req.find("budget_mb");
    if (bud != req.end()) {
      const uint64_t mb = uint_of(*bud, "budget_mb");
      if (mb == 0 || mb > kMaxBudgetMb) bad("\"budget_mb\" out of range");
      eng->set_budget_bytes(mb << 20);
    } else if (!keep) {
      eng->set_budget_bytes(cfg.budget_bytes);
    }
    ScanStats st;
    std::vector<GroupInfo> out;
    try {
      out = eng->scan(specs, keep, st);
    } catch (...) {
      eng->set_budget_bytes(before);
      sync_types();  // the engine's group list is the truth: it may or may not have cleared it
      throw;
    }
    if (!keep) types.clear();
    for (const auto& s : specs) types[s.id] = s.type;
    sync_types();
    r["groups"] = infos_json(out);
    r["stats"] = stats_json(st);
  }

  void narrow(const json& req, json& r) {
    need_attached();
    const bool atomic = get_bool(req, "atomic");
    std::vector<NarrowItem> items;
    for (const json& g : get_arr(req, "groups")) {
      if (!g.is_object()) bad("each group must be an object");
      const std::string& id = get_str(g, "id");
      const ValueType t = type_of(id);
      items.push_back(NarrowItem{id, parse_pred(get_obj(g, "pred"), t, true)});
    }
    ScanStats st;
    std::vector<GroupInfo> out;
    bool applied;
    try {
      applied = eng->narrow(items, atomic, out, st);
    } catch (...) {
      sync_types();
      throw;
    }
    r["applied"] = applied;
    r["groups"] = infos_json(out);
    r["stats"] = stats_json(st);
  }
};

Handler::Handler(Platform& platform, EngineConfig cfg) : impl_(std::make_unique<Impl>(platform, cfg)) {}
Handler::~Handler() = default;
bool Handler::wants_exit() const { return impl_->exit; }

std::string Handler::handle_line(const std::string& line) {
  json id;  // null until the request shows one
  bool has_id = false;
  json reply;
  try {
    json req = json::parse(line);
    if (!req.is_object()) bad("request must be a JSON object");
    if (auto it = req.find("id"); it != req.end()) {
      id = *it;
      has_id = true;
    }
    const json& op = field(req, "op");
    reply = impl_->run(req, str_of(op, "op"));
    reply["ok"] = true;
  } catch (const EngineError& e) {
    const std::string w = e.what();
    const std::string prefix = e.code + ": ";
    reply = json{{"ok", false}, {"error", {{"code", e.code}, {"message", w.compare(0, prefix.size(), prefix) == 0 ? w.substr(prefix.size()) : w}}}};
  } catch (const std::exception& e) {
    reply = json{{"ok", false}, {"error", {{"code", "bad_request"}, {"message", e.what()}}}};
  }
  if (has_id) reply["id"] = id;
  // a process or module name may not be valid UTF-8
  return reply.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace moth
