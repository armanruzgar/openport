#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <tuple>
#include <gtest/gtest.h>

#include "openport/trading/session.hpp"
#include "openport/pricing/black.hpp"
#include "trading/margin_detail.hpp"
#include "support/journal.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
/// Canonical padded OSI.
std::string osi(std::string_view compact) { return md::parse_osi(compact)->osi_symbol(); }
const std::string P4900 = osi("SPXW261022P04900000");
const std::string P4890 = osi("SPXW261022P04890000");
const std::string C5100 = osi("SPXW261022C05100000");
const std::string C5110 = osi("SPXW261022C05110000");
const std::string C5120 = osi("SPXW261022C05120000");
const std::string LATER = osi("SPXW261023P04890000");
const std::string LATER_4900 = osi("SPXW261023P04900000");
const std::string XSP = osi("XSP261022P00490000");

/// A few SPXW strikes around 5000, quoted together.
struct Chain {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  void define(TradingSession& s, std::initializer_list<std::string> symbols) {
    for (const auto& symbol : symbols) ASSERT_TRUE(s.define(*md::parse_osi(symbol), time).decision.ok());
  }
  /// (symbol, bid, ask, delta); every quote carries `size` on both sides.
  void quote(TradingSession& s, std::vector<std::tuple<std::string, std::string, std::string, double>> books, Quantity size = 10) {
    ++observation;
    time += md::kNanosPerSecond;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (const auto& [symbol, bid, ask, delta] : books) {
      const auto contract = *md::parse_osi(symbol);
      quotes.push_back({symbol, observation, time, m(bid), m(ask), size, size});
      valuations.push_back({symbol, time, delta, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                            md::years_between(time, contract.expiry_time()), 0.20, true});
    }
    s.on_quotes(quotes, valuations, time);
  }
};
Leg leg(const std::string& symbol, Side side, Quantity ratio = 1) { return {symbol, side, ratio}; }
OrderRequest combo(std::string client, std::vector<Leg> legs, Quantity units, std::optional<std::string_view> net,
                   TimeInForce tif = TimeInForce::Day) {
  OrderRequest r;
  r.client_order_id = std::move(client);
  r.type = net ? OrderType::Limit : OrderType::Market;
  r.tif = net ? tif : TimeInForce::Ioc;
  r.quantity = units;
  if (net) r.limit_price = m(*net);
  r.legs = std::move(legs);
  return r;
}
SessionConfig config(std::string_view cash = "100000", AccountRules rules = {}) {
  SessionConfig c;
  c.initial_cash = m(cash);
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
MarginLeg margin(const std::string& symbol, Quantity quantity, std::string_view value = "0") {
  return {*md::parse_osi(symbol), quantity, m(value), 5000.0};
}

TEST(TradingMargin, SpreadsNeedTheirWidthAndBoundedGroupsTheirWorstLoss) {
  // Naked: buy-back value plus 100 * max(20% of 5000 - 100 OTM, 10% of 4900) per contract.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500")}), m("90500"));
  EXPECT_EQ(margin_requirement({margin(P4900, -2, "1000")}), m("181000"));
  // A credit put spread holds its width; a debit spread nothing.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4900, 1), margin(P4890, -1, "400")}), m("0"));
  // An iron condor loses on one wing at most.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1)}), m("1000"));
  // A long butterfly cannot lose more than its debit, which is paid.
  EXPECT_EQ(margin_requirement({margin(C5100, 1), margin(C5110, -2, "500"), margin(C5120, 1)}), m("0"));
  // A 1x2 ratio: one short covered, the other naked at half the buy-back value.
  EXPECT_EQ(margin_requirement({margin(P4900, -2, "1000"), margin(P4890, 1)}), m("91500"));
  // A strangle is unbounded above: it holds the greater naked requirement plus the other short's value.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(C5100, -1, "300")}), m("90800"));
  // A long that expires with its short or later covers it: diagonals hold the width, calendars nothing.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(LATER, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(LATER_4900, 1)}), m("0"));
  // A long that expires first does not cover a later short.
  EXPECT_EQ(margin_requirement({margin(LATER, -1, "400"), margin(P4900, 1)}), m("400") + naked_requirement(*md::parse_osi(LATER), 5000.0));
  // An iron condor beside a calendar: each expiry on its own still wins.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1),
                                margin(LATER_4900, 1)}), m("1000"));
  EXPECT_EQ(margin_requirement({margin(P4890, 3), margin(C5100, 2)}), Money{});
}
TEST(TradingMargin, PairingCoversEveryShortItCanAcrossExpiries) {
  const auto later = [](std::string_view strike) { return osi("SPXW261023P0" + std::string(strike) + "000"); };
  const auto latest = [](std::string_view strike) { return osi("SPXW261026P0" + std::string(strike) + "000"); };
  // A diagonal (short 4900, long 4890 a day later) beside a later debit spread
  // (long 4900, short 4890 two days later): the debit spread's long must not be
  // spent on the diagonal's short, which would leave its own short naked.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(later("4890"), 1),
                                margin(latest("4900"), 1), margin(latest("4890"), -1, "400")}), m("1000"));
  // Two shorts, each covered: the higher short takes the same-strike long a day
  // later, because the other short expires after that long and needs the higher one.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(later("4900"), 1),
                                margin(latest("4890"), -1, "400"), margin(latest("4910"), 1)}), Money{});
  // Equal strikes: the condor's short put takes its own wing's long, leaving the
  // later calendar long for the calendar's short.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1),
                                margin(later("4900"), -1, "450"), margin(latest("4900"), 1)}), m("1000"));
}
TEST(TradingMargin, PairsExpiringTogetherHoldTheirCombinedWorstLoss) {
  // An iron condor with a 10-point put wing and a 20-point call wing holds its wider wing.
  const std::vector<MarginLeg> condor{margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5120, 1)};
  EXPECT_EQ(margin_requirement(condor), m("2000"));
  // A calendar beside it (short with the condor, long a day later) adds nothing:
  // its long is worth at least its intrinsic value when the short expires.
  auto with_calendar = condor;
  with_calendar.push_back(margin(osi("SPXW261022P04905000"), -1, "520"));
  with_calendar.push_back(margin(osi("SPXW261023P04905000"), 1));
  EXPECT_EQ(margin_requirement(with_calendar), m("2000"));
  // Pairs whose shorts expire on different days can both lose, so they add up.
  auto two_expiries = condor;
  two_expiries.push_back(margin(osi("SPXW261023P04900000"), -1, "520"));
  two_expiries.push_back(margin(osi("SPXW261023P04880000"), 1));
  EXPECT_EQ(margin_requirement(two_expiries), m("4000"));
}

