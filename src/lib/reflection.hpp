#pragma once

#include <string>
namespace fsp
{
  using str_t  = std::string;
  using cstr_t = std::string_view;

  class xpath
  {
  public:
    consteval xpath() = default;
    consteval explicit xpath(cstr_t path)
    : xpath(path, false) { };
    consteval xpath([[maybe_unused]] cstr_t path, [[maybe_unused]] bool is_opt) { };
  };
  class pacs8_header
  {
  public:
  private:
    [[= xpath("x:GrpHdr/MsgId")]] str_t                      msg_id;
    [[= xpath("GrpHdr/TtlIntrBkSttlmAmt")]] int64_t          amount_sum;
    [[= xpath("GrpHdr/TtlIntrBkSttlmAmt/@Cct", true)]] str_t amount_sum_cur;
    [[= xpath("x:GrpHdr/CreDtTm", true)]] str_t              msg_ts;
    [[= xpath("GrpHdr/IntrBkSttlmDt", true)]] str_t          value_date;
  };
}; // namespace fsp
