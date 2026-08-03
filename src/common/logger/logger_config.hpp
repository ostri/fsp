#pragma once
#include <string>
#include <spdlog/spdlog.h>

namespace fsp
{
  using str_t = std::string;
  // Logger configuration
  struct logger_config
  {
    bool                      enable_console = true;
    bool                      enable_file    = false;
    str_t                     log_file_path  = "xml_processor.log";
    spdlog::level::level_enum log_level      = spdlog::level::warn;
  };
} // namespace fsp
