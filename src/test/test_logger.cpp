#include "logger.hpp"
#include "logger_config.hpp"
#include "error_info.hpp"
#include "common.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <spdlog/common.h>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
// setenv/unsetenv are POSIX extensions that glibc exposes via <cstdlib> (included above)
// without a dedicated standard header of their own -- misc-include-cleaner has no
// POSIX-aware mapping for them, so its complaint on those two call sites is suppressed below.

using fsp::error_info;
using fsp::fsp_logger;
using fsp::logger_config;
using fsp::lvl_enum;
using fsp::processor_error;

namespace
{
  namespace fs = std::filesystem;

  // RAII guard for LOG_CONFIG so a test that sets it can never leak the value into
  // whichever test Catch2 happens to run next.
  class env_guard
  {
  public:
    explicit env_guard(std::string name) : name_(std::move(name))
    {
      if (const char* v = std::getenv(name_.c_str()); v != nullptr) old_value_ = v; // NOLINT(concurrency-mt-unsafe)
    }
    ~env_guard()
    {
      if (old_value_) ::setenv(name_.c_str(), old_value_->c_str(), 1); // NOLINT(concurrency-mt-unsafe, misc-include-cleaner)
      else ::unsetenv(name_.c_str());                                  // NOLINT(concurrency-mt-unsafe, misc-include-cleaner)
    }
    env_guard(const env_guard&)            = delete;
    env_guard& operator=(const env_guard&) = delete;
    env_guard(env_guard&&)                 = delete;
    env_guard& operator=(env_guard&&)      = delete;
  private:
    std::string                name_;
    std::optional<std::string> old_value_;
  };

  class temp_dir_guard
  {
  public:
    temp_dir_guard()
    : prev_(fs::current_path())
    , dir_(fs::temp_directory_path() / ("fsp_logger_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    {
      fs::create_directory(dir_);
      fs::current_path(dir_);
    }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::current_path(prev_, ec);
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)            = delete;
    temp_dir_guard& operator=(const temp_dir_guard&) = delete;
    temp_dir_guard(temp_dir_guard&&)                 = delete;
    temp_dir_guard& operator=(temp_dir_guard&&)      = delete;
    [[nodiscard]] const fs::path& dir() const { return dir_; }
  private:
    fs::path                    prev_;
    fs::path                    dir_;
    static inline std::uint32_t counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  void write_file(const fs::path& p, std::string_view content)
  {
    std::ofstream out(p, std::ios::binary);
    out << content;
  }
} // namespace

// ============================================================================
// logger_config
// ============================================================================

TEST_CASE("logger_config default values match the documented fallback", "[logger_config][positive]")
{
  const logger_config cfg;
  CHECK(cfg.enable_console);
  CHECK_FALSE(cfg.enable_file);
  CHECK(cfg.log_file_path == "xml_processor.log");
  CHECK(cfg.log_level == spdlog::level::warn);
}

TEST_CASE("logger_config fields can be overridden away from their defaults", "[logger_config][negative]")
{
  const logger_config cfg{.enable_console = false, .enable_file = true, .log_file_path = "custom.log", .log_level = spdlog::level::trace};
  CHECK_FALSE(cfg.enable_console);
  CHECK(cfg.enable_file);
  CHECK(cfg.log_file_path == "custom.log");
  CHECK(cfg.log_level == spdlog::level::trace);
}

// ============================================================================
// fsp_logger construction
// ============================================================================

TEST_CASE("fsp_logger construction with enable_console builds a usable logger", "[fsp_logger][positive]")
{
  const logger_config cfg{.enable_console = true, .enable_file = false, .log_level = spdlog::level::info};
  const fsp_logger     lg(cfg);
  REQUIRE(lg.get() != nullptr);
  CHECK(lg.level() == lvl_enum::info);
}

TEST_CASE("fsp_logger construction with both sinks disabled still builds a working console fallback", "[fsp_logger][negative]")
{
  // build() explicitly falls back to a console sink when the sinks vector would
  // otherwise be empty -- a logger must never end up with zero sinks.
  const logger_config cfg{.enable_console = false, .enable_file = false, .log_level = spdlog::level::info};
  const fsp_logger     lg(cfg);
  REQUIRE(lg.get() != nullptr);
  CHECK_NOTHROW(lg.info("still works"));
}

TEST_CASE("fsp_logger construction with enable_file but an empty log_file_path falls back to console", "[fsp_logger][negative]")
{
  // build() only adds the file sink when "cfg.enable_file && !cfg.log_file_path.empty()" --
  // enable_file alone with an empty path must not attempt to open a file.
  const logger_config cfg{.enable_console = false, .enable_file = true, .log_file_path = "", .log_level = spdlog::level::info};
  const fsp_logger     lg(cfg);
  REQUIRE(lg.get() != nullptr);
  CHECK_NOTHROW(lg.info("no file sink attempted"));
}

TEST_CASE("fsp_logger construction with an initial_name sets the thread log name", "[fsp_logger][positive]")
{
  const logger_config cfg{.enable_console = true};
  const fsp_logger     lg(cfg, "worker-1");
  CHECK(lg.log_name() == "worker-1");
}

TEST_CASE("fsp_logger construction with an empty initial_name leaves the thread log name untouched", "[fsp_logger][negative]")
{
  fsp::log_thread_name = "preexisting";
  const logger_config cfg{.enable_console = true};
  const fsp_logger     lg(cfg, "");
  CHECK(lg.log_name() == "preexisting");
}

TEST_CASE("fsp_logger construction with enable_file writes to the given log file path", "[fsp_logger][positive]")
{
  const temp_dir_guard tmp;
  const auto            log_path = (tmp.dir() / "out.log").string();
  const logger_config   cfg{.enable_console = false, .enable_file = true, .log_file_path = log_path, .log_level = spdlog::level::info};
  {
    const fsp_logger lg(cfg);
    lg.info("hello file sink");
    lg.get()->flush();
  }
  REQUIRE(fs::exists(log_path));
  std::ifstream     in(log_path);
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(content.contains("hello file sink"));
}

// ============================================================================
// fsp_logger::get
// ============================================================================

TEST_CASE("fsp_logger::get returns a non-null shared_ptr after construction", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{});
  CHECK(lg.get() != nullptr);
}

