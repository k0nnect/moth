#include <limits>
#include <memory>
#include <new>
#include <string>

#include "doctest/doctest.h"
#include "nlohmann/json.hpp"
#include "proto/handler.hpp"
#include "test/fake_platform.hpp"

using namespace moth;
using namespace moth::test;
using json = nlohmann::json;

static json J(const std::string& s) { return json::parse(s); }

TEST_CASE("ps, attach, scan, narrow, peek, write round trip") {
  FakePlatform p; p.add(208, "disco.exe", 64); p.mem(208).add_region(0x10000, 4096); p.mem(208).poke<int32_t>(0x10008, 100);
  Handler h(p);
  CHECK(J(h.handle_line(R"({"id":1,"op":"ps"})"))["processes"][0]["name"] == "disco.exe");
  CHECK(J(h.handle_line(R"({"id":2,"op":"attach","pid":208})"))["bits"] == 64);
  auto sc = J(h.handle_line(R"({"id":3,"op":"scan","keep":false,"groups":[
     {"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}},
     {"id":"f32","type":"f32","align":4,"exclude_align":0,"pred":{"op":"range","lo":99.0,"hi":101.0}}]})"));
  CHECK(sc["ok"] == true); CHECK(sc["id"] == 3); CHECK(sc["groups"][0]["count"] == 1); CHECK(sc["stats"]["regions"] == 1);
  auto pk = J(h.handle_line(R"({"id":4,"op":"peek","offset":0,"limit":5})"));              // group missing
  CHECK(pk["ok"] == false); CHECK(pk["error"]["code"] == "bad_request");
  pk = J(h.handle_line(R"({"id":5,"op":"peek","group":"i32","offset":0,"limit":5})"));
  CHECK(pk["items"][0]["addr"] == "0x10008"); CHECK(pk["items"][0]["v"] == "100");
  auto wr = J(h.handle_line(R"({"id":6,"op":"write","addr":"0x10008","type":"i32","v":"999"})"));
  CHECK(wr["prev"] == "100"); CHECK(p.mem(208).peek<int32_t>(0x10008) == 999);
  auto nr = J(h.handle_line(R"({"id":7,"op":"narrow","atomic":true,"groups":[{"id":"i32","pred":{"op":"eq","v":"5"}}]})"));
  CHECK(nr["applied"] == false);
}
TEST_CASE("errors") {
  FakePlatform p; Handler h(p);
  CHECK(J(h.handle_line("not json"))["error"]["code"] == "bad_request");
  CHECK(J(h.handle_line(R"({"id":1,"op":"dance"})"))["error"]["code"] == "unknown_op");
  CHECK(J(h.handle_line(R"({"id":2,"op":"scan","keep":false,"groups":[]})"))["error"]["code"] == "not_attached");
  CHECK(J(h.handle_line(R"({"id":3,"op":"attach","pid":9})"))["error"]["code"] == "no_such_process");
}
TEST_CASE("float values, negative integers and bye") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<float>(0x10000, 85.5f); p.mem(1).poke<int16_t>(0x10010, -5);
  Handler h(p); h.handle_line(R"({"id":1,"op":"attach","pid":1})");
  CHECK(J(h.handle_line(R"({"id":2,"op":"read","addr":"0x10000","type":"f32"})"))["v"].get<double>() == doctest::Approx(85.5));
  CHECK(J(h.handle_line(R"({"id":3,"op":"read","addr":"0x10010","type":"i16"})"))["v"] == "-5");
  CHECK(J(h.handle_line(R"({"id":4,"op":"write","addr":"0x10010","type":"i16","v":"70000"})"))["error"]["code"] == "bad_request");
  h.handle_line(R"({"id":5,"op":"bye"})"); CHECK(h.wants_exit());
}

static std::string code(Handler& h, const std::string& line) {
  auto r = J(h.handle_line(line));
  return r["ok"] == true ? "ok" : r["error"]["code"].get<std::string>();
}

