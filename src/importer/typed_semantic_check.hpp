// typed_semantic_check.hpp
#pragma once
#include "pipeline_hooks.hpp"
#include "reflection.hpp"
#include "segment_result.hpp"
#include "xml_segment.hpp"
#include <meta>
#include <variant>

// Kept separate from reflection.hpp (rather than folded into it) so a translation unit that only
// needs materialize()/materialize_variant()/proc_data_of() -- e.g. t_refl.cpp -- doesn't have to
// pull in pipeline_hooks.hpp's own logger::Logger dependency just to include reflection.hpp.
// Only a caller that actually wants typed_semantic_check (see its own class comment) includes
// this file.
namespace fsp
{
  /**
   * @brief Pack-deduction helper for typed_semantic_check's own static_assert -- true only if
   * Derived declares an on_type() overload for EVERY one of Ts (see the fold expression), false
   * (and thus a static_assert failure at the call site) if even one is missing or misspelled.
   * Kept a plain concept-like variable template (not folded inline into typed_semantic_check
   * itself) so the fold expression's operands -- one requires-expression per Ts -- stay readable.
   */
  template <typename Derived, typename... Ts>
  constexpr bool has_on_type_for_every_v = (... && requires(Derived& d, Ts& s, segment_result& r, bool b) { d.on_type(s, r, b, b); });

  /** @brief Pack-deduction helper: instantiates has_on_type_for_every_v<Derived, Ts...> from a variant_of_t<Namespace, Base> pointer. */
  template <typename Derived, typename... Ts>
  consteval bool check_on_type_coverage(std::variant<Ts...>* /*unused*/)
  { return has_on_type_for_every_v<Derived, Ts...>; }

  /**
   * @brief CRTP mixin that turns per-segment-type dispatch (materialize_variant() + std::visit()
   * + one if-constexpr branch per schema class) into a piece of plumbing a caller never has to
   * write: derive from typed_semantic_check<YourClass, ^^YourNamespace> instead of
   * pipeline_hooks_crtp<YourClass> directly, and declare one on_type() overload per schema class
   * you care about --
   *
   *   bool on_type(const YourNamespace::some_schema_class& s, segment_result& result,
   *                bool is_first, bool is_last);
   *
   * -- with the exact same parameter list pipeline_hooks::on_semantic_safe_check() itself takes,
   * minus the raw xml_segment (materialize_variant() already replaces it with s) and with the
   * segment's own materialized type as the first parameter instead -- and, same as every other
   * "_safe" hook (see pipeline_hooks.hpp's own class comment), no logger::Logger parameter: call
   * the protected log() accessor instead. on_semantic_safe_check() itself is implemented here,
   * once, and marked final -- a derived class overriding it instead of declaring on_type()
   * overloads would silently lose this whole mechanism, so that path is closed off entirely
   * rather than left as a footgun.
   *
   * @details on_semantic_safe_check()'s own static_assert requires Derived to declare an
   * on_type() overload for EVERY schema class Namespace declares, not just the ones a caller
   * happens to remember -- deliberately a compile-time error over a silent runtime fallback
   * (e.g. a warning logged the first time an unhandled type is seen): a missing or misspelled
   * overload is a caller mistake that pipeline_hooks::on_semantic_safe_check()'s own default
   * body would otherwise paper over by returning result.values().complete() unconditionally,
   * exactly like every OTHER schema class silently would if this mixin instead defaulted to "ok"
   * for a type with no overload. If a schema class genuinely has no business rule of its own,
   * its on_type() overload still needs to exist -- just with a body that returns true
   * unconditionally -- so that decision is visible in the derived class's own source instead of
   * being inferred from an absence. The check itself lives in on_semantic_safe_check(), not
   * typed_semantic_check's own class body: a class template's member functions are only
   * instantiated on first use, well after Derived (e.g. pacs8_cb) is a complete type, whereas
   * the base class itself is instantiated while Derived is still being defined (CRTP's usual
   * "incomplete type" trap) -- a static_assert placed directly in the class body would see NO
   * on_type() overloads yet, no matter how many Derived actually declares, and always fail.
   * @tparam Derived   the developer's own concrete hook class (Curiously Recurring Template
   * Pattern, same as pipeline_hooks_crtp) -- must declare one on_type() overload per Ts below.
   * @tparam Namespace reflection of the namespace holding the schema classes (see
   * materialize_variant()'s own tparam of the same name)
   * @tparam Base      marker base class identifying segment-schema classes
   */
  template <typename Derived, std::meta::info Namespace, typename Base = seg_schema>
  class typed_semantic_check : public pipeline_hooks_crtp<Derived>
  {
  protected:
    bool on_semantic_safe_check([[maybe_unused]] const xml_segment& segment, segment_result& result, bool is_first, bool is_last) final
    {
      // See the class's own doc comment for why this check lives here (first use, Derived
      // already complete) rather than directly in typed_semantic_check's own class body.
      static_assert(check_on_type_coverage<Derived>(static_cast<variant_of_t<Namespace, Base>*>(nullptr)),
                    "typed_semantic_check<Derived, Namespace>: Derived is missing an on_type() overload for at least one of "
                    "Namespace's schema classes (or one overload's parameter list doesn't match exactly) -- add "
                    "bool on_type(const YourSchemaClass&, segment_result&, bool, bool) for each one.");
      auto seg = materialize_variant<Namespace, Base>(result.seg_type(), result);
      return std::visit([&]<typename T>(T& s) -> bool { return static_cast<Derived&>(*this).on_type(s, result, is_first, is_last); }, seg);
    }
  };
} // namespace fsp
