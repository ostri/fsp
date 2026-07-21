#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <spdlog/pattern_formatter.h>
#include <string>
#include <vector>
#include <future>
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
#include "mem_buf_holder.hpp"
#include "mmap_file.hpp"
#include "processor_config.hpp"
#include "lock_queue.hpp"
#include "stats.hpp"
#include "xpath_helpers.hpp"
#include "parsing_util.hpp"
#include "segment_result.hpp"

namespace fsp
{
  using cstr_t            = std::string_view;
  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;
  using segment_queue     = lock_queue<xml_segment>;
  using s_clock           = std::chrono::time_point<std::chrono::steady_clock>;
  class xml_processor
  {
  public:
    explicit xml_processor(processor_config cfg);
    xml_processor(processor_config cfg, str_t parent_log_name);
    ~xml_processor();

    xml_processor(const xml_processor&)            = delete;
    xml_processor& operator=(const xml_processor&) = delete;
    xml_processor(xml_processor&&)                 = delete;
    xml_processor& operator=(xml_processor&&)      = delete;
    void_result    process_file(const std::string& xml_path, const std::string& xsd_path, const gr_pool_t& gp);
    void_result    process_from_buffer(mmap_file& xml_mmap, mmap_file* xsd_mmap, const gr_pool_t& gp);
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

    [[nodiscard]] bool is_successful() const { return success_.load(); }
    void               cancel();
    // static processing_result process_xml_file(const std::string&   xml_path,
    //                                           const std::string&   xsd_path,
    //                                           const proc_data&     proc_data,
    //                                           std::size_t          num_workers = 0,
    //                                           const logger_config& log_cfg     = logger_config{});
    static void    file_worker_task(lock_queue<std::size_t>&   file_queue,
                                    const doc_set_dscr&        ds_dscr,
                                    std::mutex&                results_agg_mutex,
                                    vec_seg_result&            all_results,
                                    vec_seg_result&            all_errors,
                                    std::atomic<std::size_t>&  file_processed,
                                    std::atomic<bool>&         has_error,
                                    std::optional<error_info>& first_error,
                                    std::size_t                worker_idx,
                                    const std::string&         parent_log_name,
                                    const processor_config&    config,
                                    const fsp_logger&          log,
                                    const gr_pool_t&           gp,
                                    std::latch&                gr_latch,
                                    std::atomic<bool>&         gr_loaded,
                                    bool                       has_grammar);
    vec_seg_result move_results();
    vec_seg_result move_errors();
  private: /// methods
    void_result setup_parser_no_validation();
    void_result start_workers();
    void        stop_workers();
    // Zažene validacijo v ločeni niti. Vrne future ki se razreši z
    // nullopt (ok) ali error_info (napaka). Klic je neblokirajočen — validacija
    // teče vzporedno s SAX parsingom. Če XSD ni podan, future se takoj razreši
    // z nullopt da ostala koda ne rabi ločevati med "validacija vklopljena" in
    // "validacija izklopljena".
    std::shared_future<std::optional<error_info>> launch_validation_thread( //
                                                                            //
      const cstr_t&    f_xml_data,                                          // xml file contents
      const gr_pool_t& gp,                                                  // grammar pool
      std::string      xsd_path                                             // path to the grammar file
    );

    static std::optional<error_info> validate_xml_worker(const cstr_t&      f_xml_data,
                                                         const gr_pool_t&   gp,
                                                         std::string        xsd_path,
                                                         const fsp_logger&  logger,
                                                         const std::string& parent_log_name);

    static void process_one_file(const std::string&                  xml_path,
                                 const doc_set_dscr&                 ds_dscr,
                                 std::mutex&                         results_agg_mutex,
                                 std::vector<segment_result>&        all_results,
                                 std::vector<segment_result>&        all_errors,
                                 std::atomic<bool>&                  has_error,
                                 std::optional<error_info>&          first_error,
                                 const processor_config&             config,
                                 const fsp_logger&                   log, // Using auto to deduce the fsp::logger type
                                 const gr_pool_t&                    gp,
                                 [[maybe_unused]] std::latch&        gr_latch,
                                 [[maybe_unused]] std::atomic<bool>& gr_loaded,
                                 [[maybe_unused]] bool               have_grammar);
    void        save_stats();
    stats_t     stats() const { return stats_; }
  private: /// members
    const s_clock                           start_ = std::chrono::steady_clock::now();
    const fsp_logger                        log_; // logger must be created first and destructed last
    processor_config                        config_;
    std::unique_ptr<xercesc::SAX2XMLReader> parser_;
    std::unique_ptr<Handler>                handler_;
    segment_queue                           seg_queue_;
    vec_seg_result                          results_;
    vec_seg_result                          errors_;
    mutable std::mutex                      results_mutex_;
    mutable std::mutex                      errors_mutex_;
    std::atomic<bool>                       success_{false};
    std::vector<std::jthread>               workers_;
    s_clock                                 start_time_;
    std::unique_ptr<mem_buf_holder>         xsd_holder_;
    const fsp::mmap_file*                   active_mmap_ = nullptr; // reference to mmap file (needed by workers)
    str_t                                   parent_log_name_;
    const bool                              log_trace_ = log_.active(lvl_enum::trace);
    const bool                              log_debug_ = log_.active(lvl_enum::debug);
    const bool                              log_info_  = log_.active(lvl_enum::info);
    const bool                              log_warn_  = log_.active(lvl_enum::warn);
    const bool                              log_error_ = log_.active(lvl_enum::err);
    const bool                              log_crit_  = log_.active(lvl_enum::crit);
    stats_t                                 stats_; // processing statistics
  };

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
