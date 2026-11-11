#include "state_delta.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>

namespace openport::trading::detail {
using Json = nlohmann::json;

Json state_delta(const Json& before, const Json& after) {
  if (before.is_object() && after.is_object()) {
    Json changes = Json::object();
    for (auto it = after.begin(); it != after.end(); ++it) {
      const auto old = before.find(it.key());
      if (old == before.end())
        changes[it.key()] = Json{{"v", it.value()}};
      else if (*old != it.value())
        changes[it.key()] = state_delta(*old, it.value());
    }
    Json node{{"o", std::move(changes)}};
    Json removed = Json::array();
    for (auto it = before.begin(); it != before.end(); ++it)
      if (!after.contains(it.key())) removed.push_back(it.key());
    if (!removed.empty()) node["d"] = std::move(removed);
    return node;
  }
  if (before.is_array() && after.is_array()) {
    const auto common = std::min(before.size(), after.size());
    Json changes = Json::object();
    std::size_t changed = 0;
    for (std::size_t i = 0; i < common; ++i) {
      if (before[i] == after[i]) continue;
      changes[std::to_string(i)] = state_delta(before[i], after[i]);
      ++changed;
    }
    // An array rewritten in place is smaller whole.
    if (changed * 2 > common && common > 0) return Json{{"v", after}};
    for (std::size_t i = common; i < after.size(); ++i) changes[std::to_string(i)] = Json{{"v", after[i]}};
    return Json{{"a", std::move(changes)}, {"n", after.size()}};
  }
  return Json{{"v", after}};
}

void apply_state_delta(Json& state, const Json& delta) {
  if (!delta.is_object()) throw std::invalid_argument("A state delta node must be an object");
  if (const auto value = delta.find("v"); value != delta.end()) {
    state = *value;
    return;
  }
  if (const auto keys = delta.find("o"); keys != delta.end()) {
    if (!state.is_object() || !keys->is_object()) throw std::invalid_argument("An object delta patches an object");
    for (auto it = keys->begin(); it != keys->end(); ++it) {
      // A new key must arrive whole; a nested patch needs a value to patch.
      if (!state.contains(it.key()) && !it.value().contains("v"))
        throw std::invalid_argument("A new key's delta must carry its value");
      apply_state_delta(state[it.key()], it.value());
    }
    // The writer removes only keys the state had, in an array of their names.
    if (const auto removed = delta.find("d"); removed != delta.end()) {
      if (!removed->is_array()) throw std::invalid_argument("An object delta's removed keys must be an array");
      for (const auto& key : *removed) {
        if (!key.is_string() || !state.contains(key.get<std::string>()))
          throw std::invalid_argument("An object delta removes only keys the object has");
        state.erase(key.get<std::string>());
      }
    }
    return;
  }
  if (const auto items = delta.find("a"); items != delta.end()) {
    if (!state.is_array() || !items->is_object()) throw std::invalid_argument("An array delta patches an array");
    const auto n = delta.find("n");
    if (n == delta.end() || !n->is_number_unsigned()) throw std::invalid_argument("An array delta's length must be a whole number");
    const auto length = n->get<std::size_t>();
    const auto old = state.size();
    // An array grows only by the elements the delta carries whole: checked before
    // padding, so a forged length can neither leave nulls nor exhaust memory.
    if (length > old && length - old > items->size())
      throw std::invalid_argument("An array delta's length runs past the elements it carries");
    if (length < old) state.erase(state.begin() + static_cast<std::ptrdiff_t>(length), state.end());
    while (state.size() < length) state.push_back(nullptr);
    std::size_t added = 0;
    for (auto it = items->begin(); it != items->end(); ++it) {
      std::size_t index = 0;
      const auto& key = it.key();
      const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
      if (error != std::errc{} || end != key.data() + key.size() || index >= length)
        throw std::invalid_argument("An array delta index is out of range");
      if (index >= old && !it.value().contains("v"))
        throw std::invalid_argument("A new element's delta must carry its value");
      if (index >= old) ++added;
      apply_state_delta(state[index], it.value());
    }
    if (old + added < length) throw std::invalid_argument("An array delta leaves a new element without its value");
    return;
  }
  throw std::invalid_argument("Unknown state delta node");
}

}  // namespace openport::trading::detail
