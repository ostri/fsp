#pragma once

// #include <memory>
#include <string>
// #include <string_view>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "common.hpp"
#include "error_info.hpp"
#include "logger_config.hpp"

namespace fsp
{
  // ---------------------------------------------------------------------------
  // Thread-local ime niti — vsaka nit nastavi svoje ime, ki ga formatter bere.
  // Definirano kot inline, da je vidno v vseh prevajalnih enotah brez ODR kršitev.
  // ---------------------------------------------------------------------------
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp, bugprone-throwing-static-initialization)
  inline thread_local str_t log_thread_name = "unknown";
  // ---------------------------------------------------------------------------
  // Custom spdlog flag formatter: izpisuje log_thread_name trenutne niti.
  // Registriraj ga kot '%*' v vzorcu formatiranja.
  // ---------------------------------------------------------------------------
  class ThreadNameFormatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override;
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override;
  };
  enum class lvl_enum : std::uint8_t
  {
    crit  = spdlog::level::critical,
    err   = spdlog::level::err,
    warn  = spdlog::level::warn,
    info  = spdlog::level::info,
    debug = spdlog::level::debug,
    trace = spdlog::level::trace
  };
  // ---------------------------------------------------------------------------
  // fsp_logger — spdlog::logger wrapper
  //
  // Odgovornosti:
  //   - gradi spdlog logger iz logger_config (konzola, datoteka ali oboje)
  //   - hrani shared_ptr<spdlog::logger> ki ga je mogoče deliti z workerji
  //     in validacijsko nitjo
  //   - ponuja typed log_*() metode s preverjanjem ravni (izogibamo se
  //     gradnji sporočilnih nizov ko raven ni aktivna)
  //   - izpostavlja underlying shared_ptr za kodo ki neposredno kliče spdlog
  //
  // Usage:
  //   fsp::fsp_logger lg(cfg.log_config);
  //   lg.info("Starting...");
  //   auto sptr = lg.get();          // to transfer to workers
  // ---------------------------------------------------------------------------
  class fsp_logger
  {
  public:
    explicit fsp_logger(const logger_config& cfg, cstr_t initial_name = "");
    // cant be copied or moved — ownership through shared_ptr.
    fsp_logger(const fsp_logger&)            = delete;
    fsp_logger& operator=(const fsp_logger&) = delete;
    fsp_logger(fsp_logger&&)                 = delete;
    fsp_logger& operator=(fsp_logger&&)      = delete;
    ~fsp_logger();
    [[nodiscard]] std::shared_ptr<spdlog::logger> get() const;
    void                                          critical(const error_info& e) const;
    void                                          error(const error_info& e) const;
    void                                          critical(cstr_t msg) const;
    void                                          error(cstr_t msg) const;
    void                                          warn(cstr_t msg) const;
    void                                          info(cstr_t msg) const;
    void                                          debug(cstr_t msg) const;
    void                                          trace(cstr_t msg) const;
    [[nodiscard]] constexpr bool                  active(lvl_enum lvl = lvl_enum::trace) const noexcept;
    [[nodiscard]] lvl_enum                        level() const noexcept;
    [[nodiscard]] str_t                           log_name() const;
    void                                          make_log_name(cstr_t parent_name, cstr_t child_name) const;
    void                                          make_log_name(cstr_t name) const;
    void                                          set_level(lvl_enum lvl);
  private: /// methods
    static std::unique_ptr<spdlog::pattern_formatter> make_formatter(cstr_t pattern);
    void                                              build(const logger_config& cfg);
  private:
    std::shared_ptr<spdlog::logger> logger_;
    uint8_t                         level_ = spdlog::level::off; // local cache for level
  };

  /// true if level is right for logging
  [[nodiscard]] constexpr bool fsp_logger::active(lvl_enum lvl) const noexcept
  {
    if constexpr (is_release())
      if (lvl == lvl_enum::trace || lvl == lvl_enum::debug) return false;
    return static_cast<uint8_t>(lvl) >= level_;
  }

  inline str_t fsp_logger::log_name() const { return log_thread_name; }
  inline void  fsp_logger::make_log_name(cstr_t parent_name, cstr_t child_name) const
  {
    if (child_name.empty()) log_thread_name = parent_name;
    else log_thread_name = fmt::format("{}/{}", parent_name, child_name);
  }

  inline void fsp_logger::make_log_name(cstr_t name) const { make_log_name(name, ""); };

  /**
   * @brief Loads a logger_config, trying in order:
   * 1. LOG_CONFIG environment variable -> path to a config file (if set and non-empty).
   * 2. log_<program_name>_debug.conf / log_<program_name>_release.conf (matching
   *    is_debug()/is_release()) in the current working directory, if LOG_CONFIG was unset,
   *    empty, or pointed at a file that couldn't be read.
   * 3. Hardcoded fallback if neither file could be read: console logging, level info.
   * @param program_name Identifies the calling program (e.g. "pacs8", "pacs8-cb", "fsp") --
   * used only to pick the right fallback config file in step 2, so each program can have its own
   * log_file_path (see config/log_debug.conf.in/log_release.conf.in, generated per-program by
   * CMake into log_<program_name>_debug.conf/_release.conf).
   */
  [[nodiscard]] logger_config load_logger_config(cstr_t program_name);
} // namespace fsp
