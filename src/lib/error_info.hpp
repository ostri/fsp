#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <string>


namespace fsp
{
  using cstr_t = std::string_view;
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
    error_extracting_xpath_values
  };
  class error_info
  {
  public:
    error_info() = default;
    error_info(processor_error code, std::string msg, std::string_view path, std::size_t line, std::size_t column = 0);
    [[nodiscard]] std::string     to_string() const;
    [[nodiscard]] cstr_t          message() const;
    [[nodiscard]] processor_error code() const;
    [[nodiscard]] cstr_t          path() const;
    [[nodiscard]] std::size_t     line() const;
    [[nodiscard]] std::size_t     column() const;
  private:
    processor_error code_{processor_error::unknown};
    std::string     message_;
    std::string     path_;
    std::size_t     line_   = 0;
    std::size_t     column_ = 0;
  };
  //////////////////////////////////////////////////////////////////////////////////////
  inline std::size_t error_info::column() const { return column_; };
}; // namespace fsp