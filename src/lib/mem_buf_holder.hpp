#pragma once

#include "logger.hpp"
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
    mem_buf_holder(const void* data, size_t size, std::string_view name, const fsp_logger& logger);
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
    const fsp_logger&                           logger_;    // logger handler
    std::unique_ptr<xercesc::MemBufInputSource> source_;    // xerces file buffer
    std::string                                 name_;      // name of the file
    bool                                        log_debug_; // shall we log debug messages
  };
} // namespace fsp