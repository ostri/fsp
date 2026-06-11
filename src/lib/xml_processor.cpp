#include "xml_processor.hpp"
#include "x_str.hpp"
#include "xpath_helpers.hpp"
#include "xml_node.hpp"
#include "xpath_limits.hpp"
#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <stack>
#include <thread>
#include <libxml/xmlreader.h>
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

  std::optional<std::string> get_attribute_value_ns(xmlTextReaderPtr   reader,
                                                    const std::string& local_name,
                                                    const std::string& namespace_uri)
  {
    xmlChar* value = xmlTextReaderGetAttributeNs( //
      reader,
      BAD_CAST local_name.c_str(),
      ! namespace_uri.empty() ? BAD_CAST namespace_uri.c_str() : nullptr);
    if (value == nullptr) return std::nullopt;
    std::string result(reinterpret_cast<char*>(value));
    xmlFree(value); // pointer must be released
    return result;
  }
  struct pp_result
  {
    fsp::p_limits limits;     // altered limits
    fsp::xml_node node;       // tree node just dealt with
    int           status{-1}; // status of the last libxml2 library operation
  };
  struct err_result
  {
    int         status; // status of last libxml2 operation
    std::string err;    // description of what is wrong
  };

  struct stack_struct
  {
    fsp::xml_node node;
    fsp::p_limits limits;
  };
  static std::string process_attribute(xmlTextReaderPtr reader, const auto& xp)
  {
    auto local_name = std::string(xp.attr_name());
    auto uri        = std::string(xp.attr_uri());
    auto value      = get_attribute_value_ns(reader, local_name, uri);
    if (value.has_value()) // non-null attribute
    {
      // if (log.active(spdlog::level::critical))
      //   log.critical(fmt::format("attribute {}:{} value: {} cnt:{}", local_name, uri, value.value(), ndx));
      return value.value();
    }
    throw std::runtime_error(fmt::format("attribute '{}:{}' has no value.", local_name, uri));
  }
  static int process_positive_xpath_element( //
    xmlTextReaderPtr       reader,
    const fsp::xml_attr&   xp,
    std::size_t            ndx,
    std::size_t            depth,
    const fsp::fsp_logger& log,
    fsp::segment_result&   seg_result)
  {
    if (xp.is_last(depth))
    {                   // we are at the end of the current xpath
      if (xp.is_attr()) // attribute xpath
      {
        auto value = process_attribute(reader, xp);
        seg_result.values[xp.name()].emplace_back(value);
        if (log.active(spdlog::level::debug))
          log.debug(fmt::format("attribute name: '{}' tag: {} value: '{}'", xp.name(), xp.attr_name(), value));
        return -1;
      }
      return static_cast<int>(ndx);
    }
    return -1;
  }

  static std::expected<pp_result, err_result> process_and_prune_node( //
    const auto&                   reader,
    const fsp::xpath_node_struct& xpaths,
    std::stack<stack_struct>&     stack,
    const fsp::xpath_limits&      limits_vec,
    const fsp::fsp_logger&        log,
    fsp::segment_result&          seg_result)
  {
    int         read_status = 0;
    const char* uri         = reinterpret_cast<const char*>(xmlTextReaderConstNamespaceUri(reader));
    const char* tag         = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader));

    // Safely handle potential null pointers from libxml2
    std::string_view safe_uri = uri ? uri : "";
    std::string_view safe_tag = tag ? tag : "";
    auto             depth    = stack.size() - 1; // first available on stack a
    pp_result        result;
    if (depth >= stack.size()) // guard to not go too deep
    {
      if (log.active(spdlog::level::trace))
        log.trace(fmt::format("pruning subtree: '{}' too deep: '{}' max allowed: '{}'", safe_tag, depth, stack.size()));
      read_status = xmlTextReaderNext(reader); // Skip all children, move to sibling or the end of parent tag
      return std::unexpected(err_result{.status = read_status, .err = "Pruning, since it is too deep."});
    }
    auto limits = limits_vec[depth] & stack.top().limits; // xpaths excluded in previous level are excluded
                                                          // also on current level

    result.node = fsp::xml_node{safe_uri, safe_tag};
    // auto low         = limits.low_tag();
    // auto high        = limits.high_tag();
    if (log.active(spdlog::level::trace)) //
      log.trace(fmt::format("current tag: {} {} limits: {}", safe_tag, safe_uri, limits.dump()));
    /// prune if we are:
    /// - deeper than any xpath we are searching for (excluded before)
    /// - the tag name is smaller than any available tag name in the list of xpaths we are searching for
    /// - the tag name is bigger than any available tag name in the list of xpaths we are searching for
    auto first = limits.first();
    auto last  = limits.last();
    //
    // std::string_view first_tag = xpaths[first].xpath()[depth].tag;
    // std::string_view last_tag  = xpaths[last].xpath()[depth].tag;
    // if ((safe_tag < first_tag))
    //   return std::unexpected(err_result{.status = xmlTextReaderNext(reader), .err = "Pruning, smaller than first xpath searched"});
    // if ((safe_tag > last_tag))
    //   return std::unexpected(err_result{.status = xmlTextReaderNext(reader), .err = "Pruning, grather than last xpath searched"});
    // bool at_least_one = false;
    // std::size_t result_value_ndx = -1; // -1 no value, 0..n index of the result value
    if (log.active(spdlog::level::trace)) log.trace(fmt::format("{}", xpaths.dump()));
    // bool at_least_once = false;
    for (auto cnt : std::views::iota(first, last + 1))
    {                                          // compare with all possible options on xpath[depth]
      if (! limits.available()[cnt]) continue; // It has been removed in earlier iterations
      const auto& xp = xpaths[cnt];
      if (depth >= xp.xpath().size())
      { // if there is shorter xpath then exclude current path and move to next xpath
        if (log.active(spdlog::level::trace))
          log.trace(fmt::format("shorter xpath: tag:{} depth:{} cnt: {} xpath-id:{}", safe_tag, depth, cnt, xp.name()));
        limits.available().reset(cnt);
        continue;
      }
      std::string_view xp_tag = xp.xpath()[depth].tag;
      std::string_view xp_uri = xp.xpath()[depth].ns;
      if (log.active(spdlog::level::trace)) log.trace(fmt::format("tag:{} xp tag:{} depth:{} cnt: {}", safe_tag, xp_tag, depth, cnt));
      if ((safe_tag == xp_tag) && (safe_uri == xp_uri))
      { // let's remember the tag value index, attribute is handled inside
        result.status = std::max(process_positive_xpath_element(reader, xp, cnt, depth, log, seg_result), result.status);
      }
      else
      { // current xpath does not match the tags read
        limits.reset(cnt);
      }
    } // for
    if (log.active(spdlog::level::trace)) log.trace(fmt::format("limits: {}", limits.dump()));
    if (limits.available().none())      // this subtree is an dead end. prune it
      return std::unexpected(err_result{//
                                        .status = xmlTextReaderNext(reader),
                                        .err    = fmt::format("Pruning, in the middle '{}'", safe_tag)});
    if (result.status != -1 && ! xpaths[result.status].is_array())
      limits.reset(result.status); // we have this value and not searching in the future
    result.limits = limits;
    stack.emplace(result.node, result.limits);
    return result; // Indicates normal execution flow, no pruning happened
  }
} // namespace
namespace fsp
{

