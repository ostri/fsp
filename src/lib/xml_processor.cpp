#include "xml_processor.hpp"
#include "x_str.hpp"
#include "xpath_helpers.hpp"

#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <stack>
#include <thread>
#include <libxml/xmlreader.h>

namespace fsp
{

  // ============================================================================
  // Logger
  // ============================================================================

  void xml_processor::setup_logger()
  {
    std::vector<spdlog::sink_ptr> sinks;
    log_thread_name = "main >";
    // Using the official spdlog alias ensures identical type matching across GCC and Clang
    if (config_.log_config.enable_console)
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

      // Use explicit unordered_map type instead of the missing custom_flags alias
      std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
      flags['*'] = std::make_unique<ThreadNameFormatter>();
      // Strict 4-argument constructor call for pattern_formatter
      auto formatter = std::make_unique<spdlog::pattern_formatter>( //
        "[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v",
        spdlog::pattern_time_type::local,
        spdlog::details::os::default_eol,
        std::move(flags));
      s->set_formatter(std::move(formatter));
      sinks.push_back(s);
    }

    if (config_.log_config.enable_file)
    {
      auto s = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config_.log_config.log_file_path, true);

      // Use explicit unordered_map type instead of the missing custom_flags alias
      std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
      flags['*'] = std::make_unique<ThreadNameFormatter>();

