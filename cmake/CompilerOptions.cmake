# Warning, sanitizer and optimisation flags shared by every OpenPort target.

add_library(openport_options INTERFACE)
add_library(openport::options ALIAS openport_options)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(openport_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Woverloaded-virtual -Wdouble-promotion
    # GCC reports false positives inside Boost.Asio templates with this one.
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wnull-dereference>
    # GCC's flow analysis flags members of inactive std::variant alternatives when a
    # variant is moved (GCC bug 80635 and relatives); Clang still checks
    # uninitialised use on every build.
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
    -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation)
  if(OPENPORT_WERROR)
    target_compile_options(openport_options INTERFACE -Werror)
  endif()

  if(OPENPORT_SANITIZE)
    target_compile_options(openport_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
      # GCC's flow-based bounds warnings misread ASan's instrumentation (for
      # example a one-element initializer list copied into a vector); the
      # unsanitized builds keep them as errors.
      $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds -Wno-stringop-overflow -Wno-stringop-overread>)
    target_link_options(openport_options INTERFACE -fsanitize=address,undefined)
  endif()

  if(OPENPORT_SANITIZE_THREAD)
    if(OPENPORT_SANITIZE)
      message(FATAL_ERROR "OPENPORT_SANITIZE_THREAD cannot be combined with OPENPORT_SANITIZE: "
                          "ThreadSanitizer does not run with AddressSanitizer")
    endif()
    target_compile_options(openport_options INTERFACE
      -fsanitize=thread -fno-omit-frame-pointer
      # ThreadSanitizer does not model std::atomic_thread_fence, which GCC reports for
      # the trading containers' ownership check (include/openport/trading/shared.hpp);
      # tests/tsan.supp covers the races it then reports there.
      $<$<CXX_COMPILER_ID:GNU>:-Wno-tsan>)
    target_link_options(openport_options INTERFACE -fsanitize=thread)
  endif()

  if(OPENPORT_NATIVE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      target_compile_options(openport_options INTERFACE -mcpu=native)
    else()
      target_compile_options(openport_options INTERFACE -march=native)
    endif()
  endif()
elseif(MSVC)
  target_compile_options(openport_options INTERFACE /W4 /permissive-)
endif()
