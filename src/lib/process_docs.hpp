#pragma once

#include "logger.hpp"
#include "processor_config.hpp"
#include "segment_result.hpp"
#include "xerces_mgr.hpp"
#include "xpath_helpers.hpp"
#include <utility>

namespace fsp
{
  using s_clock = std::chrono::time_point<std::chrono::steady_clock>;
  class process_docs
  {
  public:
    explicit process_docs(processor_config cfg);
    process_docs(processor_config cfg, str_t parent_log_name)
    : log_(cfg.log_config)
    , cfg_(std::move(cfg))
    , parent_log_name_(std::move(parent_log_name))
    { log_.make_log_name(parent_log_name_); }
    ~process_docs()                                                    = default;
    process_docs(const process_docs&)                                  = delete;
    process_docs(process_docs&&)                                       = delete;
    process_docs&                       operator=(const process_docs&) = delete;
    process_docs&                       operator=(process_docs&&)      = delete;
    void_result                         process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path);
    [[nodiscard]] const vec_seg_result& get_results() const
    {
      std::lock_guard lock(results_mutex_);
      return results_;
    }
    [[nodiscard]] const vec_seg_result& get_errors() const
    {
      std::lock_guard lock(errors_mutex_);
      return errors_;
    }
  private: // methods
    void_result process_files_internal(const std::vector<std::string>& xml_paths,
                                       const std::string&              xsd_path //,
                                                                                // std::size_t                     num_parallel
    );
  private: // data
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const s_clock          start_time_ = std::chrono::steady_clock::now();
    const fsp_logger       log_;         //< logger (before any logging)
    const xerces_mgr       xerces_life_; //< must be first to be destructed last (before any xercesc)
    const processor_config cfg_;         // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    const str_t            parent_log_name_;
    mutable std::mutex     results_mutex_;
    mutable std::mutex     errors_mutex_;
    vec_seg_result         results_; // ok segment data
    vec_seg_result         errors_;  // segments that have semantic errors


    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  inline process_docs::process_docs(processor_config cfg)
  : fsp::process_docs(std::move(cfg), "main")
  {
  }
}; // namespace fsp