#pragma once

#include <xercesc/util/XMLString.hpp>
#include <compare>
#include <string>
#include <string_view>
#include <functional>

namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;
  class x_str
  {
  public:
    x_str() noexcept = default;
    ~x_str();
    explicit x_str(XMLCh* ptr) noexcept;
    explicit x_str(cstr_t utf8);
    explicit x_str(std::u16string_view u16);
    x_str(const x_str& other);
    x_str(x_str&& other) noexcept;
    /// operators
    x_str&                         operator=(const x_str& other);
    x_str&                         operator=(x_str&& other) noexcept;
    void                           assign(const XMLCh* other);
    void                           assign(cstr_t other);
    void                           reset() noexcept;
    void                           reset(XMLCh* ptr) noexcept;
    void                           reset(XMLCh* ptr, XMLSize_t size) noexcept;
    [[nodiscard]] str_t            to_string() const;
    [[nodiscard]] std::u16string   to_u16string() const;
    [[nodiscard]] cstr_t           to_string_view() const;
    bool                           operator==(const x_str& other) const noexcept;
    bool                           operator!=(const x_str& other) const noexcept;
    std::strong_ordering           operator<=>(const x_str& other) const noexcept;
    bool                           operator==(const XMLCh* other) const noexcept;
    bool                           operator!=(const XMLCh* other) const noexcept;
    std::strong_ordering           operator<=>(const XMLCh* other) const noexcept;
    bool                           operator==(cstr_t utf8) const;
    bool                           operator!=(cstr_t utf8) const;
    bool                           operator==(std::u16string_view u16) const;
    bool                           operator!=(std::u16string_view u16) const;
    explicit                       operator const XMLCh*() const noexcept;
    [[nodiscard]] const XMLCh*     c_str() const noexcept;
    [[nodiscard]] XMLCh*           data() const noexcept;
    [[nodiscard]] bool             empty() const noexcept;
    [[nodiscard]] std::size_t      length() const noexcept;
  private:
    XMLCh*              data_ = nullptr;
    XMLSize_t           size_ = 0;
    mutable str_t       cached_utf8_; //< utf8 equivalent of data_
  };
} // namespace fsp

// Hash specializacija
template <>
struct std::hash<fsp::x_str>
{
  std::size_t operator()(const fsp::x_str& s) const { return std::hash<fsp::str_t>{}(s.to_string()); }
};