TEST(TradingMargin, ShortStraddlesHoldTheGreaterSidePlusTheOthersValue) {
  const auto C5200 = osi("SPXW261022C05200000");
  // Put 4900: 100 * max(1000 - 100, 490) = 90,000; call 5200: 100 * max(1000 - 200, 500) = 80,000.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(C5200, -1, "300")}), m("90800"));
  // Two puts and one call: one straddle, and the other put naked.
  EXPECT_EQ(margin_requirement({margin(P4900, -2, "1000"), margin(C5200, -1, "300")}), m("90800") + m("90500"));
  // Expiries need not match.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(osi("SPXW261023C05200000"), -1, "300")}), m("90800"));
  // The greater requirement includes its own value: a later put worth 20,000 with
  // 80,000 of naked requirement outweighs the call's 300 and 90,000, so the pair
  // holds the put's 100,000 plus the call's 300, not both values on the call's 90,000.
  EXPECT_EQ(margin_requirement({margin(osi("SPXW261120P04800000"), -1, "20000"), margin(C5100, -1, "300")}), m("100300"));
  // A naked call takes a put spread's short when the straddle holds less than the
  // spread's width did: 90,000 plus both values, instead of 1,000 plus the naked call.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300")}), m("90800"));
  // Not when the spread holds less: a 2-point spread's 200 beats a straddle's 500 of put value.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(osi("SPXW261022P04898000"), 1), margin(C5100, -1, "300")}),
            m("200") + m("90300"));
  // A wide condor's verticals save less than a straddle would, but its wings
  // share one worst loss, which holds less again: one 500-point wing.
  const std::vector<MarginLeg> wide{margin(P4900, -1, "500"), margin(osi("SPXW261022P04400000"), 1),
                                    margin(C5100, -1, "300"), margin(osi("SPXW261022C05600000"), 1)};
  EXPECT_EQ(margin_requirement(wide), m("50000"));
  // Underlyings never offset.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(osi("XSP261022C00510000"), -1, "30")}),
            m("90500") + m("30") + naked_requirement(*md::parse_osi("XSP261022C00510000"), 5000.0));
}
TEST(TradingMargin, SharesCoverOptionsAndLongCallsProtectShortShares) {
  const auto spy = [](std::string_view compact, Quantity quantity, std::string_view value = "0") {
    return MarginLeg{*md::parse_osi(compact), quantity, m(value), 500.0};
  };
  const auto naked_call = m("300") + naked_requirement(*md::parse_osi("SPY261022C00510000"), 500.0);
  // A covered call holds nothing; 150 shares cover one call, and the second is naked.
  EXPECT_EQ(margin_requirement({spy("SPY261022C00510000", -1, "300")}, {{"SPY", 100, m("50000")}}), Money{});
  EXPECT_EQ(margin_requirement({spy("SPY261022C00510000", -2, "600")}, {{"SPY", 150, m("75000")}}), naked_call);
  // Shares cover calls of any strike or expiry, but not puts.
  EXPECT_EQ(margin_requirement({spy("SPY261023C00450000", -1, "5000")}, {{"SPY", 100, m("50000")}}), Money{});
  const auto put = spy("SPY261022P00490000", -1, "200");
  EXPECT_EQ(margin_requirement({put}, {{"SPY", 100, m("50000")}}), margin_requirement({put}));
  // Short shares hold 150% of their value; a short put against them adds only its buy-back value.
  EXPECT_EQ(margin_requirement({}, {{"SPY", -100, m("50000")}}), m("75000"));
  EXPECT_EQ(margin_requirement({put}, {{"SPY", -100, m("50000")}}), m("75200"));
  // A long call caps 100 short shares at its strike, never above 150%.
  EXPECT_EQ(margin_requirement({spy("SPY261022C00510000", 1)}, {{"SPY", -100, m("50000")}}), m("51000"));
  EXPECT_EQ(margin_requirement({spy("SPY261022C00490000", 1)}, {{"SPY", -100, m("50000")}}), m("49000"));
  EXPECT_EQ(margin_requirement({spy("SPY261022C00800000", 1)}, {{"SPY", -100, m("50000")}}), m("75000"));
  // 250 short shares: two lots protected, the other 50 shares at 150%.
  EXPECT_EQ(margin_requirement({spy("SPY261022C00510000", 3)}, {{"SPY", -250, m("125000")}}), m("102000") + m("37500"));
  // One long call cannot both protect shares and cover a short call: it goes where it saves more.
  EXPECT_EQ(margin_requirement({spy("SPY261022C00510000", 1), spy("SPY261022C00520000", -1, "200")}, {{"SPY", -100, m("50000")}}),
            m("51000") + m("200") + naked_requirement(*md::parse_osi("SPY261022C00520000"), 500.0));
}
TEST(TradingMargin, CombinedStructuresHoldNoMoreThanEachAlone) {
  // A long put butterfly and a call calendar each hold nothing; together, still nothing.
  const std::vector<MarginLeg> book{margin(osi("SPXW261022P04980000"), 1), margin(osi("SPXW261022P04990000"), -2, "900"),
                                    margin(osi("SPXW261022P05000000"), 1), margin(osi("SPXW261022C04990000"), -1, "1500"),
                                    margin(osi("SPXW261023C04990000"), 1)};
  EXPECT_EQ(margin_requirement(book), Money{});
}
TEST(TradingMargin, VerticalsAndStraddlesChooseTheirCoversTogether) {
  const std::vector<MarginLeg> a{{*md::parse_osi("SPY261029C00480000"), -1, m("2300"), 500.0},
                                {*md::parse_osi("SPY261029P00520000"), -1, m("2300"), 500.0}};
  const std::vector<MarginLeg> b{{*md::parse_osi("SPY261022C00480000"), 2, {}, 500.0},
                                {*md::parse_osi("SPY261022P00480000"), -1, m("300"), 500.0},
                                {*md::parse_osi("SPY261029P00490000"), 1, {}, 500.0}};
  auto combined = a;
  combined.insert(combined.end(), b.begin(), b.end());
  EXPECT_EQ(margin_requirement(a), m("14600"));
  EXPECT_EQ(margin_requirement(b), Money{});
  // The long put covers the early 480 put, leaving the 520 put with the 480 call.
  EXPECT_EQ(margin_requirement(combined), m("14600"));
  std::map<MarginPartKind, Money> parts;
  for (const auto& item : margin_breakdown(combined))
    for (const auto& part : item.parts) parts[part.kind] = parts[part.kind] + part.requirement;
  EXPECT_EQ(parts.at(MarginPartKind::Straddle), m("14600"));
  EXPECT_EQ(parts.at(MarginPartKind::Vertical), Money{});
}
TEST(TradingMargin, ShareCoversLeaveTheRemainingExpiryItsWorstLoss) {
  const MarginPolicy ira{AccountType::Ira, 0, 0};
  const std::vector<MarginLeg> a{{*md::parse_osi("SPY261029P00480000"), -2, m("600"), 500.0},
                                {*md::parse_osi("SPY261029P00510000"), 1, {}, 500.0}};
  const std::vector<MarginLeg> b{{*md::parse_osi("SPY261029C00490000"), -2, m("2600"), 500.0},
                                {*md::parse_osi("SPY261029C00500000"), 1, {}, 500.0}};
  const std::vector<MarginStock> shares{{"SPY", 100, m("50000")}};
  auto combined = a;
  combined.insert(combined.end(), b.begin(), b.end());
  EXPECT_EQ(margin_requirement(a, {}, ira), m("45000"));
  EXPECT_EQ(margin_requirement(b, shares, ira), m("1000"));
  EXPECT_EQ(margin_requirement(a, {}, ira) + margin_requirement(b, shares, ira), m("46000"));
  // After one call takes the shares, the put ratio and call spread lose at
  // opposite ends of the spot range: their shared worst loss is only 45,000.
  EXPECT_EQ(margin_requirement(combined, shares, ira), m("45000"));
  std::map<MarginPartKind, Money> parts;
  for (const auto& item : margin_breakdown(combined, shares, ira))
    for (const auto& part : item.parts) parts[part.kind] = parts[part.kind] + part.requirement;
  EXPECT_EQ(parts.at(MarginPartKind::Covered), Money{});
  EXPECT_EQ(parts.at(MarginPartKind::WorstLoss), m("45000"));
}
TEST(TradingMargin, RandomBooksCombineAndTheirBreakdownsAddUp) {
  // The LCG and its upper bits are identical on libc++ and libstdc++.
  std::uint32_t state = 42;
  const auto next = [&]() {
    state = state * 1664525U + 1013904223U;
    return state >> 8;
  };
  std::vector<md::OptionContract> contracts;
  for (const auto* underlying : {"SPY", "QQQ"})
    for (const auto* expiry : {"261022", "261029"})
      for (int strike = 480; strike <= 520; strike += 10)
        for (const char type : {'C', 'P'})
          contracts.push_back(*md::parse_osi(std::string(underlying) + expiry + type + "00" + std::to_string(strike) + "000"));
  struct Book {
    std::vector<Quantity> quantities;
    std::array<Quantity, 2> shares{};
  };
  const auto random_book = [&]() {
    Book book{std::vector<Quantity>(contracts.size()), {}};
    const auto count = 1 + next() % 4;
    for (std::uint32_t i = 0; i < count; ++i) {
      const auto index = next() % contracts.size();
      book.quantities[index] += static_cast<Quantity>(next() % 5) - 2;
    }
    for (auto& shares : book.shares)
      if (next() % 3 == 0) shares = (static_cast<Quantity>(next() % 7) - 3) * 100;
    return book;
  };
  const auto build = [&](const Book& book) {
    std::vector<MarginLeg> legs;
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      const auto q = book.quantities[i];
      if (q == 0) continue;
      const auto& contract = contracts[i];
      const auto intrinsic = std::max(0, contract.type == pricing::OptionType::Call ? 500 - static_cast<int>(contract.strike) :
                                                                                  static_cast<int>(contract.strike) - 500);
      const auto value = Money::from_micros((intrinsic + 3) * 100'000'000LL);
      legs.push_back({contract, q, q < 0 ? value * -q : Money{}, 500.0});
    }
    std::vector<MarginStock> stocks;
    for (std::size_t i = 0; i < book.shares.size(); ++i) {
      const auto shares = book.shares[i];
      if (shares != 0) stocks.push_back({i == 0 ? "SPY" : "QQQ", shares, m("500") * std::abs(shares)});
    }
    return std::pair{legs, stocks};
  };
  const auto opposite = [](Quantity a, Quantity b) { return (a < 0 && b > 0) || (a > 0 && b < 0); };
  for (const auto account : {AccountType::Margin, AccountType::Ira}) {
    const MarginPolicy policy{account, 0, 0};
    int checked = 0;
    for (int iteration = 0; iteration < 6000; ++iteration) {
      SCOPED_TRACE(::testing::Message() << "account=" << static_cast<int>(account) << " iteration=" << iteration);
      const auto a = random_book(), b = random_book();
      auto combined = a;
      bool overlap = false;
      for (std::size_t i = 0; i < contracts.size(); ++i) {
        overlap = overlap || opposite(a.quantities[i], b.quantities[i]);
        combined.quantities[i] += b.quantities[i];
      }
      for (std::size_t i = 0; i < a.shares.size(); ++i) {
        overlap = overlap || opposite(a.shares[i], b.shares[i]);
        combined.shares[i] += b.shares[i];
      }
      const auto [la, sa] = build(a);
      const auto [lb, sb] = build(b);
      const auto [lc, sc] = build(combined);
      if (account == AccountType::Ira && (disallowed_shorts(la, sa, account) || disallowed_shorts(lb, sb, account) ||
                                         disallowed_shorts(lc, sc, account))) continue;
      const auto verify = [&](const auto& legs, const auto& stocks) {
        const auto requirement = margin_requirement(legs, stocks, policy);
        EXPECT_GE(requirement, Money{});
        EXPECT_LE(requirement, detail::pairing_margin_requirement(legs, stocks, policy));
        Money total;
        for (const auto& underlying : margin_breakdown(legs, stocks, policy)) {
          Money parts;
          for (const auto& part : underlying.parts) {
            EXPECT_GE(part.requirement, Money{});
            parts = parts + part.requirement;
          }
          EXPECT_EQ(parts, underlying.requirement);
          total = total + underlying.requirement;
        }
        EXPECT_EQ(total, requirement);
        return requirement;
      };
      const auto ra = verify(la, sa), rb = verify(lb, sb), rc = verify(lc, sc);
      if (!overlap) {
        EXPECT_LE(rc, ra + rb) << "requirements " << ra.str() << " + " << rb.str() << " < " << rc.str()
                               << " A=" << ::testing::PrintToString(a.quantities) << " shares=" << ::testing::PrintToString(a.shares)
                               << " B=" << ::testing::PrintToString(b.quantities) << " shares=" << ::testing::PrintToString(b.shares);
        ++checked;
      }
    }
    EXPECT_GT(checked, 500);
  }
}
TEST(TradingMargin, CashAccountsSecurePutsAndPairOnlyWithShares) {
  const MarginPolicy cash{AccountType::Cash, 0, 0};
  const MarginPolicy ira{AccountType::Ira, 0, 0};
  const std::vector<MarginLeg> put{margin(P4900, -1, "500")};
  const std::vector<MarginLeg> spread{margin(P4900, -1, "500"), margin(P4890, 1)};
  // A short put alone holds its strike in cash, in a cash account or an IRA.
  EXPECT_EQ(margin_requirement(put, {}, cash), m("490000"));
  EXPECT_EQ(margin_requirement(put, {}, ira), m("490000"));
  const auto secured = margin_breakdown(put, {}, cash);
  ASSERT_EQ(secured.size(), 1U);
  ASSERT_EQ(secured[0].parts.size(), 1U);
  EXPECT_EQ(secured[0].parts[0].kind, MarginPartKind::CashSecured);
  // An IRA nets a spread to its width; a cash account nets none, so its long is paid and its short secured.
  EXPECT_EQ(margin_requirement(spread, {}, ira), m("1000"));
  EXPECT_EQ(margin_requirement(spread, {}, cash), m("490000"));
  // A condor in an IRA holds its wider wing, as in a margin account.
  const std::vector<MarginLeg> condor{margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1)};
  EXPECT_EQ(margin_requirement(condor, {}, ira), m("1000"));
  // Neither pairs a straddle: the put is secured and the call naked.
  const std::vector<MarginLeg> strangle{margin(P4900, -1, "500"), margin(C5100, -1, "300")};
  EXPECT_EQ(margin_requirement(strangle, {}, ira), m("490000") + m("300") + naked_requirement(*md::parse_osi(C5100), 5000.0));
  // Forced delivery can leave short shares, but they never replace a put's cash collateral.
  const MarginLeg spy_put{*md::parse_osi("SPY261022P00490000"), -1, m("300"), 500.0};
  for (const auto policy : {cash, ira})
    EXPECT_EQ(margin_requirement({spy_put}, {{"SPY", -100, m("50000")}}, policy), m("124000"));
  // Shares still cover calls in a cash account.
  const MarginLeg covered{*md::parse_osi("SPY261022C00510000"), -1, m("300"), 500.0};
  EXPECT_EQ(margin_requirement({covered}, {{"SPY", 100, m("50000")}}, cash), Money{});
}
TEST(TradingMargin, HouseMarginRaisesNakedAndShortSaleRequirements) {
  const MarginPolicy house{AccountType::Margin, 25, 0};
  // 25% on the naked part only: the buy-back value stays as it is.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500")}, {}, house), m("500") + m("90000").prorate(125, 100));
  // A spread's width is what it can lose, so the house adds nothing.
  EXPECT_EQ(margin_requirement({margin(P4900, -1, "500"), margin(P4890, 1)}, {}, house), m("1000"));
  // Short shares: their value plus the half raised by 25%.
  EXPECT_EQ(margin_requirement({}, {{"SPY", -100, m("50000")}}, house), m("50000") + m("31250"));
  EXPECT_EQ(margin_requirement({}, {{"SPY", -100, m("50000")}}, {}), m("75000"));
}
TEST(TradingMargin, AccountTypesCountTheShortsTheyCannotHold) {
  const auto call = [](Quantity q) { return MarginLeg{*md::parse_osi("SPY261022C00510000"), q, {}, 500.0}; };
  const auto later = [](Quantity q) { return MarginLeg{*md::parse_osi("SPY261023C00520000"), q, {}, 500.0}; };
  EXPECT_EQ(disallowed_shorts({call(-2)}, {}, AccountType::Margin), 0);
  EXPECT_EQ(disallowed_shorts({call(-2)}, {}, AccountType::Cash), 2);
  // 150 shares cover one call.
  EXPECT_EQ(disallowed_shorts({call(-2)}, {{"SPY", 150, {}}}, AccountType::Cash), 1);
  // A long call covers in an IRA when it lasts as long, never in a cash account.
  EXPECT_EQ(disallowed_shorts({call(-1), later(1)}, {}, AccountType::Ira), 0);
  EXPECT_EQ(disallowed_shorts({call(-1), later(1)}, {}, AccountType::Cash), 1);
  EXPECT_EQ(disallowed_shorts({call(1), later(-1)}, {}, AccountType::Ira), 1);
  // Short shares count by the started hundred; puts are never disallowed.
  EXPECT_EQ(disallowed_shorts({margin(P4900, -3)}, {{"SPY", -150, {}}}, AccountType::Ira), 2);
}
TEST(TradingMargin, PortfolioVolShockAndHouseMarginRaiseTheScan) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto c = *md::parse_osi("SPXW261120C05000000");
  const auto symbol = c.osi_symbol();
  const Valuation v{symbol, now, 0.5, 0, 0, 0, 5000, 5000, 1, md::years_between(now, c.expiry_time()), 0.2, true};
  // A long call's worst loss is the price falling; volatility falling deepens it.
  const std::vector<MarginLeg> legs{{c, 1, {}, 5000.0}};
  const auto plain = portfolio_margin_breakdown(legs, {{symbol, v}}, now, md::kNanosPerMinute);
  const auto shocked = portfolio_margin_breakdown(legs, {{symbol, v}}, now, md::kNanosPerMinute, {}, {}, {AccountType::Margin, 0, 5});
  ASSERT_TRUE(plain && shocked);
  EXPECT_EQ((*plain)[0].scan->vol_points, 0);
  EXPECT_EQ((*shocked)[0].scan->vol_points, -5);
  EXPECT_GT((*shocked)[0].requirement, (*plain)[0].requirement);
  const auto house = portfolio_margin_requirement(legs, {{symbol, v}}, now, md::kNanosPerMinute, {}, {}, {AccountType::Margin, 10, 0});
  ASSERT_TRUE(house);
  EXPECT_EQ(*house, (*plain)[0].requirement.prorate(110, 100));
}
TEST(TradingMargin, TheBreakdownNamesWhatHoldsEachShortAndAddsUp) {
  const auto spy = [](std::string_view compact, Quantity quantity, std::string_view value) {
    return MarginLeg{*md::parse_osi(compact), quantity, m(value), 500.0};
  };
  const std::vector<MarginLeg> legs{margin(P4900, -1, "500"), margin(P4890, 1), margin(C5100, -1, "300"), margin(C5110, 1),
                                    spy("SPY261022C00510000", -1, "300"), spy("SPY261022P00490000", -2, "400"),
                                    spy("SPY261022C00520000", -1, "100")};
  const std::vector<MarginStock> stocks{{"SPY", 100, m("50000")}};
  const auto breakdown = margin_breakdown(legs, stocks);
  ASSERT_EQ(breakdown.size(), 2U);
  Money total;
  for (const auto& underlying : breakdown) {
    Money parts;
    for (const auto& part : underlying.parts) parts = parts + part.requirement;
    EXPECT_EQ(parts, underlying.requirement) << underlying.underlying;
    total = total + underlying.requirement;
  }
  EXPECT_EQ(total, margin_requirement(legs, stocks));
  // SPX: the condor holds one wing as its worst loss.
  ASSERT_EQ(breakdown[0].underlying, "SPX");
  ASSERT_EQ(breakdown[0].parts.size(), 1U);
  EXPECT_EQ(breakdown[0].parts[0].kind, MarginPartKind::WorstLoss);
  EXPECT_EQ(breakdown[0].parts[0].requirement, m("1000"));
  EXPECT_EQ(breakdown[0].parts[0].legs.size(), 4U);
  // SPY: the shares cover the 510 call, whose naked cost is the greater, a put
  // straddles the 520 call, and the second put is naked: 9,300 and 9,200.
  ASSERT_EQ(breakdown[1].underlying, "SPY");
  std::map<MarginPartKind, int> kinds;
  for (const auto& part : breakdown[1].parts) ++kinds[part.kind];
  EXPECT_EQ(kinds[MarginPartKind::Covered], 1);
  EXPECT_EQ(kinds[MarginPartKind::Straddle], 1);
  EXPECT_EQ(kinds[MarginPartKind::Naked], 1);
  for (const auto& part : breakdown[1].parts)
    if (part.kind == MarginPartKind::Covered) {
      EXPECT_EQ(part.requirement, Money{});
      const std::vector<std::pair<std::string, Quantity>> expected{{osi("SPY261022C00510000"), -1}, {"SPY", 100}};
      EXPECT_EQ(part.legs, expected);
    }
  EXPECT_EQ(breakdown[1].requirement, m("18500"));
  // Every position is in a part: longs and shares nothing needs are paid in full.
  const auto paid = margin_breakdown({spy("SPY261022P00490000", -1, "200"), spy("SPY261022P00480000", 1, "0"),
                                      spy("SPY261022C00520000", 2, "0")}, {{"SPY", 50, m("25000")}});
  ASSERT_EQ(paid.size(), 1U);
  std::map<MarginPartKind, Money> held;
  for (const auto& part : paid[0].parts) held[part.kind] = held[part.kind] + part.requirement;
  ASSERT_EQ(held.size(), 2U);
  EXPECT_EQ(held[MarginPartKind::Vertical], m("1000"));
  EXPECT_EQ(held[MarginPartKind::Long], Money{});
  std::vector<std::pair<std::string, Quantity>> longs;
  for (const auto& part : paid[0].parts)
    if (part.kind == MarginPartKind::Long) longs.insert(longs.end(), part.legs.begin(), part.legs.end());
  const std::vector<std::pair<std::string, Quantity>> expected_longs{{osi("SPY261022C00520000"), 2}, {"SPY", 50}};
  EXPECT_EQ(longs, expected_longs);
}
TEST(TradingMargin, PortfolioBreakdownNamesTheWorstScanPoint) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto c = *md::parse_osi("SPXW261022P05000000");
  const auto symbol = c.osi_symbol();
  const Valuation v{symbol, now, -0.5, 0.001, 2, -0.1, 5000, 5000, 0.99, 0.1, 0.2, true};
  const std::vector<MarginLeg> legs{{c, -1, {}, 5000.0}};
  const auto breakdown = portfolio_margin_breakdown(legs, {{symbol, v}}, now, md::kNanosPerMinute);
  ASSERT_TRUE(breakdown);
  ASSERT_EQ(breakdown->size(), 1U);
  const auto& item = breakdown->front();
  ASSERT_TRUE(item.scan);
  EXPECT_EQ(item.scan->spot_percent, -8);  // A short put loses most at the lowest price.
  EXPECT_EQ(item.scan->vol_points, 0);
  EXPECT_EQ(item.scan->minimum, m("37.50"));
  EXPECT_EQ(item.requirement, std::max(item.scan->loss, item.scan->minimum));
  EXPECT_EQ(item.requirement, *portfolio_margin_requirement(legs, {{symbol, v}}, now, md::kNanosPerMinute));
  EXPECT_TRUE(item.parts.empty());
}

