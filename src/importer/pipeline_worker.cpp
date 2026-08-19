#include "pipeline_worker.hpp"
#include "pipeline.hpp"
#include <fmt/format.h>

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

  void_result pipeline_worker::init() { return cutter_->init(); }

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
      pipeline_.report_syntax_result(
        doc_ndx,
        false,
        *hooks_,
        error_info{processor_error::unknown, "agent_id()==0 (unresolved agent)", pipeline_.ds_dscr()[doc_ndx].path(), 0});
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
    hooks_->on_doc_safe_open(doc_ndx, pipeline_.ds_dscr()[doc_ndx]);
    auto res = cutter_->cut(doc_ndx);
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
    hooks_->on_doc_safe_cutting_finished(doc_ndx, pipeline_.ds_dscr()[doc_ndx]);
    pipeline_.notify_cut_done();
  }

  void pipeline_worker::do_validate(std::size_t doc_ndx)
  {
    // Same agent_id()==0 rejection as do_cut() above, and for the same reason -- but here mostly
    // a defensive no-op rather than the primary rejection path: do_cut()'s own report_syntax_result
    // (false) already drags BOTH syntax and validation invalid for this document (see
    // doc_dscr::set_syntax_result()'s own doc comment), and C always runs ahead of/in parallel
    // with V (see pipeline_worker::operator()()'s own priority comment) -- but C/V order relative
    // to each other is NOT guaranteed for any single document, so V can still reach this doc_ndx
    // first. Bailing out here too, rather than relying on failed() alone, keeps this check as
    // cheap/obvious as do_cut()'s own and avoids a redundant validator_->validate() call either way.
    if (pipeline_.ds_dscr()[doc_ndx].agent_id() == 0)
    {
      log_.warn(fmt::format("Doc {}: agent_id()==0 (unresolved agent) -- validation skipped.", doc_ndx));
      pipeline_.report_validation_result(
        doc_ndx,
        false,
        *hooks_,
        error_info{processor_error::unknown, "agent_id()==0 (unresolved agent)", pipeline_.ds_dscr()[doc_ndx].path(), 0});
      return;
    }
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
      const auto err_code = validator_->last_error_source() == sax_error_source::well_formed ? processor_error::parse_failed
                                                                                             : processor_error::xsd_validation_failed;
      pipeline_.report_validation_result(doc_ndx, false, *hooks_, error_info{err_code, validator_->last_error_message(), "", 0});
    }
  }

  void pipeline_worker::operator()(const std::stop_token& st, int worker_id)
  {
    logger::Logger::make_log_name(parent_log_name_, fmt::format("pipe-wrk.{:02}", worker_id));
    const auto thread_name = logger::Logger::log_name();
    hooks_->on_wrk_safe_start(pipeline_, worker_id, thread_name, log_);
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
      //    Own shard first, then a non-blocking sweep of the others if it's currently empty.
      bool processed = false;
      for (std::size_t i = 0; i < num_shards && ! processed; ++i)
      {
        const std::size_t shard = (own_shard + i) % num_shards;
        if (pool.ready_queue_size_approx(shard) <= 0) continue;
        if (auto seg_ndx = pool.try_pop_ready(shard))
        {
          processor_->process_one(*seg_ndx); // records the segment's outcome (and runs the hook) internally
          processed = true;
        }
      }
      if (processed) continue;

      // Neither C nor V currently show any work, and no shard had anything ready -> authoritative,
      // blocking wait on this thread's own shard, the only queue guaranteed to still dynamically
      // receive work from OTHER threads relevant to it. This is also the real exit condition for
      // this thread once its own shard closes.
      std::size_t seg_ndx = 0;
      if (pool.pop_segment_ndx(own_shard, seg_ndx) != queue_status::active) break;
      processor_->process_one(seg_ndx); // records the segment's outcome (and runs the hook) internally
    }
    processor_->flush_results();
    hooks_->on_wrk_safe_end(worker_id, thread_name);
  }
} // namespace fsp