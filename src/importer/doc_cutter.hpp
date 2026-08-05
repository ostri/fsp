#pragma once

#include "handler.hpp"
#include "processor_config.hpp"
#include "doc_set_dscr.hpp"
#include "segment_pool.hpp"
#include "xpath_helpers.hpp"
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include <memory>

namespace fsp
{
  // Narrow "C toolkit": owns the xercesc parser + Handler, one instance per hybrid thread.
  // Knows nothing about parallelism, spawns no sub-threads — cuts exactly ONE document per call.
  class doc_cutter
  {
  public:
    doc_cutter(const processor_config& cfg, const logger::Logger& log, segment_pool& pool, const doc_set_dscr& ds_dscr);
    void_result               init();                   //< once, when the hybrid thread starts
    void_result               cut(std::size_t doc_ndx); //< cuts ONE document; caller checks doc_status before calling
    [[nodiscard]] std::size_t segments_found() const noexcept;
  private:
    void_result setup_parser_no_validation();
    // Experiment (cfg_.cut_with_validation): folds XSD validation into this same SAX pass --
    // see doc_cutter.cpp for details. Mirrors doc_validator::ensure_grammar_loaded()'s grammar
    // setup, plus the offset-tracking feature C needs that V doesn't.
    void_result setup_parser_with_validation();
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const logger::Logger&    log_;
    const processor_config&  cfg_;
    segment_pool&            seg_pool_;
    const doc_set_dscr&      ds_dscr_;
    // Declared before parser_ so it outlives it (members destroyed in reverse declaration order)
    // -- only ever constructed when cfg_.cut_with_validation is actually used, see init().
    std::unique_ptr<xercesc::XMLGrammarPoolImpl> grammar_pool_;
    std::unique_ptr<xercesc::SAX2XMLReader>      parser_;
    std::unique_ptr<Handler>                     handler_;
    const bool                                   log_debug_ = log_.active(logger::level::debug);
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };

  inline std::size_t doc_cutter::segments_found() const noexcept { return handler_->segments_found(); }
} // namespace fsp