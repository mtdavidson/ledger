#include "walk.h"
#include "format.h"

namespace ledger {

void sort_transactions::flush()
{
  std::stable_sort(transactions.begin(), transactions.end(),
		   compare_items<transaction_t>(sort_order));

  for (transactions_deque::iterator i = transactions.begin();
       i != transactions.end();
       i++)
    (*handler)(*i);

  transactions.clear();

  item_handler<transaction_t>::flush();
}

void calc_transactions::operator()(transaction_t * xact)
{
  if (! xact->data)
    xact->data = new transaction_data_t;

  if (last_xact && last_xact->data) {
    XACT_DATA(xact)->total += XACT_DATA(last_xact)->total;
    XACT_DATA(xact)->index = XACT_DATA(last_xact)->index + 1;
  } else {
    XACT_DATA(xact)->index = 0;
  }

  if (inverted) {
    xact->amount.negate();
    if (xact->cost)
      xact->cost->negate();
  }

  if (! (XACT_DATA(xact)->dflags & TRANSACTION_NO_TOTAL))
    XACT_DATA(xact)->total += *xact;

  (*handler)(xact);

  if (inverted) {
    xact->amount.negate();
    if (xact->cost)
      xact->cost->negate();
  }

  last_xact = xact;
}


static void handle_value(const value_t& value, account_t * account,
			 entry_t * entry, unsigned int flags,
			 transactions_deque& temps,
			 item_handler<transaction_t> * handler)
{
  balance_t * bal = NULL;

  switch (value.type) {
  case value_t::BOOLEAN:
  case value_t::INTEGER:
  case value_t::AMOUNT: {
    transaction_t * xact = new transaction_t(account);
    temps.push_back(xact);

    xact->entry   = entry;
    switch (value.type) {
    case value_t::BOOLEAN:
      xact->amount  = *((bool *) value.data);
      break;
    case value_t::INTEGER:
      xact->amount  = *((unsigned int *) value.data);
      break;
    case value_t::AMOUNT:
      xact->amount  = *((amount_t *) value.data);
      break;
    default:
      assert(0);
      break;
    }

    if (flags) {
      if (! xact->data)
	xact->data = new transaction_data_t;
      XACT_DATA(xact)->dflags |= flags;
    }

    (*handler)(xact);
    break;
  }

  case value_t::BALANCE_PAIR:
    bal = &((balance_pair_t *) value.data)->quantity;
    // fall through...

  case value_t::BALANCE:
    if (! bal)
      bal = (balance_t *) value.data;

    for (amounts_map::const_iterator i = bal->amounts.begin();
	 i != bal->amounts.end();
	 i++) {
      transaction_t * xact = new transaction_t(account);
      temps.push_back(xact);

      xact->entry   = entry;
      xact->amount  = (*i).second;

      if (flags) {
	if (! xact->data)
	  xact->data = new transaction_data_t;
	XACT_DATA(xact)->dflags |= flags;
      }

      (*handler)(xact);
    }
    break;

  default:
    assert(0);
    break;
  }
}

void collapse_transactions::report_cumulative_subtotal()
{
  if (count == 1) {
    (*handler)(last_xact);
  } else {
    assert(count > 1);

    if (! totals_account->data)
      totals_account->data = new account_data_t;
    ACCT_DATA(totals_account)->total = subtotal;
    value_t result;
    format_t::compute_total(result, details_t(totals_account));
    handle_value(result, totals_account, last_entry, 0, xact_temps, handler);
  }

  subtotal = 0;
  count    = 0;
}

void changed_value_transactions::operator()(transaction_t * xact)
{
  if (last_xact) {
    value_t	prev_bal;
    value_t	cur_bal;
    std::time_t current   = xact ? xact->entry->date : std::time(NULL);
    std::time_t prev_date = last_xact->entry->date;

    format_t::compute_total(prev_bal, details_t(last_xact));

    last_xact->entry->date = current;
    format_t::compute_total(cur_bal,  details_t(last_xact));
    last_xact->entry->date = prev_date;

    cur_bal -= prev_bal;
    if (cur_bal) {
      entry_t * entry = new entry_t;
      entry_temps.push_back(entry);

      entry->payee = "Commodities revalued";
      entry->date  = current;

      handle_value(cur_bal, NULL, entry, TRANSACTION_NO_TOTAL, xact_temps,
		   handler);
    }
  }

  if (xact) {
    if (changed_values_only) {
      if (! xact->data)
	xact->data = new transaction_data_t;
      XACT_DATA(xact)->dflags |= TRANSACTION_DISPLAYED;
    }

    (*handler)(xact);
  }

  last_xact = xact;
}

void subtotal_transactions::flush(const char * spec_fmt)
{
  char buf[256];

  if (! spec_fmt) {
    std::string fmt = "- ";
    fmt += format_t::date_format;

    // Make sure the end date is inclusive
    if (start != finish)
      finish -= 86400;

    std::strftime(buf, 255, fmt.c_str(), std::localtime(&finish));
  } else {
    std::strftime(buf, 255, spec_fmt, std::localtime(&finish));
  }

  entry_t * entry = new entry_t;
  entry_temps.push_back(entry);

  entry->payee = buf;

  value_t result;

  for (balances_map::iterator i = balances.begin();
       i != balances.end();
       i++) {
    entry->date = finish;
    {
      transaction_t temp((*i).first);
      temp.entry = entry;
      {
	std::auto_ptr<transaction_data_t> xact_data(new transaction_data_t);
	temp.data = xact_data.get();
	((transaction_data_t *) temp.data)->total = (*i).second;
	format_t::compute_total(result, details_t(&temp));
      }
      temp.data = NULL;
    }
    entry->date = start;

    handle_value(result, (*i).first, entry, 0, xact_temps, handler);
  }

  balances.clear();

  item_handler<transaction_t>::flush();
}

void subtotal_transactions::operator()(transaction_t * xact)
{
  if (balances.size() == 0) {
    start = finish = xact->entry->date;
  } else {
    if (std::difftime(xact->entry->date, start) < 0)
      start = xact->entry->date;
    if (std::difftime(xact->entry->date, finish) > 0)
      finish = xact->entry->date;
  }

  balances_map::iterator i = balances.find(xact->account);
  if (i == balances.end())
    balances.insert(balances_pair(xact->account, *xact));
  else
    (*i).second += *xact;
}

void interval_transactions::operator()(transaction_t * xact)
{
  std::time_t quant = interval.increment(begin);
  if (std::difftime(xact->entry->date, quant) > 0) {
    if (last_xact) {
      start  = begin;
      finish = quant;
      flush();
    }

    if (! interval.seconds) {
      struct std::tm * desc = std::localtime(&xact->entry->date);
      if (interval.years)
	desc->tm_mon = 0;
      desc->tm_mday = 1;
      desc->tm_hour = 0;
      desc->tm_min  = 0;
      desc->tm_sec  = 0;
      quant = std::mktime(desc);
    }

    std::time_t temp;
    while (std::difftime(xact->entry->date,
			 temp = interval.increment(quant)) > 0)
      quant = temp;
    begin = quant;
  }

  subtotal_transactions::operator()(xact);

  last_xact = xact;
}

void dow_transactions::flush()
{
  for (int i = 0; i < 7; i++) {
    for (transactions_deque::iterator d = days_of_the_week[i].begin();
	 d != days_of_the_week[i].end();
	 d++)
      subtotal_transactions::operator()(*d);
    subtotal_transactions::flush("%As");
    days_of_the_week[i].clear();
  }
}

} // namespace ledger
