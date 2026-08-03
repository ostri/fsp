#include "xerces_mgr.hpp"
#include "x_str.hpp"
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <xercesc/util/XMLString.hpp>

namespace
{
  // xerces_mgr has no observable state of its own; whether Initialize()/Terminate()
  // actually ran is checked indirectly through XMLString, which segfaults/asserts
  // internally in debug Xerces builds when used outside an Initialize/Terminate pair.
  bool xerces_is_usable()
  {
    const XMLCh* lit = xercesc::XMLString::transcode("ok");
    const bool   ok  = lit != nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    xercesc::XMLString::release(const_cast<XMLCh**>(&lit));
    return ok;
  }
} // namespace

TEST_CASE("xerces_mgr construction initializes Xerces for use", "[xerces_mgr][positive]")
{
  fsp::xerces_mgr mgr;
  REQUIRE(xerces_is_usable());
}

TEST_CASE("xerces_mgr can be constructed and destroyed repeatedly in sequence", "[xerces_mgr][positive]")
{
  // Mirrors the real usage pattern: each fsp executable (validate, validator, pacs8, ...)
  // creates exactly one xerces_mgr for its own process lifetime. This proves that doing
  // that N times in a row (e.g. across N unit-test processes) keeps working correctly.
  for (int i = 0; i < 3; ++i)
  {
    fsp::xerces_mgr mgr;
    REQUIRE(xerces_is_usable());
  }
}

TEST_CASE("xerces_mgr is neither copyable nor movable", "[xerces_mgr][negative]")
{
  // Negative test of the API contract: xerces_mgr guards a process-global singleton
  // (XMLPlatformUtils), so copying or moving it must be rejected at compile time
  // rather than allowed to silently double-Initialize/double-Terminate at runtime.
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<fsp::xerces_mgr>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<fsp::xerces_mgr>);
  STATIC_REQUIRE_FALSE(std::is_move_constructible_v<fsp::xerces_mgr>);
  STATIC_REQUIRE_FALSE(std::is_move_assignable_v<fsp::xerces_mgr>);
}
