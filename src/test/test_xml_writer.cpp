#include "xml_writer.hpp"
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

using fsp::e_result;
using logger::Logger;
using logger::logger_config;
using fsp::xml_writer;

namespace
{
  namespace fs = std::filesystem;

  // RAII temp directory: every test gets its own unique, empty directory (Catch2 test name is
  // not used here since the directory itself doesn't need to be human-readable) so parallel
  // ctest runs never collide and cleanup is automatic on scope exit.
  class temp_dir_guard
  {
  public:
    temp_dir_guard() : dir_(fs::temp_directory_path() / ("fsp_xml_writer_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    {
      fs::create_directory(dir_);
    }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)            = delete;
    temp_dir_guard& operator=(const temp_dir_guard&) = delete;
    temp_dir_guard(temp_dir_guard&&)                 = delete;
    temp_dir_guard& operator=(temp_dir_guard&&)      = delete;
    [[nodiscard]] const fs::path& dir() const { return dir_; }
    [[nodiscard]] std::string     file(std::string_view name) const { return (dir_ / name).string(); }
  private:
    fs::path                    dir_;
    static inline std::uint32_t counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  // A path inside a directory that doesn't exist -- fopen() with "wb" never creates
  // missing intermediate directories, so this reliably fails to open.
  std::string path_in_missing_dir()
  {
    return (fs::temp_directory_path() / "fsp_xml_writer_test_does_not_exist_12345" / "out.xml").string();
  }

  std::string read_file(const std::string& path)
  {
    std::ifstream     in(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
  }

  // Console-only, level-off logger -- tests just need something to bind to logger::Logger&
  // without cluttering test output.
  logger_config silent_log_cfg() { return logger_config{.console_level = logger::level::off, .file_level = logger::level::off}; }

  // logger::Logger has no public constructor (only Logger::create(), see logger.hpp), and is
  // neither copyable nor movable -- tests that need one hold it through this unique_ptr and bind
  // to *log_ptr wherever a Logger& is needed. silent_log_cfg() never fails to build (console
  // sink only), so REQUIRE'ing success here keeps every call site below simple.
  std::unique_ptr<Logger> make_silent_logger()
  {
    auto log_ptr = Logger::create(silent_log_cfg());
    REQUIRE(log_ptr.has_value());
    return std::move(*log_ptr);
  }
} // namespace

// --- constructors ------------------------------------------------------------------------

TEST_CASE("xml_writer default-constructs closed", "[xml_writer][positive]")
{
  const xml_writer w;
  CHECK_FALSE(w.is_open());
  CHECK(w.native_handle() == nullptr);
}

TEST_CASE("xml_writer(log, path) opens a writable path and leaves the writer open", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto           log_ptr = make_silent_logger();
  const xml_writer     w(*log_ptr, tmp.file("out.xml").c_str());
  CHECK(w.is_open());
  CHECK(w.native_handle() != nullptr);
}

TEST_CASE("xml_writer(log, path) never throws and leaves the writer closed for an unwritable path", "[xml_writer][negative]")
{
  const auto log_ptr = make_silent_logger();
  CHECK_NOTHROW([&] { const xml_writer w(*log_ptr, path_in_missing_dir().c_str()); }());
  const xml_writer w(*log_ptr, path_in_missing_dir().c_str());
  CHECK_FALSE(w.is_open());
}

// --- open ----------------------------------------------------------------------------------

TEST_CASE("xml_writer::open succeeds for a writable path and reserves the header region", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("open_ok.xml");
  xml_writer            w;
  const auto            res = w.open(path.c_str());
  REQUIRE(res.has_value());
  CHECK(w.is_open());
  w.close(); // force the stdio buffer to disk so file_size() reflects the reserved region
  CHECK(fs::file_size(path) == xml_writer::HEADER_RESERVE);
}

TEST_CASE("xml_writer::open returns an error and leaves the writer closed for an unwritable path", "[xml_writer][negative]")
{
  xml_writer w;
  const auto res = w.open(path_in_missing_dir().c_str());
  REQUIRE_FALSE(res.has_value());
  CHECK_FALSE(res.error().message().empty());
  CHECK_FALSE(w.is_open());
}

TEST_CASE("xml_writer::open reopening a second path closes the first file first", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("first.xml").c_str()).has_value());
  REQUIRE(w.is_open());
  const auto res = w.open(tmp.file("second.xml").c_str());
  CHECK(res.has_value());
  CHECK(w.is_open());
  CHECK(fs::exists(tmp.file("second.xml")));
}

