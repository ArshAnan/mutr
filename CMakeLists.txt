cmake_minimum_required(VERSION 3.20)

project(mutr VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake's default Release flags are "-O3 -DNDEBUG". Do not append another -O
# flag. Clang keeps the last -O it sees, so a trailing -O2 silently wins.
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

add_compile_options(-Wall -Wextra)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

# Checked on macOS 27.0 (Darwin 27.0.0) with Apple Clang 17.0.0:
# ASan hangs before main (AsanInitFromRtl -> InitializeShadowMemory -> malloc
# re-enters AsanInitFromRtl and spins on StaticSpinMutex). A one-line main
# does the same, so this is the toolchain, not mutr.
# TSan exits with signal 11 before main and prints nothing, even with
# TSAN_OPTIONS=verbosity=2. UBSan runs the test suite.

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot both be ON; the sanitizers cannot be combined.")
endif()
if(APPLE AND ENABLE_ASAN)
    message(WARNING "AddressSanitizer hangs before main on this macOS/Apple Clang (AsanInitFromRtl deadlock). The binary will not run.")
endif()
if(APPLE AND ENABLE_TSAN)
    message(WARNING "ThreadSanitizer crashes at startup on this macOS/Apple Clang (signal 11, no report). The binary will not run.")
endif()

set(MUTR_SAN_FLAGS "")
if(ENABLE_ASAN)
    list(APPEND MUTR_SAN_FLAGS -fsanitize=address -fno-omit-frame-pointer -g)
endif()
if(ENABLE_TSAN)
    list(APPEND MUTR_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
endif()
if(ENABLE_UBSAN)
    list(APPEND MUTR_SAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
endif()

add_library(mutr_core STATIC
    src/resp.cpp
    src/store.cpp
    src/commands.cpp
    src/net.cpp
    src/session.cpp
    src/server.cpp)
if(APPLE)
    target_sources(mutr_core PRIVATE src/event_loop_kqueue.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(mutr_core PRIVATE src/event_loop_epoll.cpp)
else()
    message(FATAL_ERROR "mutr's event loop supports macOS (kqueue) and Linux (epoll), not ${CMAKE_SYSTEM_NAME}")
endif()
target_include_directories(mutr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MUTR_SAN_FLAGS)
    target_compile_options(mutr_core PUBLIC ${MUTR_SAN_FLAGS})
    target_link_options(mutr_core PUBLIC ${MUTR_SAN_FLAGS})
endif()

add_executable(mutr src/main.cpp)
target_link_libraries(mutr PRIVATE mutr_core)

enable_testing()
add_executable(mutr_tests tests/tests.cpp)
target_link_libraries(mutr_tests PRIVATE mutr_core)
add_test(NAME mutr_tests COMMAND mutr_tests $<TARGET_FILE:mutr>)
set_tests_properties(mutr_tests PROPERTIES TIMEOUT 180)
