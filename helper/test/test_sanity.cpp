#include <nlohmann/json.hpp>

#include "doctest/doctest.h"

TEST_CASE("toolchain") { CHECK(sizeof(void*) == 8); CHECK(nlohmann::json::parse("{\"a\":1}")["a"] == 1); }
