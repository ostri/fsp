#pragma once

#include "pipeline.hpp"
#include "xerces_mgr.hpp"
#include "importer_config.hpp"
#include <logger/logger.hpp>
#include "segment_result.hpp"
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace fsp
{
  // Public facade — importer itself holds no processing logic; everything is delegated
  // to pipeline, which coordinates the V/C/P hybrid workers.
  class importer
  {
  public:
    ~importer()                          = default;
    importer(const importer&)            = delete;
    importer(importer&&)                 = delete;
    importer& operator=(const importer&) = delete;
    importer& operator=(importer&&)      = delete;

    [[nodiscard]] const vec_seg_result&    get_results() const;
    [[nodiscard]] const vec_seg_result&    get_errors() const;
    [[nodiscard]] std::vector<std::size_t> failed_document_indices() const;

    /**
     * @brief The only way to run an import: builds an importer on the heap and runs
     * import_docs() on it, in a single call -- the constructor and import_docs() are both
     * private (see below) precisely so this is the sole entry point. Returns the importer
     * alongside the result (rather than just the result) so a caller can still inspect
     * get_results()/get_errors()/failed_document_indices() afterwards.
     * @note Returns std::unique_ptr<importer>, not importer by value: importer's copy/move are
     * both deleted (see the class's own deleted special members above) because impl_ holds
     * references back into this SAME instance's log_ptr_/xerces_life_ -- moving the importer
     * would leave those references dangling. Heap allocation is the only way to hand ownership
     * back to the caller without ever relocating the object.
     */
    [[nodiscard]] static std::pair<std::unique_ptr<importer>, result<doc_set_counter>> exec(const importer_config&    cfg,
                                                                                            const std::vector<str_t>& xml_paths,
                                                                                            cstr_t                    xsd_path,
                                                                                            pipeline_hooks& hooks = default_pipeline_hooks);
  private:
    // Both private: the only caller of either is exec() above, itself a static member of this
    // same class (so it keeps access without needing a friend declaration) -- see exec()'s own
    // doc comment for why this is the class's sole public entry point.
    explicit importer(const importer_config& cfg);
    [[nodiscard]] result<doc_set_counter> import_docs(const std::vector<str_t>& xml_paths,
                                                      cstr_t                    xsd_path,
                                                      pipeline_hooks&           hooks = default_pipeline_hooks);

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

  inline importer::importer(const importer_config& cfg)
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

  inline std::pair<std::unique_ptr<importer>, result<doc_set_counter>> importer::exec(const importer_config&    cfg,
                                                                                      const std::vector<str_t>& xml_paths,
                                                                                      cstr_t                    xsd_path,
                                                                                      pipeline_hooks&           hooks)
  {
    // Not std::make_unique<importer>(cfg): make_unique's own "new" expression runs inside
    // <memory>'s implementation, outside importer's class scope, so it can't see the private
    // constructor -- only a "new importer(...)" written HERE, inside a member of importer
    // itself, has access. std::unique_ptr<importer>(...) then takes ownership of that pointer.
    std::unique_ptr<importer> p(new importer(cfg));
    auto                      res = p->import_docs(xml_paths, xsd_path, hooks);
    return {std::move(p), std::move(res)};
  }
} // namespace fsp