#pragma once

#include "error_info.hpp"
#include "doc_set_dscr.hpp"
#include "handler.hpp" // IWYU pragma: keep -- sax_error_source, shared with Handler (see its own doc comment)
#include <logger/logger.hpp>
#include "xpath_helpers.hpp"
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include <xercesc/sax/ErrorHandler.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <expected>
#include <memory>
#include <string>

namespace fsp
{
  // Minimal ErrorHandler that records the first validation error, then throws it straight back
  // out of parser_->parse() -- once a document is known invalid, there's no value in continuing
  // to validate the rest of it, so this short-circuits the parse instead of paying full CPU cost
  // for content we're going to discard anyway.
  class validation_error_handler : public xercesc::ErrorHandler
  {
  public:
    void warning(const xercesc::SAXParseException& /*e*/) override { }
    void error(const xercesc::SAXParseException& e) override
    {
      record(e);
      last_error_source_ = sax_error_source::validity; // schema/validity constraint violation
      throw e;                                         // NOLINT(hicpp-exception-baseclass) -- caught by type in doc_validator::validate()
    }
    void fatalError(const xercesc::SAXParseException& e) override
    {
      record(e);
      last_error_source_ = sax_error_source::well_formed; // well-formedness violation
      throw e;                                            // NOLINT(hicpp-exception-baseclass)
    }
    void resetErrors() override
    {
      has_error_ = false;
      message_.clear();
      last_error_source_ = sax_error_source::none;
    }
    [[nodiscard]] bool         has_error() const noexcept { return has_error_; }
    [[nodiscard]] const str_t& message() const noexcept { return message_; }
    // Which of error()/fatalError() most recently reported a problem -- read by
    // doc_validator::validate() from its catch block, after the fact (see Handler's own
    // identical sax_error_source, shared type -- point 15/16 of the design discussion this
    // implements).
    [[nodiscard]] sax_error_source last_error_source() const noexcept { return last_error_source_; }
    // Called by doc_validator::validate() when parser_->parse() throws xercesc::XMLException
    // directly (a lower-level parse failure that never reaches error()/fatalError() at all, so
    // last_error_source_ would otherwise stay 'none') -- always well-formedness, not a validity
    // constraint: a document xercesc can't even parse can't have gotten far enough to be judged
    // against the schema.
    void set_well_formed_error() noexcept { last_error_source_ = sax_error_source::well_formed; }
  private:
    void             record(const xercesc::SAXParseException& e);
    bool             has_error_ = false;
    str_t            message_;
    sax_error_source last_error_source_ = sax_error_source::none;
  };

  // Narrow "V toolkit": owns one grammar pool + one SGXMLScanner-based reader, bound to a
  // single, pre-loaded, locked XSD grammar. One instance per hybrid thread.
  //
  // Grammar loading is lazy (happens on the first validate() call) and always happens on the
  // SAME thread that will subsequently use it for validation -- loading a grammar pool from
  // one thread and validating with it from another is unreliable in xercesc, so a doc_validator
  // instance must never be shared or moved across threads.
  class doc_validator
  {
  public:
    doc_validator(const logger::Logger& log, const doc_set_dscr& ds_dscr);
    // Validates ONE document. Returns true/false (valid/invalid) on success, or an error_info
    // for infrastructure failures (e.g. the XSD itself could not be loaded/compiled) -- that
    // case is fatal for the whole run, distinct from an ordinary "this document is invalid".
    std::expected<bool, error_info> validate(std::size_t doc_ndx);
    [[nodiscard]] const str_t&      last_error_message() const noexcept { return err_handler_.message(); }
    // Which of error()/fatalError() the underlying validation_error_handler last saw -- see
    // sax_error_source's own doc comment (point 15/16 of the design discussion this implements).
    // Only meaningful right after a validate() call that returned false.
    [[nodiscard]] sax_error_source last_error_source() const noexcept { return err_handler_.last_error_source(); }
  private:
    e_void ensure_grammar_loaded();
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const logger::Logger&                        log_;
    const doc_set_dscr&                          ds_dscr_;
    std::unique_ptr<xercesc::XMLGrammarPoolImpl> grammar_pool_;
    std::unique_ptr<xercesc::SAX2XMLReader>      parser_;
    validation_error_handler                     err_handler_;
    bool                                         grammar_loaded_ = false;
    const bool                                   log_debug_      = log_.active(logger::level::debug);
    const bool                                   log_info_       = log_.active(logger::level::info);
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp