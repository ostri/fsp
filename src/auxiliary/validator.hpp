#pragma once

#include "load_grammar.hpp"
#include "x_str.hpp"
#include <fmt/base.h>
#include <vector>
#include <xercesc/sax2/DeclHandler.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
namespace fsp
{
  using str_t     = std::string;
  using vec_str_t = std::vector<str_t>;

  class valid_handler : public xercesc::DefaultHandler
  {
  public:
    void                validate_xml([[maybe_unused]] const std::stop_token& st,
                                     const gr_pool_t&                        gr_pool,
                                     std::latch&                             gr_latch,
                                     std::atomic<bool>&                      gr_loaded,
                                     const vec_str_t&                        xml_files);
    sax_reader_t        prepare_parser(const gr_pool_t& gr_pool);
    void                reset();
    void                error(const xercesc::SAXParseException& exc) override;
    void                fatalError(const xercesc::SAXParseException& exc) override;
    void                warning(const xercesc::SAXParseException& exc) override;
    [[nodiscard]] bool  is_error() const;
    [[nodiscard]] str_t file() const;
    void                set_file(const str_t& file);
  private:
    bool  had_error_ = false; // do we have some error?
    str_t file_;              // file we are parsing
  };
  inline void fsp::valid_handler::reset()
  {
    had_error_ = false;
    file_      = "unknown";
  }
  inline void valid_handler::error(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    fmt::print("Validation error '{}' (row: {}, col: {}):\n  {}\n",
               file_,
               e.getLineNumber(),
               e.getColumnNumber(),
               x_str(e.getMessage()).to_string());
  }
  inline void valid_handler::fatalError(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
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
  inline bool valid_handler::is_error() const { return had_error_; }

  void validate_xml([[maybe_unused]] const std::stop_token& st,
                    const gr_pool_t&                        gr_pool,
                    std::latch&                             gr_latch,
                    std::atomic<bool>&                      gr_loaded,
                    const vec_str_t&                        xml_files);
} // namespace fsp
