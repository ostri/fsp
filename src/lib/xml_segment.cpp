#include "xml_segment.hpp"

namespace fsp
{
  // Vrne pogled na XML vsebino segmenta iz mmap bufferja.
  // mmap_base mora kazati na začetek mmap-ane datoteke.
  xml_segment::xml_segment(std::size_t      id,          // segment id
                           int              xpath_index, // target index
                           std::size_t      offset,      // start from the beggining of the buffer
                           std::size_t      length,      // length of the character buffer
                           std::string_view prefix,      // prefix to be added before the buffer (actial start of the
                                                         // tag + inherited  ns)
                           int seg_type                  // segment type (ndx of the split xpath)
                           )
  : id_(id)
  , xpath_index_(xpath_index)
  , offset_(offset)
  , length_(length)
  , prefix_(prefix)
  , seg_type_(seg_type)
  {
  }
  std::string_view xml_segment::view(const std::byte* mmap_base) const noexcept
  {
    const auto* base = nullptr != mmap_base ? mmap_base : nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {reinterpret_cast<const char*>(base + offset_), length_};
  }
  std::size_t               xml_segment::get_id() const { return id_; }
  std::size_t               xml_segment::get_length() const { return length_; }
  [[nodiscard]] bool        xml_segment::empty() const noexcept { return length_ == 0; }
  int                       xml_segment::get_xpath_index() const { return xpath_index_; }
  std::string               xml_segment::prefix() const { return prefix_; }
  [[nodiscard]] std::string xml_segment::dump(const std::byte* mmap_base) const
  { //
    return fmt::format(R"(segment {} [xpath ndx: {}, offs: {}, len: {}]
  '{}{}')",
                       id_,
                       xpath_index_,
                       offset_,
                       length_,
                       prefix_,
                       view(mmap_base));
  }
  std::size_t xml_segment::get_offset() const { return offset_; }
} // namespace fsp
