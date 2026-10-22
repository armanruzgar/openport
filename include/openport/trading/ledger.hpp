#pragma once

#include "openport/trading/types.hpp"

namespace openport::trading {

struct Position {
  md::OptionContract contract;
  Quantity quantity = 0;
  Money basis;  ///< Signed total acquisition notional, excludes fees.
  Money realised;
  Money fees;
};
/// Shares of an underlying, from exercise and assignment.
struct StockPosition {
  std::string symbol;   ///< The underlying, e.g. "SPY".
  Quantity shares = 0;  ///< Signed.
  Money basis;          ///< Signed total acquisition cost, excludes fees.
  Money realised;
  Money fees;
};
struct Account {
  Money cash;
  Money realised;  ///< Gross, before fees, including settled/closed positions.
  Money fees;
};

/// Pure accounting. Mutations have the strong exception guarantee. Reductions
/// allocate signed basis proportionally, nearest micro-dollar (ties away); the
/// residual stays in the position and is fully released on the last close.
class Ledger {
 public:
  explicit Ledger(Money initial_cash = {}) : account_{initial_cash, {}, {}} {}
  [[nodiscard]] const Account& account() const { return account_; }
  [[nodiscard]] const std::map<std::string, Position>& positions() const { return positions_; }
  [[nodiscard]] const std::map<std::string, StockPosition>& stocks() const { return stocks_; }
  void fill(const md::OptionContract& contract, Quantity signed_quantity, Money price, Money fee);
  /// Buy (positive) or sell shares at a price per share, with the same basis
  /// allocation as options.
  void trade_stock(const std::string& symbol, Quantity signed_shares, Money price, Money fee);
  /// Settlement is accounting, never subject to order risk limits; delivery may charge a fee.
  void settle(const std::string& symbol, Money intrinsic, Money fee = {});
  /// Cash leaves the account (a payout); realised P&L is unchanged.
  void withdraw(Money amount);
  /// Capital credit, never realised profit or a fill.
  void add_capital(Money amount);
  /// Interest or borrow charges: cash and realised profit, without changing positions.
  void accrue(Money amount);
  /// A dividend on held shares: cash and realised P&L move by `amount`, which is
  /// negative when short shares pay it.
  void receive_dividend(const std::string& symbol, Money amount);
  /// Split held shares: every `denominator` become `numerator`, whole shares toward
  /// zero; the fraction left leaves with its proportional basis for `cash_in_lieu`
  /// (signed: short shares pay it), realising the difference. Returns the shares now held.
  Quantity split_stock(const std::string& symbol, std::int64_t numerator, std::int64_t denominator, Money cash_in_lieu);
  /// Validated journal outcomes only; public for independent outcome consumers.
  static Ledger restore(Account account, std::map<std::string, Position> positions,
                        std::map<std::string, StockPosition> stocks = {});
 private:
  Account account_;
  std::map<std::string, Position> positions_;
  std::map<std::string, StockPosition> stocks_;
};

}  // namespace openport::trading