TEST_CASE("integer value range and signedness") {
  FakePlatform p; p.add(1, "t.exe", 32); p.mem(1).add_region(0x10000, 4096);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  auto w = [&](const char* t, const char* v) {
    return code(h, std::string(R"({"op":"write","addr":"0x10000","type":")") + t + R"(","v":")" + v + R"("})");
  };
  CHECK(w("i16", "32767") == "ok");
  CHECK(w("i16", "32768") == "bad_request");
  CHECK(w("i16", "-32768") == "ok");
  CHECK(w("i16", "-32769") == "bad_request");
  CHECK(w("u8", "255") == "ok");
  CHECK(w("u8", "256") == "bad_request");
  CHECK(w("u8", "-1") == "bad_request");
  CHECK(w("i64", "-9223372036854775808") == "ok");
  CHECK(w("i64", "9223372036854775808") == "bad_request");
  CHECK(w("u64", "18446744073709551615") == "ok");
  CHECK(w("u64", "18446744073709551616") == "bad_request");
  CHECK(w("i32", "") == "bad_request");
  CHECK(w("i32", "-") == "bad_request");
  CHECK(w("i32", "+5") == "bad_request");
  CHECK(w("i32", "1x") == "bad_request");
  CHECK(code(h, R"({"op":"write","addr":"0x10000","type":"i32","v":5})") == "bad_request");  // number for an int
  CHECK(code(h, R"({"op":"write","addr":"0x10000","type":"f32","v":"5"})") == "bad_request");  // string for a float
  p.mem(1).poke<uint16_t>(0x10010, 0xFFFB);
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10010","type":"i16"})"))["v"] == "-5");
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10010","type":"u16"})"))["v"] == "65531");
  p.mem(1).poke<int64_t>(0x10018, INT64_MIN);
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10018","type":"i64"})"))["v"] == "-9223372036854775808");
  CHECK(code(h, R"({"op":"read","addr":"10010","type":"i16"})") == "bad_request");
  CHECK(code(h, R"({"op":"read","addr":"0x10010","type":"q16"})") == "bad_request");
  CHECK(code(h, R"({"op":"read","addr":"0x90000","type":"i16"})") == "read_failed");
  CHECK(code(h, R"({"op":"write","addr":"0x90000","type":"i16","v":"1"})") == "write_failed");
}

TEST_CASE("scan and narrow validation") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  auto scan = [&](const std::string& g) { return code(h, R"({"op":"scan","keep":false,"groups":[)" + g + "]}"); };
  CHECK(scan(R"({"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"lt_prev"}})") == "bad_request");
  CHECK(scan(R"({"id":"a","type":"i33","align":4,"exclude_align":0,"pred":{"op":"eq","v":"1"}})") == "bad_request");
  CHECK(scan(R"({"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"range","lo":1,"hi":2}})") == "bad_request");
  CHECK(scan(R"({"id":"a","type":"f32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"1"}})") == "bad_request");
  CHECK(scan(R"({"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"1"}})") == "ok");
  CHECK(scan(R"({"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"1"}},
               {"id":"f","type":"f32","align":4,"exclude_align":0,"pred":{"op":"range","lo":1,"hi":2}})") == "ok");
  auto nar = [&](const std::string& g) { return code(h, R"({"op":"narrow","atomic":false,"groups":[)" + g + "]}"); };
  CHECK(nar(R"({"id":"f","pred":{"op":"eq","v":"1"}})") == "bad_request");
  CHECK(nar(R"({"id":"zz","pred":{"op":"eq","v":"1"}})") == "no_such_group");
  CHECK(nar(R"({"id":"f","pred":{"op":"lt_prev"}})") == "ok");
  CHECK(code(h, R"({"op":"narrow","atomic":false,"groups":[{"id":"a","pred":{"op":"eq","v":"1"}}]})") == "ok");
  CHECK(code(h, R"({"op":"scan","keep":false,"budget_mb":8,"groups":[]})") == "ok");
  CHECK(code(h, R"({"op":"scan","keep":false,"budget_mb":0,"groups":[]})") == "bad_request");
}

