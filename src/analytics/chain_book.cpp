#include "openport/analytics/chain_book.hpp"

#include <algorithm>
#include <type_traits>

namespace openport::analytics {

void ChainBook::apply(const md::Event& event) {
  std::visit(
      [this](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, md::ContractDefinition>) {
          on_definition(e);
        } else if constexpr (std::is_same_v<T, md::OptionQuote>) {
          if (OptionState* s = defined(e.id)) {
            s->has_quote = true;
            s->bid = e.bid;
            s->ask = e.ask;
            s->bid_size = e.bid_size;
            s->ask_size = e.ask_size;
            s->quote_ts = e.ts;
            touch(*s, e.ts);
          }
        } else if constexpr (std::is_same_v<T, md::OptionVolume>) {
          if (OptionState* s = defined(e.id); s && e.ts >= s->volume_ts &&
              std::isfinite(e.contracts) && e.contracts >= 0.0) {
            s->volume = e.contracts;
            s->volume_ts = e.ts;
            touch(*s, e.ts);
          }
        } else if constexpr (std::is_same_v<T, md::OpenInterest>) {
          if (OptionState* s = defined(e.id)) {
            s->has_open_interest = true;
            s->open_interest = e.contracts;
            touch(*s, 0);
          }
        } else if constexpr (std::is_same_v<T, md::VendorGreeks>) {
          if (OptionState* s = defined(e.id)) {
            s->vendor = e;
            touch(*s, 0);
          }
        } else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) {
          UnderlyingBook& book = underlyings_[e.symbol];
          book.symbol = e.symbol;
          const double mid = e.bid > 0.0 && e.ask >= e.bid ? 0.5 * (e.bid + e.ask) : 0.0;
          book.spot = e.last > 0.0 ? e.last : mid;
          book.spot_ts = e.ts;
          book.data_time = std::max(book.data_time, e.ts);
          ++book.version;
        }
        // Trades and feed status do not change the chain's state.
      },
      event);
}

const OptionState* ChainBook::option(md::InstrumentId id) const noexcept {
  return id < options_.size() && defined_[id] ? &options_[id] : nullptr;
}

OptionState* ChainBook::defined(md::InstrumentId id) noexcept {
  return id < options_.size() && defined_[id] ? &options_[id] : nullptr;
}

void ChainBook::on_definition(const md::ContractDefinition& e) {
  if (e.id >= options_.size()) {
    // Widened first: e.id + 1 in 32 bits wraps to 0 at the largest ID.
    const auto size = static_cast<std::size_t>(e.id) + 1;
    options_.resize(size);
    defined_.resize(size, false);
  }
  if (!defined_[e.id]) ++defined_count_;
  if (defined_[e.id] && !options_[e.id].contract.standard) --nonstandard_count_;
  if (!e.contract.standard) ++nonstandard_count_;
  defined_[e.id] = true;

  OptionState& state = options_[e.id];
  state.contract = e.contract;
  state.expiry_time = e.contract.expiry_time();
  // Adjusted deliverables must never overwrite a vanilla contract at the same strike.
  if (!state.contract.standard) return;

  UnderlyingBook& book = underlyings_[e.contract.underlying];
  book.symbol = e.contract.underlying;
  ExpirySlice& slice = book.expiries[{state.expiry_time, e.contract.style}];
  slice.expiry = e.contract.expiry;
  slice.expiry_time = state.expiry_time;
  if (e.contract.root == "OEX" || e.contract.root == "XEO") slice.root = e.contract.root;
  StrikePair& pair = slice.strikes[e.contract.strike];
  (e.contract.type == pricing::OptionType::Call ? pair.call : pair.put) = e.id;
  ++book.version;
}

void ChainBook::touch(const OptionState& state, md::Timestamp ts) {
  if (!state.contract.standard) return;
  UnderlyingBook& book = underlyings_[state.contract.underlying];
  if (ts > book.data_time) book.data_time = ts;
  ++book.version;
}

}  // namespace openport::analytics
