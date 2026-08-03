#include "x_str.hpp"
#include "xerces_mgr.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include <utility>
#include <xercesc/util/XMLString.hpp>

// XMLCh comes in transitively from XMLString.hpp, same as in x_str.hpp/.cpp itself --
// there is no dedicated public header for it in Xerces-C, so misc-include-cleaner's
// "no header providing XMLCh is directly included" is suppressed file-wide.
// NOLINTBEGIN(misc-include-cleaner)

namespace
{
  // x_str wraps Xerces-allocated buffers, so XMLPlatformUtils must be initialized
  // before any x_str is constructed. One guard for the whole test binary, same as
  // every fsp executable creates exactly one xerces_mgr for its process lifetime.
  const fsp::xerces_mgr g_xerces_guard; // NOLINT(cert-err58-cpp, bugprone-throwing-static-initialization)
} // namespace

using fsp::x_str;

// --- construction -----------------------------------------------------------

TEST_CASE("x_str default-constructs empty", "[x_str][positive]")
{
  const x_str s;
  CHECK(s.empty());
  CHECK(s.length() == 0); // NOLINT(readability-container-size-empty) -- length() itself is under test here
  CHECK(s.c_str() == nullptr);
}

TEST_CASE("x_str constructs from a non-empty UTF-8 string_view", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"hello"});
  CHECK_FALSE(s.empty());
  CHECK(s.length() == 5);
  CHECK(s.to_string() == "hello");
}

TEST_CASE("x_str constructs empty from an empty UTF-8 string_view", "[x_str][negative]")
{
  // Empty input is the documented no-op path in the constructor (utf8.empty() guard):
  // it must not attempt transcode() and must leave the string in the empty state.
  const x_str s(fsp::cstr_t{});
  CHECK(s.empty());
  CHECK(s.length() == 0); // NOLINT(readability-container-size-empty) -- length() itself is under test here
}

TEST_CASE("x_str constructs from a replicated raw XMLCh*", "[x_str][positive]")
{
  XMLCh* raw = xercesc::XMLString::transcode("abc");
  REQUIRE(raw != nullptr);
  const x_str s(raw);
  xercesc::XMLString::release(&raw);
  CHECK_FALSE(s.empty());
  CHECK(s.to_string() == "abc");
}

TEST_CASE("x_str constructs empty from a null raw XMLCh*", "[x_str][negative]")
{
  const x_str s(static_cast<XMLCh*>(nullptr));
  CHECK(s.empty());
  CHECK(s.length() == 0); // NOLINT(readability-container-size-empty) -- length() itself is under test here
}

TEST_CASE("x_str constructs from a non-empty u16string_view", "[x_str][positive]")
{
  const std::u16string_view u16 = u"xyz";
  const x_str               s(u16);
  CHECK_FALSE(s.empty());
  CHECK(s.to_string() == "xyz");
}

TEST_CASE("x_str constructs empty from an empty u16string_view", "[x_str][negative]")
{
  const x_str s(std::u16string_view{});
  CHECK(s.empty());
  CHECK(s.length() == 0); // NOLINT(readability-container-size-empty) -- length() itself is under test here
}

// --- copy / move --------------------------------------------------------------

TEST_CASE("x_str copy constructor duplicates a non-empty string", "[x_str][positive]")
{
  const x_str original(fsp::cstr_t{"copy me"});
  const x_str copy(original); // NOLINT(performance-unnecessary-copy-initialization) -- the copy ctor is what's under test
  CHECK(copy == original);
  CHECK(copy.to_string() == "copy me");
  // independent buffers: the original still owns its own data
  CHECK(original.to_string() == "copy me");
}

TEST_CASE("x_str copy constructor from an empty source stays empty", "[x_str][negative]")
{
  const x_str empty_src;
  const x_str copy(empty_src); // NOLINT(performance-unnecessary-copy-initialization) -- the copy ctor is what's under test
  CHECK(copy.empty());
}

TEST_CASE("x_str move constructor transfers ownership", "[x_str][positive]")
{
  x_str       original(fsp::cstr_t{"move me"});
  const x_str moved(std::move(original));
  CHECK(moved.to_string() == "move me");
}

TEST_CASE("x_str move constructor from an empty source produces an empty target", "[x_str][negative]")
{
  x_str       empty_src;
  const x_str moved(std::move(empty_src));
  CHECK(moved.empty());
}

TEST_CASE("x_str copy assignment overwrites the target with the source", "[x_str][positive]")
{
  x_str       target(fsp::cstr_t{"old"});
  const x_str source(fsp::cstr_t{"new"});
  target = source;
  CHECK(target.to_string() == "new");
  CHECK(source.to_string() == "new");
}

TEST_CASE("x_str copy assignment from an empty source leaves the target unchanged", "[x_str][negative]")
{
  // operator=(const x_str&) only overwrites when other.data_ != nullptr (see x_str.cpp);
  // assigning from an empty source is documented as a no-op, not a clear().
  x_str       target(fsp::cstr_t{"kept"});
  const x_str empty_source;
  target = empty_source;
  CHECK(target.to_string() == "kept");
}

