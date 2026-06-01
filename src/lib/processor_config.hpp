#pragma once

#include "logger_config.hpp"
#include "parsing_util.hpp"
namespace fsp
{
  // Configuration for the processor
  struct processor_config
  {
    proc_data targets;
    size_t    num_workers          = 0;
    bool      validate_against_xsd = true;
    bool      strict_validation    = true;
    //    std::optional<std::string> schema_namespace;
    logger_config log_config;
  };
} // namespace fsp