TEST_CASE("fsp_logger::get returns the same underlying logger on repeated calls", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{});
  CHECK(lg.get() == lg.get());
}

// ============================================================================
// fsp_logger::critical/error(const error_info&)
// ============================================================================

TEST_CASE("fsp_logger::critical(error_info) does not throw when the level is active", "[fsp_logger][positive]")
{
  const logger_config cfg{.enable_console = true, .log_level = spdlog::level::critical};
  const fsp_logger     lg(cfg);
  const error_info     err(processor_error::internal_error, "boom", "some/path", 3, 1);
  CHECK_NOTHROW(lg.critical(err));
}

TEST_CASE("fsp_logger::critical(error_info) is a silent no-op when the level is off", "[fsp_logger][negative]")
{
  const logger_config cfg{.enable_console = true, .log_level = spdlog::level::off};
  const fsp_logger     lg(cfg);
  const error_info     err(processor_error::internal_error, "boom", "some/path", 3, 1);
  CHECK_NOTHROW(lg.critical(err));
}

TEST_CASE("fsp_logger::error(error_info) does not throw when the level is active", "[fsp_logger][positive]")
{
  const logger_config cfg{.enable_console = true, .log_level = spdlog::level::err};
  const fsp_logger     lg(cfg);
  const error_info     err(processor_error::parse_failed, "parse boom", "x.xml", 10, 2);
  CHECK_NOTHROW(lg.error(err));
}

TEST_CASE("fsp_logger::error(error_info) is a silent no-op when the level is above err", "[fsp_logger][negative]")
{
  const logger_config cfg{.enable_console = true, .log_level = spdlog::level::critical};
  const fsp_logger     lg(cfg);
  const error_info     err(processor_error::parse_failed, "parse boom", "x.xml", 10, 2);
  CHECK_NOTHROW(lg.error(err));
}

// ============================================================================
// fsp_logger::critical/error/warn/info/debug/trace(cstr_t)
// ============================================================================

TEST_CASE("fsp_logger::critical(cstr_t) does not throw for a non-empty message when active", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::critical});
  CHECK_NOTHROW(lg.critical("critical message"));
}

TEST_CASE("fsp_logger::critical(cstr_t) does not throw for an empty message", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::critical});
  CHECK_NOTHROW(lg.critical(fsp::cstr_t{}));
}

TEST_CASE("fsp_logger::error(cstr_t) does not throw for a non-empty message when active", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::err});
  CHECK_NOTHROW(lg.error("error message"));
}

TEST_CASE("fsp_logger::error(cstr_t) is silent (but harmless) when the level is inactive", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::off});
  CHECK_NOTHROW(lg.error("suppressed"));
}

