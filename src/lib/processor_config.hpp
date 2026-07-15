#pragma once

#include "logger_config.hpp"
#include "parsing_util.hpp"
namespace fsp
{
  // Configuration for the processor
  struct processor_config
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    proc_data     targets;                     // target points to split the xml document
    std::size_t   num_workers          = 0;    // number of workers processing one file
    std::size_t   num_docs             = 0;    // number of documents processed in parallel
    bool          validate_against_xsd = true; // should we validate with xsd
    logger_config log_config;                  // configuration of the
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::string dump(int offs) const;
  };

  inline std::string processor_config::dump(int offs) const
  {
    std::string msg;
    msg = fmt::format(R"({0}targets:{1}
  {0}num workers:{2}
  {0}num workers:{3}
  {0}validate:{4}
  {0}logger: {5})",
                      std::string(offs, ' '),
                      targets.dump(offs),
                      num_workers,
                      num_docs,
                      validate_against_xsd,
                      log_config.logger_name);
    return msg;
  }
} // namespace fsp
