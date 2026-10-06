#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <gtest/gtest.h>

#include "trading/margin_detail.hpp"
#include "openport/trading/session.hpp"
#include "support/sanitizer.hpp"

namespace openport::trading {
namespace {
Money dollars(std::string_view value) { return Money::parse(value); }
MarginLeg option(std::string_view symbol, Quantity quantity, std::string_view value = "0") {
  return {*md::parse_osi(symbol), quantity, dollars(value), 500.0};
}

TEST(TradingMarginAllocation, MixedExpiryBooksRetainTheirSeparateAllocations) {
  using Positions = std::vector<std::pair<std::string, Quantity>>;
  const std::vector<std::pair<Positions, Positions>> cases{
      {{{"SPY261022C00510000", -1}, {"SPY261022P00510000", -1}, {"SPY261022C00520000", -2},
        {"SPY261029P00510000", -2}, {"SPY261105C00480000", -2}, {"SPY261105C00490000", 1}, {"SPY261105C00530000", 2}},
       {{"SPY261022P00490000", -1}, {"SPY261022P00520000", -2}, {"SPY261029C00480000", 1},
        {"SPY261029C00500000", -2}, {"SPY261105C00520000", 1}}},
      {{{"SPY261022P00530000", -1}, {"SPY261029C00530000", -1}, {"SPY261105P00500000", -2}},
       {{"SPY261029P00470000", -1}, {"SPY261029P00480000", -2}, {"SPY261029P00500000", 3},
        {"SPY261029P00530000", -1}, {"SPY261105C00490000", 2}, {"SPY261105P00490000", -2}}},
      {{{"SPY261022C00510000", -2}, {"SPY261029P00480000", -1}, {"SPY261029C00500000", 1},
        {"SPY261105C00490000", -1}, {"SPY261105C00520000", 2}},
       {{"SPY261022P00480000", -2}, {"SPY261022P00520000", -1}, {"SPY261029C00470000", -2},
        {"SPY261029C00490000", 1}, {"SPY261029C00520000", 2}, {"SPY261105P00470000", 1}}},
      {{{"SPY261022P00470000", 1}, {"SPY261022C00490000", -1}, {"SPY261022P00500000", 2},
        {"SPY261022P00530000", -1}, {"SPY261105P00490000", 1}, {"SPY261105C00530000", 1}},
       {{"SPY261022C00470000", -2}, {"SPY261022C00480000", -1}, {"SPY261029C00490000", -2},
        {"SPY261029C00500000", 2}, {"SPY261029P00510000", -2}, {"SPY261029C00520000", 1}, {"SPY261105C00480000", -1}}},
      {{{"SPY261022P00520000", 1}, {"SPY261029P00470000", 2}, {"SPY261029P00530000", -1}, {"SPY261105P00470000", -2}},
       {{"SPY261022C00470000", -1}, {"SPY261022P00520000", 1}, {"SPY261022C00530000", 1}, {"SPY261029C00510000", 2}}}};
  const auto build = [](const Positions& positions) {
    std::vector<MarginLeg> legs;
    for (const auto& [symbol, q] : positions) {
      auto leg = option(symbol, q);
      if (q < 0) leg.value = dollars("250") * -q;
      legs.push_back(leg);
    }
    return legs;
  };
  for (std::size_t i = 0; i < cases.size(); ++i) {
    SCOPED_TRACE(i + 1);
    const auto a = build(cases[i].first), b = build(cases[i].second);
    std::map<std::string, Quantity> combined;
    for (const auto* positions : {&cases[i].first, &cases[i].second})
      for (const auto& [symbol, q] : *positions) combined[symbol] += q;
    auto ab = build(Positions(combined.begin(), combined.end()));
    const auto separate = margin_requirement(a) + margin_requirement(b);
    const auto actual = margin_requirement(ab);
    EXPECT_LE(actual, separate) << actual.str() << " > " << separate.str();
    EXPECT_LE(actual, detail::pairing_margin_requirement(ab));
    EXPECT_EQ(margin_requirement(ab), actual);
  }
}

TEST(TradingMarginAllocation, SeededMixedExpirySubadditivityRate) {
  // Use explicit LCG draws and a partial shuffle, identical on every standard
  // library. A and B contain 2..7 distinct, non-overlapping series each.
  std::uint32_t state = 0xF490123U;
  const auto next = [&]() { state = state * 1664525U + 1013904223U; return state >> 8; };
  std::vector<md::OptionContract> contracts;
  for (const auto* expiry : {"261022", "261029", "261105"})
    for (int strike = 470; strike <= 530; strike += 10)
      for (const char type : {'C', 'P'})
        contracts.push_back(*md::parse_osi(std::string("SPY") + expiry + type + "00" + std::to_string(strike) + "000"));
  struct Counts { int checked = 0, pairing = 0, joint = 0, fallbacks = 0; };
  Counts margin, ira;
  const int trials = test::sanitizer_scale(20736, 2048);
  for (int trial = 0; trial < trials; ++trial) {
    const auto na = 2 + next() % 6, nb = 2 + next() % 6;
    auto shuffled = contracts;
    std::vector<MarginLeg> a, b, ab;
    for (std::size_t i = 0; i < na + nb; ++i) {
      std::swap(shuffled[i], shuffled[i + next() % (shuffled.size() - i)]);
      const auto n = static_cast<Quantity>(1 + next() % 2);
      const auto q = next() % 2 ? n : -n;
      const MarginLeg leg{shuffled[i], q, q < 0 ? dollars("250") * n : Money{}, 500.0};
      (i < na ? a : b).push_back(leg);
      ab.push_back(leg);
    }
    for (const auto account : {AccountType::Margin, AccountType::Ira}) {
      if (account == AccountType::Ira && (disallowed_shorts(a, {}, account) || disallowed_shorts(b, {}, account) ||
                                         disallowed_shorts(ab, {}, account))) continue;
      const MarginPolicy policy{account, 0, 0};
      auto& count = account == AccountType::Margin ? margin : ira;
      ++count.checked;
      const auto old_sum = detail::pairing_margin_requirement(a, {}, policy) + detail::pairing_margin_requirement(b, {}, policy);
      const auto old_ab = detail::pairing_margin_requirement(ab, {}, policy);
      const auto exact = [&](const std::vector<MarginLeg>& positions) {
        Money requirement;
        for (const auto& item : margin_breakdown(positions, {}, policy)) {
          count.fallbacks += item.allocation != "exact";
          requirement = requirement + item.requirement;
        }
        return requirement;
      };
      const auto sum = exact(a) + exact(b);
      const auto actual = exact(ab);
      count.pairing += old_ab > old_sum;
      count.joint += actual > sum;
      EXPECT_LE(actual, old_ab) << "trial=" << trial << " account=" << static_cast<int>(account);
    }
  }
  for (const auto& [name, count] : {std::pair{"margin", margin}, std::pair{"ira", ira}}) {
    std::cout << name << " subadditivity violations: pairing=" << count.pairing << "/" << count.checked
              << " joint=" << count.joint << "/" << count.checked << " fallbacks=" << count.fallbacks << '\n';
    RecordProperty(std::string(name) + "_checked", count.checked);
    RecordProperty(std::string(name) + "_pairing_violations", count.pairing);
    RecordProperty(std::string(name) + "_joint_violations", count.joint);
    EXPECT_EQ(count.fallbacks, 0);
  }
  EXPECT_EQ(margin.checked, trials);
  EXPECT_EQ(ira.checked, test::sanitizer_scale(5423, 525));
  EXPECT_EQ(margin.joint, 0);
  EXPECT_EQ(ira.joint, 0);
}

TEST(TradingMarginAllocation, EarlierFreeLongCannotBlockAnIraPool) {
  const MarginPolicy ira{AccountType::Ira, 0, 0};
  std::vector<MarginLeg> book{
      option("SPY261029C00520000", 2), option("SPY261029P00490000", 1),
      option("SPY261029C00480000", 2), option("SPY261105C00520000", 1),
      option("SPY261029P00520000", -1, "250"), option("SPY261105P00490000", 2)};
  EXPECT_EQ(disallowed_shorts(book, {}, ira.account), 0);
  EXPECT_EQ(margin_requirement(book, {}, ira), dollars("1000"));
  book.insert(book.begin() + 1, option("SPY261022C00530000", 1));
  EXPECT_EQ(margin_requirement(book, {}, ira), dollars("1000"));
}

TEST(TradingMarginAllocation, PoolsKeepTheirCoversBesideStraddles) {
  const std::vector<MarginLeg> book{
      option("QQQ261022C00490000", -2, "2600"), option("QQQ261029P00510000", -1, "1300"),
      option("QQQ261022P00510000", -2, "2600"), option("QQQ261022P00520000", 1), option("QQQ261029P00490000", 1)};
  EXPECT_EQ(detail::pairing_margin_requirement(book), dollars("25200"));
  EXPECT_LE(margin_requirement(book), dollars("24900"));
}
TEST(TradingMarginAllocation, IraPutRatiosUseLaterLongs) {
  const MarginPolicy ira{AccountType::Ira, 0, 0};
  const std::vector<MarginLeg> book{
      option("SPY261022P00490000", -2), option("SPY261022P00480000", -2),
      option("SPY261022P00510000", 2), option("SPY261029P00490000", 1)};
  EXPECT_EQ(detail::pairing_margin_requirement(book, {}, ira), dollars("48000"));
  EXPECT_EQ(margin_requirement(book, {}, ira), dollars("43000"));
}

TEST(TradingMarginAllocation, LargeQuantitiesUseWholeTranches) {
  const MarginPolicy ira{AccountType::Ira, 0, 0};
  std::vector<MarginLeg> book{
      option("SPY261022P00490000", -20000), option("SPY261022P00480000", -20000),
      option("SPY261022P00510000", 20000), option("SPY261029P00490000", 10000)};
  const auto expected = dollars("430000000");
  EXPECT_EQ(margin_requirement(book, {}, ira), expected);
  std::reverse(book.begin(), book.end());
  EXPECT_EQ(margin_requirement(book, {}, ira), expected);
}

TEST(TradingMarginAllocation, FractionalRelaxationRequiresWholeContracts) {
  const std::vector<MarginLeg> book{
      option("SPY261022P00500000", -2, "500"), option("SPY261029P00520000", 1)};
  // The LP can give 1.04 shorts the long's zero-loss pool, leaving only 0.96
  // shorts naked ($9840). Whole contracts require one naked short ($10250).
  const auto result = margin_breakdown(book);
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].allocation, "exact");
  EXPECT_EQ(result[0].requirement, dollars("10250"));
}

