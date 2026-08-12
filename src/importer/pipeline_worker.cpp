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
    // Requirement: a document already known to be invalid is never cut at all.
    if (pipeline_.ds_dscr()[doc_ndx].status() == doc_status::validation_failed)
    {
      if (log_info_) log_.info(fmt::format("Doc {}: already invalid -- cut skipped.", doc_ndx));
      pipeline_.notify_cut_done();
      return;
    }
    pipeline_.record_doc_open(doc_ndx);
    hooks_->on_doc_open(doc_ndx, pipeline_.ds_dscr()[doc_ndx]);
    if (auto res = cutter_->cut(doc_ndx); ! res)
    {
      // A malformed document is a per-document failure, not a fatal one: mark it invalid so
      // any already-cut segments of this document get discarded by P, then keep going.
      pipeline_.report_validation_result(doc_ndx, doc_status::validation_failed, res.error());
    }
    else
    {
      pipeline_.record_doc_close(doc_ndx, cutter_->segments_found());
    }
    hooks_->on_doc_close(doc_ndx, pipeline_.ds_dscr()[doc_ndx].status(), pipeline_.ds_dscr()[doc_ndx]);
    pipeline_.notify_cut_done();
  }

  void pipeline_worker::do_validate(std::size_t doc_ndx)
  {
    auto res = validator_->validate(doc_ndx);
    if (! res)
    {
      // The XSD itself failed to load/compile -- systemic, not specific to this one document.
      pipeline_.report_fatal_error(res.error());
      return;
    }
    if (*res) pipeline_.report_validation_result(doc_ndx, doc_status::validation_ok);
    else
      pipeline_.report_validation_result(doc_ndx,
                                         doc_status::validation_failed,
                                         error_info{processor_error::xsd_validation_failed, validator_->last_error_message(), "", 0});
  }

  void pipeline_worker::operator()(const std::stop_token& st, int worker_id)
  {
    logger::Logger::make_log_name(parent_log_name_, fmt::format("pipe-wrk.{:02}", worker_id));
    const auto thread_name = logger::Logger::log_name();
    hooks_->on_wrk_start(worker_id, thread_name, log_);
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
    hooks_->on_wrk_end(worker_id, thread_name);
  }
} // namespace fsp