#include "dom_handler.hpp"
#include "x_str.hpp"

namespace fsp
{

  [[nodiscard]] dom_handler::dom_handler(std::shared_ptr<spdlog::logger> logger, int worker_id)
  : logger_(std::move(logger))
  , worker_id_(worker_id)
  {
    if (logger_) logger_->debug(fmt::format("Worker {:02}: DOM handler established.", worker_id_));
  }

  dom_handler::~dom_handler()
  {
    if (logger_) logger_->debug(fmt::format("Worker {:02}: DOM handler released.", worker_id_));
  }

  bool dom_handler::handleError(const xercesc::DOMError& err)
  {
    err_msg_  = x_str(err.getMessage()).to_string();
    auto* loc = err.getLocation();
    if (nullptr != loc)
    {
      col_ = loc->getColumnNumber();
      row_ = loc->getLineNumber();
      pos_ = loc->getByteOffset();
      if (pos_ > xml_buf_.size()) pos_ = 0; // tracing is switched off
    }
    std::string err_pos;
    if (pos_ != 0)
    {
      const auto offs_before = 20UL; // number of characters before the error
      err_pos                = std::string(xml_buf_.substr(pos_ - offs_before, (offs_before * 2)));
      err_pos.insert(offs_before, "¤");
    }
    if (logger_)
      logger_->error(fmt::format(R"(dom handler (row:{} col:{} pos:{}) error: '{}'
'{}')",
                                 row_,
                                 col_,
                                 pos_,
                                 err_msg_,
                                 err_pos));

    return false; // we abort on first erro
  }

  void                           dom_handler::resetErrors() { err_msg_.clear(); }
  [[nodiscard]] std::size_t      dom_handler::pos() const { return pos_; }
  [[nodiscard]] std::size_t      dom_handler::row() const { return row_; }
  [[nodiscard]] std::size_t      dom_handler::col() const { return col_; }
  [[nodiscard]] std::string_view dom_handler::err_msg() const { return err_msg_; }

  void dom_handler::set_xml_buf(std::string_view xml_buf) { xml_buf_ = xml_buf; }

} // namespace fsp
