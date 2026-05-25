#pragma once
#include "parsing_util.hpp"


namespace fsp
{
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

} // namespace fsp