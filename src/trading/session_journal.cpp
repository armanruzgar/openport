// A session's journal records and their recovery: each record's state as a change or
// a checkpoint, and recovering, compacting and expanding a journal.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {
/// Keeps schema 3 records small: each carries its state as the change from
/// the record before ("delta") or whole, as a checkpoint ("state"). A journal
/// starts with a checkpoint and has one every kCheckpointEvery records, and
/// any change more than half the size of the last checkpoint is recorded whole.
class StateRecorder {
 public:
  /// Whether `state` is the one last recorded: a transaction that changed nothing.
  [[nodiscard]] bool recorded(const Json& state) const { return base_ && *base_ == state; }
  void add(Json& payload, Json state) {
    if (base_ && since_ + 1 < kCheckpointEvery) {
      auto delta = detail::state_delta(*base_, state);
      if (delta.dump().size() * 2 <= checkpoint_bytes_) {
        payload["delta"] = std::move(delta);
        base_ = std::move(state);
        ++since_;
        return;
      }
    }
    checkpoint_bytes_ = state.dump().size();
    payload["state"] = state;
    base_ = std::move(state);
    since_ = 0;
  }
 private:
  std::optional<Json> base_;
  std::uint64_t since_ = 0;
  std::size_t checkpoint_bytes_ = 0;
};

