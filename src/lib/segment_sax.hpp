#include <libxml/parser.h>
#include <cstddef>
#include <string_view>
#include <vector>
#include <string>
#include <span>
#include "xml_attr.hpp"

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
    const std::vector<xml_attr>&          targets; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::vector<xml_path_el>              path_stack;
    std::vector<std::vector<std::string>> results;
    std::vector<bool>                     found;                     // Sledi, katere atribute smo že našli
    size_t                                found_count       = 0;     // števec najdenih (za ne-array xpathe)
    bool                                  is_array_present  = false; // true, če obstaja vsaj en array target
    bool                                  stop_parsing      = false;
    int                                   active_target_idx = -1;
    xmlParserCtxtPtr                      ctxt              = nullptr; // to stop the parser
    std::string                           current_buffer;              // for temporary tag values
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    explicit sax_ctx(const std::vector<xml_attr>& t)
    : targets(t)
    {
      results.resize(targets.size()); // fill with dummy values
      found.assign(targets.size(), false);
      for (const auto& el : targets)
        if (el.is_array())
        {
          is_array_present = true;
          break;
        }
      static const int buf_size = 1024;
      current_buffer.reserve(buf_size);
    }
  };

  class segment_sax
  {
  public:
    using result_t = std::vector<std::vector<std::string>>;

    result_t exec(std::string_view xml_data, const std::vector<xml_attr>& targets)
    {
      sax_ctx       ctx(targets);
      xmlSAXHandler handler{};
      handler.startElementNs = &on_start;
      handler.endElementNs   = &on_end;
      handler.characters     = &on_chars;

      // 1. Ustvari kontekst
      xmlParserCtxtPtr ctxt = xmlNewSAXParserCtxt(&handler, &ctx);
      if (nullptr == ctxt) return {};
      ctx.ctxt = ctxt; // to be able to exit prematurely

      // 2. Nastavi opcije (npr. nodict za varnost, če želiš, sicer pusti prazno)
      // NOLINTNEXTLINE(hicpp-signed-bitwise)
      xmlCtxtUseOptions(ctxt, XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);

      // 3. Parsiraj
      // xmlParseChunk(ctxt, data, size, termination_flag)
      // termination_flag = 1 pomeni, da je to zadnji (in edini) del podatkov
      int result = xmlParseChunk(ctxt, xml_data.data(), static_cast<int>(xml_data.size()), 1);

      if (result != 0)
      {
        // Obravnava napake pri parsiranju
      }
      // 4. Počisti kontekst
      xmlFreeParserCtxt(ctxt);

      return std::move(ctx.results);
    }
  private:
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
      if (ctx->stop_parsing) return;

      ctx->path_stack.push_back(
        {.uri = nullptr != URI ? reinterpret_cast<const char*>(URI) : "", .tag = reinterpret_cast<const char*>(localname)});

      // 1. Preverjanje atributov (attributes array vsebuje: localname, prefix, uri, value_ptr, value_end)
      for (int i = 0; i < nb_attributes; ++i)
      {
        const xmlChar* attr_localname = attributes[i * 5];
        const xmlChar* attr_uri       = attributes[i * 5 + 2];
        const xmlChar* val_ptr        = attributes[i * 5 + 3];
        const xmlChar* val_end        = attributes[i * 5 + 4];

        for (int t = 0; t < static_cast<int>(ctx->targets.size()); ++t)
        {
          if (! ctx->targets[t].is_attr()) continue;

          // PREVERJANJE:
          // A) Ali smo na pravem elementu? (is_path_match)
          // B) Ali se ime atributa in URI ujemata z iskanim?
          if (is_path_match(ctx->path_stack, ctx->targets[t]) &&
              ctx->targets[t].attr_name() == reinterpret_cast<const char*>(attr_localname) &&
              ctx->targets[t].attr_uri() == (nullptr != attr_uri ? reinterpret_cast<const char*>(attr_uri) : ""))
          {
            ctx->results[t].emplace_back(reinterpret_cast<const char*>(val_ptr), val_end - val_ptr);
            ctx->found[t] = true;
          }
        }
      }

      // 2. Preverjanje elementov
      for (int i = 0; i < static_cast<int>(ctx->targets.size()); ++i)
      {
        if (! ctx->targets[i].is_attr() && is_path_match(ctx->path_stack, ctx->targets[i])) { ctx->active_target_idx = i; }
      }

      // 3. Optimizacija: ali lahko prekinemo?
      // check_stop_condition(ctx);
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

        // Sedaj šele premaknemo akumulirano vsebino v rezultate
        ctx->results[idx].push_back(std::move(ctx->current_buffer));

        // Sedaj je logično povečati števec samo enkrat
        ctx->found[idx] = true;
        ctx->found_count++;
        ctx->active_target_idx = -1;
      }
      // 2. Odstranimo nivo iz sklada
      ctx->path_stack.pop_back();

      // 3. Po vsakem zaprtem elementu preverimo, ali lahko prekinemo parsiranje
      check_stop_condition(ctx);
    }
    static void check_stop_condition(sax_ctx* ctx)
    {
      if (! ctx->is_array_present && ctx->found_count == ctx->targets.size())
      {
        ctx->stop_parsing = true;
        if (ctx->ctxt != nullptr)
        {
          xmlStopParser(ctx->ctxt); // TO JE UKAZ ZA PREKINITEV
        }
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
  };
} // namespace fsp