#include "process_docs.hpp"
#include <thread>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include "xml_processor.hpp"

namespace fsp
{
  void_result process_docs::process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path)
  {
    // process_docs p(cfg);
    return process_files_internal(xml_paths, xsd_path);
  }
  void_result process_docs::process_files_internal(const std::vector<std::string>& xml_paths,
                                                   const std::string&              xsd_path //,
                                                                                            // std::size_t                     num_parallel
  )
  {
    log_.make_log_name(parent_log_name_);
    if (xml_paths.empty())
    {
      log_.info("No files to process.");
      return {};
    }
    bool                        has_grammar = ! xsd_path.empty();
    std::optional<std::jthread> gp_loader;
    gr_pool_t                   gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
    std::latch                  gp_latch(1);                                         // just waiting for grammar to be loaded
    std::atomic<bool>           gp_loaded{false};                                    // is grammar loaded?
    auto                        num_parallel = cfg_.num_docs;
    if (has_grammar) { gp_loader.emplace(load_grammar::load, std::ref(gp), std::ref(gp_latch), std::ref(gp_loaded), xsd_path); }
    if (num_parallel == 0)
    {
      num_parallel = std::thread::hardware_concurrency();
      if (num_parallel == 0) num_parallel = 1;
    }
    num_parallel = std::min(num_parallel, xml_paths.size());

    log_.info(fmt::format(
      "Processing {} XML files with {} parallel workers. XSD: {}", xml_paths.size(), num_parallel, xsd_path.empty() ? "none" : xsd_path));

    // Queue for file paths
    lock_queue<std::string> file_queue;
    for (const auto& path : xml_paths)
    {
      file_queue.push(std::string(path)); // copy
    }
    file_queue.set_finished(); // after all files are communicated we need to signal that this is all
    // otherwise program hangs on thread join
    std::vector<std::jthread>   file_workers;
    std::mutex                  results_agg_mutex;
    std::vector<segment_result> all_results;
    std::vector<segment_result> all_errors;
    std::atomic<std::size_t>    file_processed{0};
    std::atomic<bool>           has_error{false};
    std::optional<error_info>   first_error;

    // Start workers
    file_workers.reserve(num_parallel);
    if (has_grammar)
    {
      gp_latch.wait(); // wait till the grammar is loaded
      if (! gp_loaded) throw std::runtime_error(fmt::format("XSD grammar '{}' cannot be loaded. aborting", xsd_path));
    }
    for (std::size_t i = 0; i < num_parallel; ++i)
    {
      auto log_name = log_.log_name();
      // create file_worker_task thread
      file_workers.emplace_back(xml_processor::file_worker_task,
                                std::ref(file_queue),
                                std::cref(xsd_path),
                                std::ref(results_agg_mutex),
                                std::ref(results_),
                                std::ref(errors_),
                                std::ref(file_processed),
                                std::ref(has_error),
                                std::ref(first_error),
                                i,
                                log_name,
                                std::cref(cfg_),
                                std::cref(log_),
                                std::cref(gp),
                                std::ref(gp_latch),
                                std::ref(gp_loaded),
                                has_grammar);
    }

    // Wait for all workers
    for (auto& w : file_workers)
      if (w.joinable()) w.join();

    if (has_error && first_error)
    {
      log_.error(fmt::format("process_files failed with first error: {}", first_error->to_string()));
      return std::unexpected(*first_error);
    }
    auto sec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    log_.info(fmt::format("Processed {} files successfully in {:.3f} sec.", file_processed.load(), static_cast<double>(sec / 1000.0)));
    if (has_grammar) gp.reset();
    return {};
  }

} // namespace fsp
