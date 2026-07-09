#pragma once

#include "xml_node.hpp"
#include "xml_processor.hpp" // za tipove kot segment_result, proc_data itd.
// #include "lock_queue.hpp"
#include "logger.hpp"
#include "xml_segment.hpp"
#include "mmap_file.hpp"
#include "xpath_limits.hpp"
#include <libxml/xmlreader.h>
#include <stack>
#include <stop_token>
#include <vector>
#include <mutex>
#include <atomic>

namespace fsp
{
  using str_t = std::string;
  struct XmlTextReaderDeleter
  {
    void operator()(xmlTextReaderPtr reader) const;
  };
  using UniqueXmlTextReader = std::unique_ptr<std::remove_pointer_t<xmlTextReaderPtr>, XmlTextReaderDeleter>;

  struct pp_result
  {
    fsp::p_limits limits;     // altered limits
    fsp::xml_node node;       // tree node just dealt with
    int           status{-1}; // status of the last libxml2 library operation
  };
  struct stack_struct
  {
    fsp::xml_node node;
    fsp::p_limits limits;
  };
  struct err_result
  {
    int         status{}; // status of last libxml2 operation
    std::string err;      // description of what is wrong
  };
  class xml_worker
  {
  public:
    // Konstruktor sprejme vse potrebne podatke (kopije/reference kjer je smiselno)
    xml_worker(segment_queue&               seg_queue,
               const mmap_file&             xml_mmap,
               std::vector<segment_result>& results,
               std::vector<segment_result>& errors,
               std::mutex&                  results_mutex,
               std::mutex&                  errors_mutex,
               std::atomic<std::size_t>&    processed_count,
               std::atomic<std::size_t>&    error_count,
               std::atomic<bool>&           cancel_flag,
               const fsp_logger&            log,
               const proc_data&             targets);

    // main functor method
    void operator()(const std::stop_token& st, int worker_id);
  private:
    // Nekdanje statične funkcije zdaj kot članske metode
    result<segment_result>               process_segment(const xml_segment& seg);
    result<segment_result>               extract_xml_values(cstr_t xml_buf, const xml_segment& seg);
    std::expected<pp_result, err_result> process_and_prune_node( //
      const fsp::xpath_node_struct& xpaths,
      std::stack<stack_struct>&     stack,
      const fsp::xpath_limits&      limits_vec,
      fsp::segment_result&          seg_result);
    int                                  process_positive_xpath_element( //
      const fsp::xml_attr& xp,
      std::size_t          ndx,
      std::size_t          depth,
      fsp::segment_result& seg_result);
    str_t                                process_attribute(const auto& xp);
    std::optional<std::string>           get_attribute_value_ns(const str_t& local_name, const str_t& namespace_uri);
  private:
    // --- worker context ---
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger&            log_;             //< logger
    segment_queue&               seg_queue_;       //< segmetn queue
    const mmap_file&             xml_mmap_;        //< mmap file with xml segment
    std::vector<segment_result>& results_;         //< result after parsing
    std::vector<segment_result>& errors_;          //< errors after parsing
    std::mutex&                  results_mutex_;   //< mutex to lock results
    std::mutex&                  errors_mutex_;    //< mutex to lock erros
    std::atomic<std::size_t>&    processed_count_; //< number of processed segments
    std::atomic<std::size_t>&    error_count_;     //< number of errors //FIXME ostri this is size of errorss_
    std::atomic<bool>&           cancel_flag_;     //< are we interupted?
    const proc_data&             targets_;         //< targets to be processed
    UniqueXmlTextReader          reader_;          //< libxml2 reader
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  inline void XmlTextReaderDeleter::operator()(xmlTextReaderPtr reader) const
  {
    if (reader != nullptr) xmlFreeTextReader(reader);
  }

} // namespace fsp