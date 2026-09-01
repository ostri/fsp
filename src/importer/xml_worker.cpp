#include "xml_worker.hpp"
#include "parsing_util.hpp"
#include "pipeline.hpp"
#include "segment_result.hpp"
#include "xpath_helpers.hpp"
#include <algorithm>
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
    segment_pool&         pool,    // reference to segment pool
    const doc_set_dscr&   ds_dscr, // reference to document set structure
    const logger::Logger& log,     // reference to logger
    const proc_data&      targets, // structure that holds information about cutting points and xpaths of the values we are looking for
    str_t                 parent_log_name, // parent thread log thread name
    pipeline&             pl,
    pipeline_hooks&       hooks,
    std::size_t           ok_block_flush_size,
    std::size_t           nak_block_flush_size)
  : log_(log)
  , ds_dscr_(ds_dscr)
  , targets_(targets)
  , pipeline_(pl)
  , hooks_(hooks)
  , parent_log_name_(std::move(parent_log_name))
  , sax_(std::make_unique<segment_sax>(log_))
  , pool_(pool)
  , ok_block_flush_size_(ok_block_flush_size)
  , nak_block_flush_size_(nak_block_flush_size)
  {
    ok_block_indices_.reserve(ok_block_flush_size_);
    nak_block_indices_.reserve(nak_block_flush_size_);
    nak_block_errors_.reserve(nak_block_flush_size_);
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

  //       const xml_segment seg = pool_.retrieve_segment(idx); // LOCAL COPY!
  //       if (auto res = process_segment(seg))
  //       {
  //         if (loc_res_ok.size() + 1 == loc_res_ok.capacity()) loc_res_ok.reserve(loc_res_ok.size() * 2);
  //         loc_res_ok.emplace_back(std::move(*res));
  //       }
  //       else
  //       {
  //         if (loc_res_nak.size() + 1 == loc_res_nak.capacity()) loc_res_nak.reserve(loc_res_nak.size() * 2);
  //         loc_res_nak.emplace_back(std::move(*res)); // adjust based on your error type
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
        fmt::format("Error extracting xpath values: {}", "unspecified"),
        "",
        0UL);
      if (log_warn_) log_.warn(fmt::format("Segment {}: {} :: ", seg.id(), "unspecified"));
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
  //       // Successfully swapped the content
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
   * @brief processes ONE segment from the pool (for the hybrid pipeline_worker)
   * Unlike operator(), it does not run its own loop over pool_.pop_segment_ndx() --
   * the caller must already have idx (e.g. from pool_.try_pop_ready() or pool_.pop_segment_ndx()).
   * Unlike the old pool_.retrieve_segment()-based version, idx is NOT released back to pool_
   * here -- it stays "locked" (see segment_pool::segment_at()'s own doc comment) until
   * flush_ok_block()/flush_nak_block() releases it, once a on_block_safe_store()/
   * on_failed_block_safe_store() hook has had a chance to read it.
   * @param idx index of the segment in the pool
   */
  int xml_worker::process_one(std::size_t idx)
  {
    xml_segment& seg = pool_.segment_at(idx);

    // Bail out: the document was meanwhile found rejected (syntax, validation, or doc-level
    // semantic already known invalid -- doc_dscr::rejected(), NOT status().status() directly,
    // which is ALSO three_state::unknown -- not three_state::invalid -- for a document nothing has
    // reported on yet) -> skip the SAX extraction for THIS segment, same as before. Unlike before,
    // also drops every OTHER segment of this same document that this thread already buffered
    // (ok_block_indices_/nak_block_indices_) earlier, before the rejection became known -- a
    // rejected document's segments are never going to be written anywhere a cb keeps (see e.g.
    // ach's own write_docs_row_close(), which deletes them right back out on doc-close anyway), so
    // dropping them here, before they ever reach a storage hook, is strictly better than writing
    // and then deleting them. last_cleaned_doc_ndx_ remembers which document this thread's buffers
    // were last cleaned for, so a run of several consecutive segments of the SAME rejected document
    // (this thread hasn't reached the end of its own ready queue yet) only pays for the
    // ok_block_indices_/nak_block_indices_ scan once -- the moment this thread processes a segment
    // of any OTHER document (rejected or not), last_cleaned_doc_ndx_ no longer matches and the next
    // rejected-document segment (of THAT other document, or of this one again later) triggers a
    // fresh drop_doc() pass. This segment (idx) itself still goes through record_nak() below (as a
    // failure) exactly as before -- it was never added to either buffer yet, so drop_doc() above
    // cannot have touched it.
    if (ds_dscr_[seg.doc_ndx()].rejected())
    {
      const auto doc_ndx = static_cast<std::size_t>(seg.doc_ndx());
      if (last_cleaned_doc_ndx_ != doc_ndx)
      {
        drop_doc(ok_block_indices_, doc_ndx);
        drop_doc(nak_block_indices_, doc_ndx, &nak_block_errors_);
        last_cleaned_doc_ndx_ = doc_ndx;
      }
      if (log_debug_) log_.debug(fmt::format("Segment {} (doc {}): document invalid, skipped processing.", seg.id(), seg.doc_ndx()));
      pipeline_.record_segment_failed(static_cast<std::size_t>(seg.doc_ndx()), seg.id(), hooks_);
      // Captured BEFORE record_nak() -- see the doc_ndx capture below (the general rule this
      // follows) for why seg must never be read again after a record_ok()/record_nak() call.
      const int rejected_doc_ndx = seg.doc_ndx();
      record_nak(
        idx,
        seg,
        error_info{processor_error::syntax_error, "document invalid, segment skipped", "", static_cast<std::size_t>(seg.doc_ndx())});
      return rejected_doc_ndx;
    }

    if (auto res = process_segment(seg))
    {
      const bool semantically_ok = pipeline_.check_segment_semantics(seg, *res, hooks_);
      // pool_.result_at(idx) is the only place *res needs to live -- a later on_block_safe_store()/
      // on_failed_block_safe_store() hook reads it from there.
      pool_.result_at(idx) = std::move(*res);
      // Captured BEFORE record_ok()/record_nak() below, and used instead of a second seg.doc_ndx()
      // read afterwards: those calls can flush this segment straight into storage (see their own
      // doc comment below) and, once flushed, release idx back to segment_pool's shared
      // free_queues_ from INSIDE the call -- another thread's acquire_slot()/set_segment() (a
      // cutter starting a new segment on the very same, just-freed slot) can then race a later read
      // through seg, which is only ever a reference into that same pool slot, not a copy. Confirmed
      // by direct experiment: capturing doc_ndx here and never reading seg again afterwards is what
      // makes the data race TSan reports on xml_segment::operator=()/doc_ndx() (segment_pool.hpp's
      // set_segment() racing this read) disappear.
      const int captured_doc_ndx = seg.doc_ndx();
      // record_ok()/record_nak() below can flush this segment straight into storage
      // (on_block_safe_store()/on_failed_block_safe_store(), once ok_block_flush_size_/
      // nak_block_flush_size_ is reached) -- deliberately BEFORE pipeline_::finish_segment(), which
      // is what can trigger hooks.on_doc_safe_close() for this segment's own document. Flushing
      // first guarantees a document's last segment is durably stored before on_doc_close() ever
      // sees that document as finished (see pipeline::check_segment_semantics()'s own doc comment
      // for why the old, single-call record_segment_done() could not give that guarantee).
      if (semantically_ok) record_ok(idx, seg);
      else record_nak(idx, seg, error_info::semantic("on_seg_sem_check", "segment failed semantic validation"));
      pipeline_.finish_segment(static_cast<std::size_t>(captured_doc_ndx), semantically_ok, hooks_);
      return captured_doc_ndx;
    }
    else // NOLINT(readability-else-after-return, llvm-else-after-return) -- res (process_segment()'s std::expected) is only in scope
         // inside this if/else's init-statement
    {
      // res holds an error_info here, not a segment_result -- *res would be UB (dereferencing a
      // disengaged std::expected). Record the failure with the id-only constructor instead.
      pipeline_.record_segment_failed(static_cast<std::size_t>(seg.doc_ndx()), seg.id(), hooks_);
      // Captured BEFORE record_nak() -- see the doc_ndx capture above for why.
      const int failed_doc_ndx = seg.doc_ndx();
      record_nak(idx, seg, res.error());
      return failed_doc_ndx;
    }
  }

  // Common tail of every "this segment turned out OK" path above: marks seg valid and appends to
  // ok_block_indices_ (see flush_ok_block()), flushing the latter once ok_block_flush_size_ is
  // reached. The segment's own result_values already lives in pool_.result_at(idx) (copied by
  // process_one() before this call) -- no separate accumulator keeps a second copy.
  //
  // drop_rejected() below only runs at the threshold check, not on every single push_back() -
  // O(1) amortized per segment at scale (millions of segments per run), rather than an O(block
  // size) scan on every one of them. Once the block is actually full, a rejected segment (if any)
  // is compacted out FIRST, and the threshold re-checked: if the block is no longer full after
  // that (a rejected document's segments were the only thing pushing it over), this returns
  // WITHOUT flushing - there is room again, so the next record_ok() call simply keeps filling it,
  // same as if the threshold had never been reached this time. Only an actually-full, actually-
  // clean block ever reaches the real store call below.
  void xml_worker::record_ok(std::size_t idx, xml_segment& seg)
  {
    seg.set_valid(true);
    ok_block_indices_.push_back(idx);
    if (ok_block_indices_.size() < ok_block_flush_size_) return;
    drop_rejected(ok_block_indices_);
    if (ok_block_indices_.size() >= ok_block_flush_size_) flush_ok_block();
  }

  // Common tail of every "this segment failed" path above - see record_ok()'s own doc comment,
  // mirrored for the nak side (plus nak_block_errors_, parallel to nak_block_indices_). NOT
  // drop_rejected() here - see flush_nak_block()'s own doc comment on why the nak side is never
  // dropped for a rejected document (diagnostic writes must survive regardless of verdict).
  void xml_worker::record_nak(std::size_t idx, xml_segment& seg, error_info err)
  {
    seg.set_valid(false);
    nak_block_indices_.push_back(idx);
    nak_block_errors_.push_back(std::move(err));
    if (nak_block_indices_.size() >= nak_block_flush_size_) flush_nak_block();
  }

  // Both flush_ok_block()/flush_nak_block() below share the same shape: call the batch storage
  // hook, and -- ONLY if it reports success -- fold this batch's indices into each represented
  // document's own "segments stored" count (see pipeline::record_segments_stored()'s own doc
  // comment for why this must be grouped by doc_ndx rather than treated as one flat batch count:
  // a single batch can freely mix segments from several different documents, since a P-role
  // thread processes whatever segment is next ready, regardless of which document it belongs to).
  // A batch whose own store call failed must NOT count its segments as stored -- see
  // on_block_safe_store()'s own doc comment in pipeline_hooks.hpp for why that hook's error is
  // propagated here rather than merely logged, unlike every other _safe_ hook.
  void xml_worker::record_segments_stored_by_doc(std::span<const std::size_t> indices)
  {
    // by_doc accumulates "how many of THIS batch's segments belong to doc_ndx" -- a plain,
    // thread-local vector<pair<...>> is fine here (no sharing across threads): most batches touch
    // only a handful of distinct documents, so a linear scan-and-bump is cheaper than a map for
    // realistic ok_block_flush_size_/nak_block_flush_size_ values.
    std::vector<std::pair<std::size_t, std::size_t>> by_doc; // (doc_ndx, count)
    for (const std::size_t idx : indices)
    {
      const auto doc_ndx = static_cast<std::size_t>(pool_.segment_at(idx).doc_ndx());
      auto       it      = std::ranges::find_if(by_doc, [doc_ndx](const auto& p) { return p.first == doc_ndx; });
      if (it == by_doc.end()) by_doc.emplace_back(doc_ndx, 1);
      else ++it->second;
    }
    for (const auto& [doc_ndx, count] : by_doc) pipeline_.record_segments_stored(doc_ndx, count, hooks_);
  }

  // See its own doc comment in xml_worker.hpp. Swap-and-pop, not a stable compact: indices/errs
  // have no meaningful order to preserve (a block is just whatever segments happened to finish
  // processing, in no particular sequence), so an element found to belong to doc_ndx is replaced
  // by the CURRENT last element instead of shifting every element after it down by one -- lo only
  // advances past an element once it is confirmed to belong elsewhere, hi only ever shrinks, so
  // each element is inspected at most once.
  void xml_worker::drop_doc(std::vector<std::size_t>& indices, std::size_t doc_ndx, std::vector<error_info>* errs)
  {
    std::size_t lo = 0;
    std::size_t hi = indices.size(); // one past the last still-live element
    while (lo < hi)
    {
      const auto  idx = indices[lo];
      const auto& seg = pool_.segment_at(idx);
      if (static_cast<std::size_t>(seg.doc_ndx()) != doc_ndx)
      {
        ++lo;
        continue;
      }
      // errs != nullptr means indices is nak_block_indices_ (see this method's own doc comment,
      // xml_worker.hpp, on the errs/indices pairing convention) - the only side a HEADER segment's
      // own diagnostic nak (error_class::he - check_segment_semantics() already recorded it, via
      // record_nak(), before doc_ndx could possibly turn rejected() for a reason unrelated to this
      // segment - see that same doc comment) could ever be sitting in, still unflushed, at the
      // moment THIS call runs. Since pipeline::report_error_class() now calls mark_rejected()
      // unconditionally for every error_class including HE (not just UA/SE/VE, see its own doc
      // comment in pipeline.cpp), rejected() can now turn true for THIS document from the header
      // segment's own failure alone, arriving here (via a LATER, still-in-flight transaction
      // segment's own process_one() call, see that method's own doc comment) before the header's
      // own nak has ever been flushed - dropping it here would silently discard a legitimate,
      // already-decided diagnostic finding this class's own header exists to report, not "collateral
      // damage" from a document that is unusable start to finish the way UA/SE/VE's own segments
      // are. targets_.is_header[] is the same declaration-order lookup check_segment_semantics()
      // itself already consults for the identical question.
      // subtree_type() is always a valid index into is_header, set by doc_cutter at cut time
      const auto subtree_type = static_cast<std::size_t>(seg.subtree_type());
      const bool is_header_segment =
        targets_.is_header[subtree_type]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      if (errs != nullptr && is_header_segment)
      {
        ++lo;
        continue;
      }
      pool_.release_slots(std::span{&idx, 1});
      --hi;
      indices[lo] = indices[hi];
      if (errs != nullptr) (*errs)[lo] = std::move((*errs)[hi]);
      // lo deliberately NOT advanced -- the element just moved into indices[lo] (from the old
      // indices[hi]) has not been inspected yet.
    }
    indices.resize(hi);
    if (errs != nullptr) errs->resize(hi);
  }

  // Same swap-and-pop compaction as drop_doc() above, but the removal predicate is "this
  // segment's own document is rejected()" rather than "this segment's own document is doc_ndx" -
  // see this method's own doc comment in xml_worker.hpp for why a whole-block scan is needed here,
  // not just drop_doc()'s own single-document one.
  void xml_worker::drop_rejected(std::vector<std::size_t>& indices, std::vector<error_info>* errs)
  {
    std::size_t lo = 0;
    std::size_t hi = indices.size();
    while (lo < hi)
    {
      const auto idx = indices[lo];
      if (! ds_dscr_[static_cast<std::size_t>(pool_.segment_at(idx).doc_ndx())].rejected())
      {
        ++lo;
        continue;
      }
      pool_.release_slots(std::span{&idx, 1});
      --hi;
      indices[lo] = indices[hi];
      if (errs != nullptr) (*errs)[lo] = std::move((*errs)[hi]);
    }
    indices.resize(hi);
    if (errs != nullptr) errs->resize(hi);
  }

  // NOT called with drop_rejected() run again here: record_ok() (the only threshold-triggered
  // caller) already ran it once, right before deciding to call this - see its own doc comment for
  // why paying that scan again here, unconditionally, on every flush would defeat the whole point
  // (amortized O(1) per segment at millions-of-segments scale). flush_results() (the OTHER
  // caller, at thread end) calls drop_rejected() itself, right before this, for the exact same
  // reason record_ok() does - see its own doc comment.
  void xml_worker::flush_ok_block()
  {
    if (! ok_block_indices_.empty())
    {
      if (auto res = hooks_.on_block_safe_store(ok_block_indices_, pool_, ds_dscr_); res) record_segments_stored_by_doc(ok_block_indices_);
      else if (log_error_)
        log_.error(
          fmt::format("on_block_store() failed for a batch of {} segment(s): {}", ok_block_indices_.size(), res.error().to_string()));
    }
    pool_.release_slots(ok_block_indices_);
    ok_block_indices_.clear();
  }

  void xml_worker::flush_nak_block()
  {
    // NOT drop_rejected() here: nak_block_indices_ holds diagnostic writes (write_failed_segment()-
    // style findings a cb wants to keep regardless of the owning document's eventual verdict, e.g.
    // ach's own NT-D9/NT-D10 unknown-BIC header diagnostics) - see drop_doc()'s own doc comment
    // (xml_worker.hpp) on why the nak side is deliberately never dropped for a rejected document,
    // only the ok side is.
    if (! nak_block_indices_.empty())
    {
      if (auto res = hooks_.on_failed_block_safe_store(nak_block_indices_, nak_block_errors_, pool_, ds_dscr_); res)
        record_segments_stored_by_doc(nak_block_indices_);
      else if (log_error_)
        log_.error(fmt::format(
          "on_failed_block_store() failed for a batch of {} segment(s): {}", nak_block_indices_.size(), res.error().to_string()));
    }
    pool_.release_slots(nak_block_indices_);
    nak_block_indices_.clear();
    nak_block_errors_.clear();
  }

  /**
   * @brief flushes whatever remains in the ok/nak blocks (see flush_ok_block()/flush_nak_block())
   * -- called by pipeline_worker at thread end.
   *
   * drop_rejected() runs here explicitly, right before flush_ok_block(), since this is the ONE
   * caller of flush_ok_block() that does not go through record_ok()'s own threshold check (which
   * is where every OTHER call already ran it - see record_ok()'s own doc comment) - this is fsp's
   * last chance to compact a leftover rejected segment out of ok_block_indices_ before it is
   * unconditionally flushed as-is.
   */
  void xml_worker::flush_results()
  {
    drop_rejected(ok_block_indices_);
    flush_ok_block();
    flush_nak_block();
  }
} // namespace fsp