// --- append(cstr_t) --------------------------------------------------------------------------

TEST_CASE("xml_writer::append(cstr_t) succeeds on an open writer and its data survives finalize", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("append_ok.xml");
  xml_writer            w;
  REQUIRE(w.open(path.c_str()).has_value());
  const auto res = w.append(fsp::cstr_t("<tx>1</tx>"));
  CHECK(res.has_value());
  REQUIRE(w.finalize("<root>").has_value());
  CHECK(read_file(path).contains("<tx>1</tx>"));
}

TEST_CASE("xml_writer::append(cstr_t) on a closed (default-constructed) writer is a silent no-op", "[xml_writer][negative]")
{
  xml_writer w;
  const auto res = w.append(fsp::cstr_t("ignored"));
  CHECK(res.has_value()); // no-op, not an error -- there's simply nowhere to write
  CHECK_FALSE(w.is_open());
}

// --- append(const char*, size_t) -------------------------------------------------------------

TEST_CASE("xml_writer::append(data, size) writes exactly the requested number of bytes", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("append_size_ok.xml");
  xml_writer            w;
  REQUIRE(w.open(path.c_str()).has_value());
  const char* data = "0123456789";
  const auto  res  = w.append(data, 5); // NOLINT(readability-magic-numbers)
  CHECK(res.has_value());
  REQUIRE(w.finalize("<root>").has_value());
  CHECK(read_file(path).contains("01234"));
  CHECK_FALSE(read_file(path).contains("56789"));
}

TEST_CASE("xml_writer::append(data, size) with size 0 is a harmless no-op", "[xml_writer][negative]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("append_size_zero.xml").c_str()).has_value());
  const auto res = w.append("unused", 0);
  CHECK(res.has_value());
}

// --- append(const char*) ----------------------------------------------------------------------

TEST_CASE("xml_writer::append(const char*) writes a null-terminated C string", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("append_cstr_ok.xml");
  xml_writer            w;
  REQUIRE(w.open(path.c_str()).has_value());
  const auto res = w.append("<hello/>");
  CHECK(res.has_value());
  REQUIRE(w.finalize("<root>").has_value());
  CHECK(read_file(path).contains("<hello/>"));
}

TEST_CASE("xml_writer::append(const char*) with a null pointer is a harmless no-op", "[xml_writer][negative]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("append_cstr_null.xml").c_str()).has_value());
  const auto res = w.append(static_cast<const char*>(nullptr));
  CHECK(res.has_value());
}

// --- finalize --------------------------------------------------------------------------------

TEST_CASE("xml_writer::finalize writes the header into the reserved region and pads the rest", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("finalize_ok.xml");
  xml_writer            w;
  REQUIRE(w.open(path.c_str()).has_value());
  REQUIRE(w.append("<body/>").has_value());
  const auto res = w.finalize(R"(<?xml version="1.0"?>)");
  CHECK(res.has_value());
  const auto content = read_file(path);
  CHECK(content.starts_with(R"(<?xml version="1.0"?>)"));
  CHECK(content.contains("<body/>"));
}

TEST_CASE("xml_writer::finalize returns an error when the header exceeds HEADER_RESERVE", "[xml_writer][negative]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("finalize_too_big.xml").c_str()).has_value());
  const std::string oversized_header(xml_writer::HEADER_RESERVE + 1, 'x');
  const auto         res = w.finalize(oversized_header);
  REQUIRE_FALSE(res.has_value());
  CHECK_FALSE(res.error().message().empty());
}

