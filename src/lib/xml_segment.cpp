#include "xml_segment.hpp"
#include <utility>

namespace fsp
{
  // Vrne pogled na XML vsebino segmenta iz mmap bufferja.
  // mmap_base mora kazati na začetek mmap-ane datoteke.
  xml_segment::xml_segment(std::size_t id,           // unique segment id
                           int         subtree_type, // target index / subtree type
                           std::size_t offset,       // start from the beggining of the buffer
                           std::size_t length,       // length of this segment (whole xml subtree)
                           //  str_XMLCh_t prefix,       // prefix to be added before the buffer (actual start of the
                           //                            // tag + inherited  ns)
                           str_XMLCh_t ns,   // set of ns as a string which belongs to top level tag
                           str_XMLCh_t attrs //, // set of attribute values which belongs to top level tag
                                             //  x_str       ln,    // localname of top tag
                                             //  x_str       uri    // uri of the top tag
                           )
  : id_(id)
  , subtree_type_(subtree_type)
  , offset_(offset)
  , length_(length)
  // , prefix_(std::move(prefix))
  , ns_(std::move(ns))
  , attrs_(std::move(attrs))
  // , ln_(std::move(ln))
  // , uri_(std::move(uri))
  {
  }
  std::string_view xml_segment::view(const std::byte* mmap_base) const noexcept
  {
    const auto* base = nullptr != mmap_base ? mmap_base : nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {reinterpret_cast<const char*>(base + offset_), length_};
  }
  cstr_t xml_segment::extract_qname_from_offset(std::string_view base) const
  {
    assert(base.data() != nullptr);
    assert(length_ != 0);

    const char* data = reinterpret_cast<const char*>(base.data()); // after top level tag
    const char* pos  = data;

    // Poišči začetek taga '<' (poiščemo nazaj)
    while (pos != nullptr && *pos != '<') { --pos; } // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    if (*pos != '<') throw std::logic_error("missing < ");
    cstr_t tag_open(pos + 1, data - pos); // from opening '<' till closing '>' // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::size_t end_of_tag = tag_open.find_first_of(" \t\r\n>/");
    if (end_of_tag != std::string::npos) return tag_open.substr(0, end_of_tag);
    throw std::logic_error("can't find top level opening tag delimiter");
  }
  std::size_t        xml_segment::id() const { return id_; }
  std::size_t        xml_segment::length() const { return length_; }
  [[nodiscard]] bool xml_segment::empty() const noexcept { return length_ == 0; }
  int                xml_segment::subtree_type() const { return subtree_type_; }
  //  str_XMLCh_t               xml_segment::prefix() const { return prefix_; }
  [[nodiscard]] std::string xml_segment::subtree_str(std::string_view tree_content) const
  {
    thread_local std::string str;
    // str.reserve(prefix().size() + length());
    // str.clear();
    // str.append(x_str(prefix()).to_string_view());
    // str.append(base);
    auto qname = this->extract_qname_from_offset(tree_content);
    str.reserve(ns_.size() + attrs_.size() + length());
    str.clear();
    str = fmt::format("<{0} {1} {2}>{3}</{0}>", qname, x_str(ns_).to_string_view(), x_str(attrs_).to_string_view(), tree_content);
    return str;
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
    return fmt::format(R"({0}segment {1} [subtree type: {2}, offs: {3}, len: {4} ns: {5}]
  {0}{6})",
                       std::string(offs, ' '),
                       id_,
                       subtree_type_,
                       offset_,
                       length_,
                       x_str(ns_).to_string_view(),
                       //  ln_.to_string_view(),
                       //  uri_.to_string_view(),
                       //  x_str(prefix_).to_string_view(),
                       view(mmap_base));
  }
  std::size_t xml_segment::offset() const { return offset_; }
} // namespace fsp
