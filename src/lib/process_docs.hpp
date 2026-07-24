#pragma once

#include "doc_set_dscr.hpp"
#include "stats.hpp"
#include "logger.hpp"
#include "processor_config.hpp"
#include "segment_result.hpp"
#include "xerces_mgr.hpp"
#include "xpath_helpers.hpp"
#include "segment_pool.hpp"
#include <utility>


namespace fsp
{
  using s_clock = std::chrono::time_point<std::chrono::steady_clock>;
  class process_docs
  {
  public:
    explicit process_docs(processor_config cfg);
    process_docs(processor_config cfg, str_t parent_log_name);
    ~process_docs()                                                    = default;
    process_docs(const process_docs&)                                  = delete;
    process_docs(process_docs&&)                                       = delete;
    process_docs&                       operator=(const process_docs&) = delete;
    process_docs&                       operator=(process_docs&&)      = delete;
    void_result                         process_files(const std::vector<std::string>& xml_paths, const std::string& xsd_path);
    [[nodiscard]] const vec_seg_result& get_results() const;
    [[nodiscard]] const vec_seg_result& get_errors() const;
    stats_t                             stats() const { return stats_; }
  private: // methods
    void_result process_files_internal();
  private: // data
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const s_clock            start_time_ = std::chrono::steady_clock::now();
    const fsp_logger         log_;               //< logger (before any logging)
    const xerces_mgr         xerces_life_;       //< must be first to be destructed last (before any xercesc)
    const processor_config   cfg_;               //< framework configuration
    const str_t              parent_log_name_;   //< parent name for logging
    mutable std::mutex       results_mutex_;     //< results mutex
    mutable std::mutex       errors_mutex_;      //< errors mutex
    vec_seg_result           results_;           //< ok segment data
    vec_seg_result           errors_;            //< segments that have semantic errors
    stats_t                  stats_{};           //< document processing statistics
    doc_set_dscr             ds_dscr_{log_};     //< information about the xml documents to be processed
    segment_pool             pool_;              //< segment pool
    std::atomic<std::size_t> active_cutters_{0}; //< number still active cutters
    const bool               log_trace_ = log_.active(fsp::lvl_enum::trace);
    const bool               log_debug_ = log_.active(fsp::lvl_enum::debug);
    const bool               log_info_  = log_.active(fsp::lvl_enum::info);
    const bool               log_warn_  = log_.active(fsp::lvl_enum::warn);
    const bool               log_error_ = log_.active(fsp::lvl_enum::err);
    const bool               log_crit_  = log_.active(fsp::lvl_enum::crit);
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  inline process_docs::process_docs(processor_config cfg)
  : fsp::process_docs(std::move(cfg), "main")
  {
  }

  inline process_docs::process_docs(processor_config cfg, str_t parent_log_name)
  : log_(cfg.log_config)
  , cfg_(std::move(cfg))
  , parent_log_name_(std::move(parent_log_name))
  , ds_dscr_(log_)
  , pool_(log_, 1024UL * 1024UL * 8UL) // NOLINT(readability-magic-numbers)
  { log_.make_log_name(parent_log_name_); }

  inline const vec_seg_result& process_docs::get_results() const
  {
    std::lock_guard lock(results_mutex_);
    return results_;
  }

  inline const vec_seg_result& process_docs::get_errors() const
  {
    std::lock_guard lock(errors_mutex_);
    return errors_;
  }
}; // namespace fsp