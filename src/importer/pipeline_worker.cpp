#include "pipeline_worker.hpp"
#include "pipeline.hpp"
#include <fmt/format.h>
#include <thread>
#include <tuple> // std::ignore -- see the on_*_safe_*() call sites below

namespace fsp
{
  pipeline_worker::pipeline_worker(pipeline&              pl,
                                   const importer_config& cfg,
                                   const logger::Logger&  log,
                                   str_t                  parent_log_name,
                                   pipeline_hooks&        hooks)
  : pipeline_(pl)
  , log_(log)
  , parent_log_name_(std::move(parent_log_name))
  , hooks_(hooks.clone())
  {
    cutter_    = std::make_unique<doc_cutter>(cfg, log_, pipeline_.pool(), pipeline_.ds_dscr());
    processor_ = std::make_unique<xml_worker>(pipeline_.pool(),
                                              pipeline_.ds_dscr(),
                                              pipeline_.results(),
                                              pipeline_.errors(),
                                              pipeline_.results_mutex(),
                                              pipeline_.errors_mutex(),
                                              log_,
                                              cfg.targets,
                                              parent_log_name_,
                                              pipeline_,
                                              *hooks_,
                                              cfg.ok_block_flush_size,
                                              cfg.nak_block_flush_size);
    validator_ = std::make_unique<doc_validator>(log_, pipeline_.ds_dscr());
  }

  e_void pipeline_worker::init() { return cutter_->init(); }

