#include "xml_worker.hpp"
#include "parsing_util.hpp"
#include "pipeline.hpp"
#include "segment_result.hpp"
#include "xpath_helpers.hpp"
#include <chrono>
#include <fmt/format.h>
// #include <magic_enum.hpp>
// #include <libxml/xmlreader.h>
// #include <stack>
#include <utility>

// namespace
// {
//   struct xml_deleter
//   {
//     void operator()(xmlChar* str) const
//     {
//       if (str != nullptr) xmlFree(str);
//     }
//   };
//   using xml_char = std::unique_ptr<xmlChar, xml_deleter>;
// } // namespace

namespace fsp
{
  //   using xml_char = std::unique_ptr<xmlChar, xml_deleter>;
  xml_worker::xml_worker(
    segment_pool&       pool,          // reference to segment pool
    const doc_set_dscr& ds_dscr,       // reference to document set structure
    vec_seg_result&     results,       // where to store correct segments
    vec_seg_result&     errors,        // where to store non correct segmetns
    std::mutex&         results_mutex, // mutex for managing result structure
    std::mutex&         errors_mutex,  // mutex for managing errors structure
    const fsp_logger&   log,           // reference to logger
    const proc_data&    targets,       // structure that holds information about cutting points and xpaths of the values we are looking for
    str_t               parent_log_name, // parent thread log thread name
    pipeline&           pl,
    pipeline_hooks&     hooks)
  : log_(log)
  , ds_dscr_(ds_dscr)
  , results_(results)
  , errors_(errors)
  , results_mutex_(results_mutex)
  , errors_mutex_(errors_mutex)
  , targets_(targets)
  , pipeline_(pl)
  , hooks_(hooks)
  , parent_log_name_(std::move(parent_log_name))
  , sax_(std::make_unique<segment_sax>(log_))
  , pool_(pool)
  {
    //     reader_.reset(xmlReaderForMemory("", 0, "noname.xml", nullptr, reader_flags_));
    //     if (reader_.get() == nullptr) { throw std::runtime_error("Failed to initialize xmlTextReader"); }
  }
  //   ////////////////////////////////////////////////////////////////////////////////////////////////////////
  //   /**
  //    * @brief main worker functor
  //    * The thread is called to process xml document fragments.
  //    * @param st should we interrupt the processing
  //    * @param worker_id unique id of the worker
  //    */
  //   void xml_worker::operator()([[maybe_unused]] const std::stop_token& st, int worker_id)
  //   {
  //     auto t0 = std::chrono::steady_clock::now();
  //     log_.make_log_name(parent_log_name_, fmt::format("wrk.{:02}", worker_id));

  //     if (log_debug_) log_.debug(fmt::format("Worker thread: id {} name '{}' started.", worker_id, log_thread_name));

  //     thread_local vec_seg_result loc_res_ok;  // segments with ok result
  //     thread_local vec_seg_result loc_res_nak; // segments with nak result

  //     while (true)
  //     {
  //       std::size_t idx = 0;
  //       if (pool_.pop_segment_ndx(idx) != queue_status::active)
  //       {
  //         if (log_debug_) log_.debug("Worker: ready_queue finished, exiting");
  //         break;
  //       }

