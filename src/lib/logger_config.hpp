#pragma once
#include <string>
#include <spdlog/spdlog.h>

namespace fsp
{
  // Logger configuration
  struct logger_config
  {
    bool                      enable_console = true;
    bool                      enable_file    = false;
    std::string               log_file_path  = "xml_processor.log";
    spdlog::level::level_enum log_level      = spdlog::level::info;
    std::string               logger_name    = "xml_processor";
  };
} // namespace fsp
