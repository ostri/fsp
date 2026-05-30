#pragma once

#include <atomic>
#include <expected>
#include <memory>
#include <spdlog/pattern_formatter.h>
#include <stop_token>
#include <string>
#include <vector>

// [DODANO] future/promise/optional za asinhrono validacijo
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

// #include "e_tag.hpp"
// #include "dom_parser.hpp"
#include "error_info.hpp"
#include "handler.hpp"
#include "logger_config.hpp"
#include "mem_buf_holder.hpp"
#include "mmap_file.hpp"
#include "processor_config.hpp"
#include "queue.hpp"
#include "xerces_mgr.hpp"
#include "xpath_helpers.hpp"
#include "parsing_util.hpp"

namespace fsp
{
  using cstr_t = std::string_view;
  struct segment_result
  {
    std::size_t segment_id  = 0;
    int         xpath_index = -1;
    bool        success     = false;
    std::string error_message;
  };

  // 1. Global thread-local variable (each thread gets its own isolated instance)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)
  inline thread_local std::string log_thread_name = "unknown";

  // 2. Formatter class that reads the current thread's name
  class ThreadNameFormatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override
    { // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      dest.append(log_thread_name.data(), log_thread_name.data() + log_thread_name.size());
    }
    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<ThreadNameFormatter>(); }
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

    // Procesiranje iz datoteke (mmap)
    void_result process_file(const std::string& xml_path, const std::string& xsd_path = "");

    // Procesiranje iz že mmap-ane datoteke
    void_result process_from_buffer(fsp::mmap_file& xml_mmap, fsp::mmap_file* xsd_mmap = nullptr);

    // // Procesiranje iz memory bufferja
    // void_result process_buffer(const void* data, std::size_t size, const void* xsd_data = nullptr, std::size_t xsd_size = 0);

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

    void cancel();

    [[nodiscard]] std::shared_ptr<spdlog::logger> get_logger() const { return logger_; }
  private:
    // Worker context — vse kar worker potrebuje
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    struct worker_context
    {
      int                             worker_id; // unique id of the worker
      segment_queue&                  seg_queue; // input queue
      const fsp::mmap_file&           xml_mmap;  // mmap mapping for buffer segment
      std::vector<segment_result>&    results;   // output queue for valid transactions
      std::vector<segment_result>&    errors;    // output queue for transactions with errors
      std::mutex&                     results_mutex;
      std::mutex&                     errors_mutex;
      std::atomic<std::size_t>&       processed_count;
      std::atomic<std::size_t>&       error_count;
      std::atomic<bool>&              cancel_flag;
      std::shared_ptr<spdlog::logger> logger;
    };
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

    fsp::xerces_mgr                         xerces_life_; // must be first to be destructed last
    processor_config                        config_;
    std::unique_ptr<xercesc::SAX2XMLReader> parser_;
    std::unique_ptr<Handler>                handler_;
    std::shared_ptr<spdlog::logger>         logger_;

    segment_queue               seg_queue_;
    std::vector<segment_result> results_;
    std::vector<segment_result> errors_;
    std::mutex                  results_mutex_;
    std::mutex                  errors_mutex_;
    std::atomic<std::size_t>    processed_count_{0};
    std::atomic<std::size_t>    error_count_{0};
    std::atomic<bool>           cancel_flag_{false};
    std::atomic<bool>           success_{false};
    std::vector<std::jthread>   workers_;

    std::chrono::steady_clock::time_point start_time_;
    std::unique_ptr<mem_buf_holder>       xsd_holder_;

    // Trajna mmap referenca za workers (živi med procesiranjem)
    const fsp::mmap_file* active_mmap_ = nullptr;

    void        setup_logger();

    // [SPREMENJENO] setup_parser() razdeljen na dve funkciji:
    // - setup_parser_no_validation(): za SAX nit — brez validacijskih featureov,
    //   ker validacija teče v svoji niti z lastnim parserjem. fgCalculateSrcOfs
    //   mora ostati vklopljen ker Handler potrebuje byte offsete.
    // - setup_validation_parser(): za validacijsko nit — z vsemi XSD featurji,
    //   brez fgCalculateSrcOfs ker offseti niso potrebni (zmanjša overhead).
    // Stara setup_parser() je bila kombinacija obeh in se ne more več uporabiti
    // ko sta niti ločeni.
    void_result setup_parser_no_validation();

    void_result setup_validation(fsp::mmap_file& xsd_mmap);
    void_result setup_validation_from_buffer(const void* data, std::size_t size, std::string_view schema_name);
    void_result start_workers();
    void        stop_workers();

    // [DODANO] Zažene validacijo v ločeni niti. Vrne future ki se razreši z
    // nullopt (ok) ali error_info (napaka). Klic je neblokirajočen — validacija
    // teče vzporedno s SAX parsingom. Če XSD ni podan, future se takoj razreši
    // z nullopt da ostala koda ne rabi ločevati med "validacija vklopljena" in
    // "validacija izklopljena".
    std::shared_future<std::optional<error_info>> launch_validation_thread(
      const void* xml_data, std::size_t xml_size,
      const void* xsd_data, std::size_t xsd_size,
      std::string xsd_path);

    static void worker_function( //
      [[maybe_unused]] const std::stop_token& st,
      int                                     worker_id,
      [[maybe_unused]] worker_context         ctx);

    static result<segment_result> process_segment([[maybe_unused]] const worker_context& ctx, const xml_segment& seg);
    static result<segment_result> extract_xml_values( //
      cstr_t                                 xml_buf,
      const segment_result&                  sr,
      const xml_segment&                     seg,
      [[maybe_unused]] const worker_context& ctx);
    void                          log_error(const error_info& error);
    void                          log_info(const std::string& msg);
    void                          log_debug(const std::string& msg);
    void                          log_warning(const std::string& msg);
  };

  using processing_result = std::expected<std::pair<std::vector<segment_result>, std::vector<segment_result>>, error_info>;

  processing_result process_xml_file(const std::string&              xml_path,
                                     const std::string&              xsd_path,
                                     const std::vector<std::string>& xpath_strings,
                                     std::size_t                     num_workers = 0,
                                     const logger_config&            log_cfg     = logger_config{});

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
