#pragma once
#include <cstddef>
#include <fmt/format.h>
#include <string_view>
// #include <span>
// #include <string>

namespace fsp
{
  class xml_segment
  {
  public:
    xml_segment() = default;
    xml_segment(std::size_t id, int xpath_index, std::size_t offset, std::size_t length, std::string_view prefix);
    [[nodiscard]] std::string_view view(const std::byte* mmap_base = nullptr) const noexcept;

    [[nodiscard]] bool        empty() const noexcept;
    [[nodiscard]] std::string dump(const std::byte* mmap_base = nullptr) const;
    [[nodiscard]] std::size_t get_id() const;
    [[nodiscard]] int         get_xpath_index() const;
    [[nodiscard]] std::size_t get_offset() const;
    [[nodiscard]] std::size_t get_length() const;
    [[nodiscard]] std::string prefix() const;
    [[nodiscard]] std::string subtree_str(std::string_view base) const
    {
      std::string str;
      str.reserve(prefix().size() + get_length());
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      return {prefix() + base};
    }
  private:
    std::size_t id_          = 0;  // zaporedna številka segmenta
    int         xpath_index_ = -1; // indeks ujemajočega xpath pravila (0..n)
    std::size_t offset_      = 0;  // byte odmik v mmap bufferu
    std::size_t length_      = 0;  // dolžina fragmenta v bytih
    std::string prefix_;           // opening tag with inherited namespaces, tag namespaces and tag attributes
                                   //    std::string postfix_;          // just closing tag
  };

} // namespace fsp
