cmake_minimum_required(VERSION 3.22)
project(TradingEngine VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# C++20 Standard  (required for std::jthread, std::format, std::atomic, etc.)
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Build-type defaults
# ─────────────────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Compiler flags
#   Release : maximum optimisation + aggressive inlining
#   Debug   : address/UB sanitisers + full symbols
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -march=native -flto)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g3 -fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2 /GL /fp:fast)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(/Od /Zi)
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Threading support  (pthreads on Linux/macOS)
# ─────────────────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Main executable
# ─────────────────────────────────────────────────────────────────────────────
add_executable(trading_engine
    src/main.cpp
)

target_include_directories(trading_engine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(trading_engine
    PRIVATE Threads::Threads
)

# ─────────────────────────────────────────────────────────────────────────────
# Installation rules (optional)
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS trading_engine DESTINATION bin)

# ─────────────────────────────────────────────────────────────────────────────
# Print summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "============================================================")
message(STATUS " TradingEngine v${PROJECT_VERSION}")
message(STATUS " Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS " Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS " C++ Standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "============================================================")
