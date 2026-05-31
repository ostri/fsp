#include "handler.hpp"
#include "common.hpp"
#include "x_str.hpp"

#include <algorithm>
#include <ranges>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/util/XMLString.hpp>

#include <format>

namespace fsp
{

  // ============================================================================
  // Konstrukcija
  // ============================================================================

  Handler::Handler(const std::vector<xpath_t>&     targets,
                   segment_queue&                  queue,
                   std::shared_ptr<spdlog::logger> logger,
                   const xercesc::SAX2XMLReader*   parser,
                   std::string_view                base_addr //
                   )
  : targets_(targets) //
  , queue_(queue)     //
  , parser_(parser)
  , logger_(std::move(logger)) //
  , base_addr_(base_addr)      //
  {
    for (const auto& el : targets_)
    {
      xpath_wide_t tmp_vec;
      for (const auto& xp : el) { tmp_vec.emplace_back(xp.ns(), xp.tag()); }
      targets_wide_.push_back(tmp_vec);
    }
    matched_.assign(targets_.size(), 0);
    // Korenski NS nivo — vedno prisoten
    ns_stack_.emplace_back();
  }

  // ============================================================================
  // NS context stack
  // ============================================================================

  void Handler::push_ns_mapping(const XMLCh* prefix, const XMLCh* uri) { ns_pending_[x_str(prefix)] = x_str(uri); }

  void Handler::open_ns_scope(const XMLCh* qname)
  {
    if (! ns_pending_.empty())
    { // new push only if new ns definitions
      ns_stack_.emplace_back(x_str(qname), std::move(ns_pending_));
      ns_pending_.clear();
    }
  }

  void Handler::close_ns_scope(const XMLCh* qname)
  {
    // if (ns_stack_.size() > 1) ns_stack_.pop_back();
    if (! ns_stack_.empty() && ns_stack_.back().qname == qname) { ns_stack_.pop_back(); }
  }

  x_str Handler::resolve_ns(const x_str& prefix) const noexcept
  {
    // Iščemo od vrha navzdol — globji nivo ima prednost
    for (const auto& it : std::views::reverse(ns_stack_))
    {
      if (auto found = it.ns_map.find(prefix); found != it.ns_map.end()) return found->second;
    }
    return {};
  }

  std::unordered_map<x_str, x_str> Handler::active_ns() const
  {
    std::unordered_map<x_str, x_str> result;
    // Od dna navzgor — globji nivo prekrije višjega
    for (const auto& level : ns_stack_)
      for (const auto& [prefix, uri] : level.ns_map) result[prefix] = uri;
    return result;
  }

  // ============================================================================
  // Tag matching
  // ============================================================================

  bool Handler::tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept
  {
    // Local name se mora vedno ujemati
    if (tag.tag() != local_name) return false;

    // Brez NS v pravilu — ujema se z vsem
    if (tag.ns().empty()) return true;

    // Z NS — razrešimo prefix in primerjamo URI
    auto expected_uri = resolve_ns(tag.ns());
    if (expected_uri.empty())
    {
      // Prefix ni znan — primerjamo direktno z ns_uri iz SAX eventa
      return tag.ns() == ns_uri;
    }
    return expected_uri == x_str(ns_uri).to_string();
  }