TEST(TradingMarginAllocation, ButterflyWorstLossListsEachPositionOnce) {
  const std::vector<MarginLeg> book{
      option("SPXW260917C05975000", 1), option("SPXW260917C05985000", -2),
      option("SPXW260917C05995000", 1)};
  for (const bool exact : {false, true}) {
    SCOPED_TRACE(exact);
    MarginPolicy policy;
    policy.exact = exact;
    const auto result = margin_breakdown(book, {}, policy);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].allocation, exact ? "exact" : "legacy");
    EXPECT_EQ(result[0].requirement, Money{});
    EXPECT_EQ(result[0].requirement, margin_requirement(book, {}, policy));
    ASSERT_EQ(result[0].parts.size(), 1U);
    const auto& part = result[0].parts[0];
    EXPECT_EQ(part.kind, MarginPartKind::WorstLoss);
    EXPECT_EQ(part.requirement, Money{});
    ASSERT_EQ(part.legs.size(), 3U);
    std::map<std::string, Quantity> positions;
    for (const auto& [symbol, quantity] : part.legs)
      EXPECT_TRUE(positions.emplace(symbol, quantity).second);
    EXPECT_EQ(positions.at(book[1].contract.osi_symbol()), -2);
    EXPECT_EQ(positions.at(book[0].contract.osi_symbol()), 1);
    EXPECT_EQ(positions.at(book[2].contract.osi_symbol()), 1);
  }
}

