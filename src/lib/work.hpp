#pragma once

#include "reflection.hpp"
namespace[[= fsp::xpath("xx")]] work
{
  class[[= fsp::xpath("/x:Document/FIToFICstmrCdtTrf/x:GrpHdr")]] pacs8_header
  {
  public:
    // clang-format off
    [[= fsp::xpath("x:GrpHdr/MsgId")]]                      str_t   msg_id;
    [[= fsp::xpath("GrpHdr/TtlIntrBkSttlmAmt")]]            int64_t amount_sum;
    [[= fsp::xpath("GrpHdr/TtlIntrBkSttlmAmt/@Cct", true)]] str_t   amount_sum_cur;
    [[= fsp::xpath("x:GrpHdr/CreDtTm", true)]]              str_t   msg_ts;
    [[= fsp::xpath("GrpHdr/IntrBkSttlmDt", true)]]          str_t   value_date;
    // clang-format on
  };

  class[[= fsp::xpath("/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf")]] pacs8_txn
  {
  public:
    // clang-format off
    [[= fsp::xpath("CdtTrfTxInf/PmtId/TxId")]]                       str_t   txn_is;
    [[= fsp::xpath("CdtTrfTxInf/DbtrAcct/Id/IBAN")]]                 int64_t debtor_iban;
    [[= fsp::xpath("CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI", true)]]   str_t   debtor_bic;
    [[= fsp::xpath("CdtTrfTxInf/CdtrAcct/Id/IBAN")]]                 int64_t creditor_iban;
    [[= fsp::xpath("CdtTrfTxInf/CdtrAgt/FinInstnId/BICFI", true)]]   str_t   creditor_bic;
    [[= fsp::xpath("CdtTrfTxInf/IntrBkSttlmAmt", true)]]             str_t   amount;
    [[= fsp::xpath("CdtTrfTxInf/IntrBkSttlmAmt/@Ccy", true)]]        str_t   curency;
    [[= fsp::xpath("CdtTrfTxInf/InstgAgt/FinInstnId/*BICFI", true)]] str_t   inst_agent;
    // clang-format on
  };
} // namespace work
