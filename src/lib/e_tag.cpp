#include "e_tag.hpp"
namespace fsp
{

  bool                      e_tag::operator==(const e_tag& other) const { return ns_ == other.ns_ && tag_ == other.tag_; }
  bool                      e_tag::operator!=(const e_tag& other) const { return ! (*this == other); }
  [[nodiscard]] std::string e_tag::to_string() const
  {
    if (ns_.empty()) return tag_;
    return fmt::format("{}:{}", ns_, tag_);
  }
  std::string e_tag::tag() const { return tag_; }
  void        e_tag::set_tag(const std::string& tag) { tag_ = tag; }
  std::string e_tag::ns() const { return ns_; }
  void        e_tag::set_ns(const std::string& ns) { ns_ = ns; }
}; // namespace fsp
