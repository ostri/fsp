#pragma once
#include <iostream>
#include <meta>
#include <string_view>
#include <array>
#include <string>
#include <utility>
// parsing_util.hpp je IZKLJUČEN – povzroča napako pri prevajanju in
// ni potreben za delovanje refleksije.

using str_t  = std::string;
using cstr_t = std::string_view;

namespace fsp
{
  using str_t  = std::string;
  using cstr_t = std::string_view;

  enum class node_type : uint8_t
  {
    normal,
    attr,
    array
  };

  struct ns
  {
    const char* prefix;
    const char* uri;
  };
  class xpath
  {
  public:
    consteval xpath() = default;
    consteval explicit xpath(cstr_t path)
    : xpath(path, node_type::normal, false) { };
    consteval explicit xpath(cstr_t path, bool is_opt)
    : xpath(path, node_type::normal, is_opt) { };
    consteval xpath([[maybe_unused]] cstr_t path, [[maybe_unused]] node_type type, [[maybe_unused]] bool is_opt)
    : path_val(path.data()) { };
  public:
    const char* path_val; // NOLINT(misc-non-private-member-variables-in-classes)
  };

  struct grammar
  {
    str_t path_;
    grammar() = default;
    explicit grammar(str_t path)
    : path_(std::move(path))
    {
    }
  };

  //  namespace meta = std::meta;

  void indent(int n)
  {
    for (int i = 0; i != n; ++i) { std::cout << ' '; }
  }

  template <std::meta::info Item>
  static void print_xpath_annotations(int level)
  {
    (void)level;
    // POPRAVEK: "template for" → navadni "for" (range-based)
    for (constexpr std::meta::info Ann : std::define_static_array(std::meta::annotations_of(Item)))
    {
      indent(level);
      std::cout << std::meta::display_string_of(Ann) << "\n";
    }
  }

  template <std::meta::info Class>
  void print_class(int level)
  {
    indent(level);
    std::cout << "class " << std::meta::identifier_of(Class);
    print_xpath_annotations<Class>(level + 4);

    // POPRAVEK: "template for" → navadni "for" (range-based)
    for (constexpr std::meta::info Field :
         std::define_static_array(std::meta::nonstatic_data_members_of(Class, std::meta::access_context::unchecked())))
    {
      indent(level + 2);
      std::cout << std::meta::display_string_of(std::meta::type_of(Field)) << ' ' << std::meta::identifier_of(Field);
      print_xpath_annotations<Field>(level + 6);
    }
  }

  template <std::meta::info Namespace>
  void print_namespace(int level = 0)
  {
    indent(level);
    std::cout << "namespace " << std::meta::display_string_of(Namespace) << '\n';
    print_xpath_annotations<Namespace>(level + 2);

    // POPRAVEK: "template for" → navadni "for" (range-based)
    for (constexpr std::meta::info Member :
         std::define_static_array(std::meta::members_of(Namespace, std::meta::access_context::unchecked())))
    {
      if constexpr (std::meta::is_type(Member)) { print_class<Member>(level + 2); }
      else
      {
        indent(level + 2);
        if constexpr (std::meta::has_identifier(Member)) { std::cout << std::meta::identifier_of(Member); }
        else
        {
          std::cout << std::meta::display_string_of(Member);
        }
        std::cout << '\n';
        print_xpath_annotations<Member>(level + 4);
      }
    }
  }

  template <std::meta::info Namespace>
  class reflex
  {
  public:
    consteval reflex() = default;

    template <std::meta::info Item>
    static consteval auto build_ns_annotations()
    {
      constexpr auto anns = std::meta::annotations_of(Namespace);
      // POPRAVEK 1: anns.size() namesto std::std::meta::size_of(anns)
      //   size_of(info) pričakuje info, ne vector<info>
      std::array<cstr_t, anns.size()> out{};
      // POPRAVEK 2: navadna for zanka z indeksom namesto "template for" s C-style obliko
      //   "template for" podpira SAMO range-based obliko, ne C-style (init; cond; incr)
      for (std::size_t i = 0; i < anns.size(); ++i) out[i] = std::meta::display_string_of(anns[i]);
      return out;
    }

    [[nodiscard]] consteval cstr_t ns() const { return ns_; }
  private:
    static constexpr auto ns_            = cstr_t(std::meta::display_string_of(Namespace));
    static constexpr auto ns_annotation_ = build_ns_annotations<Namespace>();
  };

}; // namespace fsp