TEST_CASE("range floor and moved on the wire") {
  FakePlatform p; p.add(1, "t.exe", 64); auto& m = p.mem(1); m.add_region(0x10000, 4096);
  m.poke<float>(0x10000, 1.0f); m.poke<int32_t>(0x10004, 7);           // 7 read as a float is about 1e-44
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  auto scan = [&](const std::string& pred) {
    return J(h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"f","type":"f32","align":4,"exclude_align":0,"pred":)" + pred + "}]}"));
  };
  CHECK(scan(R"({"op":"range","lo":0.0,"hi":2.0,"floor":0.000001})")["groups"][0]["count"] == 1);
  CHECK(scan(R"({"op":"range","lo":0.0,"hi":2.0,"moved":true})")["error"]["code"] == "bad_request");
  CHECK(scan(R"({"op":"range","lo":0.0,"hi":2.0,"floor":-1})")["error"]["code"] == "bad_request");
  CHECK(scan(R"({"op":"range","lo":0.0,"hi":2.0,"moved":"yes"})")["error"]["code"] == "bad_request");
  CHECK(scan(R"({"op":"range","lo":0.0,"hi":2.0})")["groups"][0]["count"] == 2);
  m.poke<float>(0x10000, 1.5f);                                         // beyond the brief: moved in a narrow
  auto nr = J(h.handle_line(R"({"op":"narrow","atomic":true,"groups":[{"id":"f","pred":{"op":"range","lo":0.0,"hi":2.0,"moved":true}}]})"));
  CHECK(nr["applied"] == true); CHECK(nr["groups"][0]["count"] == 1);
}

TEST_CASE("peek limit is capped at 1000") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 16384);  // 4096 zero i32 slots
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"z","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})");
  auto pk = J(h.handle_line(R"({"op":"peek","group":"z","offset":0,"limit":5000})"));
  CHECK(pk["items"].size() == 1000);
  CHECK(pk["items"][999]["addr"] == "0x10f9c");
}

TEST_CASE("peek from an address") {
  FakePlatform p; p.add(1, "t.exe", 64); auto& m = p.mem(1); m.add_region(0x10000, 16384);
  for (int i = 0; i < 3; i++) m.poke<int32_t>(0x10000 + i * 16, 100);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[
     {"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}},
     {"id":"z","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})");
  auto pk = J(h.handle_line(R"({"op":"peek","group":"i32","from":"0x10004","limit":5})"));
  REQUIRE(pk["items"].size() == 2); CHECK(pk["items"][0]["addr"] == "0x10010"); CHECK(pk["items"][0]["v"] == "100");
  CHECK(code(h, R"({"op":"peek","group":"i32","offset":0,"from":"0x10004","limit":5})") == "bad_request");
  CHECK(code(h, R"({"op":"peek","group":"i32","limit":5})") == "bad_request");               // neither
  CHECK(code(h, R"({"op":"peek","group":"i32","from":"10004","limit":5})") == "bad_request");
  CHECK(code(h, R"({"op":"peek","group":"i32","from":4,"limit":5})") == "bad_request");
  CHECK(code(h, R"({"op":"peek","group":"nope","from":"0x10004","limit":5})") == "no_such_group");
  pk = J(h.handle_line(R"({"op":"peek","group":"z","from":"0x10000","limit":5000})"));      // the cap holds
  CHECK(pk["items"].size() == 1000); CHECK(pk["items"][0]["addr"] == "0x10004");
}

TEST_CASE("peek with holes: a position that cannot be read is null in its place") {
  FakePlatform p; p.add(1, "t.exe", 64); auto& m = p.mem(1); m.add_region(0x10000, 4096); m.add_region(0x30000, 4096);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x30000, 100);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}}]})");
  m.remove_region(0x10000);
  auto pk = J(h.handle_line(R"({"op":"peek","group":"i32","offset":0,"limit":5,"holes":true})"));
  REQUIRE(pk["items"].size() == 2);
  CHECK(pk["items"][0] == json{{"addr", "0x10000"}, {"v", nullptr}}); CHECK(pk["items"][1]["v"] == "100");
  pk = J(h.handle_line(R"({"op":"peek","group":"i32","from":"0x10000","limit":5,"holes":true})"));
  REQUIRE(pk["items"].size() == 2); CHECK(pk["items"][0]["v"].is_null()); CHECK(pk["items"][1]["addr"] == "0x30000");
  for (const char* without : {R"({"op":"peek","group":"i32","offset":0,"limit":5})", R"({"op":"peek","group":"i32","offset":0,"limit":5,"holes":false})",
                              R"({"op":"peek","group":"i32","from":"0x10000","limit":5})"}) {
    pk = J(h.handle_line(without));                                     // as before: left out
    REQUIRE(pk["items"].size() == 1); CHECK(pk["items"][0]["addr"] == "0x30000");
  }
  CHECK(code(h, R"({"op":"peek","group":"i32","offset":0,"limit":5,"holes":1})") == "bad_request");
  m.set_alive(false);
  CHECK(code(h, R"({"op":"peek","group":"i32","offset":0,"limit":5,"holes":true})") == "target_gone");
}

