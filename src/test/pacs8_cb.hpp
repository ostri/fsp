// pacs8_cb.hpp
#pragma once
#include "pipeline_hooks.hpp"
#include <cstddef>
#include <span>

namespace fsp::work
{
  class pacs8_header;
  class pacs8_txn;
} // namespace fsp::work

/**
 * @brief Demo pipeline_hooks: every hook logs its own name and its parameters at info level.
 *
 * One instance per worker thread (see pipeline_hooks.hpp's clone() contract), so documents_seen
 * below is plain (non-atomic) -- only ever touched by the one thread that owns it. Segment
 * ok/error counts are NOT tracked here: pipeline::record_segment_done() already folds
 * on_semantic_check()'s own return value into doc_counters (see pipeline.cpp) for every segment,
 * whether or not a hook is even installed, so on_run_end()'s own `counters` parameter already
 * carries the true, authoritative totals (counters.total_segments_ok()/total_segments_error()) --
 * keeping a second, hook-local copy here would just duplicate that bookkeeping.
 */
class pacs8_cb : public fsp::pipeline_hooks_crtp<pacs8_cb>
{
public:
  std::size_t documents_seen = 0; // NOLINT(misc-non-private-member-variables-in-classes)

  void on_run_start(const fsp::doc_set_dscr& ds_dscr, const logger::Logger& log) override;
  void on_run_end(const fsp::doc_set_counter&           counters,
                  const fsp::doc_set_dscr&              ds_dscr,
                  std::span<const fsp::pipeline_hooks*> worker_clones,
                  const logger::Logger&                 log) override;
  void on_wrk_start(int worker_id, fsp::cstr_t thread_name, const logger::Logger& log) override;
  void on_wrk_end(int worker_id, fsp::cstr_t thread_name, const logger::Logger& log) override;
  void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const logger::Logger& log) override;
  void on_doc_close(std::size_t doc_ndx, fsp::doc_status status, const fsp::doc_dscr& dscr, const logger::Logger& log) override;
  bool on_semantic_check(const fsp::xml_segment& segment,
                         fsp::segment_result&    result,
                         bool                    is_first,
                         bool                    is_last,
                         const logger::Logger&   log) override;
private:
  /**
   * @brief Per-segment-type processing, factored out of on_semantic_check() so it stays pure plumbing.
   * Each returns its own semantic verdict (true = ok) for the segment it was given.
   */
  [[nodiscard]] bool process_header(const fsp::work::pacs8_header& hdr,
                                    const fsp::segment_result&     result,
                                    bool                           is_first,
                                    bool                           is_last,
                                    const logger::Logger&          log) const;
  [[nodiscard]] bool process_txn(const fsp::work::pacs8_txn& txn,
                                 const fsp::segment_result&  result,
                                 bool                        is_first,
                                 bool                        is_last,
                                 const logger::Logger&       log) const;
};