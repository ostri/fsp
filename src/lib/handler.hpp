#pragma once

#include <map>
#include <spdlog/logger.h>
#include <string>
#include <vector>

#include <xercesc/dom/DOMLocator.hpp>
#include <xercesc/sax/Locator.hpp>
#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

// #include "common.hpp"
#include "e_tag.hpp"
#include "queue.hpp"
#include "x_str.hpp"
// #include "x_str.hpp"

namespace fsp
{
  using cstr_t       = std::string_view;
  using cstr_XMLCh_t = std::basic_string_view<XMLCh>;
  class e_tag_wide
  {
  public:
    e_tag_wide() = default;
    e_tag_wide(cstr_t ns, cstr_t tag)
    : ns_(ns)
    , tag_(tag)
    {
    }
    e_tag_wide(cstr_XMLCh_t ns, cstr_XMLCh_t tag)
    : ns_(ns)
    , tag_(tag)
    {
    }
    [[nodiscard]] x_str ns() const { return ns_; }
    [[nodiscard]] x_str tag() const { return tag_; }
    void                set_tag(const x_str& tag) { tag_.assign(tag.data()); };
    void                set_tag(const XMLCh* tag) { tag_.assign(tag); }
    void                set_ns(const x_str& ns) { ns_.assign(ns.data()); }
    void                set_ns(const XMLCh* ns) { ns_.assign(ns); }
  private:
    x_str ns_;  // namespace: prefix or uri
    x_str tag_; // tagname
  };
  using xpath_wide_t = std::vector<e_tag_wide>;

  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(const std::vector<xpath_t>&     targets,
            segment_queue&                  queue,
            std::shared_ptr<spdlog::logger> logger,
            const xercesc::SAX2XMLReader*   parser,
            std::string_view                base_addr);

    // --- SAX2 ContentHandler ---
    void startPrefixMapping( //
      const XMLCh* prefix,
      const XMLCh* uri) override;
    void endPrefixMapping(const XMLCh* prefix) override;
    void startElement(const XMLCh*                  uri,
                      [[maybe_unused]] const XMLCh* localname,
                      [[maybe_unused]] const XMLCh* qname,
                      const xercesc::Attributes&    attrs) override;
    void endElement( //
      [[maybe_unused]] const XMLCh* uri,
      [[maybe_unused]] const XMLCh* localname,
      [[maybe_unused]] const XMLCh* qname) override;
    //    void characters([[maybe_unused]] const XMLCh* chars, [[maybe_unused]] XMLSize_t length) override;
    // --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t      segments_found() const noexcept { return counter_; }
    [[nodiscard]] std::string_view base_addr() const;
  private:
    // --- NS context stack ---
    // Vsak nivo je map prefix→uri za en XML element scope.
    // open_ns_scope() potisne nov nivo, close_ns_scope() ga odstrani.
    void open_ns_scope();
    void close_ns_scope();
    void push_ns_mapping(const std::string& prefix, const std::string& uri);

    // Razreši prefix v URI. Prazen string če prefix ni znan.
    [[nodiscard]] std::string resolve_ns(const std::string& prefix) const noexcept;

    // Vrne snapshot vseh aktivnih NS preslikav (za vbrizganje v fragment).
    [[nodiscard]] std::map<std::string, std::string> active_ns() const;

    // Razreši NS URI za e_tag (enkrat, ko je NS context zgrajen).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;

    std::string make_open_tag(const std::string& qname, const xercesc::Attributes& attrs);

    // // --- Xerces string helpers ---
    // static std::string to_str(const XMLCh* xstr);
    // static std::string to_str(const XMLCh* xstr, XMLSize_t len);
    /// prepare message to report exception
    std::string prepare_msg(const xercesc::SAXParseException& e);

    // --- subtree xpaths ---
    std::vector<xpath_t>      targets_;      // xpath rules
    std::vector<xpath_wide_t> targets_wide_; // xpath rules as XMLCh* strings

    // --- NS stanje ---
    // Stack nivojev: vsak nivo je map prefix→uri
    std::vector<std::map<std::string, std::string>> ns_stack_;
    // Pending preslikave za naslednji open_ns_scope()
    std::map<std::string, std::string> ns_pending_;

    // --- Matching stanje ---
    // matched_[i] = koliko korakov xpath[i] je že ujeto
    std::vector<int> matched_;
    int              doc_depth_ = 0; // globina v dokumentu (1 = koreni elem.)

    // --- Fragment akumulacija ---
    bool capturing_  = false;
    int  frag_depth_ = 0;  // globina znotraj fragmenta
    int  active_idx_ = -1; // kateri xpath je aktiven
    // std::string fragment_;              // akumuliran XML fragment
    std::size_t frag_start_offset_ = 0; // byte offset začetka fragmenta

    // --- Output ---
    segment_queue& queue_;
    std::size_t    counter_ = 0;

    const xercesc::SAX2XMLReader*   parser_;    // reference to parser; for getSrcOffs
    std::shared_ptr<spdlog::logger> logger_;    /// logger
    std::string_view                base_addr_; // address of the start of the document
    std::string                     prefix_;    // opening tag with inherited ns, ns and attributes
  };

} // namespace fsp
