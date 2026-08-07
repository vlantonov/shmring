cmake_minimum_required(VERSION 3.20)
project(shmring VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Conan 2: Catch2 v3 and Google Benchmark ──────────────────────────────────
find_package(Catch2 REQUIRED)
find_package(benchmark REQUIRED)

# ── Optional libpcap ──────────────────────────────────────────────────────────
find_package(PCAP QUIET)
if(PCAP_FOUND)
    message(STATUS "libpcap found — demo built with pcap support")
else()
    message(STATUS "libpcap not found — demo built with synthetic packets")
endif()

# ── Header-only interface library ────────────────────────────────────────────
add_library(shmring_headers INTERFACE)
target_include_directories(shmring_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(shmring_headers INTERFACE cxx_std_20)

# ── Subdirectories ────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(src/demo)
add_subdirectory(tests)
add_subdirectory(benchmarks)
