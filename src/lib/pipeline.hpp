#pragma once

#include "doc_set_dscr.hpp"
#include "doc_set_counter.hpp"
#include "segment_pool.hpp"
#include "processor_config.hpp"
#include "logger.hpp"
#include "segment_result.hpp"
#include "error_info.hpp"
#include "stats.hpp"
#include "lock_queue.hpp"
#include "xpath_helpers.hpp"
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <expected>

namespace fsp
{
  using s_clock = std::chrono::time_point<std::chrono::steady_clock>;
  class pipeline
  {
  public:
    pipeline(processor_config cfg, const fsp_logger& log, str_t parent_log_name);
    void_result                            process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path);
    [[nodiscard]] const vec_seg_result&    get_results() const;
    [[nodiscard]] const vec_seg_result&    get_errors() const;
    // [[nodiscard]] stats_t                  stats() const { return stats_; }
    [[nodiscard]] std::vector<std::size_t> failed_document_indices() const;

    // --- API for pipeline_worker / toolkits ---
    [[nodiscard]] std::expected<std::size_t, queue_status> try_pop_cut();
    [[nodiscard]] std::expected<std::size_t, queue_status> try_pop_validate();
    [[nodiscard]] std::ptrdiff_t                           c_queue_size_approx() const noexcept;
    [[nodiscard]] std::ptrdiff_t                           v_queue_size_approx() const noexcept;
    void                                                   notify_cut_done();
    // Deadlock guard: at most half the threads may cut concurrently, so at least as many
    // threads remain structurally free for P as are currently committed to C.
    [[nodiscard]] bool try_reserve_cutter_slot();
    void               release_cutter_slot() noexcept;
    void               report_validation_result(std::size_t doc_ndx, doc_status result, error_info err = {});
    void               report_fatal_error(error_info err);
    // Per-document C+P end-to-end timing and semantic outcome counts (sparse info logs, for
    // benchmarking, and the running total dumped at the end of process_files()).
    void                                 record_doc_open(std::size_t doc_ndx);
    void                                 record_doc_close(std::size_t doc_ndx, std::size_t segment_count);
    void                                 record_segment_done(std::size_t doc_ndx, bool semantically_ok);
    [[nodiscard]] segment_pool&          pool() noexcept { return pool_; }
    [[nodiscard]] const doc_set_dscr&    ds_dscr() const noexcept { return ds_dscr_; }
    [[nodiscard]] const doc_set_counter& doc_counters() const noexcept { return *doc_counters_; }
    [[nodiscard]] vec_seg_result&        results() noexcept { return results_; }
    [[nodiscard]] vec_seg_result&        errors() noexcept { return errors_; }
    [[nodiscard]] std::mutex&            results_mutex() noexcept { return results_mutex_; }
    [[nodiscard]] std::mutex&            errors_mutex() noexcept { return errors_mutex_; }
  private:
    // Logs the "Doc N: cut+process finished" line -- called once a document is complete,
    // whichever of record_doc_close()/record_segment_done() turns out to be the one that
    // satisfies the last remaining condition (see doc_counters::maybe_complete()).
    void log_doc_done(std::size_t doc_ndx);
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger&                  log_;
    processor_config                   cfg_;
    str_t                              parent_log_name_;
    doc_set_dscr                       ds_dscr_;
    segment_pool                       pool_;
    lock_queue<std::size_t>            c_queue_;
    lock_queue<std::size_t>            v_queue_;
    std::atomic<std::size_t>           docs_remaining_to_cut_{0};
    std::size_t                        max_concurrent_cutters_{1}; //< computed in process_files()
    std::atomic<std::size_t>           threads_cutting_{0};
    std::optional<doc_set_counter>     doc_counters_; //< per-document timing + outcome counts, sized to doc_count in process_files()
    vec_seg_result                     results_;
    vec_seg_result                     errors_;
    mutable std::mutex                 results_mutex_;
    mutable std::mutex                 errors_mutex_;
    std::mutex                         first_error_mutex_;
    std::optional<error_info>          first_error_;
    stats_t                            stats_{};
    s_clock                            start_time_ = std::chrono::steady_clock::now();
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp