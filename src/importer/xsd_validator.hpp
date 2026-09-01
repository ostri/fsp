#pragma once

#include "load_grammar.hpp"
#include "x_str.hpp"
#include <fmt/base.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <xercesc/sax2/DefaultHandler.hpp>
namespace fsp
{
  using str_t     = std::string;
  using vec_str_t = std::vector<str_t>;

  sax_reader_t parser_for_validation(const gr_pool_t& gr_pool);
  void         validate_xml([[maybe_unused]] const std::stop_token& st,
                            const gr_pool_t&                        gr_pool,
                            std::latch&                             gr_latch,
                            std::atomic<bool>&                      gr_loaded,
                            const vec_str_t&                        xml_files);

  /// @brief One SAX validation finding (error/fatalError), structured - row/col/message, as
  /// xercesc::SAXParseException itself carries them - see valid_handler::last_error() for why
  /// this exists alongside the existing fmt::print() diagnostics.
  struct xsd_error_info
  {
    std::int64_t row = 0;
    std::int64_t col = 0;
    str_t        message;
  };

  class valid_handler : public xercesc::DefaultHandler
  {
  public:
    void                reset();
    void                error(const xercesc::SAXParseException& exc) override;
    void                fatalError(const xercesc::SAXParseException& exc) override;
    void                warning(const xercesc::SAXParseException& exc) override;
    [[nodiscard]] bool  is_error() const;
    [[nodiscard]] str_t file() const;
    void                set_file(const str_t& file);

    /// @brief the first error()/fatalError() finding this handler saw since the last reset() -
    /// nullopt if is_error() is false. First, not last: xerces calls the handler once per
    /// finding, in document order, and the first one is usually the most actionable (later ones
    /// are often downstream noise from the same root cause).
    [[nodiscard]] const std::optional<xsd_error_info>& last_error() const;
  private:
    bool                          had_error_ = false; // do we have some error?
    str_t                         file_;              // file we are parsing
    std::optional<xsd_error_info> last_error_;
  };
  inline void fsp::valid_handler::reset()
  {
    had_error_ = false;
    file_      = "unknown";
    last_error_.reset();
  }
  inline void valid_handler::error(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    if (! last_error_)
      last_error_ = xsd_error_info{.row     = static_cast<std::int64_t>(e.getLineNumber()),
                                   .col     = static_cast<std::int64_t>(e.getColumnNumber()),
                                   .message = x_str(e.getMessage()).to_string()};
    fmt::print("Validation error '{}' (row: {}, col: {}):\n  {}\n",
               file_,
               e.getLineNumber(),
               e.getColumnNumber(),
               x_str(e.getMessage()).to_string());
  }
  inline void valid_handler::fatalError(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    if (! last_error_)
      last_error_ = xsd_error_info{.row     = static_cast<std::int64_t>(e.getLineNumber()),
                                   .col     = static_cast<std::int64_t>(e.getColumnNumber()),
                                   .message = x_str(e.getMessage()).to_string()};
    fmt::print(
      "Validation fatal error (row: {}, col: {}): {}\n", e.getLineNumber(), e.getColumnNumber(), fsp::x_str(e.getMessage()).to_string());
  }
  inline str_t valid_handler::file() const { return file_; }
  inline void  valid_handler::set_file(const str_t& file) { file_ = file; }
  inline void  valid_handler::warning(const xercesc::SAXParseException& e)
  {
    fmt::print(
      "Validation warning (row: '{}', col: {}): {}\n", e.getLineNumber(), e.getColumnNumber(), fsp::x_str(e.getMessage()).to_string());
  }
  inline bool                                 valid_handler::is_error() const { return had_error_; }
  inline const std::optional<xsd_error_info>& valid_handler::last_error() const { return last_error_; }


} // namespace fsp
