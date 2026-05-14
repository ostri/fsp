#include "error_info.hpp"
namespace fsp
{

  [[nodiscard]] std::string error_info::to_string() const
  {
    auto code_name = magic_enum::enum_name(code_);

    if (! file_.empty() && line_ > 0) { return fmt::format("[{}] {} (file: {}, line: {})", code_name, message_, file_, line_); }
    if (! file_.empty()) { return fmt::format("[{}] {} (file: {})", code_name, message_, file_); }
    if (line_ > 0) { return fmt::format("[{}] {} (line: {})", code_name, message_, line_); }
    return fmt::format("[{}] {}", code_name, message_);
  }

}; // namespace fsp