TEST_CASE("x_str move assignment overwrites the target with the source", "[x_str][positive]")
{
  x_str target(fsp::cstr_t{"old"});
  x_str source(fsp::cstr_t{"new"});
  target = std::move(source);
  CHECK(target.to_string() == "new");
}

TEST_CASE("x_str move assignment from an empty source clears the target", "[x_str][negative]")
{
  x_str target(fsp::cstr_t{"was set"});
  x_str empty_source;
  target = std::move(empty_source);
  CHECK(target.empty());
}

// --- assign --------------------------------------------------------------------

TEST_CASE("x_str::assign(cstr_t) replaces the content with a non-empty value", "[x_str][positive]")
{
  x_str s(fsp::cstr_t{"before"});
  s.assign(fsp::cstr_t{"after"});
  CHECK(s.to_string() == "after");
}

TEST_CASE("x_str::assign(cstr_t) with an empty value clears the string", "[x_str][negative]")
{
  x_str s(fsp::cstr_t{"before"});
  s.assign(fsp::cstr_t{});
  CHECK(s.empty());
}

TEST_CASE("x_str::assign(const XMLCh*) replaces the content with a non-null value", "[x_str][positive]")
{
  x_str  s(fsp::cstr_t{"before"});
  XMLCh* raw = xercesc::XMLString::transcode("after");
  REQUIRE(raw != nullptr);
  s.assign(raw);
  xercesc::XMLString::release(&raw);
  CHECK(s.to_string() == "after");
}

TEST_CASE("x_str::assign(const XMLCh*) with nullptr clears the string", "[x_str][negative]")
{
  x_str s(fsp::cstr_t{"before"});
  s.assign(static_cast<const XMLCh*>(nullptr));
  CHECK(s.empty());
}

// --- reset -----------------------------------------------------------------------

TEST_CASE("x_str::reset() clears a non-empty string", "[x_str][positive]")
{
  x_str s(fsp::cstr_t{"data"});
  s.reset();
  CHECK(s.empty());
  CHECK(s.c_str() == nullptr);
}

TEST_CASE("x_str::reset() on an already-empty string is a harmless no-op", "[x_str][negative]")
{
  x_str s;
  s.reset();
  CHECK(s.empty());
}

TEST_CASE("x_str::reset(XMLCh*) takes ownership of a non-null buffer", "[x_str][positive]")
{
  x_str  s;
  XMLCh* raw = xercesc::XMLString::transcode("owned");
  REQUIRE(raw != nullptr);
  s.reset(raw); // s now owns raw; must not release() it separately
  CHECK(s.to_string() == "owned");
}

TEST_CASE("x_str::reset(XMLCh*) with nullptr leaves the string empty", "[x_str][negative]")
{
  x_str s(fsp::cstr_t{"was set"});
  s.reset(static_cast<XMLCh*>(nullptr));
  CHECK(s.empty());
}

// --- to_string / to_string_view --------------------------------------------------

TEST_CASE("x_str::to_string round-trips a non-empty value", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"round trip"});
  CHECK(s.to_string() == "round trip");
}

TEST_CASE("x_str::to_string on an empty string returns an empty std::string", "[x_str][negative]")
{
  const x_str s;
  CHECK(s.to_string().empty());
}

TEST_CASE("x_str::to_string_view round-trips a non-empty value", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"view me"});
  CHECK(s.to_string_view() == "view me");
}

TEST_CASE("x_str::to_string_view on an empty string returns an empty view", "[x_str][negative]")
{
  const x_str s;
  CHECK(s.to_string_view().empty());
}

// --- to_u16string ------------------------------------------------------------------

TEST_CASE("x_str::to_u16string round-trips a non-empty value", "[x_str][positive]")
{
  const x_str s(std::u16string_view(u"abc"));
  CHECK(s.to_u16string() == u"abc");
}

TEST_CASE("x_str::to_u16string on an empty string returns an empty std::u16string", "[x_str][negative]")
{
  const x_str s;
  CHECK(s.to_u16string().empty());
}

// --- equality / ordering: x_str vs x_str -------------------------------------------

TEST_CASE("x_str::operator== is true for two equal values", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"same"});
  const x_str b(fsp::cstr_t{"same"});
  CHECK(a == b);
  CHECK_FALSE(a != b);
}

TEST_CASE("x_str::operator== is false for two different values", "[x_str][negative]")
{
  const x_str a(fsp::cstr_t{"one"});
  const x_str b(fsp::cstr_t{"two"});
  CHECK_FALSE(a == b);
  CHECK(a != b);
}

TEST_CASE("x_str::operator<=> orders two different values consistently with their content", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"apple"});
  const x_str b(fsp::cstr_t{"banana"});
  CHECK(a < b);
  CHECK(b > a);
}

TEST_CASE("x_str::operator<=> reports equal for two equal values", "[x_str][negative]")
{
  // "negative" in the sense of the not-strictly-ordered branch: equal values must
  // compare as equivalent, not as less-than or greater-than.
  const x_str a(fsp::cstr_t{"same"});
  const x_str b(fsp::cstr_t{"same"});
  CHECK_FALSE(a < b);
  CHECK_FALSE(a > b);
  CHECK((a <=> b) == 0);
}

