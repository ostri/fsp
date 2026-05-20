#include "dom_parser.hpp"
// #include "x_str.hpp"
#include "dom_handler.hpp"

#include <spdlog/logger.h>
#include <utility>
#include <xercesc/parsers/DOMLSParserImpl.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/dom/DOMDocument.hpp>
#include <xercesc/dom/DOMImplementationRegistry.hpp>
#include <xercesc/dom/DOMImplementationLS.hpp>
#include <xercesc/util/XMLUni.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/sax/ErrorHandler.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/framework/Wrapper4InputSource.hpp>

// #include <utility>

// using xercesc_3_3::ErrorHandler;

// using namespace xercesc;
namespace fsp
{


  // -------------------- constructor / destructor --------------------

  // dom_parser::dom_parser(std::shared_ptr<spdlog::logger> logger)
  // : parser_(nullptr, &dom_parser::parserDeleter)
  // {
  // }

  dom_parser::dom_parser(std::shared_ptr<spdlog::logger> logger, int worker_id)
  : parser_(nullptr, &dom_parser::parserDeleter)
  , logger_(std::move(logger))
  , worker_id_(worker_id)
  {
  }

  dom_parser::~dom_parser() { auto res = done(); }

  // -------------------- deleter --------------------

  void dom_parser::parserDeleter(xercesc::DOMLSParser* p)
  {
    if (nullptr != p) { p->release(); }
  }

  // -------------------- init --------------------

  std::expected<void, Error> dom_parser::init()
  {
    if (parser_) return {};

    try
    {
      auto* impl = xercesc::DOMImplementationRegistry::getDOMImplementation(u"LS");
      if (nullptr == impl) { return std::unexpected(Error{"Failed to get DOMImplementation"}); }
      parser_.reset(static_cast<xercesc::DOMLSParser*>(impl->createLSParser(xercesc::DOMImplementationLS::MODE_SYNCHRONOUS, nullptr)));
      if (! parser_) { return std::unexpected(Error{"Failed to create DOMLSParser"}); }
      handler_ = std::make_unique<dom_handler>(logger_, worker_id_);
      // NOLINTBEGIN(hicpp-no-array-decay)
      auto* cfg = parser_->getDomConfig();
      cfg->setParameter(xercesc::XMLUni::fgDOMErrorHandler, handler_.get());
      cfg->setParameter(xercesc::XMLUni::fgDOMNamespaces, true);
      cfg->setParameter(xercesc::XMLUni::fgXercesCalculateSrcOfs, true); // TODO: ostri - make configurable
      // NOLINTEND(hicpp-no-array-decay)
      return {};
    }
    catch (...)
    {
      return std::unexpected(Error{"Xerces init exception"});
    }
  }

  // -------------------- exec (zero-copy UTF-8 input) --------------------

  std::expected<xercesc::DOMDocument*, Error> dom_parser::exec(std::string_view xml_buf)
  {
    if (! parser_) { return std::unexpected(Error{"Parser not initialized"}); }
    handler_->resetErrors();
    try
    {
      handler_->set_xml_buf(xml_buf);                       // for clear reporting
      xercesc::MemBufInputSource input                      //
        (                                                   //
          reinterpret_cast<const XMLByte*>(xml_buf.data()), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          xml_buf.size(),
          "utf8-zero-copy",
          false // do not adopt buffer
        );
      xercesc::Wrapper4InputSource wrapper(&input, false);
      xercesc::DOMDocument*        doc = parser_->parse(&wrapper);
      if (doc == nullptr)
      {
        return std::unexpected(Error{handler_->err_msg().empty() ? "Unknown parse error" : handler_->err_msg().data()});
      }
      return doc;
    }
    catch (const xercesc::SAXException&)
    {
      return std::unexpected(Error{"SAXException during parsing"});
    }
    catch (const xercesc::DOMException&)
    {
      return std::unexpected(Error{"DOMException during parsing"});
    }
    catch (...)
    {
      return std::unexpected(Error{"Unknown exception during parsing"});
    }
  }

  // -------------------- done --------------------

  std::expected<void, Error> dom_parser::done()
  {
    try
    {
      if (parser_) { parser_.reset(); }

      handler_.reset();

      return {};
    }
    catch (...)
    {
      return std::unexpected(Error{"cleanup failed"});
    }
  }

} // namespace fsp