  //       const xml_segment seg = pool_.retrieve_segment(idx); // LOKALNA KOPIJA!
  //       if (auto res = process_segment(seg))
  //       {
  //         if (loc_res_ok.size() + 1 == loc_res_ok.capacity()) loc_res_ok.reserve(loc_res_ok.size() * 2);
  //         loc_res_ok.emplace_back(std::move(*res));
  //       }
  //       else
  //       {
  //         if (loc_res_nak.size() + 1 == loc_res_nak.capacity()) loc_res_nak.reserve(loc_res_nak.size() * 2);
  //         loc_res_nak.emplace_back(std::move(*res)); // prilagodi glede na tvoj error tip
  //       }
  //     }
  //     if (log_debug_)
  //     {
  //       auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  //       log_.debug(fmt::format("Worker thread '{}' finished in {} ms, txn processed: {} (ok:{} nak:{}).",
  //                              log_thread_name,
  //                              duration,
  //                              loc_res_ok.size() + loc_res_nak.size(),
  //                              loc_res_ok.size(),
  //                              loc_res_nak.size()));
  //     }
  //     {
  //       std::lock_guard lock(results_mutex_);
  //       results_.append_range(std::move(loc_res_ok));
  //     }
  //     {
  //       std::lock_guard lock(errors_mutex_);
  //       errors_.append_range(std::move(loc_res_nak));
  //     }
  //   }
  /**
   * @brief process segmetn from the xml document
   *
   * @param seg segment to be processed
   * @return result<segment_result>  extracted segmetn data
   */
  result<segment_result> xml_worker::process_segment(const xml_segment& seg)
  {
    auto t0 = std::chrono::steady_clock::now();
    try
    {
      if (log_debug_) { log_.debug(fmt::format("process segment: {}", seg.dump())); }
      auto view     = seg.view(ds_dscr_[seg.doc_ndx()].mmf().data()); // just segment contents
      auto tmp_view = seg.subtree_str(view);                          // contents + opening and closing tag
      if (log_trace_) log_.trace(fmt::format("seg: {}: finalized doc:\n'{}'", seg.id(), tmp_view));
      auto r = sax_->exec(tmp_view, targets_.xpaths[seg.subtree_type()]);

      if (! r.empty())
      {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (log_debug_)
        {
          log_.debug(fmt::format("Segment '{}' SAX processing finished '{}'µs (offset={}, len={})", //
                                 seg.id(),
                                 us,
                                 seg.offset(),
                                 seg.length()));
        }
        segment_result res(seg.id(), seg.subtree_type(), std::move(r), seg.doc_ndx());
        return res;
      }

      error_info err( //
        processor_error::error_extracting_xpath_values,
        fmt::format("Error extracting xpath values: {}", "krneki"),
        "",
        0UL);
      if (log_warn_) log_.warn(fmt::format("Segment {}: {} :: ", seg.id(), "krneki"));
      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      error_info err( //
        processor_error::internal_error,
        fmt::format("Exception in segment {}: '{}'", seg.id(), e.what()),
        "",
        0UL);
      if (log_error_) log_.error(err.message());
      return std::unexpected(err);
    }
  }
  //   //////////////////////////////////////////////////////////////////////
  //   void xml_worker::close_tag(const xml_segment& seg)
  //   {
  //     if (log_debug_) // -2 to align with start element
  //     {
  //       log_.debug(fmt::format("seg:{:5} {}/{}", seg.id(), indent(), tree_stack_.top().node.tag()));
  //     }
  //     tree_stack_.pop();
  //   }
  //   void xml_worker::prepare_tree_stack(const auto& xpaths_depth)
  //   {
  //     if (tree_stack_.size() > 1) [[unlikely]]
  //     { // rewinding the stack. should be empty anyway
  //       while (! tree_stack_.empty())
  //       {
  //         auto el = tree_stack_.top();
  //         log_.critical(fmt::format("tag: '{}'\n", el.node.tag()));
  //         tree_stack_.pop();
  //       }
  //       throw std::runtime_error(fmt::format("stack is not empty size: {}", tree_stack_.size()));
  //     }
  //     if (tree_stack_.size() == 0)
  //       tree_stack_.emplace(stack_struct{.node = xml_node{"top", "top_uri"}, .limits = fsp::p_limits(0, xpaths_depth)});
  //   }
  //   void xml_worker::obtain_value(const xml_segment& seg, const auto& /*xpaths*/, auto& res)
  //   {
  //     if (value_ndx_ != -1)
  //     { // we have value that we need to remember
  //       const auto* value = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader_.get()));
  //       // cstr_t      value_name = xpaths[value_ndx_].name();
  //       res.values()[value_ndx_].emplace_back(value != nullptr ? value : "");
  //       value_ndx_ = -1; // again undefined
  //       if (log_debug_)
  //       {
  //         log_.debug(
  //           fmt::format("++seg:{:5} {}name: {} tag: {} value: {}", seg.id(), indent(), value_ndx_, tree_stack_.top().node.tag(), value));
  //       }
  //     }
  //     else if (log_debug_) { log_.debug(fmt::format("--seg:{:5} {} tag: {} no value", seg.id(), indent(), tree_stack_.top().node.tag()));
  //     }
  //   }
  //   bool xml_worker::reset_reader(cstr_t xml_buf)
  //   {
  //     if (! reader_) { return false; }
  //     ++segment_counter_;
  //     // using existing reader
  //     int ret = xmlReaderNewMemory(reader_.get(), xml_buf.data(), static_cast<int>(xml_buf.size()), "noname.xml", nullptr,
  //     reader_flags_); if (ret == 0)
  //     {
  //       // Uspešno smo zamenjali vsebino
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_LOADDTD, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_DEFAULTATTRS, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_VALIDATE, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_SUBST_ENTITIES, 0);
  //       return true;
  //     }
  //     // Fallback - full new creation
  //     log_.warn("xmlReaderNewMemory reuse failed, doing full recreation");
  //     reader_.reset(xmlReaderForMemory(xml_buf.data(), static_cast<int>(xml_buf.size()), "noname.xml", nullptr, reader_flags_));
  //     if (! reader_) { throw std::runtime_error("Failed to create xmlTextReader"); }
  //     xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_LOADDTD, 0);
  //     xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_DEFAULTATTRS, 0);
  //     xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_VALIDATE, 0);
  //     xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_SUBST_ENTITIES, 0);
  //     return true;
  //   }
  //   bool xml_worker::open_tag(int& read_status, const xml_segment& seg, const auto& xpaths, const auto& limits, auto& res)
  //   {
  //     auto x = process_and_prune_node(xpaths, limits, res);
  //     if (! x)
  //     { // FIXME too deep is critical error handle it properly
  //       if (log_trace_) log_.trace(fmt::format("pruning: {} val:{}", x.error().err, tree_stack_.top().node.tag()));
  //       read_status = x.error().status;
  //       return false;
  //     }
  //     value_ndx_ = x.value().status;
  //     if (log_debug_)
  //     {
  //       log_.debug(fmt::format("**seg:{:5} {}{} value_ndx: {}", seg.id(), indent(), tree_stack_.top().node.tag(), value_ndx_));
  //     }
  //     return true;
  //   }
  //   segment_result xml_worker::loop(const xml_segment& seg, const fsp::xpath_set& xpaths, const xpath_limits& limits)
  //   {
  //     segment_result res(seg.id(), seg.subtree_type(), seg.doc_ndx());
  //     prepare_tree_stack(xpaths.size());
  //     value_ndx_      = -1; // xpath index of the value
  //     int read_status = xmlTextReaderRead(reader_.get());
  //     while (read_status == 1)
  //     {
  //       int  type      = xmlTextReaderNodeType(reader_.get());
  //       auto enum_type = static_cast<xmlReaderTypes>(type);
  //       depth_         = tree_stack_.size() - 1;
  //       switch (enum_type)
  //       {
  //       case XML_READER_TYPE_ELEMENT:
  //       { // open tag
  //         if (! open_tag(read_status, seg, xpaths, limits, res)) continue;
  //         break;
  //       }
  //       case XML_READER_TYPE_TEXT:
  //       { // obtain values
  //         obtain_value(seg, xpaths, res);
  //         break;
  //       }
  //       case XML_READER_TYPE_END_ELEMENT:
  //       { // close tag
  //         close_tag(seg);
  //         break;
  //       }
  //       case XML_READER_TYPE_NONE:
  //       case XML_READER_TYPE_ATTRIBUTE:
  //       case XML_READER_TYPE_CDATA:
  //       case XML_READER_TYPE_ENTITY_REFERENCE:
  //       case XML_READER_TYPE_ENTITY:
  //       case XML_READER_TYPE_PROCESSING_INSTRUCTION:
  //       case XML_READER_TYPE_COMMENT:
  //       case XML_READER_TYPE_DOCUMENT:
  //       case XML_READER_TYPE_DOCUMENT_TYPE:
  //       case XML_READER_TYPE_DOCUMENT_FRAGMENT:
  //       case XML_READER_TYPE_NOTATION:
  //       case XML_READER_TYPE_WHITESPACE:
  //       case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
  //       case XML_READER_TYPE_END_ENTITY:
  //       case XML_READER_TYPE_XML_DECLARATION: [[fallthrough]];
  //       default:
  //         auto type_name = magic_enum::enum_name(enum_type);
  //         auto msg       = fmt::format("nonsupported: {} {}", type_name, type);
  //         if (log_crit_) log_.critical(msg);
  //         throw std::runtime_error(msg);
  //       }
  //       read_status = xmlTextReaderRead(reader_.get());
  //     }
  //     tree_stack_.pop();
  //     return res;
  //   }
  //   result<segment_result> xml_worker::extract_xml_values(cstr_t xml_buf, const xml_segment& seg)
  //   {
  //     if (! reset_reader(xml_buf)) { throw std::runtime_error("Failed to reset reader for new segment"); }

  //     if (reader_)
  //     {
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_LOADDTD, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_DEFAULTATTRS, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_VALIDATE, 0);
  //       xmlTextReaderSetParserProp(reader_.get(), XML_PARSER_SUBST_ENTITIES, 0);

  //     } // 64KB
  //     auto        subtree_type = targets_.targets[seg.subtree_type()].original_ndx(); // seg.subtree_type();
  //     const auto& xpaths       = targets_.xpaths.at(subtree_type);
  //     assert(xpaths.size() != 0);
  //     const auto& limits = xpath_limits(targets_.xpaths.at(subtree_type));
  //     if (log_trace_)
  //     {
  //       log_.trace(fmt::format("subtree type: {}\n{}", subtree_type, xpaths.dump()));
  //       log_.trace(limits.dump());
  //     }
  //     return loop(seg, xpaths, limits);
  //   }
  //   std::expected<pp_result, err_result> xml_worker::process_and_prune_node( //
  //     const xpath_set&    xpaths,
  //     const xpath_limits& limits_vec,
  //     segment_result&     seg_result)
  //   {
  //     int         read_status = 0;
  //     const char* uri         = reinterpret_cast<const char*>(xmlTextReaderConstNamespaceUri(reader_.get()));
  //     const char* tag         = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader_.get()));
  //     const bool  log_trace   = log_.active(fsp::lvl_enum::trace);

  //     // Safely handle potential null pointers from libxml2
  //     cstr_t safe_uri = uri != nullptr ? uri : "";
  //     cstr_t safe_tag = tag != nullptr ? tag : "";
  //     auto             depth    = tree_stack_.size() - 1; // first available on stack
  //     pp_result        result;
  //     if (depth >= tree_stack_.size()) // guard to not go too deep
  //     {
  //       if (log_trace)
  //         log_.trace(fmt::format( //
  //           "pruning subtree: '{}' too deep: '{}' max allowed: '{}'",
  //           safe_tag,
  //           depth,
  //           tree_stack_.size()));
  //       read_status = xmlTextReaderNext(reader_.get()); // Skip all children, move to next sibling or the end of parent tag
  //       return std::unexpected(err_result{.status = read_status, .err = "Pruning, since it is too deep."});
  //     }
  //     auto limits = limits_vec[depth] & tree_stack_.top().limits; // xpaths excluded in previous level are excluded
  //                                                                 // also on current level

  //     result.node = fsp::xml_node{safe_uri, safe_tag};
  //     if (log_trace) //
  //       log_.trace(fmt::format("current tag: {} {} limits: {}", safe_tag, safe_uri, limits.dump()));
  //     /// prune if we are:
  //     /// - deeper than any xpath we are searching for (excluded before)
  //     /// - the tag name is smaller than any available tag name in the list of xpaths we are searching for
  //     /// - the tag name is bigger than any available tag name in the list of xpaths we are searching for
  //     auto first = limits.first();
  //     auto last  = limits.last();
  //     for (auto cnt = first; cnt <= last; ++cnt)
  //     {                                          // compare with all possible options on xpath[depth]
  //       if (! limits.available()[cnt]) continue; // It has been removed in earlier iterations
  //       const auto& xp = xpaths[cnt];
  //       if (depth >= xp.xpath().size())
  //       { // if there is shorter xpath then exclude current path and move to next xpath
  //         if (log_trace) log_.trace(fmt::format("shorter xpath: tag:{} depth:{} cnt: {} xpath-id:{}", safe_tag, depth, cnt, xp.name()));
  //         limits.available().reset(cnt);
  //         continue;
  //       }
  //       cstr_t xp_tag = xp.xpath()[depth].tag;
  //       cstr_t xp_uri = xp.xpath()[depth].ns;
  //       if (log_trace) log_.trace(fmt::format("tag:{} xp tag:{} depth:{} cnt: {}", safe_tag, xp_tag, depth, cnt));
  //       if ((safe_tag == xp_tag) && (safe_uri == xp_uri)) // remember the tag value index, attribute is handled inside
  //         result.status = std::max(process_positive_xpath_element(xp, cnt, depth, seg_result), result.status);
  //       else // current xpath tag does not match any tag on current level in xpaths searched
  //         limits.reset(cnt);
  //     } // for
  //     if (log_trace) log_.trace(fmt::format("limits: {}", limits.dump()));
  //     if (limits.available().none())      // this subtree is an dead end. prune it
  //       return std::unexpected(err_result{//
  //                                         .status = xmlTextReaderNext(reader_.get()),
  //                                         .err    = fmt::format("Pruning, in the middle '{}'", safe_tag)});
  //     if (result.status != -1 && ! xpaths[result.status].is_array())
  //       limits.reset(result.status); // we have this value and not searching in the future
  //     result.limits = limits;
  //     tree_stack_.emplace(result.node, result.limits);
  //     return result; // Indicates normal execution flow, no pruning happened
  //   }
  //   int xml_worker::process_positive_xpath_element( //
  //     const xml_attr& xp,
  //     std::size_t     ndx,
  //     std::size_t     depth,
  //     segment_result& seg_result) const
  //   {
  //     if (xp.is_last(depth)) [[unlikely]] // are we reached the end of the current xpath?
  //     {
  //       if (xp.is_attr()) [[unlikely]] // attribute xpath
  //       {
  //         auto value = process_attribute(xp);
  //         seg_result.values()[ndx].emplace_back(value);
  //         const bool log_debug = log_.active(fsp::lvl_enum::debug);
  //         if (log_debug) [[unlikely]]
  //           log_.debug(fmt::format("attribute name: '{}' tag: {} value: '{}'", xp.name(), xp.attr_name(), value));
  //         return -1;
  //       }
  //       return static_cast<int>(ndx);
  //     }
  //     return -1;
  //   }

  //   std::optional<str_t> xml_worker::get_attribute_value_ns(const str_t& local_name, const str_t& namespace_uri) const
  //   {
  //     const xmlChar* ln                   = BAD_CAST local_name.c_str();
  //     int                          status = 0;

  //     if (namespace_uri.empty()) // attributes with default prefix/uri
  //     {
  //       status = xmlTextReaderMoveToAttribute(reader_.get(), ln); // e.g. Ccy="EUR"
  //     }
  //     else
  //     { // attriute with ns (e.g. ns:Ccy="EUR")
  //       const xmlChar* ns = BAD_CAST namespace_uri.c_str();
  //       status            = xmlTextReaderMoveToAttributeNs(reader_.get(), ln, ns);
  //     }
  //     if (status == 1)
  //     { // read the attribute value is attribute is found
  //       const xmlChar* val = xmlTextReaderConstValue(reader_.get());
  //       str_t    result(val != nullptr ? reinterpret_cast<const char*>(val) : "");
  //       xmlTextReaderMoveToElement(reader_.get()); // return focus to current element
  //       return result;
  //     }
  //     return std::nullopt;
  //   }
  //   str_t xml_worker::process_attribute(const xml_attr& xp) const
  //   {
  //     auto local_name = str_t(xp.attr_name());
  //     auto uri        = str_t(xp.attr_uri());
  //     auto value      = get_attribute_value_ns(local_name, uri);
  //     if (value.has_value()) [[likely]]
  //       return value.value(); // non null value
  //     throw std::runtime_error(fmt::format("attribute '{}:{}' has no value.", local_name, uri));
  //   }
  /**
   * @brief obdela EN segment iz pool-a (za hibridni pipeline_worker)
   * V nasprotju z operator() ne pozna svoje zanke po pool_.pop_segment_ndx() —
   * idx mora klicatelj že imeti (npr. iz pool_.try_pop_ready() ali pool_.pop_segment_ndx()).
   * @param idx indeks segmenta v pool-u
   */
  int xml_worker::process_one(std::size_t idx)
  {
    const xml_segment seg = pool_.retrieve_segment(idx); // slot se sprosti tukaj, ne glede na izid spodaj

    // Umik: dokument je bil medtem validiran kot neveljaven -> prihranimo SAX ekstrakcijo.
    if (ds_dscr_[seg.doc_ndx()].status() == doc_status::validation_failed)
    {
      if (log_debug_)
        log_.debug(fmt::format("Segment {} (doc {}): dokument neveljaven, procesiranje preskočeno.", seg.id(), seg.doc_ndx()));
      return seg.doc_ndx();
    }

    if (auto res = process_segment(seg))
    {
      pipeline_.record_segment_done(seg, *res, hooks_);
      if (loc_res_ok_.size() + 1 == loc_res_ok_.capacity()) loc_res_ok_.reserve(loc_res_ok_.size() * 2);
      loc_res_ok_.emplace_back(std::move(*res));
    }
    else
    {
      // res holds an error_info here, not a segment_result -- *res would be UB (dereferencing a
      // disengaged std::expected). Record the failure with the id-only constructor instead.
      pipeline_.record_segment_failed(static_cast<std::size_t>(seg.doc_ndx()), seg.id());
      if (loc_res_nak_.size() + 1 == loc_res_nak_.capacity()) loc_res_nak_.reserve(loc_res_nak_.size() * 2);
      loc_res_nak_.emplace_back(seg.id(), seg.subtree_type(), seg.doc_ndx());
    }
    return seg.doc_ndx();
  }
  /**
   * @brief prenese lokalno nabrane rezultate v skupne results_/errors_ (kliče pipeline_worker ob koncu niti)
   */
  void xml_worker::flush_results()
  {
    {
      std::lock_guard lock(results_mutex_);
      results_.append_range(std::move(loc_res_ok_));
    }
    {
      std::lock_guard lock(errors_mutex_);
      errors_.append_range(std::move(loc_res_nak_));
    }
    loc_res_ok_.clear();
    loc_res_nak_.clear();
  }
} // namespace fsp