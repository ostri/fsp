#pragma once

#include <cstddef>
#include <string_view>
#include <string>
#include <spdlog/logger.h>
#include <xercesc/dom/DOM.hpp>

namespace fsp
{
  class dom_handler : public xercesc::DOMErrorHandler
  {
  public:
    explicit dom_handler(std::shared_ptr<spdlog::logger> logger, int worker_id);
    ~dom_handler() override;
    bool handleError(const xercesc::DOMError& err) override;
    void resetErrors();

    [[nodiscard]] std::size_t                     pos() const;
    [[nodiscard]] std::size_t                     row() const;
    [[nodiscard]] std::size_t                     col() const;
    [[nodiscard]] std::shared_ptr<spdlog::logger> logger() const;
    [[nodiscard]] std::string_view                err_msg() const;
    [[nodiscard]] std::string_view                xml_buf() const;
    void                                          set_xml_buf(std::string_view xml_buf);
  private:
    std::string                     err_msg_;        // error message
    std::shared_ptr<spdlog::logger> logger_;         /// logger
    std::size_t                     col_ = 0;        // column of the error
    std::size_t                     row_ = 0;        // row of the error
    std::size_t                     pos_ = 0;        // byte post in the xml buffer
    std::string_view                xml_buf_;        // xml buffer we are parsing
    int                             worker_id_ = -1; // id of the worker
  };

} // namespace fsp