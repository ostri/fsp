#include "segment_result.hpp"
namespace fsp
{

  str_t fsp::segment_result::dump(int offs)
  {
    str_t msg = fmt::format(R"({}seg: {} seg type: {} values:
)",
                            str_t(offs, ' '),
                            seg_id_,
                            seg_type_);
    msg += values_.dump(offs + 2);
    return msg.substr(0, msg.size() - 1); // remove trailing nl
  }

} // namespace fsp