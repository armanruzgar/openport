#include <filesystem>
#include <tuple>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
std::string osi(std::string_view compact) { return md::parse_osi(compact)->osi_symbol(); }
const std::string P4900 = osi("SPXW261022P04900000");
const std::string P4890 = osi("SPXW261022P04890000");
SessionConfig config(std::int64_t impact = 0) {
  SessionConfig result;
  result.limits.aggregate = {1e9, 1e9};
  result.limits.per_underlying = {1e9, 1e9};
  result.rules.impact_ticks = impact;
  return result;
}
class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-costs-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::string file(std::string_view name) const { return (path / name).string(); }
  std::filesystem::path path;
};
/// SPXW puts quoted together; every quote carries `size` on both sides.
struct Chain {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  void define(TradingSession& s, std::initializer_list<std::string> symbols) {
    for (const auto& symbol : symbols) ASSERT_TRUE(s.define(*md::parse_osi(symbol), time).decision.ok());
  }
  void quote(TradingSession& s, std::vector<std::tuple<std::string, std::string, std::string>> books, Quantity size = 10) {
    ++observation;
    time += md::kNanosPerSecond;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (const auto& [symbol, bid, ask] : books) {
      const auto contract = *md::parse_osi(symbol);
      quotes.push_back({symbol, observation, time, m(bid), m(ask), size, size});
      valuations.push_back({symbol, time, -0.3, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                            md::years_between(time, contract.expiry_time()), 0.20, true});
    }
    s.on_quotes(quotes, valuations, time);
  }
};
OrderRequest vertical(std::string client, Quantity units, std::optional<std::string_view> net) {
  OrderRequest r;
  r.client_order_id = std::move(client);
  r.type = net ? OrderType::Limit : OrderType::Market;
  r.tif = net ? TimeInForce::Day : TimeInForce::Ioc;
  r.quantity = units;
  if (net) r.limit_price = m(*net);
  r.legs = {{P4900, Side::Sell, 1}, {P4890, Side::Buy, 1}};
  return r;
}

TEST(FillAudit, FillsKeepTheBookTheyTookAndWhenItWasFirstQuoted) {
  ScriptedMarket market;
  TradingSession session(config(), market.time);
  market.seed(session, "4.00", "4.20", 10);
  const auto first = market.time;
  // The same observation, confirmed five seconds later, keeps its first time.
  market.time += 5 * md::kNanosPerSecond;
  session.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("buy", 3), market.time).decision.ok());
  ASSERT_TRUE(session.submit(market.market("more", 2), market.time).decision.ok());
  auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  ASSERT_TRUE(fills[0].quote && fills[1].quote);
  EXPECT_EQ(fills[0].quote->bid, m("4.00"));
  EXPECT_EQ(fills[0].quote->ask, m("4.20"));
  EXPECT_EQ(fills[0].quote->bid_size, 10);
  EXPECT_EQ(fills[0].quote->ask_size, 10);
  EXPECT_EQ(fills[0].quote->left, 10);
  EXPECT_EQ(fills[1].quote->left, 7);
  EXPECT_EQ(fills[0].quote->quoted, first);
  EXPECT_EQ(fills[0].quote_time, market.time);
  EXPECT_EQ(fills[0].observation, 1U);
  // A new observation the provider gave earlier than it was offered keeps that time.
  market.next();
  auto quote = market.quote("4.10", "4.30", 4);
  quote.quoted = market.time - 2 * md::kNanosPerSecond;
  session.on_quotes({quote}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("sell", 1, Side::Sell), market.time).decision.ok());
  const auto sold = session.snapshot()->recent_fills.back();
  ASSERT_TRUE(sold.quote);
  EXPECT_EQ(sold.quote->quoted, quote.quoted);
  EXPECT_EQ(sold.quote->bid, m("4.10"));
  EXPECT_EQ(sold.quote->left, 4);
  // A quote first given after its own time is refused, as a future-dated one is.
  market.next();
  quote = market.quote();
  quote.quoted = market.time + 1;
  try {
    session.on_quotes({quote}, {market.valuation()}, market.time);
    ADD_FAILURE() << "expected INVALID_TIME";
  } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_TIME); }
}

