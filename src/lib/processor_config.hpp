#pragma once

#include "logger_config.hpp"
#include "parsing_util.hpp"
namespace fsp
{
  // Configuration for the processor
  struct processor_config
  {
    proc_data targets;                     // NOLINT(misc-non-private-member-variables-in-classes)
    size_t    num_workers          = 0;    // NOLINT(misc-non-private-member-variables-in-classes)
    bool      validate_against_xsd = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool      strict_validation    = true; // NOLINT(misc-non-private-member-variables-in-classes)
    //    std::optional<std::string> schema_namespace;
    logger_config             log_config; // NOLINT(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::string dump(int offs) const
    {
      std::string msg;
      msg = fmt::format(R"({0}targets:{1}
{0}num workers:{2}
{0}validate:{3}
{0}strict validation:{4}
{0}logger: {5})",
                        std::string(offs, ' '),
                        targets.dump(offs),
                        num_workers,
                        validate_against_xsd,
                        strict_validation,
                        log_config.logger_name);
      return msg;
    }
  };
} // namespace fsp
