#include "process_docs.hpp"
#include <thread>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include "xml_processor.hpp"

namespace fsp
{
  void_result process_docs::process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path)
  {
    if (xml_paths.empty())
    {
      log_.info("No files to process.");
      return {};
    }
    for (const auto& file : xml_paths) ds_dscr_.add_document(file);
    ds_dscr_.set_grammar(xsd_path);
    return process_files_internal();
  }
  void_result process_docs::process_files_internal()
  {
    log_.make_log_name(parent_log_name_);
    auto        xsd_path  = ds_dscr_.xsd_file();
    const auto& xml_paths = ds_dscr_.doc_set();
    //    bool                        has_grammar = ds_dscr_.has_grammar();
    //    std::optional<std::jthread> gp_loader;
    gr_pool_t gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
    // std::latch                  gp_latch(1);                                         // just waiting for grammar to be loaded
    // std::atomic<bool>           gp_loaded{false};                                    // is grammar loaded?
    auto num_parallel = cfg_.num_docs;
    // if (has_grammar)
    // {
    //   std::stop_token st{};
    //   gp_loader.emplace(load_grammar::load, //
    //                     st,
    //                     std::ref(gp),
    //                     std::ref(gp_latch),
    //                     std::ref(gp_loaded),
    //                     std::string{xsd_path});
    // }
    if (num_parallel == 0)
    {
      num_parallel = std::thread::hardware_concurrency();
      if (num_parallel == 0) num_parallel = 1;
    }
    num_parallel = std::min(num_parallel, xml_paths.size());

    log_.info(fmt::format(
      "Processing {} XML files with {} parallel workers. XSD: {}", xml_paths.size(), num_parallel, ds_dscr_.empty() ? "none" : xsd_path));

    // Queue for file paths
    lock_queue<std::size_t> doc_queue;
    for (auto ndx = 0UL; ndx < xml_paths.size(); ndx++) doc_queue.push(ndx); // copy
    doc_queue.set_finished(); // after all files are communicated we need to signal that this is all
    // otherwise program hangs on thread join
    std::vector<std::jthread>   doc_workers;
    std::mutex                  results_agg_mutex;
    std::vector<segment_result> all_results;
    std::vector<segment_result> all_errors;
    std::atomic<std::size_t>    doc_processed{0};
    std::atomic<bool>           has_error{false};
    std::optional<error_info>   first_error;

    // Start workers
    doc_workers.reserve(num_parallel);
    // if (has_grammar)
    // {
    //   gp_latch.wait(); // wait till the grammar is loaded
    //   if (! gp_loaded) throw std::runtime_error(fmt::format("XSD grammar '{}' cannot be loaded. aborting", xsd_path));
    // }
    for (std::size_t i = 0; i < num_parallel; ++i)
    {
      auto log_name = log_.log_name();
      // create file_worker_task thread
      doc_workers.emplace_back(xml_processor::doc_worker,
                               std::ref(doc_queue),
                               std::ref(ds_dscr_),
                               std::ref(pool_),
                               std::ref(results_agg_mutex),
                               std::ref(results_),
                               std::ref(errors_),
                               std::ref(doc_processed),
                               std::ref(has_error),
                               std::ref(first_error),
                               i,
                               log_name,
                               std::cref(cfg_),
                               std::cref(log_));
    }

    // Wait for all workers
    for (auto& w : doc_workers)
      if (w.joinable()) w.join();

    if (has_error && first_error)
    {
      log_.error(fmt::format("process_files failed with first error: {}", first_error->to_string()));
      return std::unexpected(*first_error);
    }
    auto sec  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    auto diff = static_cast<double>(sec) / 1000.0; // NOLINT(readability-magic-numbers)
    log_.info(fmt::format("Processed {} files successfully in {:.3f} sec.", doc_processed.load(), diff));
    // if (has_grammar) gp.reset();
    return {};
  }

} // namespace fsp
