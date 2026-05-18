#include "xml_processor.hpp"
#include "x_str.hpp"
#include "pugixml.hpp"
#include <thread>
#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace fsp
{

  // ============================================================================
  // xpath_helpers implementation
  // ============================================================================
  namespace
  {
    static result<e_tag> parse_e_tag(const std::string& etag_str)
    {
      e_tag  result;
      size_t colon_pos = etag_str.find(':');

      if (colon_pos == 0)
      {
        return std::unexpected(error_info{processor_error::invalid_xpath, fmt::format("Empty namespace in tag: '{}'", etag_str), "", 0});
      }

      if (colon_pos != std::string::npos)
      {
        result.set_ns(etag_str.substr(0, colon_pos));
        result.set_tag(etag_str.substr(colon_pos + 1));

        if (result.tag().empty())
        {
          return std::unexpected(
            error_info{processor_error::invalid_xpath, fmt::format("Empty tag name after namespace: '{}'", etag_str), "", 0});
        }
      }
      else
      {
        result.set_tag(etag_str);
      }

      if (result.tag().empty()) //
      {
        return std::unexpected(error_info{processor_error::invalid_xpath, "Empty tag name", "", 0});
      }

      return result;
    }
  } // namespace
  namespace xpath_helpers
  {
    result<xpath_t> from_string(const std::string& xpath_str)
    {
      xpath_t     result;
      std::string path = xpath_str;

      // Remove leading/trailing whitespace
      size_t start_pos = path.find_first_not_of(" \t\n\r");
      if (start_pos == std::string::npos)
      {
        return std::unexpected(error_info{processor_error::invalid_xpath, "Empty XPath string", "", 0});
      }
      path = path.substr(start_pos);

      size_t end_pos = path.find_last_not_of(" \t\n\r");
      if (end_pos != std::string::npos) path = path.substr(0, end_pos + 1);

      // Remove leading slash if present
      if (! path.empty() && path[0] == '/') path = path.substr(1);

      // Remove trailing slash if present
      if (! path.empty() && path.back() == '/') path.pop_back();

      if (path.empty())
      {
        return std::unexpected(
          error_info{processor_error::invalid_xpath, fmt::format("Empty XPath after normalization: '{}'", xpath_str), "", 0});
      }

      size_t start = 0;
      size_t end   = path.find('/');

      while (end != std::string::npos)
      {
        std::string etag_str = path.substr(start, end - start);
        if (! etag_str.empty())
        {
          auto tag_result = parse_e_tag(etag_str);
          if (! tag_result) return std::unexpected(tag_result.error());
          result.push_back(std::move(*tag_result));
        }
        start = end + 1;
        end   = path.find('/', start);
      }

      std::string last_etag = path.substr(start);
      if (! last_etag.empty())
      {
        auto tag_result = parse_e_tag(last_etag);
        if (! tag_result) return std::unexpected(tag_result.error());
        result.push_back(std::move(*tag_result));
      }

      if (result.empty())
      {
        return std::unexpected(
          error_info{processor_error::invalid_xpath, fmt::format("No valid tags found in XPath: '{}'", xpath_str), "", 0});
      }

      return result;
    }

    std::string to_string(const xpath_t& xpath)
    {
      if (xpath.empty()) return "";

      std::string result;
      for (const auto& et : xpath)
      {
        if (! result.empty()) result += "/";
        result += et.to_string();
      }
      return result;
    }

    bool validate(const xpath_t& xpath, std::string* error_msg)
    {
      if (xpath.empty())
      {
        if (error_msg->empty()) *error_msg = "XPath is empty";
        return false;
      }

      for (size_t i = 0; i < xpath.size(); ++i)
      {
        const auto& et = xpath[i];
        if (et.tag().empty())
        {
          if (nullptr != error_msg) *error_msg = fmt::format("Empty tag at position {}", i);
          return false;
        }

        if (et.tag().find_first_of(" \t\n\r<>") != std::string::npos)
        {
          if (nullptr != error_msg) *error_msg = fmt::format("Invalid characters in tag '{}' at position {}", et.tag(), i);
          return false;
        }
      }

      return true;
    }

    std::optional<e_tag> last_tag(const xpath_t& xpath)
    {
      if (xpath.empty()) return std::nullopt;
      return xpath.back();
    }

    size_t depth(const xpath_t& xpath) { return xpath.size(); }
  } // namespace xpath_helpers

  // ============================================================================
  // xml_processor implementation
  // ============================================================================

  void xml_processor::setup_logger()
  {
    std::vector<spdlog::sink_ptr> sinks;

    if (config_.log_config.enable_console)
    {
      auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
      sinks.push_back(console_sink);
    }

    if (config_.log_config.enable_file)
    {
      auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config_.log_config.log_file_path, true);
      file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");
      sinks.push_back(file_sink);
    }

    if (sinks.empty())
    {
      // Default to console if no sinks configured
      auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
      sinks.push_back(console_sink);
    }

    logger_ = std::make_shared<spdlog::logger>(config_.log_config.logger_name, sinks.begin(), sinks.end());
    logger_->set_level(config_.log_config.log_level);
    logger_->flush_on(spdlog::level::err);

    log_info(fmt::format("Logger initialized with level: {}", magic_enum::enum_name(config_.log_config.log_level)));
  }

  void xml_processor::log_error(const error_info& error)
  {
    if (logger_) logger_->error(error.to_string());
  }

  void xml_processor::log_info(const std::string& msg)
  {
    if (logger_) logger_->info(msg);
  }

  void xml_processor::log_debug(const std::string& msg)
  {
    if (logger_) logger_->debug(msg);
  }

  void xml_processor::log_warning(const std::string& msg)
  {
    if (logger_) logger_->warn(msg);
  }

  xml_processor::xml_processor(processor_config cfg)
  : config_(std::move(cfg))
  {
    setup_logger();

    if (config_.num_workers == 0)
    {
      config_.num_workers = std::thread::hardware_concurrency();
      log_debug(fmt::format("Auto-detected {} hardware threads", config_.num_workers));
    }

    if (config_.num_workers == 0)
    {
      config_.num_workers = 1;
      log_warning("Could not detect hardware concurrency, using 1 worker");
    }

    log_info(fmt::format("XML Processor initialized with {} workers, validation: {}", config_.num_workers, config_.validate_against_xsd));
  }

  xml_processor::~xml_processor()
  {
    log_info("Shutting down XML processor");
    cancel();
    stop_workers();

    if (logger_) logger_->flush();
  }

  void_result xml_processor::setup_parser()
  {
    try
    {
      log_debug("Creating XML parser");
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader());

      // NOLINTBEGIN(hicpp-no-array-decay)
      if (config_.validate_against_xsd)
      {
        parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
        parser_->setFeature(xercesc::XMLUni::fgXercesSchema, true);
        log_debug("XSD validation enabled");
      }

      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, true);
      // NOLINTEND(hicpp-no-array-decay)
      log_info("XML parser setup completed");
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto error = error_info{
        processor_error::internal_error, fmt::format("Failed to create XML parser: {}", x_str(e.getMessage()).to_string()), "", 0};
      log_error(error);
      return std::unexpected(error);
    }
    catch (const std::exception& e)
    {
      auto error = error_info{processor_error::internal_error, fmt::format("Failed to create XML parser: {}", e.what()), "", 0};
      log_error(error);
      return std::unexpected(error);
    }
  }

  void_result xml_processor::setup_validation(fsp::mmap_file& xsd_mmap)
  {
    if (! config_.validate_against_xsd || ! xsd_mmap.is_open()) return {};

    log_debug(fmt::format("Setting up validation with XSD ({} bytes)", xsd_mmap.size()));
    return setup_validation_from_buffer(xsd_mmap.data(), xsd_mmap.size());
  }

  void_result xml_processor::setup_validation_from_buffer(const void* data, size_t size)
  {
    if (! config_.validate_against_xsd) return {};

    if (data == nullptr || size == 0)
    {
      auto error = error_info{processor_error::schema_not_found, "XSD buffer is empty or null", "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    try
    {
      log_debug(fmt::format("Creating XSD buffer holder ({} bytes)", size));
      xsd_holder_ = std::make_unique<mem_buf_holder>(data, size, "schema.xsd", logger_);

      if (nullptr != xsd_holder_->source())
      {
        // NOLINTNEXTLINE(hicpp-no-array-decay)
        parser_->setProperty(xercesc::XMLUni::fgXercesSchemaExternalNoNameSpaceSchemaLocation, static_cast<void*>(xsd_holder_->source()));
        log_info("XSD schema successfully loaded and configured");
      }

      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto error =
        error_info{processor_error::xsd_validation_failed, fmt::format("Failed to load XSD: {}", x_str(e.getMessage()).to_string()), "", 0};
      log_error(error);
      return std::unexpected(error);
    }
  }

  result<segment_result> xml_processor::process_segment( //
    const xml_segment&                     seg,
    const std::shared_ptr<spdlog::logger>& logger)
  {
    segment_result result;
    result.segment_id  = seg.id;
    result.xpath_index = seg.xpath_index;

    auto start = std::chrono::steady_clock::now();

    try
    {
      pugi::xml_document     doc;
      pugi::xml_parse_result status = doc.load_string(seg.raw_content.c_str());

      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

      if (static_cast<int>(status) == pugi::status_ok)
      {
        result.success = true;
        if (logger) logger->debug("Segment {} processed successfully in {} μs", seg.id, elapsed.count());
        return result;
      }

      result.success       = false;
      result.error_message = fmt::format("Failed to parse XML segment: {}", status.description());
      if (logger) logger->warn("Segment {} failed: {}", seg.id, result.error_message);
      return result;
    }
    catch (const std::exception& e)
    {
      result.success       = false;
      result.error_message = fmt::format("Exception in segment {}: {}", seg.id, e.what());
      if (logger) logger->error("Segment {} exception: {}", seg.id, e.what());
      return result;
    }
  }

  void xml_processor::worker_function(const std::stop_token& st, worker_context ctx)
  {
    if (ctx.logger) ctx.logger->info("Worker thread started");

    while (! st.stop_requested() && ! ctx.cancel_flag.load())
    {
      xml_segment seg;
      if (! ctx.seg_queue.pop(seg)) break;

      auto process_result = process_segment(seg, ctx.logger);

      if (process_result)
      {
        if (process_result->success)
        {
          std::lock_guard<std::mutex> lock(ctx.results_mutex);
          ctx.results.push_back(std::move(*process_result));
          ctx.processed_count++;
        }
        else
        {
          std::lock_guard<std::mutex> lock(ctx.errors_mutex);
          ctx.errors.push_back(std::move(*process_result));
          ctx.error_count++;
        }
      }
      else
      {
        segment_result error_result;
        error_result.segment_id    = seg.id;
        error_result.xpath_index   = seg.xpath_index;
        error_result.success       = false;
        error_result.error_message = process_result.error().to_string();

        std::lock_guard<std::mutex> lock(ctx.errors_mutex);
        ctx.errors.push_back(std::move(error_result));
        ctx.error_count++;

        if (ctx.logger) ctx.logger->error("Unexpected error in worker: {}", process_result.error().to_string());
      }
    }

    if (ctx.logger) ctx.logger->info("Worker thread stopping");
  }

  void_result xml_processor::start_workers()
  {
    {
      std::lock_guard<std::mutex> lock(results_mutex_);
      results_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(errors_mutex_);
      errors_.clear();
    }
    processed_count_ = 0;
    error_count_     = 0;
    cancel_flag_     = false;

    log_info(fmt::format("Starting {} worker threads", config_.num_workers));

    worker_context ctx{.seg_queue       = seg_queue_,
                       .results         = results_,
                       .errors          = errors_,
                       .results_mutex   = results_mutex_,
                       .errors_mutex    = errors_mutex_,
                       .processed_count = processed_count_,
                       .error_count     = error_count_,
                       .cancel_flag     = cancel_flag_,
                       .logger          = logger_};

    workers_.reserve(config_.num_workers);
    for (size_t i = 0; i < config_.num_workers; ++i) { workers_.emplace_back(worker_function, ctx); }

    log_debug("All worker threads started");
    return {};
  }

  void xml_processor::stop_workers()
  {
    log_debug("Stopping workers");
    seg_queue_.set_finished();
    workers_.clear();
    log_info("All workers stopped");
  }

  void xml_processor::cancel()
  {
    log_warning("Processing cancelled by user request");
    cancel_flag_ = true;
    seg_queue_.set_finished();
  }

  void_result xml_processor::process_file(const std::string& xml_path, const std::string& xsd_path)
  {
    start_time_ = std::chrono::steady_clock::now();
    log_info(fmt::format("Processing XML file: '{}'", xml_path));

    fsp::mmap_file xml_mmap;
    try
    {
      xml_mmap.open(xml_path);
      log_debug(fmt::format("XML file mmapped: {} bytes", xml_mmap.size()));
    }
    catch (const std::exception& e)
    {
      auto error = error_info{processor_error::file_open_failed, e.what(), xml_path, 0};
      log_error(error);
      return std::unexpected(error);
    }

    if (! xml_mmap.is_open() || xml_mmap.empty())
    {
      auto error =
        error_info{processor_error::mmap_failed, fmt::format("Failed to mmap XML file '{}' or file is empty", xml_path), xml_path, 0};
      log_error(error);
      return std::unexpected(error);
    }

    std::optional<fsp::mmap_file> xsd_mmap;
    if (! xsd_path.empty() && config_.validate_against_xsd)
    {
      log_debug(fmt::format("Loading XSD file: '{}'", xsd_path));
      xsd_mmap.emplace();
      try
      {
        xsd_mmap->open(xsd_path);
        log_debug(fmt::format("XSD file mmapped: {} bytes", xsd_mmap->size()));
      }
      catch (const std::exception& e)
      {
        auto error = error_info{processor_error::file_open_failed, e.what(), xsd_path, 0};
        log_error(error);
        return std::unexpected(error);
      }

      if (! xsd_mmap->is_open() || xsd_mmap->empty())
      {
        auto error =
          error_info{processor_error::mmap_failed, fmt::format("Failed to mmap XSD file '{}' or file is empty", xsd_path), xsd_path, 0};
        log_error(error);
        return std::unexpected(error);
      }
    }

    return process_from_buffer(xml_mmap, xsd_mmap ? &*xsd_mmap : nullptr);
  }

  void_result xml_processor::process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap)
  {
    auto parser_setup = setup_parser();
    if (! parser_setup) return std::unexpected(parser_setup.error());

    if (nullptr != xsd_mmap)
    {
      auto validation_setup = setup_validation(*xsd_mmap);
      if (! validation_setup) return std::unexpected(validation_setup.error());
    }

    std::vector<std::string> target_strings;
    target_strings.reserve(config_.targets.size());
    for (const auto& xpath : config_.targets)
    {
      target_strings.push_back(xpath_helpers::to_string(xpath));
      log_debug(fmt::format("Target XPath: {}", xpath_helpers::to_string(xpath)));
    }

    try
    {
      handler_ = std::make_unique<Handler>(target_strings, seg_queue_);
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
      log_debug("SAX handler configured");
    }
    catch (const std::exception& e)
    {
      auto error = error_info{processor_error::internal_error, fmt::format("Failed to create SAX handler: {}", e.what()), "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    auto workers_started = start_workers();
    if (! workers_started) return workers_started;

    try
    {
      log_info("Starting XML parsing");
      xercesc::MemBufInputSource xml_source                  //
        (                                                    //
          reinterpret_cast<const XMLByte*>(xml_mmap.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          static_cast<XMLSize_t>(xml_mmap.size()),
          "xml_input",
          false);

      parser_->parse(xml_source);
      log_info("XML parsing completed");
    }
    catch (const xercesc::XMLException& e)
    {
      cancel();
      auto error = error_info{//
                              processor_error::parse_failed,
                              x_str(e.getMessage()).to_string(),
                              "",
                              static_cast<size_t>(e.getSrcLine())};
      log_error(error);
      return std::unexpected(error);
    }
    catch (const std::exception& e)
    {
      cancel();
      auto error = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    seg_queue_.set_finished();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_);
    log_info(fmt::format("Processing completed in {} ms", elapsed.count()));

    success_ = true;
    return {};
  }

  void_result xml_processor::process_buffer(const void* data, size_t size, const void* xsd_data, size_t xsd_size)
  {
    start_time_ = std::chrono::steady_clock::now();
    log_info(fmt::format("Processing XML from memory buffer ({} bytes)", size));

    if (data == nullptr || size == 0)
    {
      auto error = error_info{processor_error::xml_empty, "XML buffer is empty or null", "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    auto parser_setup = setup_parser();
    if (! parser_setup) return std::unexpected(parser_setup.error());

    if ((xsd_data != nullptr) && (xsd_size > 0))
    {
      auto validation_setup = setup_validation_from_buffer(xsd_data, xsd_size);
      if (! validation_setup) return std::unexpected(validation_setup.error());
    }

    std::vector<std::string> target_strings;
    target_strings.reserve(config_.targets.size());
    for (const auto& xpath : config_.targets) { target_strings.push_back(xpath_helpers::to_string(xpath)); }

    try
    {
      handler_ = std::make_unique<Handler>(target_strings, seg_queue_);
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      auto error = error_info{processor_error::internal_error, fmt::format("Failed to create SAX handler: {}", e.what()), "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    auto workers_started = start_workers();
    if (! workers_started) return workers_started;

    try
    {
      xercesc::MemBufInputSource xml_source(    //
        reinterpret_cast<const XMLByte*>(data), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        static_cast<XMLSize_t>(size),
        "memory_buffer",
        false);

      parser_->parse(xml_source);
    }
    catch (const xercesc::XMLException& e)
    {
      cancel();
      auto error = error_info{//
                              processor_error::parse_failed,
                              x_str(e.getMessage()).to_string(),
                              "",
                              static_cast<size_t>(e.getSrcLine())};
      log_error(error);
      return std::unexpected(error);
    }
    catch (const std::exception& e)
    {
      cancel();
      auto error = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_error(error);
      return std::unexpected(error);
    }

    seg_queue_.set_finished();

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_);
    log_info(fmt::format("Buffer processing completed in {} ms", elapsed.count()));

    success_ = true;
    return {};
  }

  std::vector<segment_result> xml_processor::get_results()
  {
    std::lock_guard<std::mutex> lock(results_mutex_);
    log_info(fmt::format("Returning {} results", results_.size()));
    return std::move(results_);
  }

  std::vector<segment_result> xml_processor::get_errors()
  {
    std::lock_guard<std::mutex> lock(errors_mutex_);
    log_warning(fmt::format("Returning {} errors", errors_.size()));
    return std::move(errors_);
  }

  xml_processor::stats xml_processor::get_stats() const
  {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time_);

    return stats{.total_segments      = processed_count_.load() + error_count_.load(),
                 .successful_segments = processed_count_.load(),
                 .failed_segments     = error_count_.load(),
                 .active_workers      = workers_.size(),
                 .processing_time_ms  = static_cast<double>(elapsed.count())};
  }

  processing_result process_xml_file(const std::string&              xml_path,
                                     const std::string&              xsd_path,
                                     const std::vector<std::string>& xpath_strings,
                                     size_t                          num_workers,
                                     const logger_config&            log_cfg)
  {
    std::vector<xpath_t> targets;
    targets.reserve(xpath_strings.size());

    for (const auto& xpath_str : xpath_strings)
    {
      auto xpath_result = xpath_helpers::from_string(xpath_str);
      if (! xpath_result) { return std::unexpected(xpath_result.error()); }
      targets.push_back(std::move(*xpath_result));
    }

    processor_config config;
    config.targets              = std::move(targets);
    config.num_workers          = num_workers;
    config.validate_against_xsd = ! xsd_path.empty();
    config.log_config           = log_cfg;

    xml_processor processor(config);

    auto result = processor.process_file(xml_path, xsd_path);
    if (! result) { return std::unexpected(result.error()); }

    auto stats  = processor.get_stats();
    auto logger = processor.get_logger();
    if (logger)
    {
      logger->info("Processing statistics: {} total segments, {} successful, {} failed in {:.2f} ms",
                   stats.total_segments,
                   stats.successful_segments,
                   stats.failed_segments,
                   stats.processing_time_ms);
    }

    return std::make_pair(processor.get_results(), processor.get_errors());
  }
} // namespace fsp