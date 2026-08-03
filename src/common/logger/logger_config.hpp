#pragma once
#include <string>
#include <cstdint>

namespace fsp
{
  using str_t = std::string;

  /// @brief Mnemonic log levels; higher value == more severe. No spdlog type appears here.
  enum class lvl_enum : std::uint8_t
  {
    trace = 0,
    debug,
    info,
    warn,
    err,
    crit,
    off,
  };

  // Logger configuration
  struct logger_config
  {
    bool     enable_console = true;
    bool     enable_file    = false;
    str_t    log_file_path  = "xml_processor.log";
    lvl_enum log_level      = lvl_enum::warn;
  };
} // namespace fsp
