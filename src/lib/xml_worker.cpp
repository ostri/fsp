#include "xml_worker.hpp"
#include "x_str.hpp"
#include "xpath_helpers.hpp"
#include "xml_node.hpp"
#include "xpath_limits.hpp"
#include <chrono>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <libxml/xmlreader.h>
#include <stack>
// #include <thread>

namespace
{

} // namespace

namespace fsp
{
  xml_worker::xml_worker(segment_queue&               seg_queue,
                         const mmap_file&             xml_mmap,
                         std::vector<segment_result>& results,
                         std::vector<segment_result>& errors,
                         std::mutex&                  results_mutex,
                         std::mutex&                  errors_mutex,
                         std::atomic<std::size_t>&    processed_count,
                         std::atomic<std::size_t>&    error_count,
                         std::atomic<bool>&           cancel_flag,
                         const fsp_logger&            log,
                         const proc_data&             targets)
  : log_(log)
  , seg_queue_(seg_queue)
  , xml_mmap_(xml_mmap)
  , results_(results)
  , errors_(errors)
  , results_mutex_(results_mutex)
  , errors_mutex_(errors_mutex)
  , processed_count_(processed_count)
  , error_count_(error_count)
  , cancel_flag_(cancel_flag)
  , targets_(targets)
  {
  }

