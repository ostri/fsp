#pragma once

#include <string>

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/sax/SAXParseException.hpp>
#include <xercesc/sax/ErrorHandler.hpp>

namespace fsp
{
  using str_t = std::string;
  // Error handler for validation with UTF-8 support
  class EH : public xercesc::ErrorHandler
  {
  public:
    explicit EH(bool quiet);

    void                             warning(const xercesc::SAXParseException& exc) override;
    void                             error(const xercesc::SAXParseException& exc) override;
    void                             fatalError(const xercesc::SAXParseException& exc) override;
    void                             resetErrors() override;
    [[nodiscard]] bool               hasValidationErrors() const;
  private:
    bool        hasErrors{};
    bool        quietMode;
    str_t       lastErrorLocation;
    str_t       lastErrorMessage;
    void        handleError(const char* type, const xercesc::SAXParseException& exc);
  };
  bool validateXML(const str_t& xmlFile, const str_t& xsdFile, bool quietMode);
  void printUsage(const char* programName);
}; // namespace fsp

// int main(int argc, char* argv[]);
