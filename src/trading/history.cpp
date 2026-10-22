#include "openport/trading/history.hpp"

#include <algorithm>
#include <cmath>

namespace openport::trading {

Money naked_requirement(const md::OptionContract& c, std::optional<double> spot) {
  const double s = spot && std::isfinite(*spot) && *spot > 0 ? *spot : c.strike;
  const bool call = c.type == pricing::OptionType::Call;
  const double otm = std::max(0.0, call ? c.strike - s : s - c.strike);
  const double per_unit = std::max(0.20 * s - otm, 0.10 * (call ? s : c.strike));
  return Money::from_double(per_unit * c.multiplier);
}

namespace {
struct Open {
  std::size_t index = 0;
  Ledger ledger;
};
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
}  // namespace

void LifecycleBuilder::start(const Fill& fill, const md::OptionContract& contract, Quantity signed_quantity) {
  Open entry;
  entry.started = started_++;
  auto& life = entry.life;
  life.symbol = fill.symbol;
  life.contract = contract;
  life.direction = signed_quantity > 0 ? 1 : -1;
  life.opened = fill.time;
  life.first_fill = fill.id;
  life.entry_order = fill.order_id;
  life.entry_context = fill.context;
  life.root = roots.emplace(fill.order_id, fill.id).first->second;
  if (record && fill.id > record_from) effects.push_back({fill.order_id, fill.time, fill.id, fill.order_id, true});
  open[fill.symbol] = std::move(entry);
}
void LifecycleBuilder::finish(const std::string& symbol, Timestamp time) {
  const auto it = open.find(symbol);
  auto& life = it->second.life;
  life.closed = time;
  life.quantity = 0;
  life.basis = Money{};
  closed.emplace_back(it->second.started, std::move(life));
  open.erase(it);
}
void LifecycleBuilder::fill(const Fill& fill, const Contracts& contracts) {
  const auto contract = contracts.find(fill.symbol);
  if (contract == contracts.end() || fill.quantity <= 0) return;
  const Quantity signed_quantity = fill.side == Side::Buy ? fill.quantity : -fill.quantity;
  auto it = open.find(fill.symbol);
  if (it == open.end()) {
    start(fill, contract->second, signed_quantity);
    it = open.find(fill.symbol);
  }
  auto* life = &it->second.life;
  const auto held = life->quantity;
  if (held != 0) {
    if ((held > 0) == (signed_quantity > 0) && fill.order_id != life->entry_order) shared.insert(fill.order_id);
    if (record && fill.id > record_from) effects.push_back({fill.order_id, fill.time, life->first_fill, life->entry_order, false});
  }
  if (held == 0 || (held > 0) == (signed_quantity > 0)) {
    it->second.ledger.fill(contract->second, signed_quantity, fill.price, fill.fee);
    life->quantity = held + signed_quantity;
    life->opened_contracts += fill.quantity;
    life->open_notional = life->open_notional + fill.price * fill.quantity;
    if (fill.order_id == life->entry_order) {
      life->entry_contracts += fill.quantity;
      life->entry_notional = life->entry_notional + fill.price * fill.quantity;
    }
  } else {
    life->exit_context = fill.context;
    life->exit_order = fill.order_id;
    const auto closing = std::min(magnitude(held), fill.quantity);
    const auto remainder = fill.quantity - closing;
    const Money closing_fee = fill.fee.prorate(closing, fill.quantity);
    it->second.ledger.fill(contract->second, signed_quantity > 0 ? closing : -closing, fill.price, closing_fee);
    life->quantity = held + (signed_quantity > 0 ? closing : -closing);
    life->closed_contracts += closing;
    life->close_notional = life->close_notional + fill.price * closing;
    if (remainder > 0) {
      life->fills.push_back(fill.id);
      life->gross = it->second.ledger.account().realised;
      life->fees = it->second.ledger.account().fees;
      finish(fill.symbol, fill.time);
      start(fill, contract->second, signed_quantity > 0 ? remainder : -remainder);
      it = open.find(fill.symbol);
      life = &it->second.life;
      it->second.ledger.fill(contract->second, signed_quantity > 0 ? remainder : -remainder,
                             fill.price, fill.fee - closing_fee);
      life->quantity = signed_quantity > 0 ? remainder : -remainder;
      life->opened_contracts += remainder;
      life->open_notional = life->open_notional + fill.price * remainder;
      life->entry_contracts += remainder;
      life->entry_notional = life->entry_notional + fill.price * remainder;
    }
  }
  life->basis = it->second.ledger.positions().contains(fill.symbol)
      ? it->second.ledger.positions().at(fill.symbol).basis : Money{};
  life->max_quantity = std::max(life->max_quantity, magnitude(life->quantity));
  life->fills.push_back(fill.id);
  life->gross = it->second.ledger.account().realised;
  life->fees = it->second.ledger.account().fees;
  if (life->quantity == 0) finish(fill.symbol, fill.time);
}
void LifecycleBuilder::closure(const Closure& closure, const Contracts& contracts) {
  const auto it = open.find(closure.symbol);
  const auto contract = contracts.find(closure.symbol);
  if (it == open.end() || contract == contracts.end()) return;
  auto& life = it->second.life;
  const auto held = life.quantity;
  if (held == 0) return;
  // Exercise and assignment can take part of the position; the rest stays open.
  const auto closing = (closure.kind == ClosureKind::Exercise || closure.kind == ClosureKind::Assignment)
      ? std::min(magnitude(closure.quantity), magnitude(held)) : magnitude(held);
  if (closure.kind == ClosureKind::Settlement) {
    it->second.ledger.settle(closure.symbol, closure.price, closure.fee);
  } else {
    it->second.ledger.fill(contract->second, held > 0 ? -closing : closing, closure.price, closure.fee);
  }
  life.exit_context.reset();
  life.exit_order = 0;
  life.basis = it->second.ledger.positions().contains(closure.symbol)
      ? it->second.ledger.positions().at(closure.symbol).basis : Money{};
  life.quantity = held > 0 ? held - closing : held + closing;
  life.closed_contracts += closing;
  life.close_notional = life.close_notional + closure.price * closing;
  life.gross = it->second.ledger.account().realised;
  life.fees = it->second.ledger.account().fees;
  if (life.quantity != 0) return;
  life.closure = closure.kind;
  finish(closure.symbol, closure.time);
}

std::vector<Lifecycle> lifecycles(const SharedVector<Fill>& fills, const SharedVector<Closure>& closures,
                                  const Contracts& contracts, std::set<OrderId>* shared) {
  LifecycleBuilder builder;
  std::size_t next_closure = 0;
  auto closures_until = [&](std::uint64_t executed) {
    while (next_closure < closures.size() && closures[next_closure].after_fill <= executed)
      builder.closure(closures[next_closure++], contracts);
  };
  closures_until(0);
  for (std::size_t i = 0; i < fills.size(); ++i) {
    builder.fill(fills[i], contracts);
    closures_until(i + 1);
  }
  while (next_closure < closures.size()) builder.closure(closures[next_closure++], contracts);
  if (shared) *shared = std::move(builder.shared);
  // In the order they opened, as the account traded them.
  auto started = std::move(builder.closed);
  for (auto& [symbol, entry] : builder.open) started.emplace_back(entry.started, std::move(entry.life));
  std::sort(started.begin(), started.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Lifecycle> out;
  out.reserve(started.size());
  for (auto& entry : started) out.push_back(std::move(entry.second));
  return out;
}

std::string trade_group(const Lifecycle& life, const SharedMap<std::string, std::string>& groups) {
  const auto it = groups.find(std::to_string(life.first_fill));
  return it == groups.end() ? std::to_string(life.root) : it->second;
}

Money entry_buying_power(const std::vector<const Lifecycle*>& legs) {
  std::vector<MarginLeg> margin;
  Money paid;
  for (const auto* leg : legs) {
    const Money value = leg->open_notional * leg->contract.multiplier;
    paid = paid + value * leg->direction;
    margin.push_back({leg->contract, leg->opened_contracts * leg->direction, leg->direction < 0 ? value : Money{},
                      leg->entry_context ? leg->entry_context->spot : std::nullopt});
  }
  return paid + margin_requirement(margin);
}

Money peak_buying_power(const std::vector<const Lifecycle*>& legs) {
  Money peak;
  for (const auto* at : legs) {
    auto filled = at->first_fill;
    for (const auto* leg : legs)
      if (leg->entry_order == at->entry_order && leg->opened == at->opened) filled = std::max(filled, leg->first_fill);
    std::vector<const Lifecycle*> open;
    for (const auto* leg : legs)
      if (leg->first_fill <= filled && (!leg->closed || *leg->closed > at->opened ||
          (*leg->closed == at->opened && !leg->closure && leg->exit_order != at->entry_order && leg->fills.back() > filled)))
        open.push_back(leg);
    peak = std::max(peak, entry_buying_power(open));
  }
  return peak;
}

std::vector<ShareLifecycle> share_lifecycles(const SharedVector<StockFill>& fills,
                                             const SharedVector<DividendPayment>& dividends) {
  std::vector<ShareLifecycle> out;
  std::map<std::string, Open> open;
  auto start = [&](const StockFill& fill, Quantity signed_shares) {
    ShareLifecycle life;
    life.symbol = fill.symbol;
    life.direction = signed_shares > 0 ? 1 : -1;
    life.opened = fill.time;
    out.push_back(std::move(life));
    open[fill.symbol] = Open{out.size() - 1, Ledger{}};
  };
  // Each fill opens, adds, reduces or reverses; the ledger keeps the basis.
  auto trade = [&](Open& o, const StockFill& fill, Quantity signed_shares) {
    auto& life = out[o.index];
    const auto held = life.shares;
    o.ledger.trade_stock(fill.symbol, signed_shares, fill.price, Money{});
    life.shares = held + signed_shares;
    if (held == 0 || (held > 0) == (signed_shares > 0)) {
      life.opened_shares += magnitude(signed_shares);
      life.open_notional = life.open_notional + fill.price * magnitude(signed_shares);
    } else {
      life.closed_shares += magnitude(signed_shares);
      life.close_notional = life.close_notional + fill.price * magnitude(signed_shares);
    }
    life.max_shares = std::max(life.max_shares, magnitude(life.shares));
    life.gross = o.ledger.account().realised;
  };
  // Dividends are paid to the round trip open in their symbol, after the stock fills
  // recorded before them: an overnight assignment comes before the ex-date's payment,
  // and a sale at the rollover's own market time after it. A payment recorded before
  // that count was kept follows the fills before its time and, at its time, the
  // rollover's assignments and the deliveries settled just before it.
  std::size_t next_dividend = 0;
  auto pay_until = [&](std::size_t filled, const StockFill* fill) {
    while (next_dividend < dividends.size()) {
      const auto& d = dividends[next_dividend];
      const bool before = !fill || (d.after_stock_fill ? *d.after_stock_fill <= filled
          : d.time < fill->time || (d.time == fill->time && fill->source != StockSource::Assignment &&
                                    fill->source != StockSource::Delivery));
      if (!before) break;
      ++next_dividend;
      if (const auto it = open.find(d.symbol); it != open.end()) out[it->second.index].dividends = out[it->second.index].dividends + d.amount;
    }
  };
  for (std::size_t filled = 0; filled < fills.size(); ++filled) {
    const auto& fill = fills[filled];
    // A split changes the shares of the round trip open in them, keeping its basis,
    // and closes it when only a fraction was left to be paid in cash.
    if (fill.source == StockSource::Split && fill.split_numerator > 0) {
      pay_until(filled, &fill);
      const auto it = open.find(fill.symbol);
      if (it == open.end()) continue;
      auto& life = out[it->second.index];
      life.shares = it->second.ledger.split_stock(fill.symbol, fill.split_numerator, fill.split_denominator, fill.cash_in_lieu);
      life.max_shares = std::max(life.max_shares, magnitude(life.shares));
      life.gross = it->second.ledger.account().realised;
      life.fills.push_back(fill.id);
      if (life.shares == 0) {
        life.closed = fill.time;
        open.erase(it);
      }
      continue;
    }
    if (fill.shares == 0) continue;
    pay_until(filled, &fill);
    if (!open.contains(fill.symbol)) start(fill, fill.shares);
    auto* o = &open.at(fill.symbol);
    const auto held = out[o->index].shares;
    const bool reverses = held != 0 && (held > 0) != (fill.shares > 0) && magnitude(fill.shares) > magnitude(held);
    trade(*o, fill, reverses ? -held : fill.shares);
    out[o->index].fills.push_back(fill.id);
    if (out[o->index].shares == 0) {
      out[o->index].closed = fill.time;
      open.erase(fill.symbol);
      if (!reverses) continue;
      const auto remainder = fill.shares + held;
      start(fill, remainder);
      o = &open.at(fill.symbol);
      trade(*o, fill, remainder);
      out[o->index].fills.push_back(fill.id);
    }
  }
  pay_until(fills.size(), nullptr);
  return out;
}

}  // namespace openport::trading
