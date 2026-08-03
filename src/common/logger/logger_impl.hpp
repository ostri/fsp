#pragma once
#include "logger.hpp" // IWYU pragma: keep

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace fsp
{
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

  class fsp_logger::impl
  {
  public:
    explicit impl(const logger_config& cfg);
    void                                          flush() const;
    void                                          critical(cstr_t msg) const;
    void                                          error(cstr_t msg) const;
    void                                          warn(cstr_t msg) const;
    void                                          info(cstr_t msg) const;
    void                                          debug(cstr_t msg) const;
    void                                          trace(cstr_t msg) const;
    void                                          set_level(lvl_enum lvl);
  private:
    static std::unique_ptr<spdlog::pattern_formatter> make_formatter(cstr_t pattern);
    void                                               build(const logger_config& cfg);
  private:
    std::shared_ptr<spdlog::logger> logger_;
  };
} // namespace fsp
