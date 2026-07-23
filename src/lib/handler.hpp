#pragma once

#include <string>
#include <vector>
#include <string_view>
#include <future>
#include <optional>

#include <xercesc/dom/DOMLocator.hpp>
#include <xercesc/sax/Locator.hpp>
#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

#include "e_tag_wide.hpp"
#include "error_info.hpp"
#include "logger.hpp"
#include "parsing_util.hpp"
#include "lock_queue.hpp"
#include "x_str.hpp"
#include "xml_segment.hpp"
#include "segment_pool.hpp"

namespace fsp
{
  using cstr_t        = std::string_view;
  using str_t         = std::string;
  using cstr_XMLCh_t  = std::basic_string_view<XMLCh>;
  using ns_def_t      = std::vector<std::pair<x_str, x_str>>;
  using RuleMask      = uint64_t;
  using segment_queue = lock_queue<xml_segment>;


  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(proc_data&                    targets,
            const fsp_logger&             log,
            const xercesc::SAX2XMLReader* parser,
            std::string_view              base_addr,
            segment_pool&                 pool);
    // --- SAX2 ContentHandler ---
    void startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) override;
    void startElement(const XMLCh* uri, const XMLCh* localname, const XMLCh* qname, const xercesc::Attributes& attrs) override;
    void endElement( //
      [[maybe_unused]] const XMLCh* uri,
      [[maybe_unused]] const XMLCh* localname,
      [[maybe_unused]] const XMLCh* qname) override;
    // void characters([[maybe_unused]] const XMLCh* chars, [[maybe_unused]] XMLSize_t length) override { };
    //  --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t segments_found() const noexcept;
    [[nodiscard]] cstr_t      doc() const;
    // // [DODANO] Injicira shared_future iz xml_processor::process_from_buffer.
    // // Handler ga polling preverja v startElement() in ob napaki vrže
    // // SAXParseException, ki jo Xerces uporabi kot signal za prekinitev parsinga.
    // // shared_future (ne future) ker get() ne sme biti destructive — handler ga
    // // lahko preveri večkrat (polling), xml_processor pa pokliče get() na koncu.
    // void set_validation_future(std::shared_future<std::optional<error_info>> f);
  private: // methods
    [[noreturn]] void logic_error(const char* msg) const;
    // --- helper methods ---------
    // void check_validation_status();
    void check_xpath_matches(const XMLCh* uri,
                             const XMLCh* localname,
                             //                             const XMLCh*               qname, // it is not used to make it faster
                             const xercesc::Attributes& attrs);
    // --- NS context stack ---
    // Vsak nivo je map prefix→uri za en XML element scope.
    // open_ns_scope() potisne nov nivo, close_ns_scope() ga odstrani.
    void               open_ns_scope();
    void               close_ns_scope();
    void               push_ns_mapping(const XMLCh* prefix, const XMLCh* uri);
    [[nodiscard]] bool is_capturing() const { return frag_depth_ != -1; }
    // Translate prefix to uri. Empty string if prefix is not defined
    //    [[nodiscard]] x_str resolve_ns(const x_str& prefix) const noexcept;
    // Razreši NS URI za e_tag (enkrat, ko je NS context zgrajen).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;
    // str_XMLCh_t        make_open_tag_new(const XMLCh* qname, const xercesc::Attributes& attrs);
    std::string make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs);
    str_XMLCh_t attr_values_str(const xercesc::Attributes& attrs);
    /// prepare message to report exception
    std::string prepare_msg(const xercesc::SAXParseException& e);
    void        rebuild_ns_decl_for_current_level();
  private: /// members
    // --- subtree xpaths ---
    proc_data                 targets_;      // xpath rules
    std::vector<xpath_wide_t> targets_wide_; // xpath rules as XMLCh* strings

    std::vector<RuleMask>    active_mask_stack_; //< stack of active rules
    std::vector<std::size_t> rule_lengths_;      //< length of the xpaths /rules

    // --- NS definicitons ---
    // stack of NS definitions. Whenever new set of ns definitions occur new push on stack
    // occurs. on endElement of the same tag this is removed.
    struct ns_level
    {
      int      depth;      //< depth in the tree where these ns are valid
      ns_def_t ns_vec;     //< list of ns that are defined at this level
                           //      str_t    ns_decl_string; //< namespaces as string
      str_XMLCh_t ns_decl; //< namespaces as XMLCh
    };
    std::vector<ns_level> ns_stack_; // stack of namespaces associated with xml tags
                                     // It is pushed on startElement tag where ns are provided
                                     // It is popped on endElement tag

    ns_def_t ns_pending_; // the ns_pending_structure is a temporary buffer that transfers information between
                          // methods startPrefixMapping and startElement. It is cleared after startElement
    int doc_depth_ = 0;   // depth in the document (1 = root elem.)
    // --- Fragment akumulacija ---
    int         frag_depth_        = -1; // depth inside the fragment
    int         seg_type_          = -1; // type/structure of the segment. document is split into segments.
    std::size_t frag_start_offset_ = 0;  // byte offset of start of the fragment

    // --- Output ---
    // queue of segments that is filled by handler and emptied by workers
    // segment_queue& queue_;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::size_t counter_ = 0; // counter to obtain unique segment id within the file

    const xercesc::SAX2XMLReader* parser_; // reference to parser; for getSrcOffs
    const fsp_logger&             log_;    // logger NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    cstr_t                        doc_;    // xml document mapped as string view over mmap file
    str_XMLCh_t                   ns_;     // current and inherited namespaces as string (for current segmetn)
    str_XMLCh_t                   attr_;   // current tag attributes as a string (for current segment)

    // [DODANO] Shared future na katerega validacijska nit postavi napako (ali nullopt).
    // Handler ga polling preverja v startElement() brez blokiranja.
    // Inicializiran kot neveljaven (valid() == false) — brez validacije se ne
    // preveri nikoli in ne povzroča overhead-a.
    using valid_future = std::shared_future<std::optional<error_info>>; // validation future
    valid_future  val_future_;
    std::size_t   element_counter_ = 0; // pooling counter check also "every"
    str_XMLCh_t   buf_;                 // space for "make_open_tag" as XMLCh
    segment_pool& pool_;                // segment pool
    const bool    log_trace_       = false;
    const bool    log_debug_       = false;
    const bool    log_info_        = false;
    const bool    log_warn_        = false;
    const bool    log_err_         = false;
    const bool    log_crit_        = false;
    int           max_xpath_depth_ = 0;
  };

  inline std::size_t Handler::segments_found() const noexcept { return counter_; }
  inline cstr_t      Handler::doc() const { return doc_; }
  // inline void        Handler::set_validation_future(std::shared_future<std::optional<error_info>> f) { val_future_ = std::move(f); }

} // namespace fsp