TEST_CASE("attach, detach, status and target_gone") {
  FakePlatform p;
  p.add(1, "a.exe", 64); p.mem(1).add_region(0x10000, 4096);
  p.add(2, "b.exe", 32); p.mem(2).add_region(0x20000, 4096);
  p.add(3, "c.exe", 64); p.deny(3);
  p.set_modules(1, {"a.exe", "k.dll"});
  Handler h(p);
  CHECK(J(h.handle_line(R"({"op":"status"})"))["attached"].is_null());
  CHECK(code(h, R"({"op":"detach"})") == "ok");
  CHECK(J(h.handle_line(R"({"op":"modules","pid":1})"))["modules"][1] == "k.dll");
  CHECK(code(h, R"({"op":"attach","pid":3})") == "access_denied");
  h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"z","type":"u8","align":1,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})");
  auto st = J(h.handle_line(R"({"op":"status"})"));
  CHECK(st["attached"]["name"] == "a.exe"); CHECK(st["groups"].size() == 1);
  h.handle_line(R"({"op":"attach","pid":2})");                          // detaches from 1 first
  st = J(h.handle_line(R"({"op":"status"})"));
  CHECK(st["attached"]["pid"] == 2); CHECK(st["groups"].size() == 0);
  CHECK(code(h, R"({"op":"peek","group":"z","offset":0,"limit":1})") == "no_such_group");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"z","type":"u8","align":1,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})");
  CHECK(code(h, R"({"op":"peek","group":"z","offset":0,"limit":1})") == "ok");
  p.mem(2).set_alive(false);
  st = J(h.handle_line(R"({"op":"status"})"));                         // the game has closed
  CHECK(st["attached"].is_null()); CHECK(st["groups"].size() == 0);
  CHECK(code(h, R"({"op":"peek","group":"z","offset":0,"limit":1})") == "target_gone");
  CHECK(code(h, R"({"op":"read","addr":"0x20000","type":"u8"})") == "target_gone");
  CHECK(code(h, R"({"op":"scan","keep":false,"groups":[]})") == "target_gone");
  CHECK(code(h, R"({"op":"detach"})") == "ok");
  CHECK(code(h, R"({"op":"read","addr":"0x20000","type":"u8"})") == "not_attached");
  CHECK(J(h.handle_line(R"({"op":"attach","pid":1})"))["name"] == "a.exe");
  CHECK(code(h, R"({"op":"read","addr":"0x10000","type":"u8"})") == "ok");
}

TEST_CASE("float values that are not finite are strings") {
  FakePlatform p; p.add(1, "t.exe", 64); auto& m = p.mem(1); m.add_region(0x10000, 4096);
  m.poke<float>(0x10000, 100.0f); m.poke<double>(0x10008, 100.0);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[
     {"id":"f32","type":"f32","align":4,"exclude_align":0,"pred":{"op":"range","lo":99.0,"hi":101.0}},
     {"id":"f64","type":"f64","align":8,"exclude_align":0,"pred":{"op":"range","lo":99.0,"hi":101.0}}]})");
  m.poke<float>(0x10000, std::numeric_limits<float>::quiet_NaN());
  m.poke<double>(0x10008, -std::numeric_limits<double>::infinity());
  CHECK(J(h.handle_line(R"({"op":"peek","group":"f32","offset":0,"limit":5})"))["items"][0]["v"] == "nan");
  CHECK(J(h.handle_line(R"({"op":"peek","group":"f64","offset":0,"limit":5})"))["items"][0]["v"] == "-inf");
  m.poke<float>(0x10010, std::numeric_limits<float>::infinity());
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10010","type":"f32"})"))["v"] == "inf");
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10008","type":"f64"})"))["v"] == "-inf");
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10000","type":"f32"})"))["v"] == "nan");
  auto wr = J(h.handle_line(R"({"op":"write","addr":"0x10000","type":"f32","v":5.5})"));
  CHECK(wr["prev"] == "nan"); CHECK(wr["prev_bits"] == "0x7fc00000");
  CHECK(J(h.handle_line(R"({"op":"read","addr":"0x10000","type":"f32"})"))["v"].get<double>() == 5.5);
}

