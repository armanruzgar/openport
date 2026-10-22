#include <gtest/gtest.h>

#include "openport/trading/session.hpp"
#include "support/recording.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view value) { return Money::parse(value); }
Timestamp ny(int hour, int minute = 0, md::Date day = {2026, 9, 22}) { return md::new_york_to_utc(day, hour, minute); }
SessionConfig config() {
  SessionConfig c;
  c.limits.aggregate = {1e12, 1e12}; c.limits.per_underlying = {1e12, 1e12};
  return c;
}
StockOrderRequest order(std::string id, Side side, StockOrderType type, Quantity shares,
                        std::optional<std::string_view> limit = {}, std::optional<std::string_view> stop = {}) {
  StockOrderRequest r;
  r.client_order_id = std::move(id);
  r.symbol = "SPY";
  r.side = side;
  r.type = type;
  r.shares = shares;
  if (limit) r.limit_price = m(*limit);
  if (stop) r.stop_price = m(*stop);
  return r;
}
CommandResult price(TradingSession& s, Timestamp time, std::string_view value) {
  return s.on_quotes({}, {}, time, {StockPrice{"SPY", time, m(value)}});
}
const StockOrder& stock_order(TradingSession& s, OrderId id) { return s.snapshot()->stock_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(StockOrders, RestingLimitsWaitForALaterPriceAndFillAtIt) {
  test::RecordingFile file;
  auto path = (file.directory / "shares.jsonl").string();
  const auto open = ny(10);
  TradingSession s(config(), open, FileJournal::create(path));
  // Below the price, a buy limit rests; it fills on the first later print at or under it.
  const auto placed = s.submit_stock_order(order("dip", Side::Buy, StockOrderType::Limit, 100, "495.00"), open,
                                           StockPrice{"SPY", open, m("500")});
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  ASSERT_EQ(placed.order_id, 1u);
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Working);
  EXPECT_TRUE(s.snapshot()->stocks.empty());
  ASSERT_TRUE(price(s, ny(10, 5), "496").decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Working);
  ASSERT_TRUE(price(s, ny(10, 10), "494.50").decision.ok());
  const auto& filled = stock_order(s, 1);
  EXPECT_EQ(filled.status, OrderStatus::Filled);
  EXPECT_EQ(filled.fill_price, m("494.50"));  // the print, better than the limit
  EXPECT_EQ(filled.filled_shares, 100);
  ASSERT_EQ(s.snapshot()->stock_fills.size(), 1u);
  EXPECT_EQ(s.snapshot()->stock_fills[0].stock_order, 1u);
  EXPECT_EQ(s.snapshot()->stocks.at(0).position.shares, 100);
  // A sell limit the price already crosses fills at once.
  const auto marketable = s.submit_stock_order(order("out", Side::Sell, StockOrderType::Limit, 40, "490.00"), ny(10, 11));
  ASSERT_TRUE(marketable.decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 2).fill_price, m("494.50"));
  EXPECT_EQ(s.snapshot()->stocks.at(0).position.shares, 60);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), s.snapshot_json());
}

TEST(StockOrders, RetriesAreAnsweredOnceAndOtherTermsRefused) {
  const auto open = ny(10);
  TradingSession s(config(), open);
  const auto request = order("one", Side::Buy, StockOrderType::Limit, 10, "400.00");
  ASSERT_TRUE(s.submit_stock_order(request, open, StockPrice{"SPY", open, m("500")}).decision.ok());
  const auto version = s.snapshot()->account_version;
  const auto retry = s.submit_stock_order(request, open);
  EXPECT_TRUE(retry.replayed);
  EXPECT_EQ(retry.order_id, 1u);
  EXPECT_EQ(s.snapshot()->account_version, version);
  auto other = request;
  other.shares = 11;
  EXPECT_EQ(s.submit_stock_order(other, open).decision.code, Reason::DUPLICATE_CLIENT_ID);
  EXPECT_EQ(s.snapshot()->stock_orders.size(), 1u);
}