  // ============================================================================
  // Logger
  // ============================================================================

  // void xml_processor::setup_logger()
  // {
  //   std::vector<spdlog::sink_ptr> sinks;
  //   log_thread_name = "main >";
  //   // Using the official spdlog alias ensures identical type matching across GCC and Clang
  //   if (config_.log_config.enable_console)
  //   {
  //     auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  //     // Use explicit unordered_map type instead of the missing custom_flags alias
  //     std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
  //     flags['*'] = std::make_unique<ThreadNameFormatter>();
  //     // Strict 4-argument constructor call for pattern_formatter
  //     auto formatter = std::make_unique<spdlog::pattern_formatter>( //
  //       "[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v",
  //       spdlog::pattern_time_type::local,
  //       spdlog::details::os::default_eol,
  //       std::move(flags));
  //     s->set_formatter(std::move(formatter));
  //     sinks.push_back(s);
  //   }

  //   if (config_.log_config.enable_file)
  //   {
  //     auto s = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config_.log_config.log_file_path, true);

  //     // Use explicit unordered_map type instead of the missing custom_flags alias
  //     std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
  //     flags['*'] = std::make_unique<ThreadNameFormatter>();

  //     auto formatter = std::make_unique<spdlog::pattern_formatter>( //
  //       "[%Y-%m-%d %H:%M:%S.%e] [%*] [%-5l] %v",
  //       spdlog::pattern_time_type::local,
  //       spdlog::details::os::default_eol,
  //       std::move(flags));
  //     s->set_formatter(std::move(formatter));
  //     sinks.push_back(s);
  //   }

