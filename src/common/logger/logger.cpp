#include "logger_impl.hpp" // IWYU pragma: keep
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace
{
  /**
   * @brief Maps a level name (trace/debug/info/warning/error/critical/off, plus the short
   * aliases warn/err/crit) to fsp::lvl_enum, mirroring spdlog::level::from_str()'s own
   * behavior: any unrecognized name silently maps to lvl_enum::off rather than being rejected.
   */
  fsp::lvl_enum level_from_str(const fsp::str_t& raw)
  {
    fsp::str_t v = raw;
    if (v == "warn") v = "warning";
    else if (v == "err") v = "error";
    else if (v == "crit") v = "critical";

    if (v == "trace") return fsp::lvl_enum::trace;
    if (v == "debug") return fsp::lvl_enum::debug;
    if (v == "info") return fsp::lvl_enum::info;
    if (v == "warning") return fsp::lvl_enum::warn;
    if (v == "error") return fsp::lvl_enum::err;
    if (v == "critical") return fsp::lvl_enum::crit;
    return fsp::lvl_enum::off;
  }

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
      else if (key == "log_level") cfg.log_level = level_from_str(fsp::str_t(val));
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
  : pimpl_(std::make_unique<impl>(cfg))
  , level_(static_cast<uint8_t>(cfg.log_level))
  {
    if (! initial_name.empty()) make_log_name(initial_name);
  }

  fsp_logger::~fsp_logger() { pimpl_->flush(); }

  void fsp_logger::flush() const { pimpl_->flush(); }

  void fsp_logger::critical(const error_info& e) const
  {
    if (active(lvl_enum::crit)) [[unlikely]]
      pimpl_->critical(e.to_string());
  }

  void fsp_logger::error(const error_info& e) const
  {
    if (active(lvl_enum::err)) [[unlikely]]
      pimpl_->error(e.to_string());
  }

  void fsp_logger::critical(cstr_t msg) const
  {
    if (active(lvl_enum::crit)) [[unlikely]]
      pimpl_->critical(msg);
  }

  void fsp_logger::error(cstr_t msg) const
  {
    if (active(lvl_enum::err)) [[unlikely]]
      pimpl_->error(msg);
  }

  void fsp_logger::warn(cstr_t msg) const
  {
    if (active(lvl_enum::warn)) [[unlikely]]
      pimpl_->warn(msg);
  }

  void fsp_logger::info(cstr_t msg) const
  {
    if (active(lvl_enum::info)) [[unlikely]]
      pimpl_->info(msg);
  }

  void fsp_logger::debug([[maybe_unused]] cstr_t msg) const
  {
    if constexpr (is_debug())
      if (active(lvl_enum::debug)) pimpl_->debug(msg);
  }

  void fsp_logger::trace([[maybe_unused]] cstr_t msg) const
  {
    if constexpr (is_debug())
      if (active(lvl_enum::trace)) pimpl_->trace(msg);
  }

  [[nodiscard]] lvl_enum fsp_logger::level() const noexcept { return static_cast<lvl_enum>(level_); }

  void fsp_logger::set_level(lvl_enum lvl)
  {
    level_ = static_cast<uint8_t>(lvl);
    pimpl_->set_level(lvl);
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

    return logger_config{.enable_console = true, .enable_file = false, .log_level = lvl_enum::info};
  }

} // namespace fsp