TEST(TradingMarginAllocation, OddShortSharesRoundOnlyTheUnderlyingTotal) {
  const MarginPolicy policy{AccountType::Margin, 25, 0};
  // 104 shares at $500.000001, holding 162.5%: exactly $84500.000169.
  // Rounding the four-share remainder first would add a micro-dollar.
  const std::vector<MarginStock> stock{{"SPY", -104, dollars("52000.000104")}};
  const auto result = margin_breakdown({}, stock, policy);
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].allocation, "exact");
  EXPECT_EQ(result[0].requirement, dollars("84500.000169"));
  Money parts;
  for (const auto& part : result[0].parts) parts = parts + part.requirement;
  EXPECT_EQ(parts, result[0].requirement);
  // A later long call protects the whole lot; only the four shares retain
  // their proportional cost, rounded up along with the exact strike reserve.
  EXPECT_EQ(margin_requirement({option("SPY261105C00500000", 1)}, stock, policy), dollars("53250.000007"));
}

TEST(TradingMarginAllocation, LargeBooksCertifyTinyDualPriceResiduals) {
  for (const Quantity scale : {1, 10000}) {
    std::vector<MarginLeg> book;
    for (int i = 0; i < 80; ++i) {
      auto contract = *md::parse_osi(i % 3 == 0 ? "SPY261029P00500000" : "SPY261022P00500000");
      contract.type = i % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
      contract.strike = 450.0 + i;
      const bool shorted = i % 4 < 2;
      book.push_back({contract, (shorted ? -2 : 2) * scale,
                      shorted ? dollars("600.000002") * scale : Money{}, 500.0});
    }
    const auto result = margin_breakdown(book);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].allocation, "exact");
    EXPECT_EQ(result[0].requirement, dollars("8400") * scale);
  }
}

