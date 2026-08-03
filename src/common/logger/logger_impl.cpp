#include "logger_impl.hpp"

namespace fsp
{
  fsp_logger::impl::impl(const logger_config& cfg) { build(cfg); }

  void fsp_logger::impl::flush() const
  {
    if (logger_) logger_->flush();
  }

  void fsp_logger::impl::critical(cstr_t msg) const { logger_->critical(msg); }
  void fsp_logger::impl::error(cstr_t msg) const { logger_->error(msg); }
  void fsp_logger::impl::warn(cstr_t msg) const { logger_->warn(msg); }
  void fsp_logger::impl::info(cstr_t msg) const { logger_->info(msg); }
  void fsp_logger::impl::debug(cstr_t msg) const { logger_->debug(msg); }
  void fsp_logger::impl::trace(cstr_t msg) const { logger_->trace(msg); }

  void fsp_logger::impl::set_level(lvl_enum lvl) { logger_->set_level(static_cast<spdlog::level::level_enum>(lvl)); }

  // auxiliary function: it builds spdlog::pattern_formatter with '%*' flag for the thread id.
  std::unique_ptr<spdlog::pattern_formatter> fsp_logger::impl::make_formatter(cstr_t pattern)
  {
    std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
    flags['*'] = std::make_unique<ThreadNameFormatter>();
    return std::make_unique<spdlog::pattern_formatter>(
      str_t(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
  }

  void fsp_logger::impl::build(const logger_config& cfg)
  {
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
    // spdlog requires a name here, but this logger is never registered in spdlog's global
    // registry and the log pattern doesn't use %n, so this value never surfaces anywhere.
    logger_ = std::make_shared<spdlog::logger>("fsp", sinks.begin(), sinks.end());
    logger_->set_level(static_cast<spdlog::level::level_enum>(cfg.log_level));
    logger_->flush_on(spdlog::level::err);
    logger_->flush();
  }

} // namespace fsp