  void pipeline_worker::do_cut(std::size_t doc_ndx)
  {
    // Assigns doc_ndx a doc-level shared-data instance (recycled if one is free, otherwise
    // freshly made) -- unconditionally, before the failed() bailout below, because even a
    // document whose cut is skipped here can still reach on_doc_safe_sem_check() (its
    // already-queued segments still flow through xml_worker::process_one(), which
    // unconditionally calls doc_data(doc_ndx) via maybe_finish_seg_processing() once "all
    // segments processed" fires -- see pipeline.hpp's own doc comment on that cascade). Every
    // doc_ndx reaches do_cut() exactly once (see pipeline::seed_queues()), so this is still
    // exactly one assign_doc_data() call per document, same recycling guarantee as before.
    pipeline_.assign_doc_data(doc_ndx, *hooks_);
    // A hook's own get_doc_agent_id() (see pipeline::add_documents(), called on the main thread
    // before any worker starts) may resolve to 0 rather than a real id -- an opaque, hook-chosen
    // convention for "no agent recognized for this document" (fsp itself never interprets
    // agent_id(), see doc_dscr::agent_id()'s own doc comment; ach's own ach_hook uses 0 this way).
    // Rejected here, before any cut work, the same way an already-known-invalid document is
    // rejected below -- report_syntax_result(false) both marks the document invalid AND (if this
    // call wins try_start_closing()) dispatches hooks.on_doc_close() itself, so a hook still gets
    // exactly one terminal callback for such a document, same as any other syntax failure.
    if (pipeline_.ds_dscr()[doc_ndx].agent_id() == 0)
    {
      log_.warn(fmt::format("Doc {}: agent_id()==0 (unresolved agent) -- cut skipped.", doc_ndx));
      // Recorded explicitly here, as error_class::ua, rather than left for report_syntax_result()
      // below to infer from ok=false -- report_syntax_result() has no way to tell a UA rejection
      // apart from a genuine SE (syntax) one, and the two are deliberately distinct error classes
      // (see docs/importer_usage.md's own "Document errors" section). no_headers=false: an
      // unresolved agent invalidates the whole document, header included.
      if (auto res = pipeline_.report_error_class(doc_ndx, error_class::ua, false, *hooks_); ! res)
        log_.error(fmt::format("on_remove_stored_data() failed for doc {} (UA): {}", doc_ndx, res.error().to_string()));
      pipeline_.report_syntax_result(
        doc_ndx,
        false,
        *hooks_,
        error_info{processor_error::unknown, "agent_id()==0 (unresolved agent) -- cut skipped", pipeline_.ds_dscr()[doc_ndx].path(), 0});
      pipeline_.notify_cut_done();
      return;
    }
    // Requirement: a document already known to be invalid is never cut at all. doc_dscr::failed()
    // (not status().status(), which is ALSO three_state::unknown -- not three_state::invalid --
    // for a document nothing has reported on yet) is the correct "known bad" predicate here.
    if (pipeline_.ds_dscr()[doc_ndx].failed())
    {
      if (log_info_) log_.info(fmt::format("Doc {}: already invalid -- cut skipped.", doc_ndx));
      pipeline_.notify_cut_done();
      return;
    }
    pipeline_.record_doc_open(doc_ndx);
    // Errors are already logged by on_doc_safe_open() itself before returning -- see its own doc
    // comment for why this deliberately swallows (rather than propagates) a failing on_doc_open().
    std::ignore = hooks_->on_doc_safe_open(doc_ndx, pipeline_.ds_dscr()[doc_ndx]);
    // Published AFTER on_doc_safe_open() has returned, BEFORE any further work on this document -
    // do_validate() (below) checks this before validating, so a V pass racing ahead of this C
    // pass on another thread never reports a result (and possibly wins try_start_closing(),
    // dispatching on_doc_close()) for a document whose own on_doc_open() hasn't run yet. See
    // doc_dscr::mark_opened()'s own doc comment for the full race this closes.
    pipeline_.ds_dscr()[doc_ndx].mark_opened();
    auto res = cutter_->cut(doc_ndx);
    // error_class::se, recorded explicitly here rather than inferred inside report_syntax_result()
    // -- see the agent_id()==0 branch above for why that call alone can't tell a UA rejection
    // apart from a genuine syntax (SE) one. no_headers=false: an ill-formed document invalidates
    // everything cut from it so far, header included.
    if (! res.has_value())
      if (auto rm_res = pipeline_.report_error_class(doc_ndx, error_class::se, false, *hooks_); ! rm_res)
        log_.error(fmt::format("on_remove_stored_data() failed for doc {} (SE): {}", doc_ndx, rm_res.error().to_string()));
    // A malformed document is a per-document failure, not a fatal one: mark it invalid so any
    // already-cut segments of this document get discarded by P, then keep going. May itself
    // dispatch hooks.on_doc_safe_close() right here, if this call wins doc_status_t::try_start_closing().
    pipeline_.report_syntax_result(doc_ndx, res.has_value(), *hooks_, res.has_value() ? error_info{} : res.error());
    // Still record_doc_close() with whatever segments WERE cut before a failure (Handler's own
    // counter_ isn't reset on a mid-cut failure, see doc_cutter.hpp) -- those segments are already
    // pushed to the pool and will be discarded by xml_worker::process_one() (doc_dscr::failed() now
    // true), but the "all segments processed" completion (on_doc_sem_check(), and via it possibly
    // on_doc_close()) still needs cut_finished_ to become true for THIS document, or that hook
    // would never fire for a syntactically-invalid document.
    pipeline_.record_doc_close(doc_ndx, cutter_->segments_found(), *hooks_);
    // Errors are already logged by on_doc_safe_cutting_end() itself -- see on_doc_safe_open()'s own
    // doc comment above for why this deliberately swallows rather than propagates.
    std::ignore = hooks_->on_doc_safe_cutting_end(doc_ndx, pipeline_.ds_dscr()[doc_ndx]);
    pipeline_.notify_cut_done();
  }