TEST(StockOrders, InvalidTermsAreRefusedBeforeAnythingIsRecorded) {
  const auto open = ny(10);
  TradingSession s(config(), open);
  for (auto bad : {order("a", Side::Buy, StockOrderType::Limit, 10),                    // no limit
                   order("b", Side::Buy, StockOrderType::Market, 10, "1.00"),            // a limit on a market order
                   order("c", Side::Buy, StockOrderType::Stop, 10),                      // no stop
                   order("d", Side::Buy, StockOrderType::Limit, 0, "1.00"),              // no shares
                   order("e", Side::Buy, StockOrderType::Limit, 10, "1.005")}) {          // a fraction of a cent
    EXPECT_THROW(s.submit_stock_order(bad, open), TradingError) << bad.client_order_id;
  }
  auto extended = order("f", Side::Buy, StockOrderType::Stop, 10, std::nullopt, "500.00");
  extended.extended_hours = true;
  EXPECT_THROW(s.submit_stock_order(extended, open), TradingError);
  auto bracket = order("g", Side::Buy, StockOrderType::Limit, 10, "500.00");
  bracket.stop_loss = m("510");
  EXPECT_THROW(s.submit_stock_order(bracket, open), TradingError);
  auto gtd = order("h", Side::Buy, StockOrderType::Limit, 10, "500.00");
  gtd.tif = TimeInForce::Gtd;
  EXPECT_THROW(s.submit_stock_order(gtd, open), TradingError);
  EXPECT_TRUE(s.snapshot()->stock_orders.empty());
}

TEST(StockOrders, StopsActivateOnThePriceAndStopLimitsThenRest) {
  const auto open = ny(10);
  TradingSession s(config(), open);
  ASSERT_TRUE(s.trade_stock("SPY", 200, open, StockPrice{"SPY", open, m("500")}).decision.ok());
  ASSERT_TRUE(s.submit_stock_order(order("stop", Side::Sell, StockOrderType::Stop, 100, std::nullopt, "490.00"), open).decision.ok());
  ASSERT_TRUE(s.submit_stock_order(order("stoplimit", Side::Sell, StockOrderType::StopLimit, 100, "489.00", "490.00"), open).decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Armed);
  ASSERT_TRUE(price(s, ny(10, 1), "491").decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Armed);
  // A gap through both stops: the stop sells at the print; the stop-limit activates but
  // its limit is above the print, so it waits.
  ASSERT_TRUE(price(s, ny(10, 2), "485").decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 1).fill_price, m("485"));
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(stock_order(s, 2).triggered_at, ny(10, 2));
  ASSERT_TRUE(price(s, ny(10, 3), "489.25").decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 2).fill_price, m("489.25"));
  EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(StockOrders, BracketsProtectTheFilledEntryAndOneExitCancelsTheOther) {
  test::RecordingFile file;
  auto path = (file.directory / "bracket.jsonl").string();
  const auto open = ny(10);
  TradingSession s(config(), open, FileJournal::create(path));
  auto entry = order("entry", Side::Buy, StockOrderType::Limit, 50, "500.00");
  entry.stop_loss = m("490");
  entry.take_profit = m("520");
  ASSERT_TRUE(s.submit_stock_order(entry, open, StockPrice{"SPY", open, m("499")}).decision.ok());
  const auto& filled = stock_order(s, 1);
  ASSERT_EQ(filled.status, OrderStatus::Filled);
  ASSERT_EQ(filled.stop_loss, 2u);
  ASSERT_EQ(filled.take_profit, 3u);
  EXPECT_EQ(stock_order(s, 2).role, OrderRole::StopLoss);
  EXPECT_EQ(stock_order(s, 2).request.side, Side::Sell);
  EXPECT_EQ(stock_order(s, 2).request.shares, 50);
  EXPECT_EQ(stock_order(s, 2).request.client_order_id, "entry:stop");
  EXPECT_EQ(stock_order(s, 3).request.limit_price, m("520"));
  EXPECT_EQ(stock_order(s, 3).request.tif, TimeInForce::Gtc);
  // Exits reserve nothing: they only reduce.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, Money{});
  ASSERT_TRUE(price(s, ny(11), "521").decision.ok());
  EXPECT_EQ(stock_order(s, 3).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(stock_order(s, 2).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->stocks.empty());
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), s.snapshot_json());
}