  //   if (sinks.empty())
  //   {
  //     auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

  //     // Use explicit unordered_map type instead of the missing custom_flags alias
  //     std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
  //     flags['*'] = std::make_unique<ThreadNameFormatter>();

  //     auto formatter = std::make_unique<spdlog::pattern_formatter>( //
  //       "[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v",
  //       spdlog::pattern_time_type::local,
  //       spdlog::details::os::default_eol,
  //       std::move(flags));
  //     s->set_formatter(std::move(formatter));
  //     sinks.push_back(s);
  //   }

  //   logger_ = std::make_shared<spdlog::logger>(config_.log_config.logger_name, sinks.begin(), sinks.end());
  //   logger_->set_level(config_.log_config.log_level);
  //   logger_->flush_on(spdlog::level::err);
  //   logger_->flush();
  //   log_info("Main program initialized.");
  // }

  // void xml_processor::log_critical(const error_info& e) { logger_.critical(e); }
  // void xml_processor::log_error(const error_info& e) { logger_.error(e); }
  // void xml_processor::log_critical(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::critical)) [[unlikely]]
  //     logger_.critical(m);
  // }
  // void xml_processor::log_error(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::err)) [[unlikely]]
  //     logger_.error(m);
  // }
  // void xml_processor::log_info(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::info)) [[unlikely]]
  //     logger_.info(m);
  // }
  // void xml_processor::log_warning(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::warn)) [[unlikely]]
  //     logger_.warning(m);
  // }
  // void xml_processor::log_debug(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::debug)) [[unlikely]]
  //     logger_.debug(m);
  // }
  // void xml_processor::log_trace(const std::string& m)
  // {
  //   if (logger_.active(spdlog::level::trace)) [[unlikely]]
  //     logger_.trace(m);
  // }

  // ============================================================================
  // Constructor / Destructor
  // ============================================================================

  xml_processor::xml_processor(processor_config cfg)
  : logger_(cfg.log_config)
  , config_(std::move(cfg))
  {
    logger_.info("Main program initialized.");
    if (config_.num_workers == 0) config_.num_workers = std::thread::hardware_concurrency();
    if (config_.num_workers == 0) config_.num_workers = 1;
    logger_.info(fmt::format("XML Processor: {} workers, validation: {}", config_.num_workers, config_.validate_against_xsd));
  }

  xml_processor::~xml_processor()
  {
    cancel();
    stop_workers();
    parser_.reset();
    //    if (logger_.get()) logger_.get()->flush();
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
      // Eksplicitno izklopi validacijo — brez tega bi Xerces morda validiral
      // po defaultu če je grammar v cache-u
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);

