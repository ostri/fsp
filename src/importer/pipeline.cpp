#include "pipeline.hpp"
#include "pipeline_worker.hpp"
#include <thread>
#include <tuple> // std::ignore -- see the on_*_safe_*() call sites below
#include <fmt/format.h>

namespace fsp
{
  pipeline::pipeline(const importer_config& cfg, const logger::Logger& log, str_t parent_log_name)
  : log_(log)
  , cfg_(std::move(cfg))
  , parent_log_name_(std::move(parent_log_name))
  , ds_dscr_(log_)
  , seg_pool_(log_, 1024UL * 1024UL * 8UL, std::max<std::size_t>(1, cfg_.pool_shard_count)) // NOLINT(readability-magic-numbers)
  {
  }

  std::expected<std::size_t, queue_status> pipeline::try_pop_cut() { return c_queue_.try_pop(); }
  std::expected<std::size_t, queue_status> pipeline::try_pop_validate() { return v_queue_.try_pop(); }
  std::ptrdiff_t                           pipeline::c_queue_size_approx() const noexcept { return c_queue_.size_approx(); }
  std::ptrdiff_t                           pipeline::v_queue_size_approx() const noexcept { return v_queue_.size_approx(); }

  void pipeline::notify_cut_done()
  {
    if (docs_remaining_to_cut_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
      log_.info("All documents accounted for (cut or skipped) -- closing the shared ready_queue_.");
      seg_pool_.ready_queue_close();
    }
  }

