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
    for (const auto& val : values_)
    {
      std::string m;
      auto        ndx = 0U;
      for (auto e : val) m += fmt::format("'{}', ", e); // set of values
      msg += fmt::format(R"(  [{}] = [{}]
)",
                         ndx++,
                         m.substr(0, m.size() - 2));
    }
    return msg.substr(0, msg.size() - 1); // remove trailing nl
  }

} // namespace fsp