  void xml_worker::operator()([[maybe_unused]] const std::stop_token& st, int worker_id)
  {
    auto t0              = std::chrono::steady_clock::now();
    log_thread_name      = fmt::format("wrk{:03}", worker_id);
    const bool log_debug = log_.active(fsp::lvl_enum::debug);

    if (log_debug) log_.debug(fmt::format("Worker thread '{}' started.", log_thread_name));

    thread_local std::size_t txn_processed = 0; // number of segments proessed in this thread

    while (! cancel_flag_.load())
    {
      xml_segment seg{};
      if (! seg_queue_.pop(seg)) break;

      auto res = process_segment(seg);
      txn_processed++;

      if (res)
      {
        std::lock_guard lock(results_mutex_);
        results_.push_back(std::move(*res));
        processed_count_++;
      }
      else
      {
        std::lock_guard lock(errors_mutex_);
        errors_.push_back(std::move(*res)); // ali posebej konstruiran error result
        error_count_++;
      }
    }

    if (log_debug)
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      log_.debug(fmt::format("Worker thread '{}' finished in {} ms, txn processed: {}.", log_thread_name, duration, txn_processed));
    }
  }

  result<segment_result> xml_worker::process_segment(const xml_segment& seg)
  {
    auto       t0        = std::chrono::steady_clock::now();
    const bool log_debug = log_.active(fsp::lvl_enum::debug);
    try
    {
      if (log_debug)
      {
        log_.debug(fmt::format("segment started: {}", seg.dump()));
        log_.trace(fmt::format("{}", seg.dump_all(xml_mmap_.data(), 0)));
      }
      auto view     = seg.view(xml_mmap_.data());
      auto tmp_view = seg.subtree_str(view);
      auto r        = extract_xml_values(tmp_view, seg);

      if (r)
      {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (log_debug)
        {
          log_.debug(fmt::format("Segment '{}' DOM processing finished '{}'µs (offset={}, len={})", //
                                 seg.id(),
                                 us,
                                 seg.offset(),
                                 seg.length()));
          log_.trace(fmt::format("{}", r->dump()));
        }
        segment_result res(seg.id(), seg.subtree_type(), r.value().values());
        return res;
      }

      error_info err( //
        processor_error::error_extracting_xpath_values,
        fmt::format("Error extracting xpath values: {}", r.error().message()),
        "",
        0UL);
      if (log_.active(fsp::lvl_enum::warn))
        log_.warn(fmt::format("Segment {}: {} :: {}", seg.id(), r.error().message(), seg.dump_all(xml_mmap_.data())));
      return std::unexpected(err);
    }
    catch (const std::exception& e)
    {
      // res.success       = false;
      auto error_message = fmt::format("Exception in segment {}: '{}'", seg.id(), e.what());
      log_.error(fmt::format("{}", error_message));
      throw;
    }
  }
  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  result<segment_result> xml_worker::extract_xml_values(cstr_t xml_buf, const xml_segment& seg)
  {
    segment_result res(seg.id(), seg.subtree_type());
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    auto flags = (XML_PARSE_NOCDATA | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS | XML_PARSE_NONET);

    //    UniqueXmlTextReader reader(xmlReaderForMemory(xml_buf.data(), static_cast<int>(xml_buf.size()), nullptr, nullptr, flags));
    reader_ = UniqueXmlTextReader(xmlReaderForMemory(xml_buf.data(), static_cast<int>(xml_buf.size()), nullptr, nullptr, flags));

    int                      read_status;
    auto                     depth     = 0UL;
    const char               pad       = '.';
    const auto&              log       = log_;
    const bool               log_trace = log.active(fsp::lvl_enum::trace);
    const bool               log_debug = log.active(fsp::lvl_enum::debug);
    const bool               log_crit  = log.active(fsp::lvl_enum::crit);
    std::stack<stack_struct> tree_stack;
    auto                     subtree_type = targets_.targets[seg.subtree_type()].original_ndx(); // seg.subtree_type();
    const auto&              xpaths       = targets_.xpaths.at(subtree_type);
    assert(xpaths.size() != 0);
    const auto& limits = xpath_limits(targets_.xpaths.at(subtree_type));
    if (log_trace)
    {
      log.trace(fmt::format("subtree type: {}\n{}", subtree_type, xpaths.dump()));
      log.trace(limits.dump());
    }
    // ---------------------------------------------------------------------------------------------
    read_status = xmlTextReaderRead(reader_.get());
    tree_stack.emplace(stack_struct{.node = xml_node{"top", "top_uri"}, .limits = fsp::p_limits(0, xpaths.size())});
    int                      value_ndx          = -1; // xpath index of the value
    const std::size_t        initial_tree_depth = 50;
    thread_local std::string indent(initial_tree_depth * 2, pad);
    while (read_status == 1)
    {
      int  type      = xmlTextReaderNodeType(reader_.get());
      auto enum_type = static_cast<xmlReaderTypes>(type);
      depth          = tree_stack.size() - 1;
      switch (enum_type)
      {
      case XML_READER_TYPE_ELEMENT:
      { // open tag
        auto x = process_and_prune_node(xpaths, tree_stack, limits, res);
        if (! x)
        { // FIXME too deep is critical error handle it properly
          if (log_trace) log.trace(fmt::format("pruning: {} val:{}", x.error().err, tree_stack.top().node.tag()));
          read_status = x.error().status;
          continue;
        }
        value_ndx = x.value().status;
        if (log_debug)
        {
          // if (indent.size() < depth * 2) indent.assign(depth * 2 * 2, pad);
          log.debug(
            fmt::format("**seg:{:5} {}{} value_ndx: {}", seg.id(), indent.substr(0, depth * 2), tree_stack.top().node.tag(), value_ndx));
        }
        break;
      }
      case XML_READER_TYPE_TEXT:
      { // obtain values
        if (value_ndx != -1)
        { // we have value that we need to remember
          const auto* value      = reinterpret_cast<const char*>(xmlTextReaderConstValue(reader_.get()));
          cstr_t      value_name = xpaths[value_ndx].name();
          res.values()[value_name].emplace_back(value != nullptr ? value : "");
          value_ndx = -1; // again undefined
          if (log_debug)
          {
            if (indent.size() < depth * 2) indent.assign(depth * 2 * 2, pad);
            log.debug(fmt::format("++seg:{:5} {}name: {} tag:'{}' value: {}",
                                  seg.id(),
                                  indent.substr(0, (depth * 2) + 2),
                                  value_name,
                                  tree_stack.top().node.tag(),
                                  value));
          }
        }
        else if (log_debug)
        {
          // if (indent.size() < depth * 2) indent.assign(depth * 2 * 2, pad);
          log.debug(fmt::format("--seg:{:5} {} tag: {} no value", seg.id(), indent.substr(0, depth * 2), tree_stack.top().node.tag()));
        }
        break;
      }
      case XML_READER_TYPE_END_ELEMENT:
      {                // close tag
        if (log_debug) // -2 to align with start element
        {
          // if (indent.size() < depth * 2) indent.assign(depth * 2 * 2, pad);
          log.debug(fmt::format("seg:{:5} {}/{}", seg.id(), indent.substr(0, depth * 2), tree_stack.top().node.tag()));
        }
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
        if (log_crit) log.critical(fmt::format("nonsupported: {} {}", type_name, type));
      }
      read_status = xmlTextReaderRead(reader_.get());
    }
    return res;
  }
  std::expected<pp_result, err_result> xml_worker::process_and_prune_node( //
    const fsp::xpath_node_struct& xpaths,
    std::stack<stack_struct>&     stack,
    const fsp::xpath_limits&      limits_vec,
    fsp::segment_result&          seg_result)
  {
    int         read_status = 0;
    const char* uri         = reinterpret_cast<const char*>(xmlTextReaderConstNamespaceUri(reader_.get()));
    const char* tag         = reinterpret_cast<const char*>(xmlTextReaderConstLocalName(reader_.get()));
    const bool  log_trace   = log_.active(fsp::lvl_enum::trace);

    // Safely handle potential null pointers from libxml2
    std::string_view safe_uri = uri != nullptr ? uri : "";
    std::string_view safe_tag = tag != nullptr ? tag : "";
    auto             depth    = stack.size() - 1; // first available on stack
    pp_result        result;
    if (depth >= stack.size()) // guard to not go too deep
    {
      if (log_trace) log_.trace(fmt::format("pruning subtree: '{}' too deep: '{}' max allowed: '{}'", safe_tag, depth, stack.size()));
      read_status = xmlTextReaderNext(reader_.get()); // Skip all children, move to sibling or the end of parent tag
      return std::unexpected(err_result{.status = read_status, .err = "Pruning, since it is too deep."});
    }
    auto limits = limits_vec[depth] & stack.top().limits; // xpaths excluded in previous level are excluded
                                                          // also on current level

    result.node = fsp::xml_node{safe_uri, safe_tag};
    if (log_trace) //
      log_.trace(fmt::format("current tag: {} {} limits: {}", safe_tag, safe_uri, limits.dump()));
    /// prune if we are:
    /// - deeper than any xpath we are searching for (excluded before)
    /// - the tag name is smaller than any available tag name in the list of xpaths we are searching for
    /// - the tag name is bigger than any available tag name in the list of xpaths we are searching for
    auto first = limits.first();
    auto last  = limits.last();
    // if (log_trace) log.trace(fmt::format("{}", xpaths.dump()));
    for (auto cnt = first; cnt <= last; ++cnt)
    {                                          // compare with all possible options on xpath[depth]
      if (! limits.available()[cnt]) continue; // It has been removed in earlier iterations
      const auto& xp = xpaths[cnt];
      if (depth >= xp.xpath().size())
      { // if there is shorter xpath then exclude current path and move to next xpath
        if (log_trace) log_.trace(fmt::format("shorter xpath: tag:{} depth:{} cnt: {} xpath-id:{}", safe_tag, depth, cnt, xp.name()));
        limits.available().reset(cnt);
        continue;
      }
      std::string_view xp_tag = xp.xpath()[depth].tag;
      std::string_view xp_uri = xp.xpath()[depth].ns;
      if (log_trace) log_.trace(fmt::format("tag:{} xp tag:{} depth:{} cnt: {}", safe_tag, xp_tag, depth, cnt));
      if ((safe_tag == xp_tag) && (safe_uri == xp_uri)) // remember the tag value index, attribute is handled inside
        result.status = std::max(process_positive_xpath_element(xp, cnt, depth, seg_result), result.status);
      else // current xpath tag does not match any tag on current level in xpaths searched
        limits.reset(cnt);
    } // for
    if (log_trace) log_.trace(fmt::format("limits: {}", limits.dump()));
    if (limits.available().none())      // this subtree is an dead end. prune it
      return std::unexpected(err_result{//
                                        .status = xmlTextReaderNext(reader_.get()),
                                        .err    = fmt::format("Pruning, in the middle '{}'", safe_tag)});
    if (result.status != -1 && ! xpaths[result.status].is_array())
      limits.reset(result.status); // we have this value and not searching in the future
    result.limits = limits;
    stack.emplace(result.node, result.limits);
    return result; // Indicates normal execution flow, no pruning happened
  }
  int xml_worker::process_positive_xpath_element( //
    const fsp::xml_attr& xp,
    std::size_t          ndx,
    std::size_t          depth,
    fsp::segment_result& seg_result)
  {
    const bool log_debug = log_.active(fsp::lvl_enum::debug);
    if (xp.is_last(depth)) // are we reached the end of the current xpath?
    {
      if (xp.is_attr()) // attribute xpath
      {
        auto value = process_attribute(xp);
        seg_result.values()[xp.name()].emplace_back(value);
        if (log_debug) log_.debug(fmt::format("attribute name: '{}' tag: {} value: '{}'", xp.name(), xp.attr_name(), value));
        return -1;
      }
      return static_cast<int>(ndx);
    }
    return -1;
  }
  std::optional<std::string> xml_worker::get_attribute_value_ns(const str_t& local_name, const str_t& namespace_uri)
  {
    xmlChar* value = xmlTextReaderGetAttributeNs( //
      reader_.get(),
      BAD_CAST local_name.c_str(),
      ! namespace_uri.empty() ? BAD_CAST namespace_uri.c_str() : nullptr);
    if (value == nullptr) return std::nullopt;
    std::string result(reinterpret_cast<char*>(value));
    xmlFree(value); // pointer must be released
    return result;
  }
  std::string xml_worker::process_attribute(const auto& xp)
  {
    auto local_name = std::string(xp.attr_name());
    auto uri        = std::string(xp.attr_uri());
    auto value      = get_attribute_value_ns(local_name, uri);
    if (value.has_value()) return value.value(); // non null value
    throw std::runtime_error(fmt::format("attribute '{}:{}' has no value.", local_name, uri));
  }
} // namespace fsp