add_executable(openport_tests
  pricing/black_test.cpp
  pricing/implied_vol_test.cpp
  pricing/binomial_test.cpp
  md/time_test.cpp
  md/contract_test.cpp
  md/event_queue_test.cpp
  analytics/forward_test.cpp
  analytics/chain_analytics_test.cpp
  analytics/svi_test.cpp
  analytics/ssvi_test.cpp
  analytics/volatility_test.cpp
  analytics/realized_test.cpp
  analytics/volatility_history_test.cpp
  analytics/american_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::options GTest::gtest_main)
# Unit tests also reach private helpers under src/.
target_include_directories(openport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(openport_tests PRIVATE
  OPENPORT_VERSION="${PROJECT_VERSION}"
  OPENPORT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
if(OPENPORT_SANITIZE)
  target_compile_definitions(openport_tests PRIVATE OPENPORT_SANITIZER_BUILD)
endif()

if(OPENPORT_BUILD_PROVIDERS)
  target_sources(openport_tests PRIVATE
    md/recording_test.cpp
    providers/replay_test.cpp
    providers/demo_test.cpp
    providers/demo_feed_test.cpp
    providers/scenario_test.cpp
    server/recording_test.cpp
    net/http_test.cpp
    providers/cboe_test.cpp
    providers/runtime_test.cpp
    providers/certification_test.cpp
    ${PROJECT_SOURCE_DIR}/apps/probe/certification.cpp
    providers/cli_test.cpp
    providers/massive_test.cpp
    providers/thetadata_test.cpp
    providers/history_test.cpp
    providers/tradier_test.cpp
    providers/tastytrade_test.cpp
    providers/dxlink_test.cpp
    providers/schwab_test.cpp
    providers/alpaca_test.cpp
    providers/failover_test.cpp
    server/api_test.cpp
    server/candles_test.cpp
    server/flow_test.cpp
    server/analysis_test.cpp
    server/earnings_test.cpp
    server/series_test.cpp
    server/surface_history_test.cpp
    server/equity_test.cpp
    server/web_policy_test.cpp
    server/sandboxes_test.cpp
    server/engine_test.cpp
    server/notifications_test.cpp
    server/notification_delivery_test.cpp
    server/playbooks_test.cpp
    server/desk_test.cpp
    server/multiday_test.cpp
    server/stress_test.cpp
    server/backtest_test.cpp
    server/paper_test.cpp
    server/review_test.cpp
    server/practice_test.cpp
  )
  target_compile_definitions(openport_tests PRIVATE OPENPORT_APPS_DIR="${PROJECT_BINARY_DIR}/apps")
  add_dependencies(openport_tests openportd openport-probe)
  target_link_libraries(openport_tests PRIVATE openport::providers openport::server
                        nlohmann_json::nlohmann_json)
  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_tests PRIVATE providers/databento_test.cpp providers/databento_history_test.cpp)
  endif()
  if(OPENPORT_WITH_IBKR)
    target_sources(openport_tests PRIVATE providers/ibkr_test.cpp)
  endif()
else()
  target_link_libraries(openport_tests PRIVATE openport::core)
endif()

# Paper trading core and the reusable scripted market fixture.
target_sources(openport_tests PRIVATE
  trading/money_ledger_test.cpp
  trading/matching_test.cpp
  trading/fills_test.cpp
  trading/walk_test.cpp
  trading/costs_test.cpp
  trading/kill_test.cpp
  trading/floor_test.cpp
  trading/risk_views_test.cpp
  trading/stress_test.cpp
  trading/exposure_limits_test.cpp
  trading/profile_test.cpp
  trading/warnings_test.cpp
  trading/risk_settlement_test.cpp
  trading/journal_test.cpp
  trading/journal_schema_test.cpp
  trading/sessions_test.cpp
  trading/notes_test.cpp
  trading/review_test.cpp
  trading/trade_groups_test.cpp
  trading/scaling_test.cpp
  trading/shared_test.cpp
  trading/attribution_test.cpp
  trading/delivery_test.cpp
  trading/cash_flows_test.cpp
  trading/dividends_test.cpp
  trading/evaluation_test.cpp
  trading/conditional_test.cpp
  trading/condition_test.cpp
  trading/alert_test.cpp
  trading/funded_test.cpp
  trading/plan_rules_test.cpp
  trading/trade_rules_test.cpp
  trading/time_rules_test.cpp
  trading/event_rules_test.cpp
  trading/margin_allocation_test.cpp
  trading/multileg_test.cpp
  trading/orders_test.cpp
)
target_link_libraries(openport_tests PRIVATE openport::trading nlohmann_json::nlohmann_json)

include(GoogleTest)
gtest_discover_tests(openport_tests DISCOVERY_TIMEOUT 60)
if(OPENPORT_SANITIZE)
  # Run after discovery so COST can refer to the individual GoogleTest names.
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/CTestCosts.cmake")
endif()