      // Obvezno — Handler::endElement() kliče parser_->getSrcOffset()
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, true);
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

  void_result xml_processor::setup_validation(fsp::mmap_file& xsd_mmap)
  {
    if (! config_.validate_against_xsd || ! xsd_mmap.is_open()) return {};
    return setup_validation_from_buffer(xsd_mmap.data(), xsd_mmap.size(), xsd_mmap.path());
  }
  // FIXME we dont need this any more
  void_result xml_processor::setup_validation_from_buffer(const void* data, std::size_t size, std::string_view schema_name)
  {
    if (! config_.validate_against_xsd) return {};
    if (data == nullptr || size == 0)
    {
      auto err = error_info{processor_error::schema_not_found, "XSD buffer is empty.", "", 0};
      logger_.error(err.to_string());
      return std::unexpected(err);
    }
    try
    {
      xsd_holder_ = std::make_unique<mem_buf_holder>(data, size, schema_name, logger_.get());

      if (xsd_holder_->source() != nullptr)
      {
        // [OPOMBA] loadGrammar() se tukaj ne kliče več za SAX parser —
        // validacijska nit si zgradi lasten parser z lastnim grammarjem.
        // Ta metoda ostane za morebitno zunanjo rabo (setup_validation_from_buffer
        // je public API) in za xsd_holder_ lifecycle management.
        logger_.info(fmt::format("XSD schema: '{}' pripravljena.", schema_name));
      }
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      auto err = error_info{processor_error::xsd_validation_failed, fmt::format("XSD load: {}", x_str(e.getMessage()).to_string()), "", 0};
      logger_.error(err.to_string());
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
    auto logger = logger_.get(); // kopija shared_ptr — nit ima lastno referenco

    // std::async vrne future; .share() ga pretvori v shared_future ki ga
    // lahko get() pokličemo večkrat brez uničenja vrednosti
    return std::async(std::launch::async,
                      [xml_data, xml_size, xsd_data, xsd_size, xsd_path = std::move(xsd_path), logger]() -> std::optional<error_info>
                      {
                        log_thread_name = "valid>";
                        auto  start     = std::chrono::steady_clock::now();
                        auto* log       = logger.get();
                        if (log) log->info(fmt::format("Validation started. file: xsd:{}", xsd_path));

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
                          vparser->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
                          vparser->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
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

                          if (log && log->should_log(spdlog::level::info))
                          {
                            auto end      = std::chrono::steady_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                            log->info(fmt::format("Validation finished in {} ms {} bytes.", duration.count(), xml_size));
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
                          if (log) log->error(err.to_string());
                          return err;
                        }
                        catch (const xercesc::XMLException& e)
                        {
                          auto err = error_info{
                            processor_error::xsd_validation_failed, fmt::format("XML error: {}", x_str(e.getMessage()).to_string()), "", 0};
                          if (log) log->error(err.to_string());
                          return err;
                        }
                        catch (...)
                        {
                          // Neznana izjema — ne smemo pustiti future v broken stanju
                          auto err = error_info{processor_error::internal_error, "Validation: unknown error", "", 0};
                          if (log) log->error(err.to_string());
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
  result<segment_result> xml_processor::process_segment(const worker_context& ctx, const xml_segment& seg)
  {
    segment_result res;
    res.segment_id  = seg.id();
    res.xpath_index = seg.subtree_type();
    // const auto& logger = ctx.logger;
    const auto& log = ctx.log;

    auto t0 = std::chrono::steady_clock::now();
    try
    {
      if (log.active(spdlog::level::debug))
      {
        log.debug(fmt::format("segment started: {}", seg.dump()));
        log.trace(fmt::format("{}", seg.dump_all(ctx.xml_mmap.data(), 0)));
      }
      auto view     = seg.view(ctx.xml_mmap.data());
      auto tmp_view = seg.subtree_str(view);
      auto r        = extract_xml_values(tmp_view, seg, ctx);

      auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();

      if (r)
      {
        // res.success = true;
        if (log.active(spdlog::level::debug))
        {
          log.debug(fmt::format("Segment '{}' DOM processing finished '{}'µs (offset={}, len={})", //
                                seg.id(),
                                us,
                                seg.offset(),
                                seg.length()));
          log.debug(fmt::format("{}", r->dump()));
        }
        return res;
      }

      // res.success       = false;
      error_info err( //
        processor_error::error_extracting_xpath_values,
        fmt::format("Error extracting xpath values: {}", r.error().message()),
        "",
        0UL);
      if (log.active(spdlog::level::warn))
        log.warning(fmt::format("Segment {}: {} :: {}", seg.id(), r.error().message(), seg.dump_all(ctx.xml_mmap.data())));
      return res;
    }
    catch (const std::exception& e)
    {
      // res.success       = false;
      auto error_message = fmt::format("Exception in segment {}: '{}'", seg.id(), e.what());
      if (log.active(spdlog::level::err)) log.error(fmt::format("{}", error_message));
      return res;
    }
  }


  result<segment_result> xml_processor::extract_xml_values(cstr_t xml_buf, const xml_segment& seg, const worker_context& ctx)
  {
    segment_result res;
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    auto flags = (XML_PARSE_NOCDATA | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS | XML_PARSE_NONET);

    UniqueXmlTextReader reader(xmlReaderForMemory(xml_buf.data(), static_cast<int>(xml_buf.size()), nullptr, nullptr, flags));

    int        read_status;
    auto       depth = 0UL;
    const char pad   = '.';
    // const auto&              logger = ctx.logger;
    const auto&              log = ctx.log;
    std::stack<stack_struct> tree_stack;
    auto                     subtree_type = ctx.targets.targets[seg.subtree_type()].original_ndx(); // seg.subtree_type();
    const auto&              xpaths       = ctx.targets.xpaths.at(subtree_type);
    if (log.active(spdlog::level::trace)) log.trace(fmt::format("subtree type: {}\n{}", subtree_type, xpaths.dump()));
    assert(xpaths.size() != 0);
    const auto& limits = xpath_limits(ctx.targets.xpaths.at(subtree_type));
    if (log.active(spdlog::level::trace)) log.trace(limits.dump());
    // ---------------------------------------------------------------------------------------------
    read_status = xmlTextReaderRead(reader.get());
    tree_stack.emplace(stack_struct{.node = xml_node{"top", "top_uri"}, .limits = fsp::p_limits(0, xpaths.size())});
    int value_ndx = -1; // xpath index of the value
    while (read_status == 1)
    {
      int  type      = xmlTextReaderNodeType(reader.get());
      auto enum_type = static_cast<xmlReaderTypes>(type);
      depth          = tree_stack.size() - 1;
      switch (enum_type)
      {
      case XML_READER_TYPE_ELEMENT:
      {
        auto x = process_and_prune_node(reader.get(), xpaths, tree_stack, limits, log, res);
        if (! x)
        { // FIXME too deep is critical error handle it properly
          if (log.active(spdlog::level::trace)) log.trace(fmt::format("pruning: {} val:{}", x.error().err, tree_stack.top().node.tag()));
          read_status = x.error().status;
          continue;
        }
        value_ndx = x.value().status;
        if (log.active(spdlog::level::debug))
          log.debug(
            fmt::format("seg:{:5} {}{} value_ndx: {}", seg.id(), std::string(depth * 2, pad), tree_stack.top().node.tag(), value_ndx));
        break;
      }
      case XML_READER_TYPE_TEXT:
      { // obtain values
        if (value_ndx != -1)
        { // we have value that we need to remember
          const auto* value      = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader.get()));
          cstr_t      value_name = xpaths[value_ndx].name();
          res.values[value_name].emplace_back(value != nullptr ? value : "");
          value_ndx = -1; // again undefined
          if (log.active(spdlog::level::debug))
            log.debug(fmt::format("seg:{:5} {}name: {} tag:'{}' value: {}",
                                  seg.id(),
                                  std::string(depth * 2, pad),
                                  value_name,
                                  tree_stack.top().node.tag(),
                                  value));
        }
        else if (log.active(spdlog::level::debug))
          log.debug(fmt::format("seg:{:5} {} tag: {} no value", seg.id(), std::string(depth * 2, pad), tree_stack.top().node.tag()));
        break;
      }
      case XML_READER_TYPE_END_ELEMENT:
      {
        if (log.active(spdlog::level::debug)) // -2 to align with start element
          log.debug(fmt::format("seg:{:5} {}/{}", seg.id(), std::string((tree_stack.size() - 2) * 2, pad), tree_stack.top().node.tag()));
        tree_stack.pop();
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
        if (log.active(spdlog::level::critical)) log.critical(fmt::format("nonsupported: {} {}", type_name, type));
      }
      read_status = xmlTextReaderRead(reader.get());
    }
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
    worker_context                          ctx)
  {
    auto t0         = std::chrono::steady_clock::now();
    log_thread_name = fmt::format("wrk{:03}", worker_id);
    // const auto& logger = ctx.logger;
    const auto& log = ctx.log;

    if (log.active(spdlog::level::debug)) log.debug(fmt::format("Worker thread '{}' started.", log_thread_name));
    ctx.worker_id                          = worker_id;
    thread_local std::size_t txn_processed = 0;
    while (! ctx.cancel_flag.load())
    {
      xml_segment seg{};
      if (! ctx.seg_queue.pop(seg)) break;
      auto res = process_segment(ctx, seg);
      txn_processed++;
      if (res)
      {
        if (res)
        { // everything is normal
          std::lock_guard lock(ctx.results_mutex);
          ctx.results.push_back(std::move(*res));
          ctx.processed_count++;
        }
        else
        { // thre were error(s)
          std::lock_guard lock(ctx.errors_mutex);
          ctx.errors.push_back(std::move(*res));
          ctx.error_count++;
        }
      }
      else
      {
        segment_result err_res;
        err_res.segment_id  = seg.id();
        err_res.xpath_index = seg.subtree_type();
        // err_res.success       = false;
        // err_res.error_message = res.error().to_string();
        std::lock_guard lock(ctx.errors_mutex);
        ctx.errors.push_back(std::move(err_res));
        ctx.error_count++;
      }
    }
    if (log.active(spdlog::level::debug))
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      log.debug(fmt::format("Worker thread '{}' finished in {} ms txn processed: {}.", log_thread_name, duration, txn_processed));
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
      logger_.error(err.to_string());
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
                                           //      .logger          = logger_.get(),    // logger
      .log     = logger_,
      .targets = config_.targets // how to split the xml tree and whic values to find
    };

    workers_.reserve(config_.num_workers);
    for (std::size_t i = 0; i < config_.num_workers; ++i) workers_.emplace_back(worker_function, i, ctx);

    logger_.info(fmt::format("{} workers started.", config_.num_workers));
    return {};
  }

  void xml_processor::stop_workers()
  {
    seg_queue_.set_finished();
    workers_.clear();
    active_mmap_ = nullptr;
    logger_.info("All workers stopped.");
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
      handler_ = std::make_unique<Handler>(config_.targets, seg_queue_, logger_.get(), parser_.get(), xml_mmap.view());
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
    // [DODANO] Zastavica: ali smo parser_->parse() prekinili zaradi validacije.
    // Ločimo validacijsko napako od prave parse napake.
    bool validation_interrupted = false;
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
    logger_.info(fmt::format("Processing time: {:.3f}sec workers:{} segments:{} (ok:{} err:{}) size:{} byte(s)",
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
