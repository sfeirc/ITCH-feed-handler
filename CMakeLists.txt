cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build-type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler detection (warn if not Clang)
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(WARNING "This project is tuned for Clang/GCC. Found: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---------------------------------------------------------------------------
# Common compile options
# ---------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wconversion
    -Wshadow
    -Wno-unused-parameter   # many ITCH handlers take (void) params for future use
    -fno-rtti
)

# Release: full optimisation, no sanitizers
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(
        -O3
        -march=native
        -DNDEBUG
        -fno-omit-frame-pointer   # keep for perf(1) profiling
        -funroll-loops
    )
    add_link_options(-O3)
endif()

# Debug: sanitizers + debug info
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(
        -O1
        -g3
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# RelWithDebInfo: optimised + symbols
if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-O3 -march=native -g -DNDEBUG)
endif()

# ---------------------------------------------------------------------------
# Optional: AF_XDP support via libbpf
# ---------------------------------------------------------------------------
option(ENABLE_XDP "Enable AF_XDP support (requires libbpf)" OFF)

if(ENABLE_XDP)
    find_library(LIBBPF_LIB bpf REQUIRED)
    find_path(LIBBPF_INCLUDE bpf/xsk.h REQUIRED)
    message(STATUS "AF_XDP enabled: ${LIBBPF_LIB}")
    add_compile_definitions(HAVE_LIBBPF)
    set(XDP_LINK_LIBS ${LIBBPF_LIB})
    include_directories(${LIBBPF_INCLUDE})
else()
    message(STATUS "AF_XDP disabled (use -DENABLE_XDP=ON to enable)")
    set(XDP_LINK_LIBS "")
endif()

# ---------------------------------------------------------------------------
# Google Benchmark
# ---------------------------------------------------------------------------
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
    # Try pkg-config fallback
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GBENCH benchmark)
    endif()
    if(NOT GBENCH_FOUND)
        message(WARNING "Google Benchmark not found — bm_itch target will be skipped")
        set(BENCHMARK_AVAILABLE FALSE)
    else()
        set(BENCHMARK_AVAILABLE TRUE)
    endif()
else()
    set(BENCHMARK_AVAILABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
include_directories(include)

# ---------------------------------------------------------------------------
# Core library: itch_lib
# ---------------------------------------------------------------------------
add_library(itch_lib STATIC
    src/itch/parser.cpp
    src/itch/order_book.cpp
    src/net/pcap_replay.cpp
    src/net/xdp_socket.cpp
)

target_include_directories(itch_lib PUBLIC include)

target_compile_options(itch_lib PRIVATE
    $<$<CONFIG:Release>:-march=native -O3>
)

if(XDP_LINK_LIBS)
    target_link_libraries(itch_lib PUBLIC ${XDP_LINK_LIBS})
endif()

# ---------------------------------------------------------------------------
# Benchmark binary: bm_itch
# ---------------------------------------------------------------------------
if(BENCHMARK_AVAILABLE)
    add_executable(bm_itch bench/bm_itch.cpp)
    target_include_directories(bm_itch PRIVATE include)

    if(benchmark_FOUND)
        target_link_libraries(bm_itch PRIVATE itch_lib benchmark::benchmark)
    else()
        target_include_directories(bm_itch PRIVATE ${GBENCH_INCLUDE_DIRS})
        target_link_libraries(bm_itch PRIVATE itch_lib ${GBENCH_LIBRARIES})
    endif()

    target_compile_options(bm_itch PRIVATE
        $<$<CONFIG:Release>:-march=native -O3>
    )
endif()

# ---------------------------------------------------------------------------
# Feed handler binary
# ---------------------------------------------------------------------------
add_executable(feed_handler tools/feed_handler_main.cpp)
target_include_directories(feed_handler PRIVATE include)
target_link_libraries(feed_handler PRIVATE itch_lib pthread)

if(XDP_LINK_LIBS)
    target_link_libraries(feed_handler PRIVATE ${XDP_LINK_LIBS})
endif()

# ---------------------------------------------------------------------------
# Install targets
# ---------------------------------------------------------------------------
install(TARGETS feed_handler DESTINATION bin)
install(TARGETS itch_lib DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== itch_feed_handler build summary ===")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  AF_XDP     : ${ENABLE_XDP}")
message(STATUS "  Benchmarks : ${BENCHMARK_AVAILABLE}")
message(STATUS "")
