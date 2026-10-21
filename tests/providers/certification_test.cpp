#include "../../apps/probe/certification.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <limits>

#include "openport/pricing/black.hpp"
#include "openport/providers/thetadata.hpp"
#include "support/http_stub.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using Json = nlohmann::json;
constexpr md::Timestamp kTime = 1790263800000LL * 1'000'000;
const auto kContract = *md::parse_osi("SPY991218C00500000");

TEST(Certification, CountsEveryPublicationAndSeparatesMarketFromOperationalClocks) {
  probe::Conformance monitor({{"SPY"}}, false, 1);
  const std::vector<md::Event> events{
      md::ContractDefinition{0, kContract}, md::OptionQuote{0, kTime, 5, 6, 1, 2},
      md::OptionQuote{0, kTime, 5.1, 6, 1, 2}, md::OptionTrade{0, kTime, 5, 1},
      md::OpenInterest{0, kTime, 0}, md::OptionVolume{0, kTime, 0},
      md::VendorGreeks{0, kTime, .2, .5, .01, 1, -.1, .2},
      md::UnderlyingQuote{"SPY", kTime, 500, 501, 500.5},
      md::UnderlyingClose{"SPY", kTime, {2026, 9, 23}, 500},
      md::SnapshotComplete{"SPY", kTime}, md::SnapshotHeartbeat{"SPY", kTime},
      md::TradingHalt{kTime, kTime + md::kNanosPerMinute},
      md::ProviderStatus{kTime + 100, md::FeedState::Stale, "index timing unconfirmed", "SPY"}};
  for (const auto& event : events) monitor.observe(event, kTime + 1);
  const auto report = monitor.report();
  EXPECT_EQ(report["event_counts"]["OptionQuote"], 2);
  EXPECT_EQ(report["event_counts"].size(), std::variant_size_v<md::Event>);
  for (const auto& [name, count] : report["event_counts"].items()) EXPECT_GT(count.get<int>(), 0) << name;
  EXPECT_EQ(report["contracts_seen"], 1);
  EXPECT_EQ(report["presence"]["volume_contracts"], 1);
  EXPECT_EQ(report["presence"]["open_interest_contracts"], 1);
  EXPECT_EQ(report["timestamps"]["OptionQuote"]["regressions"], 0);
  EXPECT_FALSE(report["timestamps"].contains("ProviderStatus"));
  EXPECT_EQ(report["exit_code"], 0);
  EXPECT_EQ(report["ready"], true);  // SnapshotComplete works even with stale index health
}

TEST(Certification, FlagsFutureUnknownReceiptAndRegressingClocksPerInstrumentAndType) {
  probe::Conformance monitor({{"SPY"}}, true, 1);
  monitor.observe(md::ContractDefinition{0, kContract}, kTime);
  monitor.observe(md::ContractDefinition{1, kContract}, kTime);
  monitor.observe(md::OptionQuote{0, kTime}, kTime + 10);
  monitor.observe(md::OptionQuote{1, kTime - 10}, kTime + 10);
  monitor.observe(md::OpenInterest{0, kTime - 100, 42}, kTime + 10);
  monitor.observe(md::OptionQuote{0, kTime - 1}, kTime + 10);
  monitor.observe(md::OptionQuote{0, 0}, kTime + 10);
  monitor.observe(md::OptionQuote{0, kTime + 11}, kTime + 10);
  monitor.observe(md::OptionQuote{0, kTime + 12}, kTime + 12);
  const auto report = monitor.report();
  const auto check = report["timestamps"]["OptionQuote"];
  EXPECT_EQ(check["regressions"], 1);
  EXPECT_EQ(check["unknown"], 1);
  EXPECT_EQ(check["future"], 1);
  EXPECT_EQ(check["receipt_time_matches"], 1);
  EXPECT_EQ(report["timestamps"]["OpenInterest"]["regressions"], 0);
  EXPECT_EQ(report["outcome"], "fail");
  EXPECT_EQ(report["exit_code"], 1);
}