TEST_CASE("xml_writer::finalize on a closed writer returns an error", "[xml_writer][negative]")
{
  xml_writer w;
  const auto res = w.finalize("<root>");
  REQUIRE_FALSE(res.has_value());
  CHECK_FALSE(res.error().message().empty());
}

// --- close -------------------------------------------------------------------------------------

TEST_CASE("xml_writer::close flushes pending data and releases the file", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  const auto            path = tmp.file("close_ok.xml");
  xml_writer            w;
  REQUIRE(w.open(path.c_str()).has_value());
  REQUIRE(w.append("pending").has_value());
  w.close();
  CHECK_FALSE(w.is_open());
  CHECK(w.native_handle() == nullptr);
}

TEST_CASE("xml_writer::close on an already-closed writer is a harmless no-op", "[xml_writer][negative]")
{
  xml_writer w;
  CHECK_NOTHROW(w.close());
  CHECK_FALSE(w.is_open());
}

// --- is_open -----------------------------------------------------------------------------------

TEST_CASE("xml_writer::is_open is true right after a successful open", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("is_open_true.xml").c_str()).has_value());
  CHECK(w.is_open());
}

TEST_CASE("xml_writer::is_open is false for a default-constructed writer", "[xml_writer][negative]")
{
  const xml_writer w;
  CHECK_FALSE(w.is_open());
}

// --- native_handle -------------------------------------------------------------------------------

TEST_CASE("xml_writer::native_handle is non-null for an open writer", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("native_handle_ok.xml").c_str()).has_value());
  CHECK(w.native_handle() != nullptr);
}

TEST_CASE("xml_writer::native_handle is null for a closed writer", "[xml_writer][negative]")
{
  const xml_writer w;
  CHECK(w.native_handle() == nullptr);
}

// --- move constructor / move assignment ---------------------------------------------------------

TEST_CASE("xml_writer move constructor transfers an open file", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  xml_writer            src;
  REQUIRE(src.open(tmp.file("move_ctor_ok.xml").c_str()).has_value());
  const xml_writer dst(std::move(src));
  CHECK(dst.is_open());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move,hicpp-invalid-access-moved) -- verifying the moved-from state is the point of this test
  CHECK_FALSE(src.is_open());
}

TEST_CASE("xml_writer move constructor from a closed writer leaves the target closed", "[xml_writer][negative]")
{
  xml_writer       src;
  const xml_writer dst(std::move(src));
  CHECK_FALSE(dst.is_open());
}

TEST_CASE("xml_writer move assignment transfers an open file and closes the previous target file", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  xml_writer            src;
  xml_writer            dst;
  REQUIRE(src.open(tmp.file("move_assign_src.xml").c_str()).has_value());
  REQUIRE(dst.open(tmp.file("move_assign_dst.xml").c_str()).has_value());
  dst = std::move(src);
  CHECK(dst.is_open());
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move,hicpp-invalid-access-moved) -- verifying the moved-from state is the point of this test
  CHECK_FALSE(src.is_open());
}

TEST_CASE("xml_writer move assignment to itself is a harmless no-op", "[xml_writer][negative]")
{
  const temp_dir_guard tmp;
  xml_writer            w;
  REQUIRE(w.open(tmp.file("self_assign.xml").c_str()).has_value());
  auto& self_ref = w;
  w              = std::move(self_ref);
  CHECK(w.is_open());
}

// --- xml_writer::try_open -------------------------------------------------------------------------

TEST_CASE("xml_writer::try_open returns a usable, open xml_writer for a writable path", "[xml_writer][positive]")
{
  const temp_dir_guard tmp;
  auto                  result = xml_writer::try_open(tmp.file("try_open_ok.xml").c_str());
  REQUIRE(result.has_value());
  CHECK(result->is_open());
}

TEST_CASE("xml_writer::try_open returns an error for an unwritable path", "[xml_writer][negative]")
{
  auto result = xml_writer::try_open(path_in_missing_dir().c_str());
  REQUIRE_FALSE(result.has_value());
  CHECK_FALSE(result.error().message().empty());
}
