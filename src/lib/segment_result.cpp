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
    msg += values_.dump(offs + 2);
    return msg.substr(0, msg.size() - 1); // remove trailing nl
  }

} // namespace fsp