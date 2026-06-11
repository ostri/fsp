#include "x_str.hpp"
#include <stdexcept>
#include <utility>
#include <memory>
namespace fsp
{
  fsp::x_str::x_str(XMLCh* ptr) noexcept
  : data_(xercesc::XMLString::replicate(ptr))
  {
  }

  x_str::~x_str() { reset(); }

  x_str::x_str(std::string_view utf8)
  {
    if (! utf8.empty())
    {
      reset(xercesc::XMLString::transcode(utf8.data())); // NOLINT(bugprone-suspicious-stringview-data-usage)
      if (data_ == nullptr) throw std::runtime_error("XMLString::transcode failed");
    }
  }

  x_str::x_str(std::u16string_view u16)
  {
    if (! u16.empty())
    {
      // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      reset(xercesc::XMLString::replicate(reinterpret_cast<const XMLCh*>(u16.data())));
      if (data_ == nullptr) throw std::runtime_error("XMLString::replicate failed");
    }
  }

  x_str::x_str(const x_str& other)
  {
    if (other.data_ != nullptr) reset(xercesc::XMLString::replicate(other.data_));
  }

  x_str::x_str(x_str&& other) noexcept
  : data_(std::exchange(other.data_, nullptr))
  {
  }

  x_str& x_str::operator=(const x_str& other)
  {
    if (this != &other)
    {
      if (other.data_ != nullptr) reset(xercesc::XMLString::replicate(other.data_));
    }
    return *this;
  }

  x_str& x_str::operator=(x_str&& other) noexcept
  {
    if (this != &other) { reset(std::exchange(other.data_, nullptr)); }
    return *this;
  }

  void x_str::assign(const XMLCh* other) { reset(xercesc::XMLString::replicate(other)); }
  void x_str::assign(const cstr_t other)
  {
    x_str tmp(other);
    *this = tmp;
  }

  void x_str::reset() noexcept
  {
    if (data_ != nullptr)
    {
      xercesc::XMLString::release(&data_);
      data_ = nullptr; // must be eventhough the xerces documentation claims that it clears data_
    }
  }

  void x_str::reset(XMLCh* ptr) noexcept
  {
    if (data_ != nullptr) xercesc::XMLString::release(&data_);
    data_ = ptr;
  }

  [[nodiscard]] std::string x_str::to_string() const
  {
    if (empty()) return {};
    char* utf8 = xercesc::XMLString::transcode(data_);
    if (utf8 == nullptr) throw std::runtime_error("XMLString::transcode failed");

    // RAII for Xerces char*
    auto deleter = [](char* p)
    {
      if (p) xercesc::XMLString::release(&p);
    };
    std::unique_ptr<char, decltype(deleter)> guard(utf8, deleter);
    return {utf8};
  }

  [[nodiscard]] std::u16string x_str::to_u16string() const
  {
    if (empty()) return {};
    return {reinterpret_cast<const char16_t*>(data_)}; // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  }

  bool x_str::operator==(const x_str& other) const noexcept { return xercesc::XMLString::equals(data_, other.data_); }
  bool x_str::operator!=(const x_str& other) const noexcept { return ! (*this == other); }
  auto x_str::operator<=>(const x_str& other) const noexcept { return xercesc::XMLString::compareString(data_, other.data_) <=> 0; }
  bool x_str::operator==(const XMLCh* other) const noexcept { return xercesc::XMLString::equals(data_, other); }
  bool x_str::operator!=(const XMLCh* other) const noexcept { return ! (*this == other); }
  auto x_str::operator<=>(const XMLCh* other) const noexcept { return xercesc::XMLString::compareString(data_, other) <=> 0; }
  bool x_str::operator==(std::string_view utf8) const
  {
    x_str temp(utf8);
    return *this == temp;
  }

  bool x_str::operator!=(std::string_view utf8) const { return ! (*this == utf8); }
  bool x_str::operator==(std::u16string_view u16) const
  {
    x_str temp(u16);
    return *this == temp;
  }

  bool                       x_str::operator!=(std::u16string_view u16) const { return ! (*this == u16); }
  x_str::                    operator const XMLCh*() const noexcept { return data_; }
  [[nodiscard]] const XMLCh* x_str::c_str() const noexcept { return data_; }
  [[nodiscard]] XMLCh*       x_str::data() const noexcept { return data_; }
  [[nodiscard]] bool         x_str::empty() const noexcept { return data_ == nullptr || *data_ == 0; }
  [[nodiscard]] std::size_t  x_str::length() const noexcept { return data_ != nullptr ? xercesc::XMLString::stringLen(data_) : 0; }
} // namespace fsp
