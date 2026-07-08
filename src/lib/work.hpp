#pragma once

#include "reflection.hpp"
namespace[[= std::to_array<fsp::ns>({{.prefix = "x", .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}})]] work
{
  static const int xxx = 10;
  // static constexpr auto ns  = std::to_array<fsp::ns>({
  //   {.prefix = "", .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"},  // default namespace
  //   {.prefix = "x", .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
  //   {.prefix = "xy", .uri = "krneki"},                                        // explicitly defined namespace and prefix
  // });
  class[[= fsp::xpath("/x:Document/FIToFICstmrCdtTrf/x:GrpHdr")]] pacs8_header
  {
  public:
    // clang-format off
    [[= "x:GrpHdr/MsgId"]]                str_t   msg_id;
    [[= "GrpHdr/TtlIntrBkSttlmAmt"]]      int64_t amount_sum;
    [[= "GrpHdr/TtlIntrBkSttlmAmt/@Cct"]] str_t   amount_sum_cur;
    [[= "x:GrpHdr/CreDtTm"]]              str_t   msg_ts;
    [[= "GrpHdr/IntrBkSttlmDt"]]          str_t   value_date;
    // clang-format on
  };

  class[[= fsp::xpath("/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf")]] pacs8_txn
  {
  public:
    // clang-format off
    [[= "CdtTrfTxInf/PmtId/TxId"]]                 str_t   txn_is;
    [[= "CdtTrfTxInf/DbtrAcct/Id/IBAN"]]           int64_t debtor_iban;
    [[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]]   str_t   debtor_bic;
    [[= "CdtTrfTxInf/CdtrAcct/Id/IBAN"]]           int64_t creditor_iban;
    [[= "CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI"]]   str_t   creditor_bic;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]             str_t   amount;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt/@Ccy"]]        str_t   curency;
    [[= "CdtTrfTxInf/InstgAgt/FinInstnId/*BICFI"]] str_t   inst_agent;
    // clang-format on
  };
} // namespace work
