#pragma once

#include <array>
#include <chrono>
#include <fmt/format.h>
#include <string_view>
#include <vector>
#include <string>

namespace fsp
{
  struct ns
  {
    std::string_view prefix;
    std::string_view uri;
  };
  struct xpath_el
  {
    std::string_view ns;
    std::string_view tag;
  };
  constexpr size_t max_xpath_depth = 4;
  struct attr_attr
  {
    std::string_view                      name;
    std::string_view                      path;
    bool                                  is_attr  = false;
    bool                                  is_array = false;
    bool                                  is_opt   = false;
    std::array<xpath_el, max_xpath_depth> xpath{};
    size_t                                xpath_size = 0;
  };


  using xpath  = std::vector<xpath_el>;
  using date_t = std::chrono::year_month_day;

  struct accnt
  {
    std::string iban_;
    std::string bic_;
  };
  struct ct_txn
  {
    int         txn_id_;
    accnt       debtor_;
    accnt       creditor_;
    int         amount_;
    std::string currency_;
    date_t      value_date_;
  };
  // Pomožna struktura za prenos podatkov iz constexpr funkcije
  struct parse_result
  {
    std::array<xpath_el, max_xpath_depth> data{};
    size_t                                size = 0;
  };

  constexpr std::string_view uri_from_prefix(std::string_view prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
    {
      if (el.prefix == prefix) return el.uri;
    }
    auto msg = fmt::format("Prefix '{}' has no matching definition in ns structure.", prefix);
    throw std::invalid_argument(msg.data());
  }
  // Constexpr funkcija za razporeditev niza
  constexpr parse_result parse_xpath(std::string_view path, const std::span<const ns> ns_arr)
  {
    parse_result res{};
    size_t       start = 0;
    if (path.empty()) throw std::invalid_argument("attribute .path must not be empty");
    if (path.at(0) == '/') path = path.substr(1);
    if (res.size >= max_xpath_depth)
    {
      auto msg = fmt::format("Path {} has more than n tags. Increase {} accordingly.", path, "max_xpath_depth");
      throw std::invalid_argument(msg);
    }
    while (start < path.size() && res.size < max_xpath_depth)
    {
      size_t           end     = path.find('/', start);
      std::string_view segment = path.substr(start, end - start);

      if (! segment.empty())
      {
        size_t colon = segment.find(':');
        if (colon != std::string_view::npos)
        { // we have namespace
          res.data.at(res.size).ns  = uri_from_prefix(segment.substr(0, colon), ns_arr);
          res.data.at(res.size).tag = segment.substr(colon + 1);
        }
        else
        { // no namespace or default namespace
          res.data.at(res.size).ns  = uri_from_prefix("", ns_arr);
          res.data.at(res.size).tag = segment;
        }
        res.size++;
      }

      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return res;
  }

  // Pomožna funkcija, ki sprejme delno inicializiran objekt,
  // dopolni .xpath ter ohrani vse ostale zastavice (is_attr, is_array, is_opt)
  constexpr attr_attr MA(attr_attr attr, const std::span<const ns> ns_arr)
  {
    auto parsed     = parse_xpath(attr.path, ns_arr);
    attr.xpath      = parsed.data;
    attr.xpath_size = parsed.size;
    return attr;
  }

} // namespace fsp
