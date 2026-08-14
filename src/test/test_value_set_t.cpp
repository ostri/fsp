// test_value_set_t.cpp
//
// Unit tests for fsp::value_set_t<Tag> (see value_set_t.hpp) -- a validated_t<> field type that
// checks membership in a fixed set of allowed values, loaded once via init(). No reflection
// involved, so this runs as a plain Catch2 test alongside the rest of unit_tests.
#include "value_set_t.hpp"
#include <catch2/catch_test_macros.hpp>

namespace
{
  // Distinct tags -> distinct static storage per test, so tests can run in any order without
  // one test's init() clobbering another's (see value_set_t's own tparam doc comment).
  struct many_values_tag
  {
  };
  struct single_value_tag
  {
  };
  struct reinit_tag
  {
  };

  using many_values_t  = fsp::value_set_t<many_values_tag>;
  using single_value_t = fsp::value_set_t<single_value_tag>;
  using reinit_t        = fsp::value_set_t<reinit_tag>;
} // namespace

TEST_CASE("value_set_t accepts values from the delimited set", "[value_set_t]")
{
  many_values_t::init("AAAADEBBXXX;BBBBFRPPXXX;CCCCITRRXXX", ';');

  auto ok = many_values_t::parse("BBBBFRPPXXX");
  REQUIRE(ok.has_value());
  CHECK(ok->value == "BBBBFRPPXXX");
}

TEST_CASE("value_set_t rejects a value not in the set", "[value_set_t]")
{
  many_values_t::init("AAAADEBBXXX;BBBBFRPPXXX;CCCCITRRXXX", ';');

  auto bad = many_values_t::parse("ZZZZZZZZZZZ");
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().code() == fsp::processor_error::semantic_error);
}

TEST_CASE("value_set_t works as a single-element (exact-match) set", "[value_set_t]")
{
  single_value_t::init("SEPA", ';'); // no delimiter occurrences -- a valid one-element set

  CHECK(single_value_t::parse("SEPA").has_value());
  CHECK_FALSE(single_value_t::parse("SWIFT").has_value());
}

TEST_CASE("value_set_t::init() can be called again to replace the set", "[value_set_t]")
{
  reinit_t::init("ONE;TWO", ';');
  CHECK(reinit_t::parse("ONE").has_value());
  CHECK(reinit_t::parse("THREE").has_value() == false);

  reinit_t::init("THREE;FOUR", ';');
  CHECK_FALSE(reinit_t::parse("ONE").has_value()); // no longer in the set after re-init
  CHECK(reinit_t::parse("THREE").has_value());
}
