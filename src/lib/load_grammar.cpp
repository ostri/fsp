#include "load_grammar.hpp"
#include "mmap_file.hpp"
#include "x_str.hpp"

#include <fmt/base.h>
#include <xercesc/util/XercesDefs.hpp>
#include <xercesc/framework/LocalFileInputSource.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/framework/XMLGrammarPool.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/util/PlatformUtils.hpp>

namespace fsp
{
  sax_reader_t load_grammar::prepare_grammar_parser(const auto& gr_pool)
  {
    // Using the previously fixed reader creation pattern
    sax_reader_t reader(xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
    // NOLINTBEGIN(hicpp-no-array-decay)
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
    // NOLINTEND(hicpp-no-array-decay)
    return reader;
  }
  // Static function for loading the XSD schema
  void load_grammar::load([[maybe_unused]] const std::stop_token& st,
                          const gr_pool_t&                        gr_pool,
                          std::latch&                             gr_latch,
                          std::atomic<bool>&                      gr_loaded,
                          const str_t&                            xsd_file)
  {
    auto start = std::chrono::high_resolution_clock::now();
    try
    {
      mmap_file mm(xsd_file);
      load_mem(st, gr_pool, gr_latch, gr_loaded, mm.string_view(), xsd_file);
      //   x_str                         xsd_path(xsd_file);
      //   xercesc::LocalFileInputSource inputSource(xsd_path.to_u16string().data());

      //   auto  reader  = prepare_grammar_parser(gr_pool);
      //   auto* grammar = reader->loadGrammar(inputSource, xercesc::Grammar::SchemaGrammarType, true);
      //   if (grammar != nullptr)
      //   {
      //     gr_pool->cacheGrammar(grammar);
      //     gr_loaded                                         = true;
      auto                                      end     = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double, std::milli> elapsed = end - start;
      fmt::print("Grammar {} successfully loaded.({:.2f}ms)\n", xsd_file, elapsed.count());
      //   }
      //   else fmt::print("Grammar {} loading failed. (loadGrammar returned nullptr).\n", xsd_file);
    }
    // catch (const xercesc::XMLException& e)
    // {
    //   fmt::print("Grammar loading xerces exception: '{}'.", xercesc::XMLString::transcode(e.getMessage()));
    // }
    // catch (const std::exception& e)
    // {
    //   fmt::print("Grammar loading standard exception: '{}'\n'", e.what());
    // }
    catch (...)
    {
      fmt::print("Grammar loading unknown exception.\n");
    }
    // gr_latch.count_down(); // Decrease latch count to unblock the validator thread
  }

  // New implementation for string_view buffer
  void load_grammar::load_mem([[maybe_unused]] const std::stop_token& st,
                              const gr_pool_t&                        gr_pool,
                              std::latch&                             gr_latch,
                              std::atomic<bool>&                      gr_loaded,
                              std::string_view                        buf,
                              const str_t&                            buffer_id)
  {
    auto start = std::chrono::high_resolution_clock::now();
    try
    {
      const auto* xml_data = reinterpret_cast<const XMLByte*>(buf.data());
      x_str       buf_id_str(buffer_id);
      // Create input source from memory buffer (false = do not adopt/take ownership of buffer)
      xercesc::MemBufInputSource inputSource(xml_data, buf.size(), buf_id_str.to_u16string().data(), false);
      auto                       reader  = load_grammar::prepare_grammar_parser(gr_pool);
      auto*                      grammar = reader->loadGrammar(inputSource, xercesc::Grammar::SchemaGrammarType, true);

      if (grammar != nullptr)
      {
        gr_pool->cacheGrammar(grammar);
        gr_loaded                                         = true;
        auto                                      end     = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        fmt::print("Grammar from buffer '{}' successfully loaded. ({:.2f}ms)\n", buffer_id, elapsed.count());
      }
      else fmt::print("Grammar from buffer '{}' loading failed. (loadGrammar returned nullptr).\n", buffer_id);
    }
    catch (const xercesc::XMLException& e)
    {
      fmt::print("Grammar loading xerces exception: '{}'.\n", xercesc::XMLString::transcode(e.getMessage()));
    }
    catch (const std::exception& e)
    {
      fmt::print("Grammar loading standard exception: '{}'\n", e.what());
    }
    catch (...)
    {
      fmt::print("Grammar loading unknown exception.\n");
    }

    gr_latch.count_down(); // Decrease latch count to unblock the validator thread
  }
} // namespace fsp
