#include "segment_result.hpp"

std::string fsp::segment_result::dump(int offs)
{
  std::string msg = fmt::format("{}seg: {} xpath ndx: {}\n", std::string(offs, ' '), segment_id, xpath_index);
  for (const auto& [key, el] : values)
  {
    std::string m;
    for (auto e : el) m += fmt::format("{}, ", e);
    msg += fmt::format("[{}] = {}\n", key, m.substr(0, m.size() - 2));
  }
  return msg;
}
