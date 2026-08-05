#pragma once

#include "pipeline.hpp"
#include "xerces_mgr.hpp"
#include "processor_config.hpp"
#include <logger/logger.hpp>
#include "segment_result.hpp"
#include <memory>
#include <stdexcept>
#include <vector>

namespace fsp
{
  // Public facade — importer itself holds no processing logic; everything is delegated
  // to pipeline, which coordinates the V/C/P hybrid workers.
  class importer
  {
  public:
    explicit importer(const processor_config& cfg);
    //    import_docs(processor_config cfg, const str_t& parent_log_name);
    ~importer()                          = default;
    importer(const importer&)            = delete;
    importer(importer&&)                 = delete;
    importer& operator=(const importer&) = delete;
    importer& operator=(importer&&)      = delete;

    [[nodiscard]] result<doc_set_counter>  import_docs(const std::vector<str_t>& xml_paths,
                                                       cstr_t                    xsd_path,
                                                       pipeline_hooks&           hooks = default_pipeline_hooks);
    [[nodiscard]] const vec_seg_result&    get_results() const;
    [[nodiscard]] const vec_seg_result&    get_errors() const;
    [[nodiscard]] std::vector<std::size_t> failed_document_indices() const;
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    // logger::Logger has no public constructor (only Logger::create(), see make_main_logger()
    // below) and is neither copyable nor movable, so it can only be held here as a unique_ptr,
    // not by value like the old fsp_logger.
    const std::unique_ptr<logger::Logger> log_ptr_;     //< "main" logger; must exist before xerces_life_ and impl_
    const xerces_mgr                      xerces_life_; //< must be constructed before, and destructed after, impl_ (xercesc lifetime)
    pipeline                              impl_;        //< actual implementation -- coordinates the V/C/P hybrid pipeline
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  namespace detail
  {
    /// @brief Builds importer's "main" logger from cfg, or throws if it could not be built.
    /// Factored out of the constructor's init-list so a failed logger::Logger::create() can be
    /// turned into an exception before log_ptr_ needs a value.
    inline std::unique_ptr<logger::Logger> make_main_logger(const logger::logger_config& cfg)
    {
      auto log_ptr = logger::Logger::create(cfg);
      if (! log_ptr)
      {
        // Logger::create() has already logged why to stderr -- fatal-startup policy: turn a
        // broken sink into an exception, caught by main()'s own try/catch (pacs8.cpp/pacs8-cb.cpp)
        // and reported as a non-zero exit code, same as any other startup failure.
        throw std::runtime_error("failed to create main logger: " + log_ptr.error());
      }
      return std::move(*log_ptr);
    }
  } // namespace detail

  inline importer::importer(const processor_config& cfg)
  : log_ptr_(detail::make_main_logger(cfg.log_config))
  , xerces_life_()
  , impl_(cfg, *log_ptr_, cfg.program_name)
  {
  }

  inline result<doc_set_counter> importer::import_docs(const std::vector<str_t>& xml_paths, cstr_t xsd_path, pipeline_hooks& hooks)
  { return impl_.process_files(xml_paths, xsd_path, hooks); }
  inline const vec_seg_result&    importer::get_results() const { return impl_.get_results(); }
  inline const vec_seg_result&    importer::get_errors() const { return impl_.get_errors(); }
  inline std::vector<std::size_t> importer::failed_document_indices() const { return impl_.failed_document_indices(); }
} // namespace fsp