TEST(TradingMarginAllocation, HardResourceLimitReportsConservativeFallback) {
  std::vector<MarginLeg> book{option("SPY261022C00500000", -1, "250")};
  for (int i = 0; i < 128; ++i) {
    auto long_put = option("SPY261029P00500000", 1);
    long_put.contract.strike += i;
    book.push_back(long_put);
  }
  const auto result = margin_breakdown(book);
  ASSERT_EQ(result.size(), 1U);
  EXPECT_EQ(result[0].allocation, "bounded");
  EXPECT_LE(result[0].requirement, detail::pairing_margin_requirement(book));
  std::reverse(book.begin(), book.end());
  EXPECT_EQ(margin_requirement(book), result[0].requirement);
  auto legacy = MarginPolicy{}; legacy.exact = false;
  EXPECT_EQ(margin_breakdown(book, {}, legacy)[0].allocation, "legacy");
}

TEST(TradingMarginAllocation, JointBuyingPowerFillsAndRecoversIdenticalJournals) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-joint-margin-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto time = md::new_york_to_utc({2026, 10, 21}, 10, 0);
  const std::vector<MarginLeg> book{
      option("SPY261022P00490000", -2), option("SPY261022P00480000", -2),
      option("SPY261022P00510000", 2), option("SPY261029P00490000", 1)};
  std::string previous;
  for (int run = 0; run < 2; ++run) {
    const auto path = (directory / (std::to_string(run) + ".jsonl")).string();
    SessionConfig config;
    config.initial_cash = dollars("45000");
    config.rules.account_type = AccountType::Ira;
    config.rules.buying_power = true;
    config.limits.aggregate = {1e9, 1e9};
    config.limits.per_underlying = {1e9, 1e9};
    auto sink = FileJournal::create(path);
    TradingSession session(config, time, sink);
    OrderRequest request;
    request.client_order_id = "ratio";
    request.type = OrderType::Market;
    request.tif = TimeInForce::Ioc;
    request.quantity = 1;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> values;
    for (const auto& leg : book) {
      const auto symbol = leg.contract.osi_symbol();
      ASSERT_TRUE(session.define(leg.contract, time).decision.ok());
      quotes.push_back({symbol, 1, time, dollars("1"), dollars("1.10"), 10, 10});
      values.push_back({symbol, time, -0.5, 0.001, 2, -0.1, 500, 501, 0.99,
                        md::years_between(time, leg.contract.expiry_time()), 0.2, true});
      request.legs.push_back({symbol, leg.quantity < 0 ? Side::Sell : Side::Buy, std::abs(leg.quantity)});
    }
    session.on_quotes(quotes, values, time);
    ASSERT_TRUE(session.preview(request, time).decision.ok());
    const auto result = session.submit(request, time);
    ASSERT_TRUE(result.decision.ok()) << result.decision.message;
    EXPECT_EQ(session.snapshot()->buying_power.short_requirement, dollars("43000"));
    EXPECT_EQ(session.snapshot()->positions.size(), 4U);
    EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), session.snapshot_json());
    std::ifstream file(path);
    std::ostringstream bytes;
    bytes << file.rdbuf();
    if (run) { EXPECT_EQ(bytes.str(), previous); }
    previous = bytes.str();
  }
  std::filesystem::remove_all(directory);
}

// An independent exhaustive oracle: enumerate partitions of individual contracts.
// Every block is naked, one legal pair, or a bounded expiry pool. It deliberately
// does not use the production pairing, pool evaluation, or improvement code.
Money exhaustive(const std::vector<MarginLeg>& atoms, const MarginPolicy& policy) {
  const auto size = std::size_t{1} << atoms.size();
  const auto infinity = Money::from_micros(std::numeric_limits<std::int64_t>::max());
  std::vector<Money> dp(size, infinity);
  dp[0] = {};
  const auto naked = [&](const MarginLeg& leg) {
    if (leg.quantity > 0) return Money{};
    if (policy.account != AccountType::Margin && leg.contract.type == pricing::OptionType::Put)
      return Money::from_double(leg.contract.strike) * 100;
    return leg.value + naked_requirement(leg.contract, leg.spot).prorate(100 + policy.house_percent, 100);
  };
  for (std::size_t mask = 1; mask < size; ++mask) {
    Money cost;
    std::vector<std::size_t> members;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
      if (!(mask & (std::size_t{1} << i))) continue;
      cost = cost + naked(atoms[i]);
      members.push_back(i);
    }
    if (members.size() == 2) {
      auto a = atoms[members[0]], b = atoms[members[1]];
      if (a.quantity > 0) std::swap(a, b);
      if (a.quantity < 0 && b.quantity > 0 && a.contract.type == b.contract.type &&
          b.contract.expiry_time() >= a.contract.expiry_time() && policy.account != AccountType::Cash) {
        const auto width = a.contract.type == pricing::OptionType::Put ? a.contract.strike - b.contract.strike :
                                                                                       b.contract.strike - a.contract.strike;
        cost = std::min(cost, Money::from_double(std::max(0.0, width)) * 100);
      }
      if (a.quantity < 0 && b.quantity < 0 && a.contract.type != b.contract.type && policy.account == AccountType::Margin) {
        const auto na = naked(a), nb = naked(b);
        const auto paired = na > nb ? na + b.value : nb > na ? nb + a.value : na + std::max(a.value, b.value);
        cost = std::min(cost, paired);
      }
    }
    std::optional<Timestamp> expiry;
    bool bounded = policy.account != AccountType::Cash;
    Quantity calls = 0;
    std::vector<double> spots{0};
    for (const auto i : members) {
      const auto& leg = atoms[i];
      spots.push_back(leg.contract.strike);
      if (leg.contract.type == pricing::OptionType::Call) calls += leg.quantity;
      if (leg.quantity < 0) {
        if (expiry && *expiry != leg.contract.expiry_time()) bounded = false;
        expiry = leg.contract.expiry_time();
      }
    }
    for (const auto i : members)
      if (expiry && atoms[i].quantity > 0 && atoms[i].contract.expiry_time() < *expiry) bounded = false;
    if (bounded && calls >= 0) {
      Money loss;
      for (const auto spot : spots) {
        Money payoff;
        for (const auto i : members) {
          const auto& leg = atoms[i];
          const auto intrinsic = leg.contract.type == pricing::OptionType::Call ? spot - leg.contract.strike : leg.contract.strike - spot;
          payoff = payoff + Money::from_double(std::max(0.0, intrinsic)) * (100 * leg.quantity);
        }
        loss = std::max(loss, -payoff);
      }
      cost = std::min(cost, loss);
    }
    dp[mask] = cost;
    // Break symmetry: the first contract always belongs to this sub-block.
    const auto first = std::size_t{1} << std::countr_zero(mask);
    for (auto sub = (mask - 1) & mask; sub; sub = (sub - 1) & mask)
      if (sub & first) dp[mask] = std::min(dp[mask], dp[sub] + dp[mask ^ sub]);
  }
  return dp.back();
}