TEST(StockOrders, AnExitEndsWhenItsSharesAreClosedAnotherWay) {
  const auto open = ny(10);
  TradingSession s(config(), open);
  auto entry = order("entry", Side::Buy, StockOrderType::Market, 30);
  entry.stop_loss = m("480");
  ASSERT_TRUE(s.submit_stock_order(entry, open, StockPrice{"SPY", open, m("500")}).decision.ok());
  ASSERT_EQ(stock_order(s, 1).status, OrderStatus::Filled);
  ASSERT_TRUE(s.trade_stock("SPY", -30, ny(10, 1)).decision.ok());
  ASSERT_TRUE(price(s, ny(10, 2), "501").decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(stock_order(s, 2).reason.code, Reason::POSITION_CLOSED);
}

TEST(StockOrders, OneCancelsOther) {
  const auto open = ny(10);
  TradingSession s(config(), open);
  ASSERT_TRUE(s.trade_stock("SPY", 100, open, StockPrice{"SPY", open, m("500")}).decision.ok());
  auto target = order("pair", Side::Sell, StockOrderType::Limit, 100, "510.00");
  target.oco.push_back(order("", Side::Sell, StockOrderType::Stop, 100, std::nullopt, "495.00"));
  ASSERT_TRUE(s.submit_stock_order(target, open).decision.ok());
  ASSERT_EQ(s.snapshot()->stock_orders.size(), 2u);
  EXPECT_EQ(stock_order(s, 2).request.client_order_id, "pair:oco");
  EXPECT_EQ(stock_order(s, 1).oco, 2u);
  ASSERT_TRUE(price(s, ny(10, 30), "494").decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Cancelled);
  EXPECT_EQ(stock_order(s, 1).reason.code, Reason::OCO_FILLED);
  // The retry of the pair is answered as first accepted.
  EXPECT_TRUE(s.submit_stock_order(target, ny(10, 31)).replayed);
}

TEST(StockOrders, ExtendedHoursLimitsWorkPremarketAndDayOrdersEndWithTheirSession) {
  const auto early = ny(7);
  TradingSession s(config(), early);
  auto pre = order("pre", Side::Buy, StockOrderType::Limit, 10, "500.00");
  pre.extended_hours = true;
  ASSERT_TRUE(s.submit_stock_order(pre, early, StockPrice{"SPY", early, m("502")}).decision.ok());
  ASSERT_TRUE(s.submit_stock_order(order("regular", Side::Buy, StockOrderType::Limit, 10, "500.00"), early).decision.ok());
  ASSERT_TRUE(price(s, ny(7, 30), "499.50").decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Filled);  // premarket
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Working);  // waits for the open
  EXPECT_EQ(stock_order(s, 2).day_end, ny(16));
  ASSERT_TRUE(price(s, ny(9, 31), "499.80").decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Filled);
  // An after-hours limit lasts until 20:00; a regular DAY limit ends at the close.
  auto late = order("late", Side::Sell, StockOrderType::Limit, 5, "530.00");
  late.extended_hours = true;
  ASSERT_TRUE(s.submit_stock_order(late, ny(15)).decision.ok());
  ASSERT_TRUE(s.submit_stock_order(order("day", Side::Sell, StockOrderType::Limit, 5, "530.00"), ny(15)).decision.ok());
  EXPECT_EQ(stock_order(s, 3).day_end, ny(20));
  ASSERT_TRUE(price(s, ny(16, 30), "531").decision.ok());
  EXPECT_EQ(stock_order(s, 4).status, OrderStatus::Cancelled);
  EXPECT_EQ(stock_order(s, 4).reason.code, Reason::DAY_END);
  EXPECT_EQ(stock_order(s, 3).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 3).fill_price, m("531"));
  // GTC waits across days.
  ASSERT_TRUE(s.submit_stock_order([] { auto r = order("gtc", Side::Buy, StockOrderType::Limit, 1, "400.00"); r.tif = TimeInForce::Gtc; return r; }(),
                                   ny(16, 31)).decision.ok());
  ASSERT_TRUE(s.on_quotes({}, {}, ny(11, 0, {2026, 9, 23})).decision.ok());
  EXPECT_TRUE(stock_order(s, 5).open());
}

