#include "pipeline.hpp"
#include "pipeline_worker.hpp"
#include <thread>
#include <fmt/format.h>

namespace fsp
{
  pipeline::pipeline(processor_config cfg, const fsp_logger& log, str_t parent_log_name)
  : log_(log)
  , cfg_(std::move(cfg))
  , parent_log_name_(std::move(parent_log_name))
  , ds_dscr_(log_)
  , pool_(log_, 1024UL * 1024UL * 8UL, std::max<std::size_t>(1, cfg_.pool_shard_count)) // NOLINT(readability-magic-numbers)
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
      pool_.ready_queue_close();
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

  void pipeline::record_doc_close(std::size_t doc_ndx, std::size_t segment_count)
  {
    if ((*doc_counters_)[doc_ndx].record_doc_close(segment_count)) log_doc_done(doc_ndx);
  }

  bool pipeline::record_segment_done(std::size_t doc_ndx, std::size_t seg_id, const result_values& values, pipeline_hooks& hooks)
  {
    auto&      counters       = (*doc_counters_)[doc_ndx];
    const auto pos            = counters.begin_segment();
    const bool semantically_ok = hooks.on_seg_proc(seg_id, doc_ndx, values, pos.is_first, pos.is_last, log_);
    if (counters.end_segment(semantically_ok)) log_doc_done(doc_ndx);
    return semantically_ok;
  }

  void pipeline::record_segment_failed(std::size_t doc_ndx)
  {
    auto& counters = (*doc_counters_)[doc_ndx];
    (void)counters.begin_segment(); // bookkeeping only (segments_seen_/first_seg_) -- no hook, no meaningful values
    if (counters.end_segment(false)) log_doc_done(doc_ndx);
  }

  // Called once a document is complete -- whichever of record_doc_close()/record_segment_done()/
  // record_segment_failed() turns out to be the one that satisfies the last remaining condition
  // (see doc_counters::maybe_complete()) is the one whose call returns true and triggers this.
  void pipeline::log_doc_done(std::size_t doc_ndx)
  {
    const auto& c = (*doc_counters_)[doc_ndx];
    log_.debug(fmt::format("Doc {}: cut+process finished ({} segments, {} ms).", doc_ndx, c.total(), c.total_latency().count()));
  }

  void pipeline::report_validation_result(std::size_t doc_ndx, doc_status result, error_info err)
  {
    ds_dscr_[doc_ndx].set_validation_result(result, std::move(err));
    (*doc_counters_)[doc_ndx].record_validation_result(result != doc_status::validation_failed);
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
      if (ds_dscr_[i].status() == doc_status::validation_failed) out.push_back(i);
    return out;
  }

  void_result pipeline::add_documents(const std::vector<std::string>& xml_paths, const std::string& xsd_path)
  {
    for (const auto& file : xml_paths)
      if (! ds_dscr_.add_document(file))
        return std::unexpected(error_info{processor_error::file_open_failed, fmt::format("Failed to add document: '{}'", file), file, 0});
    ds_dscr_.set_grammar(xsd_path);
    return {};
  }

