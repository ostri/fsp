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

    auto num_parallel = cfg_.num_docs;
    if (num_parallel == 0) num_parallel = std::thread::hardware_concurrency();
    if (num_parallel == 0) num_parallel = 1;
    num_parallel = std::min(num_parallel, xml_paths.size());

    const auto doc_count = xml_paths.size();
    docs_remaining_to_cut_.store(doc_count, std::memory_order_relaxed);

    // c_queue_ and v_queue_: both known, final quantities up front -> set_finished() immediately
    for (std::size_t i = 0; i < doc_count; ++i)
    {
      c_queue_.push(i);
      v_queue_.push(i);
    }
    c_queue_.set_finished();
    v_queue_.set_finished();

    log_.info(fmt::format("Pipeline: {} documents, {} hybrid worker threads.", doc_count, num_parallel));

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