TEST(FillAudit, ImpactAndComboFillsRecordEachSlicesBook) {
  ScriptedMarket market;
  TradingSession session(config(1), market.time);
  market.seed(session, "4.00", "4.20", 2);
  ASSERT_TRUE(session.submit(market.market("sweep", 5), market.time).decision.ok());
  const auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  // Simulated depth beyond the displayed size leaves the budget below zero.
  EXPECT_EQ(fills[0].quote->left, 2);
  EXPECT_EQ(fills[1].quote->left, 0);
  EXPECT_EQ(fills[2].quote->left, -2);

  Chain chain;
  TradingSession spread(config(), chain.time);
  chain.define(spread, {P4900, P4890});
  chain.quote(spread, {{P4900, "2.00", "2.20"}, {P4890, "1.50", "1.65"}}, 6);
  ASSERT_TRUE(spread.submit(vertical("credit", 2, std::nullopt), chain.time).decision.ok());
  const auto legs = spread.snapshot()->recent_fills;
  ASSERT_EQ(legs.size(), 2U);
  for (const auto& fill : legs) {
    ASSERT_TRUE(fill.quote);
    EXPECT_EQ(fill.quote->left, 6);
    EXPECT_EQ(fill.quote->quoted, chain.time);
    EXPECT_EQ(fill.quote->bid, m(fill.symbol == P4900 ? "2.00" : "1.50"));
  }
}

TEST(FillAudit, TheFirstQuotedTimeSurvivesRecoveryAndOlderFillsLoadWithoutIt) {
  TemporaryDirectory directory;
  const auto path = directory.file("audit.jsonl");
  ScriptedMarket market;
  const auto first = market.time;
  std::string continuous;
  {
    TradingSession session(config(), market.time, FileJournal::create(path));
    market.seed(session, "4.00", "4.20", 10);
    market.time += 5 * md::kNanosPerSecond;
    session.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market("buy", 1), market.time).decision.ok());
    continuous = session.snapshot_json();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), continuous);
  // The book recovered with its first time: a later fill on it still reports it.
  market.time += md::kNanosPerSecond;
  recovered.on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
  ASSERT_TRUE(recovered.submit(market.market("again", 1), market.time).decision.ok());
  EXPECT_EQ(recovered.snapshot()->recent_fills.back().quote->quoted, first);
  EXPECT_EQ(recovered.snapshot()->recent_fills.back().quote->left, 9);

  // A fill recorded before fills kept their book loads without one.
  auto json = nlohmann::json::parse(recovered.snapshot_json());
  auto old = json.at("recent_fills").at(0);
  old.erase("quote");
  const auto fill = old.get<Fill>();
  EXPECT_FALSE(fill.quote);
  EXPECT_EQ(nlohmann::json(fill).contains("quote"), false);
}

TEST(FillAudit, FillsNameTheFeedThatPricedThemAndKeepItThroughRecovery) {
  TemporaryDirectory directory;
  const auto path = directory.file("feeds.jsonl");
  ScriptedMarket market;
  std::string continuous;
  {
    TradingSession session(config(), market.time, FileJournal::create(path));
    market.seed(session, "4.00", "4.20", 10);
    ASSERT_TRUE(session.submit(market.market("single", 1), market.time).decision.ok());
    // A single feed names nothing, and its fills keep their bytes.
    EXPECT_TRUE(session.snapshot()->recent_fills.back().feed.empty());
    EXPECT_FALSE(nlohmann::json(session.snapshot()->recent_fills.back()).contains("feed"));
    market.next();
    auto quote = market.quote("4.00", "4.20", 10);
    quote.feed = "cboe";
    session.on_quotes({quote}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market("fallback", 1), market.time).decision.ok());
    EXPECT_EQ(session.snapshot()->recent_fills.back().feed, "cboe");
    continuous = session.snapshot_json();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), continuous);
  const auto fills = recovered.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  EXPECT_TRUE(fills[0].feed.empty());
  EXPECT_EQ(fills[1].feed, "cboe");
  EXPECT_EQ(nlohmann::json(fills[1]).at("feed"), "cboe");
}

