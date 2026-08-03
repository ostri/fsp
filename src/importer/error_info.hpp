#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <string>


namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;
  // Error types for expected<T>
  enum class processor_error : std::uint8_t
  {
    unknown = 0,
    success,
    file_open_failed,
    mmap_failed,
    xsd_validation_failed,
    parse_failed,
    worker_thread_error,
    invalid_xpath,
    schema_not_found,
    xml_empty,
    internal_error,
    error_extracting_xpath_values,
    syntax_error,   // XML well-formedness or XSD grammar violation
    semantic_error, // a schema field failed a domain-specific validation rule (see semantic())
  };
  class error_info
  {
  public:
    error_info() = default;
    error_info(processor_error code, str_t msg, cstr_t path, std::size_t line, std::size_t column = 0);

    /**
     * @brief Builds a semantic_error carrying an open, caller-defined domain_code (e.g.
     * "invalid_iban", "amount_out_of_range") alongside the message.
     * @details domain_code is what lets a validated field type (see fsp::validated_t<>/
     * reflection.hpp) or any other caller identify its own specific validation rule without
     * ever needing a new processor_error enumerator -- processor_error stays closed/fixed for
     * the library's own pipeline-mechanics errors, domain_code is open and freeform.
     */
    [[nodiscard]] static error_info semantic(str_t domain_code, str_t msg, cstr_t path = "", std::size_t line = 0, std::size_t column = 0);

    [[nodiscard]] str_t           to_string() const;
    [[nodiscard]] cstr_t          message() const;
    [[nodiscard]] processor_error code() const;
    [[nodiscard]] cstr_t          domain_code() const;
    [[nodiscard]] cstr_t          path() const;
    [[nodiscard]] std::size_t     line() const;
    [[nodiscard]] std::size_t     column() const;
  private:
    processor_error code_{processor_error::unknown};
    str_t           domain_code_; // open sub-category, only meaningful when code_ == semantic_error
    str_t           message_;
    str_t           path_;
    std::size_t     line_   = 0;
    std::size_t     column_ = 0;
  };
  //////////////////////////////////////////////////////////////////////////////////////
  inline std::size_t error_info::column() const { return column_; };
}; // namespace fsp