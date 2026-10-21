#include <gtest/gtest.h>
#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
TEST(ExposureLimits, GammaThetaAndGridReserveIndependentPendingOrdersAndAllowReductions) {
  test::ScriptedMarket f;
  Ledger ledger; ledger.fill(f.contract, 1, Money::parse("4"), {});
  Contracts contracts{{f.symbol(), f.contract}};
  Valuations values{{f.symbol(), f.valuation()}};
  Limits limits; limits.aggregate = {1e9, 1e9}; limits.per_underlying = {1e9, 1e9};
  const auto original = portfolio_risk(ledger, {}, contracts, values, limits, f.time);
  limits.aggregate.dollar_gamma_1pct = std::abs(original.aggregate.position.dollar_gamma_1pct) / 2;
  limits.aggregate.theta = std::abs(original.aggregate.position.theta) / 2;
  limits.aggregate.worst_grid_loss = 1;
  Order order; order.id = 1; order.request = f.limit("pending", 1, "3"); order.status = OrderStatus::Working;
  const auto held = portfolio_risk(ledger, {}, contracts, values, limits, f.time);
  const auto more = portfolio_risk(ledger, {&order}, contracts, values, limits, f.time);
  EXPECT_EQ(check_exposure(more, held).code, Reason::GAMMA_LIMIT);
  ASSERT_TRUE(more.aggregate.worst_grid_loss); ASSERT_TRUE(held.aggregate.worst_grid_loss);
  EXPECT_NEAR(*more.aggregate.worst_grid_loss, 2 * *held.aggregate.worst_grid_loss, 1e-8);
  order.request.side = Side::Sell;
  const auto close = portfolio_risk(ledger, {&order}, contracts, values, limits, f.time);
  EXPECT_TRUE(check_exposure(close, held).ok());
  EXPECT_EQ(close.aggregate.short_vega, 0);
  limits.aggregate.dollar_gamma_1pct.reset();
  order.request.side = Side::Buy;
  EXPECT_EQ(check_exposure(portfolio_risk(ledger, {&order}, contracts, values, limits, f.time),
      portfolio_risk(ledger, {}, contracts, values, limits, f.time)).code, Reason::THETA_LIMIT);
  limits.aggregate.theta.reset();
  EXPECT_EQ(check_exposure(portfolio_risk(ledger, {&order}, contracts, values, limits, f.time)).code, Reason::GRID_LOSS_LIMIT);
}
TEST(ExposureLimits, ShortVegaIsGrossByContractAndCombosReserveNetGamma) {
  test::ScriptedMarket f, wing; wing.contract.strike += 10;
  Valuations values{{f.symbol(), f.valuation()}, {wing.symbol(), wing.valuation()}};
  Contracts contracts{{f.symbol(), f.contract}, {wing.symbol(), wing.contract}};
  Ledger ledger; ledger.fill(f.contract, -1, Money::parse("4"), {}); ledger.fill(wing.contract, 1, Money::parse("3"), {});
  Limits limits; limits.aggregate = {1e9, 1e9}; limits.per_underlying = {1e9, 1e9};
  const auto risk = portfolio_risk(ledger, {}, contracts, values, limits, f.time);
  EXPECT_EQ(risk.aggregate.position.vega, 0);
  EXPECT_GT(risk.aggregate.short_vega, 0);
  limits.aggregate.short_vega = risk.aggregate.short_vega / 2;
  EXPECT_EQ(check_exposure(portfolio_risk(ledger, {}, contracts, values, limits, f.time)).code, Reason::SHORT_VEGA_LIMIT);
  Order combo; combo.status = OrderStatus::Working; combo.request.quantity = 1;
  combo.request.legs = {{f.symbol(), Side::Buy, 1}, {wing.symbol(), Side::Sell, 1}};
  limits.aggregate.dollar_gamma_1pct = 0; limits.aggregate.short_vega.reset();
  const auto atomic = portfolio_risk(Ledger{}, {&combo}, contracts, values, limits, f.time);
  EXPECT_EQ(atomic.aggregate.reachable.gamma_low, 0); EXPECT_EQ(atomic.aggregate.reachable.gamma_high, 0);
}
TEST(ExposureLimits, ScopedTagsIncludeHeldAndPendingExposureAndPlaybookVersions) {
  test::ScriptedMarket f;
  SessionConfig config; config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  ExposureLimits caps{1e9, 1e9}; caps.dollar_gamma_1pct = 1e9;
  config.limits.tag_overrides["hedge"] = caps;
  config.limits.playbook_overrides["weekly"] = caps;
  TradingSession session(config, f.time); f.seed(session);
  auto entry = f.market("entry", 1); entry.tags = {"hedge", "playbook:weekly@v2"};
  ASSERT_TRUE(session.submit(entry, f.time).decision.ok());
  auto risk = session.snapshot()->risk;
  ASSERT_TRUE(risk.scopes.contains("tag:hedge")); ASSERT_TRUE(risk.scopes.contains("playbook:weekly"));
  EXPECT_EQ(risk.scopes.at("tag:hedge").position.dollar_delta, risk.aggregate.position.dollar_delta);
  auto limits = config.limits;
  limits.tag_overrides["hedge"].dollar_gamma_1pct = 0;
  ASSERT_TRUE(session.set_limits(limits, f.time).decision.ok());
  const auto more = session.submit(f.market("more", 1), f.time);
  EXPECT_EQ(more.decision.code, Reason::GAMMA_LIMIT); EXPECT_EQ(more.decision.scope, "tag:hedge");
  EXPECT_TRUE(session.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
}
TEST(ExposureLimits, AddingATagReservesTheExistingFungiblePositionBeforeTheFill) {
  test::ScriptedMarket f;
  Ledger ledger; ledger.fill(f.contract, 2, Money::parse("4"), {});
  Contracts contracts{{f.symbol(), f.contract}};
  Valuations values{{f.symbol(), f.valuation()}};
  Limits limits; limits.aggregate = {1e9, 1e9}; limits.per_underlying = {1e9, 1e9};
  limits.tag_overrides["hedge"] = {300000, 1e9};
  const auto before = portfolio_risk(ledger, {}, contracts, values, limits, f.time);
  EXPECT_EQ(before.scopes.at("tag:hedge").position.dollar_delta, 0);
  Order order; order.status = OrderStatus::Working; order.request = f.limit("tagged", 1);
  order.request.tags = {"hedge"};
  const auto after = portfolio_risk(ledger, {&order}, contracts, values, limits, f.time);
  EXPECT_EQ(after.scopes.at("tag:hedge").position.dollar_delta, 500000);
  EXPECT_EQ(after.scopes.at("tag:hedge").reachable.delta_high, 750000);
  const auto decision = check_exposure(after, before);
  EXPECT_EQ(decision.code, Reason::DELTA_LIMIT); EXPECT_EQ(decision.scope, "tag:hedge");
  // Valued over the same positions, the add still raises the scope; a tagged sell lowers it.
  const auto scoped = portfolio_risk(ledger, {}, contracts, values, limits, f.time, {}, {}, {}, {&order});
  EXPECT_EQ(scoped.scopes.at("tag:hedge").position.dollar_delta, 500000);
  EXPECT_EQ(check_exposure(after, scoped).code, Reason::DELTA_LIMIT);
  order.request.side = Side::Sell;
  const auto trim = portfolio_risk(ledger, {&order}, contracts, values, limits, f.time);
  EXPECT_EQ(check_exposure(trim, before).code, Reason::DELTA_LIMIT);
  EXPECT_TRUE(check_exposure(trim, portfolio_risk(ledger, {}, contracts, values, limits, f.time, {}, {}, {}, {&order})).ok());
}
TEST(ExposureLimits, ATaggedCloseOfAnUntaggedBookOverItsScopeCapPasses) {
  test::ScriptedMarket f;
  SessionConfig config; config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  ExposureLimits caps{1e9, 1e9}; caps.theta = 1e9;
  config.limits.tag_overrides["hedge"] = caps;
  TradingSession session(config, f.time); f.seed(session);
  ASSERT_TRUE(session.submit(f.market("short", 2, Side::Sell), f.time).decision.ok());
  const auto theta = std::abs(session.snapshot()->risk.aggregate.position.theta);
  ASSERT_GT(theta, 0);
  auto limits = config.limits;
  limits.tag_overrides["hedge"].theta = theta / 5;
  ASSERT_TRUE(session.set_limits(limits, f.time).decision.ok());
  auto add = f.market("add", 1, Side::Sell); add.tags = {"hedge"};
  const auto refused = session.submit(add, f.time);
  EXPECT_EQ(refused.decision.code, Reason::THETA_LIMIT); EXPECT_EQ(refused.decision.scope, "tag:hedge");
  auto cover = f.market("cover", 1); cover.tags = {"hedge"};
  EXPECT_TRUE(session.submit(cover, f.time).decision.ok());
}
TEST(ExposureLimits, RememberedGridsFollowValuationsQuantitiesAndFreshness) {
  test::ScriptedMarket f;
  Contracts contracts{{f.symbol(), f.contract}};
  Limits limits; limits.aggregate = {1e9, 1e9}; limits.per_underlying = {1e9, 1e9};
  limits.aggregate.worst_grid_loss = 1e9;
  const auto direct = [&](const Ledger& ledger, const Valuations& values, Timestamp now) {
    const auto grid = scenario_grid(ledger, values, {}, now, limits.max_valuation_age);
    double loss = 0;
    for (const auto& cell : grid.cells) loss = std::max(loss, -cell.pnl);
    return std::pair(grid.complete, loss);
  };
  const auto check = [&](const Ledger& ledger, const Valuations& values, Timestamp now) {
    for (int repeat = 0; repeat < 2; ++repeat) {
      const auto risk = portfolio_risk(ledger, {}, contracts, values, limits, now);
      const auto [complete, loss] = direct(ledger, values, now);
      EXPECT_EQ(risk.complete, complete);
      ASSERT_TRUE(risk.aggregate.worst_grid_loss);
      EXPECT_EQ(*risk.aggregate.worst_grid_loss, loss);
    }
  };
  Ledger ledger; ledger.fill(f.contract, -1, Money::parse("4"), {});
  Valuations values{{f.symbol(), f.valuation()}};
  check(ledger, values, f.time);
  auto moved = f.valuation(); moved.smile_iv += 0.05; moved.forward *= 1.01;
  check(ledger, {{f.symbol(), moved}}, f.time);
  ledger.fill(f.contract, -2, Money::parse("4"), {});
  check(ledger, values, f.time);
  check(ledger, values, f.time + 10 * limits.max_valuation_age);  // stale: incomplete, not remembered
  EXPECT_FALSE(portfolio_risk(ledger, {}, contracts, values, limits, f.time + 10 * limits.max_valuation_age).complete);
}
TEST(ExposureLimits, NewCapsRoundTripAndLooseningWaitsForRollover) {
  Limits current, requested;
  current.aggregate.theta = 0; current.aggregate.short_vega = 12;
  current.tag_overrides["test"] = current.aggregate;
  requested.aggregate.theta = 100;
  const auto tightened = tightened_limits(current, requested);
  EXPECT_EQ(tightened.aggregate.theta, 0); EXPECT_EQ(tightened.aggregate.short_vega, 12);
  EXPECT_TRUE(tightened.tag_overrides.contains("test"));
  const auto stored = Json(current).get<Limits>();
  EXPECT_EQ(stored.aggregate, current.aggregate); EXPECT_EQ(stored.tag_overrides, current.tag_overrides);
  current.aggregate.theta = -1;
  EXPECT_THROW(validate_limits(current), TradingError);
}
}
}
