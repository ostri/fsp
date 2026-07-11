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

// using namespace xercesc;
namespace
{
  using gr_pool_t = std::unique_ptr<xercesc::XMLGrammarPoolImpl>;
  using str_t     = std::string;
  class validation_err_handler : public xercesc::DefaultHandler
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

  // Static function for loading the XSD schema
  static void load_grammar([[maybe_unused]] const std::stop_token& st,
                           const gr_pool_t&                        gr_pool,
                           std::latch&                             gr_latch,
                           std::atomic<bool>&                      gr_loaded,
                           const str_t&                            xsd_file)
  {
    try
    {
      XMLCh*                        xsd_path = xercesc::XMLString::transcode(xsd_file.c_str());
      xercesc::LocalFileInputSource inputSource(xsd_path);
      xercesc::XMLString::release(&xsd_path);

      // Using the previously fixed reader creation pattern
      std::unique_ptr<xercesc::SAX2XMLReader> reader(
        xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
      // NOLINTBEGIN(hicpp-no-array-decay)
      reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
      reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
      reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
      reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
      // NOLINTEND(hicpp-no-array-decay)
      auto* grammar = reader->loadGrammar(inputSource, xercesc::Grammar::SchemaGrammarType, true);
      if (grammar != nullptr)
      {
        gr_pool->cacheGrammar(grammar);
        gr_loaded = true;
        fmt::print("Grammar {} successfully loaded.\n", xsd_file);
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
  // Static function for validating XML files
  static void validate_xml([[maybe_unused]] const std::stop_token& st,
                           const gr_pool_t&                        gr_pool,
                           std::latch&                             gr_latch,
                           std::atomic<bool>&                      gr_loaded,
                           const std::vector<str_t>&               xml_files)
  {
    gr_latch.wait();
    if (! gr_loaded)
    {
      fmt::print("Grammar is not loaded. Validation aborted.\n");
      return;
    }
    // Create the parser ONCE outside the loop
    std::unique_ptr<xercesc::SAX2XMLReader> reader(
      xercesc::XMLReaderFactory::createXMLReader(xercesc::XMLPlatformUtils::fgMemoryManager, gr_pool.get()));
    // Set permanent features
    // NOLINTBEGIN(hicpp-no-array-decay)
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation, true);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchema, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking, false);
    reader->setFeature(xercesc::XMLUni::fgXercesUseCachedGrammarInParse, true);
    // NOLINTEND(hicpp-no-array-decay)
    validation_err_handler eh;
    reader->setErrorHandler(&eh);
    for (const auto& xml_file : xml_files)
    {
      try
      {
        eh.reset(); // Reset error state for the new file
        eh.set_file(xml_file);
        XMLCh*                        xmlPath = xercesc::XMLString::transcode(xml_file.c_str());
        xercesc::LocalFileInputSource xmlSource(xmlPath);
        xercesc::XMLString::release(&xmlPath);
        reader->parse(xmlSource);
        if (! eh.is_error()) fmt::print("File '{}' is valid.\n", xml_file);
        else fmt::print("File '{}' is invalid.\n", xml_file);
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
  inline void validation_err_handler::reset()
  {
    had_error_ = false;
    file_      = "unknown";
  }
  inline void validation_err_handler::error(const xercesc::SAXParseException& e)
  {
    had_error_ = true;
    fmt::print("Validation error '{}' (row: {}, col: {}):\n  {}\n",
               file_,
               e.getLineNumber(),
               e.getColumnNumber(),
               xercesc::XMLString::transcode(e.getMessage()));
  }
  inline void validation_err_handler::fatalError(const xercesc::SAXParseException& exc)
  {
    had_error_ = true;
    fmt::print("Validation fatal error (row: {}, col: {}): {}\n",
               exc.getLineNumber(),
               exc.getColumnNumber(),
               xercesc::XMLString::transcode(exc.getMessage()));
  }
  inline str_t validation_err_handler::file() const { return file_; }
  inline void  validation_err_handler::set_file(const str_t& file) { file_ = file; }
  inline void  validation_err_handler::warning(const xercesc::SAXParseException& exc)
  {
    fmt::print("Validation warning (row: '{}', col: {}): {}\n",
               exc.getLineNumber(),
               exc.getColumnNumber(),
               xercesc::XMLString::transcode(exc.getMessage()));
  }
  inline bool validation_err_handler::is_error() const { return had_error_; }
} // namespace
int main(int argc, char* argv[])
{
  if (argc < 3)
  {
    fmt::print("Usage: <grammar.xsd> <xml1> [xml2 ...]\n", *argv);
    return 1;
  }

  try
  {
    xercesc::XMLPlatformUtils::Initialize();
  }
  catch (const xercesc::XMLException& e)
  {
    fmt::print("Xerces initialization error: {}\n", xercesc::XMLString::transcode(e.getMessage()));
    return 1;
  }
  // // shared pointer since there
  auto               gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
  std::latch         grammarLatch(1);
  std::atomic<bool>  grammarLoaded{false};
  str_t              xsdFile = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::vector<str_t> xmlFiles;
  for (int i = 2; i < argc; ++i) xmlFiles.emplace_back(argv[i]); // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  std::jthread loader(load_grammar, std::ref(gp), std::ref(grammarLatch), std::ref(grammarLoaded), xsdFile);
  std::jthread validator(validate_xml, std::ref(gp), std::ref(grammarLatch), std::ref(grammarLoaded), std::cref(xmlFiles));
  // Počakamo, da se obe niti zaključita
  loader.join();
  validator.join();
  gp.reset();
  xercesc::XMLPlatformUtils::Terminate();
  return 0;
}