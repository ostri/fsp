#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <spdlog/pattern_formatter.h>
#include <stop_token>
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
#include "error_info.hpp"
#include "handler.hpp"
#include "logger.hpp"
#include "logger_config.hpp"
#include "mem_buf_holder.hpp"
#include "mmap_file.hpp"
#include "processor_config.hpp"
#include "queue.hpp"
#include "xerces_mgr.hpp"
#include "xpath_helpers.hpp"
#include "parsing_util.hpp"
#include "segment_result.hpp"

namespace fsp
{
  using cstr_t = std::string_view;
  // Worker context — vse kar worker potrebuje
  // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
  struct worker_context
  {
    int                          worker_id;       // unique id of the worker
    segment_queue&               seg_queue;       // input queue
    const fsp::mmap_file&        xml_mmap;        // mmap mapping for buffer segment
    std::vector<segment_result>& results;         // output queue for valid transactions
    std::vector<segment_result>& errors;          // output queue for transactions with errors
    std::mutex&                  results_mutex;   // results queue mutex
    std::mutex&                  errors_mutex;    // errors queue mitex
    std::atomic<std::size_t>&    processed_count; // number of processed segments
    std::atomic<std::size_t>&    error_count;     // number of detected errors
    std::atomic<bool>&           cancel_flag;     // is the operation cancelled?
    const fsp_logger&            log;             // logger wrapper
    const proc_data&             targets;         // how to partition the xml buffer and which
                                                  // tags to extract from each partition type
  };
  // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  class xml_processor
  {
  public:
    explicit xml_processor(processor_config cfg);
    ~xml_processor();

    xml_processor(const xml_processor&)            = delete;
    xml_processor& operator=(const xml_processor&) = delete;
    xml_processor(xml_processor&&)                 = delete;
    xml_processor& operator=(xml_processor&&)      = delete;
    // Procesiranje iz datoteke (mmap)
    void_result process_file(const std::string& xml_path, const std::string& xsd_path = "");
    // Procesiranje iz že mmap-ane datoteke
    void_result                               process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap = nullptr);
    [[nodiscard]] std::vector<segment_result> get_results();
    [[nodiscard]] std::vector<segment_result> get_errors();
    struct stats
    {
      std::size_t total_segments      = 0;
      std::size_t successful_segments = 0;
      std::size_t failed_segments     = 0;
      std::size_t active_workers      = 0;
      double      processing_time_ms  = 0.0;
    };
    [[nodiscard]] stats get_stats() const;
    [[nodiscard]] bool  is_successful() const { return success_.load(); }
    void                cancel();
  private: /// methods
    void_result setup_parser_no_validation();
    void_result start_workers();
    void        stop_workers();
    // Zažene validacijo v ločeni niti. Vrne future ki se razreši z
    // nullopt (ok) ali error_info (napaka). Klic je neblokirajočen — validacija
    // teče vzporedno s SAX parsingom. Če XSD ni podan, future se takoj razreši
    // z nullopt da ostala koda ne rabi ločevati med "validacija vklopljena" in
    // "validacija izklopljena".
    std::shared_future<std::optional<error_info>> launch_validation_thread(const void* xml_data,
                                                                           std::size_t xml_size,
                                                                           const void* xsd_data,
                                                                           std::size_t xsd_size,
                                                                           std::string xsd_path);

    static void                   worker_function( //
      [[maybe_unused]] const std::stop_token& st,
      int                                     worker_id,
      worker_context                          ctx);
    static result<segment_result> process_segment(const worker_context& ctx, const xml_segment& seg);
    static result<segment_result> extract_xml_values(cstr_t xml_buf, const xml_segment& seg, const worker_context& ctx);
    using s_clock = std::chrono::time_point<std::chrono::steady_clock>;
  private: /// members
    const s_clock                           start_ = std::chrono::steady_clock::now();
    const fsp_logger                        logger_;      // logger must be created first and destructed last
    fsp::xerces_mgr                         xerces_life_; // must be first to be destructed last
    processor_config                        config_;
    std::unique_ptr<xercesc::SAX2XMLReader> parser_;
    std::unique_ptr<Handler>                handler_;
    segment_queue                           seg_queue_;
    std::vector<segment_result>             results_;
    std::vector<segment_result>             errors_;
    std::mutex                              results_mutex_;
    std::mutex                              errors_mutex_;
    std::atomic<std::size_t>                processed_count_{0};
    std::atomic<std::size_t>                error_count_{0};
    std::atomic<bool>                       cancel_flag_{false};
    std::atomic<bool>                       success_{false};
    std::vector<std::jthread>               workers_;
    std::chrono::steady_clock::time_point   start_time_;
    std::unique_ptr<mem_buf_holder>         xsd_holder_;
    const fsp::mmap_file*                   active_mmap_ = nullptr; // reference to mmap file (needed by workers)
  };

  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;

  processing_result process_xml_file(const std::string&   xml_path,
                                     const std::string&   xsd_path,
                                     const proc_data&     proc_data,
                                     std::size_t          num_workers = 0,
                                     const logger_config& log_cfg     = logger_config{});

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
