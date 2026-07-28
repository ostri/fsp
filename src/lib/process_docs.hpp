#pragma once

#include "pipeline.hpp"
#include "xerces_mgr.hpp"
#include "processor_config.hpp"
#include "logger.hpp"
#include "segment_result.hpp"
#include <vector>
#include <string>

namespace fsp
{
  // Public facade — process_docs itself holds no processing logic; everything is delegated
  // to pipeline, which coordinates the V/C/P hybrid workers.
  class process_docs
  {
  public:
    explicit process_docs(processor_config cfg);
    process_docs(processor_config cfg, const str_t& parent_log_name);
    ~process_docs()                              = default;
    process_docs(const process_docs&)            = delete;
    process_docs(process_docs&&)                 = delete;
    process_docs& operator=(const process_docs&) = delete;
    process_docs& operator=(process_docs&&)      = delete;

    [[nodiscard]] result<doc_set_counter>  process_files(const std::vector<str_t>& xml_paths,
                                                         cstr_t                    xsd_path,
                                                         pipeline_hooks&           hooks = default_pipeline_hooks);
    [[nodiscard]] const vec_seg_result&    get_results() const;
    [[nodiscard]] const vec_seg_result&    get_errors() const;
    [[nodiscard]] std::vector<std::size_t> failed_document_indices() const;
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger log_;         //< "main" logger; must exist before xerces_life_ and impl_
    const xerces_mgr xerces_life_; //< must be constructed before, and destructed after, impl_ (xercesc lifetime)
    pipeline         impl_;        //< actual implementation -- coordinates the V/C/P hybrid pipeline
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  inline process_docs::process_docs(processor_config cfg)
  : process_docs(std::move(cfg), "main")
  {
  }

  inline process_docs::process_docs(processor_config cfg, const str_t& parent_log_name)
  : log_(cfg.log_config, parent_log_name)
  , impl_(std::move(cfg), log_, parent_log_name)
  {
  }

  inline result<doc_set_counter> process_docs::process_files(const std::vector<str_t>& xml_paths, cstr_t xsd_path, pipeline_hooks& hooks)
  { return impl_.process_files(xml_paths, xsd_path, hooks); }
  inline const vec_seg_result&    process_docs::get_results() const { return impl_.get_results(); }
  inline const vec_seg_result&    process_docs::get_errors() const { return impl_.get_errors(); }
  inline std::vector<std::size_t> process_docs::failed_document_indices() const { return impl_.failed_document_indices(); }
} // namespace fsp