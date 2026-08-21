#pragma once

#include "doc_set_dscr.hpp"
#include "doc_set_counter.hpp"
#include "pipeline_hooks.hpp"
#include "segment_pool.hpp"
#include "importer_config.hpp"
#include <logger/logger.hpp>
#include "segment_result.hpp"
#include "error_info.hpp"
#include "stats.hpp"
#include "lock_queue.hpp"
#include "xpath_helpers.hpp"
#include "xml_segment.hpp"
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <expected>

namespace fsp
{
  class pipeline_worker; // forward declaration is enough -- only used via unique_ptr/reference here

  using s_clock = std::chrono::time_point<std::chrono::steady_clock>;
  class pipeline
  {
  public:
    pipeline(const importer_config& cfg, const logger::Logger& log, str_t parent_log_name);
    [[nodiscard]] result<doc_set_counter> process_files(const std::vector<str_t>& xml_paths,
                                                        cstr_t                    xsd_path,
                                                        pipeline_hooks&           hooks = default_pipeline_hooks);
    [[nodiscard]] const vec_seg_result&   get_results() const;
    [[nodiscard]] const vec_seg_result&   get_errors() const;
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
    // Reported by C (pipeline_worker::do_cut()) once doc_cutter::cut() returns -- folds ok into
    // doc_dscr::set_syntax_result() (see its own doc comment for the separate-V vs.
    // folded-C-with-validation rules; folded is this run's own cut_with_validation_ flag, decided
    // once in plan_run()). If that call is the one that wins doc_status_t::try_start_closing()
    // (see doc_dscr.hpp), dispatches hooks.on_doc_safe_close() itself, right here.
    void report_syntax_result(std::size_t doc_ndx, bool ok, pipeline_hooks& hooks, error_info err = {});
    // Reported by V (pipeline_worker::do_validate(), a SEPARATE validation pass) once
    // doc_validator::validate() returns -- folds ok into doc_dscr::set_validation_result(). Same
    // "dispatch on_doc_safe_close() if this call wins try_start_closing()" behavior as
    // report_syntax_result() above.
    void report_validation_result(std::size_t doc_ndx, bool ok, pipeline_hooks& hooks, error_info err = {});
    // Called by xml_worker::flush_ok_block()/flush_nak_block(), once per (document, flush-batch)
    // pair, AFTER hooks.on_block_safe_store()/on_failed_block_safe_store() has already returned
    // success for that batch -- count is however many of doc_ndx's own segments were in that one
    // batch (a batch can freely mix segments from several different documents, and one document's
    // segments can be flushed across several batches from several different worker threads -- see
    // docs/importer_usage.md's own "Knowing every segment has been stored" section). Folds count
    // into doc_counters()[doc_ndx].add_segments_stored() (see its own doc comment) -- if THAT call
    // is the one whose running total crosses doc_ndx's known segment count, dispatches
    // hooks.on_doc_safe_stored() and folds ITS result into doc_dscr::set_stored_result(); if THAT
    // wins doc_status_t::try_start_closing() (see doc_dscr.hpp), dispatches hooks.on_doc_safe_close()
    // itself, right here -- same "dispatch on_doc_safe_close() if this call wins try_start_closing()"
    // shape as report_syntax_result()/report_validation_result() above.
    void record_segments_stored(std::size_t doc_ndx, std::size_t count, pipeline_hooks& hooks);
    void report_fatal_error(error_info err);
    // Per-document C+P end-to-end timing and semantic outcome counts (sparse info logs, for
    // benchmarking, and the running total dumped at the end of process_files()). May itself
    // trigger the "all segments processed" completion (see doc_counters::maybe_seg_processing_complete())
    // and, from there, hooks.on_doc_safe_sem_check() + doc_status_t::set_semantic() + (possibly)
    // hooks.on_doc_safe_close() -- see maybe_finish_seg_processing().
    void record_doc_open(std::size_t doc_ndx);
    void record_doc_close(std::size_t doc_ndx, std::size_t segment_count, pipeline_hooks& hooks);
    // Runs ONLY the on_seg_sem_check hook -- no doc_counters bookkeeping, so this alone can never
    // trigger the "all segments processed" cascade (unlike the old, single-call record_segment_done()
    // this replaces). Split out so a caller (xml_worker::process_one()) can flush this segment into
    // storage (record_ok()/record_nak() -> flush_ok_block()/flush_nak_block()) BEFORE calling
    // finish_segment() below -- otherwise a segment that happens to be its document's last could
    // reach hooks.on_doc_safe_close() while still sitting unflushed in ok_block_indices_/
    // nak_block_indices_, letting a caller's on_doc_close() run before that segment's own storage
    // write ever happened. Returns the segment's own verdict (NOT the document's -- see
    // on_doc_sem_check() for that).
    bool check_segment_semantics(const xml_segment& segment, segment_result& result, pipeline_hooks& hooks);
    // Second half of the old record_segment_done(): folds semantically_ok (check_segment_semantics()'s
    // own return value) into doc_counters -- may trigger the "all segments processed" cascade (and,
    // from there, hooks.on_doc_safe_close()). Call this AFTER the segment has been flushed into
    // storage (see check_segment_semantics()'s own doc comment above).
    void finish_segment(std::size_t doc_ndx, bool semantically_ok, pipeline_hooks& hooks);
    // For a segment that failed technically (never reached process_segment()'s value extraction,
    // so there's no result_values to hand to a hook) -- bookkeeping only, no hook call, but may
    // still trigger the same cascade as finish_segment() above.
    void                                 record_segment_failed(std::size_t doc_ndx, std::size_t seg_id, pipeline_hooks& hooks);
    [[nodiscard]] segment_pool&          pool() noexcept { return seg_pool_; }
    [[nodiscard]] const doc_set_dscr&    ds_dscr() const noexcept { return ds_dscr_; }
    [[nodiscard]] const doc_set_counter& doc_counters() const noexcept { return *doc_counters_; }
    [[nodiscard]] vec_seg_result&        results() noexcept { return results_; }
    [[nodiscard]] vec_seg_result&        errors() noexcept { return errors_; }
    [[nodiscard]] std::mutex&            results_mutex() noexcept { return results_mutex_; }
    [[nodiscard]] std::mutex&            errors_mutex() noexcept { return errors_mutex_; }
    // This run's single run-level shared-data instance (see run_doc_data.hpp) -- constructed via
    // hooks.make_run_data_struct() right before hooks.on_run_safe_start() in process_files(),
    // destroyed right after hooks.on_run_safe_end() returns. The SAME instance for every worker
    // clone (see pipeline_hooks::run_data_impl()'s own doc comment).
    [[nodiscard]] run_data_root& run_data() noexcept
    {
      assert(run_data_ != nullptr && "pipeline::run_data() called outside process_files()'s on_run_safe_start()/on_run_safe_end() window");
      return *run_data_;
    }
    // doc_ndx's own doc-level shared-data instance (see run_doc_data.hpp) -- assigned (possibly
    // recycled from a previously-finished document, see doc_data_root::reset()) right before
    // pipeline_worker::do_cut() unconditionally (see its own doc comment on why this happens even
    // for a document whose cut is skipped), released back to the recycling pool once BOTH
    // independent consumers are done with it -- see mark_doc_data_reader_done()'s own doc comment
    // on why one release call isn't enough. Never null in between.
    [[nodiscard]] doc_data_root& doc_data(std::size_t doc_ndx) noexcept
    {
      auto* p =
        doc_data_active_[doc_ndx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- doc_ndx always caller-bounded
      assert(p != nullptr && "pipeline::doc_data(doc_ndx) called before assign_doc_data(doc_ndx) or after both readers finished with it");
      return *p;
    }
    // Assigns doc_ndx a doc-level shared-data instance -- a recycled one from the free-list if
    // available, else a freshly hooks.make_doc_data_struct()-ed one (pool grows by one). Called
    // once, on whichever thread calls pipeline_worker::do_cut(doc_ndx), unconditionally (see
    // do_cut()'s own doc comment), before either of doc_data()'s two independent readers
    // (maybe_finish_seg_processing()/finish_doc_close(), see doc_status_t's own "any single fact
    // can short-circuit the other two" doc comment in doc_dscr.hpp -- these two can complete in
    // EITHER order, so neither alone is a safe point to recycle from).
    void assign_doc_data(std::size_t doc_ndx, pipeline_hooks& hooks);
    // Marks one of doc_ndx's two independent doc_data() readers as done -- maybe_finish_seg_
    // processing() and finish_doc_close() each call this once, themselves (NOT
    // hooks.on_doc_safe_finish() -- this function dispatches that itself, see below). Only the
    // SECOND call (for a given doc_ndx) actually fires hooks.on_doc_safe_finish(doc_ndx) and
    // returns the slot to the free-list -- see doc_status_t's own doc comment in doc_dscr.hpp for
    // why a single fact (e.g. validation failing) can make finish_doc_close() run before segment
    // processing has even finished, so firing on_doc_safe_finish()/recycling on just one of these
    // two signals would race the other one still reading/writing doc_data(doc_ndx).
    void mark_doc_data_reader_done(std::size_t doc_ndx, pipeline_hooks& hooks);
  private:
    // Logs the "Doc N: cut+process finished" line -- called once a document's segment processing
    // is complete, whichever of record_doc_close()/record_segment_done() turns out to be the one
    // that satisfies the last remaining condition (see doc_counters::maybe_seg_processing_complete()).
    void log_doc_done(std::size_t doc_ndx);
    // Shared tail of record_doc_close()/record_segment_done()/record_segment_failed() -- called
    // only once doc_counters::maybe_seg_processing_complete() (the ORIGINAL, single-stage "all
    // segments processed" condition, unrelated to syntax/validation) has just been won by the
    // caller. Dispatches hooks.on_doc_safe_sem_check(), feeds its bool result into
    // doc_dscr::set_semantic_result() (see doc_dscr.hpp's doc_status_t), and -- if THAT call wins
    // doc_status_t::try_start_closing() -- dispatches hooks.on_doc_safe_close() itself, right here.
    void maybe_finish_seg_processing(std::size_t doc_ndx, pipeline_hooks& hooks);
    // Calls the renamed on_doc_close() hook and logs its verdict -- called by whichever of
    // report_syntax_result()/report_validation_result()/maybe_finish_seg_processing() is the one
    // whose doc_dscr::set_*_result() call won doc_status_t::try_start_closing() (see doc_dscr.hpp)
    // -- fsp-core guarantees this happens for each document EXACTLY once, from exactly one of
    // those three call sites.
    void finish_doc_close(std::size_t doc_ndx, pipeline_hooks& hooks);

    // --- process_files() broken into named phases, purely to keep each piece small and
    // separately readable -- none of these are meant to be called from anywhere else. ---
    // hooks: only get_doc_id() is called here, once per document, on the main thread, before
    // add_documents() returns -- see pipeline_hooks::get_doc_id()'s own doc comment.
    [[nodiscard]] e_void add_documents(const std::vector<str_t>& xml_paths, cstr_t xsd_path, pipeline_hooks& hooks);
    // Modulo used to turn a doc_ndx into get_doc_id()'s node_hint parameter -- deliberately
    // generic (not, say, a Snowflake-specific "max node id"): pipeline/importer stay
    // domain-neutral, a hook implementation (e.g. one built on a Snowflake-style id generator) is
    // the one that gives node_hint any real meaning. 1024 is simply a round number comfortably
    // above realistic worker-thread counts.
    static constexpr std::size_t doc_id_node_hint_modulo = 1024; // NOLINT(readability-magic-numbers)
    struct run_plan
    {
      bool        run_validation;      // NOLINT(misc-non-private-member-variables-in-classes)
      bool        cut_with_validation; // NOLINT(misc-non-private-member-variables-in-classes)
      std::size_t num_parallel;        // NOLINT(misc-non-private-member-variables-in-classes)
    };
    // Also sets max_concurrent_cutters_ as a side effect (needed by try_reserve_cutter_slot()).
    [[nodiscard]] run_plan                                              plan_run(std::size_t doc_count);
    void                                                                seed_queues(std::size_t doc_count, bool run_validation);
    [[nodiscard]] result<std::vector<std::unique_ptr<pipeline_worker>>> start_workers(std::size_t num_parallel, pipeline_hooks& hooks);
    static void         run_workers(std::vector<std::unique_ptr<pipeline_worker>>& worker_state);
    void                discard_invalid_doc_results();
    [[nodiscard]] str_t build_summary(std::size_t doc_count, double elapsed_ms, std::size_t failed_count) const;
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const logger::Logger&    log_;             //< reference to the logger
    const importer_config&   cfg_;             //< copy of the config passed to the constructor, used by process_files() and its helpers
    str_t                    parent_log_name_; //< parent worker thread's name (parent_log_name_ + ".worker_N")
    doc_set_dscr             ds_dscr_;         //< doc_set_dscr is thread-safe, so one instance is shared by all workers
    segment_pool             seg_pool_;        //< segment_pool is thread-safe, so one instance is shared by all workers
    lock_queue<std::size_t>  c_queue_;         //< queue of document indices for cutting (C) -- one instance is shared by all workers
    lock_queue<std::size_t>  v_queue_;         //< queue of document indices for validation (V) -- one instance is shared by all workers
    std::atomic<std::size_t> docs_remaining_to_cut_{0};
    std::size_t              max_concurrent_cutters_{1}; //< computed in process_files()
    std::atomic<std::size_t> threads_cutting_{
      0}; //< used by try_reserve_cutter_slot() to enforce the "at most half the threads may cut concurrently" rule
    std::optional<doc_set_counter> doc_counters_;      //< per-document timing + outcome counts, sized to doc_count in process_files()
    vec_seg_result                 results_;           //< all segments that were processed successfully
    vec_seg_result                 errors_;            //< all segments that failed syntactically or semantically
    mutable std::mutex             results_mutex_;     //< protects results_
    mutable std::mutex             errors_mutex_;      //< protects errors_
    std::mutex                     first_error_mutex_; //< protects first_error_
    std::optional<error_info>      first_error_;       //<  the first fatal error reported by any worker thread, if any
    stats_t                        stats_{};           //< cumulative stats for the run, updated by multiple threads
    s_clock                        start_time_ = std::chrono::steady_clock::now(); //< to compute total elapsed time for the run
    // This run's own cut_with_validation/run_validation effective values (see plan_run()) -- both
    // set once, on the main thread, before any worker starts; read (never written) by every
    // worker thread afterwards, so no synchronization is needed (same happens-before argument as
    // doc_dscr::out_doc_id_). report_syntax_result() needs cut_with_validation_ to apply
    // doc_dscr::set_syntax_result()'s correct folded-vs-separate-V rule (point 14 of the design
    // discussion this implements) -- C is the sole authority for BOTH syntax and validation only
    // when cut_with_validation_ genuinely folded V into its own SAX pass. When NEITHER
    // cut_with_validation_ NOR run_validation_ is true (no XSD grammar supplied at all, see
    // plan_run()), C still only ever reports syntax -- process_files() pre-seeds every document's
    // doc_status_t::valid_ to three_state::valid on the MAIN thread instead (round 6 of the design
    // discussion: a worker role must never claim a verdict it didn't actually produce, even a
    // convenient one).
    bool cut_with_validation_ = false;
    bool run_validation_      = false;
    // This run's single run-level shared-data instance -- constructed in process_files() right
    // before hooks.on_run_safe_start(), destroyed right after hooks.on_run_safe_end() returns.
    // See run_data()'s own doc comment.
    std::unique_ptr<run_data_root> run_data_;
    // Doc-level shared-data recycling pool (see run_data_/doc_data()'s own doc comments):
    // doc_data_storage_ OWNS every instance ever created (grows on demand, never shrinks -- see
    // assign_doc_data()); doc_data_free_ holds previously-released instances available for reuse;
    // doc_data_active_ is the doc_ndx -> currently-assigned-instance map; doc_data_pending_
    // readers_ is doc_ndx's own countdown of independent readers not yet done with it (see
    // mark_doc_data_reader_done()'s own doc comment on why 2, not 1) -- all sized to doc_count in
    // add_documents(). All four are protected by doc_data_pool_mutex_, since assign_doc_data()/
    // mark_doc_data_reader_done() can run concurrently on different documents from different
    // worker threads.
    std::mutex                                  doc_data_pool_mutex_;
    std::vector<std::unique_ptr<doc_data_root>> doc_data_storage_;
    std::vector<doc_data_root*>                 doc_data_free_;
    std::vector<doc_data_root*>                 doc_data_active_;
    std::vector<int>                            doc_data_pending_readers_;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp