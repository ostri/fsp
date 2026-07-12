#include "load_grammar.hpp"
#include "x_str.hpp"

#include <fmt/base.h>
#include <xercesc/framework/LocalFileInputSource.hpp>
#include <xercesc/framework/XMLGrammarPool.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/util/PlatformUtils.hpp>

namespace fsp
{
  sax_reader_t prepare_grammar_parser(const auto& gr_pool)
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
  void load_grammar([[maybe_unused]] const std::stop_token& st,
                    const gr_pool_t&                        gr_pool,
                    std::latch&                             gr_latch,
                    std::atomic<bool>&                      gr_loaded,
                    const str_t&                            xsd_file)
  {
    auto start = std::chrono::high_resolution_clock::now();
    try
    {
      x_str                         xsd_path(xsd_file);
      xercesc::LocalFileInputSource inputSource(xsd_path.to_u16string().data());

      auto  reader  = prepare_grammar_parser(gr_pool);
      auto* grammar = reader->loadGrammar(inputSource, xercesc::Grammar::SchemaGrammarType, true);
      if (grammar != nullptr)
      {
        gr_pool->cacheGrammar(grammar);
        gr_loaded                                         = true;
        auto                                      end     = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        fmt::print("Grammar {} successfully loaded.({:.2f}ms)\n", xsd_file, elapsed.count());
      }
      else fmt::print("Grammar {} loading failed. (loadGrammar returned nullptr).\n", xsd_file);
    }
    catch (const xercesc::XMLException& e)
    {
      fmt::print("Grammar loading xerces exception: '{}'.", xercesc::XMLString::transcode(e.getMessage()));
    }
    catch (const std::exception& e)
    {
      fmt::print("Grammar loading standard exception: '{}'\n'", e.what());
    }
    catch (...)
    {
      fmt::print("Grammar loading unknown exception.\n");
    }
    gr_latch.count_down(); // Decrease latch count to unblock the validator thread
  }
} // namespace fsp
