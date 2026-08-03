#pragma once

#include <exception>
namespace fsp
{
  // --- compile time exception ----------------------------------------------------------
  class compile_error : public std::exception
  {
  public:
    // compile_error
    constexpr explicit compile_error(const char* msg) noexcept
    : message(msg)
    {
    }
    [[nodiscard]] constexpr const char* what() const noexcept override { return message; }
  private:
    const char* message;
  };

} // namespace fsp