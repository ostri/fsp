#include "handler.hpp"
#include "common.hpp"
#include "e_tag_wide.hpp"
#include "x_str.hpp"
#include "xml_line_context.hpp"

#include <ranges>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/util/XMLString.hpp>
namespace fsp
{
  // ============================================================================
  // Konstrukcija
  // ============================================================================
  Handler::Handler(const proc_data&              targets, //
                   const logger::Logger&         log,
                   const xercesc::SAX2XMLReader* parser,
                   segment_pool&                 pool,
                   const doc_set_dscr&           ds_dscr,
                   std::vector<bool>             is_header_seg_type)
  : log_(log)                                          // log
  , targets_(targets)                                  //
  , parser_(parser)                                    // parser
  , ds_dscr_(ds_dscr)                                  // documents to be processed
  , pool_(pool)                                        // segment pool
  , is_header_seg_type_(std::move(is_header_seg_type)) // see its own doc comment in handler.hpp
  , max_xpath_depth_(static_cast<int>(targets_.targets.max_xpath_size()))
  {
    // targets are converted to wide characters to make all matching in XMLCh
    if (targets_wide_.capacity() < targets.targets.size()) targets_wide_.reserve(targets.targets.size());
    for (const auto& el : targets_.targets)
    {
      xpath_wide_t tmp_vec;
      if (tmp_vec.capacity() < el.path().size()) tmp_vec.reserve(el.xpath().size());
      for (const auto& xp : el.xpath()) { tmp_vec.emplace_back(str_t(xp.ns), str_t(xp.tag)); }
      targets_wide_.push_back(tmp_vec);
    }
    auto              all_rules_mask = 0ULL;
    const std::size_t num_rules      = targets_wide_.size();
    if ((sizeof(all_rules_mask) * CHAR_BIT) < num_rules) logic_error("more xpaths than 64"); // FIXME ostri initial test
    all_rules_mask = (1ULL << num_rules) - 1ULL;

    rule_lengths_.reserve(num_rules);
    for (const auto& xp : targets_wide_) rule_lengths_.push_back(xp.size());
    const std::size_t expected_depth_of_the_search_tree = 32;
    active_mask_stack_.reserve(expected_depth_of_the_search_tree); // expected max depth of the xml tree
                                                                   // preallocate to avoid expansion of the vector during the processing
    active_mask_stack_.emplace_back(all_rules_mask);
    //  root level - always present
    ns_stack_.emplace_back(ns_level{.depth = -1, .ns_vec = {}, .ns_decl = {}});
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
      // rebuild_ns_decl_for_current_level() is called only when we have the first full match
    }
  }
  inline void Handler::rebuild_ns_decl_for_current_level()
  {
    static const int buf_size = 4096;
    if (ns_stack_.empty()) return;

    auto& current = ns_stack_.back();
    if (! current.ns_decl.empty()) return; // last ns_decl is empty; it was already generated

    // current.ns_decl is empty, there is no need to clear it
    current.ns_decl.reserve(buf_size);

    if (ns_stack_.size() >= 2) // copy ns string from previous level if exists
      current.ns_decl = ns_stack_[ns_stack_.size() - 2].ns_decl;

    thread_local str_XMLCh_t tmp_str;
    tmp_str.clear();
    const std::size_t buf_siz_min = 1024;
    if (tmp_str.capacity() < buf_siz_min) tmp_str.reserve(buf_siz_min);
    // add only declarations of the current level
    for (const auto& [prefix, uri] : current.ns_vec)
    {
      if (prefix.empty()) [[unlikely]] { tmp_str.append(u"xmlns=\""); }
      else
      {
        tmp_str.append(u"xmlns:");
        tmp_str.append(prefix.data());
        tmp_str.append(u"=\"");
      }
      tmp_str.append(uri.data());
      tmp_str.append(u"\" ");
    }
    if (current.ns_decl.capacity() < current.ns_decl.size() + tmp_str.size()) // allocate upfront if necessary
      current.ns_decl.resize(current.ns_decl.size() * 2);
    current.ns_decl.append(tmp_str.data(), tmp_str.size() - 1); // remove the trailing space
    if (log_trace_)
    {
      log_.trace(fmt::format("append ns:    '{}'", x_str(tmp_str).to_string_view()));
      log_.trace(fmt::format("whole ns str: '{}'", x_str(current.ns_decl).to_string_view()));
    }
  }
  inline void Handler::close_ns_scope()
  {
    if (ns_stack_.back().depth == doc_depth_) ns_stack_.pop_back();
  }

  // ============================================================================
  // Tag matching
  // ============================================================================
  /**
   * @brief compare current tag (local_name + ns_uri) with tag from xpath
   *
   * @param tag tag from the xpath (ns is already uri)
   * @param local_name parser tag local name
   * @param ns_uri parser tag uri
   * @return true xpath node and current node are the same
   * @return false xpath node and current node are different
   */
  inline bool Handler::tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept
  {
    if (! xercesc::XMLString::equals(tag.tag().data(), local_name)) return false; // localname must match or false
    if (tag.ns().empty() && (ns_uri == nullptr)) return true;                     // equal localname and no ns
    return xercesc::XMLString::equals(tag.ns().data(), ns_uri); // tag.ns() is already uri. it was converted during the compile time
  }
  /**
   * @brief extract attribute values from attrs structure
   * The method stores the definition and values of the provided attributes in the result string.
   * warning:
   * - this method is on critical path it must be as fast as possible
   *   - hence push_back & append instead of fmt::format
   *   - string is XMLCh to postpone the utf16->utf8 conversion
   * @param attrs structure as provided by xercesc
   * @return str_XMLCh_t tag attribute string ready to be inserted into the tag definition
   */
  str_XMLCh_t Handler::attr_values_str(const xercesc::Attributes& attrs)
  {
    const std::size_t buf_size = 4096;
    buf_.clear();
    if (buf_.capacity() < buf_size) buf_.reserve(buf_size);
    for (XMLSize_t i = 0; i < attrs.getLength(); ++i)
    {
      const auto* qn = attrs.getQName(i);
      // Attributes — xmlns:* skip, they were inserted in the previous loop
      if (xercesc::XMLString::startsWith(qn, u"xmlns")) [[unlikely]] // skip NS definitions
        continue;
      // const auto escaped_str = escape_xml_attr_xmlch(attrs.getValue(i));
      buf_.push_back(u' ');
      buf_.append(qn);
      buf_.append(u"=\"");
      escape_xml_attr_xmlch(attrs.getValue(i), buf_);
      buf_.append(u"\"");
      ;
    }
    buf_.push_back(u'>');
    return buf_;
  }
  // ============================================================================
  // SAX2 prefix mapping (it starts before startElement)
  // ============================================================================
  inline void Handler::startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) { push_ns_mapping(prefix, uri); }
  // ============================================================================
  // startElement

  inline void Handler::check_xpath_matches( //
    const XMLCh* uri,
    const XMLCh* localname,
    //    [[maybe_unused]] const XMLCh* qname,
    const xercesc::Attributes& attrs)
  {
    if (active_mask_stack_.empty()) [[unlikely]]
      logic_error("active_mask_stack_ is empty");
    RuleMask previous = active_mask_stack_.back();
    if (previous == 0) return;

    RuleMask current_match = 0;
    if (doc_depth_ <= 0) [[unlikely]]
      logic_error("doc_depth_ is <= 0: should be greter than 0");
    for (std::size_t i = 0; i < targets_wide_.size(); ++i)
    {
      if ((previous & (1ULL << i)) == 0) continue;                   // skip if already eliminated in previous step
      if (doc_depth_ > static_cast<int>(rule_lengths_[i])) continue; // too deep; skip to next
      const xpath_wide_t& xpath = targets_wide_[i];
      // doc_depth_ as index of xpath step
      const e_tag_wide& step = xpath[doc_depth_ - 1];
      if (tag_matches(step, localname, uri)) current_match |= (1ULL << i);
    }

    RuleMask new_active = previous & current_match;
    active_mask_stack_.push_back(new_active);

    // check if we fullfiled some path
    for (std::size_t i = 0; i < targets_wide_.size(); ++i)
    { // rule/xpath is still active and current path is full
      if (((new_active & (1ULL << i)) != 0U) && (doc_depth_ == static_cast<int>(rule_lengths_[i])))
      {
        rebuild_ns_decl_for_current_level(); // we have first hit. we should recalculate the the ns string
        frag_depth_ = 0;
        seg_type_   = static_cast<int>(i);
        if (! ns_stack_.empty()) ns_ = ns_stack_.back().ns_decl;
        frag_start_offset_ = parser_->getSrcOffset();
        if (attrs.getLength() > 0) attr_ = attr_values_str(attrs);

        if (log_trace_) [[unlikely]]
        {
          log_.trace(fmt::format(R"(tag:'{}' ns:'{}' offset:{}
  ns:'{}'
  attr: '{}')",
                                 x_str(localname).to_string_view(),
                                 x_str(uri).to_string_view(),
                                 frag_start_offset_,
                                 x_str(ns_).to_string_view(),
                                 x_str(attr_).to_string_view()));
        }
        break; // there can be only one xpath fullfiled at once (we don't cut over attributes)
      }
    }
  }
  [[noreturn]] void Handler::logic_error(const char* msg) const { throw std::runtime_error(str_t("internal error: ") + msg); }

  void Handler::startElement(const XMLCh*                  uri,
                             const XMLCh*                  localname,
                             [[maybe_unused]] const XMLCh* qname,
                             const xercesc::Attributes&    attrs)
  {
    open_ns_scope();
    doc_depth_++;
    if (log_debug_) [[unlikely]]
      log_.trace(fmt::format(
        "startElement depth:{:2} local:'{:10}' uri:'{}'", doc_depth_, x_str(localname).to_string_view(), x_str(uri).to_string_view()));
    if (! is_capturing() && (doc_depth_ <= max_xpath_depth_)) check_xpath_matches(uri, localname, attrs);
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
        std::size_t segment_id = counter_++;
        std::size_t idx        = pool_.acquire_slot(segment_id);
        auto        seg        = xml_segment(segment_id, seg_type_, doc_ndx_, frag_start_offset_, length, ns_, attr_);
        if (log_debug_) [[unlikely]]
        {
          log_.debug(fmt::format("pushing to queue: '{}' '{}'", x_str(localname).to_string_view(), seg.dump()));
          log_.trace(fmt::format("{}", seg.dump_all(doc_)));
        }
        pool_.set_segment(idx, std::move(seg));
        // is_header_seg_type_ is empty unless importer_config::header_seg_types was actually set
        // (see its own doc comment) -- the bounds check below is therefore free in the common
        // case (empty vector short-circuits before ever indexing it) and only ever indexes
        // is_header_seg_type_[seg_type_] once it's known non-empty AND seg_type_ (always >= 0
        // here -- set from a live, matched xpath rule, never the -1 "no capture" sentinel) is in
        // range.
        const bool is_header = ! is_header_seg_type_.empty() && static_cast<std::size_t>(seg_type_) < is_header_seg_type_.size() &&
                               is_header_seg_type_[static_cast<std::size_t>(seg_type_)];
        if (is_header) pool_.push_ready_header(idx);
        else pool_.push_ready(idx);

        frag_depth_ = -1; // we are outside of capturing
        seg_type_   = -1; // undefined segment type
        // restore old active xpath mask
        if (! active_mask_stack_.empty()) active_mask_stack_.pop_back();
        else [[unlikely]] logic_error("active_mask_stack_ empty in endElement");
      }
    }
    doc_depth_--;
    close_ns_scope();
  }
  // ============================================================================
  // Error handler
  // ============================================================================
  inline str_t Handler::prepare_msg(const xercesc::SAXParseException& e)
  {
    auto col = e.getColumnNumber();
    auto row = e.getLineNumber();
    // .to_string() (owning copy), not .to_string_view() -- the x_str here is a temporary that's
    // destroyed at the end of this statement, so a view into its cached_utf8_ would dangle by
    // the time fmt::format() reads it below.
    auto msg     = x_str(e.getMessage()).to_string();
    auto context = context_around(doc_, row);
    // context is empty when row is out of range (see context_around()'s own doc comment) -- omit
    // the "near: ..." suffix entirely rather than print an empty/misleading one.
    if (context.empty()) return fmt::format("sax parser '{}' row: {} col: {}", msg, row, col);
    return fmt::format("sax parser '{}' row: {} col: {} near: {}", msg, row, col, context);
  }
  void Handler::warning(const xercesc::SAXParseException& e)
  {
    if (log_warn_) { log_.warn(prepare_msg(e)); }
  }
  void Handler::error(const xercesc::SAXParseException& e)
  {
    if (log_err_) { log_.error(prepare_msg(e)); }
    last_error_source_ = sax_error_source::validity; // schema/validity constraint violation, see sax_error_source's own doc comment
    if (validating_) throw e;                        // NOLINT(hicpp-exception-baseclass) -- caught by type in doc_cutter::cut()
  }
  void Handler::fatalError(const xercesc::SAXParseException& e)
  {
    if (log_crit_) { log_.critical(prepare_msg(e)); }
    last_error_source_ = sax_error_source::well_formed; // well-formedness violation, see sax_error_source's own doc comment
    if (validating_) throw e;                           // NOLINT(hicpp-exception-baseclass)
  }

} // namespace fsp
