# This is always the first line in the CMakeLists.txt file, and it tells CMake the minimum version of CMake that is required to build the project.
cmake_minimum_required(VERSION 3.20)

# project() declares the project name, version, and language. Here, CXX means C++.
project(multiThreadRedis VERSION 1.0.0 LANGUAGES CXX)

# add_executable() creates an executable target named "multiThreadRedis" that builds the "src/main.cpp" file.
add_executable(multiThreadRedis src/main.cpp src/resp.cpp)

# set_target_properties() sets the properties of the target.
set_target_properties(multiThreadRedis PROPERTIES
                        CXX_STANDARD 17 # Sets the C++ standard to 17.
                        CXX_STANDARD_REQUIRED ON # If the compiler does not support C++17, fail loudly instead of silently downgrading.
                        CXX_EXTENSIONS OFF) # Disables GNU-specific extensions (like gnu++17). Without this, GCC defaults to gnu++17 instead of pure c++17, which can hide portability bugs that would break on MSVC or Clang.

target_compile_options(multiThreadRedis PRIVATE
                                            -Wall # Adds the -Wall flag to the compiler. This is a good practice to catch common mistakes and improve code quality.
                                            -Wextra # Adds the -Wextra flag to the compiler. This is a good practice to catch common mistakes and improve code quality.
                                            $<$<CONFIG:Debug>:-g>
                                            $<$<CONFIG:Debug>:-O0>
                                            $<$<CONFIG:Release>:-O2>) # Adds the -O2 flags to the compiler when the build type is Release.

# AddressSanitizer is opt-in. On macOS 26 (Darwin 25) with Apple Clang 17, ASan
# deadlocks during process init: InitializeShadowMemory() -> get_dyld_hdr() ->
# dyld_shared_cache_iterate_text_swift() mallocs, which re-enters AsanInitFromRtl()
# and spins forever on StaticSpinMutex. The process burns ~99% CPU, never reaches
# main(), and produces no output. Enable later with: cmake -DENABLE_ASAN=ON ..
option(ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
if(ENABLE_ASAN)
    if(APPLE)
        message(WARNING "AddressSanitizer currently deadlocks at startup on this macOS/Xcode combination. The binary will hang before main().")
    endif()
    target_compile_options(multiThreadRedis PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
    target_link_options(multiThreadRedis PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)
endif()