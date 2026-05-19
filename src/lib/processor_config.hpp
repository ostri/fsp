#pragma once

#include "e_tag.hpp"
#include "logger_config.hpp"
#include <optional>
namespace fsp
{
  // Configuration for the processor
  struct processor_config
  {
    std::vector<xpath_t>       targets;
    size_t                     num_workers          = 0;
    bool                       validate_against_xsd = true;
    bool                       strict_validation    = true;
    std::optional<std::string> schema_namespace;
    logger_config              log_config;
  };
} // namespace fsp
