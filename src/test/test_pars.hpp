#pragma once
#include "parsing_util.hpp"
// #include <chrono>


namespace fsp
{
  using str_t = std::string;

  struct accnt
  {
    str_t iban_;
    str_t bic_;
  };

  struct ct_txn
  {
    int         txn_id_;
    accnt       debtor_;
    accnt       creditor_;
    int         amount_;
    str_t       currency_;
    date_t      value_date_;
  };

} // namespace fsp