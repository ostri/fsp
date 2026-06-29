#pragma once
#include <iostream>
#include <meta>
#include <string_view>
// #include <array>
// #include <algorithm>
// #include <iostream>
#include "parsing_util.hpp"

#include <string>
#include <utility>
using str_t = std::string;
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

  class xpath
  {
  public:
    consteval xpath() = default;
    consteval explicit xpath(cstr_t path)
    : xpath(path, node_type::normal, false) { };
    consteval explicit xpath(cstr_t path, bool is_opt)
    : xpath(path, node_type::normal, is_opt) { };
    consteval xpath( //
      [[maybe_unused]] cstr_t    path,
      [[maybe_unused]] node_type type,
      [[maybe_unused]] bool      is_opt)
    : path_val(path.data()) { };
  public:                 //< members
    const char* path_val; // NOLINT(misc-non-private-member-variables-in-classes)
  };
  struct grammar
  {              // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
    str_t path_; //< name of the grammar
    grammar() = default;
    explicit grammar(str_t path)
    : path_(std::move(path))
    {
    }
  };

  namespace meta = std::meta;

  void indent(int n)
  {
    for (int i = 0; i != n; ++i) { std::cout << ' '; }
  }

  template <meta::info Item>
  static void print_xpath_annotations(int level)
  {
    (void)level;

    template for (constexpr meta::info Ann : std::define_static_array(meta::annotations_of(Item)))
    {
      indent(level);
      std::cout << meta::display_string_of(Ann) << "\n";
    }
  }

  template <meta::info Class>
  void print_class(int level)
  {
    indent(level);
    std::cout << "class " << meta::identifier_of(Class);

    // indent(level + 2);
    print_xpath_annotations<Class>(level + 4);

    template for (constexpr meta::info Field :
                  std::define_static_array(meta::nonstatic_data_members_of(Class, meta::access_context::unchecked())))
    {
      indent(level + 2);
      std::cout << meta::display_string_of(meta::type_of(Field)) << ' ' << meta::identifier_of(Field);

      // indent(level + 4);
      print_xpath_annotations<Field>(level + 6);
    }
  }

  template <meta::info Namespace>
  void print_namespace(int level = 0)
  {
    indent(level);
    std::cout << "namespace " << meta::display_string_of(Namespace) << '\n';
    print_xpath_annotations<Namespace>(level + 2);

    template for (constexpr meta::info Member : std::define_static_array(meta::members_of(Namespace, meta::access_context::unchecked())))
    {
      if constexpr (meta::is_type(Member)) { print_class<Member>(level + 2); }
      else
      {
        indent(level + 2);
        if constexpr (meta::has_identifier(Member)) { std::cout << meta::identifier_of(Member); }
        else
        {
          std::cout << meta::display_string_of(Member);
        }
        std::cout << '\n';
        print_xpath_annotations<Member>(level + 4);
      }
    }
  }
  template <meta::info Namespace>
  class reflex
  {
  public:
    consteval reflex() = default;
    // : ns_(meta::display_string_of(Namespace))
    // , ns_annotation_(load_annotations<Namespace>())
    // { load_annotations<Namespace>(); }
    template <meta::info Item>
    static consteval auto build_ns_annotations()
    {
      constexpr auto anns = std::meta::annotations_of(Namespace);

      std::array<std::string_view, std::meta::size_of(anns)> out{};
      template for (std::size_t i = 0; i < std::meta::size_of(anns); ++i) out[i] = std::meta::display_string_of(anns[i]);

      return out;
    }
    [[nodiscard]] consteval std::string_view ns() const { return ns_; }
  private:
    static constexpr auto ns_            = cstr_t(meta::display_string_of(Namespace));
    static constexpr auto ns_annotation_ = build_ns_annotations<Namespace>();
  };
}; // namespace fsp
