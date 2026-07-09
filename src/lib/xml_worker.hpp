#pragma once

#include "xml_processor.hpp" // za tipove kot segment_result, proc_data itd.
// #include "lock_queue.hpp"
#include "logger.hpp"
#include "xml_segment.hpp"
#include "mmap_file.hpp"
#include <stop_token>
#include <vector>
#include <mutex>
#include <atomic>

namespace fsp
{

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

    // Functor – glavna funkcija worker niti
    void operator()(const std::stop_token& st, int worker_id);
  private:
    // Nekdanje statične funkcije zdaj kot članske metode
    result<segment_result> process_segment(const xml_segment& seg);
    result<segment_result> extract_xml_values(cstr_t xml_buf, const xml_segment& seg);

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
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

} // namespace fsp