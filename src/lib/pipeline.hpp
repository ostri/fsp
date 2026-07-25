#pragma once

#include "doc_set_dscr.hpp"
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
    [[nodiscard]] stats_t                  stats() const { return stats_; }
    [[nodiscard]] std::vector<std::size_t> failed_document_indices() const;

    // --- API for pipeline_worker / toolkits ---
    [[nodiscard]] std::expected<std::size_t, queue_status> try_pop_cut();
    [[nodiscard]] std::expected<std::size_t, queue_status> try_pop_validate();
    [[nodiscard]] std::ptrdiff_t                           c_queue_size_approx() const noexcept;
    [[nodiscard]] std::ptrdiff_t                           v_queue_size_approx() const noexcept;
    void                                                   notify_cut_done();
    void                              report_validation_result(std::size_t doc_ndx, doc_status result, error_info err = {});
    void                              report_fatal_error(error_info err);
    [[nodiscard]] segment_pool&       pool() noexcept { return pool_; }
    [[nodiscard]] const doc_set_dscr& ds_dscr() const noexcept { return ds_dscr_; }
    [[nodiscard]] vec_seg_result&     results() noexcept { return results_; }
    [[nodiscard]] vec_seg_result&     errors() noexcept { return errors_; }
    [[nodiscard]] std::mutex&         results_mutex() noexcept { return results_mutex_; }
    [[nodiscard]] std::mutex&         errors_mutex() noexcept { return errors_mutex_; }
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger&         log_;
    processor_config          cfg_;
    str_t                     parent_log_name_;
    doc_set_dscr              ds_dscr_;
    segment_pool              pool_;
    lock_queue<std::size_t>   c_queue_;
    lock_queue<std::size_t>   v_queue_;
    std::atomic<std::size_t>  docs_remaining_to_cut_{0};
    vec_seg_result            results_;
    vec_seg_result            errors_;
    mutable std::mutex        results_mutex_;
    mutable std::mutex        errors_mutex_;
    std::mutex                first_error_mutex_;
    std::optional<error_info> first_error_;
    stats_t                   stats_{};
    s_clock                   start_time_ = std::chrono::steady_clock::now();
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp