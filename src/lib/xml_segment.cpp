#include "xml_segment.hpp"

#include <utility>

namespace fsp
{
  // Vrne pogled na XML vsebino segmenta iz mmap bufferja.
  // mmap_base mora kazati na začetek mmap-ane datoteke.
  xml_segment::xml_segment(std::size_t id,
                           int         subtree_type,
                           std::size_t offset,
                           std::size_t length,
                           x_str       prefix,
                           x_str       ns,
                           x_str       ln,
                           x_str       uri)
  : id_(id)
  , subtree_type_(subtree_type)
  , offset_(offset)
  , length_(length)
  , prefix_(std::move(prefix))
  , ns_(std::move(ns))
  , ln_(std::move(ln))
  , uri_(std::move(uri))
  {
  }
  std::string_view xml_segment::view(const std::byte* mmap_base) const noexcept
  {
    const auto* base = nullptr != mmap_base ? mmap_base : nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {reinterpret_cast<const char*>(base + offset_), length_};
  }
  std::size_t               xml_segment::id() const { return id_; }
  std::size_t               xml_segment::length() const { return length_; }
  [[nodiscard]] bool        xml_segment::empty() const noexcept { return length_ == 0; }
  int                       xml_segment::subtree_type() const { return subtree_type_; }
  x_str                     xml_segment::prefix() const { return prefix_; }
  [[nodiscard]] std::string xml_segment::subtree_str(std::string_view base) const
  {
    std::string str;
    str.reserve(prefix().to_string_view().size() + length());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {prefix().to_string() + base};
  }
  [[nodiscard]] std::string xml_segment::dump(int offs) const
  {
    std::string str = fmt::format("{}id: {} subtree type: {} offset: {} length: {}", //
                                  std::string(offs, ' '),
                                  id_,
                                  subtree_type_,
                                  offset_,
                                  length_);
    return str;
  }
  [[nodiscard]] std::string xml_segment::dump_all(std::string_view base, int offs) const //
  { return dump_all(reinterpret_cast<const std::byte*>(base.data()), offs); }
  [[nodiscard]] std::string xml_segment::dump_all(const std::byte* mmap_base, int offs) const
  { //
    return fmt::format(R"({0}segment {1} [subtree type: {2}, offs: {3}, len: {4} ns: {5} ln: {6} uri: {7}]
  {0}{8}{9})",
                       std::string(offs, ' '),
                       id_,
                       subtree_type_,
                       offset_,
                       length_,
                       ns_.to_string_view(),
                       ln_.to_string_view(),
                       uri_.to_string_view(),
                       prefix_.to_string_view(),
                       view(mmap_base));
  }
  std::size_t xml_segment::offset() const { return offset_; }
} // namespace fsp