TEST(Certification, MissingEvidenceIsNotAPassAndMalformedCountsAreFailures) {
  probe::Conformance monitor({{"SPY", "QQQ"}}, true, 1);
  monitor.observe(md::ContractDefinition{0, kContract}, kTime);
  monitor.observe(md::OptionQuote{0, kTime}, kTime + 1);
  auto report = monitor.report();
  EXPECT_EQ(report["ready"], false);
  EXPECT_EQ(report["outcome"], "incomplete");
  EXPECT_EQ(report["greeks"]["iv"]["minimum"], nullptr);
  monitor.observe(md::OptionVolume{0, kTime, -1}, kTime + 1);
  monitor.observe(md::OpenInterest{0, kTime, 1.5}, kTime + 1);
  monitor.observe(md::OptionQuote{42, kTime}, kTime + 1);
  report = monitor.report();
  EXPECT_EQ(report["errors"]["invalid_activity_count"], 2);
  EXPECT_EQ(report["errors"]["undefined_instrument"], 1);
  EXPECT_EQ(report["outcome"], "fail");
}

TEST(Certification, DetectsRawDerivativeScalesWithoutClaimingToValidateVendorModels) {
  probe::Conformance monitor({{"SPY"}}, true, 1);
  const auto contract = *md::parse_osi("SPY261218C00500000");
  monitor.observe(md::ContractDefinition{0, contract}, kTime);
  monitor.observe(md::OptionQuote{0, kTime}, kTime + 1);
  monitor.observe(md::UnderlyingQuote{"SPY", kTime, 0, 0, 500}, kTime + 1);
  const auto greek = pricing::bsm_greeks({pricing::OptionType::Call, 500, 500,
      md::years_between(kTime, contract.expiry_time()), 0, 0, .2});
  monitor.observe(md::VendorGreeks{0, kTime, .2, greek.delta, greek.gamma,
                                  greek.vega / 100, greek.theta / 365, greek.rho / 100}, kTime + 1);
  for (const auto* field : {"vega", "theta", "rho"}) {
    EXPECT_EQ(monitor.report()["greeks"][field]["scale_samples"], 1);
    EXPECT_EQ(monitor.report()["greeks"][field]["scale_suspect"], 0);
  }
  monitor.observe(md::VendorGreeks{0, kTime, .2, greek.delta, greek.gamma,
                                  greek.vega, greek.theta, greek.rho}, kTime + 1);
  for (const auto* field : {"vega", "theta", "rho"})
    EXPECT_EQ(monitor.report()["greeks"][field]["scale_suspect"], 1);
  EXPECT_EQ(monitor.report()["outcome"], "review");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  monitor.observe(md::VendorGreeks{0, 0, 20, nan, nan, nan, nan, nan}, kTime);
  EXPECT_EQ(monitor.report()["greeks"]["iv"]["scale_suspect"], 1);
  EXPECT_EQ(monitor.report()["greeks"]["theta"]["missing"], 1);
  monitor.observe(md::VendorGreeks{0, 0, -1, nan, nan, std::numeric_limits<double>::infinity(), nan, nan}, kTime);
  EXPECT_EQ(monitor.report()["outcome"], "fail");
}

TEST(Certification, RedactionOmitsArbitraryDiagnosticsSymbolsAndConfiguration) {
  probe::Conformance monitor({{"account-123456"}}, true, 1);
  auto contract = kContract;
  contract.root = "key-secret";
  contract.underlying = "account-123456";
  monitor.observe(md::ContractDefinition{0, contract}, kTime);
  monitor.observe(md::UnderlyingQuote{"oauth-secret", kTime, 0, 0, 500}, kTime);
  monitor.observe(md::ProviderStatus{kTime, md::FeedState::Error,
      "Bearer token-secret api_key=key-secret account=account-123456 https://user:pass@host?token=oauth-secret",
      "account-123456"}, kTime);
  const auto report = monitor.report();
  for (const auto& text : {report.dump(), probe::Conformance::markdown(report)})
    for (const auto* secret : {"token-secret", "key-secret", "account-123456", "oauth-secret", "user:pass"})
      EXPECT_EQ(text.find(secret), std::string::npos);
  EXPECT_EQ(report["errors"]["provider"], 1);
  EXPECT_EQ(report["provider_states"]["error"], 1);
}

