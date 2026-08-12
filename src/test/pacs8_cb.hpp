// pacs8_cb.hpp
#pragma once
#include "typed_semantic_check.hpp"
#include "work.hpp" // IWYU pragma: keep -- ^^fsp::work below needs the actual schema classes, not just a forward declaration
#include <cstddef>
#include <span>

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
 *
 * Derives from fsp::typed_semantic_check<pacs8_cb, ^^fsp::work> instead of
 * fsp::pipeline_hooks_crtp<pacs8_cb> directly -- on_semantic_check() itself (the
 * materialize_variant()/std::visit()/if-constexpr plumbing) is implemented once, generically, by
 * that mixin; this class only declares one on_type() overload per fsp::work schema class (see
 * typed_semantic_check's own class comment for why BOTH are required, not just the ones with
 * real business logic).
 */
class pacs8_cb : public fsp::typed_semantic_check<pacs8_cb, ^^fsp::work>
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

  /**
   * @brief One overload per fsp::work schema class -- typed_semantic_check's own
   * on_semantic_check() dispatches to whichever of these matches the segment just materialized.
   * Each returns its own semantic verdict (true = ok) for the segment it was given.
   */
  [[nodiscard]] bool on_type(const fsp::work::pacs8_hdr& hdr,
                             fsp::segment_result&        result,
                             bool                        is_first,
                             bool                        is_last,
                             const logger::Logger&       log) const;
  [[nodiscard]] bool on_type(const fsp::work::pacs8_txn& txn,
                             fsp::segment_result&        result,
                             bool                        is_first,
                             bool                        is_last,
                             const logger::Logger&       log) const;
};