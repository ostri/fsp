#pragma once

#include "error_info.hpp"
#include "doc_set_dscr.hpp"
#include "logger.hpp"
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
      throw e; // NOLINT(hicpp-exception-baseclass) -- caught by type in doc_validator::validate()
    }
    void fatalError(const xercesc::SAXParseException& e) override
    {
      record(e);
      throw e; // NOLINT(hicpp-exception-baseclass)
    }
    void resetErrors() override
    {
      has_error_ = false;
      message_.clear();
    }
    [[nodiscard]] bool               has_error() const noexcept { return has_error_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
  private:
    void        record(const xercesc::SAXParseException& e);
    bool        has_error_ = false;
    std::string message_;
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
    doc_validator(const fsp_logger& log, const doc_set_dscr& ds_dscr);
    // Validates ONE document. Returns true/false (valid/invalid) on success, or an error_info
    // for infrastructure failures (e.g. the XSD itself could not be loaded/compiled) -- that
    // case is fatal for the whole run, distinct from an ordinary "this document is invalid".
    std::expected<bool, error_info>  validate(std::size_t doc_ndx);
    [[nodiscard]] const std::string& last_error_message() const noexcept { return err_handler_.message(); }
  private:
    void_result ensure_grammar_loaded();
  private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
    const fsp_logger&                            log_;
    const doc_set_dscr&                          ds_dscr_;
    std::unique_ptr<xercesc::XMLGrammarPoolImpl> grammar_pool_;
    std::unique_ptr<xercesc::SAX2XMLReader>      parser_;
    validation_error_handler                     err_handler_;
    bool                                         grammar_loaded_ = false;
    const bool                                   log_debug_      = log_.active(lvl_enum::debug);
    const bool                                   log_info_       = log_.active(lvl_enum::info);
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
  };
} // namespace fsp