TEST(Certification, ThetaHttpMockFeedsACompleteRedactedReport) {
  providers::ThetaDataProvider provider;
  probe::Conformance monitor({{"SPY"}}, false, 1);
  test::HttpStub http;
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/snapshot/quote?") != std::string_view::npos)
      return {200, R"({"symbol":"SPY","expiration":"2099-12-18","strike":500,"right":"call",
        "timestamp":"2026-09-22T15:33:42.125","bid":5,"ask":6,"volume":321})"};
    if (url.find("/snapshot/open_interest?") != std::string_view::npos)
      return {200, R"({"symbol":"SPY","expiration":"2099-12-18","strike":500,"right":"call",
        "timestamp":"2026-09-22T06:30:00.000","open_interest":42})"};
    return {200, {}};
  };
  provider.poll_once(http, "SPY", {{"SPY"}}, monitor);
  const auto report = monitor.report();
  EXPECT_EQ(report["contracts_seen"], 1);
  EXPECT_EQ(report["event_counts"]["SnapshotComplete"], 1);
  EXPECT_EQ(report["presence"]["volume_contracts"], 1);
  EXPECT_EQ(report["presence"]["open_interest_contracts"], 1);
  EXPECT_EQ(report["outcome"], "review");  // absent Greeks cannot be certified
  EXPECT_EQ(report["exit_code"], 0);
}

TEST(Certification, DurationDoesNotStopAtReadinessAndSnapshotDoes) {
  probe::Conformance monitor({{"SPY"}}, false, 1);
  monitor.observe(md::SnapshotComplete{"SPY", kTime}, kTime + 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  monitor.wait(deadline, true);
  EXPECT_LT(std::chrono::steady_clock::now(), deadline);
  monitor.wait(deadline, false);
  EXPECT_GE(std::chrono::steady_clock::now(), deadline);
}

TEST(Certification, ReplayWritesBothReportsAndStartupFailureStillWritesEvidence) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.subscription.underlyings = {"SPY"};
  test::record_events(file.path, {md::ContractDefinition{0, kContract}, md::OptionQuote{0, kTime},
                                md::SnapshotComplete{"SPY", kTime}}, header);
  const md::ProviderConfig config{"replay", "key-secret", {{"file", file.path.string()}, {"speed", "max"}}};
  const auto directory = file.directory / "report";
  EXPECT_EQ(probe::certify(config, {{"SPY"}}, 1, false, false, 1, directory), 0);
  std::ifstream json_file(directory / "conformance.json");
  const auto report = Json::parse(json_file);
  EXPECT_EQ(report["source"], "replayed; original provenance unverified");
  EXPECT_EQ(report["provider"], "replay");
  EXPECT_EQ(report.dump().find("key-secret"), std::string::npos);
  std::ifstream markdown_file(directory / "conformance.md");
  const std::string markdown{std::istreambuf_iterator<char>(markdown_file), {}};
  EXPECT_EQ(markdown, probe::Conformance::markdown(report));
  EXPECT_THROW(probe::certify(config, {{"SPY"}}, 1, false, false, 1, directory), std::invalid_argument);
  const auto failed_directory = file.directory / "failed";
  EXPECT_EQ(probe::certify({"unknown-token-secret", "", {}}, {{"SPY"}}, 1, false, false, 1, failed_directory), 1);
  std::ifstream failed_file(failed_directory / "conformance.json");
  const auto failed = Json::parse(failed_file);
  EXPECT_EQ(failed["errors"]["startup"], 1);
  EXPECT_EQ(failed.dump().find("token-secret"), std::string::npos);
}
}  // namespace
