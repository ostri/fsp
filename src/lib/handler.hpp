#pragma once

// #include <stdexcept>
#include <vector>
#include <string>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax/Locator.hpp>
#include <xercesc/dom/DOMLocator.hpp>
#include "queue.hpp"
// --- SAX Handler ---
namespace fsp
{
  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(const std::vector<std::string>& t, segment_queue& q);

    void startElement([[maybe_unused]] const XMLCh*               uri,
                      [[maybe_unused]] const XMLCh*               localname,
                      [[maybe_unused]] const XMLCh*               qname,
                      [[maybe_unused]] const xercesc::Attributes& attrs) override;

    void characters(const XMLCh* chars, [[maybe_unused]] XMLSize_t length) override;

    void endElement([[maybe_unused]] const XMLCh* uri,
                    [[maybe_unused]] const XMLCh* localname,
                    [[maybe_unused]] const XMLCh* qname) override;

    void fatalError(const xercesc::SAXParseException& e) override;
    void setDocumentLocator(const xercesc::Locator* locator) override;
  private:
    std::vector<std::string> targets_;                // xpath to the subtrees in xml file that we process
                                                      // in parallel (cutting points)
    segment_queue&             queue_;                // queue of sliced subtrees
    size_t                     counter_   = 0;        // number of processed subtrees
    bool                       capturing_ = false;    // are we processing a subtree?
    std::string                buffer_;               // subtree in character format
    int                        active_idx_ = -1;      // which subtry type (xpath) we are processing (0..n)
    const xercesc::DOMLocator* m_locator   = nullptr; // location of the current tag
  };
}; // namespace fsp