#include "logger.hpp"
#include <spdlog/pattern_formatter.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace
{
  /**
   * @brief Parses a simple "key=value" logger config file (# comments and blank lines ignored).
   * Recognized keys: enable_console, enable_file, log_file_path, log_level (spdlog level name:
   * trace/debug/info/warning/error/critical/off, plus the short aliases warn/err/crit).
   * @return std::nullopt if the file couldn't be opened.
   */
  std::optional<fsp::logger_config> parse_logger_config_file(const fsp::str_t& path)
  {
    std::ifstream in(path);
    if (! in.is_open()) return std::nullopt;

    fsp::logger_config cfg;
    fsp::str_t         line;
    while (std::getline(in, line))
    {
      auto trimmed = fsp::trim(line);
      if (trimmed.empty() || trimmed.front() == '#') continue;
      auto eq = trimmed.find('=');
      if (eq == fsp::cstr_t::npos) continue;
      auto key = fsp::trim(trimmed.substr(0, eq));
      auto val = fsp::trim(trimmed.substr(eq + 1));

      if (key == "enable_console") cfg.enable_console = (val == "true" || val == "1");
      else if (key == "enable_file") cfg.enable_file = (val == "true" || val == "1");
      else if (key == "log_file_path") cfg.log_file_path = fsp::str_t(val);
      else if (key == "log_level")
      {
        // spdlog::level::from_str() only accepts the canonical names (warning/error/critical),
        // not the short aliases some config authors might expect -- accept both spellings.
        fsp::str_t level_str(val);
        if (level_str == "warn") level_str = "warning";
        else if (level_str == "err") level_str = "error";
        else if (level_str == "crit") level_str = "critical";
        cfg.log_level = spdlog::level::from_str(level_str);
      }
    }
    return cfg;
  }
} // namespace

namespace fsp
{

  void ThreadNameFormatter::format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    dest.append(log_thread_name.data(), log_thread_name.data() + log_thread_name.size());
  }

  [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> ThreadNameFormatter::clone() const
  { return std::make_unique<ThreadNameFormatter>(); }

  /**
   * @brief Construct a new logger in accordance with the provided configuration
   *
   * If both enable_console and enable_file are false it opens color console
   * as predefined logging device.
   *
   * @param cfg logger configuration
   */
  fsp_logger::fsp_logger(const logger_config& cfg, cstr_t initial_name)
  {
    build(cfg);
    if (! initial_name.empty()) make_log_name(initial_name);
  }

  fsp_logger::~fsp_logger()
  {
    if (logger_) logger_->flush();
  }
  /**
   * @brief returns shred pointer to the logger
   * Shared pointer can be copied or transfered to the threads.
   *
   * @return std::shared_ptr<spdlog::logger>
   */
  [[nodiscard]] std::shared_ptr<spdlog::logger> fsp_logger::get() const { return logger_; }

  void fsp_logger::critical(const error_info& e) const
  {
    if (active(lvl_enum::crit)) [[unlikely]]
      logger_->critical(e.to_string());
  }

  void fsp_logger::error(const error_info& e) const
  {
    if (active(lvl_enum::err)) [[unlikely]]
      logger_->error(e.to_string());
  }

  void fsp_logger::critical(cstr_t msg) const
  {
    if (active(lvl_enum::crit)) [[unlikely]]
      logger_->critical(msg);
  }

  void fsp_logger::error(cstr_t msg) const
  {
    if (active(lvl_enum::err)) [[unlikely]]
      logger_->error(msg);
  }

  void fsp_logger::warn(cstr_t msg) const
  {
    if (active(lvl_enum::warn)) [[unlikely]]
      logger_->warn(msg);
  }

  void fsp_logger::info(cstr_t msg) const
  {
    if (active(lvl_enum::info)) [[unlikely]]
      logger_->info(msg);
  }

  void fsp_logger::debug([[maybe_unused]] cstr_t msg) const
  {
    if constexpr (is_debug())
      if (active(lvl_enum::debug)) logger_->debug(msg);
  }

  void fsp_logger::trace([[maybe_unused]] cstr_t msg) const
  {
    if constexpr (is_debug())
      if (active(lvl_enum::trace)) logger_->trace(msg);
  }

  [[nodiscard]] lvl_enum fsp_logger::level() const noexcept { return static_cast<lvl_enum>(level_); }

  void fsp_logger::set_level(lvl_enum lvl)
  {
    level_ = static_cast<uint8_t>(lvl);
    logger_->set_level(static_cast<spdlog::level::level_enum>(level_));
  }

  // auxiliary function: it builds spdlog::pattern_formatter with '%*' flag for the thread is.
  std::unique_ptr<spdlog::pattern_formatter> fsp_logger::make_formatter(cstr_t pattern)
  {
    std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
    flags['*'] = std::make_unique<ThreadNameFormatter>();
    return std::make_unique<spdlog::pattern_formatter>(
      str_t(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
  }

  void fsp_logger::build(const logger_config& cfg)
  {
    // make_log_name("main >");
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
    logger_->set_level(cfg.log_level);
    logger_->flush_on(spdlog::level::err);
    logger_->flush();
    level_ = static_cast<uint8_t>(cfg.log_level); // caching level to speedup
                                                  // should one implement set_level method, it must set
                                                  // level_ too
  }

  logger_config load_logger_config(cstr_t program_name)
  {
    // Safe: called once at startup, before any worker thread exists.
    if (const char* env_path = std::getenv("LOG_CONFIG"); env_path != nullptr && *env_path != '\0') // NOLINT(concurrency-mt-unsafe)
    {
      if (auto cfg = parse_logger_config_file(env_path)) return *cfg;
      std::cerr << "LOG_CONFIG='" << env_path << "' could not be read -- falling back.\n";
    }

    // program_name is argv[0] verbatim (e.g. "./pacs8-cb"); strip any directory
    // component so the lookup filename doesn't get mangled into a bogus path.
    const auto program_stem  = std::filesystem::path(program_name).filename().string();
    const auto fallback_path = fmt::format("log_{}_{}.conf", program_stem, is_debug() ? "debug" : "release");
    if (auto cfg = parse_logger_config_file(fallback_path)) return *cfg;

    return logger_config{.enable_console = true, .enable_file = false, .log_level = spdlog::level::info};
  }

} // namespace fsp