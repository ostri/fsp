#include "pipeline_worker.hpp"
#include "pipeline.hpp"
#include <fmt/format.h>

namespace fsp
{
  pipeline_worker::pipeline_worker(pipeline& pl, const processor_config& cfg, const fsp_logger& log, str_t parent_log_name)
  : pipeline_(pl)
  , log_(log)
  , parent_log_name_(std::move(parent_log_name))
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
                                              parent_log_name_);
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
    pipeline_.record_cut_start(doc_ndx);
    if (auto res = cutter_->cut(doc_ndx); ! res)
    {
      // A malformed document is a per-document failure, not a fatal one: mark it invalid so
      // any already-cut segments of this document get discarded by P, then keep going.
      pipeline_.report_validation_result(doc_ndx, doc_status::validation_failed, res.error());
    }
    else
    {
      pipeline_.record_cut_finished(doc_ndx, cutter_->segments_found());
    }
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
    log_.make_log_name(parent_log_name_, fmt::format("pipe-wrk.{:02}", worker_id));
    auto& pool = pipeline_.pool();

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
      //    chance to flag invalid documents before we spend effort processing their segments
      if (pool.ready_queue_size_approx() > 0)
        if (auto seg_ndx = pool.try_pop_ready())
        {
          auto doc_ndx = processor_->process_one(*seg_ndx);
          pipeline_.record_segment_done(static_cast<std::size_t>(doc_ndx));
          continue;
        }

      // Neither C nor V currently show any work -> authoritative, blocking wait on the only
      // queue that can still dynamically receive work from OTHER threads. This is also the
      // real exit condition for this thread.
      std::size_t seg_ndx = 0;
      if (pool.pop_segment_ndx(seg_ndx) != queue_status::active) break;
      {
        auto doc_ndx = processor_->process_one(seg_ndx);
        pipeline_.record_segment_done(static_cast<std::size_t>(doc_ndx));
      }
    }
    processor_->flush_results();
  }
} // namespace fsp