#pragma once

#include "paper_json.hpp"
#include "event_calendar.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/strategy_search.hpp"
#include "openport/server/hedges.hpp"
#include "openport/server/account_groups.hpp"
#include "playbook_api.hpp"
#include "paper_csv.hpp"
#include "metric_cache.hpp"
#include "fill_calibration.hpp"
#include "underlyings_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "openport/md/utf8.hpp"
#include "openport/server/candles.hpp"
#include "openport/server/series.hpp"
#include "openport/trading/stress.hpp"
#include "openport/server/plans.hpp"
#include "openport/providers/cboe.hpp"
#include "openport/trading/contracts.hpp"
#include "openport/trading/fill_quality.hpp"
#include "openport/trading/history.hpp"

// The paper-trading API's internals, shared by paper_api.cpp (its routes and command
// answers), paper_json.cpp (the encoders) and paper_parse.cpp (the request parsers).
// They were paper_api.cpp's anonymous namespace; a helper only one file uses stays in
// an anonymous namespace there. Private: only those three files include this header.

namespace openport::server::paper_internal {
using nlohmann::json;
using namespace trading;
json number(double value);
json nullable(const std::string& value);
json money(const std::optional<Money>& value);
json positive(Money value);
const char* status_name(EvaluationStatus status);
inline constexpr const char* kReviewSteps[] = {"warning", "forfeit", "cap"};
inline constexpr const char* kPayoutFloors[] = {"keep", "reset", "starting_balance"};
inline constexpr const char* kDrawdownModes[] = {"intraday", "end_of_day", "static"};
inline constexpr const char* kDrawdownBases[] = {"equity", "balance"};
inline constexpr const char* kOpenLossBases[] = {"amount", "day_profit", "drawdown_room"};
inline constexpr const char* kOpenLossActions[] = {"flatten", "block_adds", "fail"};
inline constexpr const char* kProfitBases[] = {"equity", "balance"};
inline constexpr const char* kDailyLossBases[] = {"equity", "balance", "higher", "peak"};
inline constexpr const char* kBreachActions[] = {"lock", "fail"};
inline constexpr const char* kConsistencyBases[] = {"total", "positive_days"};
json rules_json(const AccountRules& r, Money initial_cash, const std::optional<SizeScalingProgress>& scaling = {});
json scope_json(const std::string& scope);
json evidence_value(const Decision& d, bool limit);
json rule_evidence_json(const RuleEvidence& e);
json decision_json(const Decision& d);
json time_or_null(Timestamp time);
json what_if_account_json(const WhatIfAccount& a, const ScenarioConfig& grid);
json what_if_json(const WhatIf& w, const std::vector<std::string>& names, const ScenarioConfig& grid);
json preview_json(const OrderPreview& p);
std::string underlying(const TradingView& view, const std::string& symbol);
json alert_json(const Alert& a);
json stock_order_json(const StockOrder& o);
json order_json(const Order& o, const TradingView& view);
json flatten_preview_json(const FlattenPreview& f, const TradingView& view);
json day_notes_json(const TradingSnapshot& s);
bool from_attempt(std::uint64_t first_stock_fill, Timestamp started, std::uint64_t fill, Timestamp time);
json fill_json(const Fill& f, const TradingView& view);
json fill_quality_json(const TradingView& view, bool current);
json calibration_json(const CalibrationReport& report);
json portfolio_json(const TradingView& view);
json account_json(const TradingView& view);
json stock_fill_json(const StockFill& fill);
json trades_json(const TradingView& view, std::string_view status, bool current_only,
                 std::optional<Timestamp> since = {}, std::optional<Timestamp> until = {});
json plans_json();
json kill_json(const TradingSnapshot& s);
json risk_json(const TradingView& view, const ScenarioGrid* grid = nullptr,
               SmileDynamics dynamics = SmileDynamics::StickyStrike);
void fields(const json& object, std::initializer_list<std::string_view> required,
             std::initializer_list<std::string_view> optional = {});
std::string string_field(const json& j, const char* key);
std::int64_t integer_field(const json& j, const char* key);
std::uint64_t identifier(std::string_view text);
Money decimal_field(const json& j, const char* key);
bool boolean_field(const json& j, const char* key);
/// New restriction values use 422 without changing legacy rule error statuses.
struct PlanRestrictionError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};
AccountRules parse_rules(const json& j);
json strict_json(const std::string& body);
bool valid_client_order_id(std::string_view id);
bool valid_account(std::string_view id);
bool order_id_path(std::string_view path);
std::string trimmed_name(std::string_view name);
OrderRequest parse_order(const json& body, std::size_t depth = 0);
TradingCommand parse_command(const ApiRequest& request, std::string_view path);
}  // namespace openport::server::paper_internal

namespace openport::server {
// The routes, encoders and parsers call the helpers unqualified, as they did when the
// helpers were an anonymous namespace in paper_api.cpp.
using namespace paper_internal;
json accounts_json(const MetricsSource& source, const ApiAccess& access, bool archived = false);
}  // namespace openport::server
