#pragma once

#include <memory>
#include <string>
#include <vector>
#include <stop_token>
#include <atomic>
#include <expected>
#include <optional>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "e_tag.hpp"
#include "queue.hpp"
#include "handler.hpp"
#include "xerces_mgr.hpp"
#include "mmap_file.hpp"
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include "error_info.hpp"
#include "mem_buf_holder.hpp"

namespace fsp
{
  // Result of processing a segment
  struct segment_result
  {
    size_t      segment_id;
    int         xpath_index;
    bool        success;
    std::string error_message;
  };

  // Type aliases for expected patterns
  template <typename T>
  using result      = std::expected<T, error_info>;
  using void_result = std::expected<void, error_info>;

  // Logger configuration
  struct logger_config
  {
    bool                      enable_console = true;
    bool                      enable_file    = false;
    std::string               log_file_path  = "xml_processor.log";
    spdlog::level::level_enum log_level      = spdlog::level::info;
    std::string               logger_name    = "xml_processor";
  };

  // Forward declarations
  class xml_processor;

  // Configuration for the processor
  struct processor_config
  {
    std::vector<xpath_t>       targets;
    size_t                     num_workers          = 0;
    bool                       validate_against_xsd = true;
    bool                       strict_validation    = true;
    std::optional<std::string> schema_namespace;
    logger_config              log_config;
  };

  class xml_processor
  {
  public:
    explicit xml_processor(processor_config cfg);
    ~xml_processor();

    xml_processor(const xml_processor&)            = delete;
    xml_processor& operator=(const xml_processor&) = delete;
    xml_processor(xml_processor&&)                 = delete;
    xml_processor& operator=(xml_processor&&)      = delete;

    // Process XML file from path
    void_result process_file(const std::string& xml_path, const std::string& xsd_path = "");
    // Process from already mmapped files
    void_result process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap = nullptr);
    // Process from memory buffer
    void_result process_buffer(const void* data, size_t size, const void* xsd_data = nullptr, size_t xsd_size = 0);

    // Get results (call after successful process())
    std::vector<segment_result> get_results();
    std::vector<segment_result> get_errors();

    // Get processing statistics
    struct stats
    {
      size_t total_segments      = 0;
      size_t successful_segments = 0;
      size_t failed_segments     = 0;
      size_t active_workers      = 0;
      double processing_time_ms  = 0.0;
    };
    [[nodiscard]] stats get_stats() const;

    // Check if processing was successful
    [[nodiscard]] bool is_successful() const { return success_.load(); }

    // Cancel processing
    void cancel();

    // Get logger instance
    [[nodiscard]] std::shared_ptr<spdlog::logger> get_logger() const { return logger_; }
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    struct worker_context
    {
      segment_queue&                  seg_queue;
      std::vector<segment_result>&    results;
      std::vector<segment_result>&    errors;
      std::mutex&                     results_mutex;
      std::mutex&                     errors_mutex;
      std::atomic<size_t>&            processed_count;
      std::atomic<size_t>&            error_count;
      std::atomic<bool>&              cancel_flag;
      std::shared_ptr<spdlog::logger> logger;
    };
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
    processor_config                        config_;
    std::unique_ptr<xercesc::SAX2XMLReader> parser_;
    std::unique_ptr<Handler>                handler_;
    fsp::xerces_mgr                         xerces_life_;
    std::shared_ptr<spdlog::logger>         logger_;

    segment_queue                         seg_queue_;
    std::vector<segment_result>           results_;
    std::vector<segment_result>           errors_;
    std::mutex                            results_mutex_;
    std::mutex                            errors_mutex_;
    std::atomic<size_t>                   processed_count_{0};
    std::atomic<size_t>                   error_count_{0};
    std::atomic<bool>                     cancel_flag_{false};
    std::atomic<bool>                     success_{false};
    std::vector<std::jthread>             workers_;
    std::chrono::steady_clock::time_point start_time_;

    // RAII helper for XSD buffer lifetime


    std::unique_ptr<mem_buf_holder> xsd_holder_;

    void        setup_logger();
    void_result setup_parser();
    void_result setup_validation(fsp::mmap_file& xsd_mmap);
    void_result setup_validation_from_buffer(const void* data, size_t size);
    void_result start_workers();
    void        stop_workers();

    static void                   worker_function(const std::stop_token& st, worker_context ctx);
    static result<xpath_t>        string_to_xpath(const std::string& xpath_str);
    static result<segment_result> process_segment(const xml_segment& seg, const std::shared_ptr<spdlog::logger>& logger);

    void log_error(const error_info& error);
    void log_info(const std::string& msg);
    void log_debug(const std::string& msg);
    void log_warning(const std::string& msg);
  };

  // Helper functions for working with e_tag and xpath_t
  namespace xpath_helpers
  {
    // Parse XPath string like "/ns:root/child/grandchild" or "root/child"
    result<xpath_t> from_string(const std::string& xpath_str);

    // Convert xpath_t to string representation
    std::string to_string(const xpath_t& xpath);

    // Validate xpath_t (no empty tags, valid characters, etc.)
    bool validate(const xpath_t& xpath, std::string* error_msg = nullptr);

    // Get the last tag from xpath
    std::optional<e_tag> last_tag(const xpath_t& xpath);

    // Get depth of xpath
    size_t depth(const xpath_t& xpath);
  } // namespace xpath_helpers

  // Result type for the entire processing operation
  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;

  // Convenience function for one-shot processing
  processing_result process_xml_file(const std::string&              xml_path,
                                     const std::string&              xsd_path,
                                     const std::vector<std::string>& xpath_strings,
                                     size_t                          num_workers = 0,
                                     const logger_config&            log_cfg     = logger_config{});

} // namespace fsp
// Magic enum customization - MUST be outside namespace fsp
namespace magic_enum::customize
{
  template <>
  struct enum_range<fsp::processor_error>
  {
    static constexpr int min = 0;
    static constexpr int max = static_cast<int>(fsp::processor_error::internal_error);
  };
} // namespace magic_enum::customize