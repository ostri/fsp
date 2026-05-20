#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <string>


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
    error_info(processor_error code, std::string msg, std::string_view path, size_t line);
    [[nodiscard]] std::string to_string() const;
  private:
    processor_error code_;
    std::string     message_;
    std::string     path_;
    size_t          line_ = 0;
  };
}; // namespace fsp