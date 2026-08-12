#pragma once

#include "doc_set_dscr.hpp"
#include "error_info.hpp"
#include "lock_queue.hpp"
#include "parsing_util.hpp"
#include "pipeline_hooks.hpp"
#include "segment_result.hpp"
#include <logger/logger.hpp>
#include "xml_segment.hpp"
#include "xpath_helpers.hpp"
#include "segment_sax.hpp"
#include "segment_pool.hpp"
// #include <libxml/xmlreader.h>
// #include <stack>
#include <vector>
#include <mutex>

namespace fsp
{
  class pipeline; // forward declaration is enough -- only a reference is used here

  using str_t         = std::string;
  using segment_queue = lock_queue<xml_segment>;
  //   struct XmlTextReaderDeleter
  //   {
  //     void operator()(xmlTextReaderPtr reader) const;
  //   };
  //   using UniqueXmlTextReader = std::unique_ptr<std::remove_pointer_t<xmlTextReaderPtr>, XmlTextReaderDeleter>;

  //   struct pp_result
  //   {
  //     fsp::p_limits limits;     // altered limits
  //     fsp::xml_node node;       // tree node just dealt with
  //     int           status{-1}; // status of the last libxml2 library operation
  //   };
  //   struct stack_struct
  //   {
  //     fsp::xml_node node;
  //     fsp::p_limits limits;
  //   };
  //   struct err_result
  //   {
  //     int         status{}; // status of last libxml2 operation
  //     str_t err;      // description of what is wrong
  //   };
  class xml_worker
  {
  public:
    // Upon construction we provide all relevant global structure references
    xml_worker(
      segment_pool&         pool,          // reference to segment pool
      const doc_set_dscr&   ds_dscr,       // reference to document set structure
      vec_seg_result&       results,       // where to store correct segments
      vec_seg_result&       errors,        // where to store non correct segmetns
      std::mutex&           results_mutex, // mutex for managing result structure
      std::mutex&           errors_mutex,  // mutex for managing errors structure
      const logger::Logger& log,           // reference to logger
      const proc_data&      targets, // structure that holds information about cutting points and xpaths of the values we are looking for
      str_t                 parent_log_name, // parent thread log thread name
      pipeline&             pl,              // for record_segment_done()/record_segment_failed() (doc_counters bookkeeping + hook dispatch)
      pipeline_hooks&       hooks,           // this worker thread's own hooks clone (see pipeline_worker)
      std::size_t           ok_block_flush_size, // see importer_config::ok_block_flush_size
      std::size_t           nak_block_flush_size // see importer_config::nak_block_flush_size
    );