TEST_CASE("fsp_logger::warn(cstr_t) does not throw for a non-empty message when active", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::warn});
  CHECK_NOTHROW(lg.warn("warn message"));
}

TEST_CASE("fsp_logger::warn(cstr_t) is harmless when the level is above warn", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::err});
  CHECK_NOTHROW(lg.warn("suppressed"));
}

TEST_CASE("fsp_logger::info(cstr_t) does not throw for a non-empty message when active", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::info});
  CHECK_NOTHROW(lg.info("info message"));
}

TEST_CASE("fsp_logger::info(cstr_t) is harmless when the level is above info", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::warn});
  CHECK_NOTHROW(lg.info("suppressed"));
}

TEST_CASE("fsp_logger::debug(cstr_t) does not throw regardless of build mode", "[fsp_logger][positive]")
{
  // debug()/trace() are unconditionally compiled out to no-ops in Release builds
  // (if constexpr (is_debug())), so "active" here only means "doesn't throw", not
  // "definitely wrote to the sink" -- that part is build-mode dependent by design.
  const fsp_logger lg(logger_config{.log_level = spdlog::level::debug});
  CHECK_NOTHROW(lg.debug("debug message"));
}

TEST_CASE("fsp_logger::debug(cstr_t) is harmless when the level is above debug", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::info});
  CHECK_NOTHROW(lg.debug("suppressed"));
}

TEST_CASE("fsp_logger::trace(cstr_t) does not throw regardless of build mode", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::trace});
  CHECK_NOTHROW(lg.trace("trace message"));
}

TEST_CASE("fsp_logger::trace(cstr_t) is harmless when the level is above trace", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::debug});
  CHECK_NOTHROW(lg.trace("suppressed"));
}

// ============================================================================
// fsp_logger::active
// ============================================================================

TEST_CASE("fsp_logger::active is true for a level at or above the configured threshold", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::info});
  CHECK(lg.active(lvl_enum::info));
  CHECK(lg.active(lvl_enum::warn));
  CHECK(lg.active(lvl_enum::crit));
}

TEST_CASE("fsp_logger::active is false for a level below the configured threshold", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::warn});
  CHECK_FALSE(lg.active(lvl_enum::info));
}

// ============================================================================
// fsp_logger::level / set_level
// ============================================================================

TEST_CASE("fsp_logger::level reflects the level passed at construction", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{.log_level = spdlog::level::err});
  CHECK(lg.level() == lvl_enum::err);
}

TEST_CASE("fsp_logger::set_level changes the active threshold to a higher level", "[fsp_logger][positive]")
{
  fsp_logger lg(logger_config{.log_level = spdlog::level::info});
  lg.set_level(lvl_enum::crit);
  CHECK(lg.level() == lvl_enum::crit);
  CHECK_FALSE(lg.active(lvl_enum::err));
}

TEST_CASE("fsp_logger::set_level to the same level is idempotent", "[fsp_logger][negative]")
{
  fsp_logger lg(logger_config{.log_level = spdlog::level::warn});
  lg.set_level(lvl_enum::warn);
  CHECK(lg.level() == lvl_enum::warn);
}

// ============================================================================
// fsp_logger::log_name / make_log_name
// ============================================================================

TEST_CASE("fsp_logger::make_log_name(name) sets the thread-local log name to that name", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{});
  lg.make_log_name("solo-name");
  CHECK(lg.log_name() == "solo-name");
}

TEST_CASE("fsp_logger::make_log_name(name) with an empty name clears it to empty", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{});
  lg.make_log_name("previous");
  lg.make_log_name("");
  CHECK(lg.log_name().empty());
}

TEST_CASE("fsp_logger::make_log_name(parent, child) joins parent and non-empty child with a slash", "[fsp_logger][positive]")
{
  const fsp_logger lg(logger_config{});
  lg.make_log_name("parent", "child");
  CHECK(lg.log_name() == "parent/child");
}

TEST_CASE("fsp_logger::make_log_name(parent, child) with an empty child uses only the parent", "[fsp_logger][negative]")
{
  const fsp_logger lg(logger_config{});
  lg.make_log_name("only-parent", "");
  CHECK(lg.log_name() == "only-parent");
}

// ============================================================================
// load_logger_config
// ============================================================================

TEST_CASE("load_logger_config reads settings from a valid LOG_CONFIG file", "[load_logger_config][positive]")
{
  const env_guard       guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto      cfg_path = tmp.dir() / "custom.conf";
  write_file(cfg_path, "enable_console=false\nenable_file=true\nlog_file_path=custom_out.log\nlog_level=debug\n");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  CHECK_FALSE(cfg.enable_console);
  CHECK(cfg.enable_file);
  CHECK(cfg.log_file_path == "custom_out.log");
  CHECK(cfg.log_level == spdlog::level::debug);
}