TEST(TradingMultiLeg, SlippageAppliesToEveryLegAndWaitsForTheNetLimit) {
  for (const bool credit : {true, false}) {
    Chain f;
    auto c = config();
    c.rules.slippage_ticks = 2;
    TradingSession s(c, f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    const auto legs = credit ? std::vector<Leg>{leg(P4900, Side::Sell), leg(P4890, Side::Buy)}
                             : std::vector<Leg>{leg(P4900, Side::Buy), leg(P4890, Side::Sell)};
    ASSERT_TRUE(s.submit(combo("wait", legs, 1, credit ? "-0.80" : "1.20"), f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    s.cancel(1, f.time);
    ASSERT_TRUE(s.submit(combo("fill", legs, 2, credit ? "-0.40" : "1.60"), f.time).decision.ok());
    ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
    EXPECT_EQ(s.snapshot()->recent_fills[0].price, m(credit ? "4.80" : "5.40"));
    EXPECT_EQ(s.snapshot()->recent_fills[1].price, m(credit ? "4.40" : "3.80"));
    EXPECT_EQ(s.snapshot()->recent_orders.back().filled_notional, m(credit ? "-0.80" : "3.20"));
    EXPECT_EQ(s.snapshot()->account.fees, m("2.60"));
    ASSERT_TRUE(s.submit(combo("market", legs, 1, {}), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 4U);
    EXPECT_EQ(s.snapshot()->recent_orders.back().filled_notional, m(credit ? "-0.40" : "1.60"));
  }
}

TEST(TradingMargin, PortfolioScanUsesBothEndpointsAboveTheContractMinimum) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const auto* symbol : {"SPXW261022C05000000", "XSP261022C00500000", "SPY261022C00500000", "AAPL261022C00500000"}) {
    const auto c = *md::parse_osi(symbol);
    const auto osi_symbol = c.osi_symbol();
    Valuation v{osi_symbol, now, 0.5, 0.001, 2, -0.1, c.strike, c.strike * 1.01, 0.99, 0.1, 0.2, true};
    const bool index = md::is_index_underlying(c.underlying);
    for (const Quantity quantity : {-2, 2}) {
      const double shock = quantity < 0 ? (index ? 0.06 : 0.15) : (index ? -0.08 : -0.15);
      const double base = pricing::black_price(c.type, v.forward, c.strike, v.years, v.smile_iv, v.discount);
      const double shocked = pricing::black_price(c.type, v.forward * (1 + shock), c.strike, v.years, v.smile_iv, v.discount);
      const auto expected = std::max(Money::from_double(-static_cast<double>(quantity) * c.multiplier * (shocked - base)), m("75"));
      const auto actual = portfolio_margin_requirement({{c, quantity, {}, v.spot}}, {{osi_symbol, v}}, now, md::kNanosPerMinute);
      ASSERT_TRUE(actual);
      EXPECT_EQ(*actual, expected) << symbol << " quantity " << quantity;
    }
  }
}

TEST(TradingMargin, PortfolioMinimumCoversFarOutOfTheMoneyLongsAndMissingScansAreIncomplete) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto c = *md::parse_osi("SPXW261022C09000000");
  const auto symbol = c.osi_symbol();
  const Valuation v{symbol, now, 0, 0, 0, 0, 5000, 5000, 1, 0.01, 0.01, true};
  const std::vector<MarginLeg> legs{{c, 3, {}, 5000.0}};
  EXPECT_EQ(portfolio_margin_requirement(legs, {{symbol, v}}, now, md::kNanosPerMinute), m("112.50"));
  EXPECT_FALSE(portfolio_margin_requirement(legs, {}, now, md::kNanosPerMinute));
  EXPECT_FALSE(portfolio_margin_requirement(legs, {{symbol, v}}, now + 2 * md::kNanosPerMinute, md::kNanosPerMinute));
}

TEST(TradingMargin, PortfolioScanFindsLossesBetweenItsEndpoints) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::vector<MarginLeg> legs;
  Valuations valuations;
  for (const auto* symbol : {"SPY261022C00090000", "SPY261022C00100000", "SPY261022C00110000"}) {
    const auto c = *md::parse_osi(symbol);
    const auto key = c.osi_symbol();
    legs.push_back({c, c.strike == 100 ? 2 : -1, {}, 95.0});
    valuations.emplace(key, Valuation{key, now, 0, 0, 0, 0, 95, 95, 1, 0, 0.2, true});
  }
  // A short butterfly gains at both endpoints. At the scan point 100.70 it loses $430
  // against its value at 95, more than the $150 minimum for four option contracts.
  EXPECT_EQ(portfolio_margin_requirement(legs, valuations, now, md::kNanosPerMinute), m("430"));
}

TEST(TradingMargin, PortfolioSharesMoveLinearlyAndUnderlyingsCannotOffsetEachOther) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const Quantity shares : {-100, 100}) {
    const StockPosition stock{"SPY", shares, {}, {}, {}};
    EXPECT_EQ(portfolio_margin_requirement({}, {}, now, md::kNanosPerMinute, {{"SPY", stock}}, {{"SPY", 100}}), m("1500"));
    EXPECT_FALSE(portfolio_margin_requirement({}, {}, now, md::kNanosPerMinute, {{"SPY", stock}}));
  }
  const std::map<std::string, StockPosition> stocks{{"SPY", {"SPY", 100, {}, {}, {}}}, {"QQQ", {"QQQ", -100, {}, {}, {}}}};
  EXPECT_EQ(portfolio_margin_requirement({}, {}, now, md::kNanosPerMinute, stocks, {{"SPY", 100}, {"QQQ", 100}}), m("3000"));
  const auto c = *md::parse_osi("SPY261022C00100000");
  const auto symbol = c.osi_symbol();
  const Valuation v{symbol, now, 1, 0, 0, 0, 500, 500, 1, 0, 0.2, true};
  // At expiry in the model, a deep call offsets the shares dollar for dollar.
  EXPECT_EQ(portfolio_margin_requirement({{c, -1, {}, 500.0}}, {{symbol, v}}, now, md::kNanosPerMinute,
      {{"SPY", {"SPY", 100, {}, {}, {}}}}, {{"SPY", 500}}), m("37.50"));
}

TEST(TradingMargin, ProtectivePutsReduceThePortfolioScanIncludingPartialShareHedges) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto put = *md::parse_osi("SPY261022P00500000");
  const auto symbol = put.osi_symbol();
  const Valuation v{symbol, now, -0.5, 0.01, 1, -0.1, 500, 500, 1, md::years_between(now, put.expiry_time()), 0.2, true};
  const std::vector<MarginLeg> legs{{put, 1, {}, 500.0}};
  std::optional<Money> protected_lot;
  for (const Quantity shares : {50, 100, 150}) {
    const std::map<std::string, StockPosition> stocks{{"SPY", {"SPY", shares, {}, {}, {}}}};
    const auto plain = portfolio_margin_requirement({}, {}, now, md::kNanosPerMinute, stocks, {{"SPY", 500}});
    const auto protected_shares = portfolio_margin_requirement(legs, {{symbol, v}}, now, md::kNanosPerMinute, stocks, {{"SPY", 500}});
    ASSERT_TRUE(plain && protected_shares);
    EXPECT_LT(*protected_shares, *plain);
    EXPECT_GE(*protected_shares, m("37.50"));
    if (shares == 100) protected_lot = protected_shares;
    if (shares == 150) { ASSERT_TRUE(protected_lot); EXPECT_GT(*protected_shares, *protected_lot); }
    // Strategy accounts pay for long shares in full: there is no additional
    // share requirement for the put to reduce, and its premium is already paid.
    EXPECT_EQ(margin_requirement(legs, {{"SPY", shares, m("500") * shares}}), Money{});
  }
}

