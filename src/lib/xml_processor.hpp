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
#include "error_info.hpp"
#include "handler.hpp"
#include "logger.hpp"
#include "logger_config.hpp"
#include "mem_buf_holder.hpp"
#include "mmap_file.hpp"
#include "processor_config.hpp"
#include "lock_queue.hpp"
#include "xerces_mgr.hpp"
#include "xpath_helpers.hpp"
#include "parsing_util.hpp"
#include "segment_result.hpp"

namespace fsp
{
  using cstr_t            = std::string_view;
  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;
  using segment_queue     = lock_queue<xml_segment>;
  class xml_processor
  {
  public:
    explicit xml_processor(processor_config cfg);
    ~xml_processor();

    xml_processor(const xml_processor&)            = delete;
    xml_processor& operator=(const xml_processor&) = delete;
    xml_processor(xml_processor&&)                 = delete;
    xml_processor& operator=(xml_processor&&)      = delete;
    void_result    process_file(const std::string& xml_path, const std::string& xsd_path = "");
    void_result    process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap = nullptr);
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
    [[nodiscard]] stats      get_stats() const;
    [[nodiscard]] bool       is_successful() const { return success_.load(); }
    void                     cancel();
    static processing_result process_xml_file(const std::string&   xml_path,
                                              const std::string&   xsd_path,
                                              const proc_data&     proc_data,
                                              std::size_t          num_workers = 0,
                                              const logger_config& log_cfg     = logger_config{});
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