  // Guarantees that at most (max_concurrent_cutters_) threads are ever inside a cut at once,
  // always leaving at least as many threads free to drain the pool via P. Without this, cutting
  // is fully synchronous per document: if every thread commits to C simultaneously, nobody is
  // left to free pool slots and Handler::endElement()'s acquire_slot() deadlocks permanently.
  bool pipeline::try_reserve_cutter_slot()
  {
    std::size_t cur = threads_cutting_.load(std::memory_order_relaxed);
    while (cur < max_concurrent_cutters_)
    {
      if (threads_cutting_.compare_exchange_weak(cur, cur + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
    }
    return false;
  }
  void pipeline::release_cutter_slot() noexcept { threads_cutting_.fetch_sub(1, std::memory_order_acq_rel); }
  void pipeline::record_doc_open(std::size_t doc_ndx) { (*doc_counters_)[doc_ndx].record_doc_open(); }

  void pipeline::record_doc_close(std::size_t doc_ndx, std::size_t segment_count, pipeline_hooks& hooks)
  {
    const bool completed = (*doc_counters_)[doc_ndx].record_doc_close(segment_count);
    if (completed) log_doc_done(doc_ndx);
    if (completed) maybe_finish_seg_processing(doc_ndx, hooks);
  }

  bool pipeline::check_segment_semantics(const xml_segment& segment, segment_result& result, pipeline_hooks& hooks)
  {
    const auto doc_ndx  = static_cast<std::size_t>(result.doc_ndx());
    auto&      counters = (*doc_counters_)[doc_ndx];
    const auto pos      = counters.begin_segment(result.seg_id());
    const bool ok       = hooks.on_seg_sem_safe_check(segment, ds_dscr_[doc_ndx], result, pos.is_first, pos.is_last);
    // error_class::he/te -- recorded here, not inside doc_status_t::set_semantic() (which only
    // ever sees the document-wide on_doc_sem_check() verdict, never an individual segment's own
    // outcome), since this is the one place that has BOTH the failing segment's own header/
    // non-header identity (segment.subtree_type(), looked up against cfg_.targets.is_header --
    // the same vector hdr_seg_schema compiles into, see parsing_util.hpp's own doc comment on
    // proc_data::is_header) AND its semantic verdict, at the same time. A document can accumulate
    // BOTH bits over its lifetime (e.g. one non-header segment already failed as TE before the
    // header segment itself later fails as HE too) -- see doc_status_t::mark_error()'s own doc
    // comment on why that's fine. no_headers=true only for HE (a header semantic failure -- see
    // on_remove_stored_data_safe()'s own doc comment on why TE alone never reaches here at all:
    // a single failed non-header segment does not, by itself, reject the whole document).
    if (! ok)
    {
      const bool is_header_segment = cfg_.targets.is_header[static_cast<std::size_t>(segment.subtree_type())]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- subtree_type() is always a valid index into is_header, set by doc_cutter at cut time
      // Not propagated as a fatal, run-stopping error (see report_fatal_error()) -- same "log and
      // move on" handling as on_block_safe_store()'s own error (see xml_worker::flush_ok_block()):
      // a rollback failing for one document's storage is that document's own problem, not a reason
      // to abort every other document still in flight.
      if (is_header_segment)
      {
        if (auto res = report_error_class(doc_ndx, error_class::he, true, hooks); ! res)
          log_.error(fmt::format("on_remove_stored_data() failed for doc {} (HE): {}", doc_ndx, res.error().to_string()));
      }
      else
      {
        // TE alone never triggers on_remove_stored_data_safe() -- see error_class::te's own doc
        // comment: a single failed non-header segment does not reject the whole document, so
        // there is nothing to roll back yet (mark_error() below still records the bit, for
        // status()-adjacent diagnostics, but intentionally does not go through
        // report_error_class(), which fires the rollback hook). Its own bool return (whether this
        // was the first error class ever recorded) is of no use here -- report_error_class() is
        // the one call site that acts on it.
        std::ignore = ds_dscr_[doc_ndx].mark_error(error_class::te);
      }
    }
    return ok;
  }

  void pipeline::finish_segment(std::size_t doc_ndx, bool semantically_ok, pipeline_hooks& hooks)
  {
    auto&      counters  = (*doc_counters_)[doc_ndx];
    const bool completed = counters.end_segment(semantically_ok);
    if (completed) log_doc_done(doc_ndx);
    if (completed) maybe_finish_seg_processing(doc_ndx, hooks);
  }

  void pipeline::record_segment_failed(std::size_t doc_ndx, std::size_t seg_id, pipeline_hooks& hooks)
  {
    auto& counters = (*doc_counters_)[doc_ndx];
    (void)counters.begin_segment(seg_id); // bookkeeping only (first_seg_ timing) -- no hook, no meaningful values
    const bool completed = counters.end_segment(false);
    if (completed) log_doc_done(doc_ndx);
    if (completed) maybe_finish_seg_processing(doc_ndx, hooks);
  }

  // Called once a document's segment processing is complete -- whichever of record_doc_close()/
  // record_segment_done()/record_segment_failed() turns out to be the one that satisfies the
  // last remaining condition (see doc_counters::maybe_seg_processing_complete()) is the one whose
  // call returns true and triggers this.
  void pipeline::log_doc_done(std::size_t doc_ndx)
  {
    const auto& c = (*doc_counters_)[doc_ndx];
    log_.debug(fmt::format("Doc {}: cut+process finished ({} segments, {} ms).", doc_ndx, c.total(), c.total_latency().count()));
  }

  // Only ever called once doc_counters::maybe_seg_processing_complete() has just been won by the
  // caller (record_doc_close()/record_segment_done()/record_segment_failed(), see their own doc
  // comments in pipeline.hpp) -- runs the doc-level semantic check exactly once and folds its
  // verdict into doc_status_t. Segment PROCESSING itself still runs to completion independent of
  // whether syntax/validation are already known (every segment, ok or rejected, must still be
  // accounted for -- see doc_counters' own "all segments processed" completion condition) -- but
  // the semantic CHECK ITSELF is skipped once the document is already known rejected
  // (doc_dscr::rejected() -- syntax/validation invalid, or a UA/SE/VE/HE error_class already
  // recorded, see report_error_class()): a hook's own on_doc_sem_check(doc_ndx) typically reads
  // doc-level data a cb staged while reading segments (e.g. a header segment's own row id, to
  // attach a finding to) that on_remove_stored_data() may already have rolled back by this point
  // for a rejected document -- calling the hook again here would either re-derive a verdict nobody
  // still cares about (the document is already rejected on some OTHER fact) or reference data that
  // is no longer there. semantic_ is folded in as three_state::valid instead of actually asking the
  // hook: the document's own aggregate status() is unaffected either way, since at least one other
  // fact is already three_state::invalid (that's what rejected() means) and status() only needs
  // ONE invalid fact to report invalid overall.
  void pipeline::maybe_finish_seg_processing(std::size_t doc_ndx, pipeline_hooks& hooks)
  {
    const bool semantic_ok = ds_dscr_[doc_ndx].rejected() || hooks.on_doc_safe_sem_check(doc_ndx);
    if (ds_dscr_[doc_ndx].set_semantic_result(semantic_ok)) finish_doc_close(doc_ndx, hooks);
    // doc_data(doc_ndx) is done being read from THIS side (on_doc_safe_sem_check() above already
    // returned, or was skipped) -- see mark_doc_data_reader_done()'s own doc comment on why
    // finish_doc_close() (called just above, possibly, or independently by report_syntax_result()/
    // report_validation_result()) is the OTHER reader that must also finish before recycling.
    mark_doc_data_reader_done(doc_ndx, hooks);
  }

  void pipeline::finish_doc_close(std::size_t doc_ndx, pipeline_hooks& hooks)
  {
    const auto& dscr    = ds_dscr_[doc_ndx];
    const auto& verdict = dscr.status();
    const bool  ok      = hooks.on_doc_safe_close(doc_ndx, verdict, dscr.error(), dscr);
    log_.debug(fmt::format("Doc {}: on_doc_close verdict={} (syntax={} validation={} semantic={}).",
                           doc_ndx,
                           ok,
                           static_cast<int>(verdict.syntax_status()),
                           static_cast<int>(verdict.valid_status()),
                           static_cast<int>(verdict.semantic_status())));
    // doc_data(doc_ndx) is done being read from THIS side too -- see mark_doc_data_reader_done()'s
    // own doc comment. on_doc_safe_finish() itself is dispatched from THERE, not here, once BOTH
    // readers are confirmed done -- see doc_status_t's own doc comment in doc_dscr.hpp on why a
    // single fact (e.g. validation failing) can get finish_doc_close() here well before segment
    // processing (and thus maybe_finish_seg_processing()) has even run.
    mark_doc_data_reader_done(doc_ndx, hooks);
  }

  void pipeline::assign_doc_data(std::size_t doc_ndx, pipeline_hooks& hooks)
  {
    const std::scoped_lock lock(doc_data_pool_mutex_);
    doc_data_root*         slot = nullptr;
    if (! doc_data_free_.empty())
    {
      slot = doc_data_free_.back();
      doc_data_free_.pop_back();
      slot->reset();
    }
    else
    {
      doc_data_storage_.push_back(hooks.make_doc_data_struct());
      slot = doc_data_storage_.back().get();
    }
    doc_data_active_[doc_ndx] = // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- doc_ndx always caller-bounded
      slot;
    doc_data_pending_readers_[doc_ndx] = // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      2;                                 // maybe_finish_seg_processing() and finish_doc_close() -- see mark_doc_data_reader_done()
    slot->timing().start();
  }

  void pipeline::mark_doc_data_reader_done(std::size_t doc_ndx, pipeline_hooks& hooks)
  {
    // Dispatches on_doc_safe_finish() (stopping doc_data(doc_ndx).timing() and calling
    // on_doc_finish()) OUTSIDE doc_data_pool_mutex_ -- a cb's own on_doc_finish() override may
    // itself take a while (or even, in principle, re-enter pipeline via another hook), and
    // nothing about the pool's own bookkeeping needs to stay locked while it runs. Both branches
    // below still read doc_data(doc_ndx) (via hooks.on_doc_safe_finish()) BEFORE the slot is
    // touched again -- fine, since it isn't returned to the free-list until after this call.
    bool last_reader = false;
    {
      const std::scoped_lock lock(doc_data_pool_mutex_);
      auto& remaining = doc_data_pending_readers_[doc_ndx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      assert(remaining > 0 && "pipeline::mark_doc_data_reader_done(doc_ndx) called more than twice for the same document");
      last_reader = (--remaining == 0);
    }
    if (! last_reader) return;
    // Errors are already logged by on_doc_safe_finish() itself -- see its own doc comment for why
    // this deliberately swallows rather than propagates.
    std::ignore = hooks.on_doc_safe_finish(doc_ndx);
    const std::scoped_lock lock(doc_data_pool_mutex_);
    auto&                  slot = doc_data_active_[doc_ndx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    doc_data_free_.push_back(slot);
    slot = nullptr;
  }

  e_void pipeline::report_error_class(std::size_t doc_ndx, error_class cls, bool no_headers, pipeline_hooks& hooks)
  {
    // no_headers==false is exactly "this class is UA/SE/VE" (see every call site in
    // pipeline_worker.cpp) -- the whole document is unusable, so rejected() must become true here,
    // BEFORE on_remove_stored_data_safe() below returns, not only later via the corresponding
    // report_syntax_result()/report_validation_result() call - see doc_dscr::mark_rejected()'s own
    // doc comment for the race this closes. HE (no_headers==true) deliberately does NOT call this
    // - a header semantic failure does not reject the whole document the way UA/SE/VE do (see
    // error_class::he's own doc comment) - so rejected() must stay whatever it already was.
    if (! no_headers) ds_dscr_[doc_ndx].mark_rejected();
    if (! ds_dscr_[doc_ndx].mark_error(cls)) return {}; // not the first error class recorded for this document -- already fired below
    return hooks.on_remove_stored_data_safe(ds_dscr_[doc_ndx].out_doc_id(), no_headers);
  }

  void pipeline::report_syntax_result(std::size_t doc_ndx, bool ok, pipeline_hooks& hooks, error_info err)
  {
    // C is the sole authority for both syntax AND validation ONLY when cut_with_validation_
    // folded V into its own SAX pass -- C never claims a validation verdict it didn't actually
    // perform. The "no XSD grammar supplied at all" case (!run_validation_ && !cut_with_validation_)
    // is handled differently: process_files() pre-seeds every document's valid_ to
    // three_state::valid on the MAIN thread, before any worker starts (see its own doc comment) --
    // C here still only ever reports syntax, never validation, even in that case.
    if (ds_dscr_[doc_ndx].set_syntax_result(ok, cut_with_validation_, std::move(err))) finish_doc_close(doc_ndx, hooks);
  }

  void pipeline::report_validation_result(std::size_t doc_ndx, bool ok, pipeline_hooks& hooks, error_info err)
  {
    if (ds_dscr_[doc_ndx].set_validation_result(ok, std::move(err))) finish_doc_close(doc_ndx, hooks);
  }

  void pipeline::record_segments_stored(std::size_t doc_ndx, std::size_t count, pipeline_hooks& hooks)
  {
    if (! (*doc_counters_)[doc_ndx].add_segments_stored(count)) return;
    // This call is the one whose running total crossed doc_ndx's known segment count -- fire
    // on_doc_safe_stored() exactly once for this document, fold its result into
    // doc_status_t::set_stored_result() (always three_state::valid, see its own doc comment), and
    // dispatch on_doc_safe_close() if THAT wins try_start_closing() -- same shape as
    // maybe_finish_seg_processing()'s on_doc_safe_sem_check()/set_semantic_result() pair above.
    // Errors from on_doc_safe_stored() are already logged by the hook itself -- see its own doc
    // comment in pipeline_hooks.hpp for why this deliberately swallows rather than propagates.
    std::ignore = hooks.on_doc_safe_stored(doc_ndx, ds_dscr_[doc_ndx]);
    if (ds_dscr_[doc_ndx].set_stored_result()) finish_doc_close(doc_ndx, hooks);
  }

  void pipeline::report_fatal_error(error_info err)
  {
    std::lock_guard lock(first_error_mutex_);
    if (! first_error_) first_error_ = std::move(err);
  }

  std::vector<std::size_t> pipeline::failed_document_indices() const
  {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < ds_dscr_.size(); ++i)
      if (ds_dscr_[i].failed()) out.push_back(i);
    return out;
  }

  e_void pipeline::add_documents(const std::vector<str_t>& xml_paths, cstr_t xsd_path, pipeline_hooks& hooks)
  {
    // doc_data_active_/doc_data_pending_readers_ are sized (not reserved) here so
    // assign_doc_data() can index them directly -- every slot starts nullptr/0 (not yet assigned,
    // see doc_data()'s own precondition assert), filled in lazily by assign_doc_data() right
    // before each document is actually cut, not here.
    doc_data_active_.assign(xml_paths.size(), nullptr);
    doc_data_pending_readers_.assign(xml_paths.size(), 0);
    for (std::size_t doc_ndx = 0; doc_ndx < xml_paths.size(); ++doc_ndx)
    {
      const auto& file = xml_paths[doc_ndx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- doc_ndx <
                                             // xml_paths.size() by the loop condition
      try
      {
        // Built here, not via doc_set_dscr::add_document(cstr_t), so out_doc_id()/agent_id() can be
        // filled in BEFORE the doc_dscr is handed to doc_set_dscr -- see get_doc_id()'s/
        // get_doc_agent_id()'s own doc comments on why this must happen strictly before any worker
        // thread starts.
        doc_dscr doc(file);
        doc.set_out_doc_id(hooks.get_doc_id(doc_ndx % doc_id_node_hint_modulo));
        doc.set_agent_id(hooks.get_doc_agent_id(file));
        if (! ds_dscr_.add_document(std::move(doc)))
          return std::unexpected(error_info{processor_error::file_open_failed, fmt::format("Failed to add document: '{}'", file), file, 0});
      }
      catch (const std::exception& e)
      {
        return std::unexpected(
          error_info{processor_error::file_open_failed, fmt::format("Failed to open document '{}': {}", file, e.what()), file, 0});
      }
    }
    ds_dscr_.set_grammar(xsd_path);
    return {};
  }

  pipeline::run_plan pipeline::plan_run(std::size_t doc_count)
  {
    // Whether C folds XSD validation into its own SAX pass instead of running it separately as
    // V (see doc_cutter.cpp / importer_config.hpp for the measured break-even). cfg_.cut_with_validation
    // lets a caller force either mode; left unset, default to the empirically-best choice for the
    // actual document count: separate for a single document, merged from 2 documents up. This
    // must match doc_cutter::init()'s own computation of the same condition exactly, since both
    // decide independently from the same (ds_dscr_.has_grammar(), doc count) inputs.
    const bool cut_with_validation = ds_dscr_.has_grammar() && cfg_.cut_with_validation.value_or(doc_count > 1);

    // Skip V entirely when no XSD grammar was successfully loaded -- has_grammar() also covers
    // the case where a path was given but loading it failed (set_grammar() swallows that and
    // returns false without setting anything). Also skip it when cut_with_validation folded
    // validation into C's own SAX pass instead -- running V separately too would just validate
    // the same document twice.
    const bool run_validation = ds_dscr_.has_grammar() && ! cut_with_validation;
    if (! ds_dscr_.has_grammar()) log_.info("No XSD grammar available -- V (validation) is disabled for this run.");
    else if (cut_with_validation) log_.info("cut_with_validation active -- C validates while cutting, separate V pass skipped.");

    auto requested_threads = cfg_.num_of_workers;
    if (requested_threads == 0) requested_threads = std::thread::hardware_concurrency();
    if (requested_threads == 0) requested_threads = 1;

    auto hw_concurrency = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (hw_concurrency == 0) hw_concurrency = 1;

    // At most one cutter can ever usefully work per document (try_reserve_cutter_slot() would
    // just leave surplus cutter slots permanently unused past doc_count), and cutting more
    // documents at once than there are hardware threads just oversubscribes the CPU -- so C is
    // capped by the smallest of (caller's requested thread count, hardware concurrency, doc
    // count), not sized as a fixed fraction of the thread budget. This matters most for small
    // batches: for a single document, 15 idle-prone extra P threads competing over one cutter's
    // trickle of segments cost us a measured ~25-35% wall-time regression (see bisection of
    // commit a838163) for zero throughput gain.
    max_concurrent_cutters_ = std::max<std::size_t>(1, std::min({requested_threads, hw_concurrency, doc_count}));

    // P is then sized off the caller-configured C:P ratio (cfg_.cutter_ratio_num/_den, default
    // 13:6, empirically the fastest of 13:5/13:6/13:7/13:8 tested on a 10-doc/10M-txn batch),
    // scaled to the actual number of cutters rather than the raw thread budget, so a small
    // document batch doesn't oversupply P threads relative to the segments its few cutters can
    // produce. Revisit once P grows to include real business-logic and DB-write cost (that
    // should get its own, separately-sized I/O thread pool instead of being folded into this
    // ratio).
    const auto cutter_ratio_num = std::max<std::size_t>(1, cfg_.cutter_ratio_num);
    const auto num_processors   = std::max<std::size_t>(1, (max_concurrent_cutters_ * cfg_.cutter_ratio_den) / cutter_ratio_num);

    const auto num_parallel = std::min(requested_threads, max_concurrent_cutters_ + num_processors);
    return {.run_validation = run_validation, .cut_with_validation = cut_with_validation, .num_parallel = num_parallel};
  }

  void pipeline::seed_queues(std::size_t doc_count, bool run_validation)
  {
    // c_queue_ and v_queue_: both known, final quantities up front -> set_finished() immediately.
    // v_queue_ stays empty when validation is disabled -- pipeline_worker's size_approx() check
    // then naturally never selects the V role, no changes needed anywhere else.
    for (std::size_t i = 0; i < doc_count; ++i)
    {
      c_queue_.push(i);
      if (run_validation) v_queue_.push(i);
    }
    c_queue_.set_finished();
    v_queue_.set_finished();
  }

  result<std::vector<std::unique_ptr<pipeline_worker>>> pipeline::start_workers(std::size_t num_parallel, pipeline_hooks& hooks)
  {
    std::vector<std::unique_ptr<pipeline_worker>> worker_state;
    worker_state.reserve(num_parallel);
    for (std::size_t i = 0; i < num_parallel; ++i)
    {
      auto w = std::make_unique<pipeline_worker>(*this, cfg_, log_, parent_log_name_, hooks);
      if (auto init_res = w->init(); ! init_res)
      {
        log_.error(fmt::format("pipeline_worker {} init failed: {}", i, init_res.error().to_string()));
        return std::unexpected(init_res.error()); // infra-level failure, treated as fatal for the whole run
      }
      worker_state.push_back(std::move(w));
    }
    return worker_state;
  }

  void pipeline::run_workers(std::vector<std::unique_ptr<pipeline_worker>>& worker_state)
  {
    std::vector<std::jthread> threads;
    threads.reserve(worker_state.size());
    for (std::size_t i = 0; i < worker_state.size(); ++i) threads.emplace_back(std::ref(*worker_state[i]), static_cast<int>(i));
    for (auto& t : threads)
      if (t.joinable()) t.join();
  }

  str_t pipeline::build_summary(std::size_t doc_count, double elapsed_ms, std::size_t failed_count) const
  {
    const auto pool_capacity = seg_pool_.size();
    const auto pool_peak     = seg_pool_.high_water_mark();
    const auto pool_pct      = pool_capacity > 0 ? (static_cast<double>(pool_peak) / static_cast<double>(pool_capacity)) * 100.0
                                                 : 0.0; // NOLINT(readability-magic-numbers)
    // segments ok/err counts come from doc_counters_ (atomic bookkeeping updated as each segment
    // is processed, see doc_set_counter.hpp), not from a results_/errors_ accumulator -- fsp-core
    // never materializes a second copy of every segment's extracted values for the whole run; a
    // caller consumes each segment through its own hooks (on_type()/on_block_store()/...) as it is
    // processed instead (see docs/importer_usage.md).
    auto       msg           = fmt::format(R"(
  Processed {0} docs in {1:.3f} sec (segments ok:{2} err:{3}, docs failed:{4}) seg. peak: {5} / {6} slots ({7:.2f}%))",
                                           doc_count,
                                           elapsed_ms / 1000.0, // NOLINT(readability-magic-numbers)
                                           doc_counters_->total_segments_ok(ds_dscr_),
                                           doc_counters_->total_segments_error(ds_dscr_),
                                           failed_count,
                                           pool_peak,
                                           pool_capacity,
                                           pool_pct);
    msg += fmt::format("\ndocument statistics:\n{}", doc_counters_->dump(2)); // NOLINT(readability-magic-numbers)
    return msg;
  }

  result<doc_set_counter> pipeline::process_files(const std::vector<str_t>& xml_paths, cstr_t xsd_path, pipeline_hooks& hooks)
  {
    // run_data_ is constructed here, before on_run_safe_start() in EITHER branch below, and
    // destroyed when process_files() returns (run_data_.reset() at every return point) -- see
    // run_data()'s own doc comment in pipeline.hpp.
    run_data_ = hooks.make_run_data_struct();
    run_data_->timing().start();
    if (xml_paths.empty())
    {
      log_.info("No files to process.");
      if (auto started = hooks.on_run_safe_start(*this, ds_dscr_, log_); ! started)
      {
        run_data_.reset();
        return std::unexpected(started.error());
      }
      run_data_->timing().end();
      // Errors are already logged by on_run_safe_end() itself -- see its own doc comment for why
      // this deliberately swallows rather than propagates.
      std::ignore = hooks.on_run_safe_end(doc_set_counter(0), ds_dscr_, {});
      run_data_.reset();
      return doc_set_counter(0);
    }

    if (auto added = add_documents(xml_paths, xsd_path, hooks); ! added)
    {
      run_data_.reset();
      return std::unexpected(added.error());
    }
    if (auto started = hooks.on_run_safe_start(*this, ds_dscr_, log_); ! started)
    {
      run_data_.reset();
      return std::unexpected(started.error());
    }

    const auto doc_count = xml_paths.size();
    const auto plan      = plan_run(doc_count);
    cut_with_validation_ = plan.cut_with_validation; // read (never rewritten) by every worker thread from here on
    run_validation_      = plan.run_validation;      // ditto -- see pipeline.hpp's own doc comment on these two flags

    // No XSD grammar was supplied at all (has_grammar()==false, distinct from cut_with_validation
    // folding V into C's own pass) -- neither C nor a separate V will EVER report a validation
    // verdict for any document this run, so nothing would ever call doc_status_t::set_valid() and
    // is_finished()/on_doc_close() would hang forever. Pre-seed every document's valid_ to
    // three_state::valid right here, on the MAIN thread, strictly before seed_queues()/
    // start_workers() below -- mirrors the run-wide validation_done_ pre-seed this design used
    // before doc_status_t existed: a run-wide fact known before any worker starts is decided here,
    // not faked by a worker role (C) that never actually validated anything (round 6 of the design
    // discussion this implements -- rejected letting C itself call set_valid() in this case).
    if (! run_validation_ && ! cut_with_validation_)
      for (std::size_t i = 0; i < doc_count; ++i) (void)ds_dscr_[i].status().set_valid(true);

    docs_remaining_to_cut_.store(doc_count, std::memory_order_relaxed);
    doc_counters_.emplace(doc_count);
    seed_queues(doc_count, plan.run_validation);

    log_.info(fmt::format("Pipeline: {} documents, {} hybrid worker threads (max {} cutting concurrently).",
                          doc_count,
                          plan.num_parallel,
                          max_concurrent_cutters_));

    auto worker_state = start_workers(plan.num_parallel, hooks);
    if (! worker_state)
    {
      run_data_.reset();
      return std::unexpected(worker_state.error());
    }

    run_workers(*worker_state);

    // Always fires exactly once, paired with on_run_safe_start() above, regardless of whether the
    // run goes on to succeed or hit a fatal error below -- so the developer always sees the
    // final counters/ds_dscr/worker clones for whatever actually happened. run_data_'s timing is
    // stopped right before, so duration() is already frozen and readable inside on_run_end().
    std::vector<const pipeline_hooks*> worker_clones;
    worker_clones.reserve(worker_state->size());
    for (const auto& w : *worker_state) worker_clones.push_back(&w->hooks());
    run_data_->timing().end();
    // Errors are already logged by on_run_safe_end() itself -- see its own doc comment for why
    // this deliberately swallows rather than propagates.
    std::ignore = hooks.on_run_safe_end(*doc_counters_, ds_dscr_, worker_clones);
    run_data_.reset();

    if (first_error_)
    {
      log_.error(fmt::format("Pipeline failed with a fatal error: {}", first_error_->to_string()));
      return std::unexpected(*first_error_);
    }

    auto failed = failed_document_indices();
    if (! failed.empty()) log_.warn(fmt::format("{} of {} document(s) failed and were skipped.", failed.size(), doc_count));

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();

    log_.info(build_summary(doc_count, static_cast<double>(ms), failed.size())); // NOLINT(readability-magic-numbers)
    return std::move(*doc_counters_);
  }
} // namespace fsp