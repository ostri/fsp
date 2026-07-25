#include "doc_cutter.hpp"
#include "x_str.hpp"
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>

namespace fsp
{
  using std::make_unique;

  doc_cutter::doc_cutter(const processor_config& cfg, const fsp_logger& log, segment_pool& pool, const doc_set_dscr& ds_dscr)
  : log_(log)
  , cfg_(cfg)
  , seg_pool_(pool)
  , ds_dscr_(ds_dscr)
  {
  }

  // Identical body to xml_processor::setup_parser_no_validation() — same xercesc configuration,
  // just without validation grammar (validation is a separate V responsibility in this pipeline).
  void_result doc_cutter::setup_parser_no_validation()
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

  void_result doc_cutter::init()
  {
    if (auto ps = setup_parser_no_validation(); ! ps) return std::unexpected(ps.error());
    try
    {
      handler_ = make_unique<Handler>(cfg_.targets, log_, parser_.get(), seg_pool_, ds_dscr_);
      parser_->setContentHandler(handler_.get());
      parser_->setErrorHandler(handler_.get());
    }
    catch (const std::exception& e)
    {
      return std::unexpected(error_info{processor_error::internal_error, fmt::format("Handler init: {}", e.what()), "", 0});
    }
    return {};
  }

  void_result doc_cutter::cut(std::size_t doc_ndx)
  {
    handler_->set_doc(ds_dscr_[doc_ndx].string_view());
    handler_->set_doc_ndx(static_cast<int>(doc_ndx));
    try
    {
      xercesc::MemBufInputSource src(
        reinterpret_cast<const XMLByte*>(handler_->doc().data()), static_cast<XMLSize_t>(handler_->doc().size()), "xml_input", false);
      parser_->parse(src);
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