TEST(TradingMarginAllocation, SplittingBuybackValuesCannotCreateMicroDollarSavings) {
  const std::vector<MarginLeg> book{
      option("QQQ261022C00490000", -3, "3900.000001"), option("QQQ261029P00510000", -1, "1300"),
      option("QQQ261022P00510000", -2, "2600"), option("QQQ261022P00520000", 1), option("QQQ261029P00490000", 1)};
  // Scale money, strikes and spot by three: every individual contract then
  // has an integer value, letting the oracle retain the exact one-third micros.
  std::vector<MarginLeg> atoms;
  for (const auto& leg : book) {
    const auto n = std::abs(leg.quantity);
    auto atom = leg;
    atom.contract.strike *= 3;
    atom.spot = 1500;
    atom.quantity = leg.quantity < 0 ? -1 : 1;
    atom.value = (leg.value * 3).prorate(1, n);
    for (Quantity i = 0; i < n; ++i) atoms.push_back(atom);
  }
  const auto scaled = exhaustive(atoms, {});
  const auto lower = Money::from_micros((scaled.micros() + 2) / 3);
  EXPECT_EQ(lower, dollars("36200.000001"));
  EXPECT_EQ(margin_requirement(book), lower);
  EXPECT_LT(margin_requirement(book), detail::pairing_margin_requirement(book));
}

TEST(TradingMarginAllocation, RandomSmallBooksEqualTheBruteForceOptimum) {
  std::uint32_t state = 918237;
  const auto next = [&]() { state = state * 1664525U + 1013904223U; return state >> 8; };
  std::vector<md::OptionContract> contracts;
  for (const auto* expiry : {"261022", "261029", "261105"})
    for (const auto* strike : {"00480000", "00490000", "00500000", "00510000", "00520000"})
      for (const char type : {'C', 'P'})
        contracts.push_back(*md::parse_osi(std::string("SPY") + expiry + type + strike));
  int improved = 0;
  for (const auto account : {AccountType::Margin, AccountType::Ira, AccountType::Cash}) {
    for (int trial = 0; trial < 800; ++trial) {
      SCOPED_TRACE(::testing::Message() << "account=" << static_cast<int>(account) << " trial=" << trial);
      const MarginPolicy policy{account, trial % 3 == 0 ? 25 : 0, 0};
      std::map<std::size_t, Quantity> quantities;
      const auto count = 3 + next() % 5;
      for (std::uint32_t n = 0; n < count; ++n) quantities[next() % contracts.size()] += next() % 2 ? 1 : -1;
      std::vector<MarginLeg> book, atoms;
      for (const auto& [i, quantity] : quantities) {
        if (!quantity) continue;
        const auto n = quantity < 0 ? -quantity : quantity;
        const auto premium = Money::from_micros(300000001 + static_cast<std::int64_t>(next() % 2000000000));
        book.push_back({contracts[i], quantity, quantity < 0 ? premium * n : Money{}, 500.0});
        for (Quantity q = 0; q < n; ++q) atoms.push_back({contracts[i], quantity < 0 ? -1 : 1, quantity < 0 ? premium : Money{}, 500.0});
      }
      const auto optimum = exhaustive(atoms, policy);
      const auto baseline = detail::pairing_margin_requirement(book, {}, policy);
      const auto actual = margin_requirement(book, {}, policy);
      EXPECT_EQ(actual, optimum);
      EXPECT_LE(actual, baseline);
      EXPECT_EQ(actual, margin_requirement(book, {}, policy));
      improved += actual < baseline;
      std::map<std::string, Quantity> used, expected;
      for (const auto& leg : book) expected[leg.contract.osi_symbol()] = leg.quantity;
      Money total;
      for (const auto& underlying : margin_breakdown(book, {}, policy)) {
        EXPECT_EQ(underlying.allocation, "exact");
        Money subtotal;
        for (const auto& part : underlying.parts) {
          EXPECT_GE(part.requirement, Money{});
          std::vector<MarginLeg> allocated;
          for (const auto& [symbol, q] : part.legs) {
            used[symbol] += q;
            const auto it = std::find_if(atoms.begin(), atoms.end(), [&](const auto& leg) { return leg.contract.osi_symbol() == symbol; });
            ASSERT_NE(it, atoms.end());
            EXPECT_EQ(q < 0, it->quantity < 0);
            for (Quantity n = 0; n < (q < 0 ? -q : q); ++n) allocated.push_back(*it);
          }
          EXPECT_GE(part.requirement, exhaustive(allocated, policy));
          subtotal = subtotal + part.requirement;
        }
        EXPECT_EQ(subtotal, underlying.requirement);
        total = total + subtotal;
      }
      EXPECT_EQ(total, actual);
      EXPECT_EQ(used, expected);
    }
  }
  EXPECT_GT(improved, 0);
}
}  // namespace
}  // namespace openport::trading