TEST_CASE("a write replies with the bytes it replaced, and can put bytes back") {
  FakePlatform p; p.add(1, "t.exe", 64); auto& m = p.mem(1); m.add_region(0x10000, 4096);
  m.poke<int32_t>(0x10000, 100); m.poke<uint64_t>(0x10008, 0x7ff8000000000123ull);   // a nan with a payload
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  auto wr = J(h.handle_line(R"({"op":"write","addr":"0x10000","type":"i32","v":"-2"})"));
  CHECK(wr["prev"] == "100"); CHECK(wr["prev_bits"] == "0x00000064");
  wr = J(h.handle_line(R"({"op":"write","addr":"0x10000","type":"i32","bits":"0x00000064"})"));
  CHECK(wr["prev"] == "-2"); CHECK(wr["prev_bits"] == "0xfffffffe"); CHECK(m.peek<int32_t>(0x10000) == 100);
  wr = J(h.handle_line(R"({"op":"write","addr":"0x10008","type":"f64","v":1.5})"));
  CHECK(wr["prev"] == "nan"); CHECK(wr["prev_bits"] == "0x7ff8000000000123");
  CHECK(code(h, R"({"op":"write","addr":"0x10008","type":"f64","bits":"0x7ff8000000000123"})") == "ok");
  CHECK(m.peek<uint64_t>(0x10008) == 0x7ff8000000000123ull);
  CHECK(J(h.handle_line(R"({"op":"write","addr":"0x10000","type":"u8","bits":"0x7"})"))["prev_bits"] == "0x64");
  CHECK(m.peek<int32_t>(0x10000) == 7);
  for (const char* bad : {R"("bits":"0x")", R"("bits":"0x123")", R"("bits":"12")", R"("bits":"0xzz")", R"("bits":7)",
                          R"("bits":"0x01","v":"1")"})
    CHECK(code(h, std::string(R"({"op":"write","addr":"0x10000","type":"u8",)") + bad + "}") == "bad_request");
  CHECK(code(h, R"({"op":"write","addr":"0x10000","type":"i16","bits":"0x10000"})") == "bad_request");
}

TEST_CASE("modules that cannot be listed are an error") {
  FakePlatform p; p.add(1, "t.exe", 64); p.set_modules(1, {"t.exe"}); p.fail_modules(1);
  Handler h(p);
  auto r = J(h.handle_line(R"({"op":"modules","pid":1})"));
  CHECK(r["ok"] == false); CHECK(r["error"]["code"] == "modules_failed");
}

TEST_CASE("drop and reply shape") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"z","type":"u8","align":1,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})");
  CHECK(code(h, R"({"op":"drop","ids":["nope"]})") == "no_such_group");
  CHECK(code(h, R"({"op":"drop","ids":["z"]})") == "ok");
  CHECK(J(h.handle_line(R"({"op":"status"})"))["groups"].size() == 0);
  const std::string line = h.handle_line(R"({"id":"x","op":"ps"})");
  CHECK(line.find('\n') == std::string::npos);
  CHECK(J(line)["id"] == "x");
  CHECK(code(h, "[1]") == "bad_request");
  CHECK(code(h, R"({"id":1})") == "bad_request");
}

TEST_CASE("a failed keep:false scan leaves the old groups usable") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<int32_t>(0x10008, 7);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"7"}}]})");
  const char* g = R"({"id":"d","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"7"}})";
  CHECK(code(h, std::string(R"({"op":"scan","keep":false,"groups":[)") + g + "," + g + "]}") == "bad_request");  // duplicate id
  auto pk = J(h.handle_line(R"({"op":"peek","group":"a","offset":0,"limit":5})"));
  CHECK(pk["ok"] == true); CHECK(pk["items"][0]["v"] == "7");
  CHECK(code(h, R"({"op":"narrow","atomic":false,"groups":[{"id":"a","pred":{"op":"eq","v":"7"}}]})") == "ok");
  p.mem(1).set_alive(false);  // target_gone before the engine clears anything
  CHECK(code(h, std::string(R"({"op":"scan","keep":false,"groups":[)") + g + "]}") == "target_gone");
  p.mem(1).set_alive(true);
  CHECK(code(h, R"({"op":"peek","group":"a","offset":0,"limit":5})") == "ok");
}

