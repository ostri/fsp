#pragma once

#include <latch>
#include <memory>
#include <stop_token>
#include <string_view>
#include <xercesc/sax2/SAX2XMLReader.hpp>
namespace fsp
{
  using sax_reader_t = std::unique_ptr<xercesc::SAX2XMLReader>;
  using gr_pool_t    = std::unique_ptr<xercesc::XMLGrammarPool>;
  using str_t        = std::string;
  using cstr_t       = std::string_view;

  class load_grammar
  {
  public:
    static void load([[maybe_unused]] const std::stop_token& st,
                     const gr_pool_t&                        gr_pool,
                     std::latch&                             gr_latch,
                     std::atomic<bool>&                      gr_loaded,
                     cstr_t                                  xsd_file);

    // New buffer-based load
    static void load_mem([[maybe_unused]] const std::stop_token& st,
                         const gr_pool_t&                        gr_pool,
                         std::latch&                             gr_latch,
                         std::atomic<bool>&                      gr_loaded,
                         cstr_t                                  buf,
                         cstr_t                                  buffer_id = "mem_buffer_xsd");
  private:
    static sax_reader_t prepare_grammar_parser(const auto& gr_pool);
  };
} // namespace fsp