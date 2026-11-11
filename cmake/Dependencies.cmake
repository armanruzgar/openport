# Third-party dependencies.
#
# Small header-mostly libraries are fetched at configure time, pinned by version
# and checksum so every build sees the same code. Boost and OpenSSL are large,
# so they come from the system package manager (Homebrew or apt).

# Homebrew installs OpenSSL keg-only; point CMake at it on macOS. This has to happen
# before anything (including databento-cpp) looks for OpenSSL.
if(APPLE AND NOT OPENSSL_ROOT_DIR)
  foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
    if(EXISTS "${prefix}/include/openssl/ssl.h")
      set(OPENSSL_ROOT_DIR "${prefix}")
      break()
    endif()
  endforeach()
endif()

# macOS has no OpenSSL or zstd of its own, so the macOS release archive links them
# statically and leaves out Brotli (optional to Databento's HTTP client) to run on a
# Mac without Homebrew. Libraries a shared build left in the cache are found again.
if(OPENPORT_STATIC_DEPS)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
  foreach(library OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY OPENPORT_ZSTD_LIBRARY)
    if(${library} AND NOT ${library} MATCHES "\\.a$")
      unset(${library} CACHE)
    endif()
  endforeach()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)
# Some dependencies install their own headers and libraries; keep those out of
# the openport component (`cmake --install build --component openport`).
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME thirdparty)

if(OPENPORT_BUILD_PROVIDERS AND OPENPORT_WITH_DATABENTO)
  # Databento's official client. It downloads a prebuilt libdbn_c for the platform and
  # brings nlohmann_json with it, so it has to come before our own copy below.
  find_package(zstd CONFIG QUIET)
  if(OPENPORT_STATIC_DEPS AND TARGET zstd::libzstd_static AND TARGET zstd::libzstd)
    set_target_properties(zstd::libzstd PROPERTIES INTERFACE_LINK_LIBRARIES zstd::libzstd_static)
  endif()
  # databento-cpp fetches JSON for Modern C++, cpp-httplib and date by URL alone, and
  # every OpenPort target compiles against that JSON copy. The first declaration of a
  # name wins, so these, the same releases pinned by checksum, are what it builds with.
  # Keep them at the versions its CMakeLists.txt names when it is updated.
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.54.1.tar.gz
    URL_HASH SHA256=7310f5312e1423830d649b38ed028e9db86303a979ccbfdbd1c4b1574f422dfb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(date_src
    URL https://github.com/HowardHinnant/date/archive/refs/tags/v3.0.5.tar.gz
    URL_HASH SHA256=ef786edc203daec76475825640b3af247bd08e31fc52217e5ce8f76107b4bb05
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_Declare(databento
    URL https://github.com/databento/databento-cpp/archive/refs/tags/v0.68.0.tar.gz
    URL_HASH SHA256=de8ff21cffce4003e55b7067146f7301ba6cc9677925cbf86449b2b87018ac51
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  set(DATABENTO_ENABLE_UNIT_TESTING OFF CACHE INTERNAL "")
  set(DATABENTO_ENABLE_EXAMPLES OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(databento)
endif()

FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)

FetchContent_Declare(simdjson
  URL https://github.com/simdjson/simdjson/archive/refs/tags/v4.6.11.tar.gz
  URL_HASH SHA256=61d948fc24f0d793829ad658058e7597d064988a89b4607ea02e401a82df98ff
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(SIMDJSON_DEVELOPER_MODE OFF CACHE INTERNAL "")
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_MakeAvailable(nlohmann_json)
endif()
FetchContent_MakeAvailable(simdjson)

if(OPENPORT_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE INTERNAL "")
  set(gtest_force_shared_crt ON CACHE INTERNAL "")
  FetchContent_MakeAvailable(googletest)
endif()

if(OPENPORT_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
    URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE INTERNAL "")
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE INTERNAL "")
  set(BENCHMARK_ENABLE_WERROR OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(benchmark)
endif()

if(OPENPORT_BUILD_PROVIDERS)
  find_package(OpenSSL 3 REQUIRED)
  find_package(Boost 1.83 CONFIG REQUIRED)
  find_package(ZLIB REQUIRED)
  find_package(Threads REQUIRED)
endif()

# Package managers install their own copies of several of these libraries beside
# Boost and OpenSSL (Homebrew puts simdjson, nlohmann-json and googletest headers in
# the same include directory as Boost), often at another version. Compiling against
# those headers while linking the pinned library breaks its ABI: with Homebrew's
# simdjson 5 a parser's destructor jumped through a vtable from simdjson 4. A
# target's own include directories come before those of the libraries it links,
# so naming the pinned headers here puts them ahead of every system prefix.
foreach(pinned IN ITEMS nlohmann_json::nlohmann_json simdjson::simdjson databento::databento
                        GTest::gtest GTest::gmock benchmark::benchmark)
  if(NOT TARGET ${pinned})
    continue()
  endif()
  get_target_property(real ${pinned} ALIASED_TARGET)
  if(NOT real)
    set(real ${pinned})
  endif()
  include_directories(SYSTEM "$<TARGET_PROPERTY:${real},INTERFACE_INCLUDE_DIRECTORIES>")
endforeach()
