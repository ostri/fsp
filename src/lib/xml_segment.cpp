#include "xml_segment.hpp"

namespace fsp
{
  // Vrne pogled na XML vsebino segmenta iz mmap bufferja.
  // mmap_base mora kazati na začetek mmap-ane datoteke.
  xml_segment::xml_segment(std::size_t      id,           // segment id
                           int              subtree_type, // target index
                           std::size_t      offset,       // start from the beggining of the buffer
                           std::size_t      length,       // length of the character buffer
                           std::string_view prefix        // prefix to be added before the buffer (actial start of the
                                                          // tag + inherited  ns)
                           )
  : id_(id)
  , subtree_type_(subtree_type)
  , offset_(offset)
  , length_(length)
  , prefix_(prefix)
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
  std::string               xml_segment::prefix() const { return prefix_; }
  [[nodiscard]] std::string xml_segment::subtree_str(std::string_view base) const
  {
    std::string str;
    str.reserve(prefix().size() + length());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {prefix() + base};
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
    return fmt::format(R"({0}segment {1} [subtree type: {2}, offs: {3}, len: {4}]
  {0}{5}{6})",
                       std::string(offs, ' '),
                       id_,
                       subtree_type_,
                       offset_,
                       length_,
                       prefix_,
                       view(mmap_base));
  }
  std::size_t xml_segment::offset() const { return offset_; }
} // namespace fsp
