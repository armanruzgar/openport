cmake_minimum_required(VERSION 3.25)

# The oldest macOS the release archive runs on. A Mac build that links OpenSSL and zstd
# statically (OPENPORT_STATIC_DEPS), as the archive does, is compiled for it, which
# has to be set before project(). The libraries must be built for it or an older
# macOS too: tools/release.sh and the release workflow check them and the programs.
set(OPENPORT_MACOS_FLOOR 15.0)
if(CMAKE_HOST_APPLE AND OPENPORT_STATIC_DEPS AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET ${OPENPORT_MACOS_FLOOR} CACHE STRING "Oldest macOS the programs run on")
endif()

project(openport
  VERSION 0.6.0
  DESCRIPTION "Options pricing, volatility surfaces, matching and real-time risk"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OPENPORT_BUILD_TESTS "Build the unit tests" ON)
option(OPENPORT_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(OPENPORT_BUILD_PROVIDERS "Build the provider adapters and apps (needs Boost, OpenSSL, zlib)" ON)
option(OPENPORT_WITH_DATABENTO "Build the Databento adapter (fetches databento-cpp)" ON)
option(OPENPORT_WITH_IBKR "Build the Interactive Brokers Client Portal adapter" ON)
option(OPENPORT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OPENPORT_SANITIZE_THREAD "Build with ThreadSanitizer (not together with OPENPORT_SANITIZE)" OFF)
option(OPENPORT_NATIVE "Tune code generation for the build machine's CPU" OFF)
option(OPENPORT_WERROR "Treat compiler warnings as errors" OFF)
option(OPENPORT_STATIC_DEPS "Link OpenSSL and zstd statically, so a release archive runs without them installed" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)
include(BuildInfo)

add_subdirectory(src)

if(OPENPORT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OPENPORT_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(OPENPORT_BUILD_PROVIDERS)
  add_subdirectory(apps)
endif()
