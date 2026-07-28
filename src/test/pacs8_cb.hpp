// pacs8_cb.hpp
#pragma once
#include "pipeline_hooks.hpp"
#include <chrono>
#include <cstddef>
#include <span>

/**
 * @brief Demo pipeline_hooks: every hook logs its own name and its parameters at info level.
 *
 * One instance per worker thread (see pipeline_hooks.hpp's clone() contract), so the counters
 * below are plain (non-atomic) -- each is only ever touched by the one thread that owns it.
 * on_run_end() folds every worker clone's counters into a cumulative total.
 */
class pacs8_cb : public fsp::pipeline_hooks_crtp<pacs8_cb>
{
public:
  std::size_t documents_seen = 0; // NOLINT(misc-non-private-member-variables-in-classes)
  std::size_t segments_seen  = 0; // NOLINT(misc-non-private-member-variables-in-classes)
  std::size_t segments_ok    = 0; // NOLINT(misc-non-private-member-variables-in-classes)
  std::size_t segments_error = 0; // NOLINT(misc-non-private-member-variables-in-classes)

  // run_start_ is only meaningful on the ORIGINAL instance (on_run_start/on_run_end are the
  // only two hooks called on it, never on a clone). worker_start_ is per-clone, set and read
  // by the one thread that owns that clone.
  std::chrono::steady_clock::time_point run_start_;    // NOLINT(misc-non-private-member-variables-in-classes)
  std::chrono::steady_clock::time_point worker_start_; // NOLINT(misc-non-private-member-variables-in-classes)

  void on_run_start(const fsp::doc_set_dscr& ds_dscr, const fsp::fsp_logger& log) override;
  void on_run_end(const fsp::doc_set_counter&           counters,
                  const fsp::doc_set_dscr&              ds_dscr,
                  std::span<const fsp::pipeline_hooks*> worker_clones,
                  const fsp::fsp_logger&                log) override;
  void on_wrk_start(int worker_id, fsp::cstr_t thread_name, const fsp::fsp_logger& log) override;
  void on_wrk_end(int worker_id, fsp::cstr_t thread_name, const fsp::fsp_logger& log) override;
  void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log) override;
  void on_doc_close(std::size_t doc_ndx, fsp::doc_status status, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log) override;
  bool on_seg_proc(const fsp::xml_segment&    segment,
                   const fsp::segment_result& result,
                   bool                       is_first,
                   bool                       is_last,
                   const fsp::fsp_logger&     log) override;
};