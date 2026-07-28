#include "work.hpp"
#include <array>
#include <iostream>
#include <meta>
#include <string_view>
#include <type_traits>

// ---- Podatkovna struktura, v katero shranimo rezultat xpath anotacije ----

struct class_info
{
  fsp::raw_attr                 target;
  std::array<fsp::raw_attr, 32> fields{};
  std::size_t                   field_count = 0;
};

// Zgradi seznam razredov v podanem imenskem prostoru, ki dedujejo od
// fsp::seg_schema, za vsak razred pa shrani njegov target-raw_attr in
// raw_attr tabelo polj -- isti podatki, ki jih fsp::proc_data_of() uporabi
// za zgraditev fsp::proc_data (glej reflection.hpp), tu samo za izpis.
template <std::meta::info Ns>
consteval auto get_classes()
{
  constexpr auto        ctx     = std::meta::access_context::unchecked();
  static constexpr auto members = std::define_static_array(std::meta::members_of(Ns, ctx));

  std::array<class_info, 32> result{};
  std::size_t                count = 0;

  template for (constexpr auto m : members)
  {
    if constexpr (std::meta::is_type(m) && std::meta::has_identifier(m))
    {
      // gnezden if constexpr: typename[:m:] se preveri za CEL if-constexpr
      // pogoj tudi, ko is_type(m) ni izpolnjen, zato mora biti ločen (glej
      // enako opombo pri fsp::classes_of() v reflection.hpp).
      if constexpr (std::is_base_of_v<fsp::seg_schema, typename[:m:]>)
      {
        class_info ci{};
        ci.target = fsp::attr_of<m>();

        constexpr auto raw_fields = fsp::fields_of<m>();
        for (std::size_t i = 0; i < raw_fields.size(); ++i) ci.fields[ci.field_count++] = raw_fields[i];

        result[count++] = ci;
      }
    }
  }

  return result;
}

namespace
{
  // "1" / "0..1" / "1..*" / "0..*" -- glede na is_opt()/is_array(), ki ju
  // fsp::xml_attr izpelje iz istega raw_attr, ki ga uporablja prava cevovodna
  // koda (glej fsp::field_attr_of() v reflection.hpp za pomen ?, *, + markerjev).
  std::string_view cardinality(const fsp::xml_attr& a)
  {
    if (a.is_array()) return a.is_opt() ? "0..*" : "1..*";
    return a.is_opt() ? "0..1" : "1";
  }

  void print_row(std::string_view indent, const fsp::raw_attr& raw)
  {
    fsp::xml_attr a(0, raw, fsp::work::ns);
    std::cout << indent << raw.name << "  ->  " << a.full_xpath_with_uri() << "  [" << (a.is_attr() ? "attr" : "tag") << ", "
              << cardinality(a) << "]\n";
  }
} // namespace

int main()
{
  constexpr auto classes = get_classes<^^fsp::work>();

  std::cout << "Razredi z xpath anotacijo:\n";
  for (const auto& ci : classes)
  {
    if (ci.target.name.empty()) break;

    print_row("", ci.target);
    for (std::size_t i = 0; i < ci.field_count; ++i) print_row("    ", ci.fields[i]);
  }
}