TEST(TradingMultiLeg, PortfolioMarginAllowsAStraddleThatStrategyMarginCannotFund) {
  const auto put = osi("SPXW261022P05000000");
  const auto call = osi("SPXW261022C05000000");
  for (const auto mode : {MarginMode::Strategy, MarginMode::Portfolio}) {
    Chain f;
    auto c = config("50000");
    EXPECT_EQ(c.rules.margin, MarginMode::Strategy);
    c.rules.margin = mode;
    c.rules.buying_power = true;
    // Changing the risk display cannot weaken the portfolio margin scan.
    c.scenarios.spot_percent = {0};
    c.scenarios.vol_points = {0};
    TradingSession s(c, f.time);
    f.define(s, {put, call});
    f.quote(s, {{put, "5", "5.20", -0.5}, {call, "5", "5.20", 0.5}});
    const auto result = s.submit(combo("straddle", {leg(put, Side::Sell), leg(call, Side::Sell)}, 1, {}), f.time);
    if (mode == MarginMode::Strategy) {
      EXPECT_EQ(result.decision.code, Reason::BUYING_POWER);
      EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    } else {
      ASSERT_TRUE(result.decision.ok()) << result.decision.message;
      ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
      const std::vector<MarginLeg> legs{margin(put, -1, "510"), margin(call, -1, "510")};
      const auto expected = portfolio_margin_requirement(legs, s.valuations(), f.time, c.limits.max_valuation_age);
      ASSERT_TRUE(expected);
      EXPECT_EQ(s.snapshot()->buying_power.short_requirement, *expected);
      EXPECT_LT(*expected, margin_requirement(legs));
      // Buying power is equity, cash less the shorts' buy-back value, less the requirement.
      EXPECT_EQ(s.snapshot()->buying_power.available, s.snapshot()->equity - *expected);
      EXPECT_GT(s.snapshot()->buying_power.available, Money{});
      // The snapshot's breakdown carries the scan behind it.
      const auto& detail = s.snapshot()->margin;
      ASSERT_EQ(detail.size(), 1U);
      EXPECT_EQ(detail[0].requirement, *expected);
      ASSERT_TRUE(detail[0].scan);
      EXPECT_TRUE(detail[0].parts.empty());
    }
  }
}

TEST(TradingMultiLeg, AShortStrangleHoldsItsGreaterSideUnderStrategyMargin) {
  Chain f;
  auto c = config("95000");
  c.rules.buying_power = true;
  TradingSession s(c, f.time);
  f.define(s, {P4900, C5100});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {C5100, "3.00", "3.20", 0.25}});
  // Both sides naked would need about 181,000; the greater side plus the other's value fits.
  const auto result = s.submit(combo("strangle", {leg(P4900, Side::Sell), leg(C5100, Side::Sell)}, 1, {}), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->positions.size(), 2U);
  Money values;
  for (const auto& p : snap->positions) values = values + *p.mark * 100;
  EXPECT_EQ(snap->buying_power.short_requirement, m("90000") + values);
  EXPECT_GT(snap->buying_power.available, Money{});
  // The snapshot names what holds it: one straddle of both shorts.
  ASSERT_EQ(snap->margin.size(), 1U);
  EXPECT_EQ(snap->margin[0].requirement, snap->buying_power.short_requirement);
  EXPECT_FALSE(snap->margin[0].scan);
  ASSERT_EQ(snap->margin[0].parts.size(), 1U);
  EXPECT_EQ(snap->margin[0].parts[0].kind, MarginPartKind::Straddle);
  EXPECT_EQ(snap->margin[0].parts[0].legs.size(), 2U);
}

TEST(TradingMultiLeg, CreditSpreadFillsBothLegsTogetherAtTheFarSides) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  // Sell 4900 at the bid, buy 4890 at the ask: a net 0.80 credit.
  auto result = s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 2, "-0.80"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  auto snap = s.snapshot();
  const auto& order = snap->recent_orders.at(0);
  EXPECT_EQ(order.status, OrderStatus::Filled);
  EXPECT_EQ(order.filled_quantity, 2);
  EXPECT_EQ(order.filled_notional, m("-1.60"));
  ASSERT_EQ(snap->recent_fills.size(), 2);
  EXPECT_EQ(snap->recent_fills[0].symbol, P4900);
  EXPECT_EQ(snap->recent_fills[0].side, Side::Sell);
  EXPECT_EQ(snap->recent_fills[0].quantity, 2);
  EXPECT_EQ(snap->recent_fills[0].price, m("5.00"));
  EXPECT_EQ(snap->recent_fills[0].order_id, order.id);
  EXPECT_EQ(snap->recent_fills[1].symbol, P4890);
  EXPECT_EQ(snap->recent_fills[1].price, m("4.20"));
  EXPECT_EQ(snap->account.cash, m("100157.40"));  // + 2 * 100 * 0.80 - 4 * 0.65
  EXPECT_EQ(snap->positions.size(), 2);
  // The spread holds its width, not a naked requirement.
  EXPECT_EQ(snap->buying_power.short_requirement, m("2000"));

  // A 1x2 ratio rests until the book reaches its limit; the legs' sizes bound the units.
  result = s.submit(combo("wider", {leg(P4900, Side::Sell), leg(P4890, Side::Buy, 2)}, 5, "2.00"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.at(1).status, OrderStatus::Working);
  f.quote(s, {{P4900, "8.80", "9.00", -0.40}, {P4890, "4.20", "4.30", -0.30}}, 5);
  snap = s.snapshot();
  const auto& wider = snap->recent_orders.at(1);
  EXPECT_EQ(wider.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(wider.filled_quantity, 2);  // 5 displayed on 4890 covers two units of two
  EXPECT_EQ(wider.filled_notional, m("-0.40"));  // 2 * (2 * 4.30 - 8.80)
  EXPECT_EQ(snap->recent_fills.at(2).quantity, 2);
  EXPECT_EQ(snap->recent_fills.at(3).quantity, 4);
}

TEST(TradingMultiLeg, OrdersAreCheckedAsAWhole) {
  Chain f;
  auto limits = config();
  limits.limits.max_order_contracts = 20;
  TradingSession s(limits, f.time);
  f.define(s, {P4900, P4890, C5100, XSP});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.30}, {XSP, "0.50", "0.52", -0.30}});
  const auto spread = std::vector<Leg>{leg(P4900, Side::Sell), leg(P4890, Side::Buy)};
  const auto code = [&](OrderRequest r) { return s.submit(std::move(r), f.time).decision.code; };
  EXPECT_EQ(code(combo("one", {leg(P4900, Side::Sell)}, 1, "-0.80")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("same", {leg(P4900, Side::Sell), leg(P4900, Side::Buy)}, 1, "0.00")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("mixed", {leg(P4900, Side::Sell), leg(XSP, Side::Buy)}, 1, "-4.00")), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("ratio", {leg(P4900, Side::Sell, 11), leg(P4890, Side::Buy)}, 1, "-0.80")), Reason::INVALID_ORDER);
  auto triggered = combo("trigger", spread, 1, "-0.80");
  triggered.trigger = Trigger{TriggerSource::Underlying, TriggerDirection::AtOrBelow, m("4900")};
  EXPECT_EQ(code(triggered), Reason::INVALID_ORDER);
  auto named = combo("named", spread, 1, "-0.80");
  named.symbol = P4900;
  EXPECT_EQ(code(named), Reason::INVALID_ORDER);
  EXPECT_EQ(code(combo("tick", spread, 1, "-0.83")), Reason::INVALID_TICK);  // SPXW's smallest tick is 0.05
  EXPECT_EQ(code(combo("size", {leg(P4900, Side::Sell, 3), leg(P4890, Side::Buy, 3)}, 7, "-2.40")), Reason::MAX_ORDER_CONTRACTS);
  EXPECT_EQ(code(combo("band", spread, 1, "-4.00")), Reason::PRICE_BAND);
  EXPECT_EQ(code(combo("zero", spread, 0, "-0.80")), Reason::INVALID_ORDER);
  auto day_market = combo("market-day", spread, 1, std::nullopt);
  day_market.tif = TimeInForce::Day;  // market orders are IOC
  EXPECT_EQ(code(day_market), Reason::INVALID_ORDER);
  // Buy-only plans trade single legs.
  AccountRules buy_only;
  buy_only.buy_only = true;
  TradingSession b(config("100000", buy_only), f.time);
  f.define(b, {P4900, P4890});
  f.quote(b, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_EQ(b.submit(combo("long-legs", {leg(P4900, Side::Buy), leg(P4890, Side::Buy)}, 1, "9.40"), f.time).decision.code, Reason::BUY_ONLY);
}

TEST(TradingMultiLeg, BuyingPowerNetsSpreadsThatANakedShortCouldNotAfford) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  OrderRequest naked{"naked", P4900, Side::Sell, OrderType::Limit, TimeInForce::Day, 1, m("5.00"), {}, {}, {}};
  EXPECT_EQ(s.submit(naked, f.time).decision.code, Reason::BUYING_POWER);
  // Resting, a credit spread reserves its width less its credit, plus fees.
  auto result = s.submit(combo("rest", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 3, "-1.20"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("2643.90"));  // 3 * (1000 - 120) + 6 * 0.65
  // Ten more units would need more than the account has.
  result = s.submit(combo("too-many", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 9, "-0.80"), f.time);
  EXPECT_EQ(result.decision.code, Reason::BUYING_POWER);
  // Filled, the account keeps the credit and holds the width.
  result = s.submit(combo("fill", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 5, "-0.80"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, m("10393.50"));  // + 5 * 80 - 10 * 0.65
  EXPECT_EQ(snap->buying_power.short_requirement, m("5000"));
  EXPECT_EQ(snap->buying_power.available, m("10393.50") - m("5000") - m("2643.90"));
}
TEST(TradingMultiLeg, ADebitSpreadBesideADiagonalNeedsOnlyItsDebit) {
  // The debit spread's long must not be spent covering the diagonal's short:
  // held together the book can lose only the diagonal's 10-point width.
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("50000", rules), f.time);
  const auto far_long = osi("SPXW261026P04900000"), far_short = osi("SPXW261026P04890000");
  f.define(s, {P4900, LATER, far_long, far_short});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER, "5.40", "5.60", -0.28},
              {far_long, "8.00", "8.20", -0.32}, {far_short, "7.40", "7.60", -0.30}});
  ASSERT_TRUE(s.submit(combo("diagonal", {leg(P4900, Side::Sell), leg(LATER, Side::Buy)}, 1, "0.60"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("1000"));
  const auto spread = s.submit(combo("debit", {leg(far_long, Side::Buy), leg(far_short, Side::Sell)}, 1, "0.80"), f.time);
  ASSERT_TRUE(spread.decision.ok()) << spread.decision.message;
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("1000"));
}

OrderRequest single(std::string client, const std::string& symbol, Side side, std::optional<std::string_view> limit = {}) {
  return {std::move(client), symbol, side, limit ? OrderType::Limit : OrderType::Market, limit ? TimeInForce::Day : TimeInForce::Ioc,
          1, limit ? std::optional<Money>(m(*limit)) : std::nullopt, {}, {}, {}};
}