    //     // main functor method
    //     void              operator()(const std::stop_token& st, int worker_id);
    int  process_one(std::size_t idx); // returns the segment's doc_ndx; also records the segment's outcome (and runs the hook) internally
    void flush_results();
  private:
    // Former free functions, now member methods
    result<segment_result> process_segment(const xml_segment& seg);
    // Common tail shared by every process_one() outcome -- see their own doc comments in
    // xml_worker.cpp. idx/seg refer to the same pool slot; result is moved/copied into
    // loc_res_ok_/loc_res_nak_ (the existing route into pipeline_.results()/errors()).
    void record_ok(std::size_t idx, xml_segment& seg, segment_result result);
    void record_nak(std::size_t idx, xml_segment& seg, segment_result result, error_info err);
    // Calls hooks_.store_block()/store_block_failed() on whatever's accumulated in
    // ok_block_indices_/nak_block_indices_ (a no-op if empty), then releases those slots back to
    // pool_ via segment_pool::release_slots() and clears the accumulator(s) for reuse.
    void flush_ok_block();
    void flush_nak_block();
    //     result<segment_result>               extract_xml_values(cstr_t xml_buf, const xml_segment& seg);
    //     std::expected<pp_result, err_result> process_and_prune_node( //
    //       const xpath_set&    xpaths,
    //       const xpath_limits& limits_vec,
    //       segment_result&     seg_result);
    //     int                                  process_positive_xpath_element( //
    //       const xml_attr& xp,
    //       std::size_t     ndx,
    //       std::size_t     depth,
    //       segment_result& seg_result) const;
    //     [[nodiscard]] str_t                  process_attribute(const xml_attr& xp) const;
    //     [[nodiscard]] std::optional<str_t>   get_attribute_value_ns(const str_t& local_name, const str_t& namespace_uri) const;
    //     segment_result                       loop(const xml_segment& seg, const fsp::xpath_set& xpaths, const xpath_limits& limits);
    //     bool                 open_tag(int& read_status, const xml_segment& seg, const auto& xpaths, const auto& limits, auto& res);
    //     [[nodiscard]] cstr_t indent() const;
    //     void                 close_tag(const xml_segment& seg);
    //     void                 prepare_tree_stack(const auto& xpaths);
    //     void                 obtain_value(const xml_segment& seg, const auto& xpaths, auto& res);
    //     bool                 reset_reader(cstr_t xml_buf);
  private:
    // --- worker context ---
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const logger::Logger&        log_;           //< logger
    const doc_set_dscr&          ds_dscr_;       //< structre of all input documents
    std::vector<segment_result>& results_;       //< result after parsing
    std::vector<segment_result>& errors_;        //< errors after parsing
    std::mutex&                  results_mutex_; //< mutex to lock results
    std::mutex&                  errors_mutex_;  //< mutex to lock erros
    const proc_data&             targets_;       //< targets to be processed
    pipeline&                    pipeline_;      //< for record_segment_done()/record_segment_failed()
    pipeline_hooks&              hooks_;         //< this worker thread's own hooks clone
                                                 //     UniqueXmlTextReader          reader_;        //< libxml2 reader
    const bool log_trace_ = log_.active(logger::level::trace);
    const bool log_debug_ = log_.active(logger::level::debug);
    const bool log_info_  = log_.active(logger::level::info);
    const bool log_warn_  = log_.active(logger::level::warn);
    const bool log_error_ = log_.active(logger::level::error);
    const bool log_crit_  = log_.active(logger::level::critical);
    //     std::size_t                  depth_     = 0UL; // depth within the tree/xpath
    //     std::stack<stack_struct>     tree_stack_;      // node and limits on specific depth
    //     int                          value_ndx_ = -1;  // index of the xpath value; -1 -> no value found
    str_t parent_log_name_;
    //     int                          reader_flags_    = (XML_PARSE_NOCDATA |    // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NOERROR |    // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NOWARNING |  // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NOBLANKS |   // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NONET |      // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NSCLEAN |    // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_IGNORE_ENC | // NOLINT(hicpp-signed-bitwise)
    //                                                      XML_PARSE_NODICT       // NOLINT(hicpp-signed-bitwise)
    //     );
    //     std::size_t                  segment_counter_ = 0;
    std::unique_ptr<segment_sax> sax_;  // sax parser
    segment_pool&                pool_; // segment pool
    vec_seg_result               loc_res_ok_;
    vec_seg_result               loc_res_nak_;
    // Pool slot indices for store_block()/store_block_failed() -- pre-sized to
    // ok_block_flush_size_/nak_block_flush_size_ at construction so normal-case operation never
    // reallocates (see importer_config::ok_block_flush_size's own doc comment). A slot's index
    // stays in one of these two vectors -- "locked" against reuse -- from the moment
    // process_one() decides its segment's fate until flush_ok_block()/flush_nak_block() releases
    // it back to pool_ via segment_pool::release_slots().
    std::vector<std::size_t> ok_block_indices_;
    std::vector<std::size_t> nak_block_indices_;
    // Parallel to nak_block_indices_ (same length, same order): why each of those segments
    // failed semantically (on_seg_proc() returned false). No ok_block equivalent -- an ok
    // segment's own segment_result (via segment_pool::result_at()) already carries everything a
    // store_block() hook needs.
    std::vector<error_info> nak_block_errors_;
    const std::size_t       ok_block_flush_size_;
    const std::size_t       nak_block_flush_size_;
    //  NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  //   inline void XmlTextReaderDeleter::operator()(xmlTextReaderPtr reader) const
  //   {
  //     if (reader != nullptr) xmlFreeTextReader(reader);
  //   }

  //   inline cstr_t xml_worker::indent() const
  //   {
  //     thread_local str_t indent_str(50, '.'); // NOLINT(readability-magic-numbers)
  //     auto               len = depth_ * 2;
  //     if (len > indent_str.size()) indent_str += indent_str;
  //     return cstr_t{indent_str.data(), len};
  //   }

} // namespace fsp