#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <spdlog/pattern_formatter.h>
#include <string>
#include <vector>
#include <optional>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include "doc_set_dscr.hpp"
#include "error_info.hpp"
#include "handler.hpp"
#include "load_grammar.hpp"
#include "logger.hpp"
#include "processor_config.hpp"
#include "lock_queue.hpp"
#include "stats.hpp"
#include "xpath_helpers.hpp"
#include "segment_result.hpp"
#include "segment_pool.hpp"

namespace fsp
{
  using cstr_t            = std::string_view;
  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;
  using s_clock           = std::chrono::time_point<std::chrono::steady_clock>;
  class xml_processor
  {
  public:
    // explicit xml_processor(const processor_config& cfg);
    xml_processor(processor_config cfg, str_t parent_log_name, segment_pool& pool, doc_set_dscr& ds_dscr);
    ~xml_processor();

    xml_processor(const xml_processor&)            = delete;
    xml_processor& operator=(const xml_processor&) = delete;
    xml_processor(xml_processor&&)                 = delete;
    xml_processor& operator=(xml_processor&&)      = delete;
    void_result    process_one_doc_from_buffer(std::size_t xml_path_ndx);
    /** @brief Process multiple XML files in parallel using N workers.
     * Each worker processes files sequentially from the queue using process_file.
     * Results and errors are collected from all files.
     *
     * @param xml_paths vector of XML file paths
     * @param xsd_path XSD file path (shared for all)
     * @param num_parallel number of parallel workers (0 = auto)
     * @return void_result success or first error
     */
    void_result process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path = "", std::size_t num_parallel = 0);
    [[nodiscard]] const vec_seg_result& get_results() const;
    [[nodiscard]] const vec_seg_result& get_errors() const;
    [[nodiscard]] bool                  is_successful() const;
    void                                cancel();
    static void                         doc_worker(lock_queue<std::size_t>&   doc_queue,
                                                   doc_set_dscr&              ds_dscr,
                                                   segment_pool&              pool,
                                                   std::mutex&                results_agg_mutex,
                                                   vec_seg_result&            all_results,
                                                   vec_seg_result&            all_errors,
                                                   std::atomic<std::size_t>&  file_processed,
                                                   std::atomic<bool>&         has_error,
                                                   std::optional<error_info>& first_error,
                                                   std::size_t                worker_idx,
                                                   const std::string&         parent_log_name,
                                                   const processor_config&    config,
                                                   const fsp_logger&          log);
    vec_seg_result                      move_results();
    vec_seg_result                      move_errors();
    void_result                         init_parser_and_handler();
  private: /// methods
    void_result setup_parser_no_validation();
    void_result start_workers();
    void        stop_workers();
    void        process_one_doc(std::size_t                  doc_ndx,
                                std::mutex&                  results_agg_mutex,
                                std::vector<segment_result>& all_results,
                                std::vector<segment_result>& all_errors,
                                std::atomic<bool>&           has_error,
                                std::optional<error_info>&   first_error);
    void        save_stats();
    stats_t     stats() const;
    const auto& active_mmap() const { return ds_dscr_[doc_ndx_].mmf(); }
  private: /// members
    const s_clock                           start_ = std::chrono::steady_clock::now();
    const fsp_logger                        log_;       //< logger must be created first and destructed last
    processor_config                        cfg_;       //< processor configuration
    segment_pool&                           seg_pool_;  //< pool of segments to be processed / are free
    doc_set_dscr&                           ds_dscr_;   //< document description
    std::size_t                             doc_ndx_{}; //< index of the document within the ds_dscr_ structure
    std::unique_ptr<xercesc::SAX2XMLReader> parser_;
    std::unique_ptr<Handler>                handler_;
    vec_seg_result                          results_;
    vec_seg_result                          errors_;
    mutable std::mutex                      results_mutex_;
    mutable std::mutex                      errors_mutex_;
    std::atomic<bool>                       success_{false};
    std::vector<std::jthread>               workers_;
    // s_clock                                 start_time_;
    str_t   parent_log_name_;
    bool    log_trace_ = log_.active(lvl_enum::trace);
    bool    log_debug_ = log_.active(lvl_enum::debug);
    bool    log_info_  = log_.active(lvl_enum::info);
    bool    log_warn_  = log_.active(lvl_enum::warn);
    bool    log_error_ = log_.active(lvl_enum::err);
    bool    log_crit_  = log_.active(lvl_enum::crit);
    stats_t stats_; // processing statistics
  };
  /////////////////////////////////////////////////////////////////////////////////////////////
  inline bool           xml_processor::is_successful() const { return success_.load(); }
  inline vec_seg_result xml_processor::move_results()
  {
    std::lock_guard lock(results_mutex_);
    return std::move(results_);
  }
  inline vec_seg_result xml_processor::move_errors()
  {
    std::lock_guard lock(errors_mutex_);
    return std::move(errors_);
  }
  inline stats_t xml_processor::stats() const { return stats_; }
} // namespace fsp

namespace magic_enum::customize
{
  template <>
  struct enum_range<fsp::processor_error>
  {
    static constexpr int min = 0;
    static constexpr int max = static_cast<int>(fsp::processor_error::internal_error);
  };
} // namespace magic_enum::customize
