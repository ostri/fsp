#include "error_info.hpp"
namespace fsp
{

  [[nodiscard]] error_info::error_info(processor_error code, str_t msg, cstr_t path, std::size_t line, std::size_t column)
  : code_(code)
  , message_(std::move(msg))
  , path_(path)
  , line_(line)
  , column_(column)
  {
  }

  error_info error_info::semantic(str_t domain_code, str_t msg, cstr_t path, std::size_t line, std::size_t column)
  {
    error_info e{processor_error::semantic_error, std::move(msg), path, line, column};
    e.domain_code_ = std::move(domain_code);
    return e;
  }

  processor_error error_info::code() const { return code_; }
  cstr_t          error_info::domain_code() const { return domain_code_; }

  str_t error_info::to_string() const
  {
    auto code_name = magic_enum::enum_name(code_);
    auto prefix    = domain_code_.empty() ? fmt::format("[{}]", code_name) : fmt::format("[{}:{}]", code_name, domain_code_);

    if (! path_.empty() && line_ > 0) { return fmt::format("{} {} (file: {}, line: {})", prefix, message_, path_, line_); }
    if (! path_.empty()) { return fmt::format("{} {} (file: {})", prefix, message_, path_); }
    if (line_ > 0) { return fmt::format("{} {} (line: {})", prefix, message_, line_); }
    return fmt::format("{} {}", prefix, message_);
  }

  cstr_t error_info::path() const { return path_; }

  cstr_t error_info::message() const { return message_; }

  size_t error_info::line() const { return line_; }

}; // namespace fsp