TEST_CASE("budget_mb lasts for the hunt and resets on the next keep:false scan") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x100000, size_t{64} << 20);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  auto grp = [](const char* id) {
    return std::string(R"({"id":")") + id + R"(","type":"u8","align":1,"exclude_align":0,"pred":{"op":"eq","v":"0"}})";
  };
  auto tracked = [&](const std::string& body, size_t i) {
    return J(h.handle_line(body))["groups"][i]["tracked"].get<bool>();
  };
  CHECK_FALSE(tracked(R"({"op":"scan","keep":false,"budget_mb":1,"groups":[)" + grp("a") + "]}", 0));
  CHECK_FALSE(tracked(R"({"op":"scan","keep":true,"groups":[)" + grp("b") + "]}", 0));      // carried
  CHECK(tracked(R"({"op":"scan","keep":false,"groups":[)" + grp("c") + "]}", 0));            // reset
  CHECK(tracked(R"({"op":"scan","keep":true,"groups":[)" + grp("d") + "]}", 0));
}

TEST_CASE("snapshot, then narrow by change, over the wire") {
  FakePlatform p; p.add(208, "disco.exe", 64); p.mem(208).add_region(0x10000, 4096); p.mem(208).poke<int32_t>(0x10010, 100);
  Handler h(p); h.handle_line(R"({"id":1,"op":"attach","pid":208})");
  auto sn = J(h.handle_line(R"({"id":2,"op":"snapshot","groups":[{"id":"i32","type":"i32","align":4},{"id":"f32","type":"f32","align":4}]})"));
  CHECK(sn["ok"] == true); CHECK(sn["groups"][0]["count"] == 1024); CHECK(sn["groups"][0]["has_values"] == true); CHECK(sn["stats"]["bytes"] == 4096);
  p.mem(208).poke<int32_t>(0x10010, 90);
  auto nr = J(h.handle_line(R"({"id":3,"op":"narrow","atomic":true,"groups":[{"id":"i32","pred":{"op":"lt_prev"}},{"id":"f32","pred":{"op":"lt_prev"}}]})"));
  CHECK(nr["applied"] == true); CHECK(nr["groups"][0]["count"] == 1);
  auto pk = J(h.handle_line(R"({"id":4,"op":"peek","group":"i32","offset":0,"limit":5})"));
  CHECK(pk["items"][0]["addr"] == "0x10010"); CHECK(pk["items"][0]["v"] == "90");
}

TEST_CASE("snapshot requests are checked") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096);
  Handler h(p);
  CHECK(code(h, R"({"op":"snapshot","groups":[{"id":"a","type":"i32","align":4}]})") == "not_attached");
  h.handle_line(R"({"op":"attach","pid":1})");
  auto snap = [&](const std::string& groups, const std::string& extra = "") {
    return code(h, R"({"op":"snapshot","groups":)" + groups + extra + "}");
  };
  const std::string ok = R"([{"id":"a","type":"i32","align":4}])";
  CHECK(code(h, R"({"op":"snapshot"})") == "bad_request");
  CHECK(snap("{}") == "bad_request");
  CHECK(snap("[]") == "bad_request");
  CHECK(snap(R"([{"type":"i32","align":4}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","align":4}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"i32"}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"i32","align":4,"pred":{"op":"eq","v":"1"}}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"nope","align":4}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"i32","align":4},{"id":"a","type":"f32","align":4}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"i32","align":0}])") == "bad_request");
  CHECK(snap(R"([{"id":"a","type":"i32","align":2}])") == "bad_request");
  for (const char* cap : {"0", "-1", R"("8")", "1.5", "1048577"}) CHECK(snap(ok, std::string(R"(,"cap_mb":)") + cap) == "bad_request");
  CHECK(snap(ok, R"(,"budget_mb":0)") == "bad_request");
  CHECK(snap(ok, R"(,"cap_mb":1048576)") == "ok");
  CHECK(snap(ok, R"(,"cap_mb":1)") == "ok");
}

TEST_CASE("a snapshot over cap_mb is too_big and leaves the hunt alone") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<int32_t>(0x10008, 7);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  h.handle_line(R"({"op":"scan","keep":false,"groups":[{"id":"old","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"7"}}]})");
  p.mem(1).add_region(0x100000, size_t{2} << 20);
  auto r = J(h.handle_line(R"({"op":"snapshot","cap_mb":1,"groups":[{"id":"n","type":"u8","align":1}]})"));
  CHECK(r["ok"] == false); CHECK(r["error"]["code"] == "too_big");
  CHECK(r["error"]["message"] == "2101248 to snapshot, limit 1048576");
  auto st = J(h.handle_line(R"({"op":"status"})"));
  REQUIRE(st["groups"].size() == 1); CHECK(st["groups"][0]["id"] == "old");
  auto pk = J(h.handle_line(R"({"op":"peek","group":"old","offset":0,"limit":5})"));
  CHECK(pk["ok"] == true); CHECK(pk["items"][0]["v"] == "7");
}

