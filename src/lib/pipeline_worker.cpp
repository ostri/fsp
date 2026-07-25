#include "pipeline_worker.hpp"
#include "pipeline.hpp"
#include <fmt/format.h>
#include <chrono>
#include <thread>

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
    if (auto res = cutter_->cut(doc_ndx); ! res)
    {
      // A malformed document is a per-document failure, not a fatal one: mark it invalid so
      // any already-cut segments of this document get discarded by P, then keep going.
      pipeline_.report_validation_result(doc_ndx, doc_status::validation_failed, res.error());
    }
    pipeline_.notify_cut_done();
  }

  void pipeline_worker::do_validate(std::size_t doc_ndx)
  {
    // TODO: ostri - ostri replace with a real xercesc/XSD validator; for now a stub as agreed.
    std::this_thread::sleep_for(std::chrono::seconds(17)); // NOLINT(readability-magic-numbers)
    pipeline_.report_validation_result(doc_ndx, doc_status::validation_ok);
  }

  void pipeline_worker::operator()(const std::stop_token& st, int worker_id)
  {
    log_.make_log_name(parent_log_name_, fmt::format("pw.{:02}", worker_id));
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
          processor_->process_one(*seg_ndx);
          continue;
        }

      // Neither C nor V currently show any work -> authoritative, blocking wait on the only
      // queue that can still dynamically receive work from OTHER threads. This is also the
      // real exit condition for this thread.
      std::size_t seg_ndx = 0;
      if (pool.pop_segment_ndx(seg_ndx) != queue_status::active) break;
      processor_->process_one(seg_ndx);
    }
    processor_->flush_results();
  }
} // namespace fsp