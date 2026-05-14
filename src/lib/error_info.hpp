#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <string>
#include <utility>


namespace fsp
{
  // Error types for expected<T>
  enum class processor_error : std::uint8_t
  {
    success = 0,
    file_open_failed,
    mmap_failed,
    xsd_validation_failed,
    parse_failed,
    worker_thread_error,
    invalid_xpath,
    schema_not_found,
    xml_empty,
    internal_error
  };
  class error_info
  {
  public:
    error_info(processor_error code, std::string msg, std::string file, size_t line)
    : code_(code)
    , message_(std::move(msg))
    , file_(std::move(file))
    , line_(line)
    {
    }
    [[nodiscard]] std::string to_string() const;
  private:
    processor_error code_;
    std::string     message_;
    std::string     file_;
    size_t          line_ = 0;
  };
}; // namespace fsp