  void pipeline_worker::do_validate(std::size_t doc_ndx)
  {
    // Same agent_id()==0 rejection as do_cut() above and for the same reason, but a plain, silent
    // bailout here rather than do_cut()'s own report_syntax_result(false) call: do_cut() is the
    // ONLY caller of pipeline::assign_doc_data() (see its own doc comment - "every doc_ndx reaches
    // do_cut() exactly once, so this is still exactly one assign_doc_data() call per document"),
    // and calling report_validation_result(false) here (winning try_start_closing()) would
    // dispatch finish_doc_close() -> mark_doc_data_reader_done() before assign_doc_data() has
    // necessarily run for this doc_ndx at all - an underflow of doc_data_pending_readers_, not a
    // race that only shows up occasionally. do_cut() itself still rejects the SAME document (its
    // own agent_id()==0 bailout above), so on_doc_close() still fires exactly once for it, same
    // terminal-callback guarantee as any other rejection - just always via C, never via V.
    if (pipeline_.ds_dscr()[doc_ndx].agent_id() == 0)
    {
      if (log_info_) log_.info(fmt::format("Doc {}: agent_id()==0 (unresolved agent) -- validation skipped.", doc_ndx));
      return;
    }
    // C and V are seeded into their own queues independently (pipeline::seed_queues()) and can run
    // on different threads with no ordering between them - a fast-failing V pass could otherwise
    // report a result (and possibly win doc_status_t::try_start_closing(), dispatching
    // hooks.on_doc_safe_close()) for a document whose own on_doc_safe_open() hasn't even started
    // yet on the C thread - see doc_dscr::mark_opened()'s own doc comment for the full race this
    // closes. The window is normally sub-millisecond (on_doc_open() doing real work, e.g. a
    // database insert, is the only thing this can ever wait on) - a short spin-yield loop, not a
    // queue requeue (lock_queue<T> has no plain push(), only push_range() at seed time), keeps this
    // fix local and avoids re-plumbing the C/V queue relationship.
    while (! pipeline_.ds_dscr()[doc_ndx].is_opened()) std::this_thread::yield();
    auto res = validator_->validate(doc_ndx);
    if (! res)
    {
      // The XSD itself failed to load/compile -- systemic, not specific to this one document.
      pipeline_.report_fatal_error(res.error());
      return;
    }
    if (*res) { pipeline_.report_validation_result(doc_ndx, true, *hooks_); }
    else
    {
      // Both Handler::error()/fatalError() and validation_error_handler::error()/fatalError()
      // throw the SAME xercesc::SAXParseException type -- last_error_source() (set by WHICH
      // callback actually fired) is what distinguishes a schema violation from a well-formedness
      // one here, see point 15/16 of the design discussion this implements.
      const bool is_well_formed_error = validator_->last_error_source() == sax_error_source::well_formed;
      const auto err_code             = is_well_formed_error ? processor_error::parse_failed : processor_error::xsd_validation_failed;
      // error_class::se for a well-formedness failure (V caught what is really a syntax problem,
      // not a schema one), error_class::ve for a genuine XSD violation -- same distinction
      // err_code itself already makes, recorded explicitly for the same reason as the two call
      // sites above (report_validation_result() alone can't infer which class this is).
      // no_headers=false either way: both invalidate the whole document, header included.
      if (auto rm_res = pipeline_.report_error_class(doc_ndx, is_well_formed_error ? error_class::se : error_class::ve, false, *hooks_);
          ! rm_res)
        log_.error(fmt::format("on_remove_stored_data() failed for doc {} ({}): {}",
                               doc_ndx,
                               is_well_formed_error ? "SE" : "VE",
                               rm_res.error().to_string()));
      pipeline_.report_validation_result(doc_ndx, false, *hooks_, error_info{err_code, validator_->last_error_message(), "", 0});
    }
  }

  // Header segments (see importer_config::header_seg_types) win within P itself -- own shard
  // first, then a non-blocking sweep of the OTHER header shards, all BEFORE this thread ever
  // looks at an ordinary ready queue. Checked on every single call (i.e. every work-fetch, not
  // just at thread start-up), so a header segment can never be starved behind an unbounded pile
  // of ordinary ones (see docs/importer_usage.md's own "Header segments are processed first"
  // section). A no-op sweep (empty queues) when header_seg_types was never set.
  bool pipeline_worker::try_process_ready_segment(std::size_t own_shard, std::size_t num_shards)
  {
    auto& pool = pipeline_.pool();
    for (std::size_t i = 0; i < num_shards; ++i)
    {
      const std::size_t shard = (own_shard + i) % num_shards;
      if (pool.ready_queue_size_approx_header(shard) <= 0) continue;
      if (auto seg_ndx = pool.try_pop_ready_header(shard))
      {
        processor_->process_one(*seg_ndx); // records the segment's outcome (and runs the hook) internally
        return true;
      }
    }
    // Own shard first, then a non-blocking sweep of the others if it's currently empty.
    for (std::size_t i = 0; i < num_shards; ++i)
    {
      const std::size_t shard = (own_shard + i) % num_shards;
      if (pool.ready_queue_size_approx(shard) <= 0) continue;
      if (auto seg_ndx = pool.try_pop_ready(shard))
      {
        processor_->process_one(*seg_ndx); // records the segment's outcome (and runs the hook) internally
        return true;
      }
    }
    return false;
  }