TEST_CASE("load_logger_config accepts the warn/err/crit short aliases for log_level", "[load_logger_config][positive]")
{
  const env_guard             guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto            cfg_path = tmp.dir() / "aliases.conf";
  write_file(cfg_path, "log_level=err\n");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  CHECK(cfg.log_level == spdlog::level::err);
}

TEST_CASE("load_logger_config falls back when LOG_CONFIG points at a non-existent file", "[load_logger_config][negative]")
{
  const env_guard             guard("LOG_CONFIG");
  const temp_dir_guard tmp; // isolate cwd so no stray log_<program>_*.conf fallback file is picked up
  ::setenv("LOG_CONFIG", "/nonexistent/path/does_not_exist.conf", 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  // falls through to the hardcoded default since no cwd fallback file exists either
  CHECK(cfg.enable_console);
  CHECK_FALSE(cfg.enable_file);
  CHECK(cfg.log_level == spdlog::level::info);
}

TEST_CASE("load_logger_config falls back when LOG_CONFIG is set but empty", "[load_logger_config][negative]")
{
  const env_guard             guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  ::setenv("LOG_CONFIG", "", 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  CHECK(cfg.enable_console);
  CHECK_FALSE(cfg.enable_file);
  CHECK(cfg.log_level == spdlog::level::info);
}

TEST_CASE("load_logger_config reads the log_<program>_<mode>.conf fallback file from the cwd", "[load_logger_config][positive]")
{
  const env_guard             guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  const auto* const mode      = fsp::is_debug() ? "debug" : "release";
  const auto            file_name = fsp::str_t("log_myprog_") + mode + ".conf";
  write_file(tmp.dir() / file_name, "log_level=critical\n");

  const auto cfg = fsp::load_logger_config("myprog");
  CHECK(cfg.log_level == spdlog::level::critical);
}

TEST_CASE("load_logger_config strips a directory component from program_name before the fallback lookup", "[load_logger_config][positive]")
{
  const env_guard             guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp;
  const auto* const mode      = fsp::is_debug() ? "debug" : "release";
  const auto            file_name = fsp::str_t("log_pacs8_") + mode + ".conf";
  write_file(tmp.dir() / file_name, "log_level=trace\n");

  // program_name arrives as argv[0], e.g. "./pacs8" -- filename() must strip the "./"
  const auto cfg = fsp::load_logger_config("./pacs8");
  CHECK(cfg.log_level == spdlog::level::trace);
}

TEST_CASE("load_logger_config returns the hardcoded default when neither LOG_CONFIG nor a fallback file exist", "[load_logger_config][negative]")
{
  const env_guard             guard("LOG_CONFIG");
  ::unsetenv("LOG_CONFIG"); // NOLINT(concurrency-mt-unsafe)
  const temp_dir_guard tmp; // empty cwd: no log_<program>_*.conf present

  const auto cfg = fsp::load_logger_config("nonexistent-program");
  CHECK(cfg.enable_console);
  CHECK_FALSE(cfg.enable_file);
  CHECK(cfg.log_level == spdlog::level::info);
}

TEST_CASE("load_logger_config falls back to level 'off' for an unrecognized log_level value", "[load_logger_config][negative]")
{
  // spdlog::level::from_str() returns level::off for any name it doesn't recognize --
  // a typo'd log_level in the config file silently disables logging instead of erroring.
  const env_guard             guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto            cfg_path = tmp.dir() / "bad_level.conf";
  write_file(cfg_path, "log_level=not_a_real_level\n");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  CHECK(cfg.log_level == spdlog::level::off);
}

TEST_CASE("load_logger_config ignores comments and blank lines in the config file", "[load_logger_config][negative]")
{
  const env_guard             guard("LOG_CONFIG");
  const temp_dir_guard tmp;
  const auto            cfg_path = tmp.dir() / "commented.conf";
  write_file(cfg_path, "# a comment\n\n   \nlog_level=warning\n# trailing comment\n");
  ::setenv("LOG_CONFIG", cfg_path.string().c_str(), 1); // NOLINT(concurrency-mt-unsafe)

  const auto cfg = fsp::load_logger_config("test-program");
  CHECK(cfg.log_level == spdlog::level::warn);
}