TEST(QuoteLiquidity, PreviewsShowEachLegsQuoteAndTheDisplayedSizeTheAccountLeft) {
  Chain chain;
  TradingSession session(config(), chain.time);
  chain.define(session, {P4900, P4890});
  chain.quote(session, {{P4900, "2.00", "2.20"}, {P4890, "1.50", "1.65"}}, 6);
  ASSERT_TRUE(session.submit(vertical("credit", 2, std::nullopt), chain.time).decision.ok());
  const auto left = session.sizes_left();
  ASSERT_EQ(left.size(), 2U);
  EXPECT_EQ(left.at(P4900).bid, 4);
  EXPECT_EQ(left.at(P4900).ask, 6);
  EXPECT_EQ(left.at(P4890).ask, 4);
  EXPECT_EQ(left.at(P4890).observation, chain.observation);
  auto preview = session.preview(vertical("more", 5, "-0.30"), chain.time);
  ASSERT_EQ(preview.liquidity.size(), 2U);
  EXPECT_EQ(preview.liquidity[0].symbol, P4900);
  EXPECT_EQ(preview.liquidity[0].side, Side::Sell);
  EXPECT_EQ(preview.liquidity[0].contracts, 5);
  EXPECT_TRUE(preview.liquidity[0].quote.ok());
  EXPECT_EQ(preview.liquidity[0].displayed, 6);
  EXPECT_EQ(preview.liquidity[0].left, 4);
  EXPECT_EQ(preview.liquidity[1].left, 4);
  // A new observation refreshes the budget; a one-sided quote cannot fill.
  ++chain.observation;
  chain.time += md::kNanosPerSecond;
  QuoteObservation one_sided{P4900, chain.observation, chain.time, std::nullopt, m("2.20"), 0, 6};
  QuoteObservation refreshed{P4890, chain.observation, chain.time, m("1.50"), m("1.65"), 6, 6};
  session.on_quotes({one_sided, refreshed}, {}, chain.time);
  EXPECT_TRUE(session.sizes_left().empty());
  preview = session.preview(vertical("again", 1, "-0.30"), chain.time);
  ASSERT_EQ(preview.liquidity.size(), 2U);
  EXPECT_EQ(preview.liquidity[0].quote.code, Reason::INVALID_QUOTE);
  EXPECT_EQ(preview.liquidity[0].quote.message, "No bid: the quote is one-sided");
  EXPECT_EQ(preview.liquidity[0].displayed, 0);
  EXPECT_EQ(preview.liquidity[1].left, 6);
}

/// $1.00 to open and nothing to close, at most $10 a leg; $0.10 clearing, $0.02
/// regulatory and $0.60 on SPXW; $5 for each exercise or assignment.
FeeSchedule schedule() {
  FeeSchedule fees;
  fees.open = m("1.00");
  fees.leg_cap = m("10.00");
  fees.clearing = m("0.10");
  fees.regulatory = m("0.02");
  fees.index = {{"SPXW", m("0.60")}};
  fees.exercise = m("5.00");
  return fees;
}
SessionConfig itemized() {
  auto result = config();
  result.rules.fees = schedule();
  return result;
}

TEST(FeeSchedule, OpeningAndClosingContractsPayTheirOwnRatesAndEveryContractItsFees) {
  ScriptedMarket market;
  TradingSession session(itemized(), market.time);
  market.seed(session, "4.00", "4.20", 10);
  ASSERT_TRUE(session.submit(market.market("open", 3), market.time).decision.ok());
  ASSERT_TRUE(session.submit(market.market("close", 2, Side::Sell), market.time).decision.ok());
  // One contract closes the long and two open a short: only those two pay commission.
  ASSERT_TRUE(session.submit(market.market("reverse", 3, Side::Sell), market.time).decision.ok());
  const auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  ASSERT_TRUE(fills[0].fees && fills[1].fees && fills[2].fees);
  EXPECT_EQ(*fills[0].fees, (FillFees{m("3.00"), m("0.30"), m("0.06"), m("1.80")}));
  EXPECT_EQ(fills[0].fee, m("5.16"));
  EXPECT_EQ(*fills[1].fees, (FillFees{Money{}, m("0.20"), m("0.04"), m("1.20")}));
  EXPECT_EQ(fills[1].fee, m("1.44"));
  EXPECT_EQ(fills[2].fees->commission, m("2.00"));
  EXPECT_EQ(fills[2].fee, m("4.16"));
  EXPECT_EQ(session.snapshot()->account.fees, m("10.76"));
  // The flat fee keeps its bytes: no itemized fees on its fills.
  TradingSession flat(config(), market.time);
  market.seed(flat, "4.00", "4.20", 10);
  ASSERT_TRUE(flat.submit(market.market("open", 3), market.time).decision.ok());
  const auto plain = flat.snapshot()->recent_fills.back();
  EXPECT_FALSE(plain.fees);
  EXPECT_EQ(plain.fee, m("1.95"));
  EXPECT_FALSE(Json(plain).contains("fees"));
  EXPECT_FALSE(Json(flat.config().rules).contains("fees"));
}

