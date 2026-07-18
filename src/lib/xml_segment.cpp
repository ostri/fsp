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
                           str_XMLCh_t ns,           // set of ns as a string which belongs to top level tag
                           str_XMLCh_t attrs         // set of attribute values which belongs to top level tag
                           )
  : id_(id)
  , subtree_type_(subtree_type)
  , offset_(offset)
  , length_(length)
  , ns_(std::move(ns))
  , attrs_(std::move(attrs))
  {
  }
  /**
   * @brief returns the xml doc segment as defined by mmap_base, offset and length
   *
   * @return std::string_view
   * @param mmap_base address of the start of the xml document
   */
  std::string_view xml_segment::view(const std::byte* mmap_base) const noexcept
  {
    const auto* base = nullptr != mmap_base ? mmap_base : nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return {reinterpret_cast<const char*>(base + offset_), length_};
  }
  /**
   * @brief return sv which denotes qname (tag or prefix:tag)
   * The method searches for xml element q name.
   * The metod first rolls back to the start of the xml element (char '<'). According wit the standard between
   * end of opening xml element opening tag, there must not be any '<' character but that that denotes the
   * very start of the xml element. qname is contents between starting '<' and first whitespace character
   * or '/'. This covers cases:
   * <prefix:tag>  -> qname: prefix:tag
   * <tag>         -> qname: tag
   * <prefix:tag/> -> qname: prefix:tag
   * <tag/>        -> qname: tag
   * @param base start of the string view where the qname is located
   * @return cstr_t qname
   */
  cstr_t xml_segment::extract_qname_from_offset(std::string_view base) const
  {
    if (base.empty()) throw std::logic_error("empty base in extract_qname");
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const char* pos = base.data(); // we are starting with first child or next neighbour

    // Find fast back character '<' with limitation
    constexpr std::size_t max_scan = 512; // logically we dont need this limit, bu t in case
                                          // of invalid xml string this is safety measure
    std::size_t scanned = 0;

    while (*pos != '<' && scanned < max_scan)
    {
      --pos;
      ++scanned;
    }

    if (*pos != '<') throw std::logic_error("missing < in extract_qname_from_offset");

    const char* start = pos + 1;
    const char* end   = start;

    while (end < base.data() + base.size() && //
           *end != ' ' &&                     //
           *end != '\t' &&                    //
           *end != '\r' &&                    //
           *end != '\n' &&                    //
           *end != '>' &&                     //
           *end != '/')
      ++end;
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    cstr_t res{start, static_cast<std::size_t>(end - start)};
    return res;
  }
  std::size_t        xml_segment::id() const { return id_; }
  std::size_t        xml_segment::length() const { return length_; }
  [[nodiscard]] bool xml_segment::empty() const noexcept { return length_ == 0; }
  int                xml_segment::subtree_type() const { return subtree_type_; }
  /**
   * @brief construct full subtree in utf8 from components
   * This hack is neccessary since xerces startElement returns index of one char after the opening tag of
   * xml element (i.e. <prefix:tag>!, index of character !). To fully reconstruct the fragment of the xml
   * doc, we need to join together:
   * - opening tag < + qname + ns-definition + attribute definition + >
   * - fragment content (copied directly from the xml doc)
   * - closing tag: < + qname + />
   * @param tree_content whole xml document as string view buffer
   * @return std::string just segment of the tree
   */
  [[nodiscard]] std::string xml_segment::subtree_str(std::string_view tree_content) const
  {
    if (tree_content.empty()) return std::string{};
    auto  qname = extract_qname_from_offset(tree_content);
    x_str work_ns(ns_);
    x_str work_attrs(attrs_);
    // calculation of size to make only one reserve
    const std::size_t ns_len     = work_ns.to_string_view().size();
    const std::size_t attrs_len  = work_attrs.to_string_view().size();
    const std::size_t cont_len   = tree_content.size();
    const std::size_t total_len  = 2 + qname.size() +                        // <qname>
                                   (ns_len > 0 ? 1 + ns_len : 0) +           // (leading ws + ns len) or 0
                                   (attrs_len > 0 ? 1 + attrs_len : 0) + 1 + // (leading ws + attrs len or 0
                                   cont_len +                                // contents len
                                   3 + qname.size();                         // </qname>
    const std::size_t active_len = ns_len + attrs_len + cont_len;

    thread_local std::string result;
    if (total_len > result.capacity()) result.reserve(total_len * 2);
    result.clear(); // result is thread_local, we need to clean it before next iteration
    // --- top level opening tag ---
    result.push_back('<');
    result.append(qname);

    if (active_len == 0) [[unlikely]]
    { // primitive element no ns, no attributess, no subelement contents
      result.append("/>");
      return result; // <qname/>
    }
    if (ns_len > 0) // there are namespaces
    {
      result.push_back(' ');
      result.append(work_ns.to_string_view());
    }
    if (attrs_len > 0) [[unlikely]] // there are attribute values of top element
    {
      result.push_back(' ');
      result.append(work_attrs.to_string_view());
    }
    result.push_back('>');
    // --- xml element content ---
    result.append(tree_content);
    // --- xml element closing tag
    result.append("</");
    result.append(qname);
    result.push_back('>');
    return result;
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
                       subtree_str(view(mmap_base)));
  }
  std::size_t xml_segment::offset() const { return offset_; }
} // namespace fsp
