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
    std::size_t   num_docs             = 0;    // number of documents processed in parallel
    bool          validate_against_xsd = true; // should we validate with xsd
    // C:P worker-thread ratio (cutter_ratio_num : cutter_ratio_den) -- num_processors is derived
    // from the actual cutter count as cutters * cutter_ratio_den / cutter_ratio_num. Default
    // 13:6 was found empirically fastest on the 10-doc/10M-txn benchmark (see pipeline.cpp).
    std::size_t   cutter_ratio_num     = 13; // NOLINT(readability-magic-numbers)
    std::size_t   cutter_ratio_den     = 6;  // NOLINT(readability-magic-numbers)
    // Number of independent shards segment_pool splits its ready/free queues into, to reduce
    // lock/condition_variable contention between concurrent C/P threads. Default 2 was found
    // empirically fastest against N=1,3,4 (see pipeline.cpp / segment_pool.hpp).
    std::size_t   pool_shard_count     = 2; // NOLINT(readability-magic-numbers)
    logger_config log_config;                  // configuration of the
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::string dump(int offs) const;
  };

  inline std::string processor_config::dump(int offs) const
  {
    std::string msg;
    msg = fmt::format(R"({0}targets:{1}
  {0}num workers:{2}
  {0}validate:{3}
  {0}logger: {4})",
                      std::string(offs, ' '),
                      targets.dump(offs),
                      num_docs,
                      validate_against_xsd,
                      log_config.logger_name);
    return msg;
  }
} // namespace fsp
