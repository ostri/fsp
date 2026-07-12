#include "xml_processor.hpp"
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
  : logger_(cfg.log_config)
  , config_(std::move(cfg))
  , parent_log_name_(std::move(parent_log_name))
  {
    bool first_time = logger_.log_name() == "unknown";
    if (first_time)
    {
      std::string_view build_type;
      if constexpr (is_release()) build_type = "release";
      else build_type = "debug";
      if (config_.num_workers == 0) config_.num_workers = std::thread::hardware_concurrency();
      if (config_.num_workers == 0) config_.num_workers = 1; // if statement above fails
      logger_.info(fmt::format("XML Processor: started: build type: {} -> {} workers, validation: {}",
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
    auto       stat = get_stats();
    const auto kilo = 1000;
    logger_.info(fmt::format("XML Processor: finished: {:.3f} sec segments:{} (ok:{} err:{})",
                             stat.processing_time_ms / kilo, // converting from milisecond to seconds
                             stat.total_segments,
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
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader());
      // NOLINTBEGIN(hicpp-no-array-decay)
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);   // must be false
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true); // we need offset
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);    // we need namespaces
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
      // NOLINTEND(hicpp-no-array-decay)

      logger_.debug("Parser (no-validation) setup ok");
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Parser init: {}", x_str(e.getMessage()).to_string()), "", 0};
      logger_.error(err.to_string());
      return std::unexpected(err);
    }
  }
  // ============================================================================
  // Validation thread
  // ============================================================================

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
    const void* xml_data,
    std::size_t xml_size,
    const void* xsd_data,
    std::size_t xsd_size,
    std::string xsd_path)
  {
    const auto& logger = logger_; // the thread has its own copy of logger
                                  // feature is converted to shared feature (see .share at the end),
                                  // so that we can read (get()) it more than onece
    return std::async(            //
             std::launch::async,
             [xml_data, xml_size, xsd_data, xsd_size, xsd_path = std::move(xsd_path), &logger]() -> std::optional<error_info>
             {
               log_thread_name   = "valid>";
               auto        start = std::chrono::steady_clock::now();
               const auto& log   = logger;
               if (log.active(info)) log.info(fmt::format("Validation started. file: xsd:{}", xsd_path));
               try
               {
                 // we need to create own parser, since it is not reentrant
                 std::unique_ptr<xercesc::SAX2XMLReader> vparser(xercesc::XMLReaderFactory::createXMLReader());
                 // NOLINTBEGIN(hicpp-no-array-decay)
                 vparser->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
                 vparser->setFeature(xercesc::XMLUni::fgXercesSchema, true);
                 vparser->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
                 vparser->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
                 vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
                 vparser->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
                 vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
                 vparser->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, false);
                 // NOLINTEND(hicpp-no-array-decay)
                 // load xsd schema to the parser
                 mem_buf_holder xsd_holder(xsd_data, xsd_size, xsd_path, log);
                 if (xsd_holder.source() != nullptr) //
                   vparser->loadGrammar(*xsd_holder.source(), xercesc::Grammar::SchemaGrammarType, true);
                 struct ThrowingErrorHandler : public xercesc::DefaultHandler // Handler that throws on error no matter what
                 {                                                            // NOLINTBEGIN(hicpp-exception-baseclass)
                   void error(const xercesc::SAXParseException& e) override { throw e; }
                   void fatalError(const xercesc::SAXParseException& e) override { throw e; }
                   // NOLINTEND(hicpp-exception-baseclass)
                 } err_handler;
                 vparser->setErrorHandler(&err_handler);

                 xercesc::MemBufInputSource src(
                   // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                   reinterpret_cast<const XMLByte*>(xml_data),
                   static_cast<XMLSize_t>(xml_size),
                   "xml_validation",
                   false);
                 vparser->parse(src);
                 if (log.active(fsp::lvl_enum::info))
                 {
                   auto end      = std::chrono::steady_clock::now();
                   auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                   log.info(fmt::format("Validation finished in {} ms {} bytes.", duration.count(), xml_size));
                 }
                 return std::nullopt; // no errors
               }
               catch (const xercesc::SAXParseException& e)
               { // there was a validaton error detected. signal & exit
                 auto err = error_info{
                   processor_error::xsd_validation_failed,
                   fmt::format(
                     "SAX validation error: {} (row:{} col:{})", x_str(e.getMessage()).to_string(), e.getLineNumber(), e.getColumnNumber()),
                   "",
                   static_cast<std::size_t>(e.getLineNumber())};
                 if (log.active(lvl_enum::err)) log.error(err.to_string());
                 return err;
               }
               catch (const xercesc::XMLException& e)
               { // there was xml syntax error: signal & exit
                 auto err = error_info{
                   processor_error::xsd_validation_failed, fmt::format("XML error: {}", x_str(e.getMessage()).to_string()), "", 0};
                 if (log.active(lvl_enum::err)) log.error(err.to_string());
                 return err;
               }
               catch (...)
               { // unknown error - we should not leave the feature in broken state
                 auto err = error_info{processor_error::internal_error, "Validation: unknown error", "", 0};
                 if (log.active(lvl_enum::err)) log.error(err.to_string());
                 return err;
               }
             })
      .share(); // .share() → shared_future
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
    processed_count_ = 0;
    error_count_     = 0;
    cancel_flag_     = false;

    if (active_mmap_ == nullptr)
    {
      auto err = error_info{processor_error::internal_error, "mmap is null before 'start_workers()'", active_mmap_->path(), 0};
      logger_.error(err.to_string());
      return std::unexpected(err);
    }

    for (std::size_t i = 0; i < config_.num_workers; ++i)
    {
      str_t parent_name = logger_.log_name();
      // logger_.info(fmt::format("DEBUG: start_workers parent for wrk{}: {}", i, parent_name)); // ← DODAJ
      workers_.emplace_back(xml_worker{seg_queue_,
                                       *active_mmap_,
                                       results_,
                                       errors_,
                                       results_mutex_,
                                       errors_mutex_,
                                       processed_count_,
                                       error_count_,
                                       cancel_flag_,
                                       logger_,
                                       config_.targets,
                                       parent_name},
                            i);
    }
    logger_.info(fmt::format("{} workers started.", config_.num_workers));
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
    logger_.info("All workers stopped.");
  }
  /**
   * @brief signal to workers to immediately finish with work
   *
   */
  void xml_processor::cancel()
  {
    cancel_flag_ = true;
    seg_queue_.set_finished();
  }
  /**
   * @brief parse xml file
   *
   * @param xml_path filepath to the xml file
   * @param xsd_path filepath to the corresponding grammas (XSD)
   * @return void_result
   */
  void_result xml_processor::process_file(const std::string& xml_path, const std::string& xsd_path)
  {
    start_time_ = std::chrono::steady_clock::now();
    logger_.info(fmt::format("XML file: '{}'", xml_path));

    fsp::mmap_file xml_mmap;
    try
    {
      xml_mmap.open(xml_path);
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::file_open_failed, e.what(), xml_path, 0};
      logger_.error(err.to_string());
      return std::unexpected(err);
    }

    if (! xml_mmap.is_open() || xml_mmap.empty())
    {
      auto err = error_info{processor_error::mmap_failed, fmt::format("mmap neuspešen: '{}'", xml_path), xml_path, 0};
      logger_.error(err.to_string());
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
        logger_.error(err.to_string());
        return std::unexpected(err);
      }
    }
    return process_from_buffer(xml_mmap, xsd_mmap ? &*xsd_mmap : nullptr);
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
  void_result xml_processor::process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap)
  {
    auto ps = setup_parser_no_validation();
    if (! ps) return std::unexpected(ps.error());

    try
    {
      handler_ = std::make_unique<Handler>(config_.targets, seg_queue_, logger_, parser_.get(), xml_mmap.string_view());
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0};
      logger_.error(err.to_string());
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
      val_future =
        launch_validation_thread(xml_mmap.data(), xml_mmap.size(), xsd_mmap->data(), xsd_mmap->size(), std::string(xsd_mmap->path()));
      handler_->set_validation_future(val_future); // to receive signal, taht validation failed
    }
    bool validation_interrupted = false; // true: validation error; false: xml parsing error
    // starting workers
    auto ws = start_workers(); // validation is on critical path workers start last
    if (! ws) return std::unexpected(ws.error());
    try
    {
      auto t0 = std::chrono::steady_clock::now();
      logger_.info("SAX parsing started.");
      xercesc::MemBufInputSource src(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const XMLByte*>(xml_mmap.data()),
        static_cast<XMLSize_t>(xml_mmap.size()),
        "xml_input",
        false);
      parser_->parse(src);
      auto us = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      logger_.info(fmt::format("SAX parsing finished. {} ms", us));
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
      logger_.error(err.to_string());
    }
    catch (const xercesc::XMLException& e)
    {
      // Prava napaka parsinga (ne validacijska) — canceliramo in vrnemo napako
      cancel();
      if (val_future.valid()) val_future.wait(); // počakamo nit pred return
      workers_.clear();
      active_mmap_ = nullptr;
      auto err = error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())};
      logger_.error(err.to_string());

      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      cancel();
      if (val_future.valid()) val_future.wait();
      workers_.clear();
      active_mmap_ = nullptr;
      auto err     = error_info{processor_error::parse_failed, e.what(), "", 0};
      logger_.error(err.to_string());
      return std::unexpected(err);
    }

    // upon exception in validation thread, we need to exit and cancel the workers.
    // unless we do this, the worker would continue until the queue is nonempty, which is
    // useless, since the result is going to be dropped anyway
    if (validation_interrupted) cancel();
    else seg_queue_.set_finished();
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
  std::vector<segment_result> xml_processor::get_results()
  {
    std::lock_guard lock(results_mutex_);
    return std::move(results_);
  }
  std::vector<segment_result> xml_processor::get_errors()
  {
    std::lock_guard lock(errors_mutex_);
    return std::move(errors_);
  }
  xml_processor::stats xml_processor::get_stats() const
  {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    return {
      .total_segments      = processed_count_.load() + error_count_.load(),
      .successful_segments = processed_count_.load(),
      .failed_segments     = error_count_.load(),
      .active_workers      = workers_.size() > 0 ? workers_.size() : config_.num_workers,
      .processing_time_ms  = static_cast<double>(ms),
    };
  }
  // ============================================================================
  // Convenience function
  // ============================================================================
  processing_result xml_processor::process_xml_file(const std::string&   xml_path,
                                                    const std::string&   xsd_path,
                                                    const proc_data&     proc_data,
                                                    std::size_t          num_workers,
                                                    const logger_config& log_cfg)
  {
    xml_processor proc({.targets              = proc_data, //
                        .num_workers          = num_workers,
                        .validate_against_xsd = ! xsd_path.empty(),
                        .log_config           = log_cfg},
                       "outer");
    auto          res = proc.process_file(xml_path, xsd_path);
    if (! res) return std::unexpected(res.error());
    return std::make_pair(proc.get_results(), proc.get_errors());
  }


  void_result xml_processor::process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path, std::size_t num_parallel)
  {
    logger_.make_log_name(">");
    if (xml_paths.empty())
    {
      logger_.info("No files to process.");
      return {};
    }

    if (num_parallel == 0)
    {
      num_parallel = std::thread::hardware_concurrency();
      if (num_parallel == 0) num_parallel = 1;
    }
    num_parallel = std::min(num_parallel, xml_paths.size());

    logger_.info(fmt::format(
      "Processing {} XML files with {} parallel workers. XSD: {}", xml_paths.size(), num_parallel, xsd_path.empty() ? "none" : xsd_path));

    start_time_ = std::chrono::steady_clock::now();

    // Queue for file paths
    lock_queue<std::string> file_queue;
    for (const auto& path : xml_paths)
    {
      file_queue.push(std::string(path)); // copy
    }
    file_queue.set_finished(); // after all files are communicated we need to signal that this is all
    // otherwise program hands on thread join
    std::vector<std::jthread>   file_workers;
    std::mutex                  results_agg_mutex;
    std::vector<segment_result> all_results;
    std::vector<segment_result> all_errors;
    std::atomic<std::size_t>    file_processed{0};
    std::atomic<bool>           has_error{false};
    std::optional<error_info>   first_error;

    // Start workers
    file_workers.reserve(num_parallel);
    for (std::size_t i = 0; i < num_parallel; ++i)
    {
      auto log_name = logger_.log_name();
      file_workers.emplace_back(
        [this, //
         &file_queue,
         &xsd_path,
         &results_agg_mutex,
         &all_results,
         &all_errors,
         &file_processed,
         &has_error,
         &first_error,
         i,
         log_name]()
        {
          const auto& log = logger_;
          log.make_log_name(log_name, fmt::format("ft{}", i));
          while (true)
          {
            std::string xml_path;
            log.info(fmt::format("Waiting for file ..."));
            if (! file_queue.pop(xml_path)) break; // queue finished
            log.info(fmt::format("Processing file: '{}'", xml_path));
            // Each file gets its own processor instance to avoid state conflicts
            xml_processor file_proc(config_, log.log_name());
            auto          res = file_proc.process_file(xml_path, xsd_path);
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
              auto fr = file_proc.get_results();
              auto fe = file_proc.get_errors();
              {
                std::lock_guard<std::mutex> lock(results_agg_mutex);
                all_results.insert(all_results.end(), std::make_move_iterator(fr.begin()), std::make_move_iterator(fr.end()));
                all_errors.insert(all_errors.end(), std::make_move_iterator(fe.begin()), std::make_move_iterator(fe.end()));
              }
              log.info(fmt::format("File '{}' success", xml_path));
            }
            ++file_processed;
          }
        });
    }

    // Wait for all workers
    for (auto& w : file_workers)
      if (w.joinable()) w.join();

    // Aggregate results into this instance
    {
      std::lock_guard lock(results_mutex_);
      results_ = std::move(all_results);
    }
    {
      std::lock_guard lock(errors_mutex_);
      errors_ = std::move(all_errors);
    }
    // update statistics
    processed_count_.store(results_.size(), std::memory_order_relaxed);
    error_count_.store(errors_.size(), std::memory_order_relaxed);
    if (has_error && first_error)
    {
      logger_.error(fmt::format("process_files failed with first error: {}", first_error->to_string()));
      return std::unexpected(*first_error);
    }

    success_ = true;
    logger_.info(fmt::format("Processed {} files successfully.", file_processed.load()));
    return {};
  }
} // namespace fsp
