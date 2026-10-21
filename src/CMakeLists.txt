# Pricing, analytics and the normalised market-data model: no network dependencies.
add_library(openport_core
  pricing/black.cpp
  pricing/implied_vol.cpp
  pricing/binomial.cpp
  md/time.cpp
  md/contract.cpp
  analytics/chain_book.cpp
  analytics/forward.cpp
  analytics/chain_analytics.cpp
  analytics/svi.cpp
  analytics/ssvi.cpp
  analytics/volatility.cpp
  analytics/realized.cpp
  analytics/volatility_history.cpp
  analytics/comparison.cpp
)
add_library(openport::core ALIAS openport_core)
target_include_directories(openport_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_core PRIVATE openport::options)
set_target_properties(openport_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Pure paper-trading reducer; only the journal performs file I/O.
if(NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
endif()
add_library(openport_trading
  trading/evaluation.cpp
  trading/events.cpp
  trading/money.cpp
  trading/types.cpp
  trading/guardrails.cpp
  trading/dividends.cpp
  trading/ledger.cpp
  trading/risk.cpp
  trading/journal.cpp
  trading/session.cpp
  trading/state_delta.cpp
  trading/history.cpp
  trading/review.cpp
  trading/margin.cpp
)
add_library(openport::trading ALIAS openport_trading)
target_include_directories(openport_trading PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_link_libraries(openport_trading PUBLIC openport::core
  PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json openport::options)
set_target_properties(openport_trading PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(OPENPORT_BUILD_PROVIDERS)
  # Recording is independent of Databento, including builds without its adapter.
  find_path(OPENPORT_ZSTD_INCLUDE_DIR zstd.h REQUIRED)
  if(OPENPORT_STATIC_DEPS)
    find_library(OPENPORT_ZSTD_LIBRARY NAMES libzstd.a REQUIRED)
  else()
    find_library(OPENPORT_ZSTD_LIBRARY NAMES zstd REQUIRED)
  endif()
  add_library(openport_recording md/recording.cpp)
  add_library(openport::recording ALIAS openport_recording)
  target_include_directories(openport_recording PRIVATE ${OPENPORT_ZSTD_INCLUDE_DIR})
  target_link_libraries(openport_recording PUBLIC openport::core PRIVATE ${OPENPORT_ZSTD_LIBRARY} Threads::Threads openport::options)

  # HTTPS and WebSocket plumbing shared by the provider adapters.
  add_library(openport_net
    net/http.cpp
    net/websocket.cpp
  )
  add_library(openport::net ALIAS openport_net)
  target_compile_definitions(openport_net PRIVATE OPENPORT_VERSION="${PROJECT_VERSION}")
  target_include_directories(openport_net PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
  target_link_libraries(openport_net
    PUBLIC Boost::headers OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads
    PRIVATE openport::options)

  # Embed the library; both edits and added/removed files trigger reconfiguration.
  file(GLOB OPENPORT_SCENARIO_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/scenarios/*.json")
  if(NOT OPENPORT_SCENARIO_FILES)
    message(FATAL_ERROR "No built-in scenarios in scenarios/")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OPENPORT_SCENARIO_FILES})
  set(OPENPORT_EMBEDDED_SCENARIOS "")
  foreach(scenario_file IN LISTS OPENPORT_SCENARIO_FILES)
    file(READ "${scenario_file}" scenario_text)
    get_filename_component(scenario_name "${scenario_file}" NAME)
    string(REPLACE "\\" "\\\\" scenario_name "${scenario_name}")
    string(REPLACE "\"" "\\\"" scenario_name "${scenario_name}")
    string(SHA256 scenario_hash "${scenario_text}")
    string(SUBSTRING "${scenario_hash}" 0 12 scenario_delimiter)
    string(FIND "${scenario_text}" ")${scenario_delimiter}\"" scenario_collision)
    if(NOT scenario_collision EQUAL -1)
      message(FATAL_ERROR "Raw string delimiter collision in ${scenario_file}")
    endif()
    string(APPEND OPENPORT_EMBEDDED_SCENARIOS
      "    append(\"scenarios/${scenario_name}\", R\"${scenario_delimiter}(${scenario_text})${scenario_delimiter}\");\n")
  endforeach()
  configure_file(providers/builtin_scenarios.cpp.in builtin_scenarios.cpp @ONLY)

  # One adapter per market-data provider.
  add_library(openport_providers
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_scenarios.cpp"
    providers/factory.cpp
    providers/demo.cpp
    providers/demo_feed.cpp
    providers/scenario.cpp
    providers/replay.cpp
    providers/replay_batches.cpp
    providers/snapshot.cpp
    providers/cboe.cpp
    providers/massive.cpp
    providers/thetadata.cpp
    providers/history.cpp
    providers/theta_history.cpp
    providers/tradier.cpp
    providers/tastytrade.cpp
    providers/dxlink.cpp
    providers/schwab.cpp
    providers/alpaca.cpp
    providers/failover.cpp
  )
  add_library(openport::providers ALIAS openport_providers)
  if(OPENPORT_BUILD_TESTS AND OPENPORT_SANITIZE)
    # Only scoped test fixtures enable sampling; apps keep the full generator.
    target_compile_definitions(openport_providers PRIVATE OPENPORT_SANITIZER_TEST_SUPPORT)
  endif()
  target_link_libraries(openport_providers PUBLIC openport::recording)
  target_link_libraries(openport_providers
    PUBLIC openport::core openport::net nlohmann_json::nlohmann_json
    PRIVATE openport::trading simdjson::simdjson openport::options)

  if(OPENPORT_WITH_DATABENTO)
    target_sources(openport_providers PRIVATE providers/databento.cpp providers/databento_history.cpp)
    target_link_libraries(openport_providers PUBLIC databento::databento)
    target_compile_definitions(openport_providers PUBLIC OPENPORT_WITH_DATABENTO)
  endif()
  if(OPENPORT_WITH_IBKR)
    # No SDK: the Client Portal Web API is plain HTTPS to the operator's local gateway.
    target_sources(openport_providers PRIVATE providers/ibkr.cpp)
    target_compile_definitions(openport_providers PUBLIC OPENPORT_WITH_IBKR)
  endif()
endif()

if(OPENPORT_BUILD_PROVIDERS)
  # The engine thread, JSON API and HTTP/WebSocket server behind openportd.
  add_library(openport_server
    server/candles.cpp
    server/metric_cache.cpp
    server/series.cpp
    server/series_api.cpp
    server/equity.cpp
    server/event_calendar.cpp
    server/strategy_template.cpp
    server/playbooks.cpp
    server/playbook_api.cpp
    server/desk_playbooks.cpp
    server/desk.cpp
    server/run.cpp
    server/backtest.cpp
    server/backtest_host.cpp
    server/engine.cpp
    server/notifications.cpp
    server/paper_engine.cpp
    server/paper_api.cpp
    server/plans.cpp
    server/api.cpp
    server/replay_host.cpp
    server/sandboxes.cpp
    server/web_server.cpp
  )
  add_library(openport::server ALIAS openport_server)
  target_compile_definitions(openport_server PRIVATE OPENPORT_VERSION="${PROJECT_VERSION}")
  # Replays run the replay provider beside the live feed.
  target_link_libraries(openport_server PUBLIC openport::recording openport::trading openport::providers)
  target_link_libraries(openport_server
    PUBLIC openport::core openport::net
    PRIVATE nlohmann_json::nlohmann_json openport::options)
endif()
