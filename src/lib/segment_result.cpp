#include "segment_result.hpp"
namespace fsp
{

  std::string fsp::segment_result::dump(int offs)
  {
    std::string msg = fmt::format(R"({}seg: {} seg type: {} values:
)",
                                  std::string(offs, ' '),
                                  seg_id_,
                                  seg_type_);
    for (const auto& [key, el] : values_)
    {
      std::string m;
      for (auto e : el) m += fmt::format("'{}', ", e); // set of values
      msg += fmt::format(R"(  [{}] = [{}]
)",
                         key,
                         m.substr(0, m.size() - 2));
    }
    return msg.substr(0, msg.size() - 1); // remove trailing nl
  }

} // namespace fsp