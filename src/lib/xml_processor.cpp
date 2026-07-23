#include "xml_processor.hpp"
#include "doc_set_dscr.hpp"
#include "load_grammar.hpp"
#include "x_str.hpp"
#include "xml_worker.hpp"
#include "xpath_helpers.hpp"
#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <utility>
#include <libxml/xmlreader.h>
#include <fmt/chrono.h>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
namespace
{
} // namespace
namespace fsp
{

  // ============================================================================
  // Constructor / Destructor
  // ============================================================================
  // xml_processor::xml_processor(const processor_config& cfg)
  // : fsp::xml_processor(std::move(cfg), "main", {})
  // {
  // }

  xml_processor::xml_processor(processor_config cfg, str_t parent_log_name, segment_pool& pool, doc_set_dscr& ds_dscr)
  : log_(cfg.log_config)
  , cfg_(std::move(cfg))
  , seg_pool_(pool)
  , ds_dscr_(ds_dscr)
  , parent_log_name_(std::move(parent_log_name))
  {
    bool first_time = log_.log_name() == "unknown";
    if (first_time)
    {
      std::string_view build_type;
      if constexpr (is_release()) build_type = "release";
      else build_type = "debug";
      if (cfg_.num_workers == 0) cfg_.num_workers = std::thread::hardware_concurrency();
      if (cfg_.num_workers == 0) cfg_.num_workers = 1; // if statement above fails
      log_.info(fmt::format(
        "XML Processor: started: build type: {} -> {} workers, validation: {}", build_type, cfg_.num_workers, cfg_.validate_against_xsd));
    }
  }
  xml_processor::~xml_processor()
  {
    cancel();
    stop_workers();
    parser_.reset();
    auto       stat = stats();
    const auto kilo = 1000;
    log_.info(fmt::format("XML Processor: finished: {:.3f} sec segments:{} (ok:{} err:{})",
                          stat.processing_time_ms / kilo, // converting from milisecond to seconds
                          stat.total_segments(),
                          stat.successful_segments,
                          stat.failed_segments));
  }
  // ============================================================================
  // Parser setup
  // ============================================================================
  void_result xml_processor::setup_parser_no_validation()
  {
    try
    {
      // no grammar pool
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager));
      // NOLINTBEGIN(hicpp-no-array-decay)
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchema, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesLoadExternalDTD, false);

      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);

      // scanner without grammar
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      parser_->setProperty(xercesc::XMLUni::fgXercesScannerName, const_cast<XMLCh*>(xercesc::XMLUni::fgWFXMLScanner));
      // NOLINTEND(hicpp-no-array-decay)

      if (log_debug_) log_.debug("Parser (WFXMLScanner - no grammar) setup successful");
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto err =
        error_info{processor_error::internal_error, fmt::format("Parser init failed: {}", x_str(e.getMessage()).to_string()), "", 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }
  }

  void_result xml_processor::start_workers()
  {
    {
      std::lock_guard lock(results_mutex_);
      results_.clear();
    }
    {
      std::lock_guard lock(errors_mutex_);
      errors_.clear();
    }
    if (! active_mmap().is_open())
    {
      auto err = error_info{//
                            processor_error::internal_error,
                            "mmap is not opened 'start_workers()'",
                            "",
                            0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }

    for (std::size_t i = 0; i < cfg_.num_workers; ++i)
    {
      str_t parent_name = log_.log_name();
      workers_.emplace_back(
        xml_worker{           //
                   seg_pool_, //
                   active_mmap(),
                   results_,
                   errors_,
                   results_mutex_,
                   errors_mutex_,
                   log_,
                   cfg_.targets,
                   parent_name},
        i);
    }
    log_.info(fmt::format("{} workers started.", cfg_.num_workers));
    return {};
  }
  /**
   * @brief wait for workers to stop and clean up related data structures
   *
   */
  void xml_processor::stop_workers()
  {
    seg_pool_.ready_queue_close();
    workers_.clear();
    log_.info("All workers stopped.");
  }
  /**
   * @brief signal to workers to immediately finish with work
   *
   */
  void xml_processor::cancel()
  {
    for (auto& el : workers_) el.request_stop();
    seg_pool_.ready_queue_close();
  }

  void_result xml_processor::process_from_buffer(std::size_t xml_path_ndx)
  {
    auto& xml_mmap = ds_dscr_[xml_path_ndx].mmf();
    auto  ps       = setup_parser_no_validation();
    if (! ps) return std::unexpected(ps.error());

    try
    {
      handler_ = std::make_unique<Handler>(cfg_.targets, //
                                           log_,
                                           parser_.get(),
                                           xml_mmap.string_view(),
                                           seg_pool_);
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }
    auto ws = start_workers(); // validation is on critical path workers start last
    if (! ws) return std::unexpected(ws.error());
    try
    {
      auto t0 = std::chrono::steady_clock::now();
      if (log_info_) log_.info("SAX parsing started.");
      xercesc::MemBufInputSource src(
        reinterpret_cast<const XMLByte*>(xml_mmap.data()), static_cast<XMLSize_t>(xml_mmap.size()), "xml_input", false);
      parser_->parse(src);
      save_stats();

      auto us = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (log_info_) log_.info(fmt::format("SAX parsing finished. {} ms pending segments: {}", us, seg_pool_.ready_queue_size()));
      seg_pool_.ready_queue_close();
    }
    // catch (const xercesc::SAXParseException& e)
    // {
    //   // [DODANO] Handler je vrgel SAXParseException kot signal za prekinitev
    //   // parsinga ob zaznani validacijski napaki. Napaka je že v val_future —
    //   // ne logiramo tukaj, logirala jo je validacijska nit.
    //   // Nastavimo zastavico da spodaj ne logiramo lažne "parse" napake.
    //   // validation_interrupted = true;
    //   auto err = error_info{
    //     processor_error::xsd_validation_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getLineNumber())};
    //   log_.error(err.to_string());
    // }
    catch (const xercesc::XMLException& e)
    {
      // Prava napaka parsinga (ne validacijska) — canceliramo in vrnemo napako
      cancel();
      workers_.clear();
      auto err = error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())};
      log_.error(err.to_string());
      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      cancel();
      workers_.clear();
      auto err = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }
    seg_pool_.ready_queue_close();
    workers_.clear();
    success_ = true;
    return {};
  }
  // ============================================================================
  // Rezultati
  // ============================================================================
  const vec_seg_result& xml_processor::get_results() const
  {
    std::lock_guard lock(results_mutex_);
    return results_;
  }
  const vec_seg_result& xml_processor::get_errors() const
  {
    std::lock_guard lock(errors_mutex_);
    return errors_;
  }
  void xml_processor::save_stats()
  {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    stats_  = stats_t{
      .successful_doc      = 0,
      .failed_doc          = 0,
      .successful_segments = results_.size(), // processed_count_,
      .failed_segments     = errors_.size(),  // error_count_,
      .active_workers      = workers_.size() > 0 ? workers_.size() : cfg_.num_workers,
      .processing_time_ms  = static_cast<double>(ms),
    };
  }
  // // ============================================================================
  // // Convenience function
  // // ============================================================================
  void xml_processor::process_one_doc(std::size_t                  doc_ndx,
                                      std::mutex&                  results_agg_mutex,
                                      std::vector<segment_result>& all_results,
                                      std::vector<segment_result>& all_errors,
                                      std::atomic<bool>&           has_error,
                                      std::optional<error_info>&   first_error)
  {
    doc_ndx_            = doc_ndx;
    const auto xml_path = ds_dscr_[doc_ndx_].path();
    log_.info(fmt::format("Processing file: '{}'", xml_path));
    //  Each file gets its own processor instance to avoid state conflicts
    xml_processor file_proc(cfg_, log_.log_name(), seg_pool_, ds_dscr_);
    auto          res = file_proc.process_from_buffer(doc_ndx_);
    if (! res)
    {
      auto err = res.error();
      {
        std::lock_guard<std::mutex> lock(results_agg_mutex);
        if (! has_error)
        {
          has_error   = true;
          first_error = err;
        }
      }
      log_.error(fmt::format("File {} failed: {}", xml_path, err.to_string()));
    }
    else
    {
      file_proc.save_stats(); // save current stratistics
      auto fr = file_proc.move_results();
      auto fe = file_proc.move_errors();
      {
        std::lock_guard<std::mutex> lock(results_agg_mutex);
        all_results.append_range(fr | std::views::as_rvalue);
        all_errors.append_range(fe | std::views::as_rvalue);
      }
      auto stats = file_proc.stats();
      log_.info(fmt::format("File '{}' success (ok: {} err:{})", //
                            xml_path,
                            stats.successful_segments,
                            stats.failed_segments));
    }
  }
  // Helper static function for jthread execution
  void xml_processor::doc_worker(lock_queue<std::size_t>&   doc_queue,
                                 doc_set_dscr&              ds_dscr,
                                 segment_pool&              seg_pool,
                                 std::mutex&                results_agg_mutex,
                                 vec_seg_result&            all_results,
                                 vec_seg_result&            all_errors,
                                 std::atomic<std::size_t>&  file_processed,
                                 std::atomic<bool>&         has_error,
                                 std::optional<error_info>& first_error,
                                 std::size_t                worker_idx,
                                 const std::string&         parent_log_name,
                                 const processor_config&    config,
                                 const fsp_logger&          log)
  {
    log.make_log_name(parent_log_name, fmt::format("doc-{:02}", worker_idx));
    std::size_t   doc_ndx;
    xml_processor doc(config, parent_log_name, seg_pool, ds_dscr);
    while (true) // loop the documents list
    {
      if (auto opt = doc_queue.try_pop()) { doc_ndx = opt.value(); }
      else
      { // the queue was initally empty and we need to wait for first doc or a signal to exit the waiting
        auto start = std::chrono::steady_clock::now();
        if (doc_queue.pop(doc_ndx) != queue_status::active) break; // finished or aborted
        auto end     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        log.info(fmt::format("waiting time for new file {} µs.", elapsed));
      }
      doc.process_one_doc(doc_ndx, results_agg_mutex, all_results, all_errors, has_error, first_error);
    }
    ++file_processed;
    if (doc_queue.is_aborted()) log.warn(fmt::format("doc_worker {}/{} aborted.", log.log_name(), worker_idx));
  }
}; // namespace fsp
