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
  , pool_(log_, 1024UL * 1024UL * 8UL) // NOLINT(readability-magic-numbers)
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

  void pipeline::report_validation_result(std::size_t doc_ndx, doc_status result, error_info err)

  { ds_dscr_[doc_ndx].set_validation_result(result, std::move(err)); }

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

  void_result pipeline::process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path)
  {
    if (xml_paths.empty())
    {
      log_.info("No files to process.");
      return {};
    }

    for (const auto& file : xml_paths) ds_dscr_.add_document(file);
    ds_dscr_.set_grammar(xsd_path);

    // Skip V entirely when no XSD grammar was successfully loaded -- has_grammar() also covers
    // the case where a path was given but loading it failed (set_grammar() swallows that and
    // returns false without setting anything).
    const bool run_validation = ds_dscr_.has_grammar();
    if (! run_validation) log_.info("No XSD grammar available -- V (validation) is disabled for this run.");

    auto num_parallel = cfg_.num_docs;
    if (num_parallel == 0) num_parallel = std::thread::hardware_concurrency();
    if (num_parallel == 0) num_parallel = 1;
    // NOTE: no longer capped to xml_paths.size() -- unlike the old per-document worker model,
    // P operates on segments independently of document count, so threads beyond doc_count are
    // still useful (they simply never win a C reservation and help drain the pool via P instead).

    // At most half the threads may cut concurrently, guaranteeing that at least as many
    // threads remain structurally free for P as are currently committed to C.
    max_concurrent_cutters_ = std::max<std::size_t>(1, num_parallel / 2);

    const auto doc_count = xml_paths.size();
    docs_remaining_to_cut_.store(doc_count, std::memory_order_relaxed);

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

    log_.info(fmt::format(
      "Pipeline: {} documents, {} hybrid worker threads (max {} cutting concurrently).", doc_count, num_parallel, max_concurrent_cutters_));
    std::vector<std::unique_ptr<pipeline_worker>> worker_state;
    worker_state.reserve(num_parallel);
    for (std::size_t i = 0; i < num_parallel; ++i)
    {
      auto w = std::make_unique<pipeline_worker>(*this, cfg_, log_, parent_log_name_);
      if (auto init_res = w->init(); ! init_res)
      {
        log_.error(fmt::format("pipeline_worker {} init failed: {}", i, init_res.error().to_string()));
        return std::unexpected(init_res.error()); // infra-level failure, treated as fatal for the whole run
      }
      worker_state.push_back(std::move(w));
    }

    std::vector<std::jthread> threads;
    threads.reserve(num_parallel);
    for (std::size_t i = 0; i < worker_state.size(); ++i) threads.emplace_back(std::ref(*worker_state[i]), static_cast<int>(i));

    for (auto& t : threads)
      if (t.joinable()) t.join();
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
    log_.info(fmt::format("Processed {} docs in {:.3f} sec (segments ok:{} err:{}, docs failed:{}).",
                          doc_count,
                          static_cast<double>(ms) / 1000.0, // NOLINT(readability-magic-numbers)
                          results_.size(),
                          errors_.size(),
                          failed.size()));
    return {};
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