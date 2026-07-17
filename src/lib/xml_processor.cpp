#include "xml_processor.hpp"
#include "load_grammar.hpp"
#include "x_str.hpp"
#include "xml_worker.hpp"
#include "xpath_helpers.hpp"
#include <chrono>
#include <fmt/format.h>
#include <functional>
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
  xml_processor::xml_processor(processor_config cfg)
  : fsp::xml_processor(std::move(cfg), "main")
  {
  }

  xml_processor::xml_processor(processor_config cfg, str_t parent_log_name)
  : log_(cfg.log_config)
  , config_(std::move(cfg))
  , parent_log_name_(std::move(parent_log_name))
  {
    bool first_time = log_.log_name() == "unknown";
    if (first_time)
    {
      std::string_view build_type;
      if constexpr (is_release()) build_type = "release";
      else build_type = "debug";
      if (config_.num_workers == 0) config_.num_workers = std::thread::hardware_concurrency();
      if (config_.num_workers == 0) config_.num_workers = 1; // if statement above fails
      log_.info(fmt::format("XML Processor: started: build type: {} -> {} workers, validation: {}",
                            build_type,
                            config_.num_workers,
                            config_.validate_against_xsd));
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
  // ============================================================================
  // Validation thread
  // ============================================================================
  // Statična funkcija, ki izvaja validacijo
  std::optional<error_info> xml_processor::validate_xml_worker(const cstr_t&      f_xml_data,
                                                               const gr_pool_t&   gp,
                                                               std::string        xsd_path,
                                                               const fsp_logger&  logger,
                                                               const std::string& parent_log_name)
  {
    auto        start = std::chrono::steady_clock::now();
    const auto& log   = logger;
    log.make_log_name(parent_log_name, "valid");

    if (log.active(info)) log.info(fmt::format("Validation started. file: xsd:{}", xsd_path));

    try
    {
      // Ustvarimo lasten parser, ker ni reentrant
      std::unique_ptr<xercesc::SAX2XMLReader> vparser(xercesc::XMLReaderFactory::createXMLReader( //
        xercesc::XMLPlatformUtils::fgMemoryManager,
        gp.get()));

      // Konfiguracija parserja
      // NOLINTBEGIN(hicpp-no-array-decay)
      vparser->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
      vparser->setFeature(xercesc::XMLUni::fgXercesSchema, ! xsd_path.empty());
      vparser->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
      vparser->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
      vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      vparser->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
      vparser->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, false);
      vparser->setFeature(xercesc::XMLUni::fgXercesCacheGrammarFromParse, false);
      // NOLINTEND(hicpp-no-array-decay)

      // Naložimo XSD shemo v parser
      // mem_buf_holder xsd_holder(f_xsd_data.data(), f_xsd_data.size(), xsd_path, log);
      // if (xsd_holder.source() != nullptr) vparser->loadGrammar(*xsd_holder.source(), xercesc::Grammar::SchemaGrammarType, true);

      // Handler za napake, ki vrže izjemo
      struct ThrowingErrorHandler : public xercesc::DefaultHandler
      { // NOLINTBEGIN(hicpp-exception-baseclass)
        void error(const xercesc::SAXParseException& e) override { throw e; }
        void fatalError(const xercesc::SAXParseException& e) override { throw e; }
        // NOLINTEND(hicpp-exception-baseclass)
      } err_handler;
      vparser->setErrorHandler(&err_handler);

      // Parsiranje XML
      xercesc::MemBufInputSource src(
        reinterpret_cast<const XMLByte*>(f_xml_data.data()), static_cast<XMLSize_t>(f_xml_data.size()), "xml_validation", false);
      vparser->parse(src);

      if (log.active(fsp::lvl_enum::info))
      {
        auto end      = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        log.info(fmt::format("Validation finished in {} ms {} bytes.", duration.count(), f_xml_data.size()));
      }
      return std::nullopt; // ni napak
    }
    catch (const xercesc::SAXParseException& e)
    {
      auto err = error_info{
        processor_error::xsd_validation_failed,
        fmt::format("SAX validation error: {} (row:{} col:{})", x_str(e.getMessage()).to_string(), e.getLineNumber(), e.getColumnNumber()),
        "",
        static_cast<std::size_t>(e.getLineNumber())};
      if (log.active(lvl_enum::err)) log.error(err.to_string());
      return err;
    }
    catch (const xercesc::XMLException& e)
    {
      auto err = error_info{processor_error::xsd_validation_failed, fmt::format("XML error: {}", x_str(e.getMessage()).to_string()), "", 0};
      if (log.active(lvl_enum::err)) log.error(err.to_string());
      return err;
    }
    catch (...)
    {
      auto err = error_info{processor_error::internal_error, "Validation: unknown error", "", 0};
      if (log.active(lvl_enum::err)) log.error(err.to_string());
      return err;
    }
  }
  // Zažene validacijo XML proti XSD v ločeni niti. Vrne shared_future ki se
  // razreši takoj ko validacija konča — bodisi z nullopt (ok) ali error_info
  // (prva napaka). shared_future (ne unique future) ker ga delita dve mesti:
  //   1. Handler::startElement() — polling z wait_for(0)
  //   2. process_from_buffer()   — get() po koncu parsinga
  // Oba klica sta na isti niti (glavna nit), zato ni race conditiona na get().
  //
  // Parametri so kopirani po vrednosti — nit mora imeti lastništvo nad podatki
  // ki jih potrebuje, saj mmap ostaja živeti v klicatelju (process_from_buffer),
  // a nit ne sme imeti surovih referenc nanj (lifetime ni garantiran).
  // xml_data/xsd_data sta raw pointer-ja na mmap ki živita dlje od niti — ok.
  std::shared_future<std::optional<error_info>> xml_processor::launch_validation_thread( //
    const cstr_t&    f_xml_data,
    const gr_pool_t& gp,      // xml file contents
    std::string      xsd_path // path to the grammar file
  )
  {
    // Kopiramo podatke za nit
    auto xml_data = f_xml_data; // kopija
    // auto xsd_data = f_xsd_data; // kopija
    auto path = std::move(xsd_path);

    // Ustvarimo async nalogo s statično funkcijo
    auto future = std::async(std::launch::async,
                             validate_xml_worker,
                             xml_data,                   // must be by value
                             std::cref(gp),              // referenca na kopijo
                             std::move(path),            // premaknemo path
                             std::cref(log_),            // referenca na logger
                             std::cref(parent_log_name_) // referenca na parent log name
    );
    // Vrnemo shared_future
    return future.share();
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
    if (active_mmap_ == nullptr)
    {
      auto err = error_info{processor_error::internal_error, "mmap is null before 'start_workers()'", active_mmap_->path(), 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }

    for (std::size_t i = 0; i < config_.num_workers; ++i)
    {
      str_t parent_name = log_.log_name();
      workers_.emplace_back(xml_worker{seg_queue_, //
                                       *active_mmap_,
                                       results_,
                                       errors_,
                                       results_mutex_,
                                       errors_mutex_,
                                       //                                       cancel_flag_,
                                       log_,
                                       config_.targets,
                                       parent_name},
                            i);
    }
    log_.info(fmt::format("{} workers started.", config_.num_workers));
    return {};
  }
  /**
   * @brief wait for workers to stop and clean up related data structures
   *
   */
  void xml_processor::stop_workers()
  {
    seg_queue_.set_finished();
    workers_.clear();
    active_mmap_ = nullptr;
    log_.info("All workers stopped.");
  }
  /**
   * @brief signal to workers to immediately finish with work
   *
   */
  void xml_processor::cancel()
  {
    // cancel_flag_ = true;
    for (auto& el : workers_) el.request_stop();
    seg_queue_.set_finished();
  }
  /**
   * @brief parse xml file
   *
   * @param xml_path filepath to the xml file
   * @param xsd_path filepath to the corresponding grammas (XSD)
   * @return void_result
   */
  void_result xml_processor::process_file(const std::string& xml_path, const std::string& xsd_path, const gr_pool_t& gp)
  {
    start_time_ = std::chrono::steady_clock::now();
    log_.info(fmt::format("XML file: '{}'", xml_path));

    fsp::mmap_file xml_mmap;
    try
    {
      xml_mmap.open(xml_path);
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::file_open_failed, e.what(), xml_path, 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }

    if (! xml_mmap.is_open() || xml_mmap.empty())
    {
      auto err = error_info{processor_error::mmap_failed, fmt::format("mmap neuspešen: '{}'", xml_path), xml_path, 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }

    std::optional<fsp::mmap_file> xsd_mmap;
    if (! xsd_path.empty() && config_.validate_against_xsd)
    {
      xsd_mmap.emplace();
      try
      {
        xsd_mmap->open(xsd_path);
      }
      catch (const std::exception& e)
      {
        auto err = error_info{processor_error::file_open_failed, e.what(), xsd_path, 0};
        log_.error(err.to_string());
        return std::unexpected(err);
      }
    }
    return process_from_buffer(xml_mmap, xsd_mmap ? &*xsd_mmap : nullptr, gp);
  }

  //   1. setup_parser_no_validation() — SAX parser brez XSD overhead-a
  //   2. launch_validation_thread()   — validacija vzporedno v svoji niti
  //   3. handler_->set_validation_future() — handler dobi shared_future za
  //      polling; ob napaki vrže SAXParseException ki prekine parser_->parse()
  //   4. parser_->parse() — teče vzporedno z validacijsko nitjo (~16s)
  //   5. Po koncu parsinga: cancel() + join workerjev + get() na future
  //
  // Potek ob validacijski napaki:
  //   validacijska nit  →  promise.set_value(err)
  //   handler polling   →  wait_for(0) == ready → throw SAXParseException
  //   parser_->parse()  →  vrže izjemo → ujamemo v catch bloku spodaj
  //   cancel()          →  cancel_flag_ = true → workerji se ustavijo
  //   val_future.get()  →  vrnemo napako klicatelju
  void_result xml_processor::process_from_buffer(mmap_file& xml_mmap, mmap_file* xsd_mmap, const gr_pool_t& gp)
  {
    auto ps = setup_parser_no_validation();
    if (! ps) return std::unexpected(ps.error());

    try
    {
      handler_ = std::make_unique<Handler>(config_.targets, seg_queue_, log_, parser_.get(), xml_mmap.string_view());
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }
    active_mmap_ = &xml_mmap;
    // [DODANO] Zaženi validacijsko nit vzporedno s SAX parsingom.
    // Če XSD ni podan, launch_validation_thread() vrne future ki je takoj
    // razrešen z nullopt — handler polling bo vedno dobil "ok" in ne bo
    // povzročal overhead-a (wait_for na already-ready future je trivial).
    std::shared_future<std::optional<error_info>> val_future;
    if (xsd_mmap != nullptr && config_.validate_against_xsd)
    {
      val_future = launch_validation_thread(xml_mmap.string_view(), gp, std::string(xsd_mmap->path()));
      handler_->set_validation_future(val_future); // to receive signal, taht validation failed
    }
    bool validation_interrupted = false; // true: validation error; false: xml parsing error
    // starting workers
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
      if (log_info_) log_.info(fmt::format("SAX parsing finished. {} ms pending segments: {}", us, seg_queue_.size()));
    }
    catch (const xercesc::SAXParseException& e)
    {
      // [DODANO] Handler je vrgel SAXParseException kot signal za prekinitev
      // parsinga ob zaznani validacijski napaki. Napaka je že v val_future —
      // ne logiramo tukaj, logirala jo je validacijska nit.
      // Nastavimo zastavico da spodaj ne logiramo lažne "parse" napake.
      validation_interrupted = true;
      auto err               = error_info{
        processor_error::xsd_validation_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getLineNumber())};
      log_.error(err.to_string());
    }
    catch (const xercesc::XMLException& e)
    {
      // Prava napaka parsinga (ne validacijska) — canceliramo in vrnemo napako
      cancel();
      if (val_future.valid()) val_future.wait(); // počakamo nit pred return
      workers_.clear();
      active_mmap_ = nullptr;
      auto err = error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())};
      log_.error(err.to_string());

      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      cancel();
      if (val_future.valid()) val_future.wait();
      workers_.clear();
      active_mmap_ = nullptr;
      auto err     = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_.error(err.to_string());
      return std::unexpected(err);
    }

    // upon exception in validation thread, we need to exit and cancel the workers.
    // unless we do this, the worker would continue until the queue is nonempty, which is
    // useless, since the result is going to be dropped anyway
    if (validation_interrupted) cancel();
    else seg_queue_.set_finished();

    // std::this_thread::sleep_for(std::chrono::milliseconds(1)); // začasno

    log_.info(fmt::format("Before stopping workers - queue size: {}", seg_queue_.size()));
    // we need to wait the validation thread to finish before cleaning on our side
    if (val_future.valid()) val_future.wait();
    workers_.clear();
    active_mmap_ = nullptr;
    // [DODANO] Preverimo rezultat validacije.
    // get() na shared_future je varen ker:
    //   - validacijska nit je že končala (val_future.wait() zgoraj)
    //   - shared_future get() ne uniči vrednosti (za razliko od unique future)
    //   - kličemo ga samo z glavne niti
    if (val_future.valid())
    {
      auto val_result = val_future.get();
      if (val_result.has_value()) // error was logged in validation thread, just signal it up
        return std::unexpected(*val_result);
    }
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
      .active_workers      = workers_.size() > 0 ? workers_.size() : config_.num_workers,
      .processing_time_ms  = static_cast<double>(ms),
    };
  }
  // // ============================================================================
  // // Convenience function
  // // ============================================================================
  void xml_processor::process_one_file(const std::string&                  xml_path,
                                       const std::string&                  xsd_path,
                                       std::mutex&                         results_agg_mutex,
                                       std::vector<segment_result>&        all_results,
                                       std::vector<segment_result>&        all_errors,
                                       std::atomic<bool>&                  has_error,
                                       std::optional<error_info>&          first_error,
                                       const processor_config&             config,
                                       const fsp_logger&                   log, // Using auto to deduce the fsp::logger type
                                       const gr_pool_t&                    gp,
                                       [[maybe_unused]] std::latch&        gr_latch,
                                       [[maybe_unused]] std::atomic<bool>& gr_loaded,
                                       [[maybe_unused]] bool               have_grammar)
  {
    log.info(fmt::format("Processing file: '{}'", xml_path));

    // Each file gets its own processor instance to avoid state conflicts
    xml_processor file_proc(config, log.log_name());
    auto          res = file_proc.process_file(xml_path, xsd_path, gp);

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
      log.error(fmt::format("File {} failed: {}", xml_path, err.to_string()));
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
      log.info(fmt::format("File '{}' success (ok: {} err:{})", //
                           xml_path,
                           stats.successful_segments,
                           stats.failed_segments));
    }
  }
  // Helper static function for jthread execution
  void xml_processor::file_worker_task(lock_queue<std::string>&   file_queue,
                                       const std::string&         xsd_path,
                                       std::mutex&                results_agg_mutex,
                                       vec_seg_result&            all_results,
                                       vec_seg_result&            all_errors,
                                       std::atomic<std::size_t>&  file_processed,
                                       std::atomic<bool>&         has_error,
                                       std::optional<error_info>& first_error,
                                       std::size_t                worker_idx,
                                       const std::string&         parent_log_name,
                                       const processor_config&    config,
                                       const fsp_logger&          log,
                                       const gr_pool_t&           gp,
                                       std::latch&                gr_latch,
                                       std::atomic<bool>&         gr_loaded,
                                       bool                       has_grammar)
  {
    log.make_log_name(parent_log_name, fmt::format("doc<{:02}>", worker_idx));
    std::string xml_path;
    while (true)
    {
      if (auto opt = file_queue.try_pop())
        process_one_file(*opt,
                         xsd_path,
                         results_agg_mutex,
                         all_results,
                         all_errors,
                         has_error,
                         first_error,
                         config,
                         log,
                         gp,
                         gr_latch,
                         gr_loaded,
                         has_grammar);
      else
      {
        {
          if (file_queue.is_finished()) break; // end of job
          auto start = std::chrono::steady_clock::now();
          if (file_queue.pop(xml_path)) // new item arrived after while
          {
            auto end     = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            log.info(fmt::format("waiting time for new file {} ms.", elapsed));
            process_one_file(xml_path,
                             xsd_path,
                             results_agg_mutex,
                             all_results,
                             all_errors,
                             has_error,
                             first_error,
                             config,
                             log,
                             gp,
                             gr_latch,
                             gr_loaded,
                             has_grammar);
          }
          else break; // processing finished
        }
      }
      ++file_processed;
    }
  }

} // namespace fsp
