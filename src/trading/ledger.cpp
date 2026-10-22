#include "openport/trading/ledger.hpp"

#include <algorithm>
#include <limits>

namespace openport::trading {
namespace {
Quantity magnitude(Quantity q) {
  if (q == std::numeric_limits<Quantity>::min()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Quantity magnitude overflow");
  return q < 0 ? -q : q;
}
Quantity add_quantity(Quantity a, Quantity b) {
  Quantity result = 0;
  if (__builtin_add_overflow(a, b, &result)) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Position quantity overflow");
  return result;
}
/// Trade `units` (signed contracts or shares) at `unit` (the price times the
/// multiplier) against a holding and return the realised P&L. A reduction
/// releases its share of the signed basis; a reversal opens the rest at the price.
Money trade(Quantity& quantity, Money& basis, Quantity units, Money unit) {
  const auto old_q = quantity;
  const auto total_q = add_quantity(old_q, units);
  (void)magnitude(total_q);
  const auto size = magnitude(units);
  const auto old_size = magnitude(old_q);
  Money realised;
  if (old_q == 0 || (old_q > 0) == (units > 0)) {
    basis = basis + unit * units;
  } else {
    const auto closed = std::min(old_size, size);
    const Money allocated = basis.prorate(closed, old_size);
    realised = (unit * (old_q > 0 ? closed : -closed)) - allocated;
    basis = basis - allocated;
    if (size > closed) basis = unit * total_q;
  }
  quantity = total_q;
  return realised;
}
/// Add a fill's realised P&L and fee to the lifecycle a holding describes. A
/// reversal ends that lifecycle and opens the excess as a new one, which starts
/// with nothing realised and the excess's share of the fee, as the trades view
/// splits it.
void book(Money& lifecycle_realised, Money& lifecycle_fees, Quantity old_q, Quantity new_q, Quantity units,
          Money realised, Money fee) {
  if (old_q != 0 && new_q != 0 && (old_q > 0) != (new_q > 0)) {
    lifecycle_realised = Money{};
    lifecycle_fees = fee - fee.prorate(magnitude(old_q), magnitude(units));
    return;
  }
  lifecycle_realised = lifecycle_realised + realised;
  lifecycle_fees = lifecycle_fees + fee;
}
/// The terms a ledger holds: the session admits an adjusted series with its deliverable,
/// so the ledger checks the rest of its terms.
Decision ledger_terms(const md::OptionContract& contract) {
  const Deliverable adjusted;
  return eligible(contract, contract.standard ? nullptr : &adjusted);
}
}  // namespace
void Ledger::fill(const md::OptionContract& contract, Quantity signed_quantity, Money price, Money fee) {
  const auto decision = ledger_terms(contract);
  if (!decision.ok()) throw TradingError(decision.code, decision.message);
  if (signed_quantity == 0 || price < Money{} || fee < Money{})
    throw TradingError(Reason::INVALID_ORDER, "Fill requires nonzero quantity and nonnegative price/fee");
  const auto symbol = contract.osi_symbol();
  const auto it = positions_.find(symbol);
  Position next = it == positions_.end() ? Position{contract, 0, {}, {}, {}} : it->second;
  const Money cash = account_.cash - (price * 100) * signed_quantity - fee;
  const auto held = next.quantity;
  const Money realised = trade(next.quantity, next.basis, signed_quantity, price * 100);
  book(next.realised, next.fees, held, next.quantity, signed_quantity, realised, fee);
  const Account account{cash, account_.realised + realised, account_.fees + fee};
  if (next.quantity == 0) positions_.erase(symbol);
  else positions_.insert_or_assign(symbol, std::move(next));
  account_ = account;
}
void Ledger::trade_stock(const std::string& symbol, Quantity signed_shares, Money price, Money fee) {
  if (symbol.empty() || signed_shares == 0 || price < Money{} || fee < Money{})
    throw TradingError(Reason::INVALID_ORDER, "A stock trade needs a symbol, nonzero shares and nonnegative price/fee");
  const auto it = stocks_.find(symbol);
  StockPosition next = it == stocks_.end() ? StockPosition{symbol, 0, {}, {}, {}} : it->second;
  const Money cash = account_.cash - price * signed_shares - fee;
  const auto held = next.shares;
  const Money realised = trade(next.shares, next.basis, signed_shares, price);
  book(next.realised, next.fees, held, next.shares, signed_shares, realised, fee);
  const Account account{cash, account_.realised + realised, account_.fees + fee};
  if (next.shares == 0) stocks_.erase(symbol);
  else stocks_.insert_or_assign(symbol, std::move(next));
  account_ = account;
}
void Ledger::settle(const std::string& symbol, Money intrinsic, Money fee) {
  const auto it = positions_.find(symbol);
  if (it == positions_.end() || intrinsic < Money{} || fee < Money{})
    throw TradingError(Reason::INVALID_SETTLEMENT, "Settlement requires an open position, nonnegative intrinsic and fee");
  const Money proceeds = (intrinsic * 100) * it->second.quantity;
  const Account next{account_.cash + proceeds - fee, account_.realised + proceeds - it->second.basis, account_.fees + fee};
  positions_.erase(it);
  account_ = next;
}
void Ledger::accrue(Money amount) {
  const Account next{account_.cash + amount, account_.realised + amount, account_.fees};
  account_ = next;
}
void Ledger::add_capital(Money amount) {
  if (amount <= Money{}) throw TradingError(Reason::INVALID_MONEY, "Capital credit must be positive");
  account_.cash = account_.cash + amount;
}
void Ledger::withdraw(Money amount) {
  if (amount <= Money{}) throw TradingError(Reason::INVALID_PAYOUT, "Withdrawal must be positive");
  account_.cash = account_.cash - amount;
}
void Ledger::receive_dividend(const std::string& symbol, Money amount) {
  const auto it = stocks_.find(symbol);
  if (it == stocks_.end() || amount == Money{})
    throw TradingError(Reason::INVALID_ORDER, "A dividend needs held shares and a nonzero amount");
  const Account next{account_.cash + amount, account_.realised + amount, account_.fees};
  it->second.realised = it->second.realised + amount;
  account_ = next;
}
Quantity Ledger::split_stock(const std::string& symbol, std::int64_t numerator, std::int64_t denominator, Money cash_in_lieu) {
  const auto it = stocks_.find(symbol);
  if (it == stocks_.end() || numerator < 1 || denominator < 1)
    throw TradingError(Reason::INVALID_ORDER, "A split needs held shares and positive terms");
  __extension__ using Wide = __int128;
  const auto held = it->second.shares;
  const Wide scaled = static_cast<Wide>(held) * numerator;  // the split shares, times the denominator
  const auto after = static_cast<Quantity>(scaled / denominator);  // whole shares, toward zero
  const Wide fraction = scaled - static_cast<Wide>(after) * denominator;
  // The fraction leaves with its share of the basis, sold for the cash in lieu.
  const Money released = after == 0 ? it->second.basis
      : Money::from_micros(static_cast<std::int64_t>(static_cast<Wide>(it->second.basis.micros()) * fraction / scaled));
  const Money realised = cash_in_lieu - released;
  StockPosition next = it->second;
  next.shares = after;
  next.basis = next.basis - released;
  next.realised = next.realised + realised;
  const Account account{account_.cash + cash_in_lieu, account_.realised + realised, account_.fees};
  if (after == 0) stocks_.erase(it);
  else it->second = std::move(next);
  account_ = account;
  return after;
}
Ledger Ledger::restore(Account account, std::map<std::string, Position> positions,
                       std::map<std::string, StockPosition> stocks) {
  for (const auto& [symbol, p] : positions) {
    if (!ledger_terms(p.contract).ok() || p.contract.osi_symbol() != symbol || p.quantity == 0 ||
        p.quantity == std::numeric_limits<Quantity>::min() || p.fees < Money{} ||
        (p.quantity > 0 ? p.basis < Money{} : p.basis > Money{}))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Invalid recorded ledger position");
  }
  for (const auto& [symbol, p] : stocks) {
    if (symbol.empty() || p.symbol != symbol || p.shares == 0 || p.shares == std::numeric_limits<Quantity>::min() ||
        p.fees < Money{} || (p.shares > 0 ? p.basis < Money{} : p.basis > Money{}))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Invalid recorded stock position");
  }
  if (account.fees < Money{}) throw TradingError(Reason::JOURNAL_CORRUPT, "Negative recorded fees");
  Ledger result;
  result.account_ = account;
  result.positions_ = std::move(positions);
  result.stocks_ = std::move(stocks);
  return result;
}
}  // namespace openport::trading