TEST(FeeSchedule, ExitEquityChargesTheClosingScheduleWithAFreshCommissionCap) {
  for (const auto side : {Side::Buy, Side::Sell}) {
    for (const Quantity quantity : {3, 12}) {
      ScriptedMarket market;
      auto c = itemized();
      c.initial_cash = m("10000");
      c.rules.fees->close = m("1.25");
      TradingSession session(c, market.time);
      market.seed(session, "4.00", "4.20", 30);
      ASSERT_TRUE(session.submit(market.market("open", quantity, side), market.time).decision.ok());
      const auto snapshot = session.snapshot();
      // A $20 spread per contract, opening fees, then closing commission (capped
      // anew at $10) plus $0.10 clearing, $0.02 regulatory and $0.60 index fees.
      EXPECT_EQ(snapshot->exit_equity, quantity == 3 ? m("9928.93") : m("9722.72"));
      EXPECT_EQ(snapshot->equity - snapshot->exit_equity, quantity == 3 ? m("35.91") : m("138.64"));
      EXPECT_FALSE(Json::parse(session.snapshot_json()).contains("exit_equity"));
      const auto closing = side == Side::Buy ? Side::Sell : Side::Buy;
      ASSERT_TRUE(session.submit(market.market("close", quantity, closing), market.time).decision.ok());
      EXPECT_TRUE(session.snapshot()->positions.empty());
      EXPECT_EQ(session.snapshot()->equity, snapshot->exit_equity);
    }
  }
}

TEST(FeeSchedule, CommissionStopsAtTheLegCapAcrossAnOrdersFills) {
  ScriptedMarket market;
  TradingSession session(itemized(), market.time);
  market.seed(session, "4.00", "4.20", 4);
  const auto order = session.submit(market.limit("cap", 15, "4.20"), market.time);
  ASSERT_TRUE(order.decision.ok());
  for (int i = 0; i < 3; ++i) {
    market.next();
    session.on_quotes({market.quote("4.00", "4.20", 4)}, {market.valuation()}, market.time);
  }
  const auto fills = session.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 4U);
  // 4 + 4 + 2 dollars of commission reach the $10 cap; the last fill pays only fees.
  EXPECT_EQ(fills[0].fees->commission, m("4.00"));
  EXPECT_EQ(fills[1].fees->commission, m("4.00"));
  EXPECT_EQ(fills[2].fees->commission, m("2.00"));
  EXPECT_EQ(fills[3].fees->commission, Money{});
  EXPECT_EQ(fills[3].fee, m("2.16"));
  // The cap is per order: a new order pays commission again.
  market.next();
  session.on_quotes({market.quote("4.00", "4.20", 4)}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("again", 1), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().fees->commission, m("1.00"));
  // A spread caps each leg; a root without an index fee pays none.
  Chain chain;
  TradingSession spread(itemized(), chain.time);
  chain.define(spread, {P4900, P4890});
  chain.quote(spread, {{P4900, "5.00", "5.20"}, {P4890, "4.00", "4.20"}}, 20);
  ASSERT_TRUE(spread.submit(vertical("v", 12, std::nullopt), chain.time).decision.ok());
  const auto legs = spread.snapshot()->recent_fills;
  ASSERT_EQ(legs.size(), 2U);
  for (const auto& leg : legs) {
    EXPECT_EQ(leg.fees->commission, m("10.00"));
    EXPECT_EQ(leg.fee, m("18.64"));
  }
}

TEST(FeeSchedule, PreviewsAndReservationsChargeTheSchedule) {
  ScriptedMarket market;
  auto c = itemized();
  c.rules.buying_power = true;
  TradingSession session(c, market.time);
  market.seed(session, "4.00", "4.20", 20);
  const auto preview = session.preview(market.limit("p", 12, "4.10"), market.time);
  ASSERT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.fee, m("18.64"));
  ASSERT_TRUE(preview.fees);
  EXPECT_EQ(*preview.fees, (FillFees{m("10.00"), m("1.20"), m("0.24"), m("7.20")}));
  // A resting limit reserves its premium and its fees under the schedule.
  ASSERT_TRUE(session.submit(market.limit("rest", 12, "4.10"), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->buying_power.reserved, m("4920") + m("18.64"));
  EXPECT_EQ(preview.buying_power_required, m("4920") + m("18.64"));
}

