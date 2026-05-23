#include "xml_processor.hpp"
#include "dom_parser.hpp"
#include "x_str.hpp"
#include "xpath_helpers.hpp"

#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>
#include <thread>

namespace fsp
{

  // ============================================================================
  // Logger
  // ============================================================================

  void xml_processor::setup_logger()
  {
    std::vector<spdlog::sink_ptr> sinks;

    if (config_.log_config.enable_console)
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      s->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
      sinks.push_back(s);
    }
    if (config_.log_config.enable_file)
    {
      auto s = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config_.log_config.log_file_path, true);
      s->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
      sinks.push_back(s);
    }
    if (sinks.empty())
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      s->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
      sinks.push_back(s);
    }

    logger_ = std::make_shared<spdlog::logger>(config_.log_config.logger_name, sinks.begin(), sinks.end());
    logger_->set_level(config_.log_config.log_level);
    logger_->flush_on(spdlog::level::err);
  }

  void xml_processor::log_error(const error_info& e)
  {
    if (logger_) logger_->error(e.to_string());
  }
  void xml_processor::log_info(const std::string& m)
  {
    if (logger_) logger_->info(m);
  }
  void xml_processor::log_debug(const std::string& m)
  {
    if (logger_) logger_->debug(m);
  }
  void xml_processor::log_warning(const std::string& m)
  {
    if (logger_) logger_->warn(m);
  }

  // ============================================================================
  // Konstrukcija / destrukcija
  // ============================================================================

  xml_processor::xml_processor(processor_config cfg)
  : config_(std::move(cfg))
  {
    setup_logger();

    if (config_.num_workers == 0) config_.num_workers = std::thread::hardware_concurrency();
    if (config_.num_workers == 0) config_.num_workers = 1;

    log_info(fmt::format("XML Processor: {} workers, validation: {}", config_.num_workers, config_.validate_against_xsd));
  }

  xml_processor::~xml_processor()
  {
    cancel();
    stop_workers();
    parser_.reset();
    if (logger_) logger_->flush();
  }

  // ============================================================================
  // Parser setup
  // ============================================================================

  void_result xml_processor::setup_parser()
  {
    try
    {
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader());
      // NOLINTBEGIN(hicpp-no-array-decay)
      // === KRITIČNO ZA DOMLocator + byte offset ===
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      if (config_.validate_against_xsd)
      {
        parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
        parser_->setFeature(xercesc::XMLUni::fgXercesSchema, true);
        parser_->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
        parser_->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
      }
      else
      {
        parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);
      }
      // Kritično za DOMLocator::getByteOffset()
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, true);
      // NOLINTEND(hicpp-no-array-decay)

      log_debug("Parser setup ok");
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Parser init: {}", x_str(e.getMessage()).to_string()), "", 0};
      log_error(err);
      return std::unexpected(err);
    }
  }

  void_result xml_processor::setup_validation(fsp::mmap_file& xsd_mmap)
  {
    if (! config_.validate_against_xsd || ! xsd_mmap.is_open()) return {};
    return setup_validation_from_buffer(xsd_mmap.data(), xsd_mmap.size(), xsd_mmap.path());
  }

  void_result xml_processor::setup_validation_from_buffer(const void* data, std::size_t size, std::string_view schema_name)
  {
    if (! config_.validate_against_xsd) return {};
    if (data == nullptr || size == 0)
    {
      auto err = error_info{processor_error::schema_not_found, "XSD buffer is empty.", "", 0};
      log_error(err);
      return std::unexpected(err);
    }
    try
    {
      xsd_holder_ = std::make_unique<mem_buf_holder>(data, size, schema_name, logger_);

      if (xsd_holder_->source() != nullptr)
      {
        parser_->loadGrammar(*xsd_holder_->source(), xercesc::Grammar::SchemaGrammarType, true);
        log_info(fmt::format("XSD schema '{}' loaded.", schema_name));
      }
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto err = error_info{processor_error::xsd_validation_failed, fmt::format("XSD load: {}", x_str(e.getMessage()).to_string()), "", 0};
      log_error(err);
      return std::unexpected(err);
    }
  }

  // ============================================================================
  // Worker
  // ============================================================================

  result<segment_result> xml_processor::process_segment(int                                    worker_id,
                                                        const xml_segment&                     seg,
                                                        const fsp::mmap_file&                  xml_mmap,
                                                        const std::shared_ptr<spdlog::logger>& logger,
                                                        [[maybe_unused]] dom_parser*           parser)
  {
    segment_result res;
    res.segment_id  = seg.get_id();
    res.xpath_index = seg.get_xpath_index();

    auto t0 = std::chrono::steady_clock::now();

    try
    {
      if (logger) logger->trace(fmt::format("WORKER: {}\n'{}'", worker_id, seg.dump(xml_mmap.data())));
      auto view     = seg.view(xml_mmap.data()); // whole xml subtree but the initial start tag
      auto tmp_view = seg.subtree_str(view);     // merging together initial start tag and the rest of the xml tree
      auto r        = parser->exec(tmp_view);    // make DOM document

      auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();

      if (r)
      { /// DOM parsing je ok r.value() vsebuje DOM drevo xercesc::DOMDocument*
        res.success = true;
        if (logger)
          logger->debug("Segment '{}' DOM processing '{}'µs (offset={}, len={})", seg.get_id(), us, seg.get_offset(), seg.get_length());
        return res;
      }

      res.success       = false;
      res.error_message = fmt::format("Parse error: {}", r.error().message);
      if (logger) logger->warn("Segment {}: {} :: {}", seg.get_id(), res.error_message, seg.dump(xml_mmap.data()));
      return res;
    }
    catch (const std::exception& e)
    {
      res.success       = false;
      res.error_message = fmt::format("Exception in segment {}: '{}'", seg.get_id(), e.what());
      if (logger) logger->error("{}", res.error_message);
      return res;
    }
  }

  void xml_processor::worker_function([[maybe_unused]] const std::stop_token& st, int worker_id, [[maybe_unused]] worker_context ctx)
  {
    if (ctx.logger) ctx.logger->debug(fmt::format("Worker {:02} established.", worker_id));
    auto parser = std::make_unique<dom_parser>(ctx.logger, worker_id);
    auto res    = parser->init();
    if (! res)
    {
      auto msg = fmt::format("Error initializing worker '{:02}' dom parser: '{}'", worker_id, res.error().message);
      if (ctx.logger)
      {
        ctx.logger->error(msg);
        ctx.logger->debug(fmt::format("Worker {:02} finished.", worker_id));
      }
      return;
    }
    while (! st.stop_requested() && ! ctx.cancel_flag.load())
    {
      xml_segment seg{};
      if (! ctx.seg_queue.pop(seg)) break;

      auto res = process_segment(worker_id, seg, ctx.xml_mmap, ctx.logger, parser.get());

      if (res)
      {
        if (res->success)
        {
          std::lock_guard lock(ctx.results_mutex);
          ctx.results.push_back(std::move(*res));
          ctx.processed_count++;
        }
        else
        {
          std::lock_guard lock(ctx.errors_mutex);
          ctx.errors.push_back(std::move(*res));
          ctx.error_count++;
        }
      }
      else
      {
        segment_result err_res;
        err_res.segment_id    = seg.get_id();
        err_res.xpath_index   = seg.get_xpath_index();
        err_res.success       = false;
        err_res.error_message = res.error().to_string();
        std::lock_guard lock(ctx.errors_mutex);
        ctx.errors.push_back(std::move(err_res));
        ctx.error_count++; // do we need errors per thread?
      }
    }
    auto x = parser->done();
    if (! x && ctx.logger) ctx.logger->error(fmt::format("Error releasing dom parser: '{}'", x.error().message));
    if (ctx.logger) ctx.logger->debug(fmt::format("Worker {:02} finished.", worker_id));
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
      log_error(err);
      return std::unexpected(err);
    }

    worker_context ctx{
      .seg_queue       = seg_queue_,
      .xml_mmap        = *active_mmap_,
      .results         = results_,
      .errors          = errors_,
      .results_mutex   = results_mutex_,
      .errors_mutex    = errors_mutex_,
      .processed_count = processed_count_,
      .error_count     = error_count_,
      .cancel_flag     = cancel_flag_,
      .logger          = logger_,
    };

    workers_.reserve(config_.num_workers);
    for (std::size_t i = 0; i < config_.num_workers; ++i) workers_.emplace_back(worker_function, i, ctx);

    log_debug(fmt::format("{} workers started.", config_.num_workers));
    return {};
  }

  void xml_processor::stop_workers()
  {
    seg_queue_.set_finished();
    workers_.clear();
    active_mmap_ = nullptr;
    log_debug("All workers stopped.");
  }

  void xml_processor::cancel()
  {
    cancel_flag_ = true;
    seg_queue_.set_finished();
  }

  // ============================================================================
  // Procesiranje
  // ============================================================================

  void_result xml_processor::process_file(const std::string& xml_path, const std::string& xsd_path)
  {
    start_time_ = std::chrono::steady_clock::now();
    log_info(fmt::format("XML file: '{}'", xml_path));

    fsp::mmap_file xml_mmap;
    try
    {
      xml_mmap.open(xml_path);
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::file_open_failed, e.what(), xml_path, 0};
      log_error(err);
      return std::unexpected(err);
    }

    if (! xml_mmap.is_open() || xml_mmap.empty())
    {
      auto err = error_info{processor_error::mmap_failed, fmt::format("mmap neuspešen: '{}'", xml_path), xml_path, 0};
      log_error(err);
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
        log_error(err);
        return std::unexpected(err);
      }
    }

    return process_from_buffer(xml_mmap, xsd_mmap ? &*xsd_mmap : nullptr);
  }

  void_result xml_processor::process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap)
  {
    auto ps = setup_parser();
    if (! ps) return std::unexpected(ps.error());

    if (xsd_mmap != nullptr)
    {
      auto vs = setup_validation(*xsd_mmap);
      if (! vs) return std::unexpected(vs.error());
    }

    try
    {
      handler_ = std::make_unique<Handler>(config_.targets, seg_queue_, logger_, parser_.get(), xml_mmap.view());
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      auto err = error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0};
      log_error(err);
      return std::unexpected(err);
    }

    // Nastavimo mmap referenco za workerje
    active_mmap_ = &xml_mmap;

    auto ws = start_workers();
    if (! ws) return std::unexpected(ws.error());

    try
    {
      log_debug("SAX parsing started.");
      xercesc::MemBufInputSource src(
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<const XMLByte*>(xml_mmap.data()),
        static_cast<XMLSize_t>(xml_mmap.size()),
        "xml_input",
        false);
      // // NOLINTNEXTLINE
      // log_info(std::string(reinterpret_cast<const char*>(xml_mmap.data()), xml_mmap.size()));
      parser_->parse(src);
      log_debug("SAX parsing finished.");
    }
    catch (const xercesc::XMLException& e)
    {
      cancel();
      auto err = error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())};
      log_error(err);
      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      cancel();
      auto err = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_error(err);
      return std::unexpected(err);
    }

    seg_queue_.set_finished();
    // Počakamo da workerji končajo
    workers_.clear();
    active_mmap_ = nullptr;

    // auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_).count();
    // log_info(fmt::format("Finished in {} ms {} bytes in document.", ms, xml_mmap.size()));
    auto stat = get_stats();
    log_info(fmt::format("Processing time:{:.2f} ms workers:{} segments:{} (ok:{} err:{}) bytes:{}",
                         stat.processing_time_ms,
                         stat.active_workers,
                         stat.total_segments,
                         stat.successful_segments,
                         stat.failed_segments,
                         xml_mmap.size()));

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

  processing_result process_xml_file(const std::string&              xml_path,
                                     const std::string&              xsd_path,
                                     const std::vector<std::string>& xpath_strings,
                                     std::size_t                     num_workers,
                                     const logger_config&            log_cfg)
  {
    std::vector<xpath_t> targets;
    targets.reserve(xpath_strings.size());
    for (const auto& s : xpath_strings)
    {
      auto r = xpath_helpers::from_string(s);
      if (! r) return std::unexpected(r.error());
      targets.push_back(std::move(*r));
    }

    processor_config cfg;
    cfg.targets              = std::move(targets);
    cfg.num_workers          = num_workers;
    cfg.validate_against_xsd = ! xsd_path.empty();
    cfg.log_config           = log_cfg;

    xml_processor proc(cfg);
    auto          res = proc.process_file(xml_path, xsd_path);
    if (! res) return std::unexpected(res.error());

    return std::make_pair(proc.get_results(), proc.get_errors());
  }

} // namespace fsp
