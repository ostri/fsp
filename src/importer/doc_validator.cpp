#include "doc_validator.hpp"
#include "x_str.hpp"
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/validators/common/Grammar.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/util/XMLUni.hpp>
#include <fmt/format.h>
#include <chrono>

namespace fsp
{
  void validation_error_handler::record(const xercesc::SAXParseException& e)
  {
    if (has_error_) return; // keep the first reported error
    has_error_ = true;
    message_   = fmt::format("row:{} col:{} - {}", e.getLineNumber(), e.getColumnNumber(), x_str(e.getMessage()).to_string());
  }

  doc_validator::doc_validator(const logger::Logger& log, const doc_set_dscr& ds_dscr)
  : log_(log)
  , ds_dscr_(ds_dscr)
  {
  }

  // Loads the XSD grammar into a fresh, thread-local pool, locks it, then builds a validating
  // reader restricted to that single, static grammar (fgSGXMLScanner + fgXercesDynamic=false:
  // "a scanner that supports only XSD grammars", nothing dynamically discovered, no DTDs).
  e_void doc_validator::ensure_grammar_loaded()
  {
    if (grammar_loaded_) return {};
    try
    {
      grammar_pool_ = std::make_unique<xercesc::XMLGrammarPoolImpl>(xercesc::XMLPlatformUtils::fgMemoryManager);

      std::unique_ptr<xercesc::SAX2XMLReader> loader(
        xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, grammar_pool_.get()));
      // NOLINTBEGIN(hicpp-no-array-decay)
      loader->setFeature(xercesc::XMLUni::fgXercesDynamic, false);
      // NOLINTEND(hicpp-no-array-decay)

      const str_t xsd_path{ds_dscr_.xsd_file()};
      auto*       grammar = loader->loadGrammar(xsd_path.c_str(), xercesc::Grammar::SchemaGrammarType, true);
      if (grammar == nullptr)
        return std::unexpected(
          error_info{processor_error::internal_error, fmt::format("Failed to load XSD grammar: '{}'", xsd_path), "", 0});
      grammar_pool_->lockPool();

      parser_.reset(xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, grammar_pool_.get()));
      // NOLINTBEGIN(hicpp-no-array-decay)
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchema, true);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
      parser_->setFeature(xercesc::XMLUni::fgXercesDynamic, false); // only the pre-loaded, locked grammar is used
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      parser_->setProperty(xercesc::XMLUni::fgXercesScannerName, const_cast<XMLCh*>(xercesc::XMLUni::fgSGXMLScanner));
      // NOLINTEND(hicpp-no-array-decay)
      parser_->setErrorHandler(&err_handler_);

      grammar_loaded_ = true;
      if (log_debug_) log_.debug(fmt::format("V: grammar '{}' loaded and locked in this thread.", xsd_path));
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      return std::unexpected(
        error_info{processor_error::internal_error, fmt::format("Grammar load failed: {}", x_str(e.getMessage()).to_string()), "", 0});
    }
  }

  std::expected<bool, error_info> doc_validator::validate(std::size_t doc_ndx)
  {
    if (auto res = ensure_grammar_loaded(); ! res) return std::unexpected(res.error());
    auto t0 = std::chrono::steady_clock::now();
    err_handler_.resetErrors();
    const auto doc   = ds_dscr_[doc_ndx].string_view();
    bool       valid = true;
    try
    {
      xercesc::MemBufInputSource src(reinterpret_cast<const XMLByte*>(doc.data()), static_cast<XMLSize_t>(doc.size()), "xml_input", false);
      parser_->parse(src);
      valid = ! err_handler_.has_error();
    }
    catch (const xercesc::SAXParseException&)
    {
      // error()/fatalError() already recorded the details and threw to short-circuit parsing
      // the rest of an already-invalid document.
      valid = false;
    }
    catch (const xercesc::XMLException& e)
    {
      // A parse-level exception (not just a validation error reported via the ErrorHandler)
      // still means the document is not valid against the schema -- not an infra failure. Never
      // routed through error()/fatalError(), so last_error_source() would otherwise still read
      // 'none' here -- always well-formedness (see set_well_formed_error()'s own doc comment).
      if (log_debug_) log_.debug(fmt::format("Doc {}: validation parse exception: {}", doc_ndx, x_str(e.getMessage()).to_string()));
      err_handler_.set_well_formed_error();
      valid = false;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (log_info_) log_.info(fmt::format("Doc {}: validation {} ({} ms).", doc_ndx, valid ? "OK" : "FAILED", ms));
    return valid;
  }
} // namespace fsp