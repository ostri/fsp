#include "xerces_mgr.hpp"
#include "xsd_validator.hpp"
#include <fmt/base.h>
#include <memory>
#include <xercesc/framework/XMLGrammarPoolImpl.hpp>

#include <latch>
#include <atomic>
#include <thread>

namespace
{
  inline int help(const char** argv)
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
    fsp::gr_pool_t    gp(std::make_unique<xercesc::XMLGrammarPoolImpl>()); // std::make_shared<xercesc::XMLGrammarPoolImpl>();
    std::latch        gr_latch(1);                                         // just waiting for grammar to be loaded
    std::atomic<bool> gr_loaded{false};                                    // is grammar loaded?
    const fsp::str_t  xsd_file = argv[1];                                  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    fsp::vec_str_t    xml_files(argv + 2, argv + argc);                    // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)

    std::jthread loader(fsp::load_grammar::load, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), xsd_file);
    std::jthread validator(fsp::validate_xml, std::ref(gp), std::ref(gr_latch), std::ref(gr_loaded), std::cref(xml_files));
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
