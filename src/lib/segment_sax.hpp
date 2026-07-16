#include "xml_attr.hpp"
#include "xpath_set.hpp"
#include <libxml/parser.h>
#include <cstddef>
#include <string_view>
#include <vector>
#include <string>
#include <span>
#include <bit>
#include <cstdint>

namespace fsp
{

  struct xml_path_el
  { // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string_view uri;
    std::string_view tag;
    bool             operator==(const xml_path_el& other) const { return tag == other.tag && uri == other.uri; }
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  struct sax_ctx
  {                                                // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    const xpath_set&                      targets; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::vector<xml_path_el>              path_stack;
    std::vector<std::vector<std::string>> results;
    std::uint64_t                         remaining_mask    = 0;
    bool                                  stop_parsing      = false;
    int                                   active_target_idx = -1;
    xmlParserCtxtPtr                      ctxt              = nullptr; // to stop the parser
    std::string                           current_buffer;              // for temporary tag values
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    explicit sax_ctx(const xpath_set& t)
    : targets(t)
    , remaining_mask(targets.full_mask())
    {
      results.resize(targets.size());
      path_stack.reserve(targets.max_xpath_size());
      static const int buf_size = 1024;
      current_buffer.reserve(buf_size);
    }
    void reset_for_reuse()
    {
      for (auto& r : results) r.clear();
      remaining_mask    = targets.full_mask();
      stop_parsing      = false;
      active_target_idx = -1;
      path_stack.clear();
      current_buffer.clear();
    }
  };

  class segment_sax
  {
  public:
    using result_t = std::vector<std::vector<std::string>>;
    explicit segment_sax(const xpath_set& targets)
    : targets_(targets)
    , ctx_(targets_)
    {
      handler_.initialized    = XML_SAX2_MAGIC;
      handler_.startElementNs = &on_start;
      handler_.endElementNs   = &on_end;
      handler_.characters     = &on_chars;

      ctxt_ = xmlCreatePushParserCtxt(&handler_, &ctx_, nullptr, 0, nullptr);
      // NOLINTNEXTLINE(hicpp-signed-bitwise)
      xmlCtxtUseOptions(ctxt_, XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
      ctx_.ctxt = ctxt_;
    }

    ~segment_sax()
    {
      if (ctxt_ != nullptr) xmlFreeParserCtxt(ctxt_);
    }
    segment_sax(segment_sax&&)            = delete;
    segment_sax& operator=(segment_sax&&) = delete;

    segment_sax(const segment_sax&)            = delete;
    segment_sax& operator=(const segment_sax&) = delete;

    result_t exec(std::string_view xml_data)
    {
      ctx_.reset_for_reuse();
      // xmlCtxtResetPush ohrani že alocirane interne strukture (dict, ns tabele ...)
      // in samo "resetira" stanje parserja, ne da bi ga zgradil na novo.
      xmlCtxtResetPush(ctxt_, xml_data.data(), static_cast<int>(xml_data.size()), nullptr, nullptr);
      xmlCtxtUseOptions(ctxt_, XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET); // NOLINT(hicpp-signed-bitwise)
      ctx_.ctxt = ctxt_; // xmlCtxtResetPush lahko premakne notranje kazalce, ponovno poveži
      xmlParseChunk(ctxt_, xml_data.data(), static_cast<int>(xml_data.size()), 1);
      return ctx_.results; // brez move — caller mora podatke porabiti/kopirati pred naslednjim exec(
    }
  private:
    // Pomožna funkcija za iteracijo čez nastavljene bite
    template <typename F>
    static void for_each_set_bit(std::uint64_t bits, F&& func)
    {
      while (bits != 0)
      {
        const int t = std::countr_zero(bits);
        std::forward<F>(func)(t);
        bits &= (bits - 1); // Pobriši najnižji nastavljen bit
      }
    }
    static void process_attributes(sax_ctx* ctx, int nb_attributes, const xmlChar** attributes, std::uint64_t attr_bits)
    {
      // Uporaba vaše predlagane for_each_set_bit za čistočo
      for (int i = 0; i < nb_attributes; ++i)
      { // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers)
        const xmlChar* attr_localname = attributes[static_cast<ptrdiff_t>(i * 5)];
        const xmlChar* attr_uri       = attributes[i * 5 + 2];
        const xmlChar* val_ptr        = attributes[i * 5 + 3];
        const xmlChar* val_end        = attributes[i * 5 + 4];
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers)

        for_each_set_bit(attr_bits & ctx->remaining_mask,
                         [&](int t)
                         {
                           const auto& target = ctx->targets[static_cast<std::size_t>(t)];
                           if (is_path_match(ctx->path_stack, target) &&
                               target.attr_name() == reinterpret_cast<const char*>(attr_localname) &&
                               target.attr_uri() == (nullptr != attr_uri ? reinterpret_cast<const char*>(attr_uri) : ""))
                           {
                             ctx->results[t].emplace_back(reinterpret_cast<const char*>(val_ptr), val_end - val_ptr);
                             if ((ctx->targets.array_mask() & (std::uint64_t{1} << static_cast<std::size_t>(t))) == 0)
                               ctx->remaining_mask &= ~(std::uint64_t{1} << static_cast<std::size_t>(t));
                           }
                         });
      }
    }

