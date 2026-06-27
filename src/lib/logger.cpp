#include "logger.hpp"
#include <spdlog/pattern_formatter.h>

namespace fsp
{

  void ThreadNameFormatter::format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(log_thread_name.data(), log_thread_name.data() + log_thread_name.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> ThreadNameFormatter::clone() const
  { return std::make_unique<ThreadNameFormatter>(); }

  // Zgradi logger glede na logger_config.
  // Če sta enable_console in enable_file oba false, samodejno odpre barvno
  // konzolno izhajanje kot rezervni izhod.
  fsp_logger::fsp_logger(const logger_config& cfg) { build(cfg); }

  fsp_logger::~fsp_logger()
  {
    if (logger_) logger_->flush();
  }

  /// Vrne shared_ptr ki ga je varno kopirati in posredovati nitim.
  [[nodiscard]] std::shared_ptr<spdlog::logger> fsp_logger::get() const { return logger_; }

  void fsp_logger::critical(const error_info& e) const
  {
    if (active(crit)) [[unlikely]]
      logger_->critical(e.to_string());
  }

  void fsp_logger::error(const error_info& e) const
  {
    if (active(err)) [[unlikely]]
      logger_->error(e.to_string());
  }

  void fsp_logger::critical(std::string_view msg) const
  {
    if (active(crit)) [[unlikely]]
      logger_->critical(msg);
  }

  void fsp_logger::error(std::string_view msg) const
  {
    if (active(err)) [[unlikely]]
      logger_->error(msg);
  }

  void fsp_logger::warn(std::string_view msg) const
  {
    if (active(lvl_enum::warn)) [[unlikely]]
      logger_->warn(msg);
  }

  void fsp_logger::info(std::string_view msg) const
  {
    if (active(lvl_enum::info)) [[unlikely]]
      logger_->info(msg);
  }

  void fsp_logger::debug([[maybe_unused]] std::string_view msg) const
  {
#ifndef NDEBUG
    if (active(lvl_enum::debug)) [[unlikely]]
      logger_->debug(msg);
#endif
  }

  void fsp_logger::trace([[maybe_unused]] std::string_view msg) const
  {
#ifndef NDEBUG
    if (active(lvl_enum::trace)) [[unlikely]]
      logger_->trace(msg);
#endif
  }

  [[nodiscard]] lvl_enum fsp_logger::level() const noexcept { return static_cast<lvl_enum>(level_); }

  void fsp_logger::set_level(lvl_enum lvl)
  {
    level_ = static_cast<uint8_t>(lvl);
    logger_->set_level(static_cast<spdlog::level::level_enum>(level_));
  }

  // auxiliary function: it builds spdlog::pattern_formatter with '%*' flag for the thread is.
  std::unique_ptr<spdlog::pattern_formatter> fsp_logger::make_formatter(std::string_view pattern)
  {
    std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
    flags['*'] = std::make_unique<ThreadNameFormatter>();
    return std::make_unique<spdlog::pattern_formatter>(
      std::string(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
  }

  void fsp_logger::build(const logger_config& cfg)
  {
    log_thread_name = "main >";
    std::vector<spdlog::sink_ptr> sinks;
    if (cfg.enable_console) // console logger
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v"));
      sinks.push_back(std::move(s));
    }
    if (cfg.enable_file && ! cfg.log_file_path.empty()) // file logger
    {
      auto s = std::make_shared<spdlog::sinks::basic_file_sink_mt>(cfg.log_file_path, true);
      s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%-5l] %v"));
      sinks.push_back(std::move(s));
    }
    // backup console if no sink is configured
    if (sinks.empty())
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v"));
      sinks.push_back(std::move(s));
    }
    logger_ = std::make_shared<spdlog::logger>(cfg.logger_name, sinks.begin(), sinks.end());
    logger_->set_level(cfg.log_level);
    logger_->flush_on(spdlog::level::err);
    logger_->flush();
    level_ = static_cast<uint8_t>(cfg.log_level); // caching level to speedup
                                                  // should one implement set_level method, it must set
                                                  // level_ too
  }

} // namespace fsp