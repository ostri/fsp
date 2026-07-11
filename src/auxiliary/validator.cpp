#include "x_str.hpp"
#include "xerces_mgr.hpp"
#include <fmt/base.h>
#include <memory>
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>
#include <xercesc/framework/LocalFileInputSource.hpp>
#include <xercesc/validators/common/Grammar.hpp>
#include <xercesc/sax/ErrorHandler.hpp>
#include <xercesc/sax/SAXParseException.hpp>

#include <latch>
#include <atomic>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

// using namespace xercesc;
namespace
{
  using gr_pool_t    = std::unique_ptr<xercesc::XMLGrammarPoolImpl>;
  using sax_reader_t = std::unique_ptr<xercesc::SAX2XMLReader>;
  using str_t        = std::string;
  using vec_str_t    = std::vector<str_t>;
  class valid_handler : public xercesc::DefaultHandler
  {
  public:
    void                reset();
    void                error(const xercesc::SAXParseException& exc) override;
    void                fatalError(const xercesc::SAXParseException& exc) override;
    void                warning(const xercesc::SAXParseException& exc) override;
    [[nodiscard]] bool  is_error() const;
    [[nodiscard]] str_t file() const;
    void                set_file(const str_t& file);
  private:
    bool  had_error_ = false; // do we have some error?
    str_t file_;              // file we are parsing
  };

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
  static void load_grammar([[maybe_unused]] const std::stop_token& st,
                           const gr_pool_t&                        gr_pool,
                           std::latch&                             gr_latch,
                           std::atomic<bool>&                      gr_loaded,
                           const str_t&                            xsd_file)
  {
    auto start = std::chrono::high_resolution_clock::now();
    try
    {
      fsp::x_str                    xsd_path(xsd_file);
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
  /**
   * @brief create sax reader for validator with assigned grammar pool
   *
   * @param gr_pool grammar pool
   * @return sax reader
   */
  static sax_reader_t prepare_parser(const gr_pool_t& gr_pool)
  {
    // Create the parser ONCE outside the loop
    std::unique_ptr<xercesc::SAX2XMLReader> reader(
      xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
    // NOLINTBEGIN(hicpp-no-array-decay)
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
    reader->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
    // NOLINTEND(hicpp-no-array-decay)
    return reader;
  }
  // Static function for validating XML files
  static void validate_xml([[maybe_unused]] const std::stop_token& st,
                           const gr_pool_t&                        gr_pool,
                           std::latch&                             gr_latch,
                           std::atomic<bool>&                      gr_loaded,
                           const vec_str_t&                        xml_files)
  {
    gr_latch.wait();
    if (! gr_loaded) throw std::runtime_error("Grammar is not loaded. Validation aborted.");
    auto          reader = prepare_parser(gr_pool);
    valid_handler eh;
    reader->setErrorHandler(&eh);
    for (const auto& xml_file : xml_files)
    {
      auto start = std::chrono::high_resolution_clock::now();
      try
      {
        eh.reset(); // Reset error state for the new file
        eh.set_file(xml_file);
        XMLCh*                        xmlPath = xercesc::XMLString::transcode(xml_file.c_str());
        xercesc::LocalFileInputSource xmlSource(xmlPath);
        xercesc::XMLString::release(&xmlPath);
        reader->parse(xmlSource);
        std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - start;
        if (! eh.is_error()) fmt::print("File '{}' is valid. ({:.2f}ms)\n", xml_file, elapsed.count());
        else fmt::print("File '{}' is invalid. ({:.2f}ms)\n", xml_file, elapsed.count());
      }
      catch (const xercesc::XMLException& e)
      {
        fmt::print("Validation xerces exception '{}': {}\n", xml_file, xercesc::XMLString::transcode(e.getMessage()));
      }
      catch (const std::exception& e)
      {
        fmt::print("Validation standard exception '{}': {}\n", xml_file, e.what());
      }
      catch (...)
      {
        fmt::print("Validation unknown exception '{}'.\n", xml_file);
      }
    }
  }
  inline void valid_handler::reset()
  {
    had_error_ = false;
    file_      = "unknown";
  }
  inline void valid_handler::error(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    fmt::print("Validation error '{}' (row: {}, col: {}):\n  {}\n",
               file_,
               e.getLineNumber(),
               e.getColumnNumber(),
               fsp::x_str(e.getMessage()).to_string());
  }
  inline void valid_handler::fatalError(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    fmt::print(
      "Validation fatal error (row: {}, col: {}): {}\n", e.getLineNumber(), e.getColumnNumber(), fsp::x_str(e.getMessage()).to_string());
  }
  inline str_t valid_handler::file() const { return file_; }
  inline void  valid_handler::set_file(const str_t& file) { file_ = file; }
  inline void  valid_handler::warning(const xercesc::SAXParseException& e)
  {
    fmt::print(
      "Validation warning (row: '{}', col: {}): {}\n", e.getLineNumber(), e.getColumnNumber(), fsp::x_str(e.getMessage()).to_string());
  }
  inline bool valid_handler::is_error() const { return had_error_; }
  inline int  help(const char** argv)
  {
    fmt::print("Usage: <grammar.xsd> <xml1> [xml2 ...]\n", *argv);
    return 1;
  }
} // namespace
int main(int argc, const char* argv[])
{
  if (argc < 3) return help(argv);
  try
  {
    fsp::xerces_mgr   x;                                                   // xercess environment
    auto              gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
    std::latch        gr_latch(1);                                         // just waiting for grammar to be loaded
    std::atomic<bool> gr_loaded{false};                                    // is grammar loaded?
    str_t             xsd_file = argv[1];                                  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    vec_str_t         xml_files(argv + 2, argv + argc);                    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    std::jthread loader(load_grammar, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), xsd_file);
    std::jthread validator(validate_xml, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), std::cref(xml_files));
    // waiting for threads to finish;
    loader.join();
    validator.join();
    gp.reset();
    return 0;
  }
  catch (const std::runtime_error& e)
  {
    fmt::print("runtime error: {}", e.what());
    return 2;
  }
  catch (...)
  {
    fmt::print("Unhandled program exception");
    return 1;
  };
}