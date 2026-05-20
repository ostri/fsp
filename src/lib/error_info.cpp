#include "error_info.hpp"
namespace fsp
{

  [[nodiscard]] error_info::error_info(processor_error code, std::string msg, std::string_view path, size_t line)
  : code_(code)
  , message_(std::move(msg))
  , path_(path)
  , line_(line)
  {
  }

  std::string error_info::to_string() const
  {
    auto code_name = magic_enum::enum_name(code_);

    if (! path_.empty() && line_ > 0) { return fmt::format("[{}] {} (file: {}, line: {})", code_name, message_, path_, line_); }
    if (! path_.empty()) { return fmt::format("[{}] {} (file: {})", code_name, message_, path_); }
    if (line_ > 0) { return fmt::format("[{}] {} (line: {})", code_name, message_, line_); }
    return fmt::format("[{}] {}", code_name, message_);
  }

}; // namespace fsp