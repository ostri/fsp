#include "mmap_file.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

using fsp::mmap_file;
using fsp::try_mmap_file;

namespace
{
  namespace fs = std::filesystem;

  // RAII temp file: created with fixed content, removed on scope exit. Each test gets a
  // unique name (Catch2's test name + this process's pid) so parallel ctest runs don't collide.
  class temp_file
  {
  public:
    explicit temp_file(std::string_view content, std::string_view suffix = "")
    : path_(fs::temp_directory_path()
            / ("fsp_mmap_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++) + std::string(suffix)))
    {
      std::ofstream out(path_, std::ios::binary);
      out << content;
    }
    ~temp_file() { std::error_code ec; fs::remove(path_, ec); }
    temp_file(const temp_file&)            = delete;
    temp_file& operator=(const temp_file&) = delete;
    temp_file(temp_file&&)                 = delete;
    temp_file& operator=(temp_file&&)      = delete;

    [[nodiscard]] std::string string_path() const { return path_.string(); }
  private:
    fs::path                     path_;
    static inline std::uint32_t  counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  std::string nonexistent_path()
  {
    return (fs::temp_directory_path() / "fsp_mmap_test_does_not_exist_12345").string();
  }
} // namespace

// --- constructors --------------------------------------------------------------------

TEST_CASE("mmap_file default-constructs closed and empty", "[mmap_file][positive]")
{
  const mmap_file f;
  CHECK_FALSE(f.is_open());
  CHECK(f.empty());
  CHECK(f.size() == 0); // NOLINT(readability-container-size-empty) -- size() itself is under test here
  CHECK(f.data() == nullptr);
  CHECK_FALSE(static_cast<bool>(f));
}

TEST_CASE("mmap_file(cstr_t) opens and maps an existing non-empty file", "[mmap_file][positive]")
{
  const temp_file tf("hello mmap");
  const mmap_file f(tf.string_path());
  CHECK(f.is_open());
  CHECK_FALSE(f.empty());
  CHECK(f.size() == 10);
  CHECK(f.string_view() == "hello mmap");
}

TEST_CASE("mmap_file(cstr_t) throws for a non-existent path", "[mmap_file][negative]")
{
  CHECK_THROWS_AS(mmap_file(nonexistent_path()), std::runtime_error);
}

// --- open ------------------------------------------------------------------------------

TEST_CASE("mmap_file::open succeeds on an existing file and exposes its content", "[mmap_file][positive]")
{
  const temp_file tf("open me");
  mmap_file       f;
  f.open(tf.string_path());
  CHECK(f.is_open());
  CHECK(f.string_view() == "open me");
}

TEST_CASE("mmap_file::open throws for a non-existent path and leaves the object closed", "[mmap_file][negative]")
{
  mmap_file f;
  CHECK_THROWS_AS(f.open(nonexistent_path()), std::runtime_error);
  CHECK_FALSE(f.is_open());
}

TEST_CASE("mmap_file::open on an empty (zero-size) file succeeds with a null data pointer", "[mmap_file][negative]")
{
  // Zero-size files take the explicit "data_ = nullptr" branch in open() (mmap() of length
  // 0 is undefined behavior, so it must never be attempted) -- an edge case worth locking down.
  const temp_file tf("");
  mmap_file       f(tf.string_path());
  CHECK(f.is_open());
  CHECK(f.empty());
  CHECK(f.size() == 0); // NOLINT(readability-container-size-empty) -- size() itself is under test here
  CHECK(f.data() == nullptr);
}

TEST_CASE("mmap_file::open reopening a second file closes the first mapping first", "[mmap_file][positive]")
{
  const temp_file tf1("first");
  const temp_file tf2("second file");
  mmap_file       f(tf1.string_path());
  REQUIRE(f.string_view() == "first");
  f.open(tf2.string_path());
  CHECK(f.string_view() == "second file");
  CHECK(f.path() == tf2.string_path());
}

// --- close -----------------------------------------------------------------------------

TEST_CASE("mmap_file::close unmaps an open file and resets its state", "[mmap_file][positive]")
{
  const temp_file tf("to be closed");
  mmap_file       f(tf.string_path());
  REQUIRE(f.is_open());
  f.close();
  CHECK_FALSE(f.is_open());
  CHECK(f.empty());
  CHECK(f.data() == nullptr);
  CHECK(f.path().empty());
}

TEST_CASE("mmap_file::close on an already-closed file is a harmless no-op", "[mmap_file][negative]")
{
  mmap_file f;
  f.close();
  CHECK_FALSE(f.is_open());
}

// --- string_view / data / size / empty / is_open -----------------------------------------

TEST_CASE("mmap_file::string_view returns the full mapped content for an open file", "[mmap_file][positive]")
{
  const temp_file tf("view content");
  const mmap_file f(tf.string_path());
  CHECK(f.string_view() == "view content");
}

TEST_CASE("mmap_file::string_view returns empty for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.string_view().empty());
}

TEST_CASE("mmap_file::data is non-null for a non-empty open file", "[mmap_file][positive]")
{
  const temp_file tf("data");
  const mmap_file f(tf.string_path());
  CHECK(f.data() != nullptr);
}

TEST_CASE("mmap_file::data is null for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.data() == nullptr);
}

TEST_CASE("mmap_file::size reports the exact byte count of the mapped file", "[mmap_file][positive]")
{
  const temp_file tf("0123456789"); // 10 bytes
  const mmap_file f(tf.string_path());
  CHECK(f.size() == 10);
}

TEST_CASE("mmap_file::size is zero for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.size() == 0); // NOLINT(readability-container-size-empty) -- size() itself is under test here
}

TEST_CASE("mmap_file::empty is false for a non-empty open file", "[mmap_file][positive]")
{
  const temp_file tf("x");
  const mmap_file f(tf.string_path());
  CHECK_FALSE(f.empty());
}

TEST_CASE("mmap_file::empty is true for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.empty());
}

TEST_CASE("mmap_file::is_open is true right after a successful open", "[mmap_file][positive]")
{
  const temp_file tf("open check");
  const mmap_file f(tf.string_path());
  CHECK(f.is_open());
}

TEST_CASE("mmap_file::is_open is false for a default-constructed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK_FALSE(f.is_open());
}

// --- operator[] / at ---------------------------------------------------------------------

TEST_CASE("mmap_file::operator[] returns the byte at a valid index", "[mmap_file][positive]")
{
  const temp_file tf("ABC");
  const mmap_file f(tf.string_path());
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- operator[] itself is under test here
  CHECK(f[0] == std::byte{'A'});
  CHECK(f[2] == std::byte{'C'});
  // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("mmap_file::at returns the byte at a valid index", "[mmap_file][positive]")
{
  const temp_file tf("ABC");
  const mmap_file f(tf.string_path());
  CHECK(f.at(1) == std::byte{'B'});
}

TEST_CASE("mmap_file::at throws std::out_of_range for an index past the end", "[mmap_file][negative]")
{
  const temp_file tf("AB");
  const mmap_file f(tf.string_path());
  CHECK_THROWS_AS(f.at(2), std::out_of_range);
  CHECK_THROWS_AS(f.at(1000), std::out_of_range);
}

TEST_CASE("mmap_file::at throws std::out_of_range for any index into an empty file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK_THROWS_AS(f.at(0), std::out_of_range);
}

// --- begin/cbegin/end/cend ----------------------------------------------------------------

TEST_CASE("mmap_file::begin/end bound the full mapped range for a non-empty file", "[mmap_file][positive]")
{
  const temp_file tf("range");
  const mmap_file f(tf.string_path());
  CHECK(std::distance(f.begin(), f.end()) == 5);
  CHECK(f.cbegin() == f.begin());
  CHECK(f.cend() == f.end());
}

TEST_CASE("mmap_file::begin equals end for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.begin() == f.end());
  CHECK(f.begin() == nullptr);
}

// --- span ------------------------------------------------------------------------------------

TEST_CASE("mmap_file::span covers the whole mapped content for a non-empty file", "[mmap_file][positive]")
{
  const temp_file tf("span it");
  const mmap_file f(tf.string_path());
  const auto      sp = f.span();
  CHECK(sp.size() == 7);
  CHECK(sp.data() == f.data());
}

TEST_CASE("mmap_file::span is empty for a closed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.span().empty());
}

// --- subspan ---------------------------------------------------------------------------------

TEST_CASE("mmap_file::subspan returns the requested slice for a valid in-range offset/count", "[mmap_file][positive]")
{
  const temp_file tf("0123456789");
  const mmap_file f(tf.string_path());
  const auto      sp = f.subspan(2, 3);
  REQUIRE(sp.size() == 3);
  CHECK(sp.at(0) == std::byte{'2'});
  CHECK(sp.at(2) == std::byte{'4'});
}

TEST_CASE("mmap_file::subspan clamps a count that runs past the end of the file", "[mmap_file][positive]")
{
  const temp_file tf("0123456789"); // size 10
  const mmap_file f(tf.string_path());
  const auto      sp = f.subspan(8, 100);
  CHECK(sp.size() == 2); // clamped to size_ - offset
}

TEST_CASE("mmap_file::subspan returns an empty span when offset is at or past size", "[mmap_file][negative]")
{
  const temp_file tf("01234");
  const mmap_file f(tf.string_path());
  CHECK(f.subspan(5, 1).empty());  // offset == size
  CHECK(f.subspan(100, 1).empty()); // offset > size
}

TEST_CASE("mmap_file::subspan with count 0 returns an empty span", "[mmap_file][negative]")
{
  const temp_file tf("01234");
  const mmap_file f(tf.string_path());
  CHECK(f.subspan(0, 0).empty());
}

// --- prefetch --------------------------------------------------------------------------------

TEST_CASE("mmap_file::prefetch on a valid in-range offset does not throw or crash", "[mmap_file][positive]")
{
  const temp_file tf(std::string(8192, 'z'));
  const mmap_file f(tf.string_path());
  CHECK_NOTHROW(f.prefetch(0));
  CHECK_NOTHROW(f.prefetch(4096, 1024));
}

TEST_CASE("mmap_file::prefetch with an out-of-range offset is a silent no-op", "[mmap_file][negative]")
{
  const temp_file tf("small");
  const mmap_file f(tf.string_path());
  CHECK_NOTHROW(f.prefetch(1000));
}

TEST_CASE("mmap_file::prefetch on a closed file is a silent no-op", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK_NOTHROW(f.prefetch(0));
}

// --- operator bool ---------------------------------------------------------------------------

TEST_CASE("mmap_file converts to true when open", "[mmap_file][positive]")
{
  const temp_file tf("truthy");
  const mmap_file f(tf.string_path());
  CHECK(static_cast<bool>(f));
}

TEST_CASE("mmap_file converts to false when closed", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK_FALSE(static_cast<bool>(f));
}

// --- path ------------------------------------------------------------------------------------

TEST_CASE("mmap_file::path returns the path used to open the file", "[mmap_file][positive]")
{
  const temp_file tf("path check");
  const mmap_file f(tf.string_path());
  CHECK(f.path() == tf.string_path());
}

TEST_CASE("mmap_file::path is empty for a closed/default-constructed file", "[mmap_file][negative]")
{
  const mmap_file f;
  CHECK(f.path().empty());
}

// --- move constructor / move assignment -------------------------------------------------------

TEST_CASE("mmap_file move constructor transfers an open mapping", "[mmap_file][positive]")
{
  const temp_file tf("move me");
  mmap_file       src(tf.string_path());
  const mmap_file dst(std::move(src));
  CHECK(dst.is_open());
  CHECK(dst.string_view() == "move me");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move,hicpp-invalid-access-moved) -- verifying the moved-from state is the point of this test
  CHECK_FALSE(src.is_open());
}

TEST_CASE("mmap_file move constructor from a closed file leaves the target closed", "[mmap_file][negative]")
{
  mmap_file       src;
  const mmap_file dst(std::move(src));
  CHECK_FALSE(dst.is_open());
}

TEST_CASE("mmap_file move assignment transfers an open mapping and closes the previous target mapping", "[mmap_file][positive]")
{
  const temp_file tf_src("source content");
  const temp_file tf_dst("dest content");
  mmap_file       src(tf_src.string_path());
  mmap_file       dst(tf_dst.string_path());
  dst = std::move(src);
  CHECK(dst.string_view() == "source content");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move,hicpp-invalid-access-moved) -- verifying the moved-from state is the point of this test
  CHECK_FALSE(src.is_open());
}

TEST_CASE("mmap_file move assignment to itself is a harmless no-op", "[mmap_file][negative]")
{
  const temp_file tf("self assign");
  mmap_file       f(tf.string_path());
  auto&           self_ref = f;
  f                        = std::move(self_ref);
  CHECK(f.is_open());
  CHECK(f.string_view() == "self assign");
}

// --- try_mmap_file -----------------------------------------------------------------------------

TEST_CASE("try_mmap_file returns a usable mmap_file for an existing file", "[mmap_file][positive]")
{
  const temp_file tf("try me");
  auto            result = try_mmap_file(tf.string_path());
  REQUIRE(result.has_value());
  CHECK(result->string_view() == "try me");
}

TEST_CASE("try_mmap_file returns an error message for a non-existent path", "[mmap_file][negative]")
{
  auto result = try_mmap_file(nonexistent_path());
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().empty());
}
