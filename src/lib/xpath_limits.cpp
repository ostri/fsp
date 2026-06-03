#include "xpath_limits.hpp"
namespace fsp
{

  [[nodiscard]] std::string p_limits::dump(int offs) const
  {
    return fmt::format("{}low:[{} {} {}] high: [{} {} {}] max: {}", //
                       std::string(offs, ' '),
                       low_tag_,
                       low_uri_,
                       low_ndx_,
                       high_tag_,
                       high_uri_,
                       high_ndx_,
                       max_);
  }

  std::size_t p_limits::low_ndx() const { return low_ndx_; }

  std::size_t p_limits::high_ndx() const { return high_ndx_; }

  cstr_t p_limits::low_tag() const { return low_tag_; }

  cstr_t p_limits::high_tag() const { return high_tag_; }

  std::size_t p_limits::max() const { return max_; }

  cstr_t p_limits::low_uri() const { return low_uri_; }

  cstr_t p_limits::high_uri() const { return high_uri_; }

  std::vector<bool> p_limits::available() const { return available_; }

} // namespace fsp