TEST(TradingBuyingPower, BuyingBackALongThatReCoversAWorkingSellIsAllowed) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890, C5100});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.25}});
  ASSERT_TRUE(s.submit(single("long", P4890, Side::Buy), f.time).decision.ok());
  // A working sell against the long reserves the width less its credit.
  ASSERT_TRUE(s.submit(single("sell", P4900, Side::Sell, "5.60"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("440.65"));
  // Selling the long is a close, so it is allowed; the working sell is now naked.
  ASSERT_TRUE(s.submit(single("close", P4890, Side::Sell), f.time).decision.ok());
  EXPECT_LT(s.snapshot()->buying_power.available, Money{});
  // Buying the long back costs 420.65 but re-covers the working sell, which then
  // reserves 440.65 again instead of a naked requirement: it frees buying power.
  const auto back = s.submit(single("back", P4890, Side::Buy, "4.20"), f.time);
  ASSERT_TRUE(back.decision.ok()) << back.decision.message;
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->buying_power.available, m("9117.40"));
  // A buy that frees nothing still needs the buying power.
  ASSERT_TRUE(s.submit(single("cover-gone", P4890, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(single("unrelated", C5100, Side::Buy, "3.20"), f.time).decision.code, Reason::BUYING_POWER);
}
TEST(TradingBuyingPower, AFillThatReCoversPartOfAWorkingSellFillsAsItWasAccepted) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  auto longs = single("longs", P4890, Side::Buy);
  longs.quantity = 2;
  ASSERT_TRUE(s.submit(longs, f.time).decision.ok());
  auto sell = single("sell", P4900, Side::Sell, "5.60");
  sell.quantity = 2;
  ASSERT_TRUE(s.submit(sell, f.time).decision.ok());
  auto close = single("close", P4890, Side::Sell);
  close.quantity = 2;
  ASSERT_TRUE(s.submit(close, f.time).decision.ok());
  const auto short_of_power = s.snapshot()->buying_power.available;
  ASSERT_LT(short_of_power, Money{});
  // One long back re-covers one of the two working sells: still short of buying
  // power, but less so than without it. It is accepted, so its fill goes through too.
  const auto back = s.submit(single("back", P4890, Side::Buy, "4.20"), f.time);
  ASSERT_TRUE(back.decision.ok()) << back.decision.message;
  const auto& order = s.snapshot()->recent_orders.back();
  EXPECT_EQ(order.status, OrderStatus::Filled) << order.reason.message;
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
  EXPECT_LT(s.snapshot()->buying_power.available, Money{});
  EXPECT_GT(s.snapshot()->buying_power.available, short_of_power);
}
TEST(TradingBuyingPower, LeggingIntoASpreadReservesOnlyItsWidth) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  {
    // Short first: the put is naked until its protection is bought.
    TradingSession s(config("10000", rules), f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.submit(single("short-first", P4900, Side::Sell), f.time).decision.code, Reason::BUYING_POWER);
  }
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(single("long", P4890, Side::Buy), f.time).decision.ok());
  // Selling against the long reserves the spread's width less the credit, not a naked requirement.
  ASSERT_TRUE(s.submit(single("rest", P4900, Side::Sell, "5.40"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("460.65"));  // 1000 - 540 + 0.65
  ASSERT_TRUE(s.cancel(2, f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("short", P4900, Side::Sell), f.time).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->positions.size(), 2);
  EXPECT_EQ(snap->account.cash, m("10078.70"));  // - 420 + 500 - 2 * 0.65
  EXPECT_EQ(snap->buying_power.short_requirement, m("1000"));
  EXPECT_EQ(snap->buying_power.available, m("9078.70"));
}

TEST(TradingDefinedRisk, ShortsNeedALongOfTheirTypeExpiringWithThemOrLater) {
  Chain f;
  AccountRules rules;
  rules.defined_risk = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {P4900, P4890, LATER, C5100, C5110});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {LATER, "4.50", "4.70", -0.27},
              {C5100, "3.00", "3.20", 0.30}, {C5110, "2.50", "2.70", 0.27}});
  // A lone short is naked, and a call does not cover a put.
  EXPECT_EQ(s.submit(single("naked", P4900, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  ASSERT_TRUE(s.submit(single("call", C5110, Side::Buy), f.time).decision.ok());
  EXPECT_EQ(s.submit(single("still naked", P4900, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  // A long put, of any strike, expiring with the short or later covers it.
  ASSERT_TRUE(s.submit(single("later", LATER, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("covered", P4900, Side::Sell), f.time).decision.ok());
  // Selling the long would uncover the short; closing the short first is always allowed.
  EXPECT_EQ(s.submit(single("uncover", LATER, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  ASSERT_TRUE(s.submit(single("close short", P4900, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("close long", LATER, Side::Sell), f.time).decision.ok());
  // A spread opens as one order; a calendar with the short expiring later does not.
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.ok());
  EXPECT_EQ(s.submit(combo("backwards", {leg(LATER, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.code,
            Reason::DEFINED_RISK);
  // Without the rule the same lone short goes through.
  TradingSession any(config("100000", {}), f.time);
  f.define(any, {P4900});
  f.quote(any, {{P4900, "5.00", "5.20", -0.30}});
  EXPECT_TRUE(any.submit(single("naked", P4900, Side::Sell), f.time).decision.ok());
}

TEST(TradingDefinedRisk, OpenOrdersCountAsIfTheirSellsFilled) {
  Chain f;
  AccountRules rules;
  rules.defined_risk = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {P4900, P4890, LATER, LATER_4900});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {LATER, "4.50", "4.70", -0.27},
              {LATER_4900, "5.50", "5.70", -0.29}});
  ASSERT_TRUE(s.submit(single("long", LATER, Side::Buy), f.time).decision.ok());
  // A working order to sell the long keeps a short from leaning on it.
  const auto sell_long = s.submit(single("sell long", LATER, Side::Sell, "5.00"), f.time);
  ASSERT_TRUE(sell_long.decision.ok());
  const auto refused = s.submit(single("short", P4900, Side::Sell), f.time).decision;
  EXPECT_EQ(refused.code, Reason::DEFINED_RISK);
  EXPECT_NE(refused.message.find("open orders"), std::string::npos) << refused.message;
  ASSERT_TRUE(s.cancel(*sell_long.order_id, f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("short again", P4900, Side::Sell), f.time).decision.ok());
  // A working short keeps the long it needs: with another short resting, only one
  // long is spare.
  ASSERT_TRUE(s.submit(single("long 2", LATER, Side::Buy), f.time).decision.ok());
  const auto resting = s.submit(single("short 2", P4890, Side::Sell, "4.60"), f.time);
  ASSERT_TRUE(resting.decision.ok());
  EXPECT_EQ(s.submit(single("sell a long", LATER, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
  // A spread working as one order brings its own long.
  ASSERT_TRUE(s.cancel(*resting.order_id, f.time).decision.ok());
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4890, Side::Sell), leg(LATER_4900, Side::Buy)}, 1, "1.00"), f.time).decision.ok());
  const auto sold = s.submit(single("sell a long now", LATER, Side::Sell), f.time);
  EXPECT_TRUE(sold.decision.ok()) << sold.decision.message;
}

TEST(TradingAccountType, ACashAccountSecuresPutsAndSellsNoNakedCalls) {
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Cash;
  rules.buying_power = true;
  TradingSession s(config("600000", rules), f.time);
  f.define(s, {P4900, P4890, C5100, C5110});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.25},
              {C5110, "2.00", "2.20", 0.22}});
  const auto naked = s.submit(single("call", C5100, Side::Sell), f.time).decision;
  EXPECT_EQ(naked.code, Reason::ACCOUNT_TYPE);
  EXPECT_NE(naked.message.find("cash account"), std::string::npos) << naked.message;
  // A call spread's long covers nothing in a cash account.
  EXPECT_EQ(s.submit(combo("call spread", {leg(C5100, Side::Sell), leg(C5110, Side::Buy)}, 1, {}), f.time).decision.code,
            Reason::ACCOUNT_TYPE);
  // A short put holds its strike: one fits in 600,000, a second does not, even as a spread.
  ASSERT_TRUE(s.submit(single("put", P4900, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("490000"));
  ASSERT_EQ(s.snapshot()->margin.size(), 1U);
  EXPECT_EQ(s.snapshot()->margin[0].parts.at(0).kind, MarginPartKind::CashSecured);
  EXPECT_EQ(s.submit(combo("put spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.code,
            Reason::BUYING_POWER);
}

TEST(TradingAccountType, AnIraNetsSpreadsButHoldsNoNakedCall) {
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {C5100, C5110});
  f.quote(s, {{C5100, "3.00", "3.20", 0.25}, {C5110, "2.00", "2.20", 0.22}});
  ASSERT_TRUE(s.submit(combo("call spread", {leg(C5100, Side::Sell), leg(C5110, Side::Buy)}, 1, {}), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("1000"));
  // Selling the long alone would leave the short call naked.
  EXPECT_EQ(s.submit(single("sell long", C5110, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
  // Closing both together, or the short first, is allowed.
  ASSERT_TRUE(s.submit(single("buy back", C5100, Side::Buy), f.time).decision.ok());
  // A working sell of the long keeps a new short from leaning on it.
  ASSERT_TRUE(s.submit(single("sell long later", C5110, Side::Sell, "2.50"), f.time).decision.ok());
  const auto refused = s.submit(single("short", C5100, Side::Sell), f.time).decision;
  EXPECT_EQ(refused.code, Reason::ACCOUNT_TYPE);
  EXPECT_NE(refused.message.find("open orders"), std::string::npos) << refused.message;
}

TEST(TradingAccountType, AnIraCannotAbandonTheLongCoveringItsShortCall) {
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  TradingSession s(config("1000000", rules), f.time);
  f.define(s, {C5100, C5110, C5120});
  f.quote(s, {{C5100, "3.00", "3.20", 0.25}, {C5110, "2.00", "2.20", 0.22}, {C5120, "1.00", "1.20", 0.20}});
  ASSERT_TRUE(s.submit(combo("call spread", {leg(C5100, Side::Sell), leg(C5110, Side::Buy)}, 1, {}), f.time).decision.ok());
  const auto no_bid = [&](const std::string& symbol) {
    f.time += md::kNanosPerSecond;
    s.on_quotes({{symbol, ++f.observation, f.time, std::nullopt, m("0.05"), 0, 10}}, {}, f.time);
  };
  no_bid(C5110);
  EXPECT_EQ(s.abandon(C5110, f.time).decision.code, Reason::ACCOUNT_TYPE);
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  // Another long is spare while the spread's long still covers the short.
  ASSERT_TRUE(s.submit(single("spare long", C5120, Side::Buy), f.time).decision.ok());
  no_bid(C5120);
  EXPECT_TRUE(s.abandon(C5120, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  EXPECT_EQ(s.abandon(C5110, f.time).decision.code, Reason::ACCOUNT_TYPE);
}

TEST(TradingAccountType, AbandonCancelsItsOwnSalesBeforeCountingCovers) {
  for (const auto type : {AccountType::Cash, AccountType::Ira}) {
    for (const bool bracket : {false, true}) {
      Chain f;
      AccountRules rules;
      rules.account_type = type;
      rules.buying_power = true;
      TradingSession s(config("100000", rules), f.time);
      f.define(s, {C5100});
      f.quote(s, {{C5100, "3.00", "3.20", 0.25}});
      auto request = single("long", C5100, Side::Buy);
      if (bracket) request.bracket = Bracket{{}, ExitSpec{{}, m("3.50")}};
      ASSERT_TRUE(s.submit(request, f.time).decision.ok());
      if (!bracket) { ASSERT_TRUE(s.submit(single("sell long", C5100, Side::Sell, "3.50"), f.time).decision.ok()); }
      ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
      const auto sale = s.snapshot()->open_orders[0].id;
      f.time += md::kNanosPerSecond;
      s.on_quotes({{C5100, ++f.observation, f.time, std::nullopt, m("0.05"), 0, 10}}, {}, f.time);
      const auto result = s.abandon(C5100, f.time);
      ASSERT_TRUE(result.decision.ok()) << result.decision.message;
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_TRUE(s.snapshot()->open_orders.empty());
      EXPECT_EQ(s.snapshot()->recent_orders.at(sale - 1).reason.code, Reason::POSITION_CLOSED);
    }
  }
}

TEST(TradingAccountType, AbandonReleasesTheCancelledSalesCashReservation) {
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  TradingSession s(config("500000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.3}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, {}), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("sell long", P4890, Side::Sell, "4.50"), f.time).decision.ok());
  f.time += md::kNanosPerSecond;
  s.on_quotes({{P4890, ++f.observation, f.time, std::nullopt, m("0.05"), 0, 10}}, {}, f.time);
  // The one remaining short put is fully cash-secured. The cancelled sale
  // must not also reserve cash as though it could open another short put.
  const auto result = s.abandon(P4890, f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("490000"));
  EXPECT_GT(s.snapshot()->buying_power.available, Money{});
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingAccountType, IraBracketExitsReserveTheirLongOnlyOnce) {
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {C5100, C5110});
  f.quote(s, {{C5100, "3.00", "3.20", 0.25}, {C5110, "2.00", "2.20", 0.22}});
  auto entry = single("bracketed", C5110, Side::Buy);
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("1.00")}, {}},
                          ExitSpec{{}, m("3.00")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 2U);
  EXPECT_EQ(s.submit(single("uncovered", C5100, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
  ASSERT_TRUE(s.submit(single("plain", C5110, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("covered", C5100, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(single("extra", C5100, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
  // The target sells only the spare long; the other keeps the short covered.
  f.quote(s, {{C5110, "3.00", "3.20", 0.22}});
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingAccountType, HouseAndVolShocksReachSessionBuyingPower) {
  const auto compare = [](MarginMode mode, std::int64_t house, std::int64_t shock) {
    Chain f;
    AccountRules rules;
    rules.buying_power = true;
    rules.margin = mode;
    rules.house_margin_percent = house;
    rules.pm_vol_shock = shock;
    TradingSession s(config("1000000", rules), f.time);
    f.define(s, {C5100});
    f.quote(s, {{C5100, "3.00", "3.20", 0.25}});
    EXPECT_TRUE(s.submit(single("short", C5100, Side::Sell), f.time).decision.ok());
    return s.snapshot()->buying_power.short_requirement;
  };
  EXPECT_EQ(compare(MarginMode::Strategy, 25, 0), m("310") + m("90000").prorate(125, 100));
  const auto plain = compare(MarginMode::Portfolio, 0, 0);
  EXPECT_GT(compare(MarginMode::Portfolio, 0, 5), plain);
  EXPECT_EQ(compare(MarginMode::Portfolio, 25, 0), plain.prorate(125, 100));
}

TEST(TradingAccountType, RulesKeepCashAndIraOnStrategyMarginWithBuyingPower) {
  AccountRules rules;
  rules.account_type = AccountType::Cash;
  EXPECT_THROW(validate_rules(rules), TradingError);  // Buying power is not enforced.
  rules.buying_power = true;
  EXPECT_NO_THROW(validate_rules(rules));
  rules.margin = MarginMode::Portfolio;
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules = {};
  rules.house_margin_percent = 401;
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules.house_margin_percent = 400;
  rules.pm_vol_shock = 51;
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST(TradingAccountType, AccountSettingsSurviveRecovery) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-account-type-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "ira.jsonl").string();
  Chain f;
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  rules.house_margin_percent = 30;
  std::string expected, head;
  {
    auto journal = test::create_journal(path);
    TradingSession s(config("600000", rules), f.time, journal);
    f.define(s, {C5100});
    f.quote(s, {{C5100, "3.00", "3.20", 0.25}});
    EXPECT_EQ(s.submit(single("call", C5100, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto s = TradingSession::recover(FileJournal::read(path, head), test::resume_journal(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.config().rules, rules);
  std::filesystem::remove_all(directory);
}

TEST(TradingDefinedRisk, ABracketsExitsSellTheLongOnce) {
  Chain f;
  AccountRules rules;
  rules.defined_risk = true;
  TradingSession s(config("100000", rules), f.time);
  f.define(s, {P4900, LATER});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER, "4.50", "4.70", -0.27}});
  auto entry = single("bracketed", LATER, Side::Buy);
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.00")}, {}},
                          ExitSpec{{}, m("5.00")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 2U);  // the stop and the target
  ASSERT_TRUE(s.submit(single("plain", LATER, Side::Buy), f.time).decision.ok());
  // The exits sell one long between them, leaving the other to cover one short.
  ASSERT_TRUE(s.submit(single("short", P4900, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(single("short 2", P4900, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
}

TEST(TradingDefinedRisk, CountsShortsNoLongCovers) {
  const auto contract = [](std::string_view s) { return *md::parse_osi(s); };
  const auto legs = [&](std::vector<std::pair<std::string_view, Quantity>> held) {
    std::vector<MarginLeg> out;
    for (const auto& [symbol, q] : held) out.push_back({contract(symbol), q, {}, std::nullopt});
    return out;
  };
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -2}, {"SPXW261022P04890000", 1}})), 1);
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -1}, {"SPXW261023P04890000", 1}})), 0);  // a later long
  EXPECT_EQ(naked_shorts(legs({{"SPXW261023P04900000", -1}, {"SPXW261022P04890000", 1}})), 1);  // an earlier one
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022C05100000", -1}, {"SPXW261022P04890000", 1}})), 1);  // another type
  EXPECT_EQ(naked_shorts(legs({{"SPY261022P00500000", -1}, {"SPXW261022P04890000", 1}})), 1);   // another underlying
  // The latest shorts take the earliest long that covers them, covering the most.
  EXPECT_EQ(naked_shorts(legs({{"SPXW261022P04900000", -1}, {"SPXW261023P04900000", -1},
                               {"SPXW261022P04890000", 1}, {"SPXW261023P04890000", 1}})), 0);
}

TEST(TradingBuyingPower, UncoveringAShortNeedsBuyingPowerButClosingItNeverDoes) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("10000", rules), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("spread", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.80"), f.time).decision.ok());
  // Selling the long alone would leave a naked put the account cannot carry.
  const auto uncover = s.submit(single("sell-long", P4890, Side::Sell), f.time).decision;
  EXPECT_EQ(uncover.code, Reason::BUYING_POWER);
  EXPECT_NE(uncover.message.find("uncovers a short"), std::string::npos) << uncover.message;
  // Closing both together frees buying power, so it is always allowed.
  const auto close = s.submit(combo("close", {leg(P4900, Side::Buy), leg(P4890, Side::Sell)}, 1, "1.20"), f.time);
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // So is buying the short back first, then selling the long.
  ASSERT_TRUE(s.submit(combo("again", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.80"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("buy-short", P4900, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("sell-long-after", P4890, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingBuyingPower, ProtectionIsAllowedWhenBuyingPowerIsNegative) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  auto c = config("100000", rules);
  c.limits.max_daily_loss = m("1000000");
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(single("naked", P4900, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, m("9989.35"));  // 100499.35 - 510 - 90000
  // A sharp fall: the put is deep in the money and its naked requirement grows.
  ++f.observation;
  f.time += md::kNanosPerSecond;
  std::vector<QuoteObservation> quotes{{P4900, f.observation, f.time, m("119"), m("121"), 10, 10},
                                       {P4890, f.observation, f.time, m("114"), m("116"), 10, 10}};
  std::vector<Valuation> valuations;
  for (const auto& symbol : {P4900, P4890})
    valuations.push_back({symbol, f.time, -0.9, 0.001, 2.0, -0.1, 4800, 4810, 0.99,
                          md::years_between(f.time, md::parse_osi(symbol)->expiry_time()), 0.20, true});
  s.on_quotes(quotes, valuations, f.time);
  EXPECT_EQ(s.snapshot()->buying_power.available, m("-7500.65"));  // 100499.35 - 12000 - 96000
  EXPECT_EQ(s.submit(single("more", P4890, Side::Sell, "114"), f.time).decision.code, Reason::BUYING_POWER);
  // Buying protection frees far more than it costs.
  const auto protect = s.submit(single("protect", P4890, Side::Buy), f.time);
  ASSERT_TRUE(protect.decision.ok()) << protect.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->buying_power.available, m("87898.70"));  // 88898.70 - 1000
}

TEST(TradingMultiLeg, CalendarsHoldOnlyTheirDebit) {
  Chain f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(config("2000", rules), f.time);
  f.define(s, {P4900, LATER_4900});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER_4900, "6.00", "6.20", -0.31}});
  // Sell the near put, buy the next day's: a 1.20 debit that the later put covers.
  const auto legs = std::vector<Leg>{leg(P4900, Side::Sell), leg(LATER_4900, Side::Buy)};
  ASSERT_TRUE(s.submit(combo("rest", legs, 5, "1.00"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("506.50"));  // 5 * 100 * 1.00 + 10 * 0.65
  const auto result = s.submit(combo("calendar", legs, 1, "1.20"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_EQ(snap->buying_power.short_requirement, Money{});
  EXPECT_EQ(snap->account.cash, m("1878.70"));  // - 120 - 2 * 0.65
}

TEST(TradingMultiLeg, AWorkingComboIsOnePendingExposure) {
  Chain f;
  auto tight = config();
  tight.limits.per_underlying = {100'000, 1e9};
  TradingSession s(tight, f.time);
  f.define(s, {P4900, C5100});
  f.quote(s, {{P4900, "5.00", "5.20", -0.50}, {C5100, "5.00", "5.20", 0.50}});
  // Either leg alone reaches 1 * 100 * 0.5 * 5000 = $250,000 of delta.
  OrderRequest call{"call", C5100, Side::Buy, OrderType::Limit, TimeInForce::Day, 1, m("4.60"), {}, {}, {}};
  EXPECT_EQ(s.submit(call, f.time).decision.code, Reason::DELTA_LIMIT);
  // Together, a straddle's legs offset and fill together.
  const auto result = s.submit(combo("straddle", {leg(P4900, Side::Buy), leg(C5100, Side::Buy)}, 1, "9.00"), f.time);
  ASSERT_TRUE(result.decision.ok()) << result.decision.message;
  const auto risk = s.snapshot()->risk.underlyings.at("SPX");
  EXPECT_DOUBLE_EQ(risk.reachable.delta_low, 0);
  EXPECT_DOUBLE_EQ(risk.reachable.delta_high, 0);
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Working);
}

TEST(TradingMultiLeg, CombosSurviveRecoveryAndCancelAtTheSessionEnd) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-multileg-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "combo.jsonl").string();
  Chain f;
  std::string expected, head;
  {
    auto journal = test::create_journal(path);
    TradingSession s(config(), f.time, journal);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    ASSERT_TRUE(s.submit(combo("rest", {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}, 1, "-0.95"), f.time).decision.ok());
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto s = TradingSession::recover(FileJournal::read(path, head), test::resume_journal(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  const auto order = s.snapshot()->open_orders.at(0);
  ASSERT_EQ(order.request.legs.size(), 2);
  EXPECT_EQ(order.request.legs[1].symbol, P4890);
  EXPECT_EQ(order.request.limit_price, m("-0.95"));
  EXPECT_EQ(order_symbols(order.request), (std::vector<std::string>{P4900, P4890}));
  // A DAY combo ends with its legs' regular session.
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 20);
  ASSERT_TRUE(s.on_quotes({}, {}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.at(0).reason.code, Reason::DAY_END);
  std::filesystem::remove_all(directory);
}


Bracket spread_bracket(TriggerSource source = TriggerSource::Combo) {
  return {ExitSpec{Trigger{source, TriggerDirection::AtOrAbove, m(source == TriggerSource::Combo ? "2.00" : "5010")}, {}},
          ExitSpec{{}, m("0.40")}};
}
std::vector<Leg> credit_legs() { return {leg(P4900, Side::Sell), leg(P4890, Side::Buy)}; }
std::vector<Leg> close_legs() { return {leg(P4900, Side::Buy), leg(P4890, Side::Sell)}; }

TEST(TradingMultiLeg, BracketCreatesAndGrowsAtomicExitsAndTagsOpeningTrades) {
  Chain f;
  auto c = config();
  c.rules.defined_risk = true;
  c.rules.buying_power = true;
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  auto r = combo("entry", credit_legs(), 3, "-0.80");
  r.bracket = spread_bracket();
  r.tags = {" Put-credit-10d-5w ", "Test"};
  r.note = " Entry plan ";
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  auto snap = s.snapshot();
  ASSERT_EQ(snap->recent_orders.size(), 3U);
  EXPECT_EQ(snap->recent_orders[1].status, OrderStatus::Armed);
  EXPECT_EQ(snap->recent_orders[1].request.legs, close_legs());
  EXPECT_EQ(snap->recent_orders[2].request.quantity, 1);
  EXPECT_EQ(snap->recent_orders[1].oco, 3U);
  ASSERT_EQ(snap->annotations.size(), 2U);
  EXPECT_EQ(snap->annotations.at("1").tags, (std::vector<std::string>{"put-credit-10d-5w", "test"}));
  EXPECT_EQ(snap->annotations.at("2").note, "Entry plan");
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 2);
  snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(snap->recent_orders[1].request.quantity, 3);
  EXPECT_EQ(snap->recent_orders[2].request.quantity, 3);
  EXPECT_EQ(snap->annotations.size(), 2U);  // additions remain the same round trips
  EXPECT_EQ(snap->buying_power.reserved, m("3.90"));  // OCO pair reserves fees once
  s.trip_kill("reduce only", f.time);
  f.quote(s, {{P4900, "4.00", "4.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 3);
  snap = s.snapshot();
  EXPECT_TRUE(snap->positions.empty());
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::OCO_FILLED);
  EXPECT_EQ(snap->recent_orders[2].filled_notional, m("0.60"));  // improved net 0.20, three units
  EXPECT_EQ(snap->annotations.size(), 2U);
}

TEST(TradingMultiLeg, StopsReadDisplayedComboNetOrUnderlyingAndUseSlippageAndSize) {
  for (const auto source : {TriggerSource::Combo, TriggerSource::Underlying}) {
    Chain f;
    auto c = config();
    c.rules.slippage_ticks = 1;
    c.rules.defined_risk = true;
    TradingSession s(c, f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    auto r = combo("entry", credit_legs(), 2, {});
    r.bracket = spread_bracket(source);
    ASSERT_TRUE(s.submit(r, f.time).decision.ok());
    s.trip_kill("close only", f.time);
    // Displayed net 1.90 is below the stop, although slipped net is 2.10.
    f.quote(s, {{P4900, "5.70", "5.90", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Armed);
    f.quote(s, {{P4900, "5.80", "6.00", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
    if (source == TriggerSource::Underlying) {
      EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Armed);
      s.on_quotes({}, {{P4900, f.time, -0.30, 0.001, 2, -0.1, 5010, 5010, 0.99, 0.1, 0.2, true}}, f.time);
    }
    const auto snap = s.snapshot();
    EXPECT_EQ(snap->recent_orders[1].filled_quantity, 1);
    // The displayed size took one unit: the stop re-arms for the other, and the
    // target stays, shrunk to it.
    EXPECT_EQ(snap->recent_orders[1].status, OrderStatus::Armed);
    EXPECT_EQ(snap->recent_orders[1].remaining(), 1);
    EXPECT_TRUE(snap->recent_orders[2].open());
    EXPECT_EQ(snap->recent_orders[2].request.quantity, 1);
    ASSERT_EQ(snap->recent_fills.size(), 4U);
    EXPECT_EQ(snap->recent_fills[2].price, m("6.10"));
    EXPECT_EQ(snap->recent_fills[3].price, m("3.90"));
    ASSERT_EQ(snap->positions.size(), 2U);
    EXPECT_EQ(std::abs(snap->positions[0].position.quantity), 1);
    EXPECT_EQ(std::abs(snap->positions[1].position.quantity), 1);
    // The next quote still at the stop closes the rest and cancels the target.
    f.quote(s, {{P4900, "5.80", "6.00", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
    if (source == TriggerSource::Underlying)
      s.on_quotes({}, {{P4900, f.time, -0.30, 0.001, 2, -0.1, 5010, 5010, 0.99, 0.1, 0.2, true}}, f.time);
    EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Filled);
    EXPECT_EQ(s.snapshot()->recent_orders[2].reason.code, Reason::OCO_FILLED);
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingMultiLeg, TargetWaitsUntilTheSlippedNetFitsAndDebitEntriesReceiveCredit) {
  for (const bool credit : {true, false}) {
    Chain f;
    auto c = config();
    c.rules.slippage_ticks = 1;
    TradingSession s(c, f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    auto r = combo("entry", credit ? credit_legs() : close_legs(), 1, {});
    r.bracket = Bracket{{}, ExitSpec{{}, m(credit ? "0.40" : "-1.60")}};
    ASSERT_TRUE(s.submit(r, f.time).decision.ok());
    f.quote(s, {{P4900, credit ? "4.05" : "5.75", credit ? "4.25" : "5.95", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Working);
    f.quote(s, {{P4900, credit ? "4.00" : "6.00", credit ? "4.20" : "6.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Filled);
    EXPECT_EQ(s.snapshot()->recent_orders[1].filled_notional, m(credit ? "0.40" : "-1.60"));
  }
}

TEST(TradingMultiLeg, AComboStopLimitRestsAtItsNetOnceReached) {
  for (const bool held : {false, true}) {
    Chain f;
    TradingSession s(config(), f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    // Stop when the displayed closing net reaches 2.00, paying at most 2.20 for it.
    const ExitSpec stop_limit{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("2.00")}, m("2.20")};
    auto entry = combo("entry", credit_legs(), 2, "-0.80");
    if (!held) entry.bracket = Bracket{stop_limit, {}};
    ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
    OrderId stop = 2;
    if (held) {
      // A held spread's stop-limit: the submitted order is that stop, a GTC limit with its trigger.
      auto exits = combo("exits", close_legs(), 2, "2.20", TimeInForce::Gtc);
      exits.trigger = stop_limit.trigger;
      exits.exits_only = true;
      exits.bracket = Bracket{stop_limit, {}};
      const auto placed = s.submit(exits, f.time);
      ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
      stop = *placed.order_id;
    }
    const auto order = [&] { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(stop - 1)); };
    EXPECT_EQ(order().role, OrderRole::StopLoss);
    EXPECT_EQ(order().status, OrderStatus::Armed);
    EXPECT_EQ(order().request.type, OrderType::Limit);
    // The net gaps to 3.10: the stop triggers and waits at its 2.20 limit.
    f.quote(s, {{P4900, "6.90", "7.10", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(order().status, OrderStatus::Working);
    EXPECT_EQ(s.snapshot()->positions.size(), 2U);
    f.quote(s, {{P4900, "6.00", "6.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(order().status, OrderStatus::Filled);
    EXPECT_EQ(order().filled_notional, m("4.40"));  // a net of 2.20 for each of two units
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingMultiLeg, ATrailingComboStopFollowsTheClosingMidDown) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 1, {}), f.time).decision.ok());
  // The stop alone: a closing market IOC that buys the spread back when its mid rises 0.50 off its low.
  auto exits = combo("trail", close_legs(), 1, {});
  const Trigger stop{.source = TriggerSource::Combo, .direction = TriggerDirection::AtOrAbove, .level = m("9.00"),
                     .reference = TriggerReference::Mid, .trail = Trail{TrailUnit::Amount, m("0.50")}};
  exits.trigger = stop;
  exits.bracket = Bracket{ExitSpec{stop, {}}, {}};
  exits.exits_only = true;
  const auto placed = s.submit(exits, f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto level = [&] { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(*placed.order_id - 1)).request.trigger->level; };
  EXPECT_EQ(level(), m("1.50"));  // closing mid 5.10 - 4.10, plus 0.50
  f.quote(s, {{P4900, "4.40", "4.60", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_EQ(level(), m("0.90"));
  f.quote(s, {{P4900, "4.50", "4.70", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_EQ(level(), m("0.90"));
  EXPECT_FALSE(s.snapshot()->positions.empty());
  f.quote(s, {{P4900, "4.90", "5.10", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.at(static_cast<std::size_t>(*placed.order_id - 1)).filled_notional, m("1.10"));
}

TEST(TradingMultiLeg, HeldExitsValidateHoldingsAndCanModifyCancelAndCloseUnderKill) {
  Chain f;
  auto c = config();
  c.rules.defined_risk = true;
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  auto exits = combo("none", close_legs(), 1, "0.40", TimeInForce::Gtc);
  exits.bracket = spread_bracket();
  exits.exits_only = true;
  EXPECT_EQ(s.submit(exits, f.time).decision.code, Reason::INVALID_ORDER);
  ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 2, {}), f.time).decision.ok());
  exits.client_order_id = "oversize";
  exits.quantity = 3;
  EXPECT_EQ(s.submit(exits, f.time).decision.code, Reason::INVALID_ORDER);
  exits.quantity = 2;
  exits.client_order_id = "ratio";
  exits.legs[0].ratio = 2;
  EXPECT_EQ(s.submit(exits, f.time).decision.code, Reason::INVALID_ORDER);
  exits.legs = close_legs();
  exits.client_order_id = "valid";
  s.trip_kill("reduce", f.time);
  const auto placed = s.submit(exits, f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_TRUE(s.submit(exits, f.time).replayed);
  auto snap = s.snapshot();
  const auto target = *placed.order_id;
  const auto stop = snap->recent_orders.at(static_cast<std::size_t>(target - 1)).oco;
  ASSERT_NE(stop, 0U);
  EXPECT_EQ(snap->recent_fills.size(), 2U);  // no synthetic entry
  ASSERT_TRUE(s.modify(target, {{}, m("0.60"), {}}, f.time).decision.ok());
  ASSERT_TRUE(s.modify(stop, {{}, {}, m("2.50")}, f.time).decision.ok());
  f.quote(s, {{P4900, "4.40", "4.60", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.at(static_cast<std::size_t>(stop - 1)).reason.code, Reason::OCO_FILLED);
}

TEST(TradingMultiLeg, ARetryOfHeldExitsAManualCloseResizedStillGetsThem) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 2, {}), f.time).decision.ok());
  auto exits = combo("exits", close_legs(), 2, "0.40", TimeInForce::Gtc);
  exits.bracket = spread_bracket();
  exits.exits_only = true;
  const auto placed = s.submit(exits, f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto target = *placed.order_id;
  // Closing one unit by hand shrinks the exits to the unit left.
  ASSERT_TRUE(s.submit(combo("close-one", close_legs(), 1, {}), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->recent_orders.at(static_cast<std::size_t>(target - 1)).request.quantity, 1);
  // A retry of the submission still gets the exits as they now stand, and records nothing.
  const auto version = s.snapshot()->account_version;
  const auto retry = s.submit(exits, f.time);
  EXPECT_TRUE(retry.decision.ok()) << retry.decision.message;
  EXPECT_TRUE(retry.replayed);
  EXPECT_EQ(retry.order_id, target);
  EXPECT_EQ(s.snapshot()->account_version, version);
}

TEST(TradingMultiLeg, HeldExitsCanBeAttachedOvernightAndWaitForTheRegularSession) {
  for (const bool target : {true, false}) {
    Chain f;
    TradingSession s(config(), f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 1, {}), f.time).decision.ok());
    f.time = md::new_york_to_utc({2026, 9, 22}, 21, 0);  // the overnight session
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    // A GTC target with a stop, or a stop alone as a triggered market order.
    const Trigger stop{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("1.20")};
    auto exits = target ? combo("exits", close_legs(), 1, "1.40", TimeInForce::Gtc) : combo("exits", close_legs(), 1, {});
    exits.bracket = Bracket{ExitSpec{stop, {}}, target ? std::optional(ExitSpec{{}, m("1.40")}) : std::nullopt};
    if (!target) exits.trigger = stop;
    exits.exits_only = true;
    const auto placed = s.submit(exits, f.time);
    ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
    // Both are reached overnight (a 1.20 net), yet neither trades until the regular session.
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
    EXPECT_EQ(s.snapshot()->recent_orders.at(1).status, target ? OrderStatus::Working : OrderStatus::Armed);
    f.time = md::new_york_to_utc({2026, 9, 23}, 9, 31);
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
    EXPECT_EQ(s.snapshot()->recent_orders.at(1).status, OrderStatus::Filled);
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingMultiLeg, AnOffTickComboExitPriceIsAnInvalidTick) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  auto entry = combo("entry", credit_legs(), 1, "-0.80");
  entry.bracket = Bracket{{}, ExitSpec{{}, m("0.42")}};
  EXPECT_EQ(s.submit(entry, f.time).decision.code, Reason::INVALID_TICK);
  ASSERT_TRUE(s.submit(combo("held", credit_legs(), 1, {}), f.time).decision.ok());
  auto exits = combo("exits", close_legs(), 1, "0.40", TimeInForce::Gtc);
  exits.bracket = Bracket{ExitSpec{{}, m("2.02")}, ExitSpec{{}, m("0.40")}};
  exits.exits_only = true;
  EXPECT_EQ(s.submit(exits, f.time).decision.code, Reason::INVALID_TICK);
}

TEST(TradingMultiLeg, ExitsCloseLegsNobodyBidsFor) {
  const std::vector<Leg> condor{leg(P4900, Side::Sell), leg(P4890, Side::Buy), leg(C5100, Side::Sell), leg(C5110, Side::Buy)};
  const std::vector<Leg> closing{leg(P4900, Side::Buy), leg(P4890, Side::Sell), leg(C5100, Side::Buy), leg(C5110, Side::Sell)};
  // One batch where the far call wing has only an ask: nobody bids for it.
  const auto quote = [](TradingSession& s, Chain& f, std::vector<std::tuple<std::string, std::string, std::string>> books) {
    ++f.observation;
    f.time += md::kNanosPerSecond;
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (const auto& [symbol, bid, ask] : books) {
      const bool bidless = bid.empty();
      quotes.push_back({symbol, f.observation, f.time, bidless ? std::nullopt : std::optional(m(bid)), m(ask), bidless ? 0 : 10, 10});
      valuations.push_back({symbol, f.time, 0.1, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                            md::years_between(f.time, md::parse_osi(symbol)->expiry_time()), 0.20, true});
    }
    s.on_quotes(quotes, valuations, f.time);
  };
  for (const bool stop : {true, false}) {
    Chain f;
    TradingSession s(config(), f.time);
    f.define(s, {P4900, P4890, C5100, C5110});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.30}, {C5110, "0.05", "0.10", 0.05}});
    auto entry = combo("condor", condor, 1, {});
    entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("4.50")}, {}}, ExitSpec{{}, m("2.00")}};
    ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
    ASSERT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Armed);
    // The stop's level buys the short call back at its only side, the ask, and
    // counts the wing at zero: 8.60 - 4.00 + 0.10 - 0 = 4.70. The target's closing
    // net is 2.20 - 1.00 + 0.80 - 0 = 2.00.
    if (stop) quote(s, f, {{P4900, "8.40", "8.60"}, {P4890, "4.00", "4.20"}, {C5100, "", "0.10"}, {C5110, "", "0.05"}});
    else quote(s, f, {{P4900, "2.10", "2.20"}, {P4890, "1.00", "1.10"}, {C5100, "0.70", "0.80"}, {C5110, "", "0.05"}});
    const auto snap = s.snapshot();
    const auto& exit = snap->recent_orders[stop ? 1 : 2];
    EXPECT_EQ(exit.status, OrderStatus::Filled);
    EXPECT_EQ(exit.filled_notional, m(stop ? "4.70" : "2.00"));
    EXPECT_EQ(snap->recent_fills[6].price, m(stop ? "0.10" : "0.80"));
    EXPECT_EQ(snap->recent_orders[stop ? 2 : 1].reason.code, Reason::OCO_FILLED);
    ASSERT_EQ(snap->recent_fills.size(), 8U);
    EXPECT_EQ(snap->recent_fills.back().symbol, C5110);
    EXPECT_EQ(snap->recent_fills.back().price, Money{});
    EXPECT_EQ(snap->recent_fills.back().fee, m("0.65"));
    EXPECT_TRUE(snap->positions.empty());
  }
  // Held exits can be attached while the wing has no bid, and a plain triggered
  // combo, which is not an exit, still waits for a bid on every leg.
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890, C5100, C5110});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}, {C5100, "3.00", "3.20", 0.30}, {C5110, "0.05", "0.10", 0.05}});
  ASSERT_TRUE(s.submit(combo("condor", condor, 2, {}), f.time).decision.ok());
  auto conditional = combo("conditional", closing, 1, {});
  conditional.trigger = Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("4.50")};
  ASSERT_TRUE(s.submit(conditional, f.time).decision.ok());
  quote(s, f, {{P4900, "5.00", "5.20"}, {P4890, "4.00", "4.20"}, {C5100, "3.00", "3.20"}, {C5110, "", "0.05"}});
  auto exits = combo("exits", closing, 1, {});
  exits.trigger = Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("4.50")};
  exits.bracket = Bracket{ExitSpec{*exits.trigger, {}}, {}};
  exits.exits_only = true;
  const auto placed = s.submit(exits, f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  quote(s, f, {{P4900, "5.40", "5.60"}, {P4890, "4.00", "4.20"}, {C5100, "3.00", "3.20"}, {C5110, "", "0.05"}});
  EXPECT_EQ(s.snapshot()->recent_orders.at(static_cast<std::size_t>(*placed.order_id - 1)).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders.at(static_cast<std::size_t>(*placed.order_id - 2)).status, OrderStatus::Armed);
}

TEST(TradingMultiLeg, PlainConditionalCombosMustReduceAndNeverMatchWhileArmed) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  auto r = combo("opening", close_legs(), 1, {});
  r.trigger = Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("2.00")};
  EXPECT_EQ(s.submit(r, f.time).decision.code, Reason::INVALID_ORDER);
  ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 1, {}), f.time).decision.ok());
  r.client_order_id = "closing";
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
  s.trip_kill("reduce", f.time);
  f.quote(s, {{P4900, "5.80", "6.00", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingMultiLeg, GtcAndExitsWaitOvernightAndExpireAtTheNearestLeg) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, LATER});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER, "4.00", "4.20", -0.28}});
  auto r = combo("gtc", {leg(P4900, Side::Sell), leg(LATER, Side::Buy)}, 1, "-1.00", TimeInForce::Gtc);
  r.bracket = spread_bracket();
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 21, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  f.quote(s, {{P4900, "5.20", "5.40", -0.30}, {LATER, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  f.quote(s, {{P4900, "5.20", "5.40", -0.30}, {LATER, "4.00", "4.20", -0.28}});
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Filled);
  f.time = md::new_york_to_utc({2026, 9, 23}, 21, 0);
  f.quote(s, {{P4900, "4.00", "4.20", -0.30}, {LATER, "4.00", "4.20", -0.28}});
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
  s.on_quotes({}, {}, md::parse_osi(P4900)->last_trade_time());
  EXPECT_EQ(s.snapshot()->recent_orders[1].reason.code, Reason::EXPIRED);
  EXPECT_EQ(s.snapshot()->recent_orders[2].reason.code, Reason::EXPIRED);
}


TEST(TradingMultiLeg, ExitFillCancelsPartialEntryAndManualClosesShrinkEveryLeg) {
  Chain f;
  TradingSession s(config(), f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  auto r = combo("partial", credit_legs(), 3, "-0.80");
  r.bracket = spread_bracket();
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  f.quote(s, {{P4900, "4.00", "4.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::OCO_FILLED);
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 5);
  r.client_order_id = "full";
  const auto next = s.submit(r, f.time);
  ASSERT_TRUE(next.decision.ok());
  ASSERT_TRUE(s.submit(combo("manual", close_legs(), 1, {}), f.time).decision.ok());
  auto snap = s.snapshot();
  const auto parent = snap->recent_orders.at(static_cast<std::size_t>(*next.order_id - 1));
  EXPECT_EQ(snap->recent_orders.at(static_cast<std::size_t>(parent.stop_loss - 1)).remaining(), 2);
  EXPECT_EQ(snap->recent_orders.at(static_cast<std::size_t>(parent.take_profit - 1)).remaining(), 2);
  ASSERT_TRUE(s.submit(combo("rest", close_legs(), 2, {}), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders.at(static_cast<std::size_t>(parent.stop_loss - 1)).reason.code, Reason::POSITION_CLOSED);
}


TEST(TradingMultiLeg, GtcBracketAndAttachedExitsRecoverWithOcoAndMetadata) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-combo-exits-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "combo.jsonl").string();
  Chain f;
  std::string expected;
  {
    TradingSession s(config(), f.time, test::create_journal(path));
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
    auto r = combo("entry", credit_legs(), 2, "-0.80", TimeInForce::Gtc);
    r.bracket = spread_bracket(); r.tags = {"spread"}; r.note = "plan";
    ASSERT_TRUE(s.submit(r, f.time).decision.ok());
    expected = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(path), test::resume_journal(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[1].request.quantity, 2);
  ASSERT_TRUE(s.cancel(2, f.time).decision.ok());
  ASSERT_TRUE(s.cancel(3, f.time).decision.ok());
  auto exits = combo("attach", close_legs(), 2, "0.40", TimeInForce::Gtc);
  exits.exits_only = true; exits.bracket = spread_bracket();
  ASSERT_TRUE(s.submit(exits, f.time).decision.ok());
  auto restored = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
  f.quote(restored, {{P4900, "4.00", "4.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(restored.snapshot()->positions.empty());
  EXPECT_EQ(restored.snapshot()->recent_orders.back().reason.code, Reason::OCO_FILLED);
  EXPECT_EQ(restored.snapshot()->annotations.size(), 2U);
  std::filesystem::remove_all(directory);
}


TEST(TradingMultiLeg, AClosingConditionalComboCanReduceABuyOnlyAccount) {
  Chain f;
  auto c = config();
  c.rules.buy_only = true;
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(single("long-one", P4900, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(s.submit(single("long-two", P4890, Side::Buy), f.time).decision.ok());
  auto r = combo("conditional-close", {leg(P4900, Side::Sell), leg(P4890, Side::Sell)}, 1, {});
  r.trigger = Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("-8.00")};
  s.trip_kill("reduce", f.time);
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  f.quote(s, {{P4900, "4.00", "4.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  EXPECT_TRUE(s.snapshot()->positions.empty());
}


TEST(TradingMultiLeg, ImpactFillsEachLegInRatioAcrossSizeTiers) {
  Chain f;
  auto settings = config();
  settings.rules.impact_ticks = 1;
  TradingSession s(settings, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 2);
  const auto legs = std::vector<Leg>{leg(P4900, Side::Buy, 3), leg(P4890, Side::Sell)};
  ASSERT_TRUE(s.submit(combo("ratio", legs, 2, "11.80"), f.time).decision.ok());
  const auto first = s.snapshot();
  EXPECT_EQ(first->recent_orders.back().filled_quantity, 1);
  EXPECT_EQ(first->recent_orders.back().remaining(), 1);
  ASSERT_EQ(first->recent_fills.size(), 3U);
  EXPECT_EQ(first->recent_fills[0].quantity, 2);
  EXPECT_EQ(first->recent_fills[0].price, m("5.20"));
  EXPECT_EQ(first->recent_fills[1].quantity, 1);
  EXPECT_EQ(first->recent_fills[1].price, m("5.30"));
  EXPECT_EQ(first->recent_fills[2].price, m("4.00"));
  EXPECT_EQ(first->recent_orders.back().filled_notional, m("11.70"));
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 2);
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders.back().filled_notional, m("23.40"));
  EXPECT_EQ(s.snapshot()->account.fees, m("5.20"));
}

TEST(TradingMultiLeg, ConservativeFillsWaitForEveryLegThenSweepAdverseBlocks) {
  for (const bool credit : {false, true}) {
    Chain f;
    auto settings = config();
    settings.rules.fill_latency_ms = 1000;
    settings.rules.impact_ticks = 1;
    settings.rules.slippage_ticks = 1;
    TradingSession s(settings, f.time);
    f.define(s, {P4900, P4890});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 2);
    const auto legs = credit ? std::vector<Leg>{leg(P4900, Side::Sell), leg(P4890, Side::Buy)}
                             : std::vector<Leg>{leg(P4900, Side::Buy), leg(P4890, Side::Sell)};
    ASSERT_TRUE(s.submit(combo("pending", legs, 3, {}), f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}}, 2);
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    f.quote(s, {{P4890, "4.00", "4.20", -0.28}}, 2);
    const auto snapshot = s.snapshot();
    ASSERT_EQ(snapshot->recent_fills.size(), 6U);
    EXPECT_EQ(snapshot->recent_orders.back().filled_quantity, 3);
    EXPECT_EQ(snapshot->recent_orders.back().filled_notional, m(credit ? "-1.60" : "4.40"));
    EXPECT_EQ(snapshot->recent_fills[4].price, m(credit ? "4.80" : "5.40"));
    EXPECT_EQ(snapshot->recent_fills[5].price, m(credit ? "4.40" : "3.80"));
    EXPECT_EQ(snapshot->account.fees, m("3.90"));
  }
}

TEST(TradingMultiLeg, DelayedIocKeepsPartialRatioAndCancelsRemainder) {
  Chain f;
  auto settings = config();
  settings.rules.fill_latency_ms = 1000;
  TradingSession s(settings, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("partial", {leg(P4900, Side::Buy, 2), leg(P4890, Side::Sell)}, 3, {}), f.time).decision.ok());
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 3);
  const auto snapshot = s.snapshot();
  EXPECT_EQ(snapshot->recent_orders.back().filled_quantity, 1);
  EXPECT_EQ(snapshot->recent_orders.back().remaining(), 2);
  EXPECT_EQ(snapshot->recent_orders.back().reason.code, Reason::IOC_REMAINDER);
  ASSERT_EQ(snapshot->recent_fills.size(), 2U);
  EXPECT_EQ(snapshot->recent_fills[0].quantity, 2);
  EXPECT_EQ(snapshot->recent_fills[1].quantity, 1);
}

TEST(TradingMultiLeg, PendingLatencyRespectsCancellationAndEarliestLegExpiry) {
  for (const bool cancel : {false, true}) {
    Chain f;
    f.time = md::new_york_to_utc({2026, 10, 22}, 15, 59, 57);
    auto settings = config();
    settings.rules.fill_latency_ms = 3000;
    TradingSession s(settings, f.time);
    f.define(s, {P4900, LATER});
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER, "4.00", "4.20", -0.28}});
    ASSERT_TRUE(s.submit(combo("pending", {leg(P4900, Side::Buy), leg(LATER, Side::Sell)}, 1, "1.20", TimeInForce::Gtc), f.time).decision.ok());
    if (cancel) s.cancel(1, f.time);
    f.time += md::kNanosPerSecond;
    f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {LATER, "4.00", "4.20", -0.28}});
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, cancel ? Reason::USER_CANCEL : Reason::EXPIRED);
  }
}

TEST(TradingMultiLeg, LatencyStopWaitsFromTriggerAndOcoCancelsRemainingEntry) {
  Chain f;
  auto settings = config();
  settings.rules.fill_latency_ms = 1000;
  TradingSession s(settings, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  auto request = combo("entry", {leg(P4900, Side::Buy), leg(P4890, Side::Sell)}, 3, "1.20");
  request.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("-0.50")}, {}},
                            ExitSpec{{}, m("-2.00")}};
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[0].filled_quantity, 1);
  f.quote(s, {{P4900, "4.50", "4.70", -0.30}, {P4890, "4.10", "4.30", -0.28}}, 0);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Armed);
  // Keep entry unmarketable while the closing net reaches the stop.
  f.quote(s, {{P4900, "4.50", "6.00", -0.30}, {P4890, "4.10", "4.30", -0.28}}, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Working);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
  f.quote(s, {{P4900, "4.40", "6.00", -0.30}, {P4890, "4.10", "4.40", -0.28}}, 1);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::OCO_FILLED);
  EXPECT_EQ(s.snapshot()->recent_orders[2].reason.code, Reason::OCO_FILLED);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Filled);
}

TEST(TradingMultiLeg, ExtendedHeldComboStopsUseDisplayedTouchesAndRearmOvernight) {
  Chain f;
  auto c = config();
  TradingSession s(c, f.time);
  f.define(s, {P4900, P4890});
  f.quote(s, {{P4900, "5.00", "5.20", -0.30}, {P4890, "4.00", "4.20", -0.28}});
  ASSERT_TRUE(s.submit(combo("entry", credit_legs(), 2, "-0.80"), f.time).decision.ok());
  const auto stop = Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("2.00")};
  auto exits = combo("exits", close_legs(), 2, "0.40", TimeInForce::GtcExto);
  exits.exits_only = true;
  exits.bracket = Bracket{ExitSpec{stop, {}}, ExitSpec{{}, m("0.40")}};
  ASSERT_TRUE(s.submit(exits, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders[2].request.tif, TimeInForce::GtcExto);
  f.time = md::new_york_to_utc({2026, 9, 22}, 21, 0) - md::kNanosPerSecond;
  f.quote(s, {{P4900, "6.90", "7.10", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[2].status, OrderStatus::Armed);
  EXPECT_EQ(s.snapshot()->recent_orders[2].filled_quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[2].filled_notional, m("3.10"));
  f.quote(s, {{P4900, "6.90", "7.10", -0.30}, {P4890, "4.00", "4.20", -0.28}}, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[2].status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders[1].reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

}  // namespace
}  // namespace openport::trading
