#include "handler.hpp"
#include "common.hpp"
#include "x_str.hpp"

#include <ranges>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/util/XMLString.hpp>

#include <format>

namespace fsp
{

  // ============================================================================
  // Konstrukcija
  // ============================================================================

  Handler::Handler(proc_data&                      targets,
                   segment_queue&                  queue,
                   std::shared_ptr<spdlog::logger> logger,
                   const xercesc::SAX2XMLReader*   parser,
                   std::string_view                base_addr)
  : targets_(targets) //
  , queue_(queue)     //
  , parser_(parser)
  , logger_(std::move(logger)) //
  , base_addr_(base_addr)      //
  {
    // targets are converted to wide characters
    for (const auto& el : targets_.targets)
    {
      xpath_wide_t tmp_vec;
      for (const auto& xp : el.xpath()) { tmp_vec.emplace_back(std::string(xp.ns), std::string(xp.tag)); }
      targets_wide_.push_back(tmp_vec);
    }
    matched_.assign(targets_wide_.size(), 0);
    // Korenski NS nivo — vedno prisoten
    ns_stack_.emplace_back(ns_level{.depth = -1, .ns_vec = {}});
    for (std::size_t i = 0; i < targets_.targets.size(); ++i) logger_->debug("target[{}] = '{}'", i, targets_.targets[i].name());
  }

  // ============================================================================
  // NS context stack
  // ============================================================================

  inline void Handler::push_ns_mapping(const XMLCh* prefix, const XMLCh* uri) { ns_pending_.emplace_back(x_str(prefix), x_str(uri)); }

  inline void Handler::open_ns_scope()
  {
    if (! ns_pending_.empty())
    { // new push only if new ns definitions
      ns_stack_.emplace_back(doc_depth_, std::move(ns_pending_));
      ns_pending_.clear();
    }
  }

  inline void Handler::close_ns_scope()
  {
    if (ns_stack_.back().depth == doc_depth_) ns_stack_.pop_back();
  }

  inline x_str Handler::resolve_ns(const x_str& prefix) const noexcept
  {
    // Iščemo od vrha navzdol — globji nivo ima prednost
    for (const auto& it : std::views::reverse(ns_stack_))
    {
      for (const auto& el : it.ns_vec)
      {
        if (el.first == prefix) return el.second; // faster than hash for short vectors
      }
    }
    return {};
  }

  inline ns_def_t Handler::active_ns() const
  {
    ns_def_t result;
    // Od dna navzgor — globji nivo prekrije višjega
    for (const auto& level : ns_stack_)
      for (const auto& [prefix, uri] : level.ns_vec) result.emplace_back(prefix, uri);
    return result;
  }

  // ============================================================================
  // Tag matching
  // ============================================================================

  inline bool Handler::tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept
  {
    // Local name se mora vedno ujemati
    if (tag.tag() != local_name) return false;

    // Brez NS v pravilu — ujema se z vsem
    if (tag.ns().empty()) return true;

    // Z NS — razrešimo prefix in primerjamo URI
    const XMLCh* expected_uri = resolve_ns(tag.ns()).data();
    if (nullptr == expected_uri)
    {
      // Prefix ni znan — primerjamo direktno z ns_uri iz SAX eventa
      return tag.ns() == ns_uri;
    }
    return xercesc::XMLString::equals(expected_uri, ns_uri);
  }

