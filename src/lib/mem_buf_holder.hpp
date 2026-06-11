#pragma once

#include <cstddef>
#include <memory>
#include <spdlog/logger.h>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <string>
namespace fsp
{
  class mem_buf_holder
  {
  public:
    mem_buf_holder(const void* data, size_t size, std::string_view name, std::shared_ptr<spdlog::logger> logger);
    ~mem_buf_holder();
    /// no helper constructors
    mem_buf_holder(const mem_buf_holder&)            = delete;
    mem_buf_holder(mem_buf_holder&&)                 = delete;
    mem_buf_holder& operator=(const mem_buf_holder&) = delete;
    mem_buf_holder& operator=(mem_buf_holder&&)      = delete;
    ///
    [[nodiscard]] xercesc::MemBufInputSource* source();
    [[nodiscard]] spdlog::logger*             logger();

    void reset();

    [[nodiscard]] bool is_valid() const;
  private:
    std::unique_ptr<xercesc::MemBufInputSource> source_; // xerces file buffer
    std::shared_ptr<spdlog::logger>             logger_; // logger handler
    std::string                                 name_;   // name of the file
  };
} // namespace fsp