// --- equality / ordering: x_str vs raw XMLCh* --------------------------------------

TEST_CASE("x_str::operator==(const XMLCh*) is true for matching content", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"match"});
  XMLCh*      raw = xercesc::XMLString::transcode("match");
  REQUIRE(raw != nullptr);
  CHECK(a == static_cast<const XMLCh*>(raw));
  xercesc::XMLString::release(&raw);
}

TEST_CASE("x_str::operator==(const XMLCh*) is false for different content", "[x_str][negative]")
{
  const x_str a(fsp::cstr_t{"match"});
  XMLCh*      raw = xercesc::XMLString::transcode("no match");
  REQUIRE(raw != nullptr);
  CHECK_FALSE(a == static_cast<const XMLCh*>(raw));
  CHECK(a != static_cast<const XMLCh*>(raw));
  xercesc::XMLString::release(&raw);
}

TEST_CASE("x_str::operator<=>(const XMLCh*) orders consistently with content", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"apple"});
  XMLCh*      raw = xercesc::XMLString::transcode("banana");
  REQUIRE(raw != nullptr);
  CHECK(a < static_cast<const XMLCh*>(raw));
  xercesc::XMLString::release(&raw);
}

TEST_CASE("x_str::operator<=>(const XMLCh*) reports equal for matching content", "[x_str][negative]")
{
  const x_str a(fsp::cstr_t{"same"});
  XMLCh*      raw = xercesc::XMLString::transcode("same");
  REQUIRE(raw != nullptr);
  CHECK((a <=> static_cast<const XMLCh*>(raw)) == 0);
  xercesc::XMLString::release(&raw);
}

// --- equality: x_str vs cstr_t (UTF-8) ----------------------------------------------

TEST_CASE("x_str::operator==(cstr_t) is true for matching UTF-8 content", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"utf8"});
  CHECK(a == fsp::cstr_t{"utf8"});
}

TEST_CASE("x_str::operator==(cstr_t) is false for different UTF-8 content", "[x_str][negative]")
{
  const x_str a(fsp::cstr_t{"utf8"});
  CHECK_FALSE(a == fsp::cstr_t{"other"});
  CHECK(a != fsp::cstr_t{"other"});
}

// --- equality: x_str vs u16string_view -----------------------------------------------

TEST_CASE("x_str::operator==(u16string_view) is true for matching content", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"same"});
  CHECK(a == std::u16string_view(u"same"));
}

TEST_CASE("x_str::operator==(u16string_view) is false for different content", "[x_str][negative]")
{
  const x_str a(fsp::cstr_t{"same"});
  CHECK_FALSE(a == std::u16string_view(u"different"));
  CHECK(a != std::u16string_view(u"different"));
}

// --- accessors: c_str / data / empty / length ----------------------------------------

TEST_CASE("x_str::c_str/data return a usable pointer for a non-empty string", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"ptr"});
  REQUIRE(s.c_str() != nullptr);
  REQUIRE(s.data() != nullptr);
  CHECK(xercesc::XMLString::stringLen(s.c_str()) == 3);
}

TEST_CASE("x_str::c_str/data return nullptr for an empty string", "[x_str][negative]")
{
  const x_str s;
  CHECK(s.c_str() == nullptr);
  CHECK(s.data() == nullptr);
}

TEST_CASE("x_str::length reports the correct length for a non-empty string", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"twelve char!"});
  CHECK(s.length() == 12);
}

TEST_CASE("x_str::length is zero for an empty string", "[x_str][negative]")
{
  const x_str s;
  CHECK(s.length() == 0); // NOLINT(readability-container-size-empty) -- length() itself is under test here
}

// --- explicit operator const XMLCh* ---------------------------------------------------

TEST_CASE("x_str converts explicitly to a usable const XMLCh*", "[x_str][positive]")
{
  const x_str s(fsp::cstr_t{"conv"});
  const auto* raw = static_cast<const XMLCh*>(s);
  REQUIRE(raw != nullptr);
  CHECK(xercesc::XMLString::stringLen(raw) == 4);
}

TEST_CASE("x_str converts explicitly to nullptr when empty", "[x_str][negative]")
{
  const x_str s;
  const auto* raw = static_cast<const XMLCh*>(s);
  CHECK(raw == nullptr);
}

// --- std::hash specialization ----------------------------------------------------------

TEST_CASE("std::hash<x_str> gives equal hashes for equal strings", "[x_str][positive]")
{
  const x_str a(fsp::cstr_t{"hash me"});
  const x_str b(fsp::cstr_t{"hash me"});
  CHECK(std::hash<x_str>{}(a) == std::hash<x_str>{}(b));
}

TEST_CASE("std::hash<x_str> distinguishes an empty string from a non-empty one", "[x_str][negative]")
{
  const x_str empty_s;
  const x_str non_empty(fsp::cstr_t{"not empty"});
  CHECK(std::hash<x_str>{}(empty_s) != std::hash<x_str>{}(non_empty));
}
// NOLINTEND(misc-include-cleaner)