  /*!
   * The method generates the start part of the opening tag, that is
   * prefixed to the rest of the xml segment that is sent to the worker
   */
  inline std::string Handler::make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs)
  {
    std::string str;
    str += R"(<?xml version="1.0" encoding="UTF-8"?><)";
    str += x_str(qname).to_string();

    // vbrizgamo aktivne NS deklaracije
    // da workerjev DOM parser pravilno razreši namespace-e
    for (const auto& [prefix, uri] : active_ns())
    {
      if (prefix.empty()) [[unlikely]]
        str += std::format(" xmlns=\"{}\"", uri.to_string());
      else str += fmt::format(" xmlns:{}=\"{}\"", prefix.to_string(), uri.to_string());
    }

    // Atributi — xmlns:* preskočimo, že smo jih vbrizgali
    for (XMLSize_t i = 0; i < attrs.getLength(); ++i)
    {
      const auto qname = x_str(attrs.getQName(i)).to_string();
      if (qname.starts_with("xmlns")) continue;
      const auto aval = escape_xml_attr(x_str(attrs.getValue(i)).to_string());
      str += fmt::format(" {}=\"{}\"", qname, aval);
    }

    str += '>';
    return str;
  }

  // ============================================================================
  // SAX2 prefix mapping (pride PRED startElement)
  // ============================================================================

  inline void Handler::startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) { push_ns_mapping(prefix, uri); }
  // void Handler::endPrefixMapping() { }

  // ============================================================================
  // startElement
  // ============================================================================
  // ============================================================================
  // Helper inline methods (add declarations to handler.hpp)
  // ============================================================================

  inline void Handler::check_validation_status()
  {
    // [DODANO] Polling preverjanje validacijske napake iz vzporedne niti.
    // Izvede se vsakih 1024 elementov (bitna maska je cenejša od modulo).
    // wait_for(0) je neblokirajoč — vrne immediately z deferred/timeout/ready.
    // Ob napaki vržemo SAXParseException: to je edini način za prekinitev
    // Xerces SAX parsinga iz ContentHandler callbacka. Izjema se propagira
    // skozi parser_->parse() in jo ujame process_from_buffer().
    constexpr const auto every = 8192 - 1U; // 2**13
    if ((element_counter_++ & every) == 0 && val_future_.valid())
    {
      if (val_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      {
        const auto& val_result = val_future_.get();
        if (val_result.has_value())
        {
          if (logger_ && logger_->should_log(spdlog::level::debug))
            logger_->debug("Handler: validacijska napaka zaznana, prekinjam SAX parsing.");
          // Vržemo SAXParseException — Xerces jo ujame interno in ustavi parsing.
          // Sporočilo prenesemo naprej; row/col ni znan na tej točki (0,0).
          // TODO: ostri - ostri - preveri kako prenesemo informacijo
          // NOLINTNEXTLINE(hicpp-exception-baseclass, cert-err60-cpp)
          throw xercesc::SAXParseException(x_str(val_result->message()).c_str(), nullptr, nullptr, 0, 0);
        }
      }
    }
  }

  inline void Handler::check_xpath_matches( //
    const XMLCh*               uri,
    const XMLCh*               localname,
    const XMLCh*               qname,
    const xercesc::Attributes& attrs)
  {
    // Advance all candidates matching the current depth
    for (std::size_t i = 0; i < targets_.targets.size(); ++i)
    {
      // const auto& xpath      = targets_[i];
      const xpath_wide_t& xpath_wide = targets_wide_[i];
      int&                m          = matched_[i];

      if (m >= static_cast<int>(xpath_wide.size())) continue;

      // Next step must be at depth m+1 == doc_depth_
      if (m + 1 != doc_depth_)
      {
        if (doc_depth_ <= m) m = 0;
        continue;
      }

      const e_tag_wide& step = xpath_wide[m];
      if (tag_matches(step, localname, uri)) m++;
    }

    // Check if any xpath is completed
    for (auto i = 0U; i < targets_wide_.size(); ++i)
    {
      if (matched_[i] == static_cast<int>(targets_wide_[i].size()))
      { // start of the subtree
        // capturing_  = true;
        frag_depth_ = 0;
        active_idx_ = static_cast<int>(i);

        // Byte offset of the start of this element ;one character after opening tag '>'
        frag_start_offset_ = parser_->getSrcOffset(); //
        prefix_            = make_open_tag(qname, attrs);

        if (logger_ && logger_->should_log(spdlog::level::trace)) [[unlikely]]
        {
          auto ln     = x_str(localname).to_string();
          auto ns_uri = x_str(uri).to_string();
          logger_->trace("tag:'{}' ns:'{}' offset:{} prefix:'{}'", ln, ns_uri, frag_start_offset_, prefix_);
        }
        break;
      }
    }
  }

  void Handler::startElement(const XMLCh* uri, const XMLCh* localname, const XMLCh* qname, const xercesc::Attributes& attrs)
  {
    check_validation_status();
    open_ns_scope();
    doc_depth_++;
    if (logger_ && logger_->should_log(spdlog::level::debug)) [[unlikely]]
      logger_->trace("startElement depth:{:2} local:'{:10}' uri:'{}'", doc_depth_, x_str(localname).to_string(), x_str(uri).to_string());
    if (! is_capturing()) check_xpath_matches(uri, localname, qname, attrs);
    if (is_capturing()) [[likely]]
      frag_depth_++;
  }

  // ============================================================================
  // endElement
  // ============================================================================

  void Handler::endElement( //
    [[maybe_unused]] const XMLCh* uri,
    [[maybe_unused]] const XMLCh* localname,
    [[maybe_unused]] const XMLCh* qname)
  {
    if (is_capturing()) [[likely]]
    {
      frag_depth_--;
      if (frag_depth_ == 0) [[unlikely]]
      { // fragment is finished. wrap it up and send it to the workers
        std::size_t end_offset = parser_->getSrcOffset();
        std::size_t length     = end_offset - frag_start_offset_;
        auto        seg        = xml_segment(counter_, active_idx_, frag_start_offset_, length, prefix_);
        if (logger_ && logger_->should_log(spdlog::level::debug)) [[unlikely]]
        {
          logger_->debug("pushing to queue: {} {}", x_str(qname).to_string(), seg.dump());
          logger_->trace("{}", seg.dump_all(base_addr_));
        }
        queue_.push(std::move(seg));
        counter_++;
        frag_depth_ = -1; // we are outside of capturing
        // capturing_  = false;
        active_idx_ = -1;
      }
    }
    doc_depth_--;
    close_ns_scope();
    // Resetiramo matched_ za pravila ki so globlje od trenutne globine
    for (auto& m : matched_)
      if (m > doc_depth_) [[unlikely]]
        m = doc_depth_; // NOLINT(readability-use-std-min-max)
  }

  // ============================================================================
  // Error handler
  // ============================================================================
  inline std::string Handler::prepare_msg(const xercesc::SAXParseException& e)
  {
    auto col = e.getColumnNumber();
    auto row = e.getLineNumber();
    auto msg = x_str(e.getMessage()).to_string();
    return fmt::format("sax parser '{}' row: {} col: {}", msg, row, col);
  }
  void Handler::warning(const xercesc::SAXParseException& e)
  {
    if (logger_) { logger_->warn(prepare_msg(e)); }
  }
  void Handler::error(const xercesc::SAXParseException& e)
  {
    if (logger_) { logger_->error(prepare_msg(e)); }
  }
  void Handler::fatalError(const xercesc::SAXParseException& e)
  {
    if (logger_) { logger_->critical(prepare_msg(e)); }
  }
  std::string_view Handler::base_addr() const { return base_addr_; }

} // namespace fsp