    static void process_elements(sax_ctx* ctx, std::uint64_t elem_bits)
    {
      for_each_set_bit(elem_bits & ctx->remaining_mask,
                       [&](int t)
                       {
                         if (is_path_match(ctx->path_stack, ctx->targets[static_cast<std::size_t>(t)]))
                         {
                           ctx->active_target_idx = t;
                           // V originalu je bil "break" - tukaj lahko vrnete ali uporabite bool za nadzor
                         }
                       });
    }
    static void on_start(void*                            user_data,
                         const xmlChar*                   localname,
                         [[maybe_unused]] const xmlChar*  prefix,
                         const xmlChar*                   URI,
                         [[maybe_unused]] int             nb_namespaces,
                         [[maybe_unused]] const xmlChar** namespaces,
                         [[maybe_unused]] int             nb_attributes,
                         [[maybe_unused]] int             nb_defaulted,
                         [[maybe_unused]] const xmlChar** attributes)
    {
      auto* ctx = static_cast<sax_ctx*>(user_data);
      if (ctx->stop_parsing) [[unlikely]]
        return;

      ctx->path_stack.push_back(
        {.uri = URI != nullptr ? reinterpret_cast<const char*>(URI) : "", .tag = reinterpret_cast<const char*>(localname)});

      const auto depth = ctx->path_stack.size();
      if (depth > ctx->targets.max_xpath_size()) return;

      // Logika atributov
      process_attributes(ctx, nb_attributes, attributes, ctx->targets.attr_mask(depth));

      // Logika elementov
      process_elements(ctx, ctx->targets.elem_mask(depth));
    }
    static void on_chars(void* user_data, const xmlChar* ch, int len)
    {
      auto* ctx = static_cast<sax_ctx*>(user_data);
      if (ctx->active_target_idx != -1)
      {
        // Akumuliraj vsebino, dokler element traja
        ctx->current_buffer.append(reinterpret_cast<const char*>(ch), len);
      }
    }

    static void on_end(void* user_data, //
                       const xmlChar* /*localname*/,
                       const xmlChar* /*prefix*/,
                       const xmlChar* /*URI*/)
    {
      auto* ctx = static_cast<sax_ctx*>(user_data);
      if (ctx->active_target_idx != -1)
      {
        auto idx = ctx->active_target_idx;
        ctx->results[idx].push_back(std::move(ctx->current_buffer));
        ctx->current_buffer.clear();
        if ((ctx->targets.array_mask() & (std::uint64_t{1} << static_cast<unsigned int>(idx))) == 0U)
          ctx->remaining_mask &= ~(std::uint64_t{1} << static_cast<unsigned int>(idx));
        ctx->active_target_idx = -1;
      }
      ctx->path_stack.pop_back();
      check_stop_condition(ctx);
    }
    static void check_stop_condition(sax_ctx* ctx)
    {
      // ustavi se, ko so vsi non-array biti počiščeni;
      // če obstaja kak array target, njegov bit ostane nastavljen do konca -> se ne ustavi prezgodaj
      if (ctx->remaining_mask == 0)
      {
        ctx->stop_parsing = true;
        if (ctx->ctxt != nullptr) xmlStopParser(ctx->ctxt);
      }
    }
    static bool is_path_match(const std::vector<xml_path_el>& stack, const xml_attr& attr)
    {
      auto target_span = attr.xpath();
      if (stack.size() != target_span.size()) return false;

      // Primerjaj od zadaj naprej (običajno se tu zgodi neskladje najhitreje)
      for (size_t i = stack.size(); i > 0; --i)
      {
        const auto& s = stack[i - 1];
        const auto& t = target_span[i - 1];
        if (s.tag != t.tag || s.uri != t.ns) return false;
      }
      return true;
    }
  private:
    const xpath_set& targets_;
    xmlSAXHandler    handler_{};
    xmlParserCtxtPtr ctxt_ = nullptr;
    sax_ctx          ctx_;
  };
} // namespace fsp