cmake_minimum_required(VERSION 3.25)

project(openport
  VERSION 0.4.0
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
option(OPENPORT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OPENPORT_NATIVE "Tune code generation for the build machine's CPU" OFF)
option(OPENPORT_WERROR "Treat compiler warnings as errors" OFF)
option(OPENPORT_STATIC_DEPS "Link OpenSSL and zstd statically, so a release archive runs without them installed" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

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
