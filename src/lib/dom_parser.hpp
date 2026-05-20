#pragma once

#include <spdlog/logger.h>
#include <string_view>
#include <string>
#include <memory>
#include <expected>

#include <xercesc/dom/DOM.hpp>

namespace fsp
{
  struct Error
  {
    std::string message;
  };

  class dom_handler;
  class dom_parser
  {
  public:
    explicit dom_parser(std::shared_ptr<spdlog::logger> logger, int worker_id);
    ~dom_parser();
    dom_parser(const dom_parser&)                = delete;
    dom_parser& operator=(const dom_parser&)     = delete;
    dom_parser(dom_parser&&) noexcept            = default;
    dom_parser& operator=(dom_parser&&) noexcept = default;
    // init parser (Xerces platform init is external!)
    std::expected<void, Error> init();
    // parse UTF-8 XML (zero-copy input buffer)
    std::expected<xercesc::DOMDocument*, Error> exec(std::string_view xml_buf);
    // cleanup parser resources
    std::expected<void, Error> done();
  private:
    static void parserDeleter(xercesc::DOMLSParser* p);
  private:
    std::unique_ptr<xercesc::DOMLSParser, void (*)(xercesc::DOMLSParser*)> parser_;
    std::unique_ptr<dom_handler>                                           handler_;
    std::shared_ptr<spdlog::logger>                                        logger_;         /// logger
    int                                                                    worker_id_ = -1; // worker id
  };

} // namespace fsp