/// state_delta of one field's JSON, before and after, or nothing when it is
/// equal. History containers write out and compare only the subtrees they do not
/// share: a transaction copies the account, so the rest is untouched.
template <class T>
std::optional<Json> field_change(const T& before, const T& after) {
  const Json a = before, b = after;
  if (a == b) return std::nullopt;
  return detail::state_delta(a, b);
}
template <class T, std::size_t N>
std::optional<Json> field_change(const SharedVector<T, N>& before, const SharedVector<T, N>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedVector<T, N>::compare(before, after, [&](std::size_t i, const T& old, const T& now) {
    const Json a = old, b = now;
    if (a == b) return;
    changes[std::to_string(i)] = detail::state_delta(a, b);
    ++changed;
  });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  // As state_delta: an array rewritten in place is smaller whole.
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  for (std::size_t i = common; i < after.size(); ++i) changes[std::to_string(i)] = Json{{"v", after[i]}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
template <class V, std::size_t N>
std::optional<Json> field_change(const SharedMap<std::string, V, N>& before, const SharedMap<std::string, V, N>& after) {
  if (before.same(after)) return std::nullopt;
  Json changes = Json::object();
  Json removed = Json::array();
  SharedMap<std::string, V, N>::compare(before, after,
      [&](const std::string& key, const V& value) { changes[key] = Json{{"v", value}}; },
      [&](const std::string& key) { removed.push_back(key); },
      [&](const std::string& key, const V& old, const V& now) {
        const Json a = old, b = now;
        if (a != b) changes[key] = detail::state_delta(a, b);
      });
  if (changes.empty() && removed.empty()) return std::nullopt;
  Json node{{"o", std::move(changes)}};
  if (!removed.empty()) node["d"] = std::move(removed);
  return node;
}
template <class K>
std::optional<Json> field_change(const SharedSet<K>& before, const SharedSet<K>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedSet<K>::compare(before, after,
      [&](std::size_t i, const K& old, const K& now) {
        const Json a = old, b = now;
        if (a == b) return;
        changes[std::to_string(i)] = detail::state_delta(a, b);
        ++changed;
      },
      [&](std::size_t i, const K& key) { changes[std::to_string(i)] = Json{{"v", key}}; });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
}  // namespace
/// state_delta(Json(before), Json(after)), found field by field without writing
/// out either state: the change a record carries. Equal states give {"o": {}}.
Json state_change(const State& before, const State& after) {
  Json changes = Json::object();
#define OPENPORT_STATE_CHANGE(field) \
  if (auto change = field_change(before.field, after.field)) changes[#field] = std::move(*change);
  OPENPORT_STATE_FIELDS(OPENPORT_STATE_CHANGE)
#undef OPENPORT_STATE_CHANGE
  // Indicators are written once there is one, and never emptied again.
  if (before.indicators.empty() && !after.indicators.empty()) changes["indicators"] = Json{{"v", after.indicators}};
  else if (auto change = field_change(before.indicators, after.indicators)) changes["indicators"] = std::move(*change);
  if (before.cash_flows.empty() && !after.cash_flows.empty()) changes["cash_flows"] = Json{{"v", after.cash_flows}};
  else if (auto change = field_change(before.cash_flows, after.cash_flows)) changes["cash_flows"] = std::move(*change);
  if (before.deliverables.empty() && !after.deliverables.empty()) changes["deliverables"] = Json{{"v", after.deliverables}};
  else if (auto change = field_change(before.deliverables, after.deliverables)) changes["deliverables"] = std::move(*change);
  if (before.corporate_actions.empty() && !after.corporate_actions.empty()) changes["corporate_actions"] = Json{{"v", after.corporate_actions}};
  else if (auto change = field_change(before.corporate_actions, after.corporate_actions)) changes["corporate_actions"] = std::move(*change);
  if (before.stock_orders.empty() && !after.stock_orders.empty()) changes["stock_orders"] = Json{{"v", after.stock_orders}};
  else if (auto change = field_change(before.stock_orders, after.stock_orders)) changes["stock_orders"] = std::move(*change);
  if (before.market_halts.empty() && !after.market_halts.empty()) changes["market_halts"] = Json{{"v", after.market_halts}};
  else if (auto change = field_change(before.market_halts, after.market_halts)) changes["market_halts"] = std::move(*change);
  // Alerts likewise, once the account has created one.
  if (before.alerts.created == 0 && after.alerts.created > 0) changes["alerts"] = Json{{"v", after.alerts}};
  else if (auto change = field_change(before.alerts, after.alerts)) changes["alerts"] = std::move(*change);
  // Exercise instructions are written only while there are any (see to_json).
  const bool had = !before.do_not_exercise.empty(), has = !after.do_not_exercise.empty();
  if (has && !had) changes["do_not_exercise"] = Json{{"v", after.do_not_exercise}};
  else if (has) {
    if (auto change = field_change(before.do_not_exercise, after.do_not_exercise)) changes["do_not_exercise"] = std::move(*change);
  }
  Json node{{"o", std::move(changes)}};
  if (had && !has) node["d"] = Json::array({"do_not_exercise"});
  if (after.fee_charged.amount != Money{}) {
    if (before.fee_charged.amount == Money{}) node["o"]["fee_charged"] = Json{{"v", after.fee_charged}};
    else if (auto change = field_change(before.fee_charged, after.fee_charged)) node["o"]["fee_charged"] = std::move(*change);
  } else if (before.fee_charged.amount != Money{}) {
    if (!node.contains("d")) node["d"] = Json::array();
    node["d"].push_back("fee_charged");
  }
  return node;
}
namespace {

/// Reverify even caller-constructed recovery objects instead of trusting them.
JournalRecovery reverify(const JournalRecovery& recovery) {
  if (recovery.records.empty()) throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery requires a session_start record");
  std::string lines;
  try {
    for (const auto& r : recovery.records) {
      lines += Json{{"seq", r.seq}, {"time", r.time}, {"type", r.type}, {"payload", Json::parse(r.payload)},
                    {"prev_hash", r.prev_hash}, {"hash", r.hash}}.dump() + '\n';
    }
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  auto verified = verify_journal(lines, recovery.head);
  if (verified.records.front().type != "session_start") throw TradingError(Reason::JOURNAL_CORRUPT, "Missing session start");
  return verified;
}

/// Visit each transaction of a verified journal in order with the whole state
/// it left: read from the record (schemas 1 and 2, and schema 3 checkpoints) or
/// rebuilt by applying the record's delta. Each state must carry its record's
/// sequence and time, and each new config passes validation. The visitor gets
/// the payload without its state; returns the last state.
template <class Visit>
Json walk_states(const std::vector<JournalRecord>& records, Visit&& visit,
                 SharedVector<SettlementRecord>* settlements = nullptr) {
  Json state;
  bool chained = false;  // A delta may follow only a schema 3 record.
  Json validated;
  for (const auto& r : records) {
    auto payload = Json::parse(r.payload);
    std::map<std::string, Position> settling_positions;
    if (settlements && r.type == "settlement" && !state.is_null())
      settling_positions = state.at("ledger").at("positions").get<std::map<std::string, Position>>();
    const auto schema = payload.at("schema");
    const auto& ticks = payload.at("tick_policy");
    if ((schema != 1 && schema != 2 && schema != 3) || (ticks != "index-v1" && ticks != "v2"))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Unsupported trading journal schema/policy");
    const auto whole = payload.find("state");
    if (schema == 3) {
      const auto delta = payload.find("delta");
      if ((whole == payload.end()) == (delta == payload.end()))
        throw TradingError(Reason::JOURNAL_CORRUPT, "A schema 3 record carries either a whole state or a delta");
      if (delta != payload.end()) {
        if (!chained) throw TradingError(Reason::JOURNAL_CORRUPT, "A state delta without a checkpoint before it");
        try { detail::apply_state_delta(state, *delta); }
        catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
      }
    } else {
      const auto& snapshot = payload.at("snapshot");
      if (snapshot.at("account_version") != r.seq || snapshot.at("time") != r.time)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded snapshot version/time does not match transaction");
      if (whole == payload.end()) throw TradingError(Reason::JOURNAL_CORRUPT, "Record without its state");
    }
    if (whole != payload.end()) {
      state = std::move(*whole);
      payload.erase(whole);
    }
    chained = schema == 3;
    if (!state.is_object() || state.at("version") != r.seq || state.at("time") != r.time)
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded state version/time does not match transaction");
    if (const auto& config = state.at("config"); config != validated) {
      const auto c = config.get<SessionConfig>();
      validate_limits(c.limits);
      validate_scenarios(c.scenarios);
      validate_rules(c.rules);
      validate_guardrails(c.guardrails, true);
      validated = config;
    }
    if (settlements && r.type == "settlement") {
      for (const auto& e : payload.at("events")) {
        if (e.at("type") != "settlement") continue;
        const auto& data = e.at("payload");
        const auto symbol = data.at("symbol").get<std::string>();
        Money fee;
        if (state.contains("closures") && !state.at("closures").empty())
          fee = state.at("closures").back().value("fee", Money{});
        std::optional<SettlementSource> source;
        if (payload.contains("settlement_source") && !payload.at("settlement_source").is_null())
          source = payload.at("settlement_source").get<SettlementSource>();
        settlements->push_back(settlement_record(settling_positions.at(symbol), data.at("reference").get<Money>(),
            data.at("intrinsic").get<Money>(), fee, r.time, std::move(source)));
      }
    }
    visit(r, std::move(payload), std::as_const(state));
  }
  return state;
}
}  // namespace
}  // namespace session_internal
TradingSession TradingSession::recover(const JournalRecovery& recovery, std::shared_ptr<Journal> journal) {
  const auto verified = reverify(recovery);
  if (journal && (recovery.truncated_final_line || journal->head() != verified.head || journal->sequence() != verified.records.size()))
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery sink does not match verified journal head");
  auto impl = std::make_unique<Impl>();
  try {
    Json last;
    SharedVector<SettlementRecord> settlements;
    Money payouts_received;
    auto state = walk_states(verified.records, [&](const JournalRecord&, Json&& payload, const Json&) {
      for (const auto& event : payload.at("events"))
        if (event.at("type") == "payout") payouts_received = payouts_received + event.at("payload").at("trader_share").get<Money>();
      last = std::move(payload);
    }, &settlements);
    impl->state = state.get<State>();
    impl->state.settlements = std::move(settlements);
    impl->state.payouts_received = payouts_received;
    // Schema 3 records no snapshot; the reducer derives it from the state.
    impl->snapshot = std::make_shared<TradingSnapshot>(
        last.at("schema") == 3 ? snapshot_of(impl->state) : last.at("snapshot").get<TradingSnapshot>());
    // Older snapshots predate the exit value, which the books give.
    if (last.at("schema") != 3) {
      auto snapshot = std::make_shared<TradingSnapshot>(*impl->snapshot);
      snapshot->exit_equity = exit_equity_of(impl->state, *snapshot);
      snapshot->settlements = impl->state.settlements;
      snapshot->payouts_received = impl->state.payouts_received;
      impl->snapshot = std::move(snapshot);
    }
    if (last.at("schema") == 1) {
      // A schema 1 account has no rules; start its progress record from the
      // journal's first transaction so later records continue it.
      auto& s = impl->state;
      Evaluation e;
      for (const auto& r : verified.records) if (r.time > 0) { e.started = r.time; break; }
      e.starting_balance = s.config.initial_cash;
      e.peak = e.starting_balance;
      e.day = s.day;
      e.day_open_equity = s.start_equity;
      e.day_close_equity = impl->snapshot->equity;
      e.day_open_realised = net_realised(s);  // Unknown for the day in progress; count from here.
      e.cycle_started = e.started;
      s.evaluation = std::move(e);
      auto snapshot = std::make_shared<TradingSnapshot>(*impl->snapshot);
      snapshot->evaluation = s.evaluation;
      snapshot->buying_power = buying_power(s).total;
      impl->snapshot = std::move(snapshot);
    }
  } catch (const TradingError& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  impl->journal = std::move(journal);
  return TradingSession(std::move(impl));
}
std::string TradingSession::compact(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Compaction writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);  // Only a journal that recovers is rewritten.
  StateRecorder recorder;
  Json reread;
  try {
    const auto final_seq = verified.records.size();
    const auto last = walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (r.seq == final_seq && payload.at("schema") == 1) {
        // Recovery completes the evaluation of a journal whose last record is
        // schema 1 from that record, so it stays as it is.
        out.append(r.time, r.type, r.payload);
        return;
      }
      payload["schema"] = 3;
      payload.erase("snapshot");
      payload.erase("delta");
      recorder.add(payload, state);
      const auto line = payload.dump();
      // Read the record back as recovery will, before it is written.
      const auto check = Json::parse(line);
      if (const auto delta = check.find("delta"); delta != check.end()) detail::apply_state_delta(reread, *delta);
      else reread = check.at("state");
      if (reread != state)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Compaction would change transaction " + std::to_string(r.seq));
      out.append(r.time, r.type, line);
    });
    if (Json::parse(verified.records.back().payload).at("schema") == 1) return recover(recovery).snapshot_json();
    return Json(snapshot_of(last.get<State>())).dump();
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
void TradingSession::expand(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Expansion writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);
  try {
    (void)walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (payload.at("schema") != 3) { out.append(r.time, r.type, r.payload); return; }
      payload["schema"] = 2;
      payload.erase("delta");
      payload["snapshot"] = snapshot_of(state.get<State>());
      payload["state"] = state;
      out.append(r.time, r.type, payload.dump());
    });
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
}  // namespace openport::trading