      auto formatter = std::make_unique<spdlog::pattern_formatter>( //
        "[%Y-%m-%d %H:%M:%S.%e] [%*] [%-5l] %v",
        spdlog::pattern_time_type::local,
        spdlog::details::os::default_eol,
        std::move(flags));
      s->set_formatter(std::move(formatter));
      sinks.push_back(s);
    }

    if (sinks.empty())
    {
      auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

      // Use explicit unordered_map type instead of the missing custom_flags alias
      std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
      flags['*'] = std::make_unique<ThreadNameFormatter>();

      auto formatter = std::make_unique<spdlog::pattern_formatter>( //
        "[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v",
        spdlog::pattern_time_type::local,
        spdlog::details::os::default_eol,
        std::move(flags));
      s->set_formatter(std::move(formatter));
      sinks.push_back(s);
    }

    logger_ = std::make_shared<spdlog::logger>(config_.log_config.logger_name, sinks.begin(), sinks.end());
    logger_->set_level(config_.log_config.log_level);
    logger_->flush_on(spdlog::level::err);
    logger_->flush();
    log_info("Main program initialized.");
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
  // Constructor / Destructor
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

  // [SPREMENJENO] Preimenovano iz setup_parser() v setup_parser_no_validation().
  // Odstranjeni so vsi XSD/validacijski featurji (fgSAX2CoreValidation,
  // fgXercesSchema, fgXercesValidationErrorAsFatal, fgXercesUseCachedGrammarInParse)
  // ker validacija zdaj teče v svoji niti z lastnim parserjem (launch_validation_thread).
  // fgCalculateSrcOfs mora ostati — Handler::endElement() ga potrebuje za byte offsete.
  void_result xml_processor::setup_parser_no_validation()
  {
    try
    {
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader());
      // NOLINTBEGIN(hicpp-no-array-decay)

      // [ODSTRANJENO] Validacijski featurji — zdaj v validacijski niti:
      // parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
      // parser_->setFeature(xercesc::XMLUni::fgXercesSchema, true);
      // parser_->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
      // parser_->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);

      // Eksplicitno izklopi validacijo — brez tega bi Xerces morda validiral
      // po defaultu če je grammar v cache-u
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);

      // Obvezno — Handler::endElement() kliče parser_->getSrcOffset()
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, true);
      // NOLINTEND(hicpp-no-array-decay)

      log_debug("Parser (no-validation) setup ok");
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
        // [OPOMBA] loadGrammar() se tukaj ne kliče več za SAX parser —
        // validacijska nit si zgradi lasten parser z lastnim grammarjem.
        // Ta metoda ostane za morebitno zunanjo rabo (setup_validation_from_buffer
        // je public API) in za xsd_holder_ lifecycle management.
        log_info(fmt::format("XSD schema: '{}' pripravljena.", schema_name));
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
  // [DODANO] Validacijska nit
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
    auto logger = logger_; // kopija shared_ptr — nit ima lastno referenco

    // std::async vrne future; .share() ga pretvori v shared_future ki ga
    // lahko get() pokličemo večkrat brez uničenja vrednosti
    return std::async(std::launch::async,
                      [xml_data, xml_size, xsd_data, xsd_size, xsd_path = std::move(xsd_path), logger]() -> std::optional<error_info>
                      {
                        log_thread_name = "valid>";
                        auto start      = std::chrono::steady_clock::now();
                        if (logger && logger->should_log(logger->level()))
                          logger->info(fmt::format("Validation started. file: xsd:{}", xsd_path));

                        try
                        {
                          // Lasten parser — xercesc::SAX2XMLReader ni thread-safe, ne smemo
                          // deliti parser_ iz glavne niti
                          std::unique_ptr<xercesc::SAX2XMLReader> vparser(xercesc::XMLReaderFactory::createXMLReader());

                          // NOLINTBEGIN(hicpp-no-array-decay)
                          vparser->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
                          vparser->setFeature(xercesc::XMLUni::fgXercesSchema, true);
                          vparser->setFeature(xercesc::XMLUni::fgXercesValidationErrorAsFatal, true);
                          vparser->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
                          vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
                          //                          vparser->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, false);
                          // NOLINTEND(hicpp-no-array-decay)

                          // Naloži XSD shemo v lasten parser
                          mem_buf_holder xsd_holder(xsd_data, xsd_size, xsd_path, logger);
                          if (xsd_holder.source() != nullptr)
                            vparser->loadGrammar(*xsd_holder.source(), xercesc::Grammar::SchemaGrammarType, true);

                          // DefaultHandler ki ob napaki takoj vrže — brez tega bi Xerces
                          // nadaljeval parsing kljub validacijski napaki
                          struct ThrowingErrorHandler : public xercesc::DefaultHandler
                          { // NOLINTBEGIN(cert-err60-cpp, hicpp-exception-baseclass)
                            void error(const xercesc::SAXParseException& e) override { throw e; }
                            void fatalError(const xercesc::SAXParseException& e) override { throw e; }
                            // NOLINTEND(cert-err60-cpp, hicpp-exception-baseclass)
                          } err_handler;
                          vparser->setErrorHandler(&err_handler);

                          xercesc::MemBufInputSource src(
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                            reinterpret_cast<const XMLByte*>(xml_data),
                            static_cast<XMLSize_t>(xml_size),
                            "xml_validation",
                            false);

                          vparser->parse(src);

                          if (logger && logger->should_log(logger->level()))
                          {
                            auto end      = std::chrono::steady_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                            logger->info(fmt::format("Validation finished in {} ms {} bytes.", duration.count(), xml_size));
                          }
                          return std::nullopt; // brez napak
                        }
                        catch (const xercesc::SAXParseException& e)
                        {
                          // ThrowingErrorHandler je vrgel ob prvi validacijski napaki.
                          // Ustavimo parsing (izjema je že prekinila vparser->parse())
                          // in sporočimo napako klicatelju.
                          auto err = error_info{processor_error::xsd_validation_failed,
                                                fmt::format("SAX parser error: {} (row:{} col:{})",
                                                            x_str(e.getMessage()).to_string(),
                                                            e.getLineNumber(),
                                                            e.getColumnNumber()),
                                                "",
                                                static_cast<std::size_t>(e.getLineNumber())};
                          if (logger) logger->error(err.to_string());
                          return err;
                        }
                        catch (const xercesc::XMLException& e)
                        {
                          auto err = error_info{
                            processor_error::xsd_validation_failed, fmt::format("XML error: {}", x_str(e.getMessage()).to_string()), "", 0};
                          if (logger) logger->error(err.to_string());
                          return err;
                        }
                        catch (...)
                        {
                          // Neznana izjema — ne smemo pustiti future v broken stanju
                          auto err = error_info{processor_error::internal_error, "Validation: unknown error", "", 0};
                          if (logger) logger->error(err.to_string());
                          return err;
                        }
                      })
      .share(); // .share() → shared_future
  }

  // ============================================================================
  // Worker
  // ============================================================================
  /*! process one xml fragment
   *  The method process one xml segment. It extracts the required values from xml
   *  and yields control to user provided function for further processing.
   *  upon the result from user supplied function it adjustes normal or error queue.
   */
  result<segment_result> xml_processor::process_segment( //
    const worker_context& ctx,
    const xml_segment&    seg)
  {
    segment_result res;
    res.segment_id  = seg.id();
    res.xpath_index = seg.subtree_type();

    auto t0 = std::chrono::steady_clock::now();

    try
    {
      if (ctx.logger && ctx.logger->should_log(ctx.logger->level()))
      {
        ctx.logger->debug(fmt::format("seg: {} started ", seg.dump()));
        ctx.logger->trace(fmt::format("{}", seg.dump_all(ctx.xml_mmap.data(), 0)));
      }
      auto view     = seg.view(ctx.xml_mmap.data());
      auto tmp_view = seg.subtree_str(view);
      auto r        = extract_xml_values(tmp_view, res, seg, ctx);

      auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();

      if (r)
      {
        res.success = true;
        if (ctx.logger)
          ctx.logger->debug("Segment '{}' DOM processing finished '{}'µs (offset={}, len={})", seg.id(), us, seg.offset(), seg.length());
        return res;
      }

      res.success       = false;
      res.error_message = fmt::format("Parse error: {}", r.error().message());
      if (ctx.logger) ctx.logger->warn("Segment {}: {} :: {}", seg.id(), res.error_message, seg.dump_all(ctx.xml_mmap.data()));
      return res;
    }
    catch (const std::exception& e)
    {
      res.success       = false;
      res.error_message = fmt::format("Exception in segment {}: '{}'", seg.id(), e.what());
      if (ctx.logger) ctx.logger->error("{}", res.error_message);
      return res;
    }
  }

  namespace
  {
    struct XmlTextReaderDeleter
    {
      void operator()(xmlTextReaderPtr reader) const
      {
        if (reader != nullptr) { xmlFreeTextReader(reader); }
      }
    };
    using UniqueXmlTextReader = std::unique_ptr<std::remove_pointer_t<xmlTextReaderPtr>, XmlTextReaderDeleter>;

    struct xml_node
    {
    public:
      xml_node() = default;
      xml_node(const char* uri, const char* tag)
      : uri_(uri)
      , tag_(tag)
      {
      }
      xml_node(cstr_t uri, cstr_t tag)
      : uri_(uri)
      , tag_(tag)
      {
      }
      [[nodiscard]] std::string uri() const { return uri_; }
      [[nodiscard]] std::string tag() const { return tag_; }
    private:
      std::string uri_;
      std::string tag_;
    };
  } // namespace
  // === extract_xml_values & Helpers ===================================================================


  namespace
  {
    struct p_limits
    {
      cstr_t      low_tag;
      cstr_t      high_tag;
      std::size_t low_ndx;
      std::size_t high_ndx;
    };
    using limits_vec = std::vector<p_limits>;

    static limits_vec prepare_limits(const fsp::xpath_node_struct& xpaths)
    {
      limits_vec vec;
      vec.reserve(xpaths.max_xpath_size());
      for (auto cnt = 0UL; cnt < xpaths.max_xpath_size(); cnt++)
      {
        // p_limits tmp;
        auto low  = xpaths.first_xpath_tag_name(cnt);
        auto high = xpaths.last_xpath_tag_name(cnt);
        auto tmp  = p_limits{.low_tag = low.first, .high_tag = high.first, .low_ndx = low.second, .high_ndx = high.second};
        vec.push_back(tmp);
      }
      return vec;
    }
    static bool process_and_prune_node(const auto&           reader,
                                       std::stack<xml_node>& stack,
                                       size_t&               depth,
                                       const limits_vec&     limits,
                                       const worker_context& ctx,
                                       int&                  read_status)
    {
      const char* uri = reinterpret_cast<const char*>(xmlTextReaderConstNamespaceUri(reader));
      const char* tag = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader));

      // Safely handle potential null pointers from libxml2
      std::string_view safe_uri = uri ? uri : "";
      std::string_view safe_tag = tag ? tag : "";

      if (depth >= limits.size())
      {
        if (ctx.logger && ctx.logger->should_log(ctx.logger->level()))
          ctx.logger->debug("pruning subtree: '{}' too deep: '{}' max allowed: '{}'", safe_tag, depth, limits.size());
        read_status = xmlTextReaderNext(reader); // Skip all children, move to sibling or the end of parent tag
        return true;                             // Indicates that a 'continue' should be executed in the outer loop
      }

      auto tmp  = xml_node{safe_uri, safe_tag};
      auto low  = limits[depth].low_tag;
      auto high = limits[depth].high_tag;
      if (ctx.logger && ctx.logger->should_log(ctx.logger->level()))
      {
        ctx.logger->trace("tag: '{:15}' uri: '{:30}' low: {} high: {}", safe_tag, safe_uri, low, high);
      }
      if ((depth >= limits.size()) || (tmp.tag() < low) || (tmp.tag() > high))
      {
        if (ctx.logger && ctx.logger->should_log(ctx.logger->level()))
          ctx.logger->debug("pruning subtree: '{}' min: '{}' max: '{}'", safe_tag, limits[depth].low_tag, limits[depth].high_tag);

        read_status = xmlTextReaderNext(reader); // Skip all children, move to sibling or the end of parent tag
        return true;                             // Indicates that a 'continue' should be executed in the outer loop
      }
      stack.emplace(tmp);
      // depth++;
      return false; // Indicates normal execution flow, no pruning happened
    }
    inline bool log(auto& log) { return log && log->should_log(log->level()); }
  } // namespace
  result<segment_result> xml_processor::extract_xml_values(cstr_t                xml_buf,
                                                           const segment_result& sr,
                                                           const xml_segment&    seg,
                                                           const worker_context& ctx)
  {
    auto res = sr;
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    auto flags = (XML_PARSE_NOCDATA | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS | XML_PARSE_NONET);

    UniqueXmlTextReader reader(xmlReaderForMemory(xml_buf.data(), static_cast<int>(xml_buf.size()), nullptr, nullptr, flags));

    int                  read_status;
    const char*          value = nullptr;
    auto                 depth = 0UL;
    const char           pad   = '.';
    std::stack<xml_node> stack;
    auto                 subtree_type = ctx.targets.targets[seg.subtree_type()].original_ndx(); // seg.subtree_type();
    const auto&          xpaths       = ctx.targets.xpaths.at(subtree_type);
    if (log(ctx.logger)) //
    {
      ctx.logger->trace(fmt::format("subtree type: {}", subtree_type));
      std::string msg;
      for (const auto& el : xpaths) msg += fmt::format("- {}\n", el.dump());
      ctx.logger->trace(fmt::format("\n{}", msg));
    }
    std::vector<bool> maybe_usefull(xpaths.size(), true); // xpaths that we still need values for
    assert(xpaths.size() != 0);
    // prepare pruning limits -----------------------------------------------------------------------
    limits_vec limits = prepare_limits(ctx.targets.xpaths.at(subtree_type));
    // ---------------------------------------------------------------------------------------------
    read_status = xmlTextReaderRead(reader.get());

    while (read_status == 1)
    {
      int  type      = xmlTextReaderNodeType(reader.get());
      auto enum_type = static_cast<xmlReaderTypes>(type);
      switch (enum_type)
      {
      case XML_READER_TYPE_ELEMENT:
      {
        if (process_and_prune_node(reader.get(), stack, depth, limits, ctx, read_status)) continue;
        // for (auto cnt = limits[depth].low_ndx; cnt <= limits[depth].high_ndx; cnt++)
        // { // walk over defined range on n-th xpath path (depth)
        //   if (stack.top().tag() != xpaths[cnt].xpath()[depth].tag)
        //   { // remove specific xpath from the search
        //     continue;
        //   }
        // }
        if (log(ctx.logger)) ctx.logger->debug(fmt::format("seg:{:5}{}{}", seg.id(), std::string(depth * 2, pad), stack.top().tag()));
        depth++;
        break;
      }
      case XML_READER_TYPE_TEXT:
      {
        value = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader.get()));
        if (log(ctx.logger)) ctx.logger->debug(fmt::format("seg:{:5}{}{}", seg.id(), std::string(depth * 2, pad), value));
        break;
      }
      case XML_READER_TYPE_END_ELEMENT:
      {
        depth--;
        if (log(ctx.logger)) ctx.logger->debug(fmt::format("seg:{:5}{}/{}", seg.id(), std::string(depth * 2, pad), stack.top().tag()));
        stack.pop();
        break;
      }
      case XML_READER_TYPE_NONE:
      case XML_READER_TYPE_ATTRIBUTE:
      case XML_READER_TYPE_CDATA:
      case XML_READER_TYPE_ENTITY_REFERENCE:
      case XML_READER_TYPE_ENTITY:
      case XML_READER_TYPE_PROCESSING_INSTRUCTION:
      case XML_READER_TYPE_COMMENT:
      case XML_READER_TYPE_DOCUMENT:
      case XML_READER_TYPE_DOCUMENT_TYPE:
      case XML_READER_TYPE_DOCUMENT_FRAGMENT:
      case XML_READER_TYPE_NOTATION:
      case XML_READER_TYPE_WHITESPACE:
      case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
      case XML_READER_TYPE_END_ENTITY:
      case XML_READER_TYPE_XML_DECLARATION: [[fallthrough]];
      default:
        auto type_name = magic_enum::enum_name(enum_type);
        if (log(ctx.logger)) ctx.logger->debug(fmt::format("nonprocessed: {} {}", type_name, type));
      }
      read_status = xmlTextReaderRead(reader.get());
    }
    res.success = read_status != -1;
    return res;
  }
  /**
   * @brief execution of worker thread
   *
   * @param st don't know TODO ostri ugotovi čemu to služi
   * @param worker_id unique id of the thread
   * @param ctx worker context
   */
  void xml_processor::worker_function( //
    [[maybe_unused]] const std::stop_token& st,
    int                                     worker_id,
    [[maybe_unused]] worker_context         ctx)
  {
    auto t0         = std::chrono::steady_clock::now();
    log_thread_name = fmt::format("wrk{:03}", worker_id);
    if (ctx.logger) ctx.logger->debug("Worker thread '{}' started.", log_thread_name);
    ctx.worker_id = worker_id;

    while (! ctx.cancel_flag.load())
    {
      xml_segment seg{};
      if (! ctx.seg_queue.pop(seg)) break;
      auto res = process_segment(ctx, seg);

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
        err_res.segment_id    = seg.id();
        err_res.xpath_index   = seg.subtree_type();
        err_res.success       = false;
        err_res.error_message = res.error().to_string();
        std::lock_guard lock(ctx.errors_mutex);
        ctx.errors.push_back(std::move(err_res));
        ctx.error_count++;
      }
    }
    if (ctx.logger && ctx.logger->should_log(ctx.logger->level()))
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      ctx.logger->debug("Worker thread '{}' finished in {} ms.", log_thread_name, duration);
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
      .worker_id       = -1,               // unique id of the worker
      .seg_queue       = seg_queue_,       // queue to send segmetns to workers
      .xml_mmap        = *active_mmap_,    // input/xml file or buffer
      .results         = results_,         // queue of results
      .errors          = errors_,          // queue of errors
      .results_mutex   = results_mutex_,   // mutex for queue of results
      .errors_mutex    = errors_mutex_,    // mutex for queue of errors
      .processed_count = processed_count_, // number of processed segmetns
      .error_count     = error_count_,     // number of error segments
      .cancel_flag     = cancel_flag_,     // are we cancellig the operation?
      .logger          = logger_,          // logger
      .targets         = config_.targets   // how to split the xml tree and whic values to find
    };

    workers_.reserve(config_.num_workers);
    for (std::size_t i = 0; i < config_.num_workers; ++i) workers_.emplace_back(worker_function, i, ctx);

    log_info(fmt::format("{} workers started.", config_.num_workers));
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

  // [SPREMENJENO] process_from_buffer — glavna sprememba v tej datoteki.
  // Prej: setup_parser() z validacijo + sinhrono parsiranje (26s).
  // Zdaj:
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

    // [DODANO] Zastavica: ali smo parser_->parse() prekinili zaradi validacije.
    // Ločimo validacijsko napako od prave parse napake.
    bool validation_interrupted = false;
    // starting workers
    auto ws = start_workers(); // validation is on critical path workers start last
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
      parser_->parse(src);
      log_debug("SAX parsing finished.");
    }
    catch (const xercesc::SAXParseException&)
    {
      // [DODANO] Handler je vrgel SAXParseException kot signal za prekinitev
      // parsinga ob zaznani validacijski napaki. Napaka je že v val_future —
      // ne logiramo tukaj, logirala jo je validacijska nit.
      // Nastavimo zastavico da spodaj ne logiramo lažne "parse" napake.
      validation_interrupted = true;
      log_debug("SAX parsing prekinjen — validacijska napaka.");
    }
    catch (const xercesc::XMLException& e)
    {
      // Prava napaka parsinga (ne validacijska) — canceliramo in vrnemo napako
      cancel();
      if (val_future.valid()) val_future.wait(); // počakamo nit pred return
      workers_.clear();
      active_mmap_ = nullptr;
      auto err = error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())};
      log_error(err);
      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      cancel();
      if (val_future.valid()) val_future.wait();
      workers_.clear();
      active_mmap_ = nullptr;
      auto err     = error_info{processor_error::parse_failed, e.what(), "", 0};
      log_error(err);
      return std::unexpected(err);
    }

    // [DODANO] Ob prekinitvi (validacijska napaka ali normalni konec) canceliramo
    // workerje. cancel() nastavi cancel_flag_ = true in pokliče set_finished()
    // na vrsti — workerji se ustavijo takoj ko pop() vrne false.
    // Brez tega bi workerji nadaljevali z obdelavo segmentov ki so že v vrsti,
    // kar bi bil nepotreben overhead pri validacijski napaki.
    if (validation_interrupted) { cancel(); }
    else
    {
      seg_queue_.set_finished();
    }

    // [DODANO] Počakamo validacijsko nit pred cleanup-om. val_future.wait() je
    // idempotenten — če je nit že končala (kar je verjetno pri 1M transakcijah),
    // se vrne takoj. Brez tega bi destruktor mem_buf_holder-ja v validacijski
    // niti tekel vzporedno z našim cleanup-om.
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
      if (val_result.has_value())
      {
        // Napaka je bila že logirana v validacijski niti — samo vrnemo
        return std::unexpected(*val_result);
      }
    }

    auto       stat = get_stats();
    const auto kilo = 1000;
    log_info(fmt::format("Processing time: {:.3f}sec workers:{} segments:{} (ok:{} err:{}) size:{} byte(s)",
                         stat.processing_time_ms / kilo, // converting from milisecond to seconds
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

  processing_result process_xml_file(const std::string&   xml_path,
                                     const std::string&   xsd_path,
                                     const proc_data&     proc_data,
                                     std::size_t          num_workers,
                                     const logger_config& log_cfg)
  {
    processor_config cfg;
    cfg.targets              = proc_data;
    cfg.num_workers          = num_workers;
    cfg.validate_against_xsd = ! xsd_path.empty();
    cfg.log_config           = log_cfg;

    xml_processor proc(cfg);
    auto          res = proc.process_file(xml_path, xsd_path);
    if (! res) return std::unexpected(res.error());

    return std::make_pair(proc.get_results(), proc.get_errors());
  }

} // namespace fsp
