#pragma once

#include <logger/logger_config.hpp>
#include "parsing_util.hpp"
#include <magic_enum.hpp>
#include <optional>
#include <vector>
namespace fsp
{
  // Configuration for the importer
  struct importer_config
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    proc_data   targets;            // target points to split the xml document
    std::size_t num_of_workers = 0; // number of documents processed in parallel
    // Whether V (validation) runs at all is decided solely by whether an XSD grammar was given
    // and successfully loaded (doc_set_dscr::has_grammar(), checked in pipeline.cpp) -- no
    // separate on/off flag here, to avoid two indicators that could disagree.
    // Fold XSD validation into the same SAX pass that cuts segments (doc_cutter), instead of
    // running it as a separate full second parse (doc_validator/V role). Measured break-even is
    // between 1 and 2 concurrent documents: for a single document, C and V already run
    // concurrently on separate threads today, so merging them only serializes work that used to
    // overlap (~+24% slower, measured 17.2s vs 21.3s on a 1M-txn doc). From 2 documents up,
    // merging wins and the gain grows with document count (measured -14% at 2 docs, -28% at 10),
    // because the thread that used to be dedicated to a separate V role is instead free to help
    // cut/process other documents.
    //
    // Left unset (nullopt) by default so pipeline.cpp picks the empirically-best mode
    // automatically from the actual document count (ds_dscr_.size() > 1) whenever an XSD grammar
    // is available -- see pipeline::process_files(). Set explicitly to override that heuristic.
    std::optional<bool> cut_with_validation = std::nullopt;
    // C:P worker-thread ratio (cutter_ratio_num : cutter_ratio_den) -- num_processors is derived
    // from the actual cutter count as cutters * cutter_ratio_den / cutter_ratio_num. Default 13:10
    // was found empirically fastest on a 10-doc/10M-txn, 20-hw-thread benchmark (re-measured after
    // the results_/errors_ removal and Xerces setCopyBufToStream() changes -- 13:6, the previous
    // default, now leaves several cores idle: 41.16s/18.77GB at 13:10 vs. 51.41s/25.15GB at 13:6,
    // both with num_of_workers left at 0/hardware_concurrency(). Oversizing further (13:13, one P
    // thread per hardware thread) is WORSE, not better -- 50.49s/24.47GB -- more P threads than
    // this ratio contend over segment_pool's queues/mutexes without adding real throughput, since
    // at most max_concurrent_cutters_ (== doc_count here) documents can ever be cut concurrently
    // regardless of P thread count. Revisit if cutter_ratio_num/hardware thread count on the
    // target machine changes meaningfully from this benchmark's own (20 hw threads, 10 documents).
    std::size_t cutter_ratio_num = 13; // NOLINT(readability-magic-numbers)
    std::size_t cutter_ratio_den = 10; // NOLINT(readability-magic-numbers)
    // Number of independent shards segment_pool splits its ready/free queues into, to reduce
    // lock/condition_variable contention between concurrent C/P threads. Default 2 was found
    // empirically fastest against N=1,3,4 (see pipeline.cpp / segment_pool.hpp).
    std::size_t pool_shard_count = 2; // NOLINT(readability-magic-numbers)
    // Multiplier applied to std::thread::hardware_concurrency() before it caps
    // max_concurrent_cutters_ (see pipeline::plan_run()). Default 1.0 keeps today's behaviour (cap
    // == hw_concurrency exactly). Raising it lets a caller deliberately oversubscribe the C role
    // past the hardware thread count -- measured harmless up to 2x hw_concurrency on a
    // 20-hw-thread/10M-txn benchmark (102-103s from 20 workers up through 40, no regression), since
    // the extra threads spend most of their time blocked on I/O (DB round-trips, disk), not
    // competing for CPU. Left at 1.0, requested_threads/doc_count still clamp exactly as before --
    // this only raises the ceiling, never forces oversubscription on its own.
    double overcommit = 1.0; // NOLINT(readability-magic-numbers)
    // Batch sizes for pipeline_hooks::on_block_store()/on_failed_block_store(): a P-role thread flushes
    // its locally accumulated ok/failed segment indices once one of these many have piled up (or,
    // for whatever remains, once at thread-loop end -- see xml_worker::process_one()/
    // flush_results()). Also used to pre-size the two accumulator vectors at worker construction,
    // so normal-case operation never reallocates. Two separate knobs because ok segments are
    // expected to vastly outnumber failed ones in a healthy run.
    std::size_t ok_block_flush_size  = 1024; // NOLINT(readability-magic-numbers)
    std::size_t nak_block_flush_size = 128;  // NOLINT(readability-magic-numbers)
    logger::logger_config  log_config;                  // configuration of the
    str_t                  program_name;                // program name as displayed in the log file
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    [[nodiscard]] str_t dump(int offs) const;
  };

  inline str_t importer_config::dump(int offs) const
  {
    const str_t ind(offs, ' ');
    cstr_t      cut_with_validation_str = "unset";
    if (cut_with_validation) cut_with_validation_str = *cut_with_validation ? "true" : "false";
    return fmt::format(R"({0}targets:{1}
  {0}num_of_workers: {2}
  {0}cut_with_validation: {3}
  {0}cutter_ratio_num: {4}
  {0}cutter_ratio_den: {5}
  {0}pool_shard_count: {6}
  {0}overcommit: {7}
  {0}ok_block_flush_size: {8}
  {0}nak_block_flush_size: {9}
  {0}log_config.app_name: {10}
  {0}log_config.run_mode: {11}
  {0}log_config.console_level: {12}
  {0}log_config.file_level: {13}
  {0}log_config.log_folder: {14}
  {0}program_name: {15})",
                       ind,
                       targets.dump(offs),
                       num_of_workers,
                       cut_with_validation_str,
                       cutter_ratio_num,
                       cutter_ratio_den,
                       pool_shard_count,
                       overcommit,
                       ok_block_flush_size,
                       nak_block_flush_size,
                       log_config.app_name,
                       magic_enum::enum_name(log_config.run_mode),
                       magic_enum::enum_name(log_config.console_level),
                       magic_enum::enum_name(log_config.file_level),
                       log_config.log_folder,
                       program_name);
  }
} // namespace fsp