TEST(StockOrders, ExtendedHoursOrdersFillOnlyOnPricesFreshInTheirSession) {
  auto after_hours = [](std::string id, std::string_view limit) {
    auto r = order(std::move(id), Side::Buy, StockOrderType::Limit, 5, limit);
    r.extended_hours = true;
    return r;
  };
  // At 18:30 the regular close is hours old: an after-hours buy does not fill on it.
  TradingSession s(config(), ny(15, 59));
  ASSERT_TRUE(price(s, ny(15, 59), "531").decision.ok());
  ASSERT_TRUE(s.submit_stock_order(after_hours("late", "532.00"), ny(18, 30)).decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Working);
  // A fresh after-hours print fills it.
  ASSERT_TRUE(price(s, ny(18, 31), "531.50").decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(stock_order(s, 1).fill_price, m("531.50"));
  // Nor on a 16:05 print, once it is stale within after hours.
  TradingSession t(config(), ny(15, 59));
  ASSERT_TRUE(price(t, ny(16, 5), "529").decision.ok());
  ASSERT_TRUE(t.submit_stock_order(after_hours("print", "530.00"), ny(16, 5)).decision.ok());
  EXPECT_EQ(stock_order(t, 1).status, OrderStatus::Filled);  // fresh at 16:05
  ASSERT_TRUE(t.submit_stock_order(after_hours("stale", "530.00"), ny(18, 30)).decision.ok());
  EXPECT_EQ(stock_order(t, 2).status, OrderStatus::Working);
}

TEST(StockOrders, WorkingOpeningOrdersReserveBuyingPower) {
  auto c = config();
  c.rules.buying_power = true;
  const auto open = ny(10);
  TradingSession s(c, open);
  ASSERT_TRUE(s.submit_stock_order(order("big", Side::Buy, StockOrderType::Limit, 150, "490.00"), open,
                                   StockPrice{"SPY", open, m("500")}).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("73500"));
  EXPECT_EQ(s.snapshot()->buying_power.available, m("26500"));
  // Another order that the rest cannot pay for is refused at acceptance.
  const auto refused = s.submit_stock_order(order("more", Side::Buy, StockOrderType::Limit, 100, "490.00"), open);
  EXPECT_EQ(refused.decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(s.snapshot()->stock_orders.size(), 1u);
  // A preview shows what it would reserve, without recording.
  const auto preview = s.preview_stock_order(order("small", Side::Buy, StockOrderType::Limit, 10, "490.00"), open);
  EXPECT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.after.buying_power, m("21600"));
  EXPECT_EQ(s.snapshot()->stock_orders.size(), 1u);
  ASSERT_TRUE(s.cancel_stock_order(1, open).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, Money{});
  EXPECT_EQ(s.cancel_stock_order(1, open).decision.code, Reason::ORDER_TERMINAL);
  EXPECT_EQ(s.cancel_stock_order(9, open).decision.code, Reason::UNKNOWN_ORDER);
}

TEST(StockOrders, StopAndMarketOrdersReserveAtAcceptance) {
  auto c = config();
  c.rules.buying_power = true;
  const auto open = ny(10);
  TradingSession s(c, open);
  // With no price yet, a buy stop reserves at its stop.
  ASSERT_TRUE(s.submit_stock_order(order("stop", Side::Buy, StockOrderType::Stop, 100, std::nullopt, "510.00"), open).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("51000"));
  // A market order has no price to reserve against until one is known.
  const auto unpriced = s.submit_stock_order(order("mkt", Side::Buy, StockOrderType::Market, 50), open);
  EXPECT_EQ(unpriced.decision.code, Reason::STALE_QUOTE);
  EXPECT_EQ(s.snapshot()->stock_orders.size(), 1u);
  // Once priced, a market order outside the session rests reserving the last price.
  const auto early = ny(8);
  TradingSession t(c, early);
  ASSERT_TRUE(t.submit_stock_order(order("mkt", Side::Buy, StockOrderType::Market, 50), early,
                                   StockPrice{"SPY", early, m("500")}).decision.ok());
  EXPECT_EQ(stock_order(t, 1).status, OrderStatus::Working);
  EXPECT_EQ(t.snapshot()->buying_power.reserved, m("25000"));
  // Another the rest cannot pay for is refused.
  EXPECT_EQ(t.submit_stock_order(order("more", Side::Buy, StockOrderType::Stop, 160, std::nullopt, "505.00"), early).decision.code,
            Reason::BUYING_POWER);
}

TEST(StockOrders, AccountAndPlanRulesSeeWorkingSells) {
  const auto open = ny(10);
  auto cash = config();
  cash.rules.account_type = AccountType::Cash;
  cash.rules.buying_power = true;
  TradingSession s(cash, open);
  ASSERT_TRUE(s.trade_stock("SPY", 100, open, StockPrice{"SPY", open, m("500")}).decision.ok());
  ASSERT_TRUE(s.submit_stock_order(order("sell", Side::Sell, StockOrderType::Limit, 100, "510.00"), open).decision.ok());
  // With that sell filled, another would leave shares short.
  EXPECT_EQ(s.submit_stock_order(order("more", Side::Sell, StockOrderType::Limit, 50, "520.00"), open).decision.code, Reason::ACCOUNT_TYPE);
  auto buy_only = config();
  buy_only.rules.buy_only = true;
  TradingSession b(buy_only, open);
  EXPECT_EQ(b.submit_stock_order(order("short", Side::Sell, StockOrderType::Limit, 10, "510.00"), open,
                                 StockPrice{"SPY", open, m("500")}).decision.code, Reason::BUY_ONLY);
  EXPECT_EQ(b.submit_stock_order([] { auto r = order("idx", Side::Buy, StockOrderType::Market, 1); r.symbol = "SPX"; return r; }(), open).decision.code,
            Reason::INVALID_ORDER);
}

TEST(StockOrders, AStopLossPlanTakesBracketedShareEntriesAndTheirRiskToTheStop) {
  auto c = config();
  c.rules.require_stop_loss = true;
  c.rules.max_trade_risk = m("1000");
  const auto open = ny(10);
  TradingSession s(c, open);
  EXPECT_EQ(s.submit_stock_order(order("naked", Side::Buy, StockOrderType::Market, 100), open,
                                 StockPrice{"SPY", open, m("500")}).decision.code, Reason::STOP_REQUIRED);
  auto wide = order("wide", Side::Buy, StockOrderType::Market, 100);
  wide.stop_loss = m("480");  // $20 a share: $2,000 at risk
  const auto refused = s.submit_stock_order(wide, open);
  ASSERT_EQ(refused.order_id, 1u);
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Rejected);
  EXPECT_EQ(stock_order(s, 1).reason.code, Reason::MAX_TRADE_RISK);
  auto tight = order("tight", Side::Buy, StockOrderType::Market, 100);
  tight.stop_loss = m("495");
  ASSERT_TRUE(s.submit_stock_order(tight, open).decision.ok());
  EXPECT_EQ(stock_order(s, 2).status, OrderStatus::Filled);
  // The required stop cannot be cancelled while it protects shares.
  EXPECT_EQ(s.cancel_stock_order(3, open).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.cancel_all(std::nullopt, open).decision.code, Reason::STOP_REQUIRED);
}

