#include "handler.hpp"
#include "common.hpp"
#include "e_tag_wide.hpp"
#include "x_str.hpp"

#include <ranges>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/util/XMLString.hpp>
namespace fsp
{

  // ============================================================================
  // Konstrukcija
  // ============================================================================

  Handler::Handler(proc_data&                    targets,
                   segment_queue&                queue,
                   const fsp_logger&             log,
                   const xercesc::SAX2XMLReader* parser,
                   std::string_view              base_addr)
  : targets_(targets) //
  , queue_(queue)     //
  , parser_(parser)
  , log_(log)             //
  , base_addr_(base_addr) //
  , log_trace_(log_.active(lvl_enum::trace))
  , log_debug_(log_.active(lvl_enum::debug))
  , log_info_(log_.active(lvl_enum::info))
  , log_warn_(log_.active(lvl_enum::warn))
  , log_err_(log_.active(lvl_enum::err))
  , log_crit_(log_.active(lvl_enum::crit))
  , max_xpath_depth_(static_cast<int>(targets_.targets.max_xpath_size()))
  {
    // targets are converted to wide characters
    for (const auto& el : targets_.targets)
    {
      xpath_wide_t tmp_vec;
      for (const auto& xp : el.xpath()) { tmp_vec.emplace_back(std::string(xp.ns), std::string(xp.tag)); }
      targets_wide_.push_back(tmp_vec);
    }
    matched_.assign(targets_wide_.size(), 0);
    // root level - alway present
    ns_stack_.emplace_back(ns_level{.depth = -1, .ns_vec = {}, .ns_decl_string = {}});
    if (log_debug_) [[unlikely]]
      for (std::size_t i = 0; i < targets_.targets.size(); ++i) log_.debug(fmt::format("target[{}] = '{}'", i, targets_.targets[i].name()));
    constexpr const std::size_t max_space_for_make_open_tag = 1024;
    buf_.reserve(max_space_for_make_open_tag);
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
      ns_pending_.clear(); // clear the buffer, since the contents is on the stack already
      rebuild_ns_decl_for_current_level();
    }
  }
  inline void Handler::rebuild_ns_decl_for_current_level()
  {
    static const int buf_size = 512;
    if (ns_stack_.empty()) return;

    auto& current = ns_stack_.back();
    current.ns_decl_string.clear();
    current.ns_decl_string.reserve(buf_size);

    // copy ns tring from previous level
    if (ns_stack_.size() >= 2)
    {
      const auto& previous   = ns_stack_[ns_stack_.size() - 2];
      current.ns_decl_string = previous.ns_decl_string;
    }

    // add only declarations of the current level
    for (const auto& [prefix, uri] : current.ns_vec)
    {
      if (prefix.empty()) [[unlikely]]
        fmt::format_to(std::back_inserter(current.ns_decl_string), " xmlns=\"{}\"", uri.to_string());
      else fmt::format_to(std::back_inserter(current.ns_decl_string), " xmlns:{}=\"{}\"", prefix.to_string(), uri.to_string());
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
    buf_.clear();
    buf_.append(R"(<?xml version="1.0" encoding="UTF-8"?><)");
    buf_.append(x_str(qname).to_string());

    if (! ns_stack_.empty()) buf_.append(ns_stack_.back().ns_decl_string); // namespaces

    // Atributs — xmlns:* skip, they were inserted in the previous loop
    for (XMLSize_t i = 0; i < attrs.getLength(); ++i)
    {
      const std::string qname = x_str(attrs.getQName(i)).to_string();
      if (qname.starts_with("xmlns")) [[unlikely]]
        continue;
      const auto escaped_str = escape_xml_attr(x_str(attrs.getValue(i)).to_string_view());
      buf_.append(fmt::format(R"( {}="{}")", qname, escaped_str));
    }
    buf_ += '>';
    return buf_;
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
    constexpr const auto every = 524287U - 1U; // 2**15
    if ((element_counter_++ & every) == 0 && val_future_.valid())
    {
      if (val_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      {
        const auto& val_result = val_future_.get();
        if (val_result.has_value())
        {
          if (log_debug_) [[unlikely]]
            log_.debug("Handler: validacijska napaka zaznana, prekinjam SAX parsing.");
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
        frag_depth_ = 0;
        active_idx_ = static_cast<int>(i);

        // Byte offset of the start of this element ;one character after opening tag '>'
        frag_start_offset_ = parser_->getSrcOffset(); //
        prefix_            = make_open_tag(qname, attrs);

        if (log_trace_) [[unlikely]]
        {
          auto ln     = x_str(localname).to_string();
          auto ns_uri = x_str(uri).to_string();
          log_.trace(fmt::format("tag:'{}' ns:'{}' offset:{} prefix:'{}'", ln, ns_uri, frag_start_offset_, prefix_));
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
    if (log_debug_) [[unlikely]]
      log_.trace(
        fmt::format("startElement depth:{:2} local:'{:10}' uri:'{}'", doc_depth_, x_str(localname).to_string(), x_str(uri).to_string()));
    if (! is_capturing() && doc_depth_ <= max_xpath_depth_) check_xpath_matches(uri, localname, qname, attrs);
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
        if (log_debug_) [[unlikely]]
        {
          log_.debug(fmt::format("pushing to queue: {} {}", x_str(qname).to_string(), seg.dump()));
          log_.trace(fmt::format("{}", seg.dump_all(base_addr_)));
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
    if (log_warn_) { log_.warn(prepare_msg(e)); }
  }
  void Handler::error(const xercesc::SAXParseException& e)
  {
    if (log_err_) { log_.error(prepare_msg(e)); }
  }
  void Handler::fatalError(const xercesc::SAXParseException& e)
  {
    if (log_crit_) { log_.critical(prepare_msg(e)); }
  }
  std::string_view Handler::base_addr() const { return base_addr_; }

} // namespace fsp