// A FakePlatform whose process list, or whose memory's reads, can fail to get memory.
struct OomPlatform : FakePlatform {
  bool oom_ps = false, oom_read = false;
  std::vector<ProcInfo> processes() override {
    if (oom_ps) throw std::bad_alloc();
    return FakePlatform::processes();
  }
  std::unique_ptr<ProcessMemory> open(uint32_t pid, ProcInfo& out) override {
    FakePlatform::open(pid, out);  // its checks, and out
    return std::make_unique<Reads>(mem(pid), oom_read);
  }
  struct Reads : ProcessMemory {
    FakeMemory& m;
    const bool& oom;
    Reads(FakeMemory& f, const bool& o) : m(f), oom(o) {}
    std::vector<Region> writable_regions() override { return m.writable_regions(); }
    std::vector<Region> readable_regions() override { return m.readable_regions(); }
    size_t read(uint64_t a, void* o, size_t n) override {
      if (oom) throw std::bad_alloc();
      return m.read(a, o, n);
    }
    bool write(uint64_t a, const void* d, size_t n) override { return m.write(a, d, n); }
    bool alive() override { return m.alive(); }
  };
};

TEST_CASE("an allocation that fails is no_memory, out of memory, in any op, and the hunt stays") {
  OomPlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<int32_t>(0x10008, 7);
  Handler h(p);
  p.oom_ps = true;
  auto r = J(h.handle_line(R"({"id":1,"op":"ps"})"));
  CHECK(r["ok"] == false); CHECK(r["id"] == 1);
  CHECK(r["error"] == json{{"code", "no_memory"}, {"message", "out of memory"}});
  p.oom_ps = false;
  CHECK(code(h, R"({"op":"attach","pid":1})") == "ok");
  CHECK(code(h, R"({"op":"scan","keep":false,"groups":[{"id":"a","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"7"}}]})") == "ok");
  p.oom_read = true;
  for (const char* line : {R"({"op":"narrow","atomic":true,"groups":[{"id":"a","pred":{"op":"eq_prev"}}]})",
                           R"({"op":"read","addr":"0x10008","type":"i32"})",
                           R"({"op":"peek","group":"a","offset":0,"limit":5})",
                           R"({"op":"scan","keep":true,"groups":[{"id":"b","type":"i16","align":2,"exclude_align":0,"pred":{"op":"eq","v":"7"}}]})"}) {
    r = J(h.handle_line(line));
    CHECK(r["ok"] == false);
    CHECK(r["error"] == json{{"code", "no_memory"}, {"message", "out of memory"}});
  }
  p.oom_read = false;
  auto pk = J(h.handle_line(R"({"op":"peek","group":"a","offset":0,"limit":5})"));  // the group is as it was
  REQUIRE(pk["items"].size() == 1); CHECK(pk["items"][0]["addr"] == "0x10008");
  CHECK(code(h, R"({"op":"narrow","atomic":true,"groups":[{"id":"a","pred":{"op":"eq_prev"}}]})") == "ok");
}

TEST_CASE("snapshot groups are peeked by type, and a keep:false scan replaces them") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<float>(0x10010, 1.5f);
  Handler h(p); h.handle_line(R"({"op":"attach","pid":1})");
  CHECK(code(h, R"({"op":"snapshot","budget_mb":64,"groups":[{"id":"f","type":"f32","align":4}]})") == "ok");
  auto pk = J(h.handle_line(R"({"op":"peek","group":"f","offset":4,"limit":1})"));
  CHECK(pk["items"][0]["addr"] == "0x10010"); CHECK(pk["items"][0]["v"] == 1.5);
  CHECK(code(h, R"({"op":"scan","keep":false,"groups":[{"id":"u","type":"u8","align":1,"exclude_align":0,"pred":{"op":"eq","v":"0"}}]})") == "ok");
  CHECK(code(h, R"({"op":"peek","group":"f","offset":0,"limit":1})") == "no_such_group");
  CHECK(code(h, R"({"op":"peek","group":"u","offset":0,"limit":1})") == "ok");
}
