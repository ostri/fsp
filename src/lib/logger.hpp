#pragma once

// #include <memory>
#include <string>
// #include <string_view>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "error_info.hpp"
#include "logger_config.hpp"

namespace fsp
{
  // ---------------------------------------------------------------------------
  // Thread-local ime niti — vsaka nit nastavi svoje ime, ki ga formatter bere.
  // Definirano kot inline, da je vidno v vseh prevajalnih enotah brez ODR kršitev.
  // ---------------------------------------------------------------------------
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)
  inline thread_local std::string log_thread_name = "unknown";
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
  enum lvl_enum : std::uint8_t
  {
    crit  = spdlog::level::critical,
    err   = spdlog::level::err,
    warn  = spdlog::level::warn,
    info  = spdlog::level::info,
    debug = spdlog::level::debug,
    trace = spdlog::level::trace
  };
  // ---------------------------------------------------------------------------
  // fsp_logger — tanek razred okrog spdlog::logger.
  //
  // Odgovornosti:
  //   - gradi spdlog logger iz logger_config (konzola, datoteka ali oboje)
  //   - hrani shared_ptr<spdlog::logger> ki ga je mogoče deliti z workerji
  //     in validacijsko nitjo
  //   - ponuja typed log_*() metode s preverjanjem ravni (izogibamo se
  //     gradnji sporočilnih nizov ko raven ni aktivna)
  //   - izpostavlja underlying shared_ptr za kodo ki neposredno kliče spdlog
  //
  // Uporaba:
  //   fsp::fsp_logger lg(cfg.log_config);
  //   lg.info("Začenjam.");
  //   auto sptr = lg.get();          // za posredovanje workerjem
  // ---------------------------------------------------------------------------
  class fsp_logger
  {
  public:
    explicit fsp_logger(const logger_config& cfg);
    // Nekopirljiv, nepremakljiv — lastništvo se prenaša prek shared_ptr.
    fsp_logger(const fsp_logger&)            = delete;
    fsp_logger& operator=(const fsp_logger&) = delete;
    fsp_logger(fsp_logger&&)                 = delete;
    fsp_logger& operator=(fsp_logger&&)      = delete;
    ~fsp_logger();
    [[nodiscard]] std::shared_ptr<spdlog::logger> get() const;
    void                                          critical(const error_info& e) const;
    void                                          error(const error_info& e) const;
    void                                          critical(std::string_view msg) const;
    void                                          error(std::string_view msg) const;
    void                                          warn(std::string_view msg) const;
    void                                          info(std::string_view msg) const;
    void                                          debug(std::string_view msg) const;
    void                                          trace(std::string_view msg) const;
    [[nodiscard]] bool                            active(lvl_enum lvl = lvl_enum::trace) const noexcept;
    [[nodiscard]] lvl_enum                        level() const noexcept;
    void                                          set_level(lvl_enum lvl);
  private: /// methods
    static std::unique_ptr<spdlog::pattern_formatter> make_formatter(std::string_view pattern);
    void                                              build(const logger_config& cfg);
  private:
    std::shared_ptr<spdlog::logger> logger_;
    uint8_t                         level_ = spdlog::level::off; // local cache for level
  };

  /// true if level is right for logging
  [[nodiscard]] inline bool fsp_logger::active(lvl_enum lvl) const noexcept
  {
#ifdef NDEBUG
    // trace and debug are disabled in release version of the program
    if (lvl == lvl_enum::trace || lvl == lvl_enum::debug) return false;
#endif
    return static_cast<uint8_t>(lvl) >= level_;
  }
} // namespace fsp
