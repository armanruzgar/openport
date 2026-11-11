#include "event_calendar.hpp"

namespace openport::server {
/// This read-only catalogue never enters a reducer unless imported into plan rules.
std::optional<ApiResponse> event_calendar_read(const ApiRequest& request, const std::vector<trading::PlanEvent>& events) {
  const std::string_view target = request.target;
  const auto mark = target.find('?');
  if (target.substr(0, mark) != "/api/calendar/events") return std::nullopt;
  if (request.method != "GET") return method_refused(target.substr(0, mark), "Calendar events support GET");
  const auto query = query_parameters(mark == std::string_view::npos ? std::string_view{} : target.substr(mark + 1));
  if (!query) return api_error(400, "INVALID_REQUEST", "Duplicate or malformed calendar query");
  md::Timestamp from = 0, to = std::numeric_limits<md::Timestamp>::max();
  for (const auto& [key, value] : *query) {
    if (key != "from" && key != "to") return api_error(400, "INVALID_REQUEST", "Calendar accepts only from and to");
    const auto parsed = md::parse_datetime(value.size() == 10 ? value + "T00:00:00Z" : value, md::Zone::Utc);
    if (!parsed) return api_error(400, "INVALID_REQUEST", key + " must be a date or ISO-8601 timestamp");
    (key == "from" ? from : to) = *parsed;
  }
  if (from > to) return api_error(400, "INVALID_REQUEST", "from must not be after to");
  auto out = nlohmann::json::array();
  for (const auto& e : events) if (const auto time = trading::event_time(e); time >= from && time < to) out.push_back(calendar_event_json(e));
  return ApiResponse{200, nlohmann::json{{"events", out}}.dump()};
}
}  // namespace openport::server
