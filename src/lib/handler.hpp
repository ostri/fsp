#pragma once

// #include <stdexcept>
#include <vector>
#include <string>
#include <xercesc/sax2/DefaultHandler.hpp>
#include "lib/queue.hpp"
// --- SAX Handler ---
namespace fsp
{
  class Handler : public xercesc::DefaultHandler
  {
    std::vector<std::string> targets_;
    segment_queue&           queue_;
    size_t                   counter_   = 0;
    bool                     capturing_ = false;
    std::string              buffer_;
    int                      active_idx_ = -1;
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
  };
}; // namespace fsp