TEST(FeeSchedule, APartialFillDoesNotReserveCommissionAlreadyPaidTowardTheCap) {
  ScriptedMarket market;
  auto c = itemized();
  c.rules.buying_power = true;
  c.initial_cash = m("6320.80");  // 15 at 4.20, $10 commission and 15 x $0.72 other fees.
  TradingSession session(c, market.time);
  market.seed(session, "4.00", "4.20", 8);
  ASSERT_TRUE(session.submit(market.limit("exact", 15, "4.20"), market.time).decision.ok());
  ASSERT_EQ(session.snapshot()->recent_fills.size(), 1U);
  EXPECT_EQ(session.snapshot()->recent_fills.back().quantity, 8);
  EXPECT_EQ(session.snapshot()->buying_power.available, Money{});
  EXPECT_EQ(session.snapshot()->buying_power.reserved, m("2947.04"));
  market.next();
  session.on_quotes({market.quote("4.00", "4.20", 8)}, {market.valuation()}, market.time);
  ASSERT_EQ(session.snapshot()->recent_fills.size(), 2U);
  EXPECT_EQ(session.snapshot()->account.cash, Money{});
  EXPECT_EQ(session.snapshot()->account.fees, m("20.80"));
}

TEST(FeeSchedule, ExerciseAndDeliveryPayPerContractWhileCashSettlementIsFree) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto expiring = *md::parse_osi("SPY260922C00500000");
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  auto c = itemized();
  TemporaryDirectory directory;
  const auto path = directory.file("delivery.jsonl");
  TradingSession session(c, time, FileJournal::create(path));
  std::uint64_t observation = 0;
  const auto quote = [&](const md::OptionContract& contract, md::Timestamp at) {
    ++observation;
    session.on_quotes({{contract.osi_symbol(), observation, at, m("10.00"), m("10.20"), 10, 10}},
                      {{contract.osi_symbol(), at, 0.6, 0.02, 0.5, -0.05, 510, 510, 0.99,
                        md::years_between(at, contract.expiry_time()), 0.2, true}},
                      at, {{"SPY", at, m("510")}});
  };
  for (const auto& contract : {call, expiring}) ASSERT_TRUE(session.define(contract, time).decision.ok());
  quote(call, time);
  quote(expiring, time);
  const auto buy = [&](std::string client, const md::OptionContract& contract) {
    return OrderRequest{std::move(client), contract.osi_symbol(), Side::Buy, OrderType::Market, TimeInForce::Ioc, 2, {}, {}, {}, {}};
  };
  ASSERT_TRUE(session.submit(buy("early", call), time).decision.ok());
  ASSERT_TRUE(session.submit(buy("expiry", expiring), time).decision.ok());
  const auto before = session.snapshot()->account;
  // Equity options have no index fee: 2 x (1.00 + 0.12) per order.
  EXPECT_EQ(before.fees, m("4.48"));
  ASSERT_TRUE(session.exercise(call.osi_symbol(), 2, time).decision.ok());
  auto closures = session.snapshot()->closures;
  ASSERT_EQ(closures.size(), 1U);
  EXPECT_EQ(closures[0].fee, m("10.00"));
  EXPECT_EQ(session.snapshot()->account.fees, before.fees + m("10.00"));
  // Expiry delivers the shares of the in-the-money call, for $5 per contract.
  session.on_quotes({}, {}, expiring.expiry_time(), {{"SPY", expiring.expiry_time(), m("510")}});
  ASSERT_TRUE(session.settle(expiring.osi_symbol(), m("510"), expiring.expiry_time()).decision.ok());
  closures = session.snapshot()->closures;
  ASSERT_EQ(closures.size(), 2U);
  EXPECT_EQ(closures[1].kind, ClosureKind::Settlement);
  EXPECT_EQ(closures[1].fee, m("10.00"));
  EXPECT_EQ(session.snapshot()->account.fees, before.fees + m("20.00"));
  // The trades view charges each fee to the option trade it closed.
  const auto trades = lifecycles(session.snapshot()->recent_fills, closures, session.contracts());
  Money fees;
  for (const auto& trade : trades) fees = fees + trade.fees;
  EXPECT_EQ(fees, before.fees + m("20.00"));
  EXPECT_EQ(Json(closures[0]).get<Closure>().fee, m("10.00"));
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), session.snapshot_json());
  EXPECT_NEAR(session.snapshot()->attribution.total(), (session.snapshot()->equity - session.snapshot()->start_of_day_equity).dollars(), 1e-6);

  // An index option settles in cash, with no fee.
  ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  TradingSession index(itemized(), market.time);
  market.seed(index, "4.00", "4.20", 10);
  ASSERT_TRUE(index.submit(market.market("index", 1), market.time).decision.ok());
  const auto paid = index.snapshot()->account.fees;
  index.on_quotes({}, {}, market.contract.expiry_time());
  ASSERT_TRUE(index.settle(market.symbol(), m("5010"), market.contract.expiry_time()).decision.ok());
  EXPECT_EQ(index.snapshot()->closures.back().fee, Money{});
  EXPECT_EQ(index.snapshot()->account.fees, paid);
  EXPECT_FALSE(Json(index.snapshot()->closures.back()).contains("fee"));
}

