// pacs8_cb.hpp
#pragma once
#include "typed_semantic_check.hpp"
#include "work.hpp" // IWYU pragma: keep -- ^^fsp::work below needs the actual schema classes, not just a forward declaration
#include <cstddef>
#include <memory>
#include <span>

/**
 * @brief Demo pipeline_hooks: every hook logs its own name and its parameters at info level.
 *
 * One instance per worker thread (see pipeline_hooks.hpp's clone() contract), so documents_seen
 * below is plain (non-atomic) -- only ever touched by the one thread that owns it. Segment
 * ok/error counts are NOT tracked here: pipeline::record_segment_done() already folds
 * on_seg_sem_check()'s own return value into doc_counters (see pipeline.cpp) for every segment,
 * whether or not a hook is even installed, so on_run_end()'s own `counters` parameter already
 * carries the true, authoritative totals (counters.total_segments_ok()/total_segments_error()) --
 * keeping a second, hook-local copy here would just duplicate that bookkeeping.
 *
 * Derives from fsp::typed_semantic_check<pacs8_cb, ^^fsp::work> instead of
 * fsp::pipeline_hooks_crtp<pacs8_cb> directly -- on_seg_sem_check() itself (the
 * materialize_variant()/std::visit()/if-constexpr plumbing) is implemented once, generically, by
 * that mixin; this class only declares one on_type() overload per fsp::work schema class (see
 * typed_semantic_check's own class comment for why BOTH are required, not just the ones with
 * real business logic).
 *
 * Overrides the plain hooks (on_run_start(), on_wrk_start(), ...), not the "_safe" ones
 * (on_run_safe_start()/on_wrk_safe_start()/... ) themselves -- those are final in pipeline_hooks,
 * precisely so log_/run_start_/worker_start_ can never end up unset by a derived class forgetting
 * to chain to the base body (see pipeline_hooks.hpp's own class comment). log() (inherited,
 * protected) replaces the old `const logger::Logger& log` parameter every hook used to take.
 *
 * Doesn't care about doc-level semantics (no NbOfTxs/TtlIntrBkSttlmAmt-vs-actual aggregation
 * demo here) -- uses the default run_data_root/doc_data_root (timing only), so it doesn't name
 * either of typed_semantic_check's RunData/DocData template arguments.
 */
class pacs8_cb : public fsp::typed_semantic_check<pacs8_cb, ^^fsp::work>
{
public:
  /**
   * @brief One overload per fsp::work schema class -- typed_semantic_check's own
   * on_seg_sem_check() dispatches to whichever of these matches the segment just
   * materialized. Each returns its own semantic verdict (true = ok) for the segment it was given.
   */
  [[nodiscard]] bool on_type(const fsp::work::pacs8_hdr& hdr,
                             std::string_view            raw_msg,
                             const fsp::doc_dscr&        dscr,
                             fsp::segment_result&        result,
                             bool                        is_first,
                             bool                        is_last) const;
  [[nodiscard]] bool on_type(const fsp::work::pacs8_txn& txn,
                             std::string_view            raw_msg,
                             const fsp::doc_dscr&        dscr,
                             fsp::segment_result&        result,
                             bool                        is_first,
                             bool                        is_last) const;
protected:
  // pipeline_hooks::get_doc_agent_id()'s own truly-unoverridden default returns 0 (fsp-core's own
  // "unresolved agent" convention, see its own doc comment) -- this demo has no real agent
  // dictionary to resolve against, and isn't demonstrating that mechanism, so it overrides with a
  // fixed, non-zero id instead, to keep documents processed normally like every other hook here.
  [[nodiscard]] std::optional<std::int16_t> get_doc_agent_id(fsp::cstr_t path) override;
  [[nodiscard]] fsp::e_void                 on_run_start(const fsp::doc_set_dscr& ds_dscr) override;
  [[nodiscard]] fsp::e_void                 on_run_end(const fsp::doc_set_counter&           counters,
                                                       const fsp::doc_set_dscr&              ds_dscr,
                                                       std::span<const fsp::pipeline_hooks*> worker_clones) override;
  [[nodiscard]] fsp::e_void                 on_wrk_start(int worker_id, fsp::cstr_t thread_name) override;
  [[nodiscard]] fsp::e_void                 on_wrk_end(int worker_id, fsp::cstr_t thread_name) override;
  [[nodiscard]] fsp::e_void                 on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr) override;
  [[nodiscard]] fsp::e_void                 on_doc_cutting_end(std::size_t doc_ndx, const fsp::doc_dscr& dscr) override;
  [[nodiscard]] fsp::e_void                 on_doc_stored(std::size_t doc_ndx, const fsp::doc_dscr& dscr) override;
  [[nodiscard]] bool                        on_doc_close(std::size_t              doc_ndx,
                                                         const fsp::doc_status_t& verdict,
                                                         const fsp::error_info&   err,
                                                         const fsp::doc_dscr&     dscr,
                                                         std::size_t              segments_stored) override;
  [[nodiscard]] fsp::e_void                 on_doc_finish(std::size_t doc_ndx) override;
private:
  std::size_t documents_seen = 0;
};