  /*!
   * The method generates the start part of the opening tag, that is
   * prefixed to the rest of the xml segment that is sent to the worker
   */
  std::string Handler::make_open_tag(const std::string& qname, const xercesc::Attributes& attrs)
  {
    std::string str;
    str += R"(<?xml version="1.0" encoding="UTF-8"?><)";
    str += qname;

    // vbrizgamo aktivne NS deklaracije
    // da workerjev DOM parser pravilno razreši namespace-e
    for (const auto& [prefix, uri] : active_ns())
    {
      if (prefix.empty()) str += std::format(" xmlns=\"{}\"", uri.to_string());
      else str += std::format(" xmlns:{}=\"{}\"", prefix.to_string(), uri.to_string());
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

  void Handler::startPrefixMapping(const XMLCh* prefix, const XMLCh* uri)
  {
    // if (logger_)
    //   logger_->debug("prefix: '{}' uri: '{}' offs: {}", x_str(prefix).to_string(), x_str(uri).to_string(), parser_->getSrcOffset());
    push_ns_mapping(prefix, uri);
  }

  void Handler::endPrefixMapping([[maybe_unused]] const XMLCh* prefix)
  {
    // if (logger_) logger_->debug("end prefix mapping offs: {}", parser_->getSrcOffset());
    // NS scope se zapre v endElement prek close_ns_scope()
  }

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
    const auto every_1024 = 0x3FFU;
    if ((element_counter_++ & every_1024) == 0 && val_future_.valid())
    {
      if (val_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      {
        const auto& val_result = val_future_.get();
        if (val_result.has_value())
        {
          if (logger_) logger_->debug("Handler: validacijska napaka zaznana, prekinjam SAX parsing.");
          // Vržemo SAXParseException — Xerces jo ujame interno in ustavi parsing.
          // Sporočilo prenesemo naprej; row/col ni znan na tej točki (0,0).
          // TODO: ostri - ostri - preveri kako prenesemo informacijo
          // NOLINTNEXTLINE(hicpp-exception-baseclass, cert-err60-cpp)
          throw xercesc::SAXParseException(x_str(val_result->message()).c_str(), nullptr, nullptr, 0, 0);
        }
      }
    }
  }

  // inline void Handler::process_namespaces(const xercesc::Attributes& attrs)
  // {
  //   // Paste your original XMLCh* attribute loop and namespace logic here
  // }

  // inline void Handler::check_xpath_matches(const XMLCh* uri_ch, const XMLCh* localname_ch)
  // {
  //   // Paste your original fast XMLCh* comparison logic here
  // }
  inline void Handler::check_xpath_matches([[maybe_unused]] const XMLCh*               uri,
                                           [[maybe_unused]] const XMLCh*               localname,
                                           [[maybe_unused]] const XMLCh*               qname,
                                           [[maybe_unused]] const xercesc::Attributes& attrs)
  {
    // Advance all candidates matching the current depth
    for (std::size_t i = 0; i < targets_.size(); ++i)
    {
      // const auto& xpath      = targets_[i];
      const auto& xpath_wide = targets_wide_[i];
      int&        m          = matched_[i];

      if (m >= static_cast<int>(xpath_wide.size())) continue;

      // Next step must be at depth m+1 == doc_depth_
      if (m + 1 != doc_depth_)
      {
        if (doc_depth_ <= m) m = 0;
        continue;
      }

      const auto& step = xpath_wide[m];
      if (tag_matches(step, localname, uri)) m++;
    }

    // Check if any xpath is completed
    for (auto i = 0U; i < targets_wide_.size(); ++i)
    {
      if (matched_[i] == static_cast<int>(targets_wide_[i].size()))
      {
        capturing_  = true;
        frag_depth_ = 0;
        active_idx_ = static_cast<int>(i);

        // Byte offset of the start of this element ;one character after opening tag '>'
        frag_start_offset_ = parser_->getSrcOffset(); //
        auto ln            = x_str(localname).to_string();
        auto qn            = x_str(qname).to_string();
        auto ns_uri        = x_str(uri).to_string();

        prefix_ = make_open_tag(qn, attrs);
        if (logger_) logger_->trace("tag:'{}' ns:'{}' offset:{} prefix:'{}'", ln, ns_uri, frag_start_offset_, prefix_);

        break;
      }
    }
  }

  void Handler::startElement(const XMLCh*                  uri,
                             [[maybe_unused]] const XMLCh* localname,
                             [[maybe_unused]] const XMLCh* qname,
                             const xercesc::Attributes&    attrs)
  {
    check_validation_status();

    open_ns_scope(qname);
    doc_depth_++;

    if (! capturing_) check_xpath_matches(uri, localname, qname, attrs);

    if (capturing_) frag_depth_++;
  }

  // ============================================================================
  // endElement
  // ============================================================================

  void Handler::endElement( //
    [[maybe_unused]] const XMLCh* uri,
    [[maybe_unused]] const XMLCh* localname,
    [[maybe_unused]] const XMLCh* qname)
  {
    if (capturing_)
    {
      frag_depth_--;
      if (frag_depth_ == 0)
      { // fragment is finished. wrap it up and send it to the workers
        std::size_t end_offset = parser_->getSrcOffset();
        std::size_t length     = end_offset - frag_start_offset_;
        if (logger_)
        {
          logger_->debug("pushing to queue:{} type: {} segment: {}", x_str(qname).to_string(), active_idx_, counter_);
          logger_->trace("{} offset: {} len {} prefix '{}'", x_str(qname).to_string(), frag_start_offset_, length, prefix_);
        }
        queue_.push(xml_segment(counter_, active_idx_, frag_start_offset_, length, prefix_));
        counter_++;
        //        if (logger_) logger_->trace(fmt::format("subtree: '{}'", base_addr_.substr(frag_start_offset_, length)));
        capturing_  = false;
        active_idx_ = -1;
      }
    }
    doc_depth_--;
    close_ns_scope(qname);
    // Resetiramo matched_ za pravila ki so globlje od trenutne globine
    for (auto& m : matched_) m = std::min(m, doc_depth_);
  }

  // ============================================================================
  // Error handler
  // ============================================================================
  std::string Handler::prepare_msg(const xercesc::SAXParseException& e)
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