TEST(FeeSchedule, NonzeroCloseRatesUncappedCommissionAndRootLookupStayExact) {
  ScriptedMarket market;
  auto c = itemized();
  c.rules.fees->close = m("0.125001");
  c.rules.fees->leg_cap = {};
  c.rules.fees->index = {{"SPX", m("0.60")}, {"SPXW", m("0.700001")}};
  TradingSession session(c, market.time);
  market.seed(session, "4.00", "4.20", 30);
  ASSERT_TRUE(session.submit(market.market("open", 12), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().fees->commission, m("12"));
  EXPECT_EQ(session.snapshot()->recent_fills.back().fees->index, m("8.400012"));
  const auto close = session.preview(market.market("close", 12, Side::Sell), market.time);
  EXPECT_EQ(close.fees->commission, m("1.500012"));
  ASSERT_TRUE(session.submit(market.market("close", 12, Side::Sell), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().fee, close.fee);
  market.contract = *md::parse_osi("SPX261016C05000000");
  market.seed(session, "4.00", "4.20", 10);
  ASSERT_TRUE(session.submit(market.market("monthly", 1), market.time).decision.ok());
  EXPECT_EQ(session.snapshot()->recent_fills.back().fees->index, m("0.60"));
}

TEST(FeeSchedule, AChangePreviewUsesTheCommissionAlreadyPaidAndRecoveryContinuesIdentically) {
  TemporaryDirectory directory;
  ScriptedMarket market;
  const auto path = directory.file("resumed.jsonl");
  TradingSession continuous(itemized(), market.time, FileJournal::create(directory.file("whole.jsonl")));
  auto resumed = std::make_unique<TradingSession>(itemized(), market.time, FileJournal::create(path));
  for (auto* s : {&continuous, resumed.get()}) {
    market.seed(*s, "4.00", "4.20", 8);
    ASSERT_TRUE(s->submit(market.limit("cap", 15, "4.20"), market.time).decision.ok());
  }
  EXPECT_EQ(FileJournal::read(path).head, FileJournal::read(directory.file("whole.jsonl")).head);
  resumed.reset();
  resumed = std::make_unique<TradingSession>(TradingSession::recover(FileJournal::read(path), FileJournal::resume(path)));
  // The remaining seven contracts pay only $2 commission, plus $5.04 in other fees.
  const auto before = resumed->snapshot_json();
  OrderChange change;
  change.quantity = 15;
  const auto preview = resumed->preview_change(1, change, market.time);
  ASSERT_TRUE(preview.fees);
  EXPECT_EQ(preview.fees->commission, m("2.00"));
  EXPECT_EQ(preview.fee, m("7.04"));
  EXPECT_EQ(resumed->snapshot_json(), before);
  market.next();
  for (auto* s : {&continuous, resumed.get()})
    s->on_quotes({market.quote("4.00", "4.20", 10)}, {market.valuation()}, market.time);
  EXPECT_EQ(continuous.snapshot_json(), resumed->snapshot_json());
  // Recovery starts a checkpoint instead of a delta; expanded records still agree byte for byte.
  const auto expanded = FileJournal::create(directory.file("expanded.jsonl"));
  const auto whole = FileJournal::create(directory.file("whole-expanded.jsonl"));
  TradingSession::expand(FileJournal::read(path), *expanded);
  TradingSession::expand(FileJournal::read(directory.file("whole.jsonl")), *whole);
  EXPECT_EQ(expanded->head(), whole->head());
  EXPECT_EQ(resumed->snapshot()->recent_fills.back().fee, preview.fee);
}

TEST(FeeSchedule, ImpactSlicesShareTheCapAndClosingReservationsUseTheCloseRate) {
  Chain chain;
  auto c = itemized();
  c.rules.impact_ticks = 1;
  c.rules.fees->leg_cap = m("1.50");
  TradingSession session(c, chain.time);
  chain.define(session, {P4900, P4890});
  chain.quote(session, {{P4900, "5.00", "5.20"}, {P4890, "4.00", "4.20"}}, 2);
  const auto preview = session.preview(vertical("v", 5, std::nullopt), chain.time);
  ASSERT_TRUE(session.submit(vertical("v", 5, std::nullopt), chain.time).decision.ok());
  Money total, commission;
  for (const auto& fill : session.snapshot()->recent_fills) {
    total = total + fill.fee;
    commission = commission + fill.fees->commission;
  }
  EXPECT_EQ(commission, m("3.00"));
  EXPECT_EQ(total, m("10.20"));
  EXPECT_EQ(total, preview.fee);
  auto close = vertical("close", 5, "1.50");
  for (auto& leg : close.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
  const auto closing = session.preview(close, chain.time);
  ASSERT_TRUE(closing.fees);
  EXPECT_EQ(closing.fees->commission, Money{});
  EXPECT_EQ(closing.fee, m("7.20"));
}

TEST(FeeSchedule, AllScheduleBoundsAreValidatedOnResetWithoutChangingTheAccount) {
  ScriptedMarket market;
  TradingSession session(config(), market.time);
  const auto before = session.snapshot_json();
  for (auto member : {&FeeSchedule::open, &FeeSchedule::close, &FeeSchedule::leg_cap, &FeeSchedule::clearing,
                      &FeeSchedule::regulatory, &FeeSchedule::exercise}) {
    for (const auto invalid : {m("-0.000001"), m("1000.000001")}) {
      auto rules = itemized().rules;
      (*rules.fees).*member = invalid;
      EXPECT_THROW(session.reset_account(m("100000"), rules, "invalid", market.time), TradingError);
      EXPECT_EQ(session.snapshot_json(), before);
    }
  }
  for (const auto& root : {"", "spx", "SPX W", "TOOLONG", "SPX_"}) {
    auto rules = itemized().rules;
    rules.fees->index[root] = m("0.10");
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
  auto rules = itemized().rules;
  rules.fees->index.clear();
  for (int i = 0; i < 16; ++i) rules.fees->index["R" + std::to_string(i)] = m("1000");
  EXPECT_NO_THROW(validate_rules(rules));
  rules.fees->index["R16"] = {};
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST(FeeSchedule, AnUnknownComboLegKeepsItsReasonWhenPreviewed) {
  for (const auto& c : {config(), itemized()}) {
    Chain chain;
    TradingSession session(c, chain.time);
    chain.define(session, {P4900});
    chain.quote(session, {{P4900, "5.00", "5.20"}});
    const auto before = session.snapshot_json();
    const auto preview = session.preview(vertical("unknown", 1, "-0.50"), chain.time);
    EXPECT_EQ(preview.decision.code, Reason::UNKNOWN_CONTRACT);
    EXPECT_FALSE(preview.fee);
    EXPECT_EQ(session.snapshot_json(), before);
  }
}

TEST(FeeSchedule, ItemizedFeesSurviveRecoveryAndAreValidated) {
  TemporaryDirectory directory;
  const auto path = directory.file("journal.jsonl");
  ScriptedMarket market;
  std::string expected;
  {
    TradingSession session(itemized(), market.time, FileJournal::create(path));
    market.seed(session, "4.00", "4.20", 4);
    ASSERT_TRUE(session.submit(market.limit("cap", 15, "4.20"), market.time).decision.ok());
    market.next();
    session.on_quotes({market.quote("4.00", "4.20", 4)}, {market.valuation()}, market.time);
    expected = session.snapshot_json();
  }
  const auto recovered = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  ASSERT_TRUE(recovered.config().rules.fees);
  EXPECT_EQ(*recovered.config().rules.fees, schedule());
  ASSERT_TRUE(recovered.snapshot()->recent_fills.back().fees);
  auto rules = itemized().rules;
  rules.fees->clearing = m("-0.01");
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules = itemized().rules;
  rules.fees->index[""] = m("0.10");
  EXPECT_THROW(validate_rules(rules), TradingError);
}

}  // namespace
}  // namespace openport::trading
