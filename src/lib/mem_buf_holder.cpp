#include "mem_buf_holder.hpp"
// #include "x_str.hpp" // Dodaj za pretvorbo XMLCh v string

namespace fsp
{

  mem_buf_holder::mem_buf_holder(const void* data, size_t size, std::string_view name, std::shared_ptr<spdlog::logger> logger)
  : logger_(std::move(logger))
  , name_(name)
  {
    if ((data != nullptr) && (size > 0))
    {
      source_ = std::make_unique<xercesc::MemBufInputSource>( //
        reinterpret_cast<const XMLByte*>(data),               // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        static_cast<XMLSize_t>(size),
        name_.data(),
        false);

      if (logger_) { logger_->debug("Created buffer holder for '{}' ({} bytes)", name_, size); }
    }
    else
    {
      if (logger_)
      {
        logger_->warn("Created empty buffer holder for '{}' (data: {}, size: {})", //
                      name_,
                      data != nullptr ? "valid" : "null",
                      size);
      }
    }
  }

  mem_buf_holder::~mem_buf_holder()
  {
    if (logger_)
    {
      if (source_) { logger_->debug("Destroying '{}' buffer holder)", name_.data()); }
      else
      {
        logger_->debug("Destroying empty buffer holder '{}'", name_);
      }
    }
  }

  [[nodiscard]] xercesc::MemBufInputSource* mem_buf_holder::source() { return source_.get(); }

  void mem_buf_holder::reset()
  {
    if (logger_) { logger_->debug("Resetting buffer holder '{}'", name_); }
    source_.reset();
  }

  bool mem_buf_holder::is_valid() const { return source_ != nullptr; }

}; // namespace fsp