  pipeline::run_plan pipeline::plan_run(std::size_t doc_count)
  {
    // Whether C folds XSD validation into its own SAX pass instead of running it separately as
    // V (see doc_cutter.cpp / processor_config.hpp for the measured break-even). cfg_.cut_with_validation
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

    auto requested_threads = cfg_.num_docs;
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
    return {.run_validation = run_validation, .num_parallel = num_parallel};
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

  void pipeline::discard_invalid_doc_results()
  {
    // Safety net for the rare race where P processed a segment before V (or a late C failure)
    // marked its document invalid -- priority C > V > P makes this uncommon but not impossible.
    // Discard any result/error whose document ended up invalid.
    auto belongs_to_invalid_doc = [this](const segment_result& r)
    {
      return r.doc_ndx() >= 0 && static_cast<std::size_t>(r.doc_ndx()) < ds_dscr_.size() &&
             ds_dscr_[static_cast<std::size_t>(r.doc_ndx())].status() == doc_status::validation_failed;
    };
    std::erase_if(results_, belongs_to_invalid_doc);
    std::erase_if(errors_, belongs_to_invalid_doc);
  }

  std::string pipeline::build_summary(std::size_t doc_count, double elapsed_ms, std::size_t failed_count) const
  {
    const auto pool_capacity = pool_.size();
    const auto pool_peak     = pool_.high_water_mark();
    const auto pool_pct      = pool_capacity > 0 ? (static_cast<double>(pool_peak) / static_cast<double>(pool_capacity)) * 100.0
                                                 : 0.0; // NOLINT(readability-magic-numbers)
    auto       msg           = fmt::format(R"(
  Processed {0} docs in {1:.3f} sec (segments ok:{2} err:{3}, docs failed:{4}) seg. peak: {5} / {6} slots ({7:.2f}%))",
                                           doc_count,
                                           elapsed_ms / 1000.0, // NOLINT(readability-magic-numbers)
                                           results_.size(),
                                           errors_.size(),
                                           failed_count,
                                           pool_peak,
                                           pool_capacity,
                                           pool_pct);
    msg += fmt::format("\ndocument statistics:\n{}", doc_counters_->dump(2)); // NOLINT(readability-magic-numbers)
    return msg;
  }

  result<doc_set_counter> pipeline::process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path, pipeline_hooks& hooks)
  {
    if (xml_paths.empty())
    {
      log_.info("No files to process.");
      hooks.on_run_start(ds_dscr_, log_);
      hooks.on_run_end(doc_set_counter(0), ds_dscr_, {}, log_);
      return doc_set_counter(0);
    }

    if (auto added = add_documents(xml_paths, xsd_path); ! added) return std::unexpected(added.error());
    hooks.on_run_start(ds_dscr_, log_);

    const auto doc_count = xml_paths.size();
    const auto plan       = plan_run(doc_count);

    docs_remaining_to_cut_.store(doc_count, std::memory_order_relaxed);
    doc_counters_.emplace(doc_count);
    seed_queues(doc_count, plan.run_validation);

    log_.info(fmt::format(
      "Pipeline: {} documents, {} hybrid worker threads (max {} cutting concurrently).", doc_count, plan.num_parallel, max_concurrent_cutters_));

    auto worker_state = start_workers(plan.num_parallel, hooks);
    if (! worker_state) return std::unexpected(worker_state.error());

    run_workers(*worker_state);

    // Always fires exactly once, paired with on_run_start() above, regardless of whether the
    // run goes on to succeed or hit a fatal error below -- so the developer always sees the
    // final counters/ds_dscr/worker clones for whatever actually happened.
    std::vector<const pipeline_hooks*> worker_clones;
    worker_clones.reserve(worker_state->size());
    for (const auto& w : *worker_state) worker_clones.push_back(&w->hooks());
    hooks.on_run_end(*doc_counters_, ds_dscr_, worker_clones, log_);

    discard_invalid_doc_results();
    if (first_error_)
    {
      log_.error(fmt::format("Pipeline failed with a fatal error: {}", first_error_->to_string()));
      return std::unexpected(*first_error_);
    }

    auto failed = failed_document_indices();
    if (! failed.empty()) log_.warn(fmt::format("{} of {} document(s) failed and were skipped.", failed.size(), doc_count));

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    stats_  = stats_t{
      .successful_segments = results_.size(),
      .failed_segments     = errors_.size(),
      .processing_time_ms  = static_cast<double>(ms),
    };

    log_.info(build_summary(doc_count, static_cast<double>(ms), failed.size())); // NOLINT(readability-magic-numbers)
    return std::move(*doc_counters_);
  }

  const vec_seg_result& pipeline::get_results() const
  {
    std::lock_guard lock(results_mutex_);
    return results_;
  }
  const vec_seg_result& pipeline::get_errors() const
  {
    std::lock_guard lock(errors_mutex_);
    return errors_;
  }
} // namespace fsp