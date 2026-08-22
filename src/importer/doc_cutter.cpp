#include "doc_cutter.hpp"
#include "x_str.hpp"
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/validators/common/Grammar.hpp>

namespace fsp
{
  using std::make_unique;

  doc_cutter::doc_cutter(const importer_config& cfg, const logger::Logger& log, segment_pool& pool, const doc_set_dscr& ds_dscr)
  : log_(log)
  , cfg_(cfg)
  , seg_pool_(pool)
  , ds_dscr_(ds_dscr)
  {
  }

  // Identical body to xml_processor::setup_parser_no_validation() — same xercesc configuration,
  // just without validation grammar (validation is a separate V responsibility in this pipeline).
  e_void doc_cutter::setup_parser_no_validation()
  {
    try
    {
      parser_.reset(xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager));
      // NOLINTBEGIN(hicpp-no-array-decay)
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchema, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesLoadExternalDTD, false);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      parser_->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpacePrefixes, false);
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      parser_->setProperty(xercesc::XMLUni::fgXercesScannerName, const_cast<XMLCh*>(xercesc::XMLUni::fgWFXMLScanner));
      // NOLINTEND(hicpp-no-array-decay)

      if (log_debug_) log_.debug("Parser (WFXMLScanner - no grammar) setup successful");
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      return std::unexpected(
        error_info{processor_error::internal_error, fmt::format("Parser init failed: {}", x_str(e.getMessage()).to_string()), "", 0});
    }
  }

  // Experiment (cfg_.cut_with_validation): loads the XSD grammar into a fresh, thread-local
  // pool, locks it, then builds a validating SGXMLScanner reader bound to that single grammar --
  // identical pattern to doc_validator::ensure_grammar_loaded(), plus fgXercesCalculateSrcOfs
  // which C (unlike V) needs for Handler::endElement()'s getSrcOffset()-based segment cutting.
  e_void doc_cutter::setup_parser_with_validation()
  {
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
      parser_->setFeature(xercesc::XMLUni::fgXercesCalculateSrcOfs, true); // needed by Handler::endElement()
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      parser_->setProperty(xercesc::XMLUni::fgXercesScannerName, const_cast<XMLCh*>(xercesc::XMLUni::fgSGXMLScanner));
      // NOLINTEND(hicpp-no-array-decay)

      if (log_debug_) log_.debug(fmt::format("Parser (SGXMLScanner - validating against '{}') setup successful", xsd_path));
      return {};
    }
    catch (const xercesc::XMLException& e)
    {
      return std::unexpected(error_info{
        processor_error::internal_error, fmt::format("Validating parser init failed: {}", x_str(e.getMessage()).to_string()), "", 0});
    }
  }

  e_void doc_cutter::init()
  {
    // Must match pipeline::process_files()'s identical computation of the same condition exactly
    // -- both independently decide from the same (has_grammar(), doc count) inputs, since pipeline
    // needs it to decide whether to also run a separate V pass, and doc_cutter needs it to decide
    // which parser/scanner to set up. See importer_config.hpp for the measured break-even this
    // default (single doc: separate: multiple docs: merged) is based on.
    const bool validate_here = ds_dscr_.has_grammar() && cfg_.cut_with_validation.value_or(ds_dscr_.size() > 1);
    auto       ps            = validate_here ? setup_parser_with_validation() : setup_parser_no_validation();
    if (! ps) return std::unexpected(ps.error());
    try
    {
      // cfg_.targets.is_header is already a dense, index-by-seg_type() lookup vector -- filled by
      // proc_data_of() from each schema class's own static consteval is_header() (see
      // reflection.hpp's seg_schema/hdr_seg_schema and proc_data's own doc comment), same
      // declaration-order indexing as cfg_.targets.xpaths/segment_result::seg_type() -- nothing
      // left to resolve here, just pass it straight through to Handler.
      handler_ = make_unique<Handler>(cfg_.targets, log_, parser_.get(), seg_pool_, ds_dscr_, cfg_.targets.is_header);
      handler_->set_validating(validate_here);
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      return std::unexpected(error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0});
    }
    return {};
  }

  e_void doc_cutter::cut(std::size_t doc_ndx)
  {
    handler_->set_doc(ds_dscr_[doc_ndx].string_view());
    handler_->set_doc_ndx(static_cast<int>(doc_ndx));
    try
    {
      xercesc::MemBufInputSource src(
        reinterpret_cast<const XMLByte*>(handler_->doc().data()), static_cast<XMLSize_t>(handler_->doc().size()), "xml_input", false);
      // Xerces defaults to giving each stream it creates its OWN copy of the buffer (see
      // MemBufInputSource::setCopyBufToStream()'s own doc comment) -- adoptBuffer=false above only
      // controls ownership of THIS input source's buffer, not that per-stream copy. handler_->doc()
      // is a string_view into ds_dscr_[doc_ndx]'s own mmap, which stays valid for this call's whole
      // duration (well past parser_->parse() returning), so the copy is pure waste here -- confirmed
      // via strace (operator new -> BinMemInputStream ctor -> MemBufInputSource::makeStream()) as
      // the single largest allocation in a cutter's own SAX pass.
      src.setCopyBufToStream(false);
      parser_->parse(src);
    }
    catch (const xercesc::SAXParseException& e)
    {
      // Handler::error()/fatalError() already logged the details and threw to short-circuit
      // cutting the rest of an already-invalid document (see Handler::set_validating()). Both
      // callbacks throw this SAME exception type, so the exception itself can't tell schema
      // (error()) apart from well-formedness (fatalError()) -- read Handler's own
      // last_error_source() instead (point 15/16 of the design discussion this implements).
      const auto err_code = handler_->last_error_source() == sax_error_source::validity ? processor_error::xsd_validation_failed
                                                                                        : processor_error::parse_failed;
      return std::unexpected(error_info{err_code, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getLineNumber())});
    }
    catch (const xercesc::XMLException& e)
    {
      return std::unexpected(
        error_info{processor_error::parse_failed, x_str(e.getMessage()).to_string(), "", static_cast<std::size_t>(e.getSrcLine())});
    }
    catch (const std::exception& e)
    {
      return std::unexpected(error_info{processor_error::parse_failed, e.what(), "", 0});
    }
    return {};
  }

} // namespace fsp