  void pipeline_worker::operator()(const std::stop_token& st, int worker_id)
  {
    logger::Logger::make_log_name(parent_log_name_, fmt::format("pipe-wrk.{:02}", worker_id));
    const auto thread_name = logger::Logger::log_name();
    // Errors are already logged by on_wrk_safe_start() itself -- see its own doc comment for why
    // this deliberately swallows (rather than propagates/aborts) a failing on_wrk_start().
    std::ignore                  = hooks_->on_wrk_safe_start(pipeline_, worker_id, thread_name, log_);
    auto&             pool       = pipeline_.pool();
    const std::size_t num_shards = pool.num_shards();
    // Each P-capable thread is permanently assigned one shard (worker_id % num_shards) of the
    // segment_pool's ready/free queues -- this is the shard it tries FIRST and the one it
    // blocks on when idle, so most of the time it only ever contends with the handful of other
    // threads sharing that same shard, not all P threads at once. Other shards are only visited
    // as a non-blocking fallback so an imbalanced shard never starves a thread outright.
    const std::size_t own_shard = static_cast<std::size_t>(worker_id) % num_shards;

    while (! st.stop_requested())
    {
      // 1) highest priority: start as many C as possible right away -- but never let more than
      //    max_concurrent_cutters_ threads commit to cutting at once (see pipeline::try_reserve_cutter_slot()).
      if (pipeline_.c_queue_size_approx() > 0 && pipeline_.try_reserve_cutter_slot())
      {
        if (auto doc_ndx = pipeline_.try_pop_cut())
        {
          do_cut(*doc_ndx);
          pipeline_.release_cutter_slot();
          continue;
        }
        pipeline_.release_cutter_slot(); // reserved but nothing left to pop -- release immediately
      }

      // 2) V runs in parallel with C, picked whenever this thread currently has no C work
      if (pipeline_.v_queue_size_approx() > 0)
        if (auto doc_ndx = pipeline_.try_pop_validate())
        {
          do_validate(*doc_ndx);
          continue;
        }

      // 3) P is deliberately lowest priority: as few P as possible early on, so V gets a
      //    chance to flag invalid documents before we spend effort processing their segments.
      //    See try_process_ready_segment()'s own doc comment for why header segments win within
      //    P itself (importer_config::header_seg_types).
      if (try_process_ready_segment(own_shard, num_shards)) continue;

      // Neither C nor V currently show any work, and no shard had anything ready (header or
      // ordinary) -> authoritative, blocking wait -- this thread's own HEADER shard first (same
      // priority as try_process_ready_segment()'s own sweep), falling through to its own ordinary
      // shard only once the header queue itself has closed (see segment_pool::
      // ready_queue_close(), which closes both together). This is also the real exit condition
      // for this thread once its own shard closes.
      std::size_t seg_ndx = 0;
      if (pool.pop_segment_ndx_header(own_shard, seg_ndx) == queue_status::active)
      {
        processor_->process_one(seg_ndx); // records the segment's outcome (and runs the hook) internally
        continue;
      }
      if (pool.pop_segment_ndx(own_shard, seg_ndx) != queue_status::active) break;
      processor_->process_one(seg_ndx); // records the segment's outcome (and runs the hook) internally
    }
    processor_->flush_results();
    // Errors are already logged by on_wrk_safe_end() itself -- see on_wrk_safe_start()'s own doc
    // comment for why this deliberately swallows rather than propagates.
    std::ignore = hooks_->on_wrk_safe_end(worker_id, thread_name);
  }
} // namespace fsp