# Include CMake Version Required
# This is always the first line in the CMakeLists.txt file, and it tells CMake the minimum version of CMake that is required to build the project.
cmake_minimum_required(VERSION 3.20)

# Project Name, Version, and Language
# Declares the project. Here, CXX means C++ and VERSION 1.0.0 is the version of the project.
project(multiThreadRedis VERSION 1.0.0 LANGUAGES CXX)

# Set the C++ Standard
set(CMAKE_CXX_STANDARD 17) # Sets the C++ standard to 17.
set(CMAKE_CXX_STANDARD_REQUIRED ON) # If the compiler does not support C++17, fail loudly instead of silently downgrading.
set(CMAKE_CXX_EXTENSIONS OFF) # Disables GNU-specific extensions (like gnu++17). Without this, GCC defaults to gnu++17 instead of pure c++17, which can hide portability bugs that would break on MSVC or Clang.

# Default Build Type
# CMAKE doesn't set a default build type if one is not specified. Leaving it unset means that we will get no optimizations and debug flags, which is not what we want.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build Type" FORCE) # This block makes sure we get a default build type of Debug if one is not specified.
endif()
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}") # This line prints the build type to the console to avoid confusion.
# The the CACHE STRING ... FORCE is used to set the build type and force it to be set to Debug if one is not specified.

# Options
# option() declares a boolean that the user can set at configuration time.
        # Eg. "cmake -B build -DBUILD_TESTS=OFF"
option(BUILD_TESTS "Build unit tests" ON) # This is where we control of the "tests/" directory is included in the build. Useful for CI environments where you only want to build the library/executable without the test runner.
option(BUILD_SHARED_LIBS "Build shared libraries" ON) # This is a special CMAKE variable that controls if the library is built as a shared library.
                                                    # When ON, "add.library()" calls without an explicit "STATIC" or "SHARED" argument will default to SHARED, and will produce a shared library (.so or .dll depending on the platform) instead of a static library (.a or .lib depending on the platform).

