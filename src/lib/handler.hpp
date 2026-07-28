#pragma once

#include <string>
#include <vector>
#include <string_view>

#include <xercesc/sax2/Attributes.hpp>
#include <xercesc/sax2/DefaultHandler.hpp>
#include <xercesc/sax2/SAX2XMLReader.hpp>

#include "doc_set_dscr.hpp"
#include "e_tag_wide.hpp"
#include "logger.hpp"
#include "parsing_util.hpp"
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

  class Handler : public xercesc::DefaultHandler
  {
  public:
    Handler(const proc_data&              targets, //
            const fsp_logger&             log,
            const xercesc::SAX2XMLReader* parser,
            segment_pool&                 pool,
            const doc_set_dscr&           ds_dscr);
    // --- SAX2 ContentHandler ---
    void startPrefixMapping(const XMLCh* prefix, const XMLCh* uri) override;
    void startElement(const XMLCh* uri, const XMLCh* localname, const XMLCh* qname, const xercesc::Attributes& attrs) override;
    void endElement( //
      [[maybe_unused]] const XMLCh* uri,
      [[maybe_unused]] const XMLCh* localname,
      [[maybe_unused]] const XMLCh* qname) override;
    //  --- SAX2 ErrorHandler ---
    void warning(const xercesc::SAXParseException& e) override;
    void error(const xercesc::SAXParseException& e) override;
    void fatalError(const xercesc::SAXParseException& e) override;

    [[nodiscard]] std::size_t segments_found() const noexcept;
    void                      set_doc(cstr_t doc);
    [[nodiscard]] cstr_t      doc() const;
    [[nodiscard]] int         doc_ndx() const;
    void                      set_doc_ndx(int doc_ndx);
    // When true, error()/fatalError() throw the SAXParseException right back out of parse()
    // instead of only logging -- used when this Handler's parser was set up with schema
    // validation enabled (see doc_cutter::setup_parser_with_validation()), so an invalid
    // document short-circuits cutting immediately instead of paying for the rest of it.
    void set_validating(bool validating) { validating_ = validating; }
  private: // methods
    [[noreturn]] void logic_error(const char* msg) const;
    // --- helper methods ---------
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
    // Razreši NS URI za e_tag (enkrat, ko je NS context zgrajen).
    [[nodiscard]] bool tag_matches(const e_tag_wide& tag, const XMLCh* local_name, const XMLCh* ns_uri) const noexcept;
    str_t              make_open_tag(const XMLCh* qname, const xercesc::Attributes& attrs);
    str_XMLCh_t        attr_values_str(const xercesc::Attributes& attrs);
    /// prepare message to report exception
    str_t       prepare_msg(const xercesc::SAXParseException& e);
    void        rebuild_ns_decl_for_current_level();
  private:                  /// members
    const fsp_logger& log_; // must be first logger NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
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
    // --- segment acumutate sdata ---
    int                           frag_depth_        = -1; //< depth inside the fragment
    int                           seg_type_          = -1; //< type/structure of the segment. document is split into segments.
    std::size_t                   frag_start_offset_ = 0;  //< byte offset of start of the fragment
    std::size_t                   counter_           = 0;  //< counter to obtain unique segment id within the file
    const xercesc::SAX2XMLReader* parser_;                 //< pointer to related parser; only for getSrcOffs, not owner
    cstr_t                        doc_;                    //< xml document mapped as string view over mmap file
    const doc_set_dscr&           ds_dscr_;                //< structure of all documents to be processed
    int                           doc_ndx_ = -1;           //< index of the document within the ds_dscr global structure
    str_XMLCh_t                   ns_;                     //< current and inherited namespaces as string (for current segment)
    str_XMLCh_t                   attr_;                   //< current tag attributes as a string (for current segment)
    std::size_t                   element_counter_ = 0;    //< pooling counter check also "every"
    str_XMLCh_t                   buf_;                    //< space for "make_open_tag" as XMLCh
    segment_pool&                 pool_;                   //< segment pool
    bool                          validating_      = false; //< see set_validating()
    const bool                    log_trace_       = log_.active(lvl_enum::trace);
    const bool                    log_debug_       = log_.active(lvl_enum::debug);
    const bool                    log_info_        = log_.active(lvl_enum::info);
    const bool                    log_warn_        = log_.active(lvl_enum::warn);
    const bool                    log_err_         = log_.active(lvl_enum::err);
    const bool                    log_crit_        = log_.active(lvl_enum::crit);
    int                           max_xpath_depth_ = 0; //< max depth of all xpaths
  };
  /////////////////////////////////////////////////////////////////////////////////////////////////
  inline std::size_t Handler::segments_found() const noexcept { return counter_; }
  inline void        Handler::set_doc(cstr_t doc) { doc_ = doc; }
  inline cstr_t      Handler::doc() const { return doc_; }
  inline int         Handler::doc_ndx() const { return doc_ndx_; }
  inline void        Handler::set_doc_ndx(int doc_ndx) { doc_ndx_ = doc_ndx; }
} // namespace fsp