TEST(StockOrders, MarketOrdersWaitForTheOpenAndResetsCancelThem) {
  const auto early = ny(8);
  TradingSession s(config(), early);
  ASSERT_TRUE(s.submit_stock_order(order("mkt", Side::Buy, StockOrderType::Market, 5), early,
                                   StockPrice{"SPY", early, m("500")}).decision.ok());
  EXPECT_EQ(stock_order(s, 1).status, OrderStatus::Working);
  ASSERT_TRUE(price(s, ny(9, 30), "503").decision.ok());
  EXPECT_EQ(stock_order(s, 1).fill_price, m("503"));
  ASSERT_TRUE(s.submit_stock_order(order("rest", Side::Buy, StockOrderType::Limit, 5, "450.00"), ny(9, 31)).decision.ok());
  ASSERT_TRUE(s.cancel_all(std::string("SPY"), ny(9, 32)).decision.ok());
  EXPECT_EQ(stock_order(s, 2).reason.code, Reason::USER_CANCEL);
  ASSERT_TRUE(s.submit_stock_order(order("again", Side::Buy, StockOrderType::Limit, 5, "450.00"), ny(9, 33)).decision.ok());
  ASSERT_TRUE(s.reset_account(m("100000"), {}, "fresh", ny(9, 34)).decision.ok());
  EXPECT_EQ(stock_order(s, 3).reason.code, Reason::ACCOUNT_RESET);
}
}